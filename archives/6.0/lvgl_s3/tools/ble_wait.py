#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""等 BLE 设备出现（用于"拔线再插线"之后确认应用起来了）。

用法:
    /usr/bin/python3 tools/ble_wait.py [超时秒数] [名字片段]
"""
import asyncio
import sys
import time

from bleak import BleakScanner

TIMEOUT = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
TARGET = sys.argv[2] if len(sys.argv) > 2 else "YYQ-MX9.0"

found = {}


def cb(dev, adv):
    name = adv.local_name or dev.name or ""
    if TARGET.lower() in name.lower():
        found[dev.address] = (name, adv.rssi)


async def main():
    t0 = time.monotonic()
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        while time.monotonic() - t0 < TIMEOUT and not found:
            await asyncio.sleep(0.5)
    finally:
        await scanner.stop()
    if found:
        for addr, (name, rssi) in found.items():
            print("✓ 找到 %s  name=%r rssi=%s  (等待 %.1fs)" % (
                addr, name, rssi, time.monotonic() - t0))
        return 0
    print("✗ %.0fs 内没扫到 %s" % (TIMEOUT, TARGET))
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
