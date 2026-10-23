#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把壁纸发给键盘 —— **解码在电脑端做**，板子只收 + 展示。

和网页端那条 JPEG 路的区别：
  网页端：PNG → 浏览器压成 JPEG（14KB）→ BLE → **板子上用 JPEGDEC 解**
  本脚本：PNG → **这里就解成 RGB565**（115200 字节）→ BLE → 板子直接用

为什么这样更好：
  ① 板子上彻底不需要 JPEG 解码器。JPEGDEC 那个对象 12.6KB 静态占着内部
     DRAM，而且解析时要在输入缓冲里大量跳转 —— 输入缓冲在 PSRAM 上就不稳，
     之前"上传壁纸必崩"就是踩在这上面。板子只搬字节，这条面就没了。
  ② RAW 的长度是**固定**的 240*240*2 = 115200，不是主机随口说的数，
     所以板子上不存在"声明多少就收多少"的越界可能。
  ③ 跟传 M1 宏数据是同一套路：这边发什么，那边就显示什么。

⚠ 字节序（两边必须一致的硬约定）：
  固件 LV_COLOR_DEPTH=16 且**没有**开 LV_COLOR_16_SWAP
  ⇒ RGB565 就是 ESP32 上的 native 小端。
  固件里 JPEGDEC 用的 RGB565_LITTLE_ENDIAN 落在内存里也是这个样子，
  两条路写进 wpPixels 的字节完全一致。

用法：
    /usr/bin/python3 tools/send_wallpaper_raw.py [图片路径]
    /usr/bin/python3 tools/send_wallpaper_raw.py --verify   # 顺便导出解回来的 PNG 对照
