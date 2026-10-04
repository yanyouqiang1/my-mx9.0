#!/usr/bin/env python3
"""
高对比度主屏底部状态条的像素级预览。

所有坐标都照抄 src/lvgl_s3.ino 里 build_style_high_contrast() 的写法，
行盒高度取自各字库的 .line_height，改动布局时两边一起改。
跑：python3 mock_bottom.py
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 240, 240
S = 4                      # 输出放大倍数
OUT = "mock_bottom.png"

# ---- 设计 token（与 lvgl_s3.ino 的 CLR_* 一一对应）----
CLR_BG        = 0x0B0F17
CLR_BG_HC     = 0x000000   # 高对比度风格用纯黑
CLR_SURFACE_2 = 0x1F2739
CLR_STROKE    = 0x2B3549
CLR_TEXT      = 0xE6EDF7
CLR_TEXT_DIM  = 0x8E9BB2
CLR_ACCENT    = 0x22D3EE
CLR_AMBER     = 0xFBBF24
CLR_GREEN     = 0x34D399
CLR_RED       = 0xF87171

def rgb(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)

LATIN = "/System/Library/Fonts/Supplemental/Arial.ttf"
LATIN_B = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
CJK = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"

img = Image.new("RGB", (W, H), rgb(CLR_BG_HC))
d = ImageDraw.Draw(img)

def f(path, size, index=0):
    return ImageFont.truetype(path, size, index=index)

# LVGL 行盒高度：montserrat_48=52 / montserrat_18=21 / simsun_16_cjk=19
# PIL 没有"行盒"概念，这里按行盒中心垂直居中摆放，视觉位置一致。
def ctext(cy, cx, s, font, color, anchor="mm"):
    d.text((cx, cy), s, font=font, fill=rgb(color), anchor=anchor)

# ================= 顶栏 =================
for i, cx in enumerate((30, 74, 118)):
    on = i != 1                                   # 演示：CAPS 灭，其余亮
    c = rgb(CLR_GREEN if i == 0 else CLR_ACCENT if i == 1 else CLR_AMBER)
    if on:
        d.ellipse((cx - 10, 4, cx + 10, 24), fill=c, outline=rgb(CLR_STROKE), width=1)
    else:
        d.ellipse((cx - 10, 4, cx + 10, 24), outline=rgb(CLR_STROKE), width=1)
    name = ("NUM", "CAPS", "SCR")[i]
    ctext(31, cx, name, f(LATIN, 10), CLR_TEXT_DIM if on else CLR_TEXT_DIM)

d.rectangle((206, 0, 238, 32), outline=rgb(CLR_STROKE), width=1)   # 方案图标占位

# ================= 时间 / 日期 / 按键 =================
ctext(46 + 26, 120, "23:47", f(LATIN_B, 44), CLR_TEXT)
ctext(98 + 9,  120, "10月4日 星期日", f(CJK, 16), CLR_TEXT_DIM)
ctext(124 + 26, 120, "Space", f(LATIN_B, 44), CLR_RED)

# ================= 底部状态条 =================
# 框 x=10..230, y=178..235, w=220, h=58, radius 8, 1px CLR_STROKE
d.rounded_rectangle((10, 178, 229, 235), radius=8, outline=rgb(CLR_STROKE), width=1)
# 两区竖分隔线 x=156, y=183..230
d.line((156, 183, 156, 230), fill=rgb(CLR_STROKE), width=1)

# LVGL 行盒：simsun_16_cjk=19 / montserrat_16=18
ROW_H, BAR_H, BAR_W = 19, 6, 135
ROW1_Y, BAR1_Y = 180, 199          # 温度格
ROW2_Y, BAR2_Y = 209, 228          # 湿度格
X0, X1 = 16, 151                   # 小标 / 数值 / 条 共用的左右边缘

def meter(row_y, bar_y, cap, val, color, pct):
    # 小标左对齐、数值右对齐、条满宽 —— 三者左右边缘都是 X0 / X1
    d.text((X0, row_y + ROW_H / 2), cap, font=f(CJK, 16), fill=rgb(color), anchor="lm")
    d.text((X1, row_y + ROW_H / 2), val, font=f(LATIN, 16), fill=rgb(CLR_TEXT), anchor="rm")
    d.rounded_rectangle((X0, bar_y, X0 + BAR_W - 1, bar_y + BAR_H - 1),
                        radius=3, fill=rgb(CLR_SURFACE_2))
    w = int(BAR_W * pct + 0.5)
    if w == 0 and pct > 0:
        w = 1
    if w > 0:
        d.rounded_rectangle((X0, bar_y, X0 + w - 1, bar_y + BAR_H - 1),
                            radius=3, fill=rgb(color))

TEMP, HUM, KEYS = 23.5, 58.0, 128400
meter(ROW1_Y, BAR1_Y, "温度", f"{TEMP:.1f}°C", CLR_AMBER, TEMP / 50.0)
meter(ROW2_Y, BAR2_Y, "湿度", f"{HUM:.0f}%",   CLR_GREEN, HUM / 100.0)

# 字数：formatCountCompact(128400) -> "128.4k"（这格窄，小标在上、数值在下）
ctext(184 + 19 / 2, 192.5, "字数", f(CJK, 16), CLR_ACCENT)
ctext(208 + 18 / 2, 192.5, "128.4k", f(LATIN, 16), CLR_TEXT)

img.resize((W * S, H * S), Image.NEAREST).save(OUT)

# ---- 底部区域放大图（只看状态条）----
crop = img.crop((0, 168, W, 240)).resize((W * S, (240 - 168) * S), Image.NEAREST)
crop.save("mock_bottom_zoom.png")

print(f"写出 {OUT} / mock_bottom_zoom.png")
print(f"温度 {TEMP}°C -> 条宽 {int(BAR_W*TEMP/50+0.5)}/{BAR_W}px")
print(f"湿度 {HUM}%   -> 条宽 {int(BAR_W*HUM/100+0.5)}/{BAR_W}px")
