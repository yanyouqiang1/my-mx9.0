#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""量一段文案在 lv_font_simsun_16_cjk 里实际占多宽（像素）。

背景：底部按键提示那一行是用 16px 中文字体渲染的，屏只有 240px 宽。
之前没人量过宽度，"←→ 切换 · ↑↓ 调整 · 回车保存 · ESC 取消" 这种文案
在屏上左右各被裁掉几十像素（用户反馈"底部的字显示到区域外"）。
这个脚本直接解析字库的 glyph_dsc / cmaps，把每串候选文案的真实像素宽打出来，
改文案时先跑一遍就知道会不会溢出，不用靠估。

adv_w 在 LVGL 里是 1/16 px 单位，实际像素 = adv_w / 16。

用法：
    python text_width.py                 # 量内置的候选文案
    python text_width.py "你的文案"       # 量任意字符串
    python text_width.py --margin 216 ... # 指定可用宽度（默认 216 = 屏宽减两侧 12px 余量）
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_C = os.path.join(HERE, "src", "lv_font_simsun_16_cjk.c")

# 内置候选：底部按键提示每一行用到的文案。改文案时先跑一遍这个脚本。
# 屏只有 240px，两侧留 12px 余量 → 216px 是每一行的硬上限。
# 现在的方案是"拆两行"，所以原先一整行放不下的长句子拆开就都放得下了。
CANDIDATES = [
    # —— 旧版（超宽，留着当对照，别再改回去了）——
    ("旧-设置",   "←→ 切换 · ↑↓ 调整 · 回车保存 · ESC 取消"),      # 368px
    ("旧-倒计时", "←→ 切换 · ↑↓ 调整 · 回车开始/停止 · ESC 取消"),  # 408px
    ("旧-菜单",   "上下选择 · 回车确认 · ESC 返回"),               # 256px
    ("旧-日志",   "↑↓ 翻看 · DEL 清空 · ESC 返回"),               # 264px
    # —— 菜单（一级 / 二级）——
    ("菜单一级1", "↑↓ 选择 · 回车进入"),
    ("菜单一级2", "ESC 返回主屏"),
    ("菜单二级1", "↑↓ 选择 · 回车执行"),
    ("菜单二级2", "ESC 返回上一级"),
    # —— 设置页（第一行切字段/调值，第二行保存/取消）——
    ("设置1",     "←→ 切换 · ↑↓ 调整"),
    ("设置2",     "回车保存 · ESC 取消"),
    # —— 倒计时页：这一屏的回车是启停，不是保存 ——
    ("倒计时1",   "←→ 切换 · ↑↓ 调整"),
    ("倒计时2",   "回车启停 · ESC 取消"),
    # —— 日志页 ——
    ("日志1",     "↑↓ 翻看 · DEL 清空"),
    ("日志2",     "ESC 返回菜单"),
    # —— 录制页 ——
    ("录制1",     "M1-M12 保存 · MR 下一步"),
    ("录制2",     "ESC 取消"),
    # —— 校准页副提示 ——
    ("校准副提示", "↑↓ 调整偏移量"),
]

DEFAULT_MARGIN = 216   # 240 屏宽，两侧各留 12px 余量


