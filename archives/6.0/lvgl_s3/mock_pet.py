#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
键盘宠物（scr_pet）像素级预览 —— 三个姿态并排。

坐标约定全部按 LVGL 能画出来的东西来：只用圆（radius=CIRCLE）、
圆角矩形、线段、label。**没有三角形**（不能用 transform_angle），
所以耳朵画成圆的——这也正是固件里能 1:1 复刻的形状。

跑：python3 mock_pet.py
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 240, 240
S = 3
GAP = 12
POSES = ["idle", "greet", "hungry"]

# ---- 设计 token（与 lvgl_s3.ino 的 CLR_* 一一对应）----
CLR_BG        = 0x0B0F17
CLR_SURFACE_2 = 0x1F2739
CLR_STROKE    = 0x2B3549
CLR_TEXT      = 0xE6EDF7
CLR_TEXT_DIM  = 0x8E9BB2
CLR_ACCENT    = 0x22D3EE
CLR_AMBER     = 0xFBBF24
CLR_GREEN     = 0x34D399
CLR_PET       = 0xF5A97F   # 宠物主色（橘猫）
CLR_PET_DARK  = 0xC4805F   # 耳朵内侧 / 纹路
CLR_PET_BODY  = 0xE8C39E

LATIN   = "/System/Library/Fonts/Supplemental/Arial.ttf"
LATIN_B = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
CJK     = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"


def rgb(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)


# 三屏并排画布
img = Image.new("RGB", (W * len(POSES) + GAP * (len(POSES) - 1), H), rgb(0x05070C))


def new_screen(pose):
    """一块独立的 240x240 屏，返回 (img, draw)。"""
    im = Image.new("RGB", (W, H), rgb(CLR_BG))
    return im, ImageDraw.Draw(im)


def f(path, size):
    return ImageFont.truetype(path, size)


# ---------- 宠物本体 ----------
def draw_pet(d, cx, cy, pose):
    """cx,cy = 身体中心。整体约 110x112。全部基本图形，无三角。"""
    pet, dark, body = rgb(CLR_PET), rgb(CLR_PET_DARK), rgb(CLR_PET_BODY)

    # 影子（ellipse 压扁）——固件里用 shadow_opa 当光晕/影子，这层是可行的
    d.ellipse((cx - 40, cy + 48, cx + 40, cy + 62), fill=rgb(0x000000))

    # 尾巴：短线段（pose=greet 时翘起来）
    if pose == "greet":
        d.line((cx + 32, cy + 36, cx + 48, cy + 14), fill=pet, width=6)
    else:
        d.line((cx + 32, cy + 36, cx + 48, cy + 40), fill=pet, width=6)

    # 身体：圆角矩形 68x40
    d.rounded_rectangle((cx - 34, cy + 8, cx + 34, cy + 48), radius=19, fill=body)

    # 耳朵：两个圆（固件可画；想换尖耳就用 lv_line 三角）
    d.ellipse((cx - 37, cy - 48, cx - 11, cy - 22), fill=pet)
    d.ellipse((cx + 11, cy - 48, cx + 37, cy - 22), fill=pet)
    d.ellipse((cx - 31, cy - 41, cx - 19, cy - 29), fill=dark)
    d.ellipse((cx + 19, cy - 41, cx + 31, cy - 29), fill=dark)

    # 头：圆 r=34
    d.ellipse((cx - 34, cy - 43, cx + 34, cy + 25), fill=pet)

    # 额头纹路：三条短线
    for i in range(3):
        x = cx - 8 + i * 8
        d.line((x, cy - 29, x, cy - 19), fill=dark, width=2)

    # 眼睛
    if pose == "greet":
        # 开心眼：两段弧（用短线拼 ^ ^）
        for sx in (-1, 1):
            ex = cx + sx * 14
            d.line((ex - 7, cy + 3, ex, cy - 4), fill=rgb(0x1A1A1A), width=3)
            d.line((ex, cy - 4, ex + 7, cy + 3), fill=rgb(0x1A1A1A), width=3)
    else:
        for sx in (-1, 1):
            ex = cx + sx * 14
            d.ellipse((ex - 6, cy - 10, ex + 6, cy + 2), fill=rgb(0x1A1A1A))
            if pose == "hungry":       # 无神：瞳孔上半遮住
                d.ellipse((ex - 6, cy - 10, ex + 6, cy - 4), fill=pet)

    # 鼻子 + 嘴
    d.polygon([(cx - 4, cy + 8), (cx + 4, cy + 8), (cx, cy + 12)], fill=rgb(0xD2694A))
    if pose == "hungry":
        d.arc((cx - 11, cy + 10, cx + 11, cy + 24), 200, 340, fill=rgb(0x1A1A1A), width=2)
    else:
        d.arc((cx - 12, cy + 5, cx, cy + 17), 0, 110, fill=rgb(0x1A1A1A), width=2)
        d.arc((cx, cy + 5, cx + 12, cy + 17), 70, 180, fill=rgb(0x1A1A1A), width=2)

    # 腮红
    blush = rgb(0xE8897A) if pose == "greet" else rgb(0xE0A090)
    d.ellipse((cx - 30, cy + 8, cx - 18, cy + 15), fill=blush)
    d.ellipse((cx + 18, cy + 8, cx + 30, cy + 15), fill=blush)

    # 胡须：从口鼻两侧往外挑
    for sx in (-1, 1):
        for k in (-1, 0, 1):
            d.line((cx + sx * 18, cy + 11 + k * 3,
                    cx + sx * 36, cy + 8 + k * 5), fill=rgb(0x8A6A55), width=1)


