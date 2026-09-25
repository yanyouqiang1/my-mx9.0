#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
YYQ-MX9.0 键盘指令控制台（HID 厂商通道版）

原理：主机往键盘的 HID 自定义通道（Report ID 6）写一段文本，键盘收到后当成指令执行。
走的是 Windows 自带的 HID 驱动，所以：
  · 不需要装驱动
  · 不占 COM 口、不需要串口权限（内网禁用串口也能用）
  · 和键盘打字共用同一根 USB 线，插上就能用
  · 只用 Python 标准库，不用 pip 装任何东西

用法示例：
    python kbctl_hid.py --list                 # 看看认不认得出键盘
    python kbctl_hid.py                        # 进交互模式
    python kbctl_hid.py alert red              # 红灯爆闪（正文用默认名）
    python kbctl_hid.py alert red 磁盘空间不足  # 红灯爆闪 + 中文描述
    python kbctl_hid.py notify 开会了           # 绿灯通知，快捷写法
    python kbctl_hid.py alert off              # 清空整个通知队列
    python kbctl_hid.py alert cycle            # 红->绿->黄->清空 走一遍
    python kbctl_hid.py marquee "YYQ 极客大师"
    python kbctl_hid.py disp 1
    python kbctl_hid.py raw "ALERT:YELLOW:服务器无响应"

通知是排队的：键盘屏幕和灯光都只显示最新到的那条（屏幕底部常驻一条通知栏，
右上角显示待处理条数）。每按一次键盘上的灯光键处理掉最新的一条，前一条顶上
来，直到队列清空，灯也自然灭掉。主机侧 alert off 是一次性全清。

