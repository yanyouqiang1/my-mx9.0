#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
探测一个问题：**USB 线插着的时候，一次普通复位会不会自己落进 ROM 下载模式？**

为什么问这个：
    要烧固件就得让芯片进下载模式（IO0=0）。现在固件跑的是 TinyUSB
    （ARDUINO_USB_MODE=0），USB-Serial-JTAG 外设被占用，esptool 的
    "复位握手"完全无效 —— 它连复位都发不出去。

    但如果"插着 USB 线复位"本身就会落进下载模式（某些 S3 板子上确实会，
    因为 USB 会话在复位瞬间会影响 IO0），那我们根本不需要人按 BOOT：
        BLE 发 REBOOT → 芯片复位 → USB 重新枚举成下载模式节点
        → esptool 连上去烧 → 烧完再 REBOOT 一次回来。
    这条路一旦成立，以后烧固件就是全自动的。

怎么测：
    1. 记录当前所有 /dev/cu.usbmodem* 节点
    2. BLE 发 REBOOT
    3. 连续盯 20 秒，看有没有**新节点**冒出来（下载模式下 S3 会重新枚举，
       节点名和 app 模式的不一样）
    4. 再看 BLE 还在不在广播 —— 芯片在下载模式里是不广播的

结论怎么读：
    · 出现新节点 + BLE 广播消失   → 复位自动进下载模式 ✅ 全自动烧录可行
    · 老节点还在 + BLE 还在广播     → 复位还是正常启动 ❌ 还得人按 BOOT
"""

import asyncio
import glob
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from blekit import BleakClient, find_keyboard, Link  # noqa: E402


def usb_nodes():
    out = []
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*"):
        out += glob.glob(pat)
    return sorted(set(out))


async def ble_alive(addr, timeout=4.0):
    """快速探一下设备还在不在广播。"""
    try:
        async with BleakClient(addr, timeout=timeout) as c:
            return c.is_connected
    except Exception:
        return False


async def main():
    before = usb_nodes()
    print("=== 复位前 USB 节点 ===")
    for n in before:
        print("   ", n)

    addrs = await find_keyboard(timeout=8.0)
    if not addrs:
        print("❌ 没找到键盘（BLE 广播里没有本机 service）")
        return 1
    addr = addrs[0]
    print("\n找到键盘:", addr)

    print("\n=== 发 REBOOT ===")
    try:
        async with BleakClient(addr, timeout=8) as c:
            link = Link(c)
            await link.start()
            await link.cmd("REBOOT", wait=0.4)
    except Exception as e:
        print("   发命令时断链（正常，芯片已经复位了）:", type(e).__name__, e)

    print("\n=== 盯 20 秒，看 USB 节点变化 ===")
    seen = set(before)
    new_nodes = []
    t0 = time.time()
    while time.time() - t0 < 20.0:
        cur = usb_nodes()
        for n in cur:
            if n not in seen:
                seen.add(n)
                new_nodes.append((round(time.time() - t0, 1), n))
                print("   [+%.1fs] 新节点: %s" % (time.time() - t0, n))
        time.sleep(0.4)

    print("\n=== 结果 ===")
    print("复位前:", before)
    print("复位后:", usb_nodes())
    if new_nodes:
        print("✅ 出现了新节点:", new_nodes)
        print("   → 复位落进了 ROM 下载模式（或至少 USB 重新枚举了）")
    else:
        print("❌ 没有新节点 —— 复位后还是 app 模式")

    await asyncio.sleep(1.0)
    alive = await ble_alive(addr)
    print("\nBLE 还连得上吗:", "是（还在广播 → 芯片在跑 app）" if alive else "否（不广播 → 芯片在下载模式/ROM 里）")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.get_event_loop().run_until_complete(main()))
    except KeyboardInterrupt:
        sys.exit(130)