# ---------- 三个姿态的屏 ----------
def build(pose, name, mood, lines, hint):
    im, d = new_screen(pose)

    # 顶部状态条：宠物名 + 心情条
    d.text((12, 6), name, font=f(CJK, 16), fill=rgb(CLR_TEXT), anchor="la")
    d.rounded_rectangle((100, 9, 228, 19), radius=5, fill=rgb(CLR_SURFACE_2))
    w = int(128 * mood)
    if w > 0:
        col = CLR_GREEN if mood > 0.6 else (CLR_AMBER if mood > 0.3 else CLR_RED)
        d.rounded_rectangle((100, 9, 100 + w - 1, 19), radius=5, fill=rgb(col))
    d.line((12, 28, 228, 28), fill=rgb(CLR_STROKE), width=1)

    # 宠物本体（中心 y=104，头顶 56 / 影子底 166）
    draw_pet(d, 120, 104, pose)

    # 对话气泡：x=12..228, w=216
    #   行盒 simsun_16_cjk = 19px，216/16 = 每行最多 13 个汉字
    #   竖向预算：气泡 168..218，提示行 219..238 —— 所以台词最多 2 行
    top = 168
    h = 6 + 19 * len(lines) + 5
    d.rounded_rectangle((12, top, 228, top + h), radius=8,
                        fill=rgb(0x141A26), outline=rgb(CLR_STROKE), width=1)
    for i, ln in enumerate(lines):
        d.text((20, top + 5 + 19 * i), ln, font=f(CJK, 16),
               fill=rgb(CLR_TEXT if i == 0 else CLR_TEXT_DIM), anchor="la")

    # 底部按键提示（两行制，同 mkHintLine 的思路）
    d.text((120, 229), hint, font=f(CJK, 16), fill=rgb(CLR_TEXT_DIM), anchor="mm")
    return im


SCREENS = [
    ("idle",   "小橘", 0.72, ["在等你打字…", "不过我也不急"], "空格 摸摸 · 回车 喂食"),
    ("greet",  "小橘", 0.90, ["你回来啦！", "走了 12 分钟，我想你了"], "空格 摸摸 · 回车 喂食"),
    ("hungry", "小橘", 0.35, ["肚子叫了…", "到饭点啦，喂我一口？"], "空格 摸摸 · 回车 喂食"),
]

for i, (pose, name, mood, lines, hint) in enumerate(SCREENS):
    im = build(pose, name, mood, lines, hint)
    img.paste(im, (i * (W + GAP), 0))

# 逐屏放大输出
for i, (pose, *_rest) in enumerate(SCREENS):
    s = SCREENS[i]
    build(s[0], s[1], s[2], s[3], s[4]).resize((W * S, H * S), Image.NEAREST)\
        .save(f"mock_pet_{s[0]}.png")

img.resize((img.width * 2, img.height * 2), Image.NEAREST).save("mock_pet.png")


