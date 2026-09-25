#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
YYQ-MX9.0 键盘指令控制台

默认走 BLE（和 s3-setting.html 同一套 UUID），也可以走 USB 串口。

用法示例：
    python kbctl.py                        # 进入交互模式（默认 BLE）
    python kbctl.py alert red              # 红灯爆闪
    python kbctl.py alert green            # 绿灯闪烁
    python kbctl.py alert cycle            # 红->绿->黄->解除 走一遍
    python kbctl.py marquee "YYQ 极客大师"  # 改极客屏跑马灯标语
    python kbctl.py disp 1                 # 切到大字时钟
    python kbctl.py raw "ALERT:YELLOW"     # 直接发原始指令
    python kbctl.py --serial COM5 alert red    # 指定串口
    python kbctl.py --serial alert red         # 自动找 ESP32 串口

依赖：
    BLE  -> pip install bleak
    串口 -> pip install pyserial
"""

import argparse
import asyncio
import datetime
import sys
import time

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

DEVICE_NAME = "YYQ-MX9.0"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
USB_VID, USB_PID = 0x303A, 0x001F
BAUD = 115200

ALERT_COLORS = {
    "red": "ALERT:RED",
    "green": "ALERT:GREEN",
    "yellow": "ALERT:YELLOW",
    "off": "ALERT:OFF",
    "stop": "ALERT:OFF",
}

DISP_NAMES = {0: "极客仪表盘", 1: "大字时钟", 2: "实时击键监控", 3: "自定义壁纸"}


# ------------------------------------------------------------------ 传输层
class BleLink:
    """通过 BLE 特征值写指令，和网页端同一套 UUID。"""

    kind = "BLE"

    def __init__(self, timeout=20.0):
        self.timeout = timeout
        self.client = None
        self.label = DEVICE_NAME

    async def open(self):
        from bleak import BleakClient, BleakScanner

        print(f"扫描 BLE 设备 {DEVICE_NAME} ... (最多等 {self.timeout:.0f}s)")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=self.timeout)
        if dev is None:
            raise RuntimeError(
                f"没找到蓝牙设备 {DEVICE_NAME}\n"
                f"  · 确认键盘已上电\n"
                f"  · 确认没有被电脑/手机的蓝牙设置页或网页端占用连接"
            )

        self.client = BleakClient(dev, timeout=self.timeout)
        await self.client.connect()
        self.label = f"{dev.name or DEVICE_NAME} ({dev.address})"
        print(f"已连接 {self.label}")

    async def send(self, text):
        await self.client.write_gatt_char(CHAR_UUID, text.encode("utf-8"), response=True)

    async def close(self):
        if self.client is not None and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass


class SerialLink:
    """通过 USB 串口发指令，每条指令以 \\n 结尾。"""

    kind = "串口"

    def __init__(self, port=None, baud=BAUD):
        self.port = port
        self.baud = baud
        self.ser = None
        self.label = port or "(自动)"

    def _auto_port(self):
        from serial.tools import list_ports

        ports = list(list_ports.comports())
        for p in ports:
            if p.vid == USB_VID and p.pid == USB_PID:
                return p.device
        for p in ports:
            if p.vid == USB_VID:
                return p.device
        if len(ports) == 1:
            return ports[0].device
        found = ", ".join(f"{p.device}({p.description})" for p in ports) or "无"
        raise RuntimeError(f"没找到 ESP32 串口，当前可用端口：{found}\n可以手动指定：--serial COM5")

    def open(self):
        import serial

        if not self.port:
            self.port = self._auto_port()
        self.ser = serial.Serial(self.port, self.baud, timeout=0.1, write_timeout=1)
        time.sleep(1.5)  # 等 CDC 稳定，顺便清掉复位时喷出来的日志
        self.ser.reset_input_buffer()
        self.label = f"{self.port} @ {self.baud}"
        print(f"已打开串口 {self.label}")

    async def send(self, text):
        self.ser.write((text + "\n").encode("utf-8"))
        self.ser.flush()

    async def close(self):
        if self.ser is not None and self.ser.is_open:
            self.ser.close()


# ------------------------------------------------------------------ 指令翻译
def build_command(line):
    """把简写翻译成固件指令；返回 None 表示本地命令或空行。"""
    parts = line.strip().split(None, 1)
    if not parts:
        return None
    cmd = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ""

    if cmd in ("alert", "alarm", "a"):
        if not rest:
            return "ALERT:RED"
        if rest.lower() == "cycle":
            return "__CYCLE__"
        key = rest.lower()
        if key not in ALERT_COLORS:
            raise ValueError("颜色只能是 red / green / yellow / off / cycle")
        return ALERT_COLORS[key]

    if cmd in ("disp", "display", "style"):
        idx = int(rest)
        if idx not in DISP_NAMES:
            raise ValueError("风格编号只能是 0-3")
        return f"DISP_MODE:{idx}"

    if cmd in ("keys", "keystroke", "echo"):
        on = rest.lower() in ("1", "on", "true", "yes", "开")
        return f"SET_KEYSTROKE:{1 if on else 0}"

    if cmd in ("time", "sync"):
        return f"TIME:{int(time.time())}"

    if cmd in ("marquee", "banner"):
        if not rest:
            raise ValueError("用法：marquee <文本>")
        return f"MARQUEE:{rest}"

    if cmd == "raw":
        if not rest:
            raise ValueError("用法：raw <完整指令>")
        return rest

    # 看着像完整指令就直接透传，比如 ALERT:GREEN
    if ":" in line and parts[0].isupper():
        return line.strip()

    raise ValueError(f"不认识这条命令：{line.strip()}   输入 help 看用法")


async def run_command(link, text, quiet=False):
    await link.send(text)
    if not quiet:
        print(f"  -> {text}")


async def alert_cycle(link):
    """红 -> 绿 -> 黄 -> 解除，各闪 2 秒，用来肉眼确认灯带通了。"""
    for name in ("red", "green", "yellow", "off"):
        label = {"red": "红灯爆闪", "green": "绿灯闪烁", "yellow": "黄灯闪烁", "off": "警报解除"}[name]
        print(f"  {label}")
        await link.send(ALERT_COLORS[name])
        await asyncio.sleep(2.0)
    print("  演示结束")


# ------------------------------------------------------------------ 交互模式
HELP_TEXT = """可用命令：
  alert red|green|yellow|off   触发红/绿/黄爆闪，off 解除
  alert cycle                  红->绿->黄->解除 连续演示一遍
  disp 0|1|2|3                 切换主屏风格（0 极客 1 大字时钟 2 击键监控 3 壁纸）
  keys on|off                  按键回显开关
  time                         把电脑当前时间同步给键盘
  marquee <文本>                修改极客屏底部跑马灯标语
  raw <指令>                    发送原始指令，如 raw ALERT:GREEN
  help / exit                  帮助 / 退出
