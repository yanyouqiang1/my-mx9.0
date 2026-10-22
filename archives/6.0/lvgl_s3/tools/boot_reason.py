#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""连上设备，用 BLE 的 LOG:dump 把开机诊断日志拉回来 —— 专门看**上次是怎么重启的**。

固件每次开机都会往日志环缓冲里写：
    [BOOT] reason=N (中文名) psram=... heap=... minHeap=... boots=N
    [BOOT] prev: hang=0 stage=0(循环末尾) key=none uptimeMs=0
reason 的取值（esp_system.h）：
    0 未知 / 1 上电 / 2 外部复位 / 3 软件复位 / **4 异常崩溃(panic)** /
    5 中断看门狗 / 6 任务看门狗 / 7 其它看门狗 / 8 深度睡眠 / 9 电压掉电

用法:
    /usr/bin/python3 tools/boot_reason.py [设备地址前缀]
"""
import asyncio
import re
import sys

from bleak import BleakClient, BleakScanner

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from verify_fix import Bt, write, SERVICE_UUID, CHAR_UUID  # noqa: E402

ONLY = sys.argv[1].upper() if len(sys.argv) > 1 else ""


async def find(timeout=10.0):
    found = []

    def cb(dev, adv):
        n = (adv.local_name or dev.name or "")
        if "YYQ-MX9.0" in n and (not ONLY or dev.address.upper().startswith(ONLY)):
            found.append(dev.address)

    s = BleakScanner(detection_callback=cb)
    await s.start()
    try:
        t0 = asyncio.get_event_loop().time()
        while asyncio.get_event_loop().time() - t0 < timeout and not found:
            await asyncio.sleep(0.4)
    finally:
        await s.stop()
    return found[0] if found else None


async def main():
    addr = await find()
    if not addr:
        print("✗ 没找到设备")
        return 1
    print("✓ 设备 %s" % addr)

    bt = Bt()
    async with BleakClient(addr, timeout=20) as c:
        await c.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
        await asyncio.sleep(0.5)
        await write(c, "LOG:dump")
        # 回拉 8KB，按 20 字节一片慢慢发，等 END
        for _ in range(120):
            await asyncio.sleep(0.25)
            if bt.done:
                break
        await asyncio.sleep(1.0)

    dumps = bt.take("LOGDUMP")
    if not dumps:
        print("✗ 没收到 LOGDUMP 回包（可能链路没连上）")
        return 2
    text = dumps[0][2]
    lines = text.splitlines()
    print("=== 开机日志 %d 字节 / %d 行 ===" % (len(text), len(lines)))
    keys = ("[BOOT]", "prev:", "reason=", "panic", "abort", "assert",
            "heap", "Heap", "oom", "OOM", "watchdog", "WDT", "brownout",
            "重启", "崩溃", "异常")
    for line in lines:
        if any(k in line for k in keys):
            print("  " + line)
    print("\n=== 最后 25 行 ===")
    for line in lines[-25:]:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
