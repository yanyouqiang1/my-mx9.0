#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成「方案+按键」主屏风格要用的两个系统图标 —— 白色苹果 + 蓝色 Windows。

为什么不用 src/profile_icons.h 里现成的那套：
    那套是 mmx 图像生成出来的 512x512 位图缩到 16/32/48 三档，
    48px 那档放大看根本认不出是苹果（糊成一坨白团，边缘还有粉/绿的杂色），
    Windows 那档是**青绿渐变**，不是用户要的蓝色。
    这一版改成**真矢量路径栅格化**，边缘绝对干净。

素材来源：Font Awesome Free 6.7.2 (brands/apple, brands/windows)
    https://fontawesome.com/license/free  —— 图标本体 CC BY 4.0，字体 SIL OFL 1.1
    路径数据内嵌在下面，生成脚本不联网。

输出：src/style_icons.h
    LV_IMG_CF_TRUE_COLOR_ALPHA，每像素 3 字节 = [RGB565高, RGB565低, alpha]
    （LV_COLOR_DEPTH=16 时 LV_IMG_PX_SIZE_ALPHA_BYTE = 3，已对过
      .pio/libdeps/.../lvgl/src/draw/lv_img_buf.h）

用法：
    /usr/bin/python3 gen_style_icons.py            # 生成 src/style_icons.h
    /usr/bin/python3 gen_style_icons.py --preview  # 顺便出 PNG 预览
