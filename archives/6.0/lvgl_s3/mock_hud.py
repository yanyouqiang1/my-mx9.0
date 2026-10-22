#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HUD 提示（大小两档）的整屏 mock —— 在电脑上先把 240x240 长什么样画出来，
免得烧进去才发现字挤到边上、或者色块盖不住正文。

坐标和 src/lvgl_s3.ino 里的常量**一一对应**（HUD_BIG_W / HUD_BIG_H /
HUD_SMALL_W / HUD_SMALL_H，以及 build_hud() 里的每个坐标）。改了固件那边
也要改这里。

用法：
    /usr/bin/python3 mock_hud.py            # 输出两张图 + 一张新旧对比
"""

import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

W = H = 240

# 和固件里的配色一致（src/lvgl_s3.ino 的 CLR_*）
CLR_BG = (0x0B, 0x0F, 0x17)       # 页面底
CLR_CARD = (0x00, 0x00, 0x00)     # 提示卡底（纯黑）
CLR_TEXT = (0xE6, 0xED, 0xF7)     # 主文字
CLR_RED = (0xF8, 0x71, 0x71)
CLR_AMBER = (0xFB, 0xBF, 0x24)
CLR_GREEN = (0x34, 0xD3, 0x99)
CLR_ACCENT = (0x22, 0xD3, 0xEE)

# ---- 固件常量（src/lvgl_s3.ino 的 build_hud）----
HUD_BIG_W, HUD_BIG_H = 232, 112
HUD_SMALL_W, HUD_SMALL_H = 216, 52
BIG_BORDER = 4
SMALL_BORDER = 2
BAND_H = 8               # 大提示顶部实心横条
BAND_W = HUD_BIG_W - 32  # 和下面的数值块对齐
BLOCK_W = HUD_BIG_W - 32  # 大提示的实心数值块
BLOCK_H = 34
TITLE_W_BIG = HUD_BIG_W - 24
TITLE_W_SMALL = HUD_SMALL_W - 28
BAR_W = 5                # 小提示的左侧竖条

# 16px 中文字体的行高（lv_font_conv 生成的 simhei 16px 差不多是这个值）
LINE_H = 19

CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"


def cjk(size=16, bold=False):
    # index 0 = W3（常规），index 2 = W6（粗）
    return ImageFont.truetype(CJK, size, index=2 if bold else 0)


def center_text(draw, cx, y, text, font, fill):
    w = draw.textlength(text, font=font)
    draw.text((cx - w / 2, y), text, font=font, fill=fill)


def page_background():
    """画一块"主屏"当背景，好判断提示压上去之后看不看得清。"""
    im = Image.new("RGB", (W, H), CLR_BG)
    d = ImageDraw.Draw(im)
    f = cjk(16)
    # 主屏上大致会有的东西：顶部状态行 + 中间的大时钟
    d.text((16, 14), "NUM  CAPS", font=cjk(12), fill=(0x5B, 0x65, 0x79))
    center_text(d, W / 2, 74, "22:48", cjk(48, bold=True), CLR_TEXT)
    center_text(d, W / 2, 132, "10月22日  周三", f, (0x8E, 0x9B, 0xB2))
    d.text((16, 200), "24.3C  48%  1284 keys", font=cjk(12),
           fill=(0x5B, 0x65, 0x79))
    return im


def rounded(d, box, radius, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def draw_big(im, title, value, accent):
    """大提示：232x112，4px 描边 + 顶部实心条 + 数值压在实心色块上（黑字）。"""
    d = ImageDraw.Draw(im)
    x0 = (W - HUD_BIG_W) // 2          # 4
    y0 = (H - HUD_BIG_H) // 2 - 6      # 58  （LV_ALIGN_CENTER, 0, -6）
    rounded(d, [x0, y0, x0 + HUD_BIG_W - 1, y0 + HUD_BIG_H - 1], 10,
            fill=CLR_CARD, outline=accent, width=BIG_BORDER)

    cx = x0 + BIG_BORDER               # 内容区左沿
    cy = y0 + BIG_BORDER
    cw = HUD_BIG_W - BIG_BORDER * 2

    # 顶部实心横条（和下面的数值块同宽）
    bx_band = cx + (cw - BAND_W) // 2
    rounded(d, [bx_band, cy, bx_band + BAND_W - 1, cy + BAND_H - 1], 3,
            fill=accent)

    # 标题：语义色本身
    center_text(d, W / 2, cy + 18, title, cjk(16), accent)

    # 数值：实心色块 + 黑字
    bw = BLOCK_W
    bx = cx + (cw - bw) // 2
    by = cy + (HUD_BIG_H - BIG_BORDER * 2) - 16 - BLOCK_H
    rounded(d, [bx, by, bx + bw - 1, by + BLOCK_H - 1], 6, fill=accent)
    center_text(d, bx + bw / 2, by + (BLOCK_H - LINE_H) / 2, value,
                cjk(16, bold=True), (0, 0, 0))
    return im


def draw_small(im, title, value, accent):
    """小提示：216x52，2px 描边 + 左侧语义色竖条，标题在上、数值在下。"""
    d = ImageDraw.Draw(im)
    x0 = (W - HUD_SMALL_W) // 2        # 12
    y0 = (H - HUD_SMALL_H) // 2 + 40   # 134 （LV_ALIGN_CENTER, 0, 40）
    rounded(d, [x0, y0, x0 + HUD_SMALL_W - 1, y0 + HUD_SMALL_H - 1], 10,
            fill=CLR_CARD, outline=accent, width=SMALL_BORDER)

    cx = x0 + SMALL_BORDER
    cy = y0 + SMALL_BORDER
    cw = HUD_SMALL_W - SMALL_BORDER * 2
    ch = HUD_SMALL_H - SMALL_BORDER * 2

    # 左侧语义色竖条
    d.rectangle([cx + 5, cy + (ch - (HUD_SMALL_H - 14)) // 2,
                 cx + 5 + BAR_W - 1,
                 cy + (ch - (HUD_SMALL_H - 14)) // 2 + (HUD_SMALL_H - 14) - 1],
                fill=accent)

    center_text(d, W / 2, cy + 6, title, cjk(16), accent)
    center_text(d, W / 2, cy + ch - 5 - LINE_H, value, cjk(16), CLR_TEXT)
    return im


def draw_old(im, title, value, accent):
    """旧版 HUD（改造前的样子），只用来做对比图：216x64、2px 描边、
    左侧 5px 竖条、标题是灰的三级色、数值是白色。"""
    d = ImageDraw.Draw(im)
    x0 = (W - 216) // 2
    y0 = (H - 64) // 2 + 30
    rounded(d, [x0, y0, x0 + 215, y0 + 63], 12,
            fill=CLR_CARD, outline=accent, width=2)
    d.rectangle([x0 + 4, y0 + 6, x0 + 8, y0 + 57], fill=accent)
    center_text(d, W / 2, y0 + 9, title, cjk(16), (0x8E, 0x9B, 0xB2))
    center_text(d, W / 2, y0 + 64 - 8 - LINE_H, value, cjk(16), CLR_TEXT)
    return im


def save(im, name):
    out = os.path.join(ASSETS, name)
    im.save(out)
    im.resize((W * 3, H * 3), Image.LANCZOS).save(
        os.path.join(ASSETS, name.replace(".png", "_3x.png")))
    print("已写出 %s" % out)


def main():
    os.makedirs(ASSETS, exist_ok=True)

    # 大提示：错误（红）
    im = page_background()
    draw_big(im, "键盘已锁定", "按键不会发给电脑", CLR_RED)
    save(im, "mock_hud_big.png")

    # 小提示：例行确认（青）
    im = page_background()
    draw_small(im, "按键重映射", "3 条已生效", CLR_ACCENT)
    save(im, "mock_hud_small.png")

    # 新旧对比：左 = 旧版，右 = 新版大提示
    left = page_background()
    draw_old(left, "键盘已锁定", "按键不会发给电脑", CLR_RED)
    right = page_background()
    draw_big(right, "键盘已锁定", "按键不会发给电脑", CLR_RED)
    sheet = Image.new("RGB", (W * 2 + 12, H), (0x05, 0x07, 0x0C))
    sheet.paste(left, (0, 0))
    sheet.paste(right, (W + 12, 0))
    d = ImageDraw.Draw(sheet)
    d.text((10, 8), "OLD", font=cjk(12), fill=(0x8E, 0x9B, 0xB2))
    d.text((W + 22, 8), "NEW", font=cjk(12), fill=(0x8E, 0x9B, 0xB2))
    out = os.path.join(ASSETS, "mock_hud_compare.png")
    sheet.save(out)
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS).save(
        os.path.join(ASSETS, "mock_hud_compare_2x.png"))
    print("已写出 %s" % out)


if __name__ == "__main__":
    main()
