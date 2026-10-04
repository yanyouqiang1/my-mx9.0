#!/usr/bin/env python3
"""YYQ-MX9.0 壁纸：240×240 屏原生版

为什么单独出一版 240：
  网页端把上传的图缩到 240×240 再压 JPEG。喂 512 的图过来会被重采样两次
  （浏览器缩一次、JPEG 压一次），而且 512 构图为了在桌面看舒服留的边
  （内容只占中间 60%）在 240 的小屏上就变成"一小块东西 + 一圈黑"。
  直接给 240 的源图 = 画布 1:1 落进去，不重采样，细节最实。

版面要避让的东西（都在固件里，不是猜的）：
  · 全屏压一层 LV_OPA_20 的黑纱 → 底色别用死黑，文字要够亮
  · 右下角 112×40 半透明时钟板（x 118..230 / y 190..230）→ 不放关键信息
  · lv_img 铺满 240×240，没有黑边裁切
"""

import colorsys
import math
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 240
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yyq-mx9-wallpaper-240.png")

MONO = "/System/Library/Fonts/SFNSMono.ttf"
BG, INK, CYAN = (0, 0, 0), (255, 255, 255), (56, 189, 248)
DIM, FAINT = (146, 168, 178), (52, 70, 80)
RING, HAIR = (30, 44, 54), (22, 32, 40)

# 笔画全部 >= 2px。1px 细线在 240 的 TFT 上有两个问题：面板本身解析不出
# 1px 的稳定宽度，而 JPEG 编码会把这种高频边缘量化成毛刺。两条叠加就是"糊"。
CX, CY, R = 120, 106, 93          # 表盘圆心 / 半径（几乎顶满宽度）


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
    for c, w in zip(text, ws):
        d.text((x, y), c, font=fnt, fill=fill, anchor="l" + anchor[1])
        x += w + tr


# 底：纯黑。用户明确要纯黑，之前那圈中心辉光去掉。
# 固件自己还会压一层 LV_OPA_20 的黑纱 —— 黑乘黑仍是黑，所以底色不会因此发灰，
# 但白字会被压到 ~80% 灰度，那是固件行为，图里改不掉。
bg = Image.new("RGB", (S, S), BG)
d = ImageDraw.Draw(bg)

# 顶部产品名
tracked(d, (14, 16), "YYQ-MX9.0", font(10), DIM, 2.4, anchor="lm")

# 表盘：外圈 + 进度弧（差量引擎在转）
d.ellipse((CX - R, CY - R, CX + R, CY + R), outline=RING, width=2)
AR = R - 12
d.arc((CX - AR, CY - AR, CX + AR, CY + AR), start=-90, end=172, fill=(26, 38, 47), width=3)
d.arc((CX - AR, CY - AR, CX + AR, CY + AR), start=-90, end=112, fill=CYAN, width=3)
for ang, col in ((112, CYAN), (172, (26, 38, 47))):
    a = math.radians(ang)
    d.ellipse((CX + AR * math.cos(a) - 3, CY + AR * math.sin(a) - 3,
               CX + AR * math.cos(a) + 3, CY + AR * math.sin(a) + 3), fill=col)

# 表盘内：作者署名 + 主标 + 型号
tracked(d, (CX, CY - 60), "AUTHOR", font(9), (128, 154, 166), 2.8)
tracked(d, (CX, CY - 14), "YYQ", font(58, "Bold"), INK, 1)
tracked(d, (CX, CY + 30), "MX9.0", font(14), CYAN, 3.6)

# 律动条（灯效 / 击键监控）——10 根，贴着圆内沿收窄，末端不会被圆边切掉
hs = [6, 15, 23, 12, 25, 18, 8, 21, 13, 9]
N, BW, GAP = 10, 7, 6
span = N * BW + (N - 1) * GAP
x0 = CX - span / 2
base = CY + 68
for i, h in enumerate(hs):
    t = i / (N - 1)
    r, g, b = colorsys.hsv_to_rgb(0.50 + 0.22 * t, 0.72, 1.0)
    x = x0 + i * (BW + GAP)
    d.rounded_rectangle((x, base - h, x + BW, base), radius=BW // 2,
                        fill=(int(r * 255), int(g * 255), int(b * 255)))

# 底部硬件签名：贴左下角。长度要卡住 —— 右下角那块板从 x=118 起，
# 文字超过 ~100px 宽就会钻到板子底下变成"半截字"。
tracked(d, (14, S - 15), "ESP32-S3 · BLE", font(9), FAINT, 1.4, anchor="lm")

bg.save(OUT)
print("saved:", OUT, bg.size)
