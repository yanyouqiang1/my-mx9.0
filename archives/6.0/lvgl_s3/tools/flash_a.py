#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""全自动烧 A 板：BLE 指令让设备自己进下载模式 → esptool 刷 app → 复位回应用。

为什么能全自动：
  固件里有 BOOTDL 指令 —— 收到后把 BOOT 脚(GPIO0)拉低再复位，
  ROM 采到 IO0=0 就进下载模式，native USB 重新枚举成串口节点，
  于是 esptool 又能连上了（不需要人按 BOOT/EN）。

⚠ 最后一步（回应用模式）需要一次物理复位：
  ESP32-S3 的 **USB 会话在复位那一瞬间会把 IO0 压低**（实测 GPIO_IN 的
  IO0 位=1、GPIO_ENABLE 的 IO0 位=0，可复位时 ROM 依然采到 boot:0x0），
  所以插着 USB 线复位/按 EN 都会落回下载模式。
  → 拔掉 USB 线再插回去（上电复位，IO0 是物理高电平）就正常启动应用。
  → 如果板子另有供电（电池）导致拔插不复位，就用 COM 口的 RTS 脉冲复位。

用法:
    /usr/bin/python3 tools/flash_a.py [固件路径] [设备地址前缀]
"""
import asyncio
import os
import re
import subprocess
import sys
import time

from bleak import BleakClient, BleakScanner

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_fix import write, CHAR_UUID  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/yanyouqiang/project/my-mx9.0/archives/6.0/lvgl_s3/.pio/build/esp32-s3-devkitc-1/firmware.bin"
ONLY = sys.argv[2].upper() if len(sys.argv) > 2 else ""
ESPTOOL = os.path.expanduser(
    "~/.platformio/packages/tool-esptoolpy/esptool.py")


async def find(timeout=15.0):
    found = []

    def cb(dev, adv):
        n = (adv.local_name or dev.name or "")
        if "YYQ-MX9.0" in n and (not ONLY or dev.address.upper().startswith(ONLY)):
            found.append(dev.address)

    s = BleakScanner(detection_callback=cb)
    await s.start()
    try:
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout and not found:
            await asyncio.sleep(0.4)
    finally:
        await s.stop()
    return found[0] if found else None


def usb_serial_node():
    import glob
    nodes = sorted(glob.glob("/dev/cu.usbmodem*"))
    return nodes[0] if nodes else None


async def main():
    print("固件: %s" % BIN)
    addr = await find()
    if not addr:
        print("✗ 没找到设备（扫不到 BLE 广播）")
        return 1
    print("✓ 设备 %s" % addr)

    # ---------- ① BLE 指令让它进下载模式 ----------
    try:
        async with BleakClient(addr, timeout=10) as c:
            await c.start_notify(CHAR_UUID, lambda s, d: None)
            await asyncio.sleep(0.4)
            await write(c, "BOOTDL", 0.5)
            print("  已发 BOOTDL，等它复位进下载模式…")
    except Exception as e:
        print("  ! 发 BOOTDL 出错: %s（可能已经不在广播了）" % e)

    # ---------- ② 等 native USB 重新枚举成串口 ----------
    port = None
    for _ in range(40):
        await asyncio.sleep(0.5)
        port = usb_serial_node()
        if port:
            break
    if not port:
        print("✗ 40 秒内 native USB 没有枚举成串口（设备可能没进下载模式）")
        return 2
    print("✓ 下载模式串口: %s" % port)

    # ---------- ③ 烧 app（只写 0x10000，不碰 bootloader 和分区表）----------
    cmd = ["/usr/bin/python3", ESPTOOL, "--port", port, "--chip", "esp32s3",
           "--before", "no_reset", "--after", "no_reset",
           "write_flash", "0x10000", BIN]
    print("  烧录中…")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    tail = "\n".join((p.stdout or "").splitlines()[-6:])
    print(tail)
    if p.returncode != 0:
        print("✗ 烧录失败 rc=%d\n%s" % (p.returncode, (p.stderr or "")[-600:]))
        return 3
    print("✓ 烧录完成")
    print(">>> 现在请把 **USB 线拔掉再插回去**（不要按 EN），芯片会从 flash 正常启动应用。")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
