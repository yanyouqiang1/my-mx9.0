#!/usr/bin/env python3
"""YYQ-MX9.0 壁纸生成器

512x512,极简纯黑风格,对齐固件主屏语言(深黑底 / 白+青文字 / 律动条 / 圆屏表盘)。
画面元素与硬件/固件特色的对应关系:
  圆环表盘   -> ST7789 240x240 圆屏 + 40MHz 差量局部无闪烁引擎(环形进度)
  律动条     -> 背光灯效 / 实时击键监控(按击键强度起伏,色相渐变)
  "YYQ" 主标 -> 作者署名
  "YYQ-MX9.0"-> 固件里的产品名(USB.productName / BLEDevice::init)
  底部行     -> ESP32-S3-N16R8 / BLE 双模 / USB HID / SHT31 温湿度
"""

import colorsys
import math
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 512
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yyq-mx9-wallpaper-512.png")

BG = (4, 7, 10)
INK = (255, 255, 255)
CYAN = (56, 189, 248)          # #38BDF8
DIM = (110, 132, 143)          # 次级文字
FAINT = (58, 76, 86)           # 发丝线 / 灯点底色

MONO = "/System/Library/Fonts/SFNSMono.ttf"


def font(size, weight=None):
    f = ImageFont.truetype(MONO, size)
    if weight:
        try:
            f.set_variation_by_name(weight)
        except Exception:
            pass
    return f


def tracked(draw, xy, text, fnt, fill, tracking, anchor="mm"):
    """按字距逐字排版(SF Mono 没有内置 letter-spacing)。"""
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


# ---------- 底:近纯黑 + 极轻的中心辉光(保持"简约纯黑",只是不呆板) ----------
bg = Image.new("RGB", (S, S), BG)
glow = Image.new("L", (S, S), 0)
ImageDraw.Draw(glow).ellipse((36, 36, S - 36, S - 36), fill=44)
glow = glow.filter(ImageFilter.GaussianBlur(70))
bg = Image.composite(Image.new("RGB", (S, S), (11, 18, 24)), bg, glow)
d = ImageDraw.Draw(bg)

# ---------- 顶部产品名 ----------
tracked(d, (S / 2, 74), "YYQ-MX9.0", font(20), DIM, 4.5)
d.line((S / 2 - 22, 46, S / 2 + 22, 46), fill=(34, 48, 56), width=1)

# ---------- 圆屏表盘 ----------
CX, CY, R = S / 2, 268, 146
d.ellipse((CX - R, CY - R, CX + R, CY + R), outline=(26, 38, 46), width=1)
d.ellipse((CX - R + 9, CY - R + 9, CX + R - 9, CY + R - 9), outline=(15, 23, 29), width=1)

# 环形进度(差量引擎"在转"的暗示)
ARC_R = R - 20
d.arc((CX - ARC_R, CY - ARC_R, CX + ARC_R, CY + ARC_R), start=-90, end=175, fill=(24, 36, 45), width=2)
d.arc((CX - ARC_R, CY - ARC_R, CX + ARC_R, CY + ARC_R), start=-90, end=118, fill=CYAN, width=2)
# 弧端小圆点
for ang, col in ((118, CYAN), (175, (24, 36, 45))):
    a = math.radians(ang)
    d.ellipse((CX + ARC_R * math.cos(a) - 3, CY + ARC_R * math.sin(a) - 3,
               CX + ARC_R * math.cos(a) + 3, CY + ARC_R * math.sin(a) + 3), fill=col)

# ---------- 表盘内:作者署名 + 主标 ----------
tracked(d, (CX, CY - 62), "AUTHOR", font(11), (70, 89, 99), 4.5)
tracked(d, (CX, CY - 2), "YYQ", font(88, "Bold"), INK, 2, anchor="mm")

# ---------- 表盘内:律动条(灯效 / 击键监控) ----------
N, BW, GAP = 13, 7, 7
hs = [8, 18, 28, 15, 36, 23, 10, 29, 20, 33, 13, 21, 8]
span = N * BW + (N - 1) * GAP
x0 = CX - span / 2
base = CY + 76
for i, h in enumerate(hs):
    t = i / (N - 1)
    r, g, b = colorsys.hsv_to_rgb(0.50 + 0.22 * t, 0.70, 1.0)   # 青 -> 蓝紫,守住"纯黑+青"主色
    col = (int(r * 255), int(g * 255), int(b * 255))
    x = x0 + i * (BW + GAP)
    d.rounded_rectangle((x, base - h, x + BW, base), radius=BW // 2, fill=col)

# ---------- 底部硬件签名 ----------
d.line((CX - 150, 432, CX + 150, 432), fill=(20, 30, 37), width=1)
tracked(d, (CX, 456), "ESP32-S3  ·  BLE  ·  HID  ·  SHT31", font(13), DIM, 2.5)

# 底部 19 颗 WS2812 的暗示:一颗实心 + 其余留白
n_led = 19
lx0 = CX - (n_led * 5 + (n_led - 1) * 8) / 2
for i in range(n_led):
    x = lx0 + i * 13
    d.ellipse((x - 2, 479, x + 2, 483), fill=CYAN if i == 0 else (28, 42, 50))

bg.save(OUT)
print("saved:", OUT, bg.size)
