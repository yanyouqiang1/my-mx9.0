#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
B 板（空板 + USB CDC 调试固件）的诊断脚本。

要抓的东西就一件：**为什么"连上又断开"之后设备就再也扫不到了**。

抓法：
  1. 全程记录 /dev/cu.usbmodem* 上的串口（UART0 日志 + Arduino BLE 库内部日志）
  2. 等应用启动（看到 [BOOT] 就说明起来了）
  3. 用 BLE 连一次、发条指令、再断开 —— 这正是"死掉"的触发动作
  4. 连完之后持续观察广播有没有重新起来
  5. 把所有跟广播/连接相关的行挑出来

用法：
    /usr/bin/python3 tools/diag_ble.py [串口路径]
"""

import asyncio
import os
import re
import sys
import time

import serial
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
LOGFILE = "/tmp/boardB.log"

INTEREST = re.compile(
    r"advertis|ADV_|adv_|startAdvert|start_advert|stopAdvert|stop_advert|"
    r"BLE|GAP|conn|Conn|CONN|disconn|Disconn|DISCONN|"
    r"mtu|MTU|notify|Notify|openAdvertising|advertising",
    re.IGNORECASE)


def open_port(path, baud=115200):
    s = serial.serial_for_url(path, do_not_open=True)
    s.baudrate = baud
    s.timeout = 0.15
    s.write_timeout = 1.0
    s._dtr_state = False
    s._rts_state = False
    s.open()
    return s


class Recorder:
    def __init__(self, path):
        self.path = path
        self.ser = None
        self.f = open(LOGFILE, "wb", buffering=0)
        self.stop = False

    def run(self):
        while not self.stop:
            if self.ser is None:
                try:
                    self.ser = open_port(self.path)
                    self.f.write(b"\n===== recorder attached =====\n")
                except Exception:
                    time.sleep(1)
                    continue
            try:
                d = self.ser.read(4096)
            except Exception:
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(0.5)
                continue
            if d:
                self.f.write(d)
            else:
                time.sleep(0.02)

    def close(self):
        self.stop = True
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass


async def find_kb(timeout=6.0):
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    sem = asyncio.Semaphore(8)

    async def probe(a):
        async with sem:
            try:
                async with BleakClient(a, timeout=4) as c:
                    for s in c.services:
                        if SERVICE_UUID.lower() in str(s.uuid).lower():
                            return a
            except Exception:
                pass
            return None

    hits = [h for h in await asyncio.gather(
        *[probe(a) for a in addrs], return_exceptions=True)
        if h and not isinstance(h, Exception)]
    return hits[0] if hits else None


async def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
    rec = Recorder(port)
    import threading
    t = threading.Thread(target=rec.run, daemon=True)
    t.start()
    print("[diag] 记录器已挂到 %s → %s" % (port, LOGFILE), flush=True)

    # ---- 等应用启动 ----
    print("[diag] 等应用启动（看 [BOOT]）…", flush=True)
    booted = False
    t0 = time.time()
    while time.time() - t0 < 120:
        try:
            with open(LOGFILE, "rb") as f:
                data = f.read()
        except Exception:
            data = b""
        if b"[BOOT]" in data or b"[LVGL] Ready" in data:
            booted = True
            break
        await asyncio.sleep(1)
    if not booted:
        print("[diag] ✗ 120 秒内应用没启动 —— 可能真的卡住了（这就是现场！）", flush=True)
    else:
        print("[diag] ✓ 应用已启动", flush=True)

    await asyncio.sleep(2)

    # ---- 触发：连一次 → 发指令 → 断开 ----
    print("\n[diag] 找 BLE 设备…", flush=True)
    kb = await find_kb()
    if not kb:
        print("[diag] ✗ 扫不到设备", flush=True)
    else:
        print("[diag] 连上 %s" % kb, flush=True)
        async with BleakClient(kb, timeout=10) as c:
            await asyncio.sleep(0.5)
            try:
                await c.write_gatt_char(CHAR_UUID, b"LOG:on\n", response=True)
                await asyncio.sleep(0.5)
                await c.write_gatt_char(CHAR_UUID, b"CFGGET\n", response=True)
                await asyncio.sleep(1.5)
            except Exception as e:
                print("[diag] 写失败 %r" % e, flush=True)
        print("[diag] 已断开（这一步之后广播就死了）", flush=True)

    # ---- 观察广播有没有回来 ----
    print("[diag] 观察 25 秒，看广播起没起来…", flush=True)
    for i in range(5):
        await asyncio.sleep(5)
        kb2 = await find_kb(timeout=4.0)
        print("   第 %d 次探测: %s" % (i + 1, "✓ 扫到了" if kb2 else "✗ 扫不到"),
              flush=True)
        if kb2:
            break

    await asyncio.sleep(2)
    rec.close()

    # ---- 汇报 ----
    print("\n" + "=" * 70)
    print("广播/连接相关的日志行")
    print("=" * 70)
    try:
        with open(LOGFILE, "rb") as f:
            lines = f.read().decode("utf-8", "replace").splitlines()
    except Exception as e:
        print("读日志失败 %r" % e)
        return 1
    n = 0
    for ln in lines:
        if INTEREST.search(ln):
            print("  " + ln[:200])
            n += 1
    print("（共 %d 行相关；日志总行数 %d）" % (n, len(lines)))
    print("\n===== 最后 40 行全文 =====")
    for ln in lines[-40:]:
        print("  " + ln[:200])
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
