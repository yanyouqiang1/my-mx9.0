#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 assets/lock_src.png（MMX 生成的锁图）转成固件能直接用的 RGB565 数组，
输出 src/lock_img.c。

为什么走"生成 C 数组"这条路，而不是 JPEG：
  · 固件里已经有 JPEGDEC（壁纸用的），但解码要占一块 PSRAM + 几百毫秒，
    而这张图**只在锁屏时用一次**，开机就解纯属浪费；
  · 直接把 RGB565 数组放进 flash，lv_img_dsc.data 一指就完事 ——
    零解码、零拷贝、零 PSRAM 占用；
  · 代价是 flash 多占 ~46KB。板子是 16MB flash / app 分区 3MB，
    现在固件约 2.0MB，余量 1MB，占用得起。

⚠ 背景处理（这一步是重点，别改成"原样贴图"）：
  MMX 出的图背景不是纯黑，是一圈 13~40 的暗灰渐变。直接贴到纯黑的锁屏上，
  屏幕上会多出一个"比底色亮一点的方块"，非常难看。
  所以这里用亮度做 alpha，把背景抠掉再合成到纯黑上：
      alpha = clamp((lum - LO) / (HI - LO), 0, 1)
  锁体是纯白（lum 240+），锁孔是红（lum 82），都远高于 HI，所以不会被削暗；
  锁体边缘的抗锯齿像素（lum 40~80）会被平滑地淡出，边缘不毛躁。

用法（需要 PIL；本机 /usr/bin/python3 有，PATH 上的 python3 没有）：
    /usr/bin/python3 gen_lock_img.py
"""

import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets", "lock_src.png")
OUT_C = os.path.join(HERE, "src", "lock_img.c")
OUT_PNG = os.path.join(HERE, "assets", "lock_rgb565.png")

# 锁图在屏上的尺寸（像素）。屏是 240x240，锁占 176 高 ≈ 屏高的 73%，
# 下面留 182 / 204 两行给 "YYQ 已锁定" 和 "Fn + LOGO 解锁"。
LOCK_W = 132
LOCK_H = 176

# 背景抠图阈值（亮度 0~255）
BG_LO = 34.0   # <= 这个亮度 = 纯背景
BG_HI = 80.0   # >= 这个亮度 = 完全不透明


def srgb_to_565(r, g, b):
    """8 位 RGB -> RGB565（和 LVGL 在 ESP32 上的内存布局一致：小端 16 位）。"""
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


def main():
    if not os.path.exists(SRC):
        print("找不到 %s" % SRC)
        return 1

    im = Image.open(SRC).convert("RGB")
    px = im.load()
    w, h = im.size

    # ---- 1. 找出锁的实际外框（背景抠掉之后的非透明像素） ----
    lum = [[0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2]
            for x in range(w)] for y in range(h)]
    min_x, max_x, min_y, max_y = w, -1, h, -1
    for y in range(h):
        row = lum[y]
        for x in range(w):
            if row[x] > BG_HI:
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
    if max_x < 0:
        print("图里找不到锁（亮度阈值 %d 太高）" % BG_HI)
        return 1
    print("锁的外框: x[%d,%d] y[%d,%d]  = %dx%d" %
          (min_x, max_x, min_y, max_y, max_x - min_x + 1, max_y - min_y + 1))

    # ---- 2. 裁到外框，按比例缩放进 LOCK_W x LOCK_H ----
    crop = im.crop((min_x, min_y, max_x + 1, max_y + 1))
    scale = min(LOCK_W / crop.width, LOCK_H / crop.height)
    nw = max(1, int(round(crop.width * scale)))
    nh = max(1, int(round(crop.height * scale)))
    crop = crop.resize((nw, nh), Image.LANCZOS)

    # ---- 3. 合成到纯黑底：背景 -> 黑，锁体按 alpha 淡出边缘 ----
    canvas = Image.new("RGB", (LOCK_W, LOCK_H), (0, 0, 0))
    cp = crop.load()
    out = [[0] * LOCK_W for _ in range(LOCK_H)]
    ox = (LOCK_W - nw) // 2
    oy = (LOCK_H - nh) // 2
    for y in range(nh):
        for x in range(nw):
            r, g, b = cp[x, y]
            l = 0.299 * r + 0.587 * g + 0.114 * b
            a = (l - BG_LO) / (BG_HI - BG_LO)
            a = 0.0 if a < 0.0 else (1.0 if a > 1.0 else a)
            # 合成到纯黑：out = src * a
            out[oy + y][ox + x] = (int(r * a + 0.5), int(g * a + 0.5), int(b * a + 0.5))
    canvas.putdata([out[y][x] for y in range(LOCK_H) for x in range(LOCK_W)])
    canvas.save(OUT_PNG)

    # ---- 4. 转 RGB565 并生成 C 数组 ----
    vals = []
    for y in range(LOCK_H):
        for x in range(LOCK_W):
            r, g, b = out[y][x]
            vals.append(srgb_to_565(r, g, b))

    lines = []
    lines.append("// 自动生成，请勿手改。")
    lines.append("// 由 gen_lock_img.py 从 assets/lock_src.png 生成。")
    lines.append("//")
    lines.append("// 锁屏用的那把大锁：%dx%d，RGB565 小端（LV_COLOR_DEPTH=16）。"
                 % (LOCK_W, LOCK_H))
    lines.append("// 数组放在 flash（.flash.rodata），lv_img_dsc.data 直接指它，")
    lines.append("// 不用解码、不占 PSRAM。")
    lines.append('#include "lock_img.h"')
    lines.append("")
    lines.append("const uint16_t lock_img_pixels[LOCK_IMG_W * LOCK_IMG_H] = {")
    for i in range(0, len(vals), 8):
        row = vals[i:i + 8]
        lines.append("    " + ", ".join("0x%04X" % v for v in row) + ",")
    lines.append("};")
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    # 顺带把头文件也写了，免得两个文件里的尺寸对不上
    with open(os.path.join(HERE, "src", "lock_img.h"), "w", encoding="utf-8") as f:
        f.write("// 自动生成，请勿手改。由 gen_lock_img.py 生成。\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n")
        f.write("\n")
        f.write("#define LOCK_IMG_W %d\n" % LOCK_W)
        f.write("#define LOCK_IMG_H %d\n" % LOCK_H)
        f.write("\n")
        f.write("// RGB565 像素数组（放在 flash，lv_img_dsc.data 直接指它）\n")
        f.write("extern const uint16_t lock_img_pixels[LOCK_IMG_W * LOCK_IMG_H];\n")

    print("已写出 %s（%d 字节像素 / %d 字节 flash）" %
          (OUT_C, len(vals) * 2, len(vals) * 2))
    print("预览图: %s" % OUT_PNG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
