#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""连一次 → 断开 → 再扫：专门用来复现"连一次断开后就再也扫不到"。

配合 UART0 日志一起看，就能知道断链之后 esp_ble_gap_start_advertising
到底返回了什么错误码（Arduino 那层失败是静默的，日志里只有一条 log_e）。

用法:
    /usr/bin/python3 tools/ble_cycle.py [轮数] [每轮断开后等待秒数] [目标地址前缀]
目标地址前缀用于在多块同名板子里挑一块（macOS 上 bleak 给的是 UUID）。
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

NAME = "YYQ-MX9.0"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
ONLY = sys.argv[3].upper() if len(sys.argv) > 3 else ""


async def scan_for(seconds, want_name=NAME):
    """扫 BLE，返回 (是否找到, 列表)。"""
    found = []
    stop = time.monotonic() + seconds

    def cb(dev, adv):
        n = adv.local_name or dev.name or ""
        if want_name.lower() not in n.lower():
            return
        if ONLY and not dev.address.upper().startswith(ONLY):
            return
        found.append((dev.address, n, adv.rssi))

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        while time.monotonic() < stop and not found:
            await asyncio.sleep(0.3)
    finally:
        await scanner.stop()
    return (len(found) > 0), found


async def main():
    print("=== 第 0 步：先扫一次 ===")
    ok, lst = await scan_for(10)
    if not ok:
        print("✗ 一开始就扫不到设备 —— 先确认板子起来了")
        return 1
    print("✓ 扫到：%s" % lst)

    for r in range(1, ROUNDS + 1):
        ok, lst = await scan_for(10)
        if not ok:
            print("\n[第 %d 轮] ✗ 扫不到设备了（还没连就已经扫不到）" % r)
            return 2
        addr = lst[0][0]
        print("\n[第 %d 轮] 连上 %s" % (r, addr))
        try:
            async with BleakClient(addr, timeout=12) as client:
                # 确认链路真的通：读一下特征
                try:
                    await client.start_notify(SERVICE_UUID, lambda s, d: None)
                    print("  ✓ 订阅成功（notify 通道 OK）")
                except Exception as e:
                    print("  ! 订阅失败: %s" % e)
                await asyncio.sleep(1.5)
            print("  → 已断开")
        except Exception as e:
            print("  ! 连接异常: %s" % e)

        t0 = time.monotonic()
        ok, lst = await scan_for(WAIT)
        if ok:
            print("  ✓ 断开后 %.1fs 又扫到了：%s" % (time.monotonic() - t0, lst))
        else:
            print("  ✗ 断开后 %.0fs 内**再也扫不到**了 —— 复现成功" % WAIT)
            return 3
    print("\n=== %d 轮全部通过：断开后都能重新扫到 ===" % ROUNDS)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
