# -*- coding: utf-8 -*-
"""
从源码中提取所有用到的字符（含中文），用 simhei.ttf 生成精简子集 LVGL 字体。
输出: src/lv_font_simsun_16_cjk.c  (16px, bpp=4, ASCII + 实际用到的汉字)

⚠ 踩坑记录（别再改回去）
------------------------
这版 lv_font_conv 的 `--symbols` **不读文件**，它的参数值会被当成"字面字符"
直接塞进字集（见 node_modules/lv_font_conv/lib/collect_font_data.js:73
的 `ranger.add_symbols(source_path, item.symbols)`）。
早期版本这里写的是 `--symbol charset.txt`，等于把那条文件路径的每个字符
当成要生成的字形，charset.txt 里的 332 个汉字一个都没进去 ——
产出的字体 cmap 只剩 0x20-0x7E，界面上所有中文全是空白/豆腐块。

所以改成把码点显式列成 `-r` 区间。区间参数是纯 ASCII，
不会在 Windows 的 cmd 转发 + GBK 代码页这一路上被改写，稳。
"""
import os, re, sys, subprocess

SRC = r"E:\ai-project\mx9.0\archives\6.0\lvgl_s3\src"
INO = os.path.join(SRC, "lvgl_s3.ino")
OUT_C = os.path.join(SRC, "lv_font_simsun_16_cjk.c")
OUT_TXT = os.path.join(SRC, "charset.txt")

# ---- 1. 收集字符 ----
# 注意：要扫 **所有源文件**，不只是 .ino。crash_trace.h 这类头文件里也有
# 中文字面量（开机诊断的阶段名表就放在头里），只扫 .ino 会漏字，
# 结果就是界面上出现豆腐块 —— 上一轮"重启原因"后面两个字乱码就是这么来的。
src_files = [INO] + sorted(
    os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".h")
)
text = ""
for p in src_files:
    text += open(p, encoding="utf-8", errors="ignore").read()
lits = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
chars = set()
for s in lits:
    for ch in s:
        if ord(ch) >= 0x20:
            chars.add(ch)

# 兜底：常用运行时拼接的中文（handleCommand / 动态文案里可能漏掉的）
extra = ("系统设置菜单击键回显开关亮度方案切换录制取消确认完成校准时间"
         "媒体播放暂停上下曲待机唤醒关机重启中执行动效开启关闭温度湿度"
         "计步蓝牙连接断开键盘异常错误警告信息通知壁纸律动模式"
         "极客风格时钟壁纸面板关于版本号剩余电量亮度等级已保存未保存"
         "个次总计按键数年月日星期星期二三四五六日一二三四五六七八九十"
         "音量静音亮度减小增大屏幕分辨率刷新率语言输入法时区日期时间"
         "自动关机唤醒灯效呼吸波浪涟漪极光拖尾跟随光谱音乐律动模式档位"
         "密码输入确认再次账户登录注册退出设置音效关闭开启设备名称"
         "警告请插入电池充电器低电量模式休眠唤醒恢复出厂设置确认重置"
         "所有数据将丢失不可撤销进度上传下载完成失败重试网络已断开"
         "正在连接请稍候信号强度非常好一般较差无服务漫游费用昂贵"
         "固件版本 bootloader 烧录成功 芯片 esp32s 序列号 容量 兆字节"
         "键盘机械轴体rgb 灯效同步呼吸亮度静态渐变波浪反应自定义颜色"
         "青紫品红黄绿蓝色白色黑色主题强调色卡片圆角阴影透明度模糊"
         )
for ch in extra:
    if ord(ch) >= 0x20:
        chars.add(ch)

# ASCII 0x20-0x7E 全要（时间/数字/英文菜单名）
for c in range(0x20, 0x7F):
    chars.add(chr(c))

# 常用中文标点
for ch in "，。、；：？！（）【】《》“”‘’…—·「」":
    chars.add(ch)

cs = "".join(sorted(chars))
open(OUT_TXT, "w", encoding="utf-8").write(cs)

codes = sorted(ord(c) for c in cs)
n_cjk = len([c for c in codes if c > 0x7F])
print("charset size: %d  (ASCII %d / non-ASCII %d)" % (len(codes), len(codes) - n_cjk, n_cjk))

# ---- 2. 把码点压成 -r 区间参数（纯 ASCII）----
def to_ranges(sorted_codes):
    """相邻码点合并成区间，单个码点写成一个点。输出纯 ASCII。"""
    parts, i = [], 0
    while i < len(sorted_codes):
        start = sorted_codes[i]
        end = start
        while i + 1 < len(sorted_codes) and sorted_codes[i + 1] == end + 1:
            i += 1
            end = sorted_codes[i]
        parts.append("0x%X" % start if start == end else "0x%X-0x%X" % (start, end))
        i += 1
    return ",".join(parts)

RANGES = to_ranges(codes)
print("range spec: %d entries, %d chars" % (RANGES.count(",") + 1, len(RANGES)))

# ---- 3. 调 lv_font_conv (npm 版) 生成 ----
# lv_font_conv 不支持 .ttc (TrueType Collection)，用单字重 ttf
TTC = r"C:\Windows\Fonts\simhei.ttf"
CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "node_modules", ".bin", "lv_font_conv.cmd")
cmd = [
    CLI,
    "--font", TTC,
    "--size", "16",
    "--bpp", "4",
    "--format", "lvgl",
    "--lv-include", "lvgl.h",
    "--no-compress",
    "--no-kerning",
    # 汉字字形缺失时回落到 Montserrat 14，避免再冒出豆腐块
    "--lv-fallback", "lv_font_montserrat_14",
    "-r", RANGES,
    "-o", OUT_C,
]
print("running:", " ".join(cmd)[:200], "...")
r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print(r.stdout[-3000:])
print(r.stderr[-3000:])
if r.returncode == 0:
    # lv_font_conv 最新版会输出 .user_data 字段，LVGL 8.4 的 lv_font_t 没有该成员
    txt = open(OUT_C, encoding="utf-8").read()
    txt = txt.replace("    .user_data = NULL,\n", "")
    open(OUT_C, "w", encoding="utf-8", newline="\n").write(txt)
    print("patched .user_data for LVGL 8.4")
    print("OK, size =", os.path.getsize(OUT_C))
else:
    print("FAILED rc =", r.returncode)
sys.exit(r.returncode)