注意：需要固件里已经带上 HID 厂商通道（VendorHID），旧固件收不到，会报「设备没有该通道」。
"""

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

USB_VID, USB_PID = 0x303A, 0x001F   # 固件里 USB.VID / USB.PID
VENDOR_USAGE_PAGE = 0xFF00          # HID_USAGE_PAGE_VENDOR
VENDOR_USAGE = 0x01                 # 固件报告描述符里的 HID_USAGE(0x01)
REPORT_ID = 6                       # HID_REPORT_ID_VENDOR，固件里枚举出来的就是 6
REPORT_SIZE = 63                    # 不含 Report ID 的净荷长度

ALERT_COLORS = {
    "red": "ALERT:RED",
    "green": "ALERT:GREEN",
    "yellow": "ALERT:YELLOW",
    "off": "ALERT:OFF",
    "stop": "ALERT:OFF",
}

DISP_NAMES = {0: "极客仪表盘", 1: "大字时钟", 2: "实时击键监控", 3: "自定义壁纸"}


# ================================================================== Win32 结构
class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD),
        ("Reserved", ctypes.c_void_p),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG),
        ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT),
        ("VersionNumber", wintypes.USHORT),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT),
        ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x01
FILE_SHARE_WRITE = 0x02
OPEN_EXISTING = 3
INVALID_HANDLE = ctypes.c_void_p(-1).value


# ================================================================== Win32 绑定
class WinHID:
    def __init__(self):
        if not sys.platform.startswith("win"):
            raise RuntimeError("这个脚本只能在 Windows 上跑（用的是系统自带的 hid.dll）")

        self.hid = ctypes.WinDLL("hid.dll")
        self.setupapi = ctypes.WinDLL("setupapi.dll")
        self.kernel32 = ctypes.WinDLL("kernel32.dll")

        h = self.hid
        h.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
        h.HidD_GetHidGuid.restype = None
        h.HidD_GetAttributes.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDD_ATTRIBUTES)]
        h.HidD_GetAttributes.restype = wintypes.BOOLEAN
        h.HidD_GetPreparsedData.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        h.HidD_GetPreparsedData.restype = wintypes.BOOLEAN
        h.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
        h.HidD_FreePreparsedData.restype = wintypes.BOOLEAN
        h.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
        h.HidP_GetCaps.restype = ctypes.c_long
        h.HidD_SetOutputReport.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG]
        h.HidD_SetOutputReport.restype = wintypes.BOOLEAN
        h.HidD_SetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG]
        h.HidD_SetFeature.restype = wintypes.BOOLEAN
        h.HidD_GetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG]
        h.HidD_GetFeature.restype = wintypes.BOOLEAN

        s = self.setupapi
        s.SetupDiGetClassDevsW.argtypes = [
            ctypes.POINTER(GUID), wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD]
        s.SetupDiGetClassDevsW.restype = ctypes.c_void_p
        s.SetupDiEnumDeviceInterfaces.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID),
            wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
        s.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
        s.SetupDiGetDeviceInterfaceDetailW.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
            ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        s.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
        s.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
        s.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

        k = self.kernel32
        k.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        k.CreateFileW.restype = ctypes.c_void_p
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        k.CloseHandle.restype = wintypes.BOOL

    # ---------------------------------------------------------- 枚举/打开
    def _device_paths(self):
        guid = GUID()
        self.hid.HidD_GetHidGuid(ctypes.byref(guid))

        hdev = self.setupapi.SetupDiGetClassDevsW(
            ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
        if not hdev or hdev == INVALID_HANDLE:
            raise RuntimeError("SetupDiGetClassDevs 失败，拿不到 HID 设备列表")

        paths = []
        try:
            idx = 0
            while True:
                iface = SP_DEVICE_INTERFACE_DATA()
                iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
                if not self.setupapi.SetupDiEnumDeviceInterfaces(
                        hdev, None, ctypes.byref(guid), idx, ctypes.byref(iface)):
                    break
                idx += 1

                need = wintypes.DWORD(0)
                self.setupapi.SetupDiGetDeviceInterfaceDetailW(
                    hdev, ctypes.byref(iface), None, 0, ctypes.byref(need), None)
                if need.value == 0:
                    continue

                buf = ctypes.create_string_buffer(need.value)
                # cbSize 的取值在 32/64 位下不一样，两个都试一下，谁成谁对
                for cb in (8, 6):
                    ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = cb
                    if self.setupapi.SetupDiGetDeviceInterfaceDetailW(
                            hdev, ctypes.byref(iface), buf, need.value,
                            ctypes.byref(need), None):
                        paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
                        break
        finally:
            self.setupapi.SetupDiDestroyDeviceInfoList(hdev)
        return paths

    def open_path(self, path, write=True):
        access = GENERIC_READ | GENERIC_WRITE if write else 0
        return self.kernel32.CreateFileW(
            path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
            None, OPEN_EXISTING, 0, None)

    def describe(self, path):
        """打开设备读出 VID/PID/Usage，只读方式，不会干扰键盘打字。"""
        info = {"path": path, "vid": None, "pid": None,
                "usage_page": None, "usage": None,
                "out_len": 0, "feat_len": 0}
        h = self.open_path(path, write=False)
        if not h or h == INVALID_HANDLE:
            return info
        try:
            attrs = HIDD_ATTRIBUTES()
            attrs.Size = ctypes.sizeof(HIDD_ATTRIBUTES)
            if self.hid.HidD_GetAttributes(h, ctypes.byref(attrs)):
                info["vid"] = attrs.VendorID
                info["pid"] = attrs.ProductID

            pp = ctypes.c_void_p()
            if self.hid.HidD_GetPreparsedData(h, ctypes.byref(pp)) and pp:
                try:
                    caps = HIDP_CAPS()
                    if self.hid.HidP_GetCaps(pp, ctypes.byref(caps)) >= 0:
                        info["usage_page"] = caps.UsagePage
                        info["usage"] = caps.Usage
                        info["out_len"] = caps.OutputReportByteLength
                        info["feat_len"] = caps.FeatureReportByteLength
                finally:
                    self.hid.HidD_FreePreparsedData(pp)
        finally:
            self.kernel32.CloseHandle(h)
        return info

    def enumerate(self):
        out = []
        for p in self._device_paths():
            d = self.describe(p)
            if d["vid"] is not None:
                out.append(d)
        return out


# ================================================================== 设备连接
class HidLink:
    def __init__(self, report_id=REPORT_ID, quiet=False, verbose=False):
        self.report_id = report_id
        self.quiet = quiet
        self.verbose = verbose
        self.api = WinHID()
        self.handle = None
        self.out_len = 0
        self.feat_len = 0
        self.via = None

    def find(self):
        """挑出厂商自定义通道那个集合（和键盘集合共用同一个 USB 设备）。"""
        devices = self.api.enumerate()
        if self.verbose:
            for d in devices:
                print(f"  [HID] VID:{d['vid']:04X} PID:{d['pid']:04X} "
                      f"UsagePage:0x{d['usage_page']:04X} Usage:0x{d['usage']:04X} "
                      f"OUT:{d['out_len']} FEAT:{d['feat_len']}")

        mine = [d for d in devices if d["vid"] == USB_VID and d["pid"] == USB_PID]
        if not mine:
            raise RuntimeError(
                f"没找到键盘（VID:{USB_VID:04X} PID:{USB_PID:04X}）。\n"
                f"  · 确认键盘用 USB 线插在这台电脑上，键盘能正常打字\n"
                f"  · 如果刚改完固件：需要重新烧录，烧完重新插拔一次 USB\n"
                f"  · 加 --list 看看电脑上都认到了什么"
            )

        vendor = [d for d in mine
                  if d["usage_page"] == VENDOR_USAGE_PAGE and d["usage"] == VENDOR_USAGE]
        if not vendor:
            raise RuntimeError(
                "键盘在，但里面没有『HID 厂商通道』这一路。\n"
                "  说明固件还是旧版 —— 需要把带 VendorHID 的新固件烧进去再试。"
            )

        for d in vendor:
            if d["out_len"] > 0:
                self._pick = d
                self.via = "OutputReport"
                return
        for d in vendor:
            if d["feat_len"] > 0:
                self._pick = d
                self.via = "FeatureReport"
                return
        raise RuntimeError("厂商通道既没有 Output 也没有 Feature 报告，固件报告描述符不对")

    def open(self):
        self.find()
        d = self._pick
        self.out_len = d["out_len"]
        self.feat_len = d["feat_len"]

        h = self.api.open_path(d["path"], write=True)
        if not h or h == INVALID_HANDLE:
            raise RuntimeError(
                f"打不开厂商通道设备（{d['path']}）。\n"
                "  · 换一个 USB 口试试；\n"
                "  · 有些安全软件/组策略会拦 HID 写入，那就得找 IT 放行"
            )
        self.handle = h
        if not self.quiet:
            print(f"已连上键盘 HID 厂商通道（{self.via}，"
                  f"OUT:{self.out_len} FEAT:{self.feat_len}）")

    def send(self, text, echo=True):
        # 结尾必须带换行：固件是按 \n 切行才执行指令的
        payload = text.encode("utf-8") + b"\n"
        if len(payload) > REPORT_SIZE:
            # 超过一个报告的容量就分片，固件那边会拼回完整一行再执行
            for i in range(0, len(payload), REPORT_SIZE):
                self._write(payload[i:i + REPORT_SIZE])
        else:
            self._write(payload)
        if echo:
            print(f"  -> {text}")

    def _write(self, chunk):
        total = self.out_len if self.via == "OutputReport" else self.feat_len
        total = max(total, 1 + len(chunk))
        buf = bytes([self.report_id]) + chunk + b"\x00" * (total - 1 - len(chunk))
        cbuf = ctypes.create_string_buffer(buf, len(buf))

        if self.via == "OutputReport":
            ok = self.api.hid.HidD_SetOutputReport(self.handle, cbuf, len(buf))
        else:
            ok = self.api.hid.HidD_SetFeature(self.handle, cbuf, len(buf))

        if not ok:
            # 换个通道再试一次，两个方向固件都收
            if self.via == "OutputReport":
                ok = self.api.hid.HidD_SetFeature(self.handle, cbuf, len(buf))
                if ok:
                    self.via = "FeatureReport"
                    print("  (Output 报告写不进，已自动切到 Feature 报告)")
            else:
                ok = self.api.hid.HidD_SetOutputReport(self.handle, cbuf, len(buf))
                if ok:
                    self.via = "OutputReport"
                    print("  (Feature 报告写不进，已自动切到 Output 报告)")
        if not ok:
            raise RuntimeError("写入失败，键盘没有响应这个通道")

    def _write_feature(self, chunk):
        total = max(self.feat_len, 1 + len(chunk))
        buf = bytes([self.report_id]) + chunk + b"\x00" * (total - 1 - len(chunk))
        cbuf = ctypes.create_string_buffer(buf, len(buf))
        return bool(self.api.hid.HidD_SetFeature(self.handle, cbuf, len(buf)))

    def _read_feature(self):
        """固件把主机 SET_FEATURE 的内容原样存着，GET_FEATURE 能读回来。"""
        total = max(self.feat_len, 64)
        cbuf = ctypes.create_string_buffer(total)
        ctypes.cast(cbuf, ctypes.POINTER(ctypes.c_ubyte))[0] = self.report_id
        if not self.api.hid.HidD_GetFeature(self.handle, cbuf, total):
            return None
        return cbuf.raw

    def self_test(self):
        """发一条探针再从固件读回来，不靠肉眼就能确认通道是通的。"""
        probe = b"HIDTEST:PING\n"
        if not self._write_feature(probe):
            print("  ! 写 Feature 报告失败")
            return False
        time.sleep(0.2)
        back = self._read_feature()
        if back is None:
            print("  ! 读 Feature 报告失败")
            return False
        echo = back[1:1 + len(probe)]  # 第 0 字节是 Report ID
        if echo == probe:
            print("  OK 固件收到了探针，回读一致 —— 通道双向打通")
            return True
        print(f"  ? 回读内容：{echo!r}")
        print("    没对上，固件可能没跑到 VendorHID 的 SET_FEATURE 分支")
        return False

    def close(self):
        if self.handle:
            self.api.kernel32.CloseHandle(self.handle)
            self.handle = None


# ================================================================== 指令翻译
def build_command(line):
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
        # alert <颜色> [描述文字]：描述可以带空格、可以是中文，省略就用固件默认名
        bits = rest.split(None, 1)
        key = bits[0].lower()
        text = bits[1].strip() if len(bits) > 1 else ""
        if key not in ALERT_COLORS:
            raise ValueError("颜色只能是 red / green / yellow / off / cycle")
        if not text:
            return ALERT_COLORS[key]
        if key in ("off", "stop"):
            raise ValueError("off 是清空整个通知队列，后面不能跟描述")
        return f"{ALERT_COLORS[key]}:{text}"

    if cmd in ("notify", "notif", "n"):
        if not rest:
            raise ValueError("用法：notify <描述文字>")
        return f"NOTIFY:{rest}"

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

    if ":" in line and parts[0].isupper():
        return line.strip()

    raise ValueError(f"不认识这条命令：{line.strip()}   输入 help 看用法")


def alert_cycle(link):
    for name in ("red", "green", "yellow", "off"):
        label = {"red": "红灯爆闪", "green": "绿灯闪烁",
                 "yellow": "黄灯闪烁", "off": "清空通知队列"}[name]
        print(f"  {label}")
        link.send(ALERT_COLORS[name], echo=False)
        time.sleep(2.0)
    print("  演示结束")


HELP_TEXT = """可用命令：
  selftest                     自检：从固件回读探针，确认通道通不通
  alert red|green|yellow|off   触发红/绿/黄爆闪，off 清空整个队列
  alert red 磁盘空间不足        带中文描述下发一条通知（描述可省略）
  notify 开会了                 快捷写法，等同 alert green 开会了
  alert cycle                  红->绿->黄->清空 连续演示一遍
  disp 0|1|2|3                 切换主屏风格（0 极客 1 大字时钟 2 击键监控 3 壁纸）
  keys on|off                  按键回显开关
  time                         把电脑当前时间同步给键盘
  marquee <文本>                修改极客屏底部跑马灯标语
  raw <指令>                    发送原始指令，如 raw ALERT:GREEN
  help / exit                  帮助 / 退出
