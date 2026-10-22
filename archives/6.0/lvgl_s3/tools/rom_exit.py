#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把卡在 ROM 下载模式的 S3 弄回应用模式（不碰 BOOT / EN 按钮）。

为什么会有这个需求：
  esptool 烧录收尾用的是 HardReset，只动 RTS（EN），**不动 DTR**。
  而 DTR 的语义是 "DTR=True → IO0 被拉低 → 下载模式"（esptool reset.py 注释写明
  "_setDTR(True)  # IO0=LOW"）。所以烧完之后如果 DTR 还处于 asserted，
  芯片复位后又会落回 ROM 下载模式 —— 表现就是"烧完一直 DOWNLOAD，
  拔线重插才好"，而 USB 枚举永远是 Espressif 的 "USB JTAG/serial debug unit"。

本脚本穷举几种 DTR/RTS 时序 + 1200 波特 touch，每种之后用两个独立信号判定：
  ① USB 产品字符串是否从 "USB JTAG/serial debug unit" 变成应用自己的名字
  ② BLE 能不能扫到 "YYQ-MX9.0"
（① 只能说明 USB 设备重新枚举了，② 才能说明应用真的跑起来了。）

用法:
    /usr/bin/python3 tools/rom_exit.py [端口]      # 默认 /dev/cu.usbmodem1101
"""
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
APP_NAME = "YYQ-MX9.0"


def usb_product():
    """当前接在 USB 上的 Espressif 设备的产品字符串 + idProduct。"""
    try:
        out = subprocess.run(
            ["ioreg", "-p", "IOUSB", "-l", "-w", "0"],
            capture_output=True, text=True, timeout=15).stdout
    except Exception:
        return ("?", "?")
    prods = re.findall(r'"kUSBProductString" = "([^"]*)"', out)
    pids = re.findall(r'"idProduct" = (\d+)', out)
    return (prods[0] if prods else "?", pids[0] if pids else "?")


def scan_ble(seconds=6.0):
    """扫 BLE，看目标在不在。返回 (是否扫到, 命中数)。"""
    code = (
        "import asyncio,sys\n"
        "from bleak import BleakScanner\n"
        "hits=[]\n"
        "def cb(d,a):\n"
        "    n=(a.local_name or d.name or '')\n"
        "    if '%s'.lower() in n.lower(): hits.append((d.address,n,a.rssi))\n"
        "async def m():\n"
        "    s=BleakScanner(detection_callback=cb)\n"
        "    await s.start()\n"
        "    await asyncio.sleep(%s)\n"
        "    await s.stop()\n"
        "    print(len(hits), hits[:3])\n"
        "asyncio.run(m())\n" % (APP_NAME, seconds)
    )
    try:
        out = subprocess.run(["/usr/bin/python3", "-c", code],
                             capture_output=True, text=True, timeout=seconds + 25).stdout
        m = re.search(r"^(\d+) \[", out.strip())
        n = int(m.group(1)) if m else 0
        return (n > 0, n)
    except Exception:
        return (False, 0)


def open_port(dtr, rts):
    """按指定 DTR/RTS 打开端口（先设状态再 open，避免 pyserial 默认 DTR=True）。"""
    s = serial.serial_for_url(PORT, do_not_open=True)
    s._dtr_state = dtr
    s._rts_state = rts
    s.timeout = 0.2
    s.open()
    return s


def read_for(s, seconds):
    out = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            c = s.read(4096)
        except Exception:
            break
        if c:
            out.extend(c)
    return out.decode("utf-8", "replace")


def classify(txt):
    if "DOWNLOAD" in txt or "waiting for download" in txt:
        return "ROM下载模式"
    if txt.strip():
        return "有输出(应用?)"
    return "静音"


def trial(label, fn):
    print("\n=== %s ===" % label, flush=True)
    try:
        txt = fn()
    except Exception as e:
        print("   异常: %s" % e)
        txt = ""
    print("   串口输出: %s" % classify(txt))
    for line in txt.splitlines()[:8]:
        print("      | " + line)
    prod, pid = usb_product()
    print("   USB: product=%r idProduct=%s" % (prod, pid))
    if "JTAG/serial" not in prod:
        ok, n = scan_ble()
        print("   BLE: %s (命中 %d)" % ("扫到了!" if ok else "没扫到", n))
        return (prod, ok)
    return (prod, False)


def main():
    print("端口: %s" % PORT)
    print("起始 USB: %r" % (usb_product(),))

    def t_idle():
        s = open_port(False, False)
        try:
            return read_for(s, 2.5)
        finally:
            s.close()

    def t_rts_pulse():
        s = open_port(False, False)
        try:
            s.rts = True
            time.sleep(0.25)
            s.rts = False
            return read_for(s, 2.5)
        finally:
            s.close()

    def t_dtr_then_pulse():
        # 先把 DTR 拉低(=IO0 高)，再复位
        s = open_port(True, False)
        try:
            time.sleep(0.3)
            s.dtr = False
            time.sleep(0.2)
            s.rts = True
            time.sleep(0.25)
            s.rts = False
            return read_for(s, 2.5)
        finally:
            s.close()

    def t_touch1200():
        # 经典 1200 波特 touch：打开→关掉，很多 ESP 会借此复位进应用
        s = serial.serial_for_url(PORT, do_not_open=True)
        s._dtr_state = False
        s._rts_state = False
        s.baudrate = 1200
        s.open()
        time.sleep(0.2)
        s.close()
        time.sleep(1.0)
        s2 = open_port(False, False)
        try:
            return read_for(s2, 2.5)
        finally:
            s2.close()

    results = [
        ("DTR关/RTS关", t_idle),
        ("DTR关 + RTS脉冲", t_rts_pulse),
        ("DTR开→关 + RTS脉冲", t_dtr_then_pulse),
        ("1200 baud touch", t_touch1200),
    ]
    for label, fn in results:
        prod, ok = trial(label, fn)
        if ok:
            print("\n>>> 成功：设备已经在跑应用并广播了")
            return 0
    print("\n>>> 四种时序都没把设备带回应用模式")
    return 1


if __name__ == "__main__":
    sys.exit(main())
