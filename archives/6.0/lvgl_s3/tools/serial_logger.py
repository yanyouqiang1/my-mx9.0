#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
常驻串口记录器：把 USB CDC 的输出**持续**写进一个文件，直到被 kill。

为什么要常驻 + 断线重连：
    崩溃现场只出现一次 —— 开机 banner 里的 reset_reason 就是唯一的证据。
    而且芯片 panic 之后会重启、USB 重新枚举，**原来那个串口句柄就废了**
    （pyserial 不报错，只是再也读不到东西）。
    所以这里每隔一小段时间就"探一下活"，一旦句柄失效就重新打开。

用法：
    BLE_SECONDS=900 nohup /usr/bin/python3 tools/serial_logger.py /tmp/ble.log > /dev/null 2>&1 &
    tail -f /tmp/ble.log
"""

import os
import serial
import sys
import time

PORT = os.environ.get("BLE_PORT", "/dev/cu.usbmodem11101")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ble.log"
BOOT = os.environ.get("BLE_BOOT", "1") == "1"
SECONDS = float(os.environ.get("BLE_SECONDS", "3600"))


def open_keeping_boot_high(port, baud=115200):
    """打开串口，但**打开那一刻** DTR 必须是低（= BOOT 高 = 跑应用）。"""
    s = serial.serial_for_url(port, do_not_open=True)
    s.baudrate = baud
    s.timeout = 0.2
    s.write_timeout = 1.0
    s._dtr_state = False
    s._rts_state = False
    s.open()
    return s


def main():
    f = open(OUT, "ab", buffering=0)
    t0 = time.time()
    s = None
    first_open = True
    last_alive = time.time()
    while time.time() - t0 < SECONDS:
        if s is None:
            try:
                s = open_keeping_boot_high(PORT)
            except Exception:
                time.sleep(1.0)
                continue
            # ⚠ 只有**第一次**打开才复位。后面重开是为了接住崩溃后的重启现场，
            #   那时候再复位一次就把唯一的证据冲掉了。
            if first_open and BOOT:
                try:
                    s.rts = True
                    time.sleep(0.12)
                    s.rts = False
                except Exception:
                    pass
            first_open = False
            last_alive = time.time()
        try:
            c = s.read(4096)
            if c:
                f.write(c)
                last_alive = time.time()
        except Exception:
            c = b""
        # 句柄失效的判定：连续 3 秒一个字节都没有 —— 固件每秒都有心跳，
        # 3 秒没动静基本就是崩了（芯片重启会让 USB 重新枚举，原句柄作废）。
        if time.time() - last_alive > 3.0:
            try:
                s.close()
            except Exception:
                pass
            s = None
            last_alive = time.time()
            time.sleep(0.5)
    try:
        if s:
            s.close()
    except Exception:
        pass
    f.close()


if __name__ == "__main__":
    main()
