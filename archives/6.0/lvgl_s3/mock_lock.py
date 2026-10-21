#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
锁屏（键盘锁定态）的整屏 mock —— 在电脑上先把 240x240 长什么样画出来，
免得烧进去才发现字挤到边上或者锁太大。

用法：
    /usr/bin/python3 mock_lock.py            # 输出 assets/mock_lock.png
    /usr/bin/python3 mock_lock.py --border   # 顺便画出红框告警的样子
"""

import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "assets", "lock_rgb565.png")

W = H = 240

# 和固件里的配色一致（src/lvgl_s3.ino 的 CLR_*）
# 锁屏的底色刻意用**纯黑**而不是 CLR_BG(0x0B0F17)：锁图的背景像素就是纯黑，
# 底色不一致的话屏幕上会多出一个"比周围亮一点的方块"。
CLR_BG = (0x00, 0x00, 0x00)   # 锁屏专用底色（纯黑）
CLR_TEXT = (0xE6, 0xED, 0xF7)
CLR_TEXT_DIM = (0x8E, 0x9B, 0xB2)
CLR_TEXT_MUTE = (0x5B, 0x65, 0x79)
CLR_RED = (0xF8, 0x71, 0x71)

# 固件里的坐标（src/lvgl_s3.ino）
LOCK_W, LOCK_H = 132, 176
LOCK_X, LOCK_Y = (W - LOCK_W) // 2, 2
TXT_Y = 182          # "YYQ 已锁定"
HINT_Y = 206         # "Fn + LOGO 解锁"

CJK = "/System/Library/Fonts/Supplemental/Songti.ttc"
LATIN = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"


def cjk(size):
    return ImageFont.truetype(CJK, size, index=0)


def latin(size):
    return ImageFont.truetype(LATIN, size)


def centered(draw, y, parts, font_cjk, font_lat):
    """parts = [(text, color, is_cjk), ...] 整行水平居中。"""
    widths = []
    for text, _c, is_cjk in parts:
        f = font_cjk if is_cjk else font_lat
        widths.append(draw.textlength(text, font=f))
    total = sum(widths)
    x = (W - total) / 2
    for (text, color, is_cjk), w in zip(parts, widths):
        f = font_cjk if is_cjk else font_lat
        draw.text((x, y), text, font=f, fill=color)
        x += w


def main():
    im = Image.new("RGB", (W, H), CLR_BG)
    d = ImageDraw.Draw(im)

    lock = Image.open(LOCK).convert("RGB")
    im.paste(lock, (LOCK_X, LOCK_Y))

    fc, fl = cjk(16), latin(16)
    centered(d, TXT_Y, [("YYQ ", CLR_TEXT_DIM, False),
                        ("已锁定", CLR_RED, True)], fc, fl)
    centered(d, HINT_Y, [("Fn + LOGO ", CLR_TEXT_MUTE, False),
                         ("解锁", CLR_TEXT_MUTE, True)], fc, fl)

    if "--border" in os.sys.argv:
        d.rectangle([1, 1, W - 2, H - 2], outline=CLR_RED, width=4)

    out = os.path.join(HERE, "assets", "mock_lock.png")
    im.save(out)
    # 放大 3 倍方便肉眼核对
    im.resize((W * 3, H * 3), Image.NEAREST).save(
        os.path.join(HERE, "assets", "mock_lock_3x.png"))
    print("已写出 %s" % out)


if __name__ == "__main__":
    main()
