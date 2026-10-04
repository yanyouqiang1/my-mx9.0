#!/usr/bin/env python3
"""YYQ-MX9.0 壁纸:AI 底图 + 矢量文字混合版

底图由 mmx(image-01)生成,只负责氛围和辉光;所有文字一律用 SF Mono 矢量叠加,
保证 "YYQ" / "YYQ-MX9.0" 在 512 下绝对正确——这是纯 AI 出图做不到的。
"""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "mmx", "yyq-dial_001.jpg")
OUT = os.path.join(HERE, "yyq-mx9-wallpaper-512-mmx.png")

MONO = "/System/Library/Fonts/SFNSMono.ttf"
INK = (255, 255, 255)
DIM = (150, 170, 180)
FAINT = (96, 122, 134)


def font(size, weight=None):
    f = ImageFont.truetype(MONO, size)
    if weight:
        try:
            f.set_variation_by_name(weight)
        except Exception:
            pass
    return f


def tracked(draw, xy, text, fnt, fill, tracking, anchor="mm"):
    widths = [draw.textlength(ch, font=fnt) for ch in text]
    total = sum(widths) + tracking * (len(text) - 1)
    x, y = xy
    if anchor[0] == "m":
        x -= total / 2
    elif anchor[0] == "r":
        x -= total
    for ch, w in zip(text, widths):
        draw.text((x, y), ch, font=fnt, fill=fill, anchor="l" + anchor[1])
        x += w + tracking
    return total


im = Image.open(BASE).convert("RGB")
if im.size != (512, 512):
    im = im.resize((512, 512), Image.LANCZOS)
d = ImageDraw.Draw(im)

# 顶部产品名(与底图辉光错开,保持在纯黑区)
tracked(d, (256, 66), "YYQ-MX9.0", font(19), DIM, 4.5)
d.line((234, 40, 278, 40), fill=(58, 82, 94), width=1)

# 表盘内:作者署名 + 主标(落在环形内圈的空白区,不压弧线也不压律动条)
tracked(d, (243, 168), "AUTHOR", font(10), (108, 134, 146), 4.5)
tracked(d, (243, 214), "YYQ", font(62, "Bold"), INK, 1.5, anchor="mm")

# 底部硬件签名
d.line((106, 432, 406, 432), fill=(34, 48, 57), width=1)
tracked(d, (256, 456), "ESP32-S3  ·  BLE  ·  HID  ·  SHT31", font(13), FAINT, 2.5)

im.save(OUT)
print("saved:", OUT, im.size)
