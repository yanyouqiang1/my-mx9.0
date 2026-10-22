#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""找出这台 S3 上 DTR/RTS 的真实极性：试各种组合，看芯片是进应用还是进下载模式。"""
import serial, time, sys

PORT = "/dev/cu.usbmodem11101"

def trial(name, dtr, rts_pulse, settle=2.0):
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    out = bytearray()
    try:
        ser.dtr = dtr
        ser.rts = False
        time.sleep(0.15)
        if rts_pulse:
            ser.rts = True
            time.sleep(0.25)
            ser.rts = False
        t0 = time.time()
        while time.time() - t0 < settle:
            c = ser.read(4096)
            if c:
                out.extend(c)
    finally:
        ser.close()
    txt = out.decode("utf-8", "replace")
    mode = "DOWNLOAD" if "DOWNLOAD" in txt else ("APP" if txt.strip() else "静音")
    print("=== %-28s DTR=%-5s RTS脉冲=%-5s -> %s" % (name, dtr, rts_pulse, mode))
    for line in txt.splitlines()[:12]:
        print("      | " + line)
    return txt

if __name__ == "__main__":
    trial("DTR关 + RTS脉冲", False, True)
    trial("DTR开 + RTS脉冲", True, True)
    trial("DTR开 + 不脉冲", True, False)
    trial("DTR关 + 不脉冲", False, False)
