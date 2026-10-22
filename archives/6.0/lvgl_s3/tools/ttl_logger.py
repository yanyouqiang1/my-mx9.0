#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
USB-TTL 串口记录器（给第二块板子用）。

为什么单独写一个，不用 serial_logger.py：
  serial_logger.py 是给板子**自带的 USB-Serial-JTAG** 用的，它靠
  "DTR 电平 = BOOT 脚" 这个特性工作，所以会主动去动 DTR/RTS。
  而 USB-TTL 模块（CH340/CP2102/FTDI）如果把 DTR/RTS 接到了板子的
  EN/IO0 上，**动一下 DTR/RTS 就等于动了复位/下载模式** ——
  记录日志的过程中把板子踹进下载模式，记录就断了。
  所以这个版本**从头到尾不碰 DTR/RTS**，只读。

用法：
    /usr/bin/python3 tools/ttl_logger.py /dev/cu.usbserial-XXXX /tmp/boardB.log
"""

import os
import sys
import time

import serial


def open_port(path, baud=115200):
    # 关键：先预设 DTR/RTS 为 False，再 open()。
    # pyserial 默认在 open() 时把 DTR 拉高 —— 对 CH340 来说，
    # 那可能直接给板子的 EN/IO0 一个电平，把板子踹进下载模式。
    s = serial.serial_for_url(path, do_not_open=True)
    s.baudrate = baud
    s.timeout = 0.2
    s.write_timeout = 1.0
    s._dtr_state = False
    s._rts_state = False
    s.open()
    # open 之后再显式压一次，防止驱动在 open 后又改回去
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    return s


def main():
    if len(sys.argv) < 2:
        print("用法: ttl_logger.py <串口路径> [输出文件]")
        return 2
    path = sys.argv[1]
    out = open(sys.argv[2], "ab", buffering=0) if len(sys.argv) > 2 else None

    print("[logger] 打开 %s …" % path, flush=True)
    ser = None
    while True:
        if ser is None:
            try:
                ser = open_port(path)
                print("[logger] 已连上", flush=True)
            except Exception as e:
                print("[logger] 打开失败(%s)，2 秒后重试" % e, flush=True)
                time.sleep(2)
                continue
        try:
            data = ser.read(4096)
        except Exception as e:
            print("[logger] 读出错: %r，重开" % e, flush=True)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(1)
            continue
        if data:
            try:
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            except Exception:
                pass
            if out:
                out.write(data)
        else:
            # 偶尔探一下设备还在不在（拔了/复位了会变）
            time.sleep(0.01)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
