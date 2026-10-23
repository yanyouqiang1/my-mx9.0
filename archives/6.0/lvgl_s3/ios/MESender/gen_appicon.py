#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 MESender 的 App 图标（1024x1024，iOS 用）。

风格照着固件那套「简约纯黑」来：
    底色   #0B0F17   (lvgl_s3.ino 的 CLR_BG)
    强调青 #22D3EE   (CLR_ACCENT)
    文字   #E6EDF7   (CLR_TEXT)
    深青   #0E7490   (CLR_ACCENT_D)

图形：上半个键盘 + 下半个"打出去"的箭头 ——
      这 App 的活就是「在手机上打字 → 打到电脑上去」，
      键盘 + 箭头，一个图标说清两件事。

⚠ iOS 的 1024 图标**不能带 alpha**（App Store 会拒），
   所以底色是画上去的实色，最后转成 RGB。

坐标一律在 1024 空间里写，绘制时统一乘 SS 做超采样 ——
   （之前用百分比算坐标，算着算着自己都绕晕了，图形直接飞到屏幕外。）

用法：
    /usr/bin/python3 gen_appicon.py
    输出：MESender/Assets.xcassets/AppIcon.appiconset/AppIcon.png
"""

import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "MESender", "Assets.xcassets", "AppIcon.appiconset")

S = 1024
SS = 4              # 超采样倍数
W = S * SS

BG = (0x0B, 0x0F, 0x17)
ACCENT = (0x22, 0xD3, 0xEE)
ACCENT_D = (0x0E, 0x74, 0x90)
KEY_BG = (0x1C, 0x27, 0x36)
BOARD_BG = (0x12, 0x1A, 0x26)


def sc(*vals):
    """1024 空间 → 超采样空间。"""
    return [v * SS for v in vals]


def draw_icon():
    img = Image.new("RGB", (W, W), BG)

    # ---- 背景一层极淡的青色光晕，别让大色块太死 ----
    glow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse(
        sc(120, 60, 904, 740), fill=(*ACCENT, 30))
    glow = glow.filter(ImageFilter.GaussianBlur(70 * SS))
    img = Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")

    d = ImageDraw.Draw(img)

    # ==================== 键盘 ====================
    KX0, KY0, KX1, KY1 = 150, 150, 874, 566
    d.rounded_rectangle(sc(KX0, KY0, KX1, KY1), radius=32 * SS, fill=BOARD_BG)
    d.rounded_rectangle(sc(KX0, KY0, KX1, KY1), radius=32 * SS,
                        outline=ACCENT_D, width=5 * SS)

    PAD = 44
    GX0, GX1 = KX0 + PAD, KX1 - PAD
    GY0 = KY0 + 42
    ROWS, COLS = 3, 4
    GAP = 16
    KEY_H = 76
    KEY_W = (GX1 - GX0 - (COLS - 1) * GAP) / COLS

    for row in range(ROWS):
        y = GY0 + row * (KEY_H + GAP)
        for col in range(COLS):
            x = GX0 + col * (KEY_W + GAP)
            # 第三行最左边那颗是"发送"键 —— 青色实心，一眼看到"按这个发出去"
            if row == ROWS - 1 and col == 0:
                d.rounded_rectangle(sc(x, y, x + KEY_W, y + KEY_H),
                                    radius=11 * SS, fill=ACCENT)
            else:
                d.rounded_rectangle(sc(x, y, x + KEY_W, y + KEY_H),
                                    radius=11 * SS, fill=KEY_BG,
                                    outline=ACCENT_D, width=2 * SS)

    # 最下面一行：空格键
    y = GY0 + ROWS * (KEY_H + GAP)
    d.rounded_rectangle(sc(GX0 + 40, y, GX1 - 40, y + KEY_H),
                        radius=11 * SS, fill=KEY_BG,
                        outline=ACCENT_D, width=2 * SS)

    # ==================== 发送箭头 ====================
    # 键盘正下方一个朝下的箭头 —— 往下 = 发到电脑上
    AX = 512
    STEM_W = 74
    STEM_TOP = 626
    STEM_BOT = 742
    TRI_H = 104
    TRI_W = 208

    d.rounded_rectangle(sc(AX - STEM_W / 2, STEM_TOP, AX + STEM_W / 2, STEM_BOT),
                        radius=STEM_W / 2 * SS, fill=ACCENT)
    d.polygon([(AX * SS, (STEM_BOT + TRI_H) * SS),
               ((AX - TRI_W / 2) * SS, STEM_BOT * SS),
               ((AX + TRI_W / 2) * SS, STEM_BOT * SS)],
              fill=ACCENT)

    return img.resize((S, S), Image.LANCZOS).convert("RGB")


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    icon = draw_icon()
    path = os.path.join(OUT_DIR, "AppIcon.png")
    icon.save(path, "PNG")   # 已经是 RGB，PNG 不带 alpha
    print("生成：", path, icon.size, "mode=", icon.mode)
