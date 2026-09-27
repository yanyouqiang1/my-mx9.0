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
        lines = f.readlines()
    missing = {}
    for i, line in enumerate(lines, 1):
        if not CJK.search(line):
            continue
        # 注释里的中文不影响显示，但顺手一起报出来更容易定位
        for ch in set(CJK.findall(line)):
            if ord(ch) not in covered:
                missing.setdefault(ch, []).append(i)
    return missing


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
