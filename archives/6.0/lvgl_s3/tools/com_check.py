#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""COM 口（USB-TTL）体检：列端口 + 不复位地读 UART0 日志。

为什么读日志要"不碰 DTR/RTS"：
  ESP32 的 USB-TTL 上 DTR/RTS 是**自动复位电路**（DTR→IO0、RTS→EN）。
  esptool 自己的注释写得很清楚：
      _setDTR(True)   # IO0=LOW   ← 进下载模式
      _setDTR(False)  # IO0=HIGH  ← 正常从 flash 启动
  而 pyserial 打开串口时**默认 DTR=True**（=IO0 被拉低），
  所以"随手 open 一下端口"就会把芯片踢进 ROM 下载模式 ——
  这正是之前"板子莫名其妙停在 DOWNLOAD、拔插才好"的真正原因之一。
  所以这里统一用 do_not_open=True + 先把 _dtr_state/_rts_state 设成 False。

用法:
    /usr/bin/python3 tools/com_check.py                 # 只列端口 + USB 设备
    /usr/bin/python3 tools/com_check.py /dev/cu.xxx 10  # 读 10 秒 UART0
"""
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


def usb_tree():
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-c", "IOUSBHostDevice"],
                             capture_output=True, text=True, timeout=15).stdout
    except Exception:
        return ""
    names = re.findall(r"\+-o ([^@<]+)@", out)
    return " | ".join(names) if names else "(没有 USB 设备)"


def show_ports():
    print("=== 串口节点 ===")
    ports = list(list_ports.comports())
    if not ports:
        print("  (一个都没有)")
    for p in ports:
        print("  %-28s %-40s %s" % (p.device, p.description, p.hwid))
    print("=== USB 总线上的设备 ===")
    print("  " + usb_tree())


def read_uart(port, seconds):
    print("\n=== 读 %s 共 %.0f 秒（不碰 DTR/RTS）===" % (port, seconds))
    s = serial.serial_for_url(port, do_not_open=True)
    s._dtr_state = False
    s._rts_state = False
    s.baudrate = 115200
    s.timeout = 0.2
    s.open()
    try:
        t0 = time.time()
        got = 0
        while time.time() - t0 < seconds:
            try:
                data = s.read(4096)
            except Exception as e:
                print("  读出错: %s" % e)
                break
            if data:
                got += len(data)
                txt = data.decode("utf-8", "replace")
                for line in txt.splitlines():
                    print("  | " + line)
        if got == 0:
            print("  (一个字节都没有 —— 说明芯片没在跑应用，或者 TX 没接到 COM 口)")
    finally:
        s.close()


if __name__ == "__main__":
    show_ports()
    if len(sys.argv) > 1:
        read_uart(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 8.0)
