#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
时钟样式方案对照 —— 壁纸主屏（风格 6）+ 屏保

背景（这是本轮要解决的"时钟突兀"）：
  v1 壁纸的表盘圆心 (120,106)、R=93 → 弧线最低点 y=199，
  而固件把时钟板放在 x=118..230 / y=190..230，
  **一条连续的圆弧被一个带 1px 描边的圆角矩形硬生生挖断**。
  屏保那边更糟：204x112 的不透明面板贴底，把"YYQ"主标整片盖住，
  而且每 5 秒"啪"地盖上又消失（updateScreensaver 里的 tick 翻转）。

所以这一版出三套时钟方案，用同一张新壁纸 v2 渲染，看哪套最不打扰画面：

  A. 现状（v1 壁纸 + 112x40 描边卡片）      —— 对照组
  B. 无板无描边，纯文字压在图上              —— 最安静，但可读性依赖图本身够暗
  C. 底部渐变暗带 + 文字（推荐）              —— 暗带和壁纸的黑色融为一体，
                                                文字始终可读，又不切弧线

用法：
    /usr/bin/python3 mock_clock_style.py
"""

import io
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
WALL_V1 = os.path.join(HERE, "..", "wallpaper", "yyq-mx9-wallpaper-240.png")
WALL_V2 = os.path.join(HERE, "..", "wallpaper", "yyq-mx9-wallpaper-240-v2.png")

W = H = 240

CLR_SURFACE = (0x16, 0x1C, 0x2B)
CLR_SURFACE_2 = (0x1F, 0x27, 0x39)
CLR_STROKE = (0x2B, 0x35, 0x49)
CLR_TEXT = (0xE6, 0xED, 0xF7)
CLR_TEXT_MUTE = (0x5B, 0x65, 0x79)
CLR_AMBER = (0xFB, 0xBF, 0x24)
CLR_ACCENT = (0x22, 0xD3, 0xEE)

LV_OPA_20 = 51
LV_OPA_70 = 178

MONTSERRAT = os.path.join(HERE, ".pio", "libdeps", "esp32-s3-devkitc-1_debug",
                          "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf")
SONGTI = "/System/Library/Fonts/Supplemental/Songti.ttc"

LINE_MON16, LINE_MON28, LINE_MON48, LINE_CJK16 = 18, 30, 52, 19

# v1 固件常量
WP_PLATE_W, WP_PLATE_H = 112, 40
WP_PLATE_X, WP_PLATE_Y = W - 10 - WP_PLATE_W, H - 10 - WP_PLATE_H   # 118,190
SV_PANEL_W, SV_PANEL_H = 204, 112
SV_PANEL_X = (W - SV_PANEL_W) // 2
SV_PANEL_Y_BOTTOM = H - 16 - SV_PANEL_H


def mono(size):
    return ImageFont.truetype(MONTSERRAT, size)


def cjk(size=16):
    return ImageFont.truetype(SONGTI, size, index=0)


def rgb565(img):
    r, g, b = img.split()
    out = Image.new("RGB", img.size)
    rp, rv, gv, bv = out.load(), r.load(), g.load(), b.load()
    for y in range(img.height):
        for x in range(img.width):
            c = ((rv[x, y] >> 3) << 11) | ((gv[x, y] >> 2) << 5) | (bv[x, y] >> 3)
            rp[x, y] = (((c >> 11) & 0x1F) << 3), (((c >> 5) & 0x3F) << 2), ((c & 0x1F) << 3)
    return out


def load_wallpaper(path):
    """PNG → 浏览器 JPEG q95 → RGB565。这是实机上真正显示的那张图。"""
    im = Image.open(path).convert("RGB").resize((W, H), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=95)
    buf.seek(0)
    return rgb565(Image.open(buf).convert("RGB"))


def card_with_opa(im, box, radius, bg, opa, border=CLR_STROKE, bw=1):
    x0, y0, x1, y1 = box
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=radius,
                         fill=bg + (opa,), outline=border + (255,), width=bw)
    im.paste(layer, (0, 0), layer)


def label_at(d, cx, top, text, font, fill, line_h):
    w = d.textlength(text, font=font)
    y = top + (line_h - font.size) / 2 - 1
    d.text((cx - w / 2, y), text, font=font, fill=fill)


def scrim(im, opa=LV_OPA_20):
    return Image.blend(im, Image.new("RGB", im.size, (0, 0, 0)), opa / 255.0)


def gradient_band(im, top, height, max_opa=200, color=(0, 0, 0)):
    """底部渐变暗带：越往下越黑。

    固件里用 LVGL 的 bg_grad_dir=LV_GRAD_DIR_VER 实现不了"透明→黑"的过渡
    （LVGL 8.4 的渐变两端的颜色不带 alpha 通道），所以这里用一叠
    递增透明度的横条来模拟 —— 固件那边就是照这个叠的。
    """
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    steps = height
    for i in range(steps):
        a = int(max_opa * (i + 1) / steps)
        ld.rectangle([0, top + i, W, top + i], fill=color + (a,))
    im.paste(layer, (0, 0), layer)


# ============================================================
# 方案 A：现状（v1 壁纸 + 描边卡片）
# ============================================================
def style_a(wp):
    im = scrim(wp.copy())
    card_with_opa(im, (WP_PLATE_X, WP_PLATE_Y, WP_PLATE_X + WP_PLATE_W,
                       WP_PLATE_Y + WP_PLATE_H), 10, CLR_SURFACE_2, LV_OPA_70)
    d = ImageDraw.Draw(im)
    label_at(d, WP_PLATE_X + WP_PLATE_W / 2, WP_PLATE_Y, "22:48",
             mono(28), CLR_TEXT, LINE_MON28)
    return im


# ============================================================
# 方案 B：无板无描边，纯文字
# ============================================================
def style_b(wp):
    im = scrim(wp.copy())
    d = ImageDraw.Draw(im)
    label_at(d, WP_PLATE_X + WP_PLATE_W / 2, WP_PLATE_Y, "22:48",
             mono(28), CLR_TEXT, LINE_MON28)
    return im


# ============================================================
# 方案 C：底部渐变暗带 + 文字
# ============================================================
def style_c(wp):
    im = wp.copy()
    gradient_band(im, 176, 64, max_opa=210)
    d = ImageDraw.Draw(im)
    label_at(d, W / 2, 200, "22:48", mono(28), CLR_TEXT, LINE_MON28)
    return im


# ============================================================
# 屏保：v1 的 204x112 面板 vs v2 的底部信息带
# ============================================================
def saver_old(wp):
    im = wp.copy()
    x, y = SV_PANEL_X, SV_PANEL_Y_BOTTOM
    card_with_opa(im, (x, y, x + SV_PANEL_W, y + SV_PANEL_H), 14, CLR_SURFACE, 255)
    d = ImageDraw.Draw(im)
    cx = x + SV_PANEL_W / 2
    label_at(d, cx, y + 6, "22:48", mono(48), CLR_TEXT, LINE_MON48)
    label_at(d, cx, y + 60, "10月22日 星期三", cjk(16), CLR_TEXT_MUTE, LINE_CJK16)
    d.rectangle([cx - 0.5, y + SV_PANEL_H - 26, cx + 0.5, y + SV_PANEL_H - 8],
                fill=CLR_STROKE)
    label_at(d, cx - 52, y + SV_PANEL_H - 24, "24.3C", mono(16), CLR_AMBER, LINE_MON16)
    label_at(d, cx + 52, y + SV_PANEL_H - 24, "48%", mono(16), CLR_ACCENT, LINE_MON16)
    return im


def saver_new(wp):
    """v2 屏保：壁纸铺满 + 底部渐变暗带 + 一行紧凑信息。

    面板不再盖住主标，信息沿底部排成一行，视觉重心仍在图上。
    """
    im = wp.copy()
    gradient_band(im, 168, 72, max_opa=215)
    d = ImageDraw.Draw(im)
    label_at(d, 62, 176, "22:48", mono(28), CLR_TEXT, LINE_MON28)
    label_at(d, 168, 178, "10月22日 星期三", cjk(16), CLR_TEXT_MUTE, LINE_CJK16)
    label_at(d, 196, 214, "24.3C", mono(16), CLR_AMBER, LINE_MON16)
    label_at(d, 228, 214, "48%", mono(16), CLR_ACCENT, LINE_MON16)
    return im


def sheet(tiles, cols, pad=12, bg=(24, 26, 32)):
    rows = (len(tiles) + cols - 1) // cols
    cw, ch = W, H
    sw = cols * cw + pad * (cols + 1)
    sh = rows * (ch + 26) + pad * (rows + 1)
    out = Image.new("RGB", (sw, sh), bg)
    d = ImageDraw.Draw(out)
    f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 14)
    for i, (title, im) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = pad + r * (ch + 26 + pad)
        out.paste(im, (x, y))
        d.rectangle([x - 1, y - 1, x + cw, y + ch], outline=(60, 64, 74))
        d.text((x + 2, y + ch + 6), title, font=f, fill=(200, 205, 215))
    return out


def main():
    v1 = load_wallpaper(WALL_V1)
    v2 = load_wallpaper(WALL_V2)

    tiles = [
        ("A current (v1 art + bordered card)", style_a(v1)),
        ("B no plate, bare text on v2 art", style_b(v2)),
        ("C bottom gradient band + text (v2 art)", style_c(v2)),
        ("saver OLD: 204x112 panel over art", saver_old(v1)),
        ("saver NEW: bottom band (v2 art)", saver_new(v2)),
        ("raw v2 art (no clock)", v2),
    ]
    out = sheet(tiles, 3)
    p = os.path.join(ASSETS, "mock_clock_style.png")
    out.save(p)
    out.resize((out.width * 2, out.height * 2), Image.LANCZOS).save(
        os.path.join(ASSETS, "mock_clock_style_2x.png"))
    print("已写出", p)


if __name__ == "__main__":
    main()