# ================= 场景 2：随机探头（SCENE_PEEK）=====================
# 临时浮层，5~8s 后自动消失，长按 LOGO 收回去。
# 贴在**底部横条**而不是屏幕中央：主屏 7 种风格的内容重心都在中上部，
# 底部横条是唯一不打架的位置。
def build_peek(pose, name, lines, close_hint="长按 LOGO 收起我"):
    """底下是极客仪表盘的简化版，证明横条确实不挡主内容。"""
    im, d = new_screen(pose)

    # ---- 底下：主屏内容（简化示意，真实布局见各 build_style_*）----
    d.text((120, 64), "23:47", font=f(LATIN_B, 40), fill=rgb(CLR_TEXT), anchor="mm")
    d.text((120, 112), "10月4日 星期日", font=f(CJK, 16),
           fill=rgb(CLR_TEXT_DIM), anchor="mm")
    d.rounded_rectangle((16, 130, 110, 140), radius=5, fill=rgb(CLR_SURFACE_2))
    d.rounded_rectangle((16, 130, 86, 140), radius=5, fill=rgb(CLR_AMBER))
    d.rounded_rectangle((130, 130, 224, 140), radius=5, fill=rgb(CLR_SURFACE_2))
    d.rounded_rectangle((130, 130, 178, 140), radius=5, fill=rgb(CLR_GREEN))

    # ---- 探头横条：y=168..240，h=72 ----
    d.rectangle((0, 168, 239, 239), fill=rgb(0x141A26))
    d.line((0, 168, 239, 168), fill=rgb(CLR_STROKE), width=1)

    # 猫头：r=26，中心 (36, 204) —— 只画头，肩膀在横条外，看起来像"探出来"
    cx, cy = 36, 204
    d.ellipse((cx - 20, cy - 26, cx + 20, cy + 14), fill=rgb(CLR_PET))
    d.ellipse((cx - 21, cy - 30, cx - 7, cy - 16), fill=rgb(CLR_PET))
    d.ellipse((cx + 7, cy - 30, cx + 21, cy - 16), fill=rgb(CLR_PET))
    d.ellipse((cx - 18, cy - 26, cx - 12, cy - 20), fill=rgb(CLR_PET_DARK))
    d.ellipse((cx + 12, cy - 26, cx + 18, cy - 20), fill=rgb(CLR_PET_DARK))
    for sx in (-1, 1):                      # 眼睛
        ex = cx + sx * 8
        d.ellipse((ex - 4, cy - 12, ex + 4, cy - 4), fill=rgb(0x1A1A1A))
    d.polygon([(cx - 3, cy - 1), (cx + 3, cy - 1), (cx, cy + 2)], fill=rgb(0xD2694A))
    d.arc((cx - 8, cy - 2, cx, cy + 7), 0, 110, fill=rgb(0x1A1A1A), width=2)
    d.arc((cx, cy - 2, cx + 8, cy + 7), 70, 180, fill=rgb(0x1A1A1A), width=2)

    # 台词：x=64..232 = 168px -> 每行 10 字
    #   横条 72px 只装得下 3 行（72 / 19 = 3.8），所以是「2 行台词 + 1 行关闭提示」。
    #   ✕ 放在**提示行右端**而不是第一行右上角 —— 放右上角会吃掉台词第一行的宽度，
    #   放提示行右端则整个 168px 都能给台词。
    for i, ln in enumerate(lines):
        d.text((64, 171 + 19 * i), ln, font=f(CJK, 16),
               fill=rgb(CLR_TEXT if i == 0 else CLR_TEXT_DIM), anchor="la")
    d.text((64, 171 + 19 * 2), close_hint, font=f(CJK, 16),
           fill=rgb(CLR_TEXT_DIM), anchor="la")

    # 关闭按钮：提示行右端 16px 圆 + 叉（x=212..228, y=211..227）
    d.ellipse((212, 211, 228, 227), outline=rgb(CLR_TEXT_DIM), width=1)
    d.line((216, 215, 224, 223), fill=rgb(CLR_TEXT_DIM), width=1)
    d.line((224, 215, 216, 223), fill=rgb(CLR_TEXT_DIM), width=1)
    return im


PEEKS = [
    ("greet",  "小橘", ["你回来啦！", "我等了你 12 分钟"]),
    ("hungry", "小橘", ["肚子叫了…", "喂我一口好不好"]),
]

peek_sheet = Image.new("RGB", (W * len(PEEKS) + GAP, H), rgb(0x05070C))
for i, (pose, name, lines) in enumerate(PEEKS):
    peek_sheet.paste(build_peek(pose, name, lines), (i * (W + GAP), 0))

peek_sheet.resize((peek_sheet.width * 2, peek_sheet.height * 2), Image.NEAREST)\
    .save("mock_pet_peek.png")
build_peek(*PEEKS[0]).resize((W * S, H * S), Image.NEAREST).save("mock_pet_peek_greet.png")

print("写出 mock_pet.png / mock_pet_idle.png / mock_pet_greet.png / mock_pet_hungry.png")
print("写出 mock_pet_peek.png / mock_pet_peek_greet.png")
print(f"全屏对话气泡宽 216px，每行最多 {216 // 16} 字；探头台词宽 168px，每行 {168 // 16} 字")
