#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
壁纸主屏（风格 6）+ 屏保 的整屏 mock —— 在电脑上先把 240x240 长什么样画出来。

坐标和 src/lvgl_s3.ino 里的**一一对应**：
  · build_style_wallpaper()  壁纸页：图铺满 + LV_OPA_20 黑纱 + 右下角 112x40 时钟板
  · buildSaverPanel()        屏保信息面板：204x112 卡片 + 48px 时钟 + 日期 + 温湿度
  · enterScreensaver()       SAVER_WALL 时面板贴底（-16），SAVER_INFO 时居中

还会模拟一遍**真实劣化链**：
  壁纸 PNG → 浏览器 JPEG q=0.95 → 固件 JPEGDEC 解成 RGB565 → 240x240 TFT
  这一步不是摆设：细线稿在 q=0.95 + RGB565 之后会掉不少肉，
  在电脑上直接看原 PNG 会对"实机上有多糊"产生错误预期。

用法：
    /usr/bin/python3 mock_saver.py            # 输出 assets/mock_saver*.png
"""

import io
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
WALLPAPER = os.path.join(HERE, "..", "wallpaper", "yyq-mx9-wallpaper-240.png")

W = H = 240

# ---- 固件配色（src/lvgl_s3.ino 的 CLR_*）----
CLR_SURFACE = (0x16, 0x1C, 0x2B)
CLR_SURFACE_2 = (0x1F, 0x27, 0x39)
CLR_STROKE = (0x2B, 0x35, 0x49)
CLR_TEXT = (0xE6, 0xED, 0xF7)
CLR_TEXT_MUTE = (0x5B, 0x65, 0x79)
CLR_AMBER = (0xFB, 0xBF, 0x24)
CLR_ACCENT = (0x22, 0xD3, 0xEE)

# ---- LVGL 的 LV_OPA_* 实际值（lv_color.h）----
LV_OPA_20 = 51
LV_OPA_70 = 178

# ---- 固件常量 ----
# 壁纸页
WP_PLATE_W, WP_PLATE_H = 112, 40
WP_PLATE_X, WP_PLATE_Y = W - 10 - WP_PLATE_W, H - 10 - WP_PLATE_H   # 118, 190
# 屏保面板
SV_PANEL_W, SV_PANEL_H = 204, 112
SV_PANEL_X = (W - SV_PANEL_W) // 2                                    # 18
SV_PANEL_Y_BOTTOM = H - 16 - SV_PANEL_H                              # 112（贴底）
SV_PANEL_Y_CENTER = (H - SV_PANEL_H) // 2                            # 64（居中）

# 字体行高（lv_font_montserrat_*.c / lv_font_simsun_16_cjk.c）
LINE_MON16, LINE_MON28, LINE_MON48, LINE_CJK16 = 18, 30, 52, 19

MONTSERRAT = os.path.join(HERE, ".pio", "libdeps", "esp32-s3-devkitc-1_debug",
                          "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf")
SONGTI = "/System/Library/Fonts/Supplemental/Songti.ttc"


def mono(size):
    return ImageFont.truetype(MONTSERRAT, size)


def cjk(size=16):
    return ImageFont.truetype(SONGTI, size, index=0)


# ============================================================
# 壁纸：模拟真实的 JPEG + RGB565 劣化
# ============================================================
def rgb565(img):
    """把 RGB 缩到 RGB565 —— 固件里就是解成 RGB565 存的。"""
    r, g, b = img.split()
    out = Image.new("RGB", img.size)
    rp = out.load()
    rv, gv, bv = r.load(), g.load(), b.load()
    for y in range(img.height):
        for x in range(img.width):
            c = ((rv[x, y] >> 3) << 11) | ((gv[x, y] >> 2) << 5) | (bv[x, y] >> 3)
            rp[x, y] = (((c >> 11) & 0x1F) << 3) | (((c >> 10) & 0x1F) << 0), \
                       (((c >> 5) & 0x3F) << 2) | (((c >> 4) & 0x3F) << 0), \
                       ((c & 0x1F) << 3) | ((c & 0x1F) >> 2)
    return out


def load_wallpaper_real():
    """PNG → 浏览器 JPEG q=0.95 → RGB565。这是实机上真正显示的那张图。"""
    im = Image.open(WALLPAPER).convert("RGB").resize((W, H), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=95)   # 浏览器里的 0.95 就是 JPEG q95
    buf.seek(0)
    im = Image.open(buf).convert("RGB")
    return rgb565(im)


def apply_scrim(im, opa=LV_OPA_20):
    """全屏压一层黑纱（LV_OPA_20 = 51/255）。"""
    black = Image.new("RGB", im.size, (0, 0, 0))
    return Image.blend(im, black, opa / 255.0)


def rounded(d, box, radius, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def card_with_opa(im, box, radius, bg, opa, border=CLR_STROKE, bw=1):
    """带透明度的卡片。LVGL 是两层：底色层 + 1px 描边层。"""
    x0, y0, x1, y1 = box
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=radius,
                         fill=bg + (opa,), outline=border + (255,), width=bw)
    im.paste(layer, (0, 0), layer)


def label_at(d, cx, top, text, font, fill, line_h):
    """LVGL 的 lv_obj_align(TOP_MID/MID) 语义：文字盒按行高居中。"""
    w = d.textlength(text, font=font)
    y = top + (line_h - font.size) / 2 - 1
    d.text((cx - w / 2, y), text, font=font, fill=fill)


# ============================================================
# 界面 1：壁纸主屏（风格 6）
# ============================================================
def render_wallpaper_style(wp, time_str="22:48"):
    im = wp.copy()
    im = apply_scrim(im, LV_OPA_20)

    # 右下角 112x40 时钟板
    card_with_opa(im, (WP_PLATE_X, WP_PLATE_Y, WP_PLATE_X + WP_PLATE_W,
                       WP_PLATE_Y + WP_PLATE_H),
                  10, CLR_SURFACE_2, LV_OPA_70)
    d = ImageDraw.Draw(im)
    label_at(d, WP_PLATE_X + WP_PLATE_W / 2, WP_PLATE_Y, time_str,
             mono(28), CLR_TEXT, LINE_MON28)
    return im


# ============================================================
# 界面 2：屏保
# ============================================================
def render_saver_panel(im, panel_y, time_str="22:48",
                       date_str="10月22日 星期三",
                       temp_str="24.3C", hum_str="48%"):
    """在给定底图上叠屏保信息面板（buildSaverPanel 的还原）。"""
    x, y = SV_PANEL_X, panel_y
    card_with_opa(im, (x, y, x + SV_PANEL_W, y + SV_PANEL_H), 14,
                  CLR_SURFACE, 255)
    d = ImageDraw.Draw(im)
    cx = x + SV_PANEL_W / 2
    # 时间：48px，TOP_MID +6
    label_at(d, cx, y + 6, time_str, mono(48), CLR_TEXT, LINE_MON48)
    # 日期：中文 16px，TOP_MID +60
    label_at(d, cx, y + 60, date_str, cjk(16), CLR_TEXT_MUTE, LINE_CJK16)
    # 温湿度分隔竖线：1x18，BOTTOM_MID (0,-8)
    d.rectangle([cx - 0.5, y + SV_PANEL_H - 8 - 18, cx + 0.5, y + SV_PANEL_H - 8],
                fill=CLR_STROKE)
    # 温度 / 湿度：16px，BOTTOM_MID ∓52
    label_at(d, cx - 52, y + SV_PANEL_H - 6 - LINE_MON16, temp_str,
             mono(16), CLR_AMBER, LINE_MON16)
    label_at(d, cx + 52, y + SV_PANEL_H - 6 - LINE_MON16, hum_str,
             mono(16), CLR_ACCENT, LINE_MON16)
    return im


def render_saver_wall(wp, **kw):
    """SAVER_WALL：壁纸铺满，面板贴底。"""
    im = wp.copy()
    return render_saver_panel(im, SV_PANEL_Y_BOTTOM, **kw)


def render_saver_info(wp, **kw):
    """SAVER_INFO：纯黑底 + 面板居中。"""
    im = Image.new("RGB", (W, H), (0x0B, 0x0F, 0x17))
    return render_saver_panel(im, SV_PANEL_Y_CENTER, **kw)


def sheet(tiles, cols, pad=10, bg=(24, 26, 32)):
    """拼对照图。每个 tile = (标题, 图)。"""
    rows = (len(tiles) + cols - 1) // cols
    cw, ch = W, H
    sw = cols * cw + pad * (cols + 1)
    sh = rows * (ch + 22) + pad * (rows + 1)
    out = Image.new("RGB", (sw, sh), bg)
    d = ImageDraw.Draw(out)
    f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
    for i, (title, im) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = pad + r * (ch + 22 + pad)
        out.paste(im, (x, y))
        d.rectangle([x - 1, y - 1, x + cw, y + ch], outline=(60, 64, 74))
        d.text((x + 2, y + ch + 4), title, font=f, fill=(190, 195, 205))
    return out


def main():
    wp = load_wallpaper_real()

    tiles = [
        ("wallpaper style (current)", render_wallpaper_style(wp)),
        ("saver WALL panel (current)", render_saver_wall(wp)),
        ("saver INFO panel (current)", render_saver_info(wp)),
        ("raw wallpaper (no scrim)", wp),
    ]
    out = sheet(tiles, 4)
    p = os.path.join(ASSETS, "mock_saver_current.png")
    out.save(p)
    out.resize((out.width * 2, out.height * 2), Image.LANCZOS).save(
        os.path.join(ASSETS, "mock_saver_current_2x.png"))
    print("已写出", p)

    for name, im in [("mock_saver_wallpaper_style.png", render_wallpaper_style(wp)),
                     ("mock_saver_wall.png", render_saver_wall(wp)),
                     ("mock_saver_info.png", render_saver_info(wp))]:
        im.save(os.path.join(ASSETS, name))
        im.resize((W * 3, H * 3), Image.NEAREST).save(
            os.path.join(ASSETS, name.replace(".png", "_3x.png")))


if __name__ == "__main__":
    main()
