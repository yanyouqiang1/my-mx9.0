#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BLE 实机验证：新固件跑起来没有？新界面渲染对不对？

用固件自己的通道问它，不依赖串口 —— native USB 那条路已经被证明
"复位时 USB 会话会压低 IO0"，串口日志拿不到应用层的真实输出，
而 BLE 广播/notify 是应用层行为，能跑就说明应用活着。

用法：
    /usr/bin/python3 tools/ble_verify.py            # 全套
    /usr/bin/python3 tools/ble_verify.py --dump     # 顺便 LOG:dump
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "yyq2 号"


async def find_keyboard(timeout=10.0):
    """用 detection_callback 扫（macOS 的 discover() 会漏设备）。

    ⚠ 认人必须按 **GATT 里的 service UUID**，不能按名字、也不能取"第一个扫到的"。
    这台机器上还有一台同样叫 "yyq2 号" 的 HID 设备（macOS 里配对过的另一台键盘），
    广播顺序是随机的 —— 早先这个脚本取 cands[0] 就经常连到它身上去，
    表现是"连上了但什么服务都没有"或者"命令发出去设备没反应"。
    """
    found = []

    def cb(device, ad):
        name = (ad.local_name or device.name or "")
        if DEV_NAME in name or "YYQ" in name.upper():
            found.append((device.address, name))

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()
    return found


def is_our_keyboard(c):
    """GATT 里必须有本机的 service + 特征，才是这块板子。"""
    uuids = [str(s.uuid) for s in c.services]
    has_svc = any(SERVICE_UUID.lower() in u.lower() for u in uuids)
    has_ch = any(CHAR_UUID.lower() in str(ch.uuid).lower()
                 for s in c.services for ch in s.characteristics)
    return has_svc and has_ch


async def send_recv(client, text, wait=1.5):
    """发一条命令，收 notify 回包。固件的回包是明文行。"""
    got = []

    def handler(sender, data):
        try:
            got.append(data.decode("utf-8", "replace"))
        except Exception:
            got.append(repr(data))

    await client.start_notify(CHAR_UUID, handler)
    await asyncio.sleep(0.25)
    await client.write_gatt_char(CHAR_UUID, (text + "\n").encode(), response=True)
    await asyncio.sleep(wait)
    await client.stop_notify(CHAR_UUID)
    return got


async def main():
    want_dump = "--dump" in sys.argv
    print("=== 扫描键盘 ===")
    cands = await find_keyboard()
    if not cands:
        print("✗ 没扫到 %r —— 固件可能没在跑" % DEV_NAME)
        return 1
    for addr, name in cands:
        print("  找到 %s  %s" % (addr, name))

    addr = None
    for cand_addr, cand_name in cands:
        try:
            async with BleakClient(cand_addr, timeout=8) as probe:
                if is_our_keyboard(probe):
                    addr = cand_addr
                    print("  认人成功: %s  %s" % (cand_addr, cand_name))
                    break
        except Exception:
            continue
    if addr is None:
        print("✗ 扫到的设备里没有带本机 service/特征的 —— 固件可能没在跑")
        return 1

    print("\n=== 连上 %s ===" % addr)
    async with BleakClient(addr, timeout=10) as c:
        print("  --- 全部 GATT 服务/特征 ---")
        for s in c.services:
            print("    %s" % s.uuid)
            for ch in s.characteristics:
                print("        char %s" % ch.uuid)
        uuids = [str(s.uuid) for s in c.services]
        has_svc = any(SERVICE_UUID.lower() in u.lower() for u in uuids)
        has_ch = any(CHAR_UUID.lower() in str(ch.uuid).lower()
                     for s in c.services for ch in s.characteristics)
        print("  本机 service: %s | 本机特征: %s"
              % ("✓" if has_svc else "✗", "✓" if has_ch else "✗"))
        if not (has_svc and has_ch):
            print("  ⚠ 这台设备不是本固件的键盘（可能是同名设备或 macOS 缓存）")
            return 1

        # 1) 应用是否活着：问一个只读的状态
        print("\n=== LOG:dump（应用层日志）===")
        for line in await send_recv(c, "LOG:dump", wait=2.0):
            print("  " + line.replace("\n", " ")[:160])

        # 2) 切到壁纸主屏（风格 6 = DISP_MODE 5）
        print("\n=== 切到壁纸主屏 DISP_MODE:5 ===")
        for line in await send_recv(c, "DISP_MODE:5", wait=1.5):
            print("  " + line.replace("\n", " ")[:160])

        if want_dump:
            print("\n=== 切屏后 LOG:dump ===")
            for line in await send_recv(c, "LOG:dump", wait=2.0):
                print("  " + line.replace("\n", " ")[:160])
    print("\n✅ BLE 通道验证完成")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
