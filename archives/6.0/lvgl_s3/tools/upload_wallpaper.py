#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把新壁纸传到键盘上（复刻网页端 s3-setting.html 的那套协议）。

协议（固件侧见 src/lvgl_s3.ino 的 handleCommand）：
  1. 文本命令 "LOGO_JPEG_START:<长度>"  → 固件开接收模式
  2. 裸字节分片（每片 ≤ 协商 MTU - 3） → 固件收进 logoRxBuf
  3. 文本命令 "LOGO_JPEG_FLUSH"           → 固件收尾
  4. 固件解码 RGB565 + 写 /logo.bin，然后通知 "LOGOSTATUS:OK:0/0"

⚠ 第 3 步不能省：少了它，固件会"收满 total 就自己开解码"，
  而最后几包还在蓝牙栈里排队 —— 那几包在途的 JPEG 字节会被当成
  文本命令灌进命令队列。

=====================================================================
⚠⚠ 这版和上一版最大的差别：**发第一片二进制之前必须等固件真的进入接收模式**
=====================================================================

上一版发完 LOGO_JPEG_START 就立刻发二进制分片，于是踩到一个跨任务的竞态：

  · LOGO_JPEG_START 是**文本命令**：它在 loop 任务里被解析，
    处理完才把 logoRxActive 置 true、logoRxTotal 填好。
  · 而 BLE 写回调（onWrite → btLinkOnWrite）跑在**蓝牙协议栈任务**里，
    它靠 logoRxProbe() 判断"这包是二进制还是文本"，判据就是 logoRxActive。
  · BLE 的 ATT 写响应是协议栈**收到包就回**的，不代表 loop 任务已经处理完
    那条命令 —— 于是第一片二进制完全可能比 START 先到协议栈。
  · 那一刻 logoRxActive 还是 false → probe 返回 false → **这包 JPEG 字节
    被当成文本命令**灌进行缓冲，等遇到第一个 0x0A/0x0D 就被当成一条命令
    提交给 handleCommand()。

所以这一版在 START 之后**轮询 LOGO_STATUS**，等到固件回报
`RECV:0/<总数>` 才发第一片二进制。LOGO_STATUS 走的是同一套文本通道，
它能被处理就说明 START 也已经被处理过了 —— 这是唯一可靠的握手点。

用法：
    /usr/bin/python3 tools/upload_wallpaper.py [图片路径]
"""

import asyncio
import glob
import io
import os
import sys
import threading
import time

try:
    import serial
except ImportError:
    serial = None

from bleak import BleakClient, BleakScanner
from PIL import Image

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "yyq2 号"

# 网页端用 180 字节：足够小，一个 ATT Write Request 稳稳装得下。
# 固件侧 logoRxBuf 一次收 1024 字节，但小包更不容易触发
# "GATT Server is disconnected"。
CHUNK = 180
CHUNK_GAP_S = 0.012

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMG = os.path.join(HERE, "..", "..", "wallpaper", "yyq-mx9-wallpaper-240-v2.png")

# ---------------------------------------------------------------- CDC 抓日志
cdc_lines = []
cdc_lock = threading.Lock()
cdc_seen = threading.Event()


def find_cdc_port():
    cands = []
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"):
        cands += glob.glob(pat)
    return cands


def cdc_reader(port, stop_evt):
    """持续读 CDC。

    ⚠ dtr/rts 都置 False：这台机器上 DTR=True 会把 IO0 拉低，
    等于把芯片踢进下载模式（esptool 的 reset.py 里 _setDTR(True) // IO0=LOW）。
    调试固件用的是 TinyUSB（ARDUINO_USB_MODE=0），DTR 不参与 IO0，
    但保持 False 更安全。
    """
    if serial is None:
        return
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.2
        # ⚠ DTR 和 RTS 必须同时为 True，TinyUSB 才认为主机已连接
        #   （USBCDC.cpp 的 _onLineState：dtr && rts && !connected 才置 connected）。
        #   否则 USBCDC::write() 直接 return 0，主机这边收不到任何字节。
        #   别走 1200 波特：那会触发 IDE 的重启握手。
        ser.dtr = True
        ser.rts = True
        ser.open()
    except Exception as e:
        with cdc_lock:
            cdc_lines.append("[CDC] 打开失败: %s" % e)
        return
    with cdc_lock:
        cdc_lines.append("[CDC] 已打开 %s" % port)
    buf = b""
    while not stop_evt.is_set():
        try:
            chunk = ser.read(1024)
        except Exception:
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            txt = raw.decode("utf-8", "replace").rstrip()
            with cdc_lock:
                cdc_lines.append("[CDC] " + txt)
            cdc_seen.set()
    try:
        ser.close()
    except Exception:
        pass


def cdc_dump():
    with cdc_lock:
        for l in cdc_lines:
            print(l)


def cdc_tail(n=40):
    with cdc_lock:
        return list(cdc_lines)[-n:]


def cdc_wait_for(substr, timeout=20.0):
    """在 CDC 日志里等某一行出现（固件自己的 printf）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        with cdc_lock:
            for l in cdc_lines:
                if substr in l:
                    return l
        time.sleep(0.15)
    return None


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


