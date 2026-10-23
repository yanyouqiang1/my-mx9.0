#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
抓崩溃现场：一边开 USB CDC 收固件日志，一边用 BLE 发命令触发动作。

为什么需要这个脚本：
  BLE 的 "Service Discovery has not been performed yet" 是**症状不是原因** ——
  bleak 里 backend.services 只在断链回调里被置空，所以那条报错真正的含义是
  "设备在传输/切屏的过程中掉线了"。光看 BLE 报错永远定位不到崩溃点。

  debug 固件（ARDUINO_USB_MODE=0 + LOG_CDC_MIRROR=1）会把 LOG_PORT 的内容
  镜像一份到 USB CDC，于是崩溃前后的固件日志能直接从 /dev/cu.usbmodem* 拿到。

用法：
    /usr/bin/python3 tools/crash_probe.py            # 默认触发 DISP_MODE:5（壁纸主屏）
    /usr/bin/python3 tools/crash_probe.py 0          # 触发 DISP_MODE:0（极客仪表盘）
    /usr/bin/python3 tools/crash_probe.py cmd:XXX    # 触发任意命令
"""

import asyncio
import glob
import os
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("✗ 缺 pyserial：/usr/bin/python3 -m pip install pyserial")
    sys.exit(1)

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

lines = []
lock = threading.Lock()


def find_cdc_port():
    cands = []
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*",
                "/dev/cu.SLAB*"):
        cands += glob.glob(pat)
    return cands


def cdc_reader(port, stop_evt):
    """持续读 CDC。注意 dtr/rts 都置 False —— 这台机器上 DTR=True 会把 IO0 拉低，
    等于把芯片踢进下载模式（esptool 的 reset.py 里 _setDTR(True) // IO0=LOW）。"""
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.2
        # ⚠ DTR 和 RTS **必须同时为 True**，TinyUSB 才会把主机判成"已连接"
        #   （USBCDC.cpp 的 _onLineState：只有 dtr && rts && !connected 才置 connected）。
        #   connected=false 时 USBCDC::write() 直接 return 0 —— 固件写得再对，
        #   主机这边一个字节也收不到。
        #
        #   ⚠ 但别走"1200 波特触摸"那条重启路径：那个状态机要求依次经历
        #   !dtr&&rts → dtr&&rts → dtr&&!rts → !dtr&&!rts 四步才重启。
        #   从 (F,F) 直接跳到 (T,T) 只会命中 dtr&&rts 分支（lineState 复位），
        #   不会重启。波特率也绝不能设成 1200（那是 IDE 的重启握手）。
        ser.dtr = True
        ser.rts = True
        ser.open()
    except Exception as e:
        with lock:
            lines.append("[CDC] 打开失败: %s" % e)
        return
    with lock:
        lines.append("[CDC] 已打开 %s" % port)
    buf = b""
    while not stop_evt.is_set():
        try:
            chunk = ser.read(1024)
        except Exception as e:
            with lock:
                lines.append("[CDC] 读失败: %s" % e)
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            with lock:
                lines.append("[CDC] " + raw.decode("utf-8", "replace").rstrip())
    try:
        ser.close()
    except Exception:
        pass


async def find_keyboard(timeout=8.0):
    found = []

    def cb(device, ad):
        svc = [str(u).lower() for u in (ad.service_uuids or [])]
        if any(SERVICE_UUID.lower() in u for u in svc):
            found.append(device.address)
            return
        name = ad.local_name or device.name or ""
        if "YYQ-MX9" in name.upper():
            found.append(device.address)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()
    return list(dict.fromkeys(found))


async def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "5"
    if arg.startswith("cmd:"):
        payload = (arg[4:] + "\n").encode()
        label = arg[4:]
    else:
        payload = ("DISP_MODE:%s\n" % arg).encode()
        label = "DISP_MODE:%s" % arg

    stop_evt = threading.Event()
    ports = find_cdc_port()
    if ports:
        t = threading.Thread(target=cdc_reader, args=(ports[0], stop_evt), daemon=True)
        t.start()
    else:
        with lock:
            lines.append("[CDC] 没找到 CDC 串口节点")

    print("=== 触发命令: %s ===" % label)
    addrs = await find_keyboard()
    if not addrs:
        print("✗ 没扫到键盘")
        stop_evt.set()
        return 1

    addr = addrs[0]
    print("  键盘 %s" % addr)
    try:
        async with BleakClient(addr, timeout=15) as c:
            await c.start_notify(CHAR_UUID, lambda s, d: None)
            await asyncio.sleep(0.3)
            await c.write_gatt_char(CHAR_UUID, payload, response=True)
            print("  已发送，等待 12 秒看设备是否掉线/重启 ...")
            for i in range(12):
                await asyncio.sleep(1.0)
                if not c.is_connected:
                    print("  ⚠ 第 %d 秒：连接已断（设备很可能崩了）" % (i + 1))
                    break
            else:
                print("  ✓ 12 秒内连接一直在")

            # 可选：结束前取消订阅通知。
            # 这一步是 ble_verify.py 与本脚本之间唯一的行为差异，
            # 用来定位"崩溃是不是发生在取消订阅/通知发送这条路上"。
            if "--stop-notify" in sys.argv:
                print("  取消订阅通知 ...")
                try:
                    await c.stop_notify(CHAR_UUID)
                    print("  ✓ stop_notify 成功")
                except Exception as e:
                    print("  ⚠ stop_notify 失败：%s: %s" % (type(e).__name__, e))
                await asyncio.sleep(2.0)
                print("  取消订阅后连接状态：%s" % ("仍连着" if c.is_connected else "已断"))
    except Exception as e:
        print("  BLE 异常: %s: %s" % (type(e).__name__, e))

    await asyncio.sleep(1.0)
    stop_evt.set()
    time.sleep(0.5)

    print("\n=== CDC 抓到的日志 ===")
    with lock:
        for l in lines:
            print(l)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
