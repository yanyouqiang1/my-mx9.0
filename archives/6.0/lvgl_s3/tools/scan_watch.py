#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""长时间监听 BLE 广播，记录目标设备什么时候出现、持续多久、广播内容是什么。

用来回答两个问题：
  1) 设备到底还在不在广播？（还是只是 Mac 缓存了旧结果）
  2) 如果在广播，广播数据（名字 / service UUID / RSSI）长什么样？

用法:
    /usr/bin/python3 tools/scan_watch.py [秒数] [目标名片段]
默认 25 秒，目标 "YYQ-MX9.0"。
"""
import asyncio
import sys
import time

from bleak import BleakScanner

TARGET = sys.argv[2] if len(sys.argv) > 2 else "YYQ-MX9.0"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0

seen = {}          # addr -> 首次出现时间
last_seen = {}     # addr -> 最后出现时间
hits = 0
samples = []


def on_device(dev, adv):
    global hits
    name = adv.local_name or dev.name or ""
    if TARGET.lower() not in (name or "").lower():
        return
    hits += 1
    now = time.monotonic()
    key = dev.address
    if key not in seen:
        seen[key] = now
        samples.append({
            "addr": key,
            "name": name,
            "rssi": adv.rssi,
            "suuids": [str(u) for u in (adv.service_uuids or [])],
            "mfg": adv.manufacturer_data,
            "svc_data": {str(k): v.hex() for k, v in (adv.service_data or {}).items()},
            "tx": adv.tx_power,
        })
        print("[HIT] %s  name=%r rssi=%s tx=%s suuids=%s" % (
            key, name, adv.rssi, adv.tx_power,
            [str(u) for u in (adv.service_uuids or [])]), flush=True)
    last_seen[key] = now


async def main():
    t0 = time.monotonic()
    scanner = BleakScanner(detection_callback=on_device)
    await scanner.start()
    try:
        while time.monotonic() - t0 < DURATION:
            await asyncio.sleep(0.5)
    finally:
        await scanner.stop()

    print("\n===== 汇总 (%.1fs 内) =====" % DURATION)
    print("命中条数: %d" % hits)
    if not seen:
        print("结果: **一次都没看到目标设备**")
        return
    for key, t_first in seen.items():
        gap = last_seen[key] - t_first
        print("设备 %s  首现 t=%.1fs  末现 t=%.1fs  跨度=%.1fs" % (
            key, t_first - t0, last_seen[key] - t0, gap))
    for s in samples:
        print("样本: %s" % s)


if __name__ == "__main__":
    asyncio.run(main())
