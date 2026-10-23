#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一键烧录（人工按一次 BOOT，其余全自动）。

为什么还需要人工：
    芯片要进 ROM 下载模式（IO0=0）才能烧。而当前固件是 TinyUSB
    （ARDUINO_USB_MODE=0），USB-Serial-JTAG 外设被占用，**esptool 的复位
    握手完全无效** —— 它连复位信号都发不出去。
    实测（tools/probe_reset_mode.py）：插着 USB 线发 REBOOT，芯片复位后
    仍然回到 app 模式（boots 251→252，BLE 照常广播，USB 节点不变），
    也就是"复位"这条路根本进不了下载模式。
    ⇒ 只剩一条路：人按住 BOOT。

所以流程是：
    ① 脚本先把当前 app 模式的 USB 节点记下来
    ② 提示你：**按住 BOOT，点一下 EN**（或按住 BOOT 拔插 USB 线）
    ③ 脚本盯着 USB 节点：一旦出现"新的"节点（下载模式下 S3 会重新枚举），
       就用 esptool 连上去确认芯片真的在下载模式
    ④ 烧 app（只写 0x10000，**不碰 bootloader / 分区表 / NVS**，
       用户的按键重映射、方案、灯光全在 NVS 里，碰了就全没了）
    ⑤ esptool run 让它回 app 模式，然后等 BLE 回来

用法:
    /usr/bin/python3 tools/flash_manual.py [固件路径]
"""

import glob
import os
import subprocess
import sys
import time

ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
DEFAULT_BIN = ("/Users/yanyouqiang/project/my-mx9.0/archives/6.0/lvgl_s3/"
               ".pio/build/esp32-s3-devkitc-1_debug/firmware.bin")


def usb_nodes():
    out = []
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*"):
        out += glob.glob(pat)
    return sorted(set(out))


def esptool_ok(port, timeout=25):
    """用 esptool 探一下：这个口上是不是真的有芯片在下载模式。"""
    cmd = ["/usr/bin/python3", ESPTOOL, "--port", port, "--chip", "esp32s3",
           "--before", "no_reset", "--after", "no_reset", "chip_id"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except Exception:
        return False, ""
    return p.returncode == 0, (p.stdout or "") + (p.stderr or "")


def main():
    bin_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    if not os.path.exists(bin_path):
        print("✗ 找不到固件: %s" % bin_path)
        return 1

    before = set(usb_nodes())
    print("=" * 62)
    print(" 固件 : %s" % bin_path)
    print(" 体积 : %.1f KB" % (os.path.getsize(bin_path) / 1024.0))
    print(" 当前 USB 节点: %s" % (sorted(before) or "（无）"))
    print("=" * 62)
    print()
    print("  👉 现在请：**按住 BOOT 键不松，点一下 EN 键**")
    print("     （如果按 EN 没反应：按住 BOOT 不松，把 USB 线拔掉再插回去）")
    print()
    print(" 脚本在盯着，一旦芯片进了下载模式就自动往下走…\n")

    port = None
    t0 = time.time()
    tried = set()
    while time.time() - t0 < 180:
        now = set(usb_nodes())
        fresh = now - before
        # 下载模式下 S3 重新枚举，节点名和 app 模式的不同；
        # 万一名字一样（macOS 复用同一个路径），就靠 esptool 能连上为准。
        cands = sorted(fresh) or sorted(now - tried)
        for c in cands:
            tried.add(c)
            ok, _ = esptool_ok(c)
            if ok:
                port = c
                break
        if port:
            break
        time.sleep(1.0)

    if not port:
        print("\n✗ 180 秒内没等到芯片进下载模式。")
        print("  确认：① BOOT 键是不是真的按住了（按住到脚本说'烧录中'）")
        print("        ② 用的是原生 USB 口（不是 TTL 串口）")
        return 2

    print("✓ 芯片在下载模式: %s" % port)

    cmd = ["/usr/bin/python3", ESPTOOL, "--port", port, "--chip", "esp32s3",
           "--before", "no_reset", "--after", "no_reset",
           "write_flash", "0x10000", bin_path]
    print("\n--- 烧录中（只写 app0 @ 0x10000，不碰分区表/NVS）---")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    tail = "\n".join((p.stdout or "").splitlines()[-8:])
    print(tail)
    if p.returncode != 0:
        print("\n✗ 烧录失败 rc=%d\n%s" % (p.returncode, (p.stderr or "")[-1200:]))
        return 3
    print("✓ 烧录完成")

    # 回 app 模式
    print("\n--- 让它回 app 模式（esptool run）---")
    cmd2 = ["/usr/bin/python3", ESPTOOL, "--port", port, "--chip", "esp32s3",
            "--before", "no_reset", "--after", "no_reset", "run"]
    p2 = subprocess.run(cmd2, capture_output=True, text=True, timeout=60)
    print("\n".join((p2.stdout or "").splitlines()[-4:]))

    print("\n--- 等 BLE 回来 ---")
    t0 = time.time()
    while time.time() - t0 < 30:
        time.sleep(2)
        now = set(usb_nodes())
        if now:
            print("✓ USB 节点: %s" % sorted(now))
            break
    print("\n如果 10 秒后 BLE 还是搜不到，就把 USB 线拔掉再插回去一次。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
