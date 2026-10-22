#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 ROM 下载模式里把芯片放出来（让它去跑应用）。

背景：esptool 烧完会 "Hard resetting via RTS pin"，芯片常常停在 ROM 的
下载模式（boot:0x23）。应用没跑 = 蓝牙没开 = 什么都测不了。

在这块 S3 上，DTR/RTS 是有讲究的：
  · DTR 拉低 = 放开 BOOT(GPIO0)
  · RTS 脉冲 = 复位(EN)
所以"复位那一刻 GPIO0 必须是高"才是跑应用，"是低"就是进下载模式。

这个脚本按几种时序各试一遍，抓 ROM 的开场白判断结果：
  boot:0x2b (SPI_FAST_FLASH_BOOT)  → 跑应用了 ✓
  boot:0x23 (DOWNLOAD)             → 还在下载模式 ✗
"""
import serial
import sys
import time

PORT = "/dev/cu.usbmodem11101"


def open_port(dtr, rts):
    s = serial.serial_for_url(PORT, do_not_open=True)
    s.baudrate = 115200
    s.timeout = 0.15
    s.write_timeout = 1.0
    s._dtr_state = dtr
    s._rts_state = rts
    s.open()
    return s


def read_for(s, seconds):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            c = s.read(4096)
        except Exception:
            c = b""
        if c:
            buf.extend(c)
    return buf.decode("utf-8", "replace")


def seq(name, dtr_at_open, dtr_before_pulse, pulse=True):
    try:
        s = open_port(dtr_at_open, False)
    except Exception as e:
        print("=== %-42s 打开失败: %r" % (name, e))
        return False
    try:
        time.sleep(0.25)
        if dtr_before_pulse is not None:
            s.dtr = dtr_before_pulse
            time.sleep(0.25)
        if pulse:
            s.rts = True
            time.sleep(0.15)
            s.rts = False
        txt = read_for(s, 3.0)
    finally:
        s.close()
    mode = "APP ✓" if ("0x2b" in txt or "SPI_FAST_FLASH_BOOT" in txt) else \
           ("DOWNLOAD ✗" if "DOWNLOAD" in txt else "静音 ?")
    print("=== %-42s -> %s" % (name, mode))
    for line in txt.splitlines()[:6]:
        print("      | " + line)
    return mode.startswith("APP")


if __name__ == "__main__":
    # ① 打开就 DTR 低 + RTS 脉冲（上次试过，不行）
    if seq("① DTR低打开 + RTS脉冲", False, None):
        sys.exit(0)
    # ② 先 DTR 高（把 BOOT 拉低=进下载模式），再 DTR 低（放开 BOOT），再 RTS 脉冲
    if seq("② DTR高打开 → DTR低 → RTS脉冲", True, False):
        sys.exit(0)
    # ③ 同上但脉冲前多等一会儿让 DTR 生效
    if seq("③ DTR高打开 → DTR低(等1s) → RTS脉冲", True, False):
        sys.exit(0)
    print("\n三条时序都没跑起来 → 需要人工按一下 EN（BOOT 不按）。")
