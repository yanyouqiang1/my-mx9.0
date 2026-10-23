#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
抓 panic 现场 —— 不用再烧固件。

原理：
    黑匣子固件在 panic 的那一刻把回溯抄进 RTC 慢存；**下一次开机**时
    把这段文字打进日志（UART0 + USB CDC）。
    所以只要 CDC 一直开着，熬过"崩溃 → 自动重启 → 开机打印"这一轮，
    现场就到手了。

    之前抓不到，是因为上传脚本在检测到断链后 0.5 秒就退出、把 CDC 关了，
    而重启 + 开机打印要 2~5 秒 —— 窗口错过了。
    这个脚本在断链之后**继续守着 CDC 20 秒**。

用法：
    /usr/bin/python3 tools/capture_panic.py [--addr=XXXX] [--cdc-port=/dev/cu.usbmodemXXX]
"""

import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blekit import BleakClient, CdcLog, Link, find_keyboard, CHAR_UUID  # noqa: E402

A_BOOT_ADDR = "7810793C"          # A 板（要看新壁纸的那块）
DEFAULT_CDC = "/dev/cu.usbmodemACA704E101E02"
WAIT_AFTER_CRASH = 25.0           # 崩溃后守着 CDC 的秒数


async def main():
    argv = sys.argv[1:]
    addr_filter = None
    cdc_port = DEFAULT_CDC
    for a in argv:
        if a.startswith("--addr="):
            addr_filter = a.split("=", 1)[1].strip().upper()
        if a.startswith("--cdc-port="):
            cdc_port = a.split("=", 1)[1]

    cdc = CdcLog()
    if not cdc.start(port=cdc_port):
        print("✗ 打开 CDC 失败: %s" % cdc_port)
        return 1
    print("=== CDC: %s ===" % cdc.port)

    addrs = await find_keyboard(timeout=12)
    if addr_filter:
        addrs = [a for a in addrs if a.upper().startswith(addr_filter)]
    if not addrs:
        print("✗ 没扫到目标键盘")
        cdc.stop()
        return 1
    addr = addrs[0]
    print("=== 键盘: %s ===" % addr)

    crashed = False
    try:
        async with BleakClient(addr, timeout=20) as c:
            link = Link(c)
            await link.start()
            await asyncio.sleep(0.4)

            await link.cmd("LOGO_RAW_START:115200", wait=0.4)
            await asyncio.sleep(0.3)

            chunk = min(180, max(20, c.mtu_size - 3))
            # 用全 0 的数据就行 —— 这一轮的目的不是传图，是**把固件弄崩**。
            payload = bytes(chunk)
            sent = 0
            t0 = time.time()
            while sent < 115200:
                try:
                    await c.write_gatt_char(CHAR_UUID, payload, response=False)
                except Exception as e:
                    print("\n⚠ 第 %d 片断开：%s" % (sent // chunk, type(e).__name__))
                    crashed = True
                    break
                sent += chunk
                await asyncio.sleep(0.012)
                if (sent // chunk) % 100 == 0:
                    print("    %d/%d 字节" % (sent, 115200), end="\r", flush=True)
            if not crashed:
                print("\n    传完 %d 字节，等固件收尾…" % sent)
    except Exception as e:
        print("⚠ BLE 异常：%s: %s" % (type(e).__name__, e))
        crashed = True

    if crashed:
        print("\n=== 崩溃了，守着 CDC 等它重启（%.0f 秒）===" % WAIT_AFTER_CRASH)
        t0 = time.time()
        while time.time() - t0 < WAIT_AFTER_CRASH:
            await asyncio.sleep(1.0)
            tail = cdc.tail(6)
            for l in tail:
                if "PANIC" in l or "Guru" in l or "Backtrace" in l or "abort" in l:
                    print("  ★ %s" % l)

    print("\n=== CDC 抓到的全部内容 ===")
    cdc.dump()
    cdc.stop()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
