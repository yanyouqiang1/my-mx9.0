#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
校验固件里所有中文字符是否都在 lv_font_simsun_16_cjk 字库里。

背景：这个字库是用 lv_font_conv 从 simhei.ttf 生成的**子集**（注释里 -r 那串
只圈了约一千个常用字）。凡是不在那串范围里的汉字，LVGL 会画成 □ 方框 ——
表现为"文字乱码"。所以每加一条中文文案，都必须先过这个脚本。

用法：
    python check_font.py            # 报告所有缺字 + 出处
    python check_font.py --chars    # 只把缺字打进剪贴板用的集合
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_C = os.path.join(HERE, "src", "lv_font_simsun_16_cjk.c")
SOURCES = [
    os.path.join(HERE, "src", "lvgl_s3.ino"),
    os.path.join(HERE, "src", "elog.h"),
    os.path.join(HERE, "src", "elog.cpp"),
]

CJK = re.compile(r"[\u3000-\u303F\u4E00-\u9FFF\uFF00-\uFFEF]")


def load_covered():
    # 3500 字的 -r 串有两万多字符，头部注释很长，别只读一小段
    with open(FONT_C, "r", encoding="utf-8", errors="replace") as f:
        head = f.read(120000)
    m = re.search(r"-r\s+(.+?)(?:\s+-o\s|$)", head, re.S)
    if not m:
        raise SystemExit("解析不出 -r 范围，字库头部格式变了？")
    covered = set()
    for part in m.group(1).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            covered.update(range(int(a, 16), int(b, 16) + 1))
        else:
            covered.add(int(part, 16))
    return covered


def scan(path, covered):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = strip_comments(f.read())
    lines = text.split("\n")
    missing = {}
    for i, line in enumerate(lines, 1):
        if not CJK.search(line):
            continue
        for ch in set(CJK.findall(line)):
            if ord(ch) not in covered:
                missing.setdefault(ch, []).append(i)
    return missing


def strip_comments(text):
    """把 C/C++ 注释去掉，只留真正会被编译的代码（含字符串字面量）。

    为什么必须去注释：注释里出现汉字很正常（这份固件注释写得比代码还多），
    但注释**不会显示在屏上**。不剔除的话工具会把几百个注释用字全报成"缺字"，
    真正的缺字反而被淹没 —— 这个脚本靠"干净"才有意义。

    刻意用状态机而不是正则：正则分不清 "http://x" 里的 // 和真注释，
    会把一整行代码连带后面的字面量一起吃掉 → **漏报**真正的缺字。
    漏报比误报危险得多，所以老老实实按字符走。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                       # 行注释
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":                     # 块注释
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")                      # 保留换行，行号才不会错位
                i += 1
            i += 2
        elif c in "\"'":                                  # 字符串 / 字符字面量
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:         # 转义
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    covered = load_covered()
    total = 0
    for path in SOURCES:
        if not os.path.exists(path):
            print(f"跳过（不存在）：{path}")
            continue
        missing = scan(path, covered)
        total += len(missing)
        name = os.path.basename(path)
        if not missing:
            print(f"[OK] {name}：没有缺字")
            continue
        print(f"[缺 {len(missing)} 字] {name}：")
        for ch, lines in sorted(missing.items()):
            spots = ", ".join(str(n) for n in lines[:6])
            more = f" 等 {len(lines)} 处" if len(lines) > 6 else ""
            print(f"    {ch}  U+{ord(ch):04X}  行 {spots}{more}")
    print()
    if total:
        print(f"合计 {total} 个字不在字库里，会显示成方框。")
        sys.exit(1)
    print("全部命中字库。")


if __name__ == "__main__":
    main()
