#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BLE 探针客户端：连上探针固件，量出 conn_id / MTU / 分片行为，并反复重连。

macOS 的 CoreBluetooth **不暴露**未连接设备的广播名，所以只能挨个连、
看 GATT 里有没有本机的 service 来认人。

用法：
    /usr/bin/python3 tools/ble_probe_client.py find          # 只找出设备地址
    /usr/bin/python3 tools/ble_probe_client.py run           # 连上 + 重连 N 轮 + 收诊断
    /usr/bin/python3 tools/ble_probe_client.py run --cycles 5
"""

import asyncio
import json
import re
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "YYQ-MX9.0"


async def find_keyboard(timeout=8.0):
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    print("扫描到 %d 个候选，逐个探测 GATT…" % len(addrs), flush=True)
    sem = asyncio.Semaphore(8)

    async def probe(addr):
        async with sem:
            try:
                async with BleakClient(addr, timeout=6) as c:
                    for s in c.services:
                        if SERVICE_UUID.lower() in str(s.uuid).lower():
                            return addr
            except Exception:
                pass
            return None

    hits = [h for h in await asyncio.gather(
        *[probe(a) for a in addrs], return_exceptions=True)
        if h and not isinstance(h, Exception)]
    return hits[0] if hits else None


class Sink:
    """收固件推过来的通知，按 bt_link 的协议拼包。"""

    def __init__(self):
        self.lines = []
        self.raw = bytearray()

    def feed(self, _sender, data):
        self.raw.extend(data)
        try:
            self.lines.append(bytes(data).decode("utf-8", "replace"))
        except Exception:
            pass

    def find(self, verb):
        out = []
        for l in self.lines:
            m = re.match(r"^" + verb + r":([^:]*):(.*)$", l, re.S)
            if m:
                out.append((m.group(1), m.group(2)))
        return out


async def one_cycle(addr, idx, sink, cmd=None):
    """连一次 → 收诊断 → 断 → 等它重连。"""
    print("\n---------- 第 %d 轮连接 ----------" % idx, flush=True)
    try:
        async with BleakClient(addr, timeout=8) as c:
            print("  已连接", flush=True)
            await c.start_notify(CHAR_UUID, sink.feed)
            await asyncio.sleep(0.6)
            if cmd:
                await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
                await asyncio.sleep(1.2)
            await asyncio.sleep(0.8)
            await c.stop_notify(CHAR_UUID)
    except Exception as e:
        print("  连接失败: %r" % (e,), flush=True)


async def main():
    argv = sys.argv
    cycles = 5
    if "--cycles" in argv:
        cycles = int(argv[argv.index("--cycles") + 1])
    cmd = None
    if "--cmd" in argv:
        cmd = argv[argv.index("--cmd") + 1]

    addr = await find_keyboard()
    if not addr:
        print("✗ 没找到带 YYQ service 的 BLE 设备")
        return 1
    print("✓ 找到键盘: %s" % addr, flush=True)

    sink = Sink()
    for i in range(1, cycles + 1):
        await one_cycle(addr, i, sink, cmd if i == 1 else None)
        await asyncio.sleep(1.5)

    print("\n================= 固件诊断行 =================")
    for l in sink.lines:
        if not re.match(r"^[A-Z0-9_]+:", l):
            print("  " + l)
    print("\n================= 通知原样（前 40 条）=================")
    for l in sink.lines[:40]:
        print("  " + l)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