class Link:
    """一条 BLE 会话 + 收包缓冲。"""

    def __init__(self, client):
        self.c = client
        self.notes = []
        self.alive = True

    def _cb(self, sender, data):
        self.notes.append(data.decode("utf-8", "replace").strip())

    async def start(self):
        await self.c.start_notify(CHAR_UUID, self._cb)

    async def cmd(self, text, wait=0.6):
        """发一条文本命令并等回包（如果有）。"""
        self.notes.clear()
        try:
            await self.c.write_gatt_char(CHAR_UUID, (text + "\n").encode(), response=True)
        except Exception as e:
            self.alive = False
            raise
        await asyncio.sleep(wait)
        return list(self.notes)

    def dump_notes(self):
        return list(self.notes)


async def wait_start_handshake(link, total, timeout=8.0):
    """等固件真的进入接收模式。

    这是本版最关键的改动：LOGO_JPEG_START 是文本命令，它在 loop 任务里
    被处理；而二进制分片走的是蓝牙协议栈任务的 probe。第一片二进制
    如果抢在 START 前面到，就会被当成文本命令灌进命令队列。

    LOGO_STATUS 走的是同一条文本通道 —— 它能回包，就说明 START 已经被
    处理过了（命令队列是 FIFO，START 排在前面）。
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        notes = await link.cmd("LOGO_STATUS", wait=0.35)
        for n in notes:
            if n.startswith("LOGOSTATUS:"):
                body = n.split(":", 1)[1]
                state = body.split(":")[0]
                print("    握手 LOGO_STATUS → %s" % body)
                if state == "RECV" and body.endswith("/%d" % total):
                    return True
        if not link.alive:
            return False
    return False


async def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    no_notify = "--no-notify" in sys.argv
    img_path = args[0] if args else DEFAULT_IMG
    if not os.path.exists(img_path):
        print("✗ 图片不存在：%s" % img_path)
        return 1

    data = encode_wallpaper(img_path)
    print("=== 图片 %s ===" % img_path)
    print("    JPEG q95 → %.1f KB（分 %d 片，每片 %d 字节）"
          % (len(data) / 1024, (len(data) + CHUNK - 1) // CHUNK, CHUNK))
    if no_notify:
        print("    ⚠ --no-notify：不订阅通知。"
              "固件的 notify() 会因为「客户端没开通知」而提前返回，"
              "也就不会去遍历 BLEServer 那张无锁的 peer 表。"
              "用来验证崩溃是不是出在通知发送路径上。")

    stop_evt = threading.Event()
    ports = find_cdc_port()
    if ports:
        threading.Thread(target=cdc_reader, args=(ports[0], stop_evt), daemon=True).start()
        time.sleep(0.6)   # 等 CDC 端口真的打开
    else:
        print("（没找到 CDC 串口节点，固件日志抓不到）")

    print("\n=== 扫描键盘 ===")
    addrs = await find_keyboard()
    if not addrs:
        print("✗ 没扫到键盘")
        stop_evt.set()
        return 1
    addr = addrs[0]
    print("  %s" % addr)

    ok = False

    try:
        async with BleakClient(addr, timeout=15) as c:
            mtu = c.mtu_size
            chunk = min(CHUNK, max(20, mtu - 3))
            print("  协商 MTU=%d → 每片 %d 字节（共 %d 片）"
                  % (mtu, chunk, (len(data) + chunk - 1) // chunk))

            link = Link(c)
            if not no_notify:
                await link.start()
            await asyncio.sleep(0.3)

            # 1) 开接收模式。这条**必须**带 ACK（它是文本命令，要确认固件收到了）
            await link.cmd("LOGO_JPEG_START:%d" % len(data), wait=0.3)

            # 2) 握手：等固件真的进入接收模式，再发第一片二进制
            print("  等固件进入接收模式 ...")
            if no_notify:
                line = cdc_wait_for("status: RECV:0/%d" % len(data), timeout=8.0)
                print("    %s" % (line.strip() if line else "⚠ CDC 里没等到 RECV"))
            else:
                if not await wait_start_handshake(link, len(data)):
                    print("  ⚠ 握手没成功（固件没回报 RECV），继续发（可能丢第一片）")
            await asyncio.sleep(0.2)

            # 3) 裸字节分片。
            #    ⚠ 用**无响应写**（response=False）+ 12ms 间隔 —— 网页端就是这么做的：
            #      · 带 ACK 写每包都要等往返，81 片下来 BLE 栈容易超时踢连接
            #      · 无响应写连写太快会让 NimBLE 的 HCI 发送队列爆
            sent = 0
            for i in range(0, len(data), chunk):
                try:
                    await c.write_gatt_char(CHAR_UUID, data[i:i + chunk], response=False)
                except Exception as e:
                    print("\n  ⚠ 第 %d 片写入失败：%s: %s" % (i // chunk, type(e).__name__, e))
                    print("     → 设备在这一步掉线了（多半是固件崩了），"
                          "看下面 CDC 日志里最后几行")
                    break
                sent += len(data[i:i + chunk])
                await asyncio.sleep(CHUNK_GAP_S)
                if (i // chunk) % 20 == 0:
                    print("    %d/%d 字节" % (sent, len(data)), end="\r", flush=True)
                if not c.is_connected:
                    print("\n  ⚠ 连接在第 %d 片时断开" % (i // chunk))
                    break
            print("    %d/%d 字节 ✓" % (sent, len(data)))

            # 4) 显式收尾 —— 少了这步，在途的 JPEG 字节会被当成命令灌进命令队列
            await asyncio.sleep(0.3)
            try:
                await c.write_gatt_char(CHAR_UUID, b"LOGO_JPEG_FLUSH\n", response=True)
            except Exception as e:
                print("  ⚠ FLUSH 写失败：%s" % e)

            # 5) 轮询真实结果
            print("\n=== 等固件回报 ===")
            if no_notify:
                line = cdc_wait_for("/logo.bin written", timeout=25.0)
                print("    %s" % (line.strip() if line else "⚠ 没等到 /logo.bin written"))
                ok = line is not None
            else:
                for _ in range(30):
                    await asyncio.sleep(0.5)
                    try:
                        await c.write_gatt_char(CHAR_UUID, b"LOGO_STATUS\n", response=True)
                    except Exception:
                        break
                    await asyncio.sleep(0.25)
                    for n in link.dump_notes():
                        if "LOGOSTATUS" in n:
                            print("  ← %s" % n)
                            if ":OK:" in n or n.endswith(":OK") or ":READY:" in n:
                                ok = True
                    if ok:
                        break
                    if not c.is_connected:
                        print("  ⚠ 设备掉线了")
                        break

            # 6) 切到壁纸主屏（风格 6 = DISP_MODE 5）
            if c.is_connected:
                try:
                    await c.write_gatt_char(CHAR_UUID, b"DISP_MODE:5\n", response=True)
                    await asyncio.sleep(1.5)
                    for n in link.dump_notes():
                        print("  ← %s" % n[:120])
                except Exception as e:
                    print("  ⚠ 切屏失败：%s" % e)
    except Exception as e:
        print("✗ BLE 会话异常：%s: %s" % (type(e).__name__, e))

    await asyncio.sleep(0.5)
    stop_evt.set()
    time.sleep(0.5)

    print("\n=== CDC 抓到的固件日志 ===")
    cdc_dump()

    if not ok:
        print("\n⚠ 没收到成功的 LOGOSTATUS —— 壁纸没落到键盘上")
        return 1
    print("\n✅ 壁纸已上传并写入 /logo.bin，已切到壁纸主屏")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
