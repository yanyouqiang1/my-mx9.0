#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""让芯片从 ROM 下载模式**直接跳进 flash 里的应用**（不用拔插、不用按按钮）。

背景（2026-10-22 实测）：
  ESP32-S3 的 **USB 会话在复位那一瞬间会把 IO0 压低**，所以
  「烧完 → 按 EN / 复位」都会落回 ROM 下载模式，芯片死活起不来。
  esptool 的 `run` 命令内部是 flash_begin(0,0) + flash_end(reboot=False)，
  而 flash_end 的参数在 loader.py 里是 `struct.pack("<I", int(not reboot))` ——
  也就是 reboot=False 时发出去的是 **1**。实测发 1 的效果是"芯片复位"
  （于是又被 USB 会话按回下载模式），所以真正的"跳进应用"可能是**发 0**。

这个脚本用 esptool 的 API 直接发 flash_end(pkt=0)，然后看 USB 枚举有没有
从 "USB JTAG/serial debug unit" 变成应用自己的名字。

用法:
    /usr/bin/python3 tools/jump_to_app.py [端口]
"""
import os
import sys
import time

ESPTOOL_DIR = "/Users/yanyouqiang/.platformio/packages/tool-esptoolpy"
# esptool 的依赖（intelhex 等）放在 _contrib 里，官方脚本会把它加进 sys.path
sys.path.insert(0, ESPTOOL_DIR)
sys.path.insert(0, os.path.join(ESPTOOL_DIR, "_contrib"))

import esptool  # noqa: E402
from esptool.loader import StubFlasher  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"


def usb_product():
    import re
    import subprocess
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-c", "IOUSBHostDevice"],
                         capture_output=True, text=True, timeout=15).stdout
    names = re.findall(r"[-+]o ([^@<]+)@", out)
    return names


def main():
    print("端口: %s" % PORT)
    StubFlasher.set_preferred_stub_subdir(StubFlasher.STUB_SUBDIRS[0])
    esp = esptool.connect_loop(PORT, 115200, "esp32s3", 2, False, "no_reset")
    print("✓ 连上 ROM，正在上传 stub…")
    esp = esp.run_stub()
    print("✓ stub 在跑")

    # flash_end 的参数：loader.py 里是 struct.pack("<I", int(not reboot))
    #   reboot=False → 发 1（实测 = 复位，又被 USB 会话按回下载模式）
    #   reboot=True  → 发 0（这次试这个：期望是"跳进应用"）
    print("→ 发 flash_end(pkt=0)…")
    try:
        esp.flash_begin(0, 0)
        esp.flash_finish(True)      # → pkt = 0
        print("✓ 命令已发")
    except Exception as e:
        print("! 失败: %r" % e)

    for i in range(12):
        time.sleep(1)
        names = usb_product()
        print("  t+%ds: %s" % (i + 1, names))
        if any("YYQ" in n for n in names):
            print("✓ 应用起来了！")
            return 0
    print("✗ 没跳进应用（还在下载模式）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