也可以直接输入固件原始指令，例如 ALERT:RED、DISP_MODE:2"""


async def interactive(link):
    print("\n" + HELP_TEXT + "\n")
    loop = asyncio.get_running_loop()
    while True:
        try:
            line = (await loop.run_in_executor(None, input, "kb> ")).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        if line.lower() in ("exit", "quit", "q"):
            return
        if line.lower() == "help":
            print(HELP_TEXT)
            continue
        try:
            text = build_command(line)
        except ValueError as e:
            print(f"  ! {e}")
            continue
        if text is None:
            continue
        try:
            if text == "__CYCLE__":
                await alert_cycle(link)
            else:
                await run_command(link, text)
        except Exception as e:
            print(f"  ! 发送失败：{e}")


# ------------------------------------------------------------------ 入口
async def amain(args):
    if args.serial is not None:
        link = SerialLink(port=args.serial or None, baud=args.baud)
    else:
        link = BleLink(timeout=args.timeout)

    try:
        if isinstance(link, SerialLink):
            link.open()
        else:
            await link.open()
    except Exception as e:
        print(f"连接失败：{e}", file=sys.stderr)
        return 1

    try:
        if args.command:
            text = build_command(args.command)
            if text == "__CYCLE__":
                await alert_cycle(link)
            elif text is not None:
                await run_command(link, text)
        else:
            await interactive(link)
    finally:
        await link.close()
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="YYQ-MX9.0 键盘指令控制台",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("command", nargs="*", help="要执行的命令，留空则进交互模式")
    parser.add_argument("--serial", nargs="?", const="", default=None,
                        help="改用串口（可跟端口名如 COM5，不写则自动找 ESP32）")
    parser.add_argument("--baud", type=int, default=BAUD, help=f"串口波特率，默认 {BAUD}")
    parser.add_argument("--timeout", type=float, default=20.0, help="BLE 扫描/连接超时秒数")
    args = parser.parse_args()
    args.command = " ".join(args.command).strip()

    try:
        sys.exit(asyncio.run(amain(args)))
    except KeyboardInterrupt:
        print("\n已中断")
        sys.exit(130)


if __name__ == "__main__":
    main()
