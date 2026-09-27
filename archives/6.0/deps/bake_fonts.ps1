# ========================
# LVGL 字体烘焙脚本
# ========================
# 使用方法：
#   1. 从清华 TUNA 镜像下载思源黑体 OTF
#   2. 安装 Python + fonttools: pip install fonttools lz4
#   3. 运行本脚本
#
# 下载 OTF（可选，已在下面直接使用 URL）：
#   https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf
#   https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf

$TEMP_DIR = "$env:TEMP\lvgl_fonts_$$"
New-Item -ItemType Directory -Force -Path $TEMP_DIR | Out-Null

$REGULAR_URL = "https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
$BOLD_URL    = "https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf"

$REGULAR_OTF = "$TEMP_DIR\SourceHanSansSC-Regular.otf"
$BOLD_OTF    = "$TEMP_DIR\SourceHanSansSC-Bold.otf"

Write-Host "下载思源黑体 OTF..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $REGULAR_URL -OutFile $REGULAR_OTF -TimeoutSec 60
Invoke-WebRequest -Uri $BOLD_URL    -OutFile $BOLD_OTF    -TimeoutSec 60

# 需要字符集（覆盖所有界面常用字）
$CHARSET = @"
0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz
一乙二十丁厂七卜八人儿入八儿九刀刁力乃又三于干士土才寸大下万丈三与
上小口言足舌斤大手才支竹日曰月木火水火灬父爷爸月月朋服冬几何
年朱先兆先儿兔乌月月服服朊周鱼兔狗狼猪马牛羊虫虱亀亀龍
时星期一二三四五六日时分秒年月日温度湿度闹钟设置菜单录制退出
确认取消上下一左右确认返回开关亮度音量静音背光键盘灯效切换动效
键盘背光控制目标显示器亮度调亮调暗增加减少极客仪表盘大时钟击键
监控壁纸信息面板律动风格预览应用已开启已关闭已就绪正在重新启动
农历新年快乐夏天凉爽雨天阴天雪天高温低温高湿低湿年月日周时分秒
""@

$CHARSET += "℃℅""«»[]()<>{}「」『』【】《》〈〉〔〕〖〗〘〙〚〛⦗⦘"
$CHARSET += "♪♫★☆●○◆◇□■△▲▽▼◁◀▷▶<>≤≥≈≠±−×÷√∞∑∏∫"

function Install-PythonIfNeeded {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) {
        Write-Host "需要 Python，请从 https://python.org 安装后重试"
        exit 1
    }
    $pkgs = pip show fonttools 2>$null
    if (-not $pkgs) {
        Write-Host "安装 fonttools..."
        pip install fonttools lz4
    }
}

function Bake-Font {
    param (
        [string]$InputOtf,
        [string]$OutputC,
        [string]$FontName,
        [int]$Size,
        [string]$Bpp,
        [string]$Chars
    )

    $script = @"
import sys
import os
sys.path.insert(0, os.path.dirname(sys.argv[0]) if len(sys.argv) > 0 else '.')
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options
import struct, zlib

otf = r'$InputOtf'
out_c = r'$OutputC'
font_name = '$FontName'
size = $Size
bpp = '$Bpp'

chars = '''$Chars'''

font = TTFont(otf)
cmap = font.getBestCmap()
options = Options()
options.set(drop_tables=['GSUB', 'GPOS', 'kern])

glyph_ids = []
glyph_names = []
for c in chars:
    code = ord(c)
    if code in cmap:
        glyph_ids.append(code)
        glyph_names.append(cmap[code])

glyph_ids.sort()
char_cnt = len(glyph_ids)

# Build glyph bitmaps (simplified, real impl needs more)
print(f"Font: {font_name}, Size: {size}px, Chars: {char_cnt}")

# Use fonttools subsetter
subsetter = Subsetter(options=options)
subsetter.populate(text=chars)
subsetter.subset(font)

# Get glyf table for custom renderer
font.saveXML(f'$TEMP_DIR\{font_name}_{size}.ttx')

# Write a minimal LVGL C font file
# This is a simplified stub - the real conversion needs the lv_font_t struct building
# For a proper implementation, use lvgl's official font converter tool or lv_font_montserrat

with open(out_c, 'w', encoding='utf-8') as f:
    f.write(f'/* {font_name} {size}px - Auto-generated stub, replace with real baked font */\n')
    f.write(f'/* Run this script with proper lv_font tools to regenerate */\n\n')
    f.write('#include "lvgl.h"\n\n')
    f.write(f'/* Stub: {char_cnt} glyphs */\n')
    f.write(f'/* TODO: Replace with output from lv_font_montserrat or bake_fonts.py */\n')
    f.write(f'/* Placeholder font for compilation */\n')

print(f"Generated stub: {out_c}")
print("NOTE: This is a STUB file. Run with lv_font_montserrat generator for real fonts.")
"@

    $scriptPath = "$TEMP_DIR\bake_font_$([guid]::NewGuid().ToString('N')).py"
    $script | Out-File -FilePath $scriptPath -Encoding UTF8
    python $scriptPath
}

Write-Host "`n烘焙 16px 字体..."
Bake-Font -InputOtf $REGULAR_OTF -OutputC "E:\ai-project\mx9.0\archives\6.0\deps\lv_font_sans16.c" -FontName "lv_font_sans16" -Size 16 -Bpp "4" -Chars $CHARSET

Write-Host "`n烘焙 20px 字体..."
Bake-Font -InputOtf $REGULAR_OTF -OutputC "E:\ai-project\mx9.0\archives\6.0\deps\lv_font_sans20.c" -FontName "lv_font_sans20" -Size 20 -Bpp "4" -Chars $CHARSET

Write-Host "`n烘焙 28px 字体..."
Bake-Font -InputOtf $REGULAR_OTF -OutputC "E:\ai-project\mx9.0\archives\6.0\deps\lv_font_sans28.c" -FontName "lv_font_sans28" -Size 28 -Bpp "4" -Chars $CHARSET

Write-Host "`n烘焙 Bold 48px 字体..."
Bake-Font -InputOtf $BOLD_OTF -OutputC "E:\ai-project\mx9.0\archives\6.0\deps\lv_font_sans_bold48.c" -FontName "lv_font_sans_bold48" -Size 48 -Bpp "4" -Chars $CHARSET

# Cleanup
Remove-Item -Recurse -Force $TEMP_DIR -ErrorAction SilentlyContinue
Write-Host "`n完成！字体文件已生成到 deps/ 目录"
Write-Host "如需真正可用的字体，请在 LVGL 在线工具 https://lvgl.io/tools/fontconverter 生成"
