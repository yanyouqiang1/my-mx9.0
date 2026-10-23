#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
最终时钟样式 mock —— 统一「底部通栏条」方案

要解决的"时钟突兀"（本轮核心）：
  v1 壁纸表盘圆心 (120,106)、R=93 → 弧线最低 y=199，
  固件把时钟板放在 x=118..230 / y=190..230，
  **一条连续的圆弧被一个带 1px 描边的圆角矩形硬生生挖断**。
  屏保那边更糟：204x112 不透明面板贴底，把"YYQ"主标整片盖住，
  而且每 5 秒"啪"地盖上又消失。

为什么最后选「底部通栏条」而不是小卡片 / 纯裸字：
  · 小卡片（v1）：圆角 + 描边 + 浮在图中间 → 像张便利贴，突兀
  · 纯裸字：用户上传**亮色壁纸**时完全看不清（网页端支持传任意图）
  · 底部通栏条：无描边无圆角，是"界面结构"而不是"贴上去的控件"，
    在暗色图上就是黑底白字、在亮色图上是一条半透明黑带，两种图都成立

这个脚本同时验证三件事：
  1. 新壁纸 v2 的构图能不能和固件的底部条严丝合缝对上
  2. 底部条在**亮色壁纸**上是不是还读得清（用户可以传任意图）
  3. 旧方案 vs 新方案的观感差多少

用法：
    /usr/bin/python3 mock_final.py
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

LV_OPA_20, LV_OPA_60, LV_OPA_70 = 51, 153, 178

MONTSERRAT = os.path.join(HERE, ".pio", "libdeps", "esp32-s3-devkitc-1_debug",
                          "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf")
SONGTI = "/System/Library/Fonts/Supplemental/Songti.ttc"

LINE_MON16, LINE_MON28, LINE_MON48, LINE_CJK16 = 18, 30, 52, 19

# ---- 固件常量（新方案，与 src/lvgl_s3.ino 的 BAR_H/BAR_Y 严格一致）----
BAR_H = 72
BAR_Y = H - BAR_H            # 168：正好接住壁纸表盘的弧线最低行 y=167
BAR_X, BAR_W = 0, 240

# 标签定位（和固件里的 lv_obj_set_pos 一致）
TIME_X, TIME_Y = 14, 8
DATE_X, DATE_Y = 106, 12
TEMP_X, TEMP_Y = 134, 38
HUM_X, HUM_Y = 190, 38

# 旧方案常量（对照用）
WP_PLATE_W, WP_PLATE_H = 112, 40
WP_PLATE_X, WP_PLATE_Y = W - 10 - WP_PLATE_W, H - 10 - WP_PLATE_H
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


def load_wallpaper(path, jpeg=True):
    im = Image.open(path).convert("RGB").resize((W, H), Image.LANCZOS)
    if not jpeg:
        return im
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


def bar_with_opa(im, box, bg, opa):
    """底部通栏条：纯色、无描边、无圆角（这是它不"突兀"的关键）。"""
    x0, y0, x1, y1 = box
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rectangle([x0, y0, x1 - 1, y1 - 1], fill=bg + (opa,))
    im.paste(layer, (0, 0), layer)


def label_at(d, cx, top, text, font, fill, line_h):
    w = d.textlength(text, font=font)
    y = top + (line_h - font.size) / 2 - 1
    d.text((cx - w / 2, y), text, font=font, fill=fill)


def label_right(d, right_x, top, text, font, fill, line_h):
    w = d.textlength(text, font=font)
    y = top + (line_h - font.size) / 2 - 1
    d.text((right_x - w, y), text, font=font, fill=fill)


def scrim(im, opa=LV_OPA_20):
    return Image.blend(im, Image.new("RGB", im.size, (0, 0, 0)), opa / 255.0)


# ============================================================
# 新方案：底部通栏条
# ============================================================
def bar_time(im, with_sht=False, bar_opa=LV_OPA_60):
    """底部条 + 时间 + 日期 +（可选）温湿度。

    坐标和固件 build_style_wallpaper / buildSaverPanel 里的
    lv_obj_set_pos 完全一致（见 check_consistency.py）。
    """
    bar_with_opa(im, (BAR_X, BAR_Y, BAR_X + BAR_W, H), (0x00, 0x00, 0x00), bar_opa)
    d = ImageDraw.Draw(im)
    label_at(d, TIME_X + 75.3 / 2, BAR_Y + TIME_Y, "22:48", mono(28), CLR_TEXT, LINE_MON28)
    label_right(d, DATE_X + 120.0, BAR_Y + DATE_Y, "10月22日 星期三",
                cjk(16), CLR_TEXT_MUTE, LINE_CJK16)
    if with_sht:
        label_right(d, TEMP_X + 44.2, BAR_Y + TEMP_Y, "24.3C",
                    mono(16), CLR_AMBER, LINE_MON16)
        label_right(d, HUM_X + 34.5, BAR_Y + HUM_Y, "48%",
                    mono(16), CLR_ACCENT, LINE_MON16)
    return im


