#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""反复复位，直到芯片进入 ROM 下载模式（用户按住 BOOT 即可）。

为什么需要它：
  这块板子的 COM 口只有 TX/RX + RTS→EN，**DTR 没接到 IO0**（实测：
  DTR 开/关两种情况复位后都是 boot:0x8，esptool 报 "Wrong boot mode
  detected (0x8)"）。所以没法靠软件把 IO0 拉低进下载模式。
  但 EN 我们能控 —— 于是：
      用户按住 BOOT（IO0 被拉低）+ 脚本不停复位
      → 其中某一次复位采样到的就是 IO0=0 → ROM 进下载模式。

判定：ROM 的下载模式会在 UART0 上打印
    rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))
    waiting for download
看到 "waiting for download" 或 "DOWNLOAD" 就算进成了。

用法:
    /usr/bin/python3 tools/com_enter_download.py [端口] [超时秒数]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5C4C2142561"
TIMEOUT = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0


def main():
    s = serial.serial_for_url(PORT, do_not_open=True)
    s._dtr_state = False     # IO0 不去碰它（这个口没接）
    s._rts_state = False     # EN 放开
    s.baudrate = 115200
    s.timeout = 0.15
    s.open()
    t0 = time.time()
    try:
        attempt = 0
        while time.time() - t0 < TIMEOUT:
            attempt += 1
            # 复位：RTS 拉低(EN) 再放开
            s.rts = True
            time.sleep(0.12)
            s.rts = False
            # 复位后等 ROM 打印启动横幅
            time.sleep(0.35)
            buf = bytearray()
            t1 = time.time()
            while time.time() - t1 < 1.2:
                c = s.read(4096)
                if c:
                    buf.extend(c)
                    txt = bytes(buf).decode("utf-8", "replace")
                    if "waiting for download" in txt or "DOWNLOAD" in txt:
                        print("✓ 进下载模式了（第 %d 次复位，耗时 %.1fs）" % (
                            attempt, time.time() - t0))
                        for line in txt.splitlines()[:8]:
                            print("   | " + line)
                        return 0
                else:
                    time.sleep(0.05)
            txt = bytes(buf).decode("utf-8", "replace")
            m = re.search(r"boot:(0x[0-9a-f]+) \(([^)]+)\)", txt)
            print("  第 %d 次复位：%s" % (
                attempt, ("%s (%s)" % (m.group(1), m.group(2))) if m else "没抓到横幅"))
        print("✗ %.0fs 内没进下载模式 —— BOOT 是不是没按住？" % TIMEOUT)
        return 1
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
