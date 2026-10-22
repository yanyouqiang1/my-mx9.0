#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""列出扫描到的所有 BLE 设备（名字/地址/服务 UUID），用于确认目标板在不在。"""
import asyncio
import time

from bleak import BleakScanner

DURATION = 12.0
seen = {}


def on_device(dev, adv):
    name = adv.local_name or dev.name or ""
    key = dev.address
    if key not in seen:
        seen[key] = {
            "name": name,
            "rssi": adv.rssi,
            "suuids": [str(u) for u in (adv.service_uuids or [])],
        }
        print("[DEV] %-20s name=%-24r rssi=%s suuids=%s" % (
            key, name, adv.rssi, seen[key]["suuids"]), flush=True)


async def main():
    t0 = time.monotonic()
    scanner = BleakScanner(detection_callback=on_device)
    await scanner.start()
    try:
        while time.monotonic() - t0 < DURATION:
            await asyncio.sleep(0.5)
    finally:
        await scanner.stop()
    print("\n===== 共 %d 个设备 =====" % len(seen))


asyncio.run(main())
