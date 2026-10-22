#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
量 LVGL 字体的实际像素宽度 —— 直接读 .pio/libdeps 里的字体 .c 文件，
拿到每个字形的 adv_w（LVGL 8 格式：真实像素值 × 16）。

为什么要自己量：
    屏只有 240x240，标题、按键名、底部提示行一旦超宽就会折行或被裁掉。
    用 PIL 量系统字体只能得到"近似值"，跟 LVGL 实际排版对不上
    （Montserrat 是自带的，跟系统字体完全不是一回事）。
    直接读字体 .c 里的 adv_w 才是**屏幕上的真实宽度**。

用法：
    /usr/bin/python3 lvgl_font_width.py "Space"          # 默认 montserrat_48
    /usr/bin/python3 lvgl_font_width.py --font 28 "PgUp"
    /usr/bin/python3 lvgl_font_width.py --cjk "YYQ键盘"
    /usr/bin/python3 lvgl_font_width.py --list

字体来源（自动定位）：
    .pio/libdeps/*/lvgl/src/font/lv_font_montserrat_NN.c
    src/lv_font_simsun_16_cjk.c
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

LVGL_FONT_DIR = None
for root, dirs, files in os.walk(os.path.join(HERE, ".pio", "libdeps")):
    if "lv_font_montserrat_48.c" in files:
        LVGL_FONT_DIR = root
        break

CJK_FONT = os.path.join(HERE, "src", "lv_font_simsun_16_cjk.c")


def _parse_adv_w(path):
    """返回 {codepoint: adv_w_in_1_16_px} 和 {codepoint: adv_w_px}。"""
    src = open(path, encoding="utf-8").read()

    # 1) adv_w 数组：glyph_dsc[] 里每行一个 .adv_w = N
    adv_ws = [int(m) for m in re.findall(r"\.adv_w\s*=\s*(\d+)", src)]

    # 2) cmaps：range_start / range_length / glyph_id_start / type
    cmap_re = re.compile(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
        r"\.glyph_id_start\s*=\s*(\d+),\s*"
        r"\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),\s*"
        r"\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)"
    )
    cmaps = []
    for m in cmap_re.finditer(src):
        cmaps.append(dict(
            range_start=int(m.group(1)),
            range_length=int(m.group(2)),
            glyph_id_start=int(m.group(3)),
            unicode_list=m.group(4),
            glyph_id_ofs_list=m.group(5),
            list_length=int(m.group(6)),
            type=m.group(7),
        ))

    # 3) sparse cmap 的 unicode_list_1 数组
    def read_u16_list(name):
        if name == "NULL":
            return []
        m = re.search(re.escape(name) + r"\s*\[\]\s*=\s*\{(.*?)\};", src, re.S)
        if not m:
            return []
        return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2,6})", m.group(1))]

    out = {}
    for cm in cmaps:
        if cm["type"].endswith("FORMAT0_TINY"):
            # 连续区间：glyph_id = glyph_id_start + (cp - range_start)
            for i in range(cm["range_length"]):
                cp = cm["range_start"] + i
                gid = cm["glyph_id_start"] + i
                if gid < len(adv_ws):
                    out[cp] = adv_ws[gid]
        elif cm["type"].endswith("SPARSE_TINY"):
            ul = read_u16_list(cm["unicode_list"])
            for i, rcp in enumerate(ul):
                # ⚠️ sparse cmap 里存的是**相对码点**（letter - range_start），
                # 不是绝对码点。见 lv_font_fmt_txt.c 的 get_glyph_dsc_id()：
                #     uint32_t rcp = letter - fdsc->cmaps[i].range_start;
                #     bsearch(&key=rcp, unicode_list, ...)
                # 所以绝对码点 = range_start + 表里的值。
                cp = cm["range_start"] + rcp
                gid = cm["glyph_id_start"] + i
                if gid < len(adv_ws):
                    out[cp] = adv_ws[gid]
    return out


def _search_index(sorted_list, cp):
    """复刻 LVGL 的 search()：返回 cp 在有序数组里的下标，找不到返回长度。"""
    lo, hi = 0, len(sorted_list)
    while lo < hi:
        mid = (lo + hi) // 2
        if sorted_list[mid] < cp:
            lo = mid + 1
        elif sorted_list[mid] > cp:
            hi = mid
        else:
            return mid
    return len(sorted_list)


_FONT_CACHE = {}


def load_font(path):
    if path not in _FONT_CACHE:
        _FONT_CACHE[path] = _parse_adv_w(path)
    return _FONT_CACHE[path]


def char_width_px(path, ch):
    adv = load_font(path).get(ord(ch))
    if adv is None:
        return None
    return adv / 16.0


def text_width_px(path, text):
    """LVGL 排版宽度 = 各字形 advance 之和（不等宽字体不用 kerning）。"""
    total = 0.0
    missing = []
    for ch in text:
        w = char_width_px(path, ch)
        if w is None:
            missing.append(ch)
            w = 0
        total += w
    return total, missing


def montserrat_path(size):
    p = os.path.join(LVGL_FONT_DIR, "lv_font_montserrat_%d.c" % size)
    return p if os.path.exists(p) else None


def main():
    args = sys.argv[1:]
    if not LVGL_FONT_DIR:
        print("找不到 lvgl 字体目录（先跑一次 pio run）")
        return

    if "--list" in args:
        print("montserrat 可用档位：")
        for f in sorted(os.listdir(LVGL_FONT_DIR)):
            m = re.match(r"lv_font_montserrat_(\d+)\.c$", f)
            if m:
                print("  montserrat_%s" % m.group(1))
        return

    size = 48
    if "--font" in args:
        i = args.index("--font")
        size = int(args[i + 1])
        del args[i:i + 2]

    use_cjk = "--cjk" in args
    if use_cjk:
        args.remove("--cjk")
        path = CJK_FONT
        label = "simsun_16_cjk"
    else:
        p = montserrat_path(size)
        if not p:
            print("没有 montserrat_%d" % size)
            return
        path = p
        label = "montserrat_%d" % size

    if not args:
        print("用法：lvgl_font_width.py [--font N] [--cjk] \"文本\"")
        return

    text = " ".join(args)
    w, missing = text_width_px(path, text)
    print("%-16s %r  ->  %.1f px" % (label, text, w))
    if missing:
        print("  ! 字库里没有这些字形: %s" % " ".join(missing))


if __name__ == "__main__":
    main()
