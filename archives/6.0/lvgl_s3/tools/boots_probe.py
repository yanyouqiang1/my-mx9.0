#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""量一下「某个动作会不会把设备弄崩」。

判据用固件自己的 boots 计数（开机日志里的 [BOOT] ... boots=N）：
    做动作 → 读一次 boots → 再做动作 → 再读一次 → boots 涨了就是崩了。

比"看 BLE 还在不在"精确得多：崩了之后固件会自动重启，BLE 又回来了，
从外面看只是"卡了一下"，不数 boots 就发现不了。
"""

import asyncio
import re
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from blekit import BleakClient, find_keyboard, Link, CHAR_UUID  # noqa: E402

BOOTS_RE = re.compile(r"boots=(\d+)")


async def boots_of(link):
    notes = await link.cmd("LOG:dump", wait=1.2)
    best = None
    for n in notes:
        m = BOOTS_RE.search(n)
        if m:
            best = int(m.group(1))
    return best


async def main():
    actions = [a for a in sys.argv[1:] if not a.startswith("--")]
    addrs = await find_keyboard(timeout=10)
    if not addrs:
        print("✗ 没扫到键盘")
        return 1
    addr = addrs[0]

    async with BleakClient(addr, timeout=15) as c:
        link = Link(c)
        await link.start()
        await asyncio.sleep(0.4)

        b0 = await boots_of(link)
        print("起始 boots = %s" % b0)

        for act in actions:
            print("\n→ 执行: %s" % act)
            try:
                await c.write_gatt_char(CHAR_UUID, (act + "\n").encode(),
                                        response=True)
            except Exception as e:
                print("   写失败:", e)
            await asyncio.sleep(2.0)
            b1 = await boots_of(link)
            delta = (b1 - b0) if (b0 is not None and b1 is not None) else None
            if delta is None:
                print("   boots 读不到（可能已经掉线了）")
            elif delta > 0:
                print("   ❌ 崩了！boots %s → %s（+%d）" % (b0, b1, delta))
            else:
                print("   ✅ 没崩，boots = %s" % b1)
            b0 = b1
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
