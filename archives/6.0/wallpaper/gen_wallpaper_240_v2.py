#!/usr/bin/env python3
"""YYQ-MX9.0 壁纸 v2：240×240 屏原生版

为什么重做（v1 的问题）：
  v1 的表盘圆心在 (120,106)、R=93 → 弧线最低点 y=199，
  而固件把时钟板放在 x=118..230 / y=190..230，
  **一条连续的圆弧被一个矩形硬生生挖断**——这是"时钟看起来很突兀"的物理来源。
  屏保那边更狠：204×112 的面板贴底，把"YYQ"主标和律动条整片盖住。

v2 的构图规则（固件和图必须一起改才成立）：
  · 表盘整体**上移 + 收小**：圆心 (120,92)、R=76 → 弧线最低 y=168
  · 底部 y=190..240 留**纯黑空场**，不做任何图形元素
    → 固件那一段只要压一层暗带 + 无边框文字，两边就融成一张图
  · 律动条 10 根收窄到 96px 宽（v1 是 124px），贴着圆内沿但不越界
  · "ESP32-S3 · BLE" 签名挪到左下 y=176，不进暗带区

为什么不用 AI 底图（v1 的 gen_wallpaper_240.py 里已经踩过）：
  1024 的图缩到 240，辉光会被 JPEG q95 + RGB565 吃掉，只剩一团脏灰。
  纯黑底 + 矢量描边 = 屏上最实、字节最少。

用法：
    /usr/bin/python3 gen_wallpaper_240_v2.py
"""

import colorsys
import math
import os

from PIL import Image, ImageDraw, ImageFont

S = 240
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "yyq-mx9-wallpaper-240-v2.png")

MONO = "/System/Library/Fonts/SFNSMono.ttf"

BG, INK, CYAN = (0, 0, 0), (255, 255, 255), (56, 189, 248)
DIM, FAINT = (146, 168, 178), (52, 70, 80)
RING, ARC_DIM = (30, 44, 54), (26, 38, 47)
LABEL = (128, 154, 166)

# ---- 构图常量（改这里要同步改固件里的 BAR_H/BAR_Y）----
CX, CY, R = 120, 92, 75          # 表盘圆心 / 外圈半径
AR = R - 10                      # 进度弧半径

# 固件底部通栏条的上边缘。**必须和固件里的 BAR_Y 完全一致**：
#   固件 src/lvgl_s3.ino:  #define BAR_H 72 / #define BAR_Y (240 - BAR_H)
# check_consistency.py 会把两边的值读出来互相校验，对不上直接报错。
BAR_Y = 168


def font(size, weight=None):
    f = ImageFont.truetype(MONO, size)
    if weight:
        try:
            f.set_variation_by_name(weight)
        except Exception:
            pass
    return f


def tracked(d, xy, text, fnt, fill, tr, anchor="mm"):
    ws = [d.textlength(c, font=fnt) for c in text]
    total = sum(ws) + tr * (len(text) - 1)
    x, y = xy
    if anchor[0] == "m":
        x -= total / 2
    elif anchor[0] == "r":
        x -= total
    for c, w in zip(text, ws):
        d.text((x, y), c, font=fnt, fill=fill, anchor="l" + anchor[1])
        x += w + tr


# 底：纯黑（用户明确要纯黑；固件全屏还会再压一层 LV_OPA_20 的黑纱）
bg = Image.new("RGB", (S, S), BG)
d = ImageDraw.Draw(bg)

# 顶部产品名（左上，避开圆顶）
tracked(d, (14, 16), "YYQ-MX9.0", font(9), DIM, 2.4, anchor="lm")

# 表盘外圈
d.ellipse((CX - R, CY - R, CX + R, CY + R), outline=RING, width=2)

# 进度弧：暗色整段 + 青色当前段
d.arc((CX - AR, CY - AR, CX + AR, CY + AR), start=-90, end=172, fill=ARC_DIM, width=3)
d.arc((CX - AR, CY - AR, CX + AR, CY + AR), start=-90, end=112, fill=CYAN, width=3)
for ang, col in ((112, CYAN), (172, ARC_DIM)):
    a = math.radians(ang)
    px = CX + AR * math.cos(a)
    py = CY + AR * math.sin(a)
    d.ellipse((px - 3, py - 3, px + 3, py + 3), fill=col)

# 圆内文字
tracked(d, (CX, 32), "AUTHOR", font(9), LABEL, 2.8)
tracked(d, (CX, 74), "YYQ", font(52, "Bold"), INK, 1)
tracked(d, (CX, 112), "MX9.0", font(12), CYAN, 3.6)

# 律动条（灯效 / 击键监控）：10 根，宽 6 间距 4，总宽 96
hs = [5, 12, 19, 10, 20, 14, 7, 16, 11, 8]
N, BW, GAP = 10, 6, 4
span = N * BW + (N - 1) * GAP
x0 = CX - span / 2
base = 146
for i, h in enumerate(hs):
    t = i / (N - 1)
    r, g, b = colorsys.hsv_to_rgb(0.50 + 0.22 * t, 0.72, 1.0)
    x = x0 + i * (BW + GAP)
    d.rounded_rectangle((x, base - h, x + BW, base), radius=BW // 2,
                        fill=(int(r * 255), int(g * 255), int(b * 255)))

# 硬件签名：**右上角**，和左上角的 "YYQ-MX9.0" 对角呼应。
#
# ⚠ 两个位置都不能放：
#   · 左下（y=156）：表盘圆环左下弧正好穿过那儿（R=75，圆心 120,92），
#     签名会被弧线切一刀，看着像被划掉了
#   · 更下面（y=176）：已经是固件底部通栏条盖住的区域（BAR_Y=168），
#     签名会被半透明黑条压住，像图上有块脏东西
# 右上角是唯一干净的位置：圆环在 x=226 处离圆心 106 > 75，压根不经过。
# check_consistency.py 会实测像素、发现条覆盖区有残留就报错。
tracked(d, (226, 16), "ESP32-S3 · BLE", font(9), FAINT, 1.4, anchor="rm")

bg.save(OUT)
print("saved:", OUT, bg.size)

# 自检：**条盖住的整段**（y=BAR_Y..240）必须一个非黑像素都没有。
# 圆环半径特意取 75（不是 76）：ellipse 画出来最低一行是 cy+r，
# 76 的话那一行正好落在 y=168 = 条的上边缘，会被条压住 1px，
# 看着像弧线被切了一刀。收到 75 就整个待在条上面。
zone = bg.crop((0, BAR_Y, S, S)).convert("L")
assert zone.getbbox() is None, "条覆盖区有残留元素：%s" % (zone.getbbox(),)
print("条覆盖区 y=%d..%d 干净 ✓" % (BAR_Y, S))
