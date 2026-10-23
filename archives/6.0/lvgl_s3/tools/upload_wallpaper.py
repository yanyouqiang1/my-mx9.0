#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把新壁纸传到键盘上（复刻网页端 s3-setting.html 的那套协议）。

为什么要写这个脚本：网页端那套是给人在浏览器里点"上传"用的，
本轮要验的是**新固件 + 新壁纸图**的组合效果，得有个能自动跑、
并且能拿到**真实结果**（解码 + 写盘都完成）的通道。

协议（固件侧见 src/lvgl_s3.ino 的 handleCommand）：
  1. 文本命令 "LOGO_JPEG_START:<长度>"  → 固件开接收模式
  2. 裸字节分片（每片 ≤ 180 字节）        → 固件收进 logoRxBuf
  3. 文本命令 "LOGO_JPEG_FLUSH"           → 固件收尾
  4. 固件解码 RGB565 + 写 /logo.bin，然后通知 "LOGOSTATUS:OK:0/0"

⚠ 第 3 步不能省：少了它，固件会"收满 total 就自己开解码"，
   而最后几包还在蓝牙栈里排队 —— 那几包在途的 JPEG 字节会被当成
   文本命令灌进命令队列。

用法：
    /usr/bin/python3 tools/upload_wallpaper.py [图片路径]
"""

import asyncio
import io
import os
import sys

from bleak import BleakClient, BleakScanner
from PIL import Image

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "yyq2 号"

# 网页端用 180 字节：足够小，一个 ATT Write Request 稳稳装下。
# 固件侧 logoRxBuf 一次收 1024 字节，但小包更不容易触发
# "GATT Server is disconnected"。
CHUNK = 180

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMG = os.path.join(HERE, "..", "..", "wallpaper", "yyq-mx9-wallpaper-240-v2.png")


def encode_wallpaper(path):
    """PNG → 240x240 → JPEG q95（和网页端 canvas.toBlob(0.95) 同档）。

    为什么必须 0.95：0.82 对**细线稿**是最坏的一档 —— JPEG 是 8x8 分块 + DCT，
    1px 的亮线压在近黑底上正好落在量化系数最陡的区间，编码出来是
    "线发虚 + 线边一圈毛刺"，240 的 TFT 上一眼看过去就是"糊"。
    """
    im = Image.open(path).convert("RGB")
    if im.size != (240, 240):
        # 网页端是 Math.min 缩放 + 居中（letterbox），这里照做
        scale = min(240 / im.width, 240 / im.height)
        w, h = int(im.width * scale), int(im.height * scale)
        canvas = Image.new("RGB", (240, 240), (0, 0, 0))
        canvas.paste(im.resize((w, h), Image.LANCZOS), ((240 - w) // 2, (240 - h) // 2))
        im = canvas
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=95)
    return buf.getvalue()


async def find_keyboard(timeout=10.0):
    """按**本机 service UUID** 认人，不能按名字。

    ⚠ 这台机器上还有一台同样叫 "yyq2 号" 的 HID 设备（macOS 里配对过的
    另一台键盘）。按名字挑会连到它身上去 —— 它的 GATT 里没有本机的
    service/特征，一发 notify 就报 CharacteristicNotFound。
    """
    found = []

    def cb(device, ad):
        svc = [str(u).lower() for u in (ad.service_uuids or [])]
        if any(SERVICE_UUID.lower() in u for u in svc):
            found.append(device.address)
            return
        name = ad.local_name or device.name or ""
        # 广播里可能不带 service_uuids（iOS 不上报），先记下来待会儿用 GATT 认
        if DEV_NAME in name or "YYQ-MX9" in name.upper():
            found.append(device.address)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()

    # 按 GATT 实测过滤：只有带本机 service + 特征的那个才是键盘
    good = []
    for addr in dict.fromkeys(found):
        try:
            async with BleakClient(addr, timeout=6) as c:
                ok_svc = any(SERVICE_UUID.lower() in str(s.uuid).lower()
                             for s in c.services)
                ok_ch = any(CHAR_UUID.lower() in str(ch.uuid).lower()
                            for s in c.services for ch in s.characteristics)
                if ok_svc and ok_ch:
                    good.append(addr)
        except Exception:
            continue
    return good


async def main():
    img_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IMG
    if not os.path.exists(img_path):
        print("✗ 图片不存在：%s" % img_path)
        return 1

    data = encode_wallpaper(img_path)
    print("=== 图片 %s ===" % img_path)
    print("    JPEG q95 → %.1f KB（分 %d 片，每片 %d 字节）"
          % (len(data) / 1024, (len(data) + CHUNK - 1) // CHUNK, CHUNK))

    print("\n=== 扫描键盘 ===")
    addrs = await find_keyboard()
    if not addrs:
        print("✗ 没扫到键盘")
        return 1
    addr = addrs[0]
    print("  %s" % addr)

    notes = []

    async with BleakClient(addr, timeout=15) as c:
        await c.start_notify(CHAR_UUID, lambda s, d: notes.append(
            d.decode("utf-8", "replace").strip()))

        # 1) 开接收模式。这条**必须**带 ACK（它是文本命令，要确认固件收到了）
        await c.write_gatt_char(CHAR_UUID, ("LOGO_JPEG_START:%d\n" % len(data)).encode(),
                                response=True)
        await asyncio.sleep(0.3)

        # 2) 裸字节分片。
        #    ⚠ 用**无响应写**（response=False）+ 1ms 间隔 —— 网页端就是这么做的：
        #      · 带 ACK 写每包都要等往返，81 片下来 BLE 栈容易超时踢连接
        #        （实测用 response=True 传第二片就 disconnected）
        #      · 无响应写连写太快会让 NimBLE 的 HCI 发送队列爆，
        #        后面几十包会陆续报 "GATT Server is disconnected"，
        #        所以每包之间留 1ms，让 NimBLE 把上一包交给控制器
        for i in range(0, len(data), CHUNK):
            await c.write_gatt_char(CHAR_UUID, data[i:i + CHUNK], response=False)
            await asyncio.sleep(0.012)
            if i % (CHUNK * 20) == 0:
                print("    %d/%d 字节" % (i, len(data)), end="\r", flush=True)
        print("    %d/%d 字节 ✓" % (len(data), len(data)))

        # 3) 显式收尾 —— 少了这步，在途的 JPEG 字节会被当成命令灌进命令队列
        await asyncio.sleep(0.3)
        await c.write_gatt_char(CHAR_UUID, b"LOGO_JPEG_FLUSH\n", response=True)

        # 4) 轮询真实结果
        print("\n=== 等固件回报 ===")
        ok = False
        for _ in range(24):
            await asyncio.sleep(0.5)
            for n in list(notes):
                if "LOGOSTATUS" in n:
                    print("  ← %s" % n)
                    if "OK" in n:
                        ok = True
            if ok:
                break
        notes.clear()

        # 5) 切到壁纸主屏（风格 6 = DISP_MODE 5）
        await c.write_gatt_char(CHAR_UUID, b"DISP_MODE:5\n", response=True)
        await asyncio.sleep(0.5)
        for n in list(notes):
            print("  ← %s" % n[:120])

    if not ok:
        print("\n⚠ 没收到 LOGOSTATUS:OK —— 解码可能没成功")
        return 1
    print("\n✅ 壁纸已上传并写入 /logo.bin，已切到壁纸主屏")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
