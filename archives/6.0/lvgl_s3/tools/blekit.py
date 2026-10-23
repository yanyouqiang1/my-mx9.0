#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BLE 工具的公共部分：认人、发命令、收通知、CDC 抓固件日志。

单独抽出来是因为上传壁纸的两条路（RAW / JPEG）都要用同一套：
  · 按 **service UUID** 认人（这台机器上还有一台同名的 HID 设备）
  · 通过 CDC 抓固件自己的 printf（debug 固件才有这个镜像）
  · 同一个"等固件进入接收模式"的握手（跨任务，必须等）
"""

import asyncio
import glob
import re
import threading
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "yyq2 号"

# 状态回包的线上格式： bt_link 的规则是 `VERB:KEY:<正文>`，
# 所以 LOGOSTATUS 的回包长这样：
#     LOGOSTATUS:<请求号>:RAW:0/115200
# 请求号是 bt_link 自动加的（主机没带 #123 时退化成自增 key），
# **每次都不一样**。所以解析不能按冒号切死位置 —— 必须用正则把
# 状态词和后面的 "已收/总数" 抠出来。
#
# ⚠ 踩过的坑：按 split(":", 1) 取第二段会拿到**请求号**，
#   于是状态永远是 "<数字>"，握手判定永远不成立 —— 表现是
#   "固件明明回了 RAW:0/115200，脚本却说没进接收模式"。
STATUS_RE = re.compile(r":(RAW|RECV|READY|OK|IDLE):(\d+)/(\d+)\s*$")


def parse_status(line):
    """把一条 LOGOSTATUS 回包解析成 (状态, 已收, 总数)。认不出来就返回 None。"""
    m = STATUS_RE.search(line.strip())
    if not m:
        return None
    return m.group(1), int(m.group(2)), int(m.group(3))


# ---------------------------------------------------------------- CDC 抓日志
class CdcLog:
    """持续读 USB CDC，把固件自己的 printf 收下来。

    ⚠ DTR 和 RTS **必须同时为 True**，TinyUSB 才会把主机判成"已连接"
      （USBCDC.cpp 的 _onLineState：只有 dtr && rts && !connected 才置 connected）。
      connected=false 时 USBCDC::write() 直接 return 0 —— 固件写得再对，
      主机这边一个字节也收不到。
    ⚠ 别走"1200 波特触摸"那条重启路径：波特率绝不能是 1200，
      从 (F,F) 直接跳到 (T,T) 只会命中 dtr&&rts 分支，不会重启。
    """

    def __init__(self):
        self.lines = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self.port = None

    def start(self):
        cands = []
        for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"):
            cands += glob.glob(pat)
        if not cands:
            return False
        self.port = cands[0]
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        time.sleep(0.6)   # 等端口真的打开
        return True

    def _reader(self):
        try:
            import serial
        except ImportError:
            return
        try:
            ser = serial.Serial()
            ser.port = self.port
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.dtr = True
            ser.rts = True
            ser.open()
        except Exception as e:
            with self._lock:
                self.lines.append("[CDC] 打开失败: %s" % e)
            return
        with self._lock:
            self.lines.append("[CDC] 已打开 %s" % self.port)
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = ser.read(1024)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                with self._lock:
                    self.lines.append("[CDC] " + raw.decode("utf-8", "replace").rstrip())

    def stop(self):
        self._stop.set()

    def tail(self, n=40):
        with self._lock:
            return list(self.lines)[-n:]

    def wait_for(self, substr, timeout=20.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self._lock:
                for l in self.lines:
                    if substr in l:
                        return l
            time.sleep(0.15)
        return None

    def dump(self):
        with self._lock:
            for l in self.lines:
                print(l)


# ---------------------------------------------------------------- 认人
async def find_keyboard(timeout=10.0):
    """按 GATT 里的 service UUID 认人。

    ⚠ 这台机器上还有一台同样叫 "yyq2 号" 的 HID 设备。按名字挑会连错 ——
    它的 GATT 里没有本机的 service/特征。
    """
    found = []

    def cb(device, ad):
        svc = [str(u).lower() for u in (ad.service_uuids or [])]
        if any(SERVICE_UUID.lower() in u for u in svc):
            found.append(device.address)
            return
        name = ad.local_name or device.name or ""
        if DEV_NAME in name or "YYQ-MX9" in name.upper():
            found.append(device.address)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()

    good = []
    for addr in dict.fromkeys(found):
        try:
            async with BleakClient(addr, timeout=6) as c:
                ok_svc = any(SERVICE_UUID.lower() in str(s.uuid).lower() for s in c.services)
                ok_ch = any(CHAR_UUID.lower() in str(ch.uuid).lower()
                            for s in c.services for ch in s.characteristics)
                if ok_svc and ok_ch:
                    good.append(addr)
        except Exception:
            continue
    return good


# ---------------------------------------------------------------- 命令通道
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
        except Exception:
            self.alive = False
            raise
        await asyncio.sleep(wait)
        return list(self.notes)

    def dump_notes(self):
        return list(self.notes)


async def wait_state(link, prefix, want_total=None, timeout=10.0, verbose=True):
    """轮询 LOGO_STATUS，等固件回报指定状态。

    这是**跨任务握手的唯一可靠办法**：
    LOGO_RAW_START / LOGO_JPEG_START 是文本命令，在 loop 任务里处理；
    而二进制分片走的是蓝牙协议栈任务的 probe（判据是 logoRxActive）。
    BLE 的 ATT 写响应是协议栈**收到包就回**的，不代表 loop 任务已经处理完 ——
    所以第一片二进制完全可能抢在 START 前面到，那一片会被当成文本命令吞掉。
    LOGO_STATUS 走同一条文本通道，它能回包就说明 START 已经被处理过了。
    """
    t0 = time.time()
    last = ""
    while time.time() - t0 < timeout:
        notes = await link.cmd("LOGO_STATUS", wait=0.35)
        for n in notes:
            parsed = parse_status(n)
            if parsed is None:
                continue
            state, got, total = parsed
            last = "%s:%d/%d" % (state, got, total)
            if verbose:
                print("    握手 LOGO_STATUS → %s" % last)
            if state == prefix and (want_total is None or total == want_total):
                return last
        if not link.alive:
            return None
    return None
