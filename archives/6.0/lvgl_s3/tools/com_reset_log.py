#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通过 COM 口（USB-TTL）复位板子并抓完整启动日志。

为什么能做到复位：USB-TTL 上的 RTS 接的是芯片的 EN（低有效），
DTR 接的是 IO0。esptool 的注释写得很清楚：
    _setDTR(True)   # IO0=LOW   ← 进下载模式
    _setDTR(False)  # IO0=HIGH  ← 正常从 flash 启动
所以「DTR 放开 + RTS 脉冲一下」= 一次干净的上电复位（IO0 是高的），
应用会正常从 flash 起来。

⚠ 打开端口时**必须**把 _dtr_state/_rts_state 都设成 False 再 open，
   否则 pyserial 默认 DTR=True → IO0 被拉低 → 一复位就进 ROM 下载模式。

用法:
    /usr/bin/python3 tools/com_reset_log.py [秒数] [端口]
"""
import sys
import time

import serial

PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbmodem5C4C2142561"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0


def main():
    s = serial.serial_for_url(PORT, do_not_open=True)
    s._dtr_state = False     # IO0 = HIGH（不进下载模式）
    s._rts_state = False     # EN  = HIGH
    s.baudrate = 115200
    s.timeout = 0.2
    s.open()
    try:
        # 复位：RTS 拉低(EN) 再放开 → 芯片重启
        s.rts = True
        time.sleep(0.15)
        s.rts = False
        t0 = time.time()
        while time.time() - t0 < SECONDS:
            try:
                data = s.read(4096)
            except Exception as e:
                print("[读出错] %s" % e)
                break
            if data:
                for line in bytes(data).decode("utf-8", "replace").splitlines():
                    print(line)
    finally:
        s.close()


if __name__ == "__main__":
    main()
