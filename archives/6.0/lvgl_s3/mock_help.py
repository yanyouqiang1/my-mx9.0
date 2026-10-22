#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
按键速查对话框（菜单里的那张速查表）的整屏 mock —— 在电脑上先把 240x240
长什么样画出来，免得烧进去才发现字挤到边上或者列宽不够。

坐标和 src/lvgl_s3.ino 里的常量**一一对应**（HELP_CARD_W / HELP_VISIBLE /
HELP_ROW_H / HELP_KEY_W / HELP_ACT_W …）。改了固件那边也要改这里。

用法：
    /usr/bin/python3 mock_help.py               # 第 1 页
    /usr/bin/python3 mock_help.py --page 2      # 第 2 页
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

W = H = 240

# 和固件里的配色一致（src/lvgl_s3.ino 的 CLR_*）
CLR_BG_DIALOG = (0x05, 0x07, 0x0C)   # 对话框底（比页面底色更深）
CLR_SURFACE = (0x16, 0x1C, 0x2B)    # 卡片面
CLR_SURFACE_2 = (0x1F, 0x27, 0x39)  # 抬升面
CLR_STROKE = (0x2B, 0x35, 0x49)     # 1px 描边
CLR_ACCENT_D = (0x0E, 0x74, 0x90)   # 卡片描边
CLR_TEXT = (0xE6, 0xED, 0xF7)
CLR_TEXT_DIM = (0x8E, 0x9B, 0xB2)
CLR_TEXT_MUTE = (0x5B, 0x65, 0x79)
CLR_ACCENT = (0x22, 0xD3, 0xEE)     # 左列
CLR_AMBER = (0xFB, 0xBF, 0x24)      # 分组标题

# 固件里的常量
CARD_W, CARD_H = 232, 216
CARD_X, CARD_Y = (W - CARD_W) // 2, 12
VISIBLE = 7
ROW_H = 22
KEY_W = 92
ACT_W = 108

LIST_X, LIST_Y = CARD_X + 8, CARD_Y + 32     # 列表容器
CHIP_W, CHIP_H = 60, 18
CHIP_X = CARD_X + CARD_W - 10 - CHIP_W
CHIP_Y = CARD_Y + 8

CJK = "/System/Library/Fonts/Supplemental/Songti.ttc"
LATIN = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"

HEAD = 0
ENTRY = 1

# 和固件里的 helpItems[] 一一对应
ITEMS = [
    (HEAD, "C3 实体键", ""),
    (ENTRY, "静音键 短按", "静音切换"),
    (ENTRY, "静音键 双击", "锁定/解锁键盘"),
    (ENTRY, "静音键 长按", "整机重启"),
    (ENTRY, "灯光键 短按", "切控制目标"),
    (ENTRY, "灯光键 长按", "背光总开关"),
    (ENTRY, "CPG键 短按", "切换灯效"),
    (ENTRY, "CPG键 双击", "重启扩展芯片"),
    (HEAD, "旋钮", ""),
    (ENTRY, "旋转", "调下面的目标"),
    (ENTRY, "按下", "确认/保存"),
    (ENTRY, "键盘背光", "本机亮度"),
    (ENTRY, "屏幕亮度", "主机亮度"),
    (ENTRY, "系统音量", "主机音量"),
    (ENTRY, "灯效", "灯光效果"),
    (HEAD, "矩阵键", ""),
    (ENTRY, "LOGO 短按", "切主屏风格"),
    (ENTRY, "LOGO 长按", "宠物全屏页"),
    (ENTRY, "Fn+LOGO", "解锁键盘"),
    (ENTRY, "MC", "系统菜单"),
    (ENTRY, "MR", "宏录制"),
    (ENTRY, "MA/MB", "全局键"),
    (ENTRY, "Play", "播放/暂停"),
]


def cjk(size):
    return ImageFont.truetype(CJK, size, index=0)


def latin(size):
    return ImageFont.truetype(LATIN, size)


def main():
    page = 1
    if "--page" in sys.argv:
        page = int(sys.argv[sys.argv.index("--page") + 1])
    scroll = (page - 1) * VISIBLE

    im = Image.new("RGB", (W, H), CLR_BG_DIALOG)
    d = ImageDraw.Draw(im)

    # 卡片
    d.rounded_rectangle(
        [CARD_X, CARD_Y, CARD_X + CARD_W - 1, CARD_Y + CARD_H - 1],
        radius=12, fill=CLR_SURFACE, outline=CLR_ACCENT_D, width=1)

    fc, fl = cjk(16), latin(16)

    # 标题
    d.text((CARD_X + 10, CARD_Y + 8), "按键速查", font=fc, fill=CLR_TEXT)

    # 右上角位置胶囊
    d.rounded_rectangle(
        [CHIP_X, CHIP_Y, CHIP_X + CHIP_W - 1, CHIP_Y + CHIP_H - 1],
        radius=9, fill=CLR_SURFACE_2)
    last = min(scroll + VISIBLE, len(ITEMS))
    pos = "%d-%d/%d" % (scroll + 1, last, len(ITEMS))
    fp = latin(12)
    d.text((CHIP_X + (CHIP_W - d.textlength(pos, font=fp)) / 2, CHIP_Y + 2),
           pos, font=fp, fill=CLR_TEXT_DIM)

    # 列表
    for i in range(VISIBLE):
        idx = scroll + i
        if idx >= len(ITEMS):
            break
        kind, key, act = ITEMS[idx]
        y = LIST_Y + i * ROW_H
        if kind == HEAD:
            d.rounded_rectangle([LIST_X + 2, y + 3, LIST_X + 5, y + 15],
                                radius=1, fill=CLR_AMBER)
            d.text((LIST_X + 10, y), key, font=fc, fill=CLR_AMBER)
        else:
            d.text((LIST_X + 10, y), key, font=fc, fill=CLR_ACCENT)
            d.text((LIST_X + 10 + KEY_W + 4, y), act, font=fc, fill=CLR_TEXT)

    # 底部提示
    hint = "旋钮↑↓ 翻看 · ESC 关闭"
    hw = d.textlength(hint, font=fc)
    d.text((LIST_X + (216 - hw) / 2, CARD_Y + CARD_H - 6 - 19),
           hint, font=fc, fill=CLR_TEXT_MUTE)

    out = os.path.join(HERE, "assets", "mock_help.png")
    im.save(out)
    im.resize((W * 3, H * 3), Image.NEAREST).save(
        os.path.join(HERE, "assets", "mock_help_3x.png"))
    print("已写出 %s（第 %d 页）" % (out, page))


if __name__ == "__main__":
    main()
