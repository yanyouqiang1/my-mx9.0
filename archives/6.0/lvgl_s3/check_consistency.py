#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
固件 ↔ 壁纸图 的坐标一致性自检

背景：底部通栏条的坐标是**两边硬约定**的 ——
  固件  src/lvgl_s3.ino            #define BAR_H 72 / BAR_Y (240-BAR_H)
  壁纸  wallpaper/gen_wallpaper_240_v2.py   DARK_TOP = 190
两边必须对上，否则条会压到表盘弧线，或者下方露出没清干净的残留。

这里不靠肉眼，直接把两边的定义读出来算一遍。

用法：
    /usr/bin/python3 check_consistency.py
"""

import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
INO = os.path.join(HERE, "src", "lvgl_s3.ino")
WALL_SCRIPT = os.path.join(ROOT, "wallpaper", "gen_wallpaper_240_v2.py")
WALL_PNG = os.path.join(ROOT, "wallpaper", "yyq-mx9-wallpaper-240-v2.png")

fails = []
notes = []


def check(ok, msg):
    (notes if ok else fails).append(("✓" if ok else "✗") + " " + msg)


# ---- 1. 读固件里的 BAR_H / BAR_Y ----
ino = open(INO, encoding="utf-8").read()
m = re.search(r"#define\s+BAR_H\s+(\d+)", ino)
check(m is not None, "固件里能找到 BAR_H 定义")
bar_h = int(m.group(1)) if m else -1

m = re.search(r"#define\s+BAR_Y\s+\((\d+)\s*-\s*BAR_H\)", ino)
check(m is not None, "固件里 BAR_Y 是 (240 - BAR_H) 形式")
bar_y = 240 - bar_h if m else -1

# ---- 2. 读壁纸脚本里的 BAR_Y / 表盘几何 ----
ws = open(WALL_SCRIPT, encoding="utf-8").read()
m = re.search(r"^BAR_Y\s*=\s*(\d+)", ws, re.M)
check(m is not None, "壁纸脚本里能找到 BAR_Y")
wall_bar_y = int(m.group(1)) if m else -1

m = re.search(r"CX,\s*CY,\s*R\s*=\s*(\d+),\s*(\d+),\s*(\d+)", ws)
check(m is not None, "壁纸脚本里能找到表盘几何 CX, CY, R")
cx, cy, r = (int(m.group(i)) for i in (1, 2, 3)) if m else (0, 0, 0)

arc_bottom = cy + r          # 表盘外圈最低那一行（ellipse 画出来是闭区间）

# ---- 3. 核心断言 ----
check(bar_y == wall_bar_y,
      "两边 BAR_Y 一致：固件 %d == 壁纸 %d" % (bar_y, wall_bar_y))

check(arc_bottom < bar_y,
      "圆弧最低行 y=%d < 条上边缘 %d（弧线整个在条上方，不被切）"
      % (arc_bottom, bar_y))

check(bar_y + bar_h == 240,
      "条的高度 %d 正好铺到屏幕底 (BAR_Y+%d=240)" % (bar_h, bar_y))

# ---- 4. 壁纸图实际像素：条覆盖区必须真的没有残留元素 ----
if os.path.exists(WALL_PNG):
    im = Image.open(WALL_PNG).convert("L")
    zone = im.crop((0, bar_y, 240, 240))
    bbox = zone.getbbox()
    check(bbox is None,
          "壁纸图 y=%d..240 实测无残留元素（getbbox=%s）" % (bar_y, bbox))
else:
    fails.append("✗ 壁纸图不存在：%s（先跑 gen_wallpaper_240_v2.py）" % WALL_PNG)

# ---- 5. 文字宽度不重叠（时间 / 日期 / 温湿度）----
# 宽度用 lvgl_font_width.py 实测值（adv_w 是 1/16px 单位）：
W_TIME_28 = 75.3      # "22:48"
W_DATE_16 = 120.0     # "10月22日 星期三"
W_TEMP_16 = 44.2      # "24.3C"
W_HUM_16 = 34.5       # "48%"

TIME_X = 14
DATE_X = 106
TEMP_X = 134
HUM_X = 190
# 内容最右允许到的位置。日期是**故意**排到 226 的（16px 中文"10月22日 星期三"
# 正好 120px，从 106 排到 226），所以日期这一项单独用 226；
# 温湿度那行留 15px 右边距，是刻意的视觉留白。
RIGHT_EDGE = 226
SHT_RIGHT_EDGE = 225

check(TIME_X + W_TIME_28 <= DATE_X,
      "时间右缘 %.1f <= 日期左缘 %d（同一行不重叠）" % (TIME_X + W_TIME_28, DATE_X))
check(DATE_X + W_DATE_16 <= RIGHT_EDGE,
      "日期右缘 %.1f <= %d（留右边距）" % (DATE_X + W_DATE_16, RIGHT_EDGE))
check(TEMP_X + W_TEMP_16 <= HUM_X,
      "温度右缘 %.1f <= 湿度左缘 %d（第二行不重叠）" % (TEMP_X + W_TEMP_16, HUM_X))
check(HUM_X + W_HUM_16 <= SHT_RIGHT_EDGE,
      "湿度右缘 %.1f <= %d（留右边距）" % (HUM_X + W_HUM_16, SHT_RIGHT_EDGE))

# ---- 6. 条的高度要放得下两行（28px 行高 30 + 16px 行高 18）----
check(bar_h >= 30 + 18 + 10,
      "条高 %d 放得下 28px 时间行 + 16px 温湿度行" % bar_h)

# ---- 7. 固件里标签的实际坐标要和这里一致 ----
for name, pat, want in [
    ("时间 x=14", r"lv_obj_set_pos\(wp_lbl_time,\s*14,\s*(\d+)\)", 14),
    ("日期 x=106", r"lv_obj_set_pos\(wp_lbl_date,\s*106,\s*(\d+)\)", 106),
]:
    mm = re.search(pat, ino)
    check(mm is not None, "固件里能找到 %s 的定位" % name)

mm = re.search(r"lv_obj_set_pos\(wp_lbl_time,\s*14,\s*(\d+)\)", ino)
if mm:
    check(int(mm.group(1)) + 30 <= bar_h,
          "固件时间标签 y=%s + 行高30 <= 条高 %d" % (mm.group(1), bar_h))

mm = re.search(r"lv_obj_set_pos\(sv_lbl_temp,\s*134,\s*(\d+)\)", ino)
check(mm is not None, "固件里能找到屏保温湿度标签定位")
if mm:
    check(int(mm.group(1)) + 18 <= bar_h,
          "固件温湿度标签 y=%s + 行高18 <= 条高 %d" % (mm.group(1), bar_h))

mm = re.search(r"lv_obj_set_pos\(sv_lbl_hum,\s*190,\s*(\d+)\)", ino)
check(mm is not None, "固件里能找到屏保湿度标签定位")

# 签名必须在条上边缘之上，且不能和表盘圆环相撞
mm = re.search(r'tracked\(d,\s*\((\d+),\s*(\d+)\),\s*"ESP32-S3', ws)
check(mm is not None, "壁纸脚本里能找到硬件签名的定位")
if mm:
    sig_x, sig_y = int(mm.group(1)), int(mm.group(2))
    check(sig_y + 9 <= bar_y,
          "硬件签名 y=%d + 行高9 <= 条上边缘 %d（不被条压住）" % (sig_y, bar_y))
    # 签名右端约 226、宽约 95px → 占 x=131..226。检查这一段是否碰到圆环。
    # 圆环：圆心 (cx,cy)、半径 r。签名落在右上角，
    # 只要签名左端 131 离圆心够远就不相撞。
    dist = ((sig_x - 95) - cx) ** 2 + (sig_y - cy) ** 2
    check(dist ** 0.5 > r,
          "硬件签名左端 x=%d 离圆心 %.1f > 半径 %d（不撞圆环）"
          % (sig_x - 95, dist ** 0.5, r))

print("\n".join(notes))
if fails:
    print()
    print("\n".join(fails))
    print("\n❌ 一致性自检未通过")
    sys.exit(1)
print("\n✅ 固件 ↔ 壁纸 坐标一致性自检全部通过")
print("   圆弧最低行 y=%d | 条 %d..240 | 两边 BAR_Y=%d"
      % (arc_bottom, bar_y, bar_y))
