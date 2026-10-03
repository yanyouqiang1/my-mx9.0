#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
重新生成 lv_font_simsun_16_cjk.c —— 3500 常用字全量字库。

起因：原来的字库只圈了约一千个常用字，固件里凡是超出这个范围的汉字
（比如"遭遇""决裂""章节"里的 遭/遇/决/裂/章）LVGL 都会画成 □ 方框，
用户看到的现象就是"文字乱码"。

做法：把《通用规范汉字表》一级字表（3500 字，常用字集）整套塞进去，
再加上原来那套 ASCII / 全角标点 / 特殊符号，源字体还是 C:\\Windows\\Fonts\\simhei.ttf。

体积：16px / 4bpp，不加 --no-compress（走 LVGL 的 LZ4 压缩），
3500 字大约 500KB 上 Flash，分区还剩得下，N16R8 不用省。

依赖：node + 全局 lv_font_conv（npm i -g lv_font_conv）
用法：
    python gen_font.py          # 生成
    python gen_font.py --check  # 只报字库覆盖情况，不重新生成
"""

import os
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT_C = os.path.join(SRC, "lv_font_simsun_16_cjk.c")
TXT_CACHE = os.path.join(HERE, "hanzi_3500.txt")
TTF = r"C:\Windows\Fonts\simhei.ttf"

# 一级字表（3500 字）来源。多个镜像，谁活着用谁。
SOURCES = [
    "https://raw.githubusercontent.com/BobXU0719/han-list/master/level-1.txt",
    "https://raw.githubusercontent.com/quanh/chinese-char-dict/master/"
    "hanzi2013/basic_hanzi_wiki_8105_1.text",
]

# 除汉字以外必须保留的码位：ASCII、度数/间隔号、破折号、弯引号、省略号、
# 方向箭头、中文顿号/书名号/括号、以及全角标点。跟旧字库保持一致。
EXTRA_RANGES = [
    (0x20, 0x7E),    # ASCII 可打印
    (0xB0, 0xB0),    # °
    (0xB7, 0xB7),    # ·
    (0x2014, 0x2014),  # —
    (0x2018, 0x2019),  # ‘’
    (0x201C, 0x201D),  # “”
    (0x2026, 0x2026),  # …
    (0x2190, 0x2193),  # ↑↓→←
    (0x3001, 0x3002),  # 、。
    (0x300A, 0x300D),  # 《》「」
    (0x3010, 0x3011),  # 【】
    (0xFF01, 0xFF01),  # ！
    (0xFF08, 0xFF09),  # （）
    (0xFF0C, 0xFF0C),  # ，
    (0xFF1A, 0xFF1B),  # ：；
    (0xFF1F, 0xFF1F),  # ？
]


def load_hanzi():
    """拿 3500 字表，优先用本地缓存。"""
    if os.path.exists(TXT_CACHE):
        with open(TXT_CACHE, "r", encoding="utf-8") as f:
            chars = [c for c in f.read() if not c.isspace()]
        if len(chars) >= 3400:
            return chars
    for url in SOURCES:
        try:
            print(f"下载字表：{url}")
            with urllib.request.urlopen(url, timeout=30) as r:
                raw = r.read().decode("utf-8", errors="replace")
            chars = [c for c in raw if "一" <= c <= "鿿"]
            if len(chars) >= 3400:
                with open(TXT_CACHE, "w", encoding="utf-8") as f:
                    f.write("".join(chars))
                print(f"  拿到 {len(chars)} 字，已缓存到 {os.path.basename(TXT_CACHE)}")
                return chars
            print(f"  只拿到 {len(chars)} 字，换下一个源")
        except Exception as e:
            print(f"  失败：{e}")
    raise SystemExit("所有字表源都没拿到，检查网络，或手动放一个 hanzi_3500.txt 在项目根目录")


def covered_set(chars):
    s = set(ord(c) for c in chars)
    for a, b in EXTRA_RANGES:
        s.update(range(a, b + 1))
    return s


def to_ranges(codepoints):
    """把码位集合压成 lv_font_conv 的 -r 参数：连续段写成 a-b，单个直接写。"""
    pts = sorted(codepoints)
    out = []
    start = prev = pts[0]
    for p in pts[1:]:
        if p == prev + 1:
            prev = p
            continue
        out.append((start, prev))
        start = prev = p
    out.append((start, prev))
    return out


def fmt_ranges(seg_list):
    return ",".join(f"0x{a:X}" if a == b else f"0x{a:X}-0x{b:X}" for a, b in seg_list)


def chunk_ranges(codepoints, limit=1200):
    """按字符长度把 range 串切成多段，供多个 -r 参数使用。"""
    segs = to_ranges(codepoints)
    chunks, cur, n = [], [], 0
    for seg in segs:
        piece = fmt_ranges([seg])
        if cur and n + len(piece) + 1 > limit:
            chunks.append(",".join(cur))
            cur, n = [], 0
        cur.append(piece)
        n += len(piece) + 1
    if cur:
        chunks.append(",".join(cur))
    return chunks


def find_lv_font_conv():
    """返回一个能直接执行的入口。

    优先返回 node + 包内 js 路径，而不是 npm 生成的 .cmd 包装：
    .cmd 走 cmd.exe，整条命令行上限 8191 字符，而 3500 字的 -r 串有两万多个
    字符，怎么切分都超（切分不缩短总长）。直接用 node 拉起同一个 js，
    走 CreateProcess，上限 32767，就没这个问题了。
    """
    node = shutil.which("node")
    if node:
        cands = [
            os.path.join(os.environ.get("APPDATA", ""), "npm", "node_modules",
                         "lv_font_conv", "lv_font_conv.js"),
            os.path.join(os.path.dirname(sys.executable), "node_modules",
                         "lv_font_conv", "lv_font_conv.js"),
        ]
        for c in cands:
            if c and os.path.exists(c):
                return [node, c]
    exe = shutil.which("lv_font_conv")
    if exe:
        return [exe]
    raise SystemExit("找不到 lv_font_conv，先装：npm i -g lv_font_conv")


def scan_source_chars(path):
    """把固件源码里出现过的所有汉字也纳入字库。

    一级字表 3500 字覆盖了绝大多数日常用字，但二级/三级字（帧/珀/琥/晖…）
    一样会被代码用到 —— 比如颜色名"琥珀"。只按 3500 生成，check_font.py
    照样会报缺字、屏上照样出方框。所以这里直接扫源码补齐，缺字问题从根上消失。
    源码里新增中文文案后重跑一次本脚本即可，不用手工维护字表。
    """
    found = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ch in f.read():
            if "㐀" <= ch <= "鿿":
                found.add(ord(ch))
    return found


def source_files():
    """要扫描的固件源码。

    **所有** .ino/.h/.cpp 都要扫，不只是 lvgl_s3.ino —— 错误日志的界面文案在
    lvgl_s3.ino 里，但模块自己的头文件（elog.h）以后也可能加中文。
    漏掉任何一个文件的后果都是"屏上显示方框"，而这种问题只有烧进去才发现。

    排除 lv_font_simsun_16_cjk.c：它是**本脚本的产物**，不是输入，
    而且它本身没有任何汉字（字形是二进制数据），扫它纯属浪费。
    """
    out = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith((".ino", ".h", ".cpp")):
            continue
        if name.startswith("lv_font_"):
            continue
        out.append(os.path.join(SRC, name))
    return out


def main():
    chars = load_hanzi()
    cov = covered_set(chars)
    base = len(cov)

    srcs = source_files()
    for src in srcs:
        extra = scan_source_chars(src)
        added = extra - cov
        cov |= extra
        if added:
            print(f"另外从 {os.path.basename(src)} 补进 {len(added)} 个字："
                  + "".join(chr(c) for c in sorted(added)))

    print(f"字表 {len(chars)} 字 + 符号，合计 {len(cov)} 个码位（基础 {base}）")
    print(f"range 段 {len(to_ranges(cov))} 段，完整串 {len(fmt_ranges(to_ranges(cov)))} 字符")

    if "--check" in sys.argv:
        # 顺便报一下固件里的中文有没有漏网的
        try:
            subprocess.run([sys.executable, os.path.join(HERE, "check_font.py")], check=False)
        except Exception:
            pass
        return

    if not os.path.exists(TTF):
        raise SystemExit(f"找不到源字体 {TTF}")
    if not os.path.exists(OUT_C):
        raise SystemExit(f"找不到旧字库 {OUT_C}，先备份一份再生成")
    shutil.copy2(OUT_C, OUT_C + ".bak")

    cmd = find_lv_font_conv() + [
        "--font", TTF, "--size", "16", "--bpp", "4",
        "--format", "lvgl", "--lv-include", "lvgl.h",
        "--lv-fallback", "lv_font_montserrat_14",
        # 不压缩：lv_font_conv 默认会走 LZ4/plist 压缩，此时 LVGL 必须配
        # LV_USE_FONT_COMPRESSED=1 才能解出字形，而这条链路上一旦哪里对不上，
        # 表现就是"整屏中文静默变空白"（Montserrat 数字还在，因为它们走 fallback）。
        # 16px/4bpp 下一个汉字最多 8*16/2 = 64 字节，3500 字也只有 ~220KB，
        # 分区完全放得下，没必要为省这点空间去赌压缩链路。
        "--no-compress",
        "-r", fmt_ranges(to_ranges(cov)),
        "-o", OUT_C,
    ]
    print(f"生成中（-r {len(fmt_ranges(to_ranges(cov)))} 字符，大字表要跑一会儿）……")
    subprocess.run(cmd, check=True)
    patch_for_lvgl84()
    size = os.path.getsize(OUT_C)
    print(f"完成：{OUT_C}  {size/1024:.0f} KB（旧文件备份在 .bak）")


def patch_for_lvgl84():
    """去掉 .user_data 那一行。

    lv_font_conv 1.5.3 生成的字体结构体里带 `.user_data = NULL,`，但项目用的
    LVGL 8.4 里这个成员是被 `#if LV_USE_USER_DATA` 包着的，而本项目没开这个宏 ——
    于是编译报 "excess elements in struct initializer"，指针指向 .user_data。
    旧字库里也有这行（大概是当年 LV_USE_USER_DATA 开着时生成的），一旦重新
    生成就必炸，所以在脚本里直接抹掉，让本脚本可以反复跑。
    """
    with open(OUT_C, "r", encoding="utf-8") as f:
        lines = f.readlines()
    kept = [ln for ln in lines if not ln.strip().startswith(".user_data =")]
    if len(kept) == len(lines):
        print("  （无需修补，未找到 .user_data 行）")
        return
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(kept)
    print(f"  已修补：去掉 {len(lines)-len(kept)} 行 .user_data")


if __name__ == "__main__":
    main()