"""

import os
import re
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT_H = os.path.join(SRC, "style_icons.h")
PREVIEW_DIR = os.path.join(HERE, "assets")

# ---------------------------------------------------------------------------
# 矢量路径（Font Awesome Free 6.7.2, brands）
# ---------------------------------------------------------------------------
APPLE_SVG = (
    "M318.7 268.7c-.2-36.7 16.4-64.4 50-84.8-18.8-26.9-47.2-41.7-84.7-44.6"
    "-35.5-2.8-74.3 20.7-88.5 20.7-15 0-49.4-19.7-76.4-19.7C63.3 141.2 4 "
    "184.8 4 273.5q0 39.3 14.4 81.2c12.8 36.7 59 126.7 107.2 125.2 25.2-.6 "
    "43-17.9 75.8-17.9 31.8 0 48.3 17.9 76.4 17.9 48.6-.7 90.4-82.5 "
    "102.6-119.3-65.2-30.7-61.7-90-61.7-91.9zm-56.6-164.2c27.3-32.4 24.8-61.9 "
    "24-72.5-24.1 1.4-52 16.4-67.9 34.9-17.5 19.8-27.8 44.3-25.6 71.9 26.1 2 "
    "49.9-11.4 69.5-34.3z"
)
APPLE_VB = (0, 0, 384, 512)

WINDOWS_SVG = (
    "M0 93.7l183.6-25.3v177.4H0V93.7zm0 324.6l183.6 25.3V268.4H0v149.9zm203.8 "
    "28L448 480V268.4H203.8v177.9zm0-380.6v180.1H448V32L203.8 65.7z"
)
WINDOWS_VB = (0, 0, 448, 512)

# 配色：苹果纯白（用户点名"咬了一口的白色"），Windows 用官方蓝 #0078D4。
#
# ⚠ 只有**点亮态**两版了：左列现在只显示当前方案的那一个图标
#   （2026-10-22 用户定稿："左边图标还是展示一个吧，两个也有点分不清楚"），
#   没有第二个"压灰的另一个方案"了 —— 灰版图标连同它的 8KB 数据一起删掉。
CLR_APPLE = (0xFF, 0xFF, 0xFF)
CLR_WINDOWS = (0x00, 0x78, 0xD4)

# 目标边长（px）。必须和固件里的 PK_ICON（52）以及 mock 里的 ICON 一致 ——
# 图标是位图，尺寸对不上就是拉伸变形。
ICON_SIZE = 52
SUPERSAMPLE = 8


# ---------------------------------------------------------------------------
# 极简 SVG path 解析 + 贝塞尔展平
# ---------------------------------------------------------------------------
TOKEN_RE = re.compile(r"([MmLlHhVvCcSsQqTtZz])|(-?\d*\.?\d+(?:[eE][-+]?\d+)?)")


def tokenize(d):
    out = []
    for cmd, num in TOKEN_RE.findall(d):
        out.append(cmd if cmd else float(num))
    return out


def _flatten_cubic(p0, p1, p2, p3, steps=24):
    pts = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        x = (u ** 3 * p0[0] + 3 * u * u * t * p1[0]
             + 3 * u * t * t * p2[0] + t ** 3 * p3[0])
        y = (u ** 3 * p0[1] + 3 * u * u * t * p1[1]
             + 3 * u * t * t * p2[1] + t ** 3 * p3[1])
        pts.append((x, y))
    return pts


def _flatten_quad(p0, p1, p2, steps=18):
    pts = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
        y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        pts.append((x, y))
    return pts


def parse_path(d):
    """返回若干条子路径，每条是 [(x, y), ...]。"""
    toks = tokenize(d)
    subpaths = []
    cur = []
    start = (0.0, 0.0)
    pos = (0.0, 0.0)
    prev_ctrl = None      # 上一个三次控制点（给 S/s 用）
    prev_quad = None      # 上一个二次控制点（给 T/t 用）
    cmd = None
    i = 0
    n = len(toks)

    def flush():
        if len(cur) > 1:
            subpaths.append(list(cur))

    args_per_cmd = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2}

    while i < n:
        t = toks[i]
        if isinstance(t, str):
            cmd = t
            i += 1
            if cmd in "Zz":
                flush()
                cur = []
                pos = start
                prev_ctrl = None
                prev_quad = None
                continue
            # 数字紧跟在命令后面时，沿用当前命令 —— 这是 SVG 的"隐式重复"，
            # 比如 "c-.2-36.7 16.4-64.4 50-84.8-18.8-26.9-47.2-41.7-84.7-44.6" 里
            # 一条 c 命令连着 4 组 6 个参数。命令只在遇到下一个字母时才换。

        need = args_per_cmd[cmd.upper()]
        args = []
        for _ in range(need):
            if i >= n or isinstance(toks[i], str):
                raise ValueError("path 参数不足：%s" % cmd)
            args.append(toks[i])
            i += 1
        rel = cmd.islower()
        c = cmd.upper()

        if c == "M":
            x, y = args
            if rel:
                x, y = pos[0] + x, pos[1] + y
            flush()
            cur = [(x, y)]
            start = (x, y)
            pos = (x, y)
            # M 之后紧跟的坐标对按 L 处理
            cmd = "l" if rel else "L"
            continue
        if c == "L":
            x, y = args
            if rel:
                x, y = pos[0] + x, pos[1] + y
            cur.append((x, y))
            pos = (x, y)
        elif c == "H":
            x = args[0]
            if rel:
                x = pos[0] + x
            cur.append((x, pos[1]))
            pos = (x, pos[1])
        elif c == "V":
            y = args[0]
            if rel:
                y = pos[1] + y
            cur.append((pos[0], y))
            pos = (pos[0], y)
        elif c == "C":
            x1, y1, x2, y2, x, y = args
            if rel:
                x1, y1 = pos[0] + x1, pos[1] + y1
                x2, y2 = pos[0] + x2, pos[1] + y2
                x, y = pos[0] + x, pos[1] + y
            cur.extend(_flatten_cubic(pos, (x1, y1), (x2, y2), (x, y)))
            prev_ctrl = (x2, y2)
            pos = (x, y)
        elif c == "S":
            x2, y2, x, y = args
            if rel:
                x2, y2 = pos[0] + x2, pos[1] + y2
                x, y = pos[0] + x, pos[1] + y
            if prev_ctrl is None:
                c1 = pos
            else:
                c1 = (2 * pos[0] - prev_ctrl[0], 2 * pos[1] - prev_ctrl[1])
            cur.extend(_flatten_cubic(pos, c1, (x2, y2), (x, y)))
            prev_ctrl = (x2, y2)
            pos = (x, y)
        elif c == "Q":
            x1, y1, x, y = args
            if rel:
                x1, y1 = pos[0] + x1, pos[1] + y1
                x, y = pos[0] + x, pos[1] + y
            cur.extend(_flatten_quad(pos, (x1, y1), (x, y)))
            prev_quad = (x1, y1)
            pos = (x, y)
        elif c == "T":
            x, y = args
            if rel:
                x, y = pos[0] + x, pos[1] + y
            if prev_quad is None:
                c1 = pos
            else:
                c1 = (2 * pos[0] - prev_quad[0], 2 * pos[1] - prev_quad[1])
            cur.extend(_flatten_quad(pos, c1, (x, y)))
            prev_quad = c1
            pos = (x, y)
        else:
            raise ValueError("未支持的 path 命令：%s" % cmd)
        prev_ctrl = None if c not in ("C", "S") else prev_ctrl
        prev_quad = None if c not in ("Q", "T") else prev_quad
        continue

    flush()
    return subpaths


# ---------------------------------------------------------------------------
# 栅格化
# ---------------------------------------------------------------------------
def rasterize(path_d, viewbox, size, color, supersample=SUPERSAMPLE):
    """把一条 path 画成 size x size 的 RGBA 剪影（抗锯齿由超采样实现）。"""
    vx, vy, vw, vh = viewbox
    subpaths = parse_path(path_d)

    canvas = size * supersample
    scale = canvas / float(max(vw, vh))
    off_x = (canvas - vw * scale) / 2.0 - vx * scale
    off_y = (canvas - vh * scale) / 2.0 - vy * scale

    # 每条子路径单独画一个 mask，再取并集。
    # 不用 even-odd 也不依赖 winding 方向 —— 苹果那条路径的叶子是独立子路径，
    # Windows 是 4 块独立窗格，并集最稳。
    mask = Image.new("L", (canvas, canvas), 0)
    for sp in subpaths:
        pts = [(off_x + x * scale, off_y + y * scale) for (x, y) in sp]
        m = Image.new("L", (canvas, canvas), 0)
        ImageDraw.Draw(m).polygon(pts, fill=255)
        mask = Image.composite(m, mask, m.point(lambda v: 255 if v else 0))

    mask = mask.resize((size, size), Image.LANCZOS)

    im = Image.new("RGBA", (size, size), (color[0], color[1], color[2], 0))
    solid = Image.new("RGBA", (size, size), (color[0], color[1], color[2], 255))
    im.paste(solid, (0, 0), mask)
    return im


# ---------------------------------------------------------------------------
# RGB565 + alpha 数组
# ---------------------------------------------------------------------------
def to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def emit_array(name, im):
    """LV_IMG_CF_TRUE_COLOR_ALPHA：每像素 [高字节, 低字节, alpha]。"""
    w, h = im.size
    px = im.load()
    lines = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            v = to_rgb565(r, g, b)
            row.append("0x%02X, 0x%02X, 0x%02X" % ((v >> 8) & 0xFF, v & 0xFF, a))
        # 每行末尾也要有逗号 —— 否则换行处会变成 "00 FF" 这种非法记号
        lines.append("    " + ", ".join(row) + ",")
    body = "\n".join(lines)
    return ("static const uint8_t %s[] = {\n%s\n};\n" % (name, body))


def build_header(items):
    out = []
    out.append("// Auto-generated by gen_style_icons.py -- DO NOT EDIT BY HAND.")
    out.append("// 矢量素材: Font Awesome Free 6.7.2 (brands/apple, brands/windows)")
    out.append("//   https://fontawesome.com/license/free  (图标 CC BY 4.0)")
    out.append("//")
    out.append("// 格式 LV_IMG_CF_TRUE_COLOR_ALPHA，每像素 3 字节 =")
    out.append("//   [RGB565 高字节, RGB565 低字节, alpha]")
    out.append("//   (LV_COLOR_DEPTH=16 => LV_IMG_PX_SIZE_ALPHA_BYTE = 3)")
    out.append("#pragma once")
    out.append("#include <stdint.h>")
    out.append("#include <lvgl.h>")
    out.append("")

    for name, im, desc in items:
        out.append("// %s" % desc)
        out.append(emit_array(name + "_data", im))
        out.append("static const lv_img_dsc_t %s_desc = {" % name)
        out.append("    .header = { .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .always_zero = 0,")
        out.append("                .reserved = 0, .w = %d, .h = %d }," % im.size)
        out.append("    .data_size = sizeof(%s_data)," % name)
        out.append("    .data = %s_data," % name)
        out.append("};")
        out.append("")
    return "\n".join(out)


def main():
    size = ICON_SIZE

    apple = rasterize(APPLE_SVG, APPLE_VB, size, CLR_APPLE)
    win = rasterize(WINDOWS_SVG, WINDOWS_VB, size, CLR_WINDOWS)

    items = [
        ("style_icon_apple", apple, "苹果 logo：纯白咬一口剪影（当前方案 = macOS）"),
        ("style_icon_win", win, "Windows 四格旗：官方蓝 #0078D4（当前方案 = Windows）"),
    ]

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write(build_header(items))
    print("已写出 %s" % OUT_H)

    if "--preview" in sys.argv:
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        sheet = Image.new("RGB", (size * len(items) + 30, size + 20), (11, 15, 23))
        x = 5
        for _name, im, _d in items:
            sheet.paste(im, (x, 10), im)
            x += size + 5
        p = os.path.join(PREVIEW_DIR, "style_icons_preview.png")
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(p)
        print("预览 %s" % p)


if __name__ == "__main__":
    main()