def load_font(path=FONT_C):
    """返回 (cmap 区间列表, glyph_dsc 的 adv_w 列表, line_height)。

    cmaps: [(range_start, range_length, glyph_id_start, unicode_list|None)]
    adv_w: [int]，下标就是 glyph id
    """
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()

    # --- glyph_dsc 表：抽 adv_w ---
    dsc_m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not dsc_m:
        raise SystemExit("没找到 glyph_dsc 表，字库文件结构变了？")
    adv_w = [int(x) for x in re.findall(r"\.adv_w\s*=\s*(\d+)", dsc_m.group(1))]

    # --- cmaps 表 ---
    # 字库用的是 lv_font_conv 的 TINY 格式，两种区间：
    #   FORMAT0  连续，glyph_id = glyph_id_start + (cp - range_start)
    #   SPARSE  稀疏，unicode_list 单独是个命名数组（unicode_list_1 …），
    #            glyph_id = glyph_id_start + 该码点在数组里的下标
    uni_lists = {}
    for m in re.finditer(
        r"(unicode_list_\d+)\s*\[\]\s*=\s*\{(.*?)\n\};", src, re.S
    ):
        vals = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(2))]
        uni_lists[m.group(1)] = vals

    sec = src[src.find("cmaps[]"):]
    sec = sec[:sec.find("\n};") + 3]
    cmaps = []
    # 一个块一个块地切，不能跨块用 .*? 连，否则会串到下一块去
    for blk in re.findall(r"\{[^{}]*\}", sec):
        rs = re.search(r"\.range_start\s*=\s*(\d+)", blk)
        rl = re.search(r"\.range_length\s*=\s*(\d+)", blk)
        gs = re.search(r"\.glyph_id_start\s*=\s*(\d+)", blk)
        ul = re.search(r"\.unicode_list\s*=\s*NULL", blk)
        if not (rs and rl and gs):
            continue
        if ul:   # FORMAT0 连续区间
            cmaps.append((int(rs.group(1)), int(rl.group(1)),
                          int(gs.group(1)), None))
        else:    # SPARSE，unicode_list 是命名数组
            ref = re.search(r"\.unicode_list\s*=\s*(unicode_list_\d+)", blk)
            if not ref:
                continue
            cmaps.append((int(rs.group(1)), int(rl.group(1)),
                          int(gs.group(1)), uni_lists.get(ref.group(1), [])))

    lm = re.search(r"\.line_height\s*=\s*(\d+)", src)
    return cmaps, adv_w, (int(lm.group(1)) if lm else 0)


def glyph_id(cmaps, ch):
    """查字形 id。返回 None 表示字库里没这个字（LVGL 会画成 □ 方框）。

    注意 SPARSE 区间里 unicode_list 存的是 **码点减掉 range_start** 的相对值
    （所以列表开头会出现 0x0、0x7 这种数，分别对应 U+00B0、U+00B7），
    查的时候必须加回 range_start 再比对。
    """
    cp = ord(ch)
    for rs, rl, gs, uni in cmaps:
        if not (rs <= cp < rs + rl):
            continue
        if uni is None:                 # FORMAT0：连续
            return gs + (cp - rs)
        rel = cp - rs                   # SPARSE：列表里存相对值
        if rel in uni:                  # 在数组里按下标换算
            return gs + uni.index(rel)
    return None


def measure(cmaps, adv_w, s):
    """返回 (总像素宽, [(字符, 像素宽, 是否缺字)])。"""
    total = 0
    detail = []
    for ch in s:
        gid = glyph_id(cmaps, ch)
        if gid is None or gid >= len(adv_w):
            detail.append((ch, 0, True))
            continue
        w = adv_w[gid] / 16.0
        detail.append((ch, w, False))
        total += w
    return total, detail


def main():
    margin = DEFAULT_MARGIN
    args = sys.argv[1:]
    if "--margin" in args:
        i = args.index("--margin")
        margin = int(args[i + 1])
        del args[i:i + 2]

    cmaps, adv_w, lh = load_font()
    print(f"字库: {len(adv_w)} 个字形, line_height={lh}px, adv_w 单位=1/16px")
    print(f"可用宽度: {margin}px (屏 240px 减两侧余量)\n")

    if args:
        items = [("命令行", a) for a in args]
    else:
        items = CANDIDATES

    worst = 0
    for name, s in items:
        total, detail = measure(cmaps, adv_w, s)
        missing = "".join(ch for ch, _, miss in detail if miss)
        flag = "OK  " if total <= margin else "溢出"
        print(f"[{flag}] {name:14s} {total:6.1f}px  {s}")
        if missing:
            print(f"         ⚠ 缺字: {missing}")
        worst = max(worst, total)

    print(f"\n最宽的一串: {worst:.1f}px")
    return 0 if worst <= margin else 1


if __name__ == "__main__":
    sys.exit(main())
