#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口小工具：抓 ESP32-S3 的 USB CDC 输出，顺便能把命令发进去。

⚠⚠ 这块板子有个很坑的地方，踩过才知道：
    ESP32-S3 的 USB-Serial-JTAG，**DTR 决定 BOOT（GPIO0）**。
      · DTR = 高 → GPIO0 被拉低 → 芯片进 ROM 下载模式（应用跑不起来）
      · DTR = 低 → GPIO0 放开 → 芯片跑应用
    而 pyserial 的 `Serial(...)` 默认在 open() 时把 DTR 置**高**，
    于是"打开串口"这个动作本身就把芯片踹进了下载模式 —— 表现就是
    "烧完固件板子没反应、也扫不到 BLE、串口只有 ROM 的欢迎信息"。

    所以这里绕开 pyserial 的自动 open，用 serial_for_url(do_not_open=True)
    **先**把 _dtr_state/_rts_state 设成 False 再 open()，
    保证芯片复位时看到的是 DTR=0。

用法：
    /usr/bin/python3 tools/serial_console.py --port /dev/cu.usbmodem11101 --seconds 10
    /usr/bin/python3 tools/serial_console.py --port ... --send "PROBE:DUMP" --seconds 5
    /usr/bin/python3 tools/serial_console.py --port ... --no-boot      # 不复位，直接连
"""

import argparse
import serial
import sys
import time


def open_keeping_boot_high(port, baud):
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem11101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--send", default=None, help="连上后发一行（自动补 \\n）")
    ap.add_argument("--no-boot", action="store_true",
                    help="不复位（默认会先脉冲一次 RTS 把芯片复位进应用）")
    ap.add_argument("--out", default=None, help="同时存一份到文件")
    args = ap.parse_args()

    try:
        ser = open_keeping_boot_high(args.port, args.baud)
    except Exception as e:
        print("打开串口失败: %s" % e, file=sys.stderr)
        return 2

    # 复位芯片：只拉 RTS（EN）。DTR 保持低 → BOOT 高 → 复位后跑应用，
    # 而不是像之前那样直接进 ROM 下载模式。
    if not args.no_boot:
        try:
            ser.rts = True
            time.sleep(0.12)
            ser.rts = False
        except Exception:
            pass

    buf = bytearray()
    t0 = time.time()
    sent = False
    while time.time() - t0 < args.seconds:
        if args.send and not sent and time.time() - t0 > 1.5:
            try:
                ser.write((args.send + "\n").encode())
            except Exception as e:
                print("发送失败: %s" % e, file=sys.stderr)
            sent = True
        try:
            chunk = ser.read(4096)
        except Exception:
            chunk = b""
        if chunk:
            buf.extend(chunk)
            try:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            except Exception:
                pass

    ser.close()
    if args.out:
        with open(args.out, "wb") as f:
            f.write(bytes(buf))
        print("\n[已保存 %d 字节到 %s]" % (len(buf), args.out))
    print("\n[共收到 %d 字节]" % len(buf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