也可以直接输入固件原始指令，例如 ALERT:RED、DISP_MODE:2
通知是排队的：屏和灯都只显示最新一条，按键盘上的灯光键逐条确认。"""


def interactive(link):
    print("\n" + HELP_TEXT + "\n")
    while True:
        try:
            line = input("kb> ").strip()
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
        if line.lower() in ("selftest", "ping", "test"):
            link.self_test()
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
                alert_cycle(link)
            else:
                link.send(text)
        except Exception as e:
            print(f"  ! 发送失败：{e}")


# ================================================================== 入口
def main():
    parser = argparse.ArgumentParser(
        description="YYQ-MX9.0 键盘指令控制台（HID 厂商通道）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("command", nargs="*", help="要执行的命令，留空则进交互模式")
    parser.add_argument("--list", action="store_true", help="列出电脑上所有 HID 设备后退出")
    parser.add_argument("--report-id", type=int, default=REPORT_ID,
                        help=f"厂商报告 ID，默认 {REPORT_ID}")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印枚举到的每个 HID 接口")
    args = parser.parse_args()
    args.command = " ".join(args.command).strip()

    try:
        api = WinHID()
    except Exception as e:
        print(f"{e}", file=sys.stderr)
        sys.exit(1)

    if args.list:
        devices = api.enumerate()
        print(f"共 {len(devices)} 个 HID 接口：")
        hit = 0
        for d in devices:
            mine = d["vid"] == USB_VID and d["pid"] == USB_PID
            vend = mine and d["usage_page"] == VENDOR_USAGE_PAGE
            if mine:
                hit += 1
            tag = ""
            if vend:
                tag = "  <== 键盘厂商通道（脚本用这个）"
            elif mine:
                tag = "  (键盘)"
            print(f"  VID:{d['vid']:04X} PID:{d['pid']:04X} "
                  f"UsagePage:0x{d['usage_page']:04X} Usage:0x{d['usage']:04X} "
                  f"OUT:{d['out_len']:>3} FEAT:{d['feat_len']:>3}{tag}")
        if hit == 0:
            print("\n没看到键盘，确认 USB 线插好、键盘能打字。")
            sys.exit(1)
        if not any(d["usage_page"] == VENDOR_USAGE_PAGE
                   and d["vid"] == USB_VID and d["pid"] == USB_PID for d in devices):
            print("\n键盘在，但没有厂商通道 —— 固件是旧版的，需要烧录带 VendorHID 的新固件。")
            sys.exit(1)
        sys.exit(0)

    link = HidLink(report_id=args.report_id, verbose=args.verbose)
    try:
        link.open()
    except Exception as e:
        print(f"连接失败：{e}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.command.split()[0].lower() in ("selftest", "ping", "test"):
            ok = link.self_test()
            sys.exit(0 if ok else 3)
        elif args.command:
            try:
                text = build_command(args.command)
            except ValueError as e:
                print(f"! {e}", file=sys.stderr)
                sys.exit(2)
            if text == "__CYCLE__":
                alert_cycle(link)
            elif text is not None:
                link.send(text)
        else:
            interactive(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