"""

import array
import asyncio
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blekit import CHAR_UUID, CdcLog, Link, find_keyboard, parse_status, wait_state  # noqa: E402

from bleak import BleakClient  # noqa: E402

W = H = 240
RAW_BYTES = W * H * 2          # 115200，和固件的 WP_RAW_BYTES 一致

CHUNK = 180                    # 和网页端一致：一个 ATT 写稳稳装下
CHUNK_GAP_S = 0.012            # 无响应写之间的间隔
PROBE_EVERY = 64               # 每传这么多片问一次固件"你收到多少了"
# ⚠ --no-probe：完全不问固件"收到多少了"。
#   2026-10-23 实测：三次传输崩溃分别发生在第 121 / 621 / 121 片，
#   而探测点在第 128 / 640 / 128 片 —— 每次都紧贴在下一次探测**之前**。
#   所以先用它验证"探测本身是不是触发点"。
PROBE_EVERY_OFF = 0

# ⚠ 传大块二进制时的"保守模式"参数。
#
# 2026-10-23 实测：固件在传输途中会随机 panic（boots 计数一路涨），
# 崩点每次都不一样（一次在 21780 字节，一次在 111780 字节），而传输路径
# 的代码和上一版**逐字节相同** —— 说明不是逻辑错，是 BLE 栈在高并发
# 写入 + 通知回包下的压力问题。
#
# 所以先试"慢一点、小一点"：分片更小 → 每包 ATT 负载更轻；间隔更长 →
# 给协议栈更多喘息时间。慢的代价只是多花几十秒。
SLOW_CHUNK = 100
SLOW_GAP_S = 0.035

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMG = os.path.join(HERE, "..", "..", "wallpaper", "yyq-mx9-wallpaper-240-v2.png")


def to_240(img):
    """缩到 240x240（和网页端一样：Math.min 等比 + 居中 letterbox，底色黑）。"""
    if img.size == (W, H):
        return img
    scale = min(W / img.width, H / img.height)
    nw, nh = max(1, int(img.width * scale)), max(1, int(img.height * scale))
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(img.resize((nw, nh), Image.LANCZOS), ((W - nw) // 2, (H - nh) // 2))
    return canvas


def pack_rgb565(img):
    """RGB888 → RGB565 小端字节流（115200 字节）。

    公式是 LVGL 的 lv_color_make 那一套：
      R 取高 5 位、G 取高 6 位、B 取高 5 位，R 放最高位。
    """
    vals = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            for (r, g, b) in img.getdata()]
    arr = array.array("H", vals)
    if sys.byteorder == "big":       # ESP32 是小端，这里对齐小端
        arr.byteswap()
    return arr.tobytes()


def unpack_rgb565(raw):
    """反过来解成 RGB888，用来肉眼核对打包对不对。"""
    arr = array.array("H")
    arr.frombytes(raw)
    if sys.byteorder == "big":
        arr.byteswap()
    out = bytearray()
    for v in arr:
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return Image.frombytes("RGB", (W, H), bytes(out))


async def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_verify = "--verify" in sys.argv
    # --no-cdc：不打开 USB CDC。
    #
    # 为什么需要这个开关：固件里 ROM 的 printf 被 setDebugOutput(true) 劫持到
    # TinyUSB CDC 之后，**两个来源会并发写同一个 CDC TX FIFO**：
    #   · 主循环的日志镜像任务 → USBCDC::write → tud_cdc_n_write
    #   · 蓝牙协议栈的 log_d()  → ROM printf → tud_cdc_n_write_char
    # TinyUSB 的 TX 缓冲不是线程安全的，并发写会把它写坏。
    # 主机不打开 CDC 口的话，USBCDC::write 会因为"没连上"直接返回 0，
    # 镜像任务完全不碰 TX FIFO → 竞争的另一头就没了。
    no_cdc = "--no-cdc" in sys.argv
    # --slow：保守模式（更小的分片 + 更长的间隔），见 SLOW_CHUNK 的说明
    slow = "--slow" in sys.argv
    chunk_size = SLOW_CHUNK if slow else CHUNK
    gap_s = SLOW_GAP_S if slow else CHUNK_GAP_S
    # --chunk=N / --gap=MS：手动指定
    # --addr=XXXX：指定目标设备（两台板子同名时用）
    want_addr = None
    for a in sys.argv[1:]:
        if a.startswith("--chunk="):
            chunk_size = int(a.split("=", 1)[1])
        if a.startswith("--gap="):
            gap_s = int(a.split("=", 1)[1]) / 1000.0
        if a.startswith("--addr="):
            want_addr = a.split("=", 1)[1].strip().upper()
    probe_every = PROBE_EVERY
    if "--no-probe" in sys.argv:
        probe_every = PROBE_EVERY_OFF
    img_path = args[0] if args else DEFAULT_IMG
    if not os.path.exists(img_path):
        print("✗ 图片不存在：%s" % img_path)
        return 1

    img = to_240(Image.open(img_path).convert("RGB"))
    raw = pack_rgb565(img)
    assert len(raw) == RAW_BYTES, len(raw)

    print("=== 图片 %s ===" % img_path)
    print("    240x240 → RGB565 小端 → %d 字节（分 %d 片，每片 %d 字节）"
          % (len(raw), (len(raw) + chunk_size - 1) // chunk_size, chunk_size))

    if want_verify:
        vp = os.path.join(HERE, "..", "assets", "raw_roundtrip.png")
        os.makedirs(os.path.dirname(vp), exist_ok=True)
        unpack_rgb565(raw).save(vp)
        print("    解回来的对照图：%s" % os.path.abspath(vp))

    cdc = CdcLog()
    # --cdc-port=XXX：指定 CDC 口。这台机器上有两块板，
    #   A 板（native USB，/dev/cu.usbmodemACA704E101E02）和
    #   B 板的 CH343（/dev/cu.usbmodem5C4C2142561）——
    #   自动 glob 会按字典序挑中 CH343，panic 回溯就抓不到了。
    cdc_port = None
    for a in sys.argv[1:]:
        if a.startswith("--cdc-port="):
            cdc_port = a.split("=", 1)[1]
    if cdc_port:
        cdc.start(port=cdc_port)
        print("    CDC: %s (指定)" % cdc.port)
    elif no_cdc:
        print("    CDC: 已按 --no-cdc 跳过（固件日志抓不到）")
    elif cdc.start():
        print("    CDC: %s" % cdc.port)
    else:
        print("    （没找到 CDC 节点，固件日志抓不到）")

    print("\n=== 扫描键盘 ===")
    addrs = await find_keyboard()
    if not addrs:
        print("✗ 没扫到键盘")
        cdc.stop()
        return 1
    if want_addr:
        hit = [a for a in addrs if a.upper().startswith(want_addr)]
        if not hit:
            print("✗ 没找到地址以 %s 开头的设备（扫到：%s）" % (want_addr, addrs))
            cdc.stop()
            return 1
        addrs = hit
    addr = addrs[0]
    print("  %s" % addr)

    ok = False
    try:
        async with BleakClient(addr, timeout=20) as c:
            chunk = min(chunk_size, max(20, c.mtu_size - 3))
            print("  协商 MTU=%d → 每片 %d 字节" % (c.mtu_size, chunk))

            link = Link(c)
            await link.start()
            await asyncio.sleep(0.3)

            # 1) 声明长度，让固件切进 RAW 接收模式
            await link.cmd("LOGO_RAW_START:%d" % RAW_BYTES, wait=0.3)

            # 2) 握手：等固件真的进了接收模式再发第一片二进制。
            #    LOGO_RAW_START 是文本命令（loop 任务处理），二进制分片走的是
            #    蓝牙协议栈任务的 probe —— 第一片抢在 START 前面到就会被当成
            #    文本命令吞掉，所以这里必须等。
            body = await wait_state(link, "RAW", want_total=RAW_BYTES, timeout=10.0)
            if body is None:
                print("  ✗ 固件没进入 RAW 接收模式 —— 看下面 CDC 日志")
                cdc.stop()
                return 1
            print("  握手 OK：LOGO_STATUS → %s" % body)

            # 3) 发数据。无响应写 + 间隔（网页端就是这么做的）：
            #    带 ACK 写每包都要等往返，几百片下来 BLE 栈容易超时踢连接。
            sent = 0
            failed = False
            for i in range(0, len(raw), chunk):
                try:
                    await c.write_gatt_char(CHAR_UUID,
                                            raw[i:i + chunk], response=False)
                except Exception as e:
                    print("\n  ⚠ 第 %d 片写入失败：%s: %s" % (i // chunk, type(e).__name__, e))
                    failed = True
                    break
                sent += len(raw[i:i + chunk])
                await asyncio.sleep(gap_s)

                n = i // chunk
                if n % 20 == 0:
                    print("    %d/%d 字节" % (sent, len(raw)), end="\r", flush=True)

                # 每 PROBE_EVERY 片问一次固件"你收到多少了"：
                # 丢片在这里就能看出来，而不是等最后一句"长度不匹配"才发现。
                #
                # ⚠ **最后一片之后不要问**：那一刻固件已经收齐、正忙着把
                #   115200 字节落盘（57 个 SPIFFS 文件），loop 任务正被占着，
                #   2 秒的轮询超时必然等不到 —— 于是"其实成功了"被报成
                #   "设备没回应了"。2026-10-23 实测踩过：传输明明 115200/115200
                #   零丢包，工具却报失败。
                last_chunk = (i + chunk >= len(raw))
                if probe_every and not last_chunk and (n + 1) % probe_every == 0:
                    st = await wait_state(link, "RAW", timeout=2.0, verbose=False)
                    if st is None:
                        print("\n  ⚠ 第 %d 片之后设备没回应了" % (n + 1))
                        failed = True
                        break
                    parsed = parse_status("LOGOSTATUS:" + st)
                    got = parsed[1] if parsed else -1
                    print("    %d/%d 字节（固件确认收到 %d）" % (sent, len(raw), got),
                          end="\r", flush=True)
                    if 0 <= got < sent:
                        print("\n  ⚠ 固件只收到 %d 字节，本地发了 %d —— 丢片了" % (got, sent))
                        failed = True
                        break

                if not c.is_connected:
                    print("\n  ⚠ 连接在第 %d 片时断开" % (i // chunk))
                    failed = True
                    break

            print("    %d/%d 字节%s" % (sent, len(raw), "" if not failed else "（中断）"))

            # 4) 显式收尾。少了这步，在途的字节会被当成文本命令灌进命令队列。
            if not failed:
                await asyncio.sleep(0.3)
                try:
                    await c.write_gatt_char(CHAR_UUID,
                                            b"LOGO_RAW_FLUSH\n", response=True)
                except Exception as e:
                    print("  ⚠ FLUSH 写失败：%s" % e)

                # 5) 等固件回报真实结果（它要 memcpy + 写盘 + 挂图片源）
                print("\n=== 等固件落盘 ===")
                # 落盘 57 个 SPIFFS 文件要好几秒（实测 5~15 秒），
                # 这里给 90 次机会，别刚问两句就判失败。
                for _ in range(90):
                    await asyncio.sleep(0.5)
                    try:
                        await c.write_gatt_char(CHAR_UUID,
                                                b"LOGO_STATUS\n", response=True)
                    except Exception:
                        break
                    await asyncio.sleep(0.25)
                    for nt in link.dump_notes():
                        if "LOGOSTATUS" in nt:
                            print("  ← %s" % nt)
                            parsed = parse_status(nt)
                            if parsed and parsed[0] == "OK":
                                ok = True
                    if ok:
                        break
                    if not c.is_connected:
                        print("  ⚠ 设备掉线了")
                        break

            # 6) 切到壁纸主屏（风格 6 = DISP_MODE 5）
            if c.is_connected:
                try:
                    await c.write_gatt_char(CHAR_UUID,
                                            b"DISP_MODE:5\n", response=True)
                    await asyncio.sleep(1.5)
                    print("\n  已切到壁纸主屏")
                except Exception as e:
                    print("  ⚠ 切屏失败：%s" % e)
    except Exception as e:
        print("✗ BLE 会话异常：%s: %s" % (type(e).__name__, e))

    # 崩溃之后固件会自动重启，开机时会把「上次 panic 现场」打进 CDC。
    # 这里**多守一会儿**，不然 0.5 秒就关口，窗口根本来不及（2~5 秒才重启完）。
    keep = 25
    for a in sys.argv[1:]:
        if a.startswith("--keep="):
            keep = int(a.split("=", 1)[1])
    if keep:
        print("\n（守着 CDC 再等 %d 秒，等固件重启把 panic 现场打出来）" % keep)
        await asyncio.sleep(keep)
    cdc.stop()

    print("\n=== CDC 抓到的固件日志 ===")
    cdc.dump()

    if not ok:
        print("\n⚠ 壁纸没落到键盘上")
        return 1
    print("\n✅ 壁纸已写入 /logo.bin 并挂到主屏")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
