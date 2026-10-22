#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
崩溃复现脚本：把出问题的那个序列跑一遍。

观测到的现象（第一轮实测）：
    CONNECT #1 → DISCONNECT #1 → 重新广播 → CONNECT #2 →
    PROBE:DUMP / PROBE:MTU / PROBE:BIG / PROBE:STREAM →
    DISCONNECT #2 → 探针日志到此为止 → 设备**自己重启了**

所以怀疑点集中在两处：
    ① 断线后重新广播（BLEDevice::startAdvertising 从 loopTask 调）
    ② PROBE:STREAM 那条 20KB 的流式回拉
这个脚本把两者分开跑，各自跑几轮，看哪个能把设备送走。
"""

import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

MODE = sys.argv[1] if len(sys.argv) > 1 else "all"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 3


async def find_keyboard(timeout=6.0):
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    for a in addrs:
        try:
            async with BleakClient(a, timeout=4) as c:
                for s in c.services:
                    if SERVICE_UUID.lower() in str(s.uuid).lower():
                        return a
        except Exception:
            pass
    return None


async def send(c, cmd, wait=0.6):
    await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
    await asyncio.sleep(wait)


async def one_round(addr, do_stream, do_big):
    c = BleakClient(addr, timeout=8)
    try:
        await c.connect()
    except Exception as e:
        print("   连不上: %r" % (e,))
        return False
    notes = []
    try:
        await c.start_notify(CHAR_UUID, lambda s, d: notes.append(bytes(d)))
        await asyncio.sleep(0.5)
        await send(c, "PROBE:MTU", 1.0)
        print("   收到 %d 条通知" % len(notes))
        if do_big:
            await send(c, "PROBE:BIG:250", 1.5)
            print("   累计 %d 条通知" % len(notes))
        if do_stream:
            await send(c, "PROBE:STREAM:20480", 12.0)
            print("   累计 %d 条通知" % len(notes))
    except Exception as e:
        print("   中途异常: %r" % (e,))
    try:
        await c.disconnect()
    except Exception:
        pass
    print("   已断开，等设备重新广播…")
    # 断线之后设备要是没重新广播，这一段就会一直连不上
    for i in range(12):
        await asyncio.sleep(1.0)
        cc = BleakClient(addr, timeout=3)
        try:
            await cc.connect()
            print("   %.1fs 后可连（广播恢复了）" % (i + 1))
            try:
                await cc.disconnect()
            except Exception:
                pass
            return True
        except Exception:
            pass
    print("   ★ 12 秒都没重新广播 —— 设备可能已经挂了")
    return False


async def main():
    addr = await find_keyboard()
    if not addr:
        print("✗ 没找到设备")
        return 1
    print("✓ 设备 %s   模式=%s 轮数=%d\n" % (addr, MODE, ROUNDS))

    for r in range(1, ROUNDS + 1):
        print("=== 第 %d 轮 ===" % r)
        if MODE == "all":
            ok = await one_round(addr, True, True)
        elif MODE == "stream":
            ok = await one_round(addr, True, False)
        elif MODE == "big":
            ok = await one_round(addr, False, True)
        elif MODE == "plain":
            ok = await one_round(addr, False, False)
        else:
            print("未知模式")
            return 2
        if not ok:
            print("   >>> 这一轮之后设备失联了 <<<\n")
            return 1
        print()
    print("全部轮次跑完，设备一直活着")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
