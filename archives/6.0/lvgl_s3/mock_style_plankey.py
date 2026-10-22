#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
「方案 + 按键」主屏风格的整屏 mock —— 电脑上先把 240x240 长什么样画出来，
烧进板子之前先确认布局对不对。

用法：
    /usr/bin/python3 mock_style_plankey.py
    # 输出 assets/mock_plankey.png（当前=Windows）/ _mac.png（当前=macOS），
    # 以及 _compare.png 对照图

左列的摆法（2026-10-22 用户定稿）：
    **只画当前方案那一个图标** —— 白苹果 = macOS，蓝色 Windows = Windows。
    原来试过"两个都摆、当前点亮另一个压灰"，实测在 240px 屏上分不清哪个是当前，
    压灰那版只剩一团灰影，所以砍掉。图标下面也不再挂「方案」两个字。

坐标系全部是 240x240 的屏幕坐标，和固件里要写的值一一对应。
"""

import os

from PIL import Image, ImageDraw, ImageFont

import gen_style_icons as gi
import lvgl_font_width as fw

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

W = H = 240

# ---- 固件配色（src/lvgl_s3.ino 的 CLR_*）----
CLR_BG = (0x0B, 0x0F, 0x17)
CLR_SURFACE = (0x16, 0x1C, 0x2B)
CLR_SURFACE_2 = (0x1F, 0x27, 0x39)
CLR_STROKE = (0x2B, 0x35, 0x49)
CLR_TEXT = (0xE6, 0xED, 0xF7)
CLR_TEXT_DIM = (0x8E, 0x9B, 0xB2)
CLR_TEXT_MUTE = (0x5B, 0x65, 0x79)
CLR_ACCENT = (0x22, 0xD3, 0xEE)
CLR_VIOLET = (0xA7, 0x8B, 0xFA)
CLR_AMBER = (0xFB, 0xBF, 0x24)
CLR_GREEN = (0x34, 0xD3, 0x99)
CLR_CYAN = CLR_ACCENT

# 字体：固件用 Montserrat(自带) + SimHei 16px(中文)，本机用近似替代
LATIN = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
CJK = "/System/Library/Fonts/STHeiti Medium.ttc"

# ---- 版面（与固件 src/lvgl_s3.ino 的 PK_* 常量一一对应）----
TITLE_H = 28            # 顶栏高
BODY_TOP = 30           # 主体顶
BODY_BOT = 204          # 主体底
LEFT_X = 4              # 左列(方案)左边
LEFT_W = 62             # 左列宽
ICON = 52               # 方案图标边长
ICON_CARD = 61          # 图标后面那块高亮底卡
ICON_CARD_Y = 82        # 底卡顶边（跟按键名 label 的顶边齐平）
ICON_Y = 86             # 图标本体的顶边（底卡 61 - 图标 52，居中 → +4）
CARD_X = 70             # 右卡(按键)左边
CARD_W = 164            # 右卡宽（右边缘 234，和灯组右边缘齐平）
CARD_TOP = 32
CARD_BOT = 200
KEY_Y = 83              # 按键名顶边
LIGHT_W = 38            # 长方形锁灯
LIGHT_H = 14
LIGHT_GAP = 5
BAR_Y1 = 208            # 温度条
BAR_Y2 = 223            # 湿度条
BAR_H = 7               # 条厚
BAR_X0 = 44
BAR_X1 = 176
# 温湿度两个中文标签的顶边。**故意跟条的行距不一样**（15px）：
# 中文字体墨迹 15px 高，两行只差 15px 的话墨迹正好相接、零间隙，
# 看着像"温度湿度粘在一起"。拉到 17px 行距，中间留 2px。
LBL_T_Y = 201
LBL_H_Y = 218

# 按键名的字号候选（固件里 montserrat 开启的档位只有 48/28/24/20/18/16/14/12/10/8）
# 实测字宽见 lvgl_font_width.py：
#   Space@48=148.1  Home@48=149.6  RIGHT@48=154.0  NumLk@48=180.3
#   NumEnt@48=207.0(撑爆) @28=120.7  DOWN@48=173.0(撑爆) @28=100.9
# 所以按"能放下就放最大字号"来选，内宽 152 时绝大多数键都是 48px。
KEY_SIZES = (48, 28, 24)


def latin(size):
    return ImageFont.truetype(LATIN, size)


def cjk(size):
    return ImageFont.truetype(CJK, size)


def rounded(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def draw_title(d, locks=(True, False, False)):
    """顶部一行：左边「YYQ键盘」，右边三颗长方形锁状态灯。

    长方形灯是用户点名要的（不要顶栏那种圆点）：每颗 38x14 的圆角矩形，
    亮起来整颗填语义色（NUM=绿 / CAPS=青 / SCR=琥珀），灭着是暗底灰字。
    顺序从左到右 NUM / CAPS / SCR，和键盘上的物理指示灯一致。

    标题用**一个** label 写完（固件里也用 simsun_16_cjk 一个 label 写
    "YYQ键盘"）：拆成 "YYQ" + "键盘" 两个 label 的话，两种字号的行高不同，
    基线对不齐，中文会比英文高飘几像素。
    """
    fc = cjk(16)
    d.text((LEFT_X, 5), "YYQ键盘", font=fc, fill=CLR_TEXT)
    d.line([0, TITLE_H - 1, W, TITLE_H - 1], fill=CLR_STROKE, width=1)

    # 三颗长方形灯，贴着右边排（顺序 NUM / CAPS / SCR，和键盘上物理灯一致）
    names = ["NUM", "CAPS", "SCR"]
    colors = [CLR_GREEN, CLR_ACCENT, CLR_AMBER]
    ft = latin(10)
    x0 = W - 6 - (3 * LIGHT_W + 2 * LIGHT_GAP)
    for i, (name, color) in enumerate(zip(names, colors)):
        on = locks[i]
        x = x0 + i * (LIGHT_W + LIGHT_GAP)
        y = (TITLE_H - LIGHT_H) // 2
        rounded(d, [x, y, x + LIGHT_W, y + LIGHT_H], 4,
                fill=color if on else CLR_SURFACE_2)
        tw = d.textlength(name, font=ft)
        d.text((x + (LIGHT_W - tw) / 2, y + 1), name, font=ft,
               fill=CLR_BG if on else CLR_TEXT_DIM)


def draw_icons(d, current):
    """左列**只画当前方案那一个**图标。current: 0=Windows 1=macOS

    底卡顶边 82 = 按键名 label 的顶边，图标和那行大字共用一条上边线。
    图标在 61px 底卡里居中（61-52=9 → 上 4 下 5），所以图标顶边 86。
    """
    apple = gi.rasterize(gi.APPLE_SVG, gi.APPLE_VB, ICON, gi.CLR_APPLE)
    win = gi.rasterize(gi.WINDOWS_SVG, gi.WINDOWS_VB, ICON, gi.CLR_WINDOWS)

    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ix = LEFT_X + (LEFT_W - ICON) // 2

    rounded(d, [LEFT_X, ICON_CARD_Y,
                LEFT_X + ICON_CARD - 1, ICON_CARD_Y + ICON_CARD - 1],
            12, fill=CLR_SURFACE_2)
    im.alpha_composite(apple if current == 1 else win, (ix, ICON_Y))
    return im


def draw_key_card(d, key_text, today_count):
    """右卡：按键名 + 右下角今日按键数。"""
    top = BODY_TOP + 2
    bottom = BODY_BOT - 4
    rounded(d, [CARD_X, top, CARD_X + CARD_W, bottom], 12, fill=CLR_SURFACE)

    # 字号自适应：按**LVGL 真实字宽**挑最大的能放下的档位。
    # 这里刻意用 lvgl_font_width 量（直接读字体 .c 的 adv_w），
    # 而不是 PIL 量系统字体 —— 后者跟屏上排版对不上。
    inner = CARD_W - 12
    size = KEY_SIZES[-1]
    for s in KEY_SIZES:
        w, _ = fw.text_width_px(fw.montserrat_path(s), key_text)
        if w <= inner:
            size = s
            break

    f = latin(size)
    tw = d.textlength(key_text, font=f)
    d.text((CARD_X + (CARD_W - tw) / 2, KEY_Y), key_text, font=f, fill=CLR_TEXT)

    # 右下角：今日按键数。
    # 数字固定占 64px 的框右对齐（固件里就是 lv_obj_set_width(64) + 右对齐），
    # "TODAY" 紧贴在那个框左边 —— 这样今天按了 3 次还是 3000 次，
    # "TODAY" 这个词的右边缘都不会跑。
    # 纵向：固件里卡的内容区底边在 199，数字 BOTTOM_RIGHT -8 → 底边 191、
    # 顶边 191-24=167；TODAY BOTTOM_RIGHT -14 → 底边 185、顶边 185-12=173。
    ft = latin(10)
    fv = latin(20)
    num_right = CARD_X + CARD_W - 13      # 固件里是卡片右边缘 -12，卡片有 1px 描边
    valw = d.textlength(str(today_count), font=fv)
    d.text((num_right - valw, CARD_BOT - 33), str(today_count), font=fv, fill=CLR_VIOLET)

    cap = "TODAY"
    capw = d.textlength(cap, font=ft)
    d.text((num_right - 64 - 6 - capw, CARD_BOT - 27), cap, font=ft, fill=CLR_TEXT_MUTE)


def draw_bars(d, temp_c, hum_p):
    """底部：温度 / 湿度 两条窄条。"""
    rows = [("温度", temp_c, 0.0, 50.0, CLR_AMBER),
            ("湿度", hum_p, 0.0, 100.0, CLR_CYAN)]
    fl = latin(14)
    fc = cjk(16)
    capY = [LBL_T_Y, LBL_H_Y]
    for (name, val, lo, hi, color), y, cy in zip(rows, (BAR_Y1, BAR_Y2), capY):
        d.text((LEFT_X, cy), name, font=fc, fill=CLR_TEXT_DIM)
        tx0, tx1 = BAR_X0, BAR_X1
        rounded(d, [tx0, y, tx1, y + BAR_H], BAR_H // 2, fill=CLR_SURFACE_2)
        ratio = 0.0 if hi <= lo else max(0.0, min(1.0, (val - lo) / (hi - lo)))
        fw_ = int((tx1 - tx0) * ratio)
        if fw_ > 4:
            rounded(d, [tx0, y, tx0 + fw_, y + BAR_H], BAR_H // 2, fill=color)
        s = "%.1f°C" % temp_c if name == "温度" else "%.0f%%" % hum_p
        d.text((W - 6 - d.textlength(s, font=fl), y - 5), s, font=fl, fill=CLR_TEXT)


def render(current, key_text="Space", today=1284, temp=23.5, hum=48):
    im = Image.new("RGB", (W, H), CLR_BG)
    d = ImageDraw.Draw(im)
    draw_title(d)
    icons = draw_icons(d, current)
    im = Image.alpha_composite(im.convert("RGBA"), icons).convert("RGB")
    d = ImageDraw.Draw(im)
    draw_key_card(d, key_text, today)
    draw_bars(d, temp, hum)
    return im


def main():
    os.makedirs(ASSETS, exist_ok=True)

    cases = [
        (0, "mock_plankey.png", "当前=Windows"),
        (1, "mock_plankey_mac.png", "当前=macOS"),
    ]
    ims = []
    for cur, fname, desc in cases:
        im = render(cur)
        p = os.path.join(ASSETS, fname)
        im.save(p)
        im.resize((W * 3, H * 3), Image.NEAREST).save(p.replace(".png", "_3x.png"))
        print("%-28s -> %s" % (desc, p))
        ims.append((desc, im))

    # 对照图
    pad = 16
    sheet = Image.new("RGB", (W * len(ims) + pad * (len(ims) + 1), H + pad * 2 + 18), (40, 44, 54))
    f = latin(13)
    x = pad
    for desc, im in ims:
        sheet.paste(im, (x, pad + 18))
        d = ImageDraw.Draw(sheet)
        d.text((x, pad), desc, font=f, fill=(210, 216, 228))
        x += W + pad
    sheet.save(os.path.join(ASSETS, "mock_plankey_compare.png"))
    print("对照图 -> %s" % os.path.join(ASSETS, "mock_plankey_compare.png"))


if __name__ == "__main__":
    main()