def new_wallpaper_page(wp):
    """主屏风格 6：壁纸 + 全屏黑纱 + 底部条（时间 + 日期）。"""
    im = scrim(wp.copy())
    return bar_time(im, with_sht=False)


def new_saver(wp):
    """屏保 SAVER_WALL：壁纸 + 底部条（时间 + 日期 + 温湿度），不遮主标。"""
    im = wp.copy()
    return bar_time(im, with_sht=True, bar_opa=LV_OPA_70)


# ============================================================
# 旧方案（对照）
# ============================================================
def old_wallpaper_page(wp):
    im = scrim(wp.copy())
    card_with_opa(im, (WP_PLATE_X, WP_PLATE_Y, WP_PLATE_X + WP_PLATE_W,
                       WP_PLATE_Y + WP_PLATE_H), 10, CLR_SURFACE_2, LV_OPA_70)
    d = ImageDraw.Draw(im)
    label_at(d, WP_PLATE_X + WP_PLATE_W / 2, WP_PLATE_Y, "22:48",
             mono(28), CLR_TEXT, LINE_MON28)
    return im


def old_saver(wp):
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


def bright_wallpaper():
    """模拟用户上传的**亮色**壁纸 —— 用来验底部条在亮图上还读不读得清。"""
    im = Image.new("RGB", (W, H), (198, 206, 214))
    d = ImageDraw.Draw(im)
    for i in range(0, W, 24):
        d.rectangle([i, 0, i + 12, H], fill=(168, 178, 190))
    d.ellipse((20, 10, 220, 210), fill=(232, 236, 240))
    d.ellipse((60, 50, 180, 170), fill=(120, 190, 215))
    return rgb565(im)


def sheet(tiles, cols, pad=12, bg=(24, 26, 32)):
    rows = (len(tiles) + cols - 1) // cols
    sw = cols * W + pad * (cols + 1)
    sh = rows * (H + 40) + pad * (rows + 1)
    out = Image.new("RGB", (sw, sh), bg)
    d = ImageDraw.Draw(out)
    f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 12)
    for i, (title, im) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = pad + c * (W + pad)
        y = pad + r * (H + 40 + pad)
        out.paste(im, (x, y))
        d.rectangle([x - 1, y - 1, x + W, y + H], outline=(60, 64, 74))
        # 标题可能很长，折成两行，别让它撞到下一列
        words, line, lines = title.split(" "), "", []
        for wd in words:
            trial = (line + " " + wd).strip()
            if d.textlength(trial, font=f) > W - 4 and line:
                lines.append(line)
                line = wd
            else:
                line = trial
        if line:
            lines.append(line)
        for li, ln in enumerate(lines[:2]):
            d.text((x + 2, y + H + 6 + li * 14), ln, font=f, fill=(200, 205, 215))
    return out


def main():
    v1 = load_wallpaper(WALL_V1)
    v2 = load_wallpaper(WALL_V2)
    bright = bright_wallpaper()

    tiles = [
        ("OLD: wallpaper page (v1 art + bordered card)", old_wallpaper_page(v1)),
        ("OLD: saver (204x112 panel covers the art)", old_saver(v1)),
        ("NEW: wallpaper page (v2 art + bottom bar)", new_wallpaper_page(v2)),
        ("NEW: saver (v2 art + bottom bar)", new_saver(v2)),
        ("NEW on a BRIGHT user wallpaper", new_wallpaper_page(bright)),
        ("raw v2 art (bottom 72px reserved)", v2),
    ]
    out = sheet(tiles, 3)
    p = os.path.join(ASSETS, "mock_final.png")
    out.save(p)
    out.resize((out.width * 2, out.height * 2), Image.LANCZOS).save(
        os.path.join(ASSETS, "mock_final_2x.png"))
    print("已写出", p)


if __name__ == "__main__":
    main()
