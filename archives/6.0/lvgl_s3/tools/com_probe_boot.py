#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""摸清 COM 口的 DTR/RTS 到 IO0/EN 的实际接线。

现象：esptool 的 ClassicReset 复位后芯片进的是 SPI boot(0x8) 而不是
下载模式(0x0) → 说明 DTR 没把 IO0 拉低。
这里穷举 DTR/RTS 的各种组合，每种复位一次，看芯片报哪种 boot 模式：

    boot:0x8 (SPI_FAST_FLASH_BOOT)  → 正常从 flash 启动（IO0 高）
    boot:0x0 (DOWNLOAD)              → 进下载模式（IO0 低）

用法:
    /usr/bin/python3 tools/com_probe_boot.py [端口]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5C4C2142561"


def trial(name, dtr_before, dtr_at_release, rts_pulse=True):
    s = serial.serial_for_url(PORT, do_not_open=True)
    s._dtr_state = dtr_before
    s._rts_state = False
    s.baudrate = 115200
    s.timeout = 0.2
    s.open()
    txt = ""
    try:
        if rts_pulse:
            s.rts = True            # EN 拉低 → 复位
            time.sleep(0.15)
            s.dtr = dtr_at_release   # 决定复位释放瞬间 IO0 的状态
            time.sleep(0.05)
            s.rts = False           # EN 放开 → 芯片重新启动
        t0 = time.time()
        while time.time() - t0 < 2.5:
            c = s.read(4096)
            if c:
                txt += c.decode("utf-8", "replace")
    finally:
        s.close()
    m = re.search(r"rst:[^,]+,boot:(0x[0-9a-f]+) \(([^)]+)\)", txt)
    mode = ("%s (%s)" % (m.group(1), m.group(2))) if m else "(没抓到启动横幅)"
    print("  %-38s → %s" % (name, mode))
    return txt


if __name__ == "__main__":
    print("=== COM 口 DTR/RTS 接线探测 ===")
    trial("DTR关 + RTS脉冲", False, False)
    trial("DTR开 + RTS脉冲（松开时 DTR 仍为开）", True, True)
    trial("DTR开 + RTS脉冲（松开瞬间 DTR 关）", True, False)
    trial("DTR关 + RTS脉冲（松开瞬间 DTR 开）", False, True)
