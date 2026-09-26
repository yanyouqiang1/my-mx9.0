# YYQ-MX9.0 LVGL Chinese font baker
# Bakes Source Han Sans SC subset into LVGL-compatible .c files
# Usage: powershell -ExecutionPolicy Bypass -File .\bake_fonts.ps1

$ErrorActionPreference = 'Stop'

$OUT_DIR = "E:\ai-project\mx9.0\archives\6.0\lvgl_demo\deps"
$WORK_DIR = Join-Path $env:TEMP "lvgl-font-bake"

Write-Host ""
Write-Host "=== LVGL Chinese Font Baker ===" -ForegroundColor Cyan
Write-Host "Output: $OUT_DIR" -ForegroundColor DarkGray
Write-Host ""

# ====== 1. Check Node.js ======
Write-Host "[1/7] Checking Node.js..." -ForegroundColor Yellow
$nodeVer = $null
try {
    $nodeVer = (& node --version 2>&1) | Out-String
    $nodeVer = $nodeVer.Trim()
} catch {}
if (-not $nodeVer -or -not ($nodeVer -match "^v(\d+)\.")) {
    Write-Host "  X Node.js not found." -ForegroundColor Red
    exit 1
}
$nodeMajor = [int]$Matches[1]
if ($nodeMajor -lt 14) {
    Write-Host "  X Node.js $nodeVer too old (need >= 14)" -ForegroundColor Red
    exit 1
}
Write-Host "  V $nodeVer" -ForegroundColor Green
Write-Host ""

# ====== 2. Install lv_font_conv ======
Write-Host "[2/7] Installing lv_font_conv..." -ForegroundColor Yellow
& npm install -g lv_font_conv 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  X npm install failed" -ForegroundColor Red
    exit 1
}
Write-Host "  V $(& lv_font_conv --version 2>&1 | Out-String).Trim()" -ForegroundColor Green
Write-Host ""

# ====== 3. Prepare work dir ======
Write-Host "[3/7] Preparing work dir..." -ForegroundColor Yellow
if (Test-Path $WORK_DIR) { Remove-Item $WORK_DIR -Recurse -Force }
New-Item -ItemType Directory -Path $WORK_DIR | Out-Null
Set-Location $WORK_DIR
Write-Host "  Work dir: $WORK_DIR" -ForegroundColor Green
Write-Host ""

# ====== 4. Download fonts (tuna mirror) ======
Write-Host "[4/7] Downloading Source Han Sans SC..." -ForegroundColor Yellow
$fonts = @{
    "SourceHanSansSC-Regular.otf" = "https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
    "SourceHanSansSC-Bold.otf"    = "https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf"
}
foreach ($k in $fonts.Keys) {
    if (Test-Path $k) {
        $sizeMB = [math]::Round((Get-Item $k).Length / 1MB, 2)
        Write-Host "  (cached) $k ($sizeMB MB)" -ForegroundColor DarkGray
        continue
    }
    Write-Host "  Downloading $k..." -ForegroundColor DarkGray
    Invoke-WebRequest -Uri $fonts[$k] -OutFile $k -UseBasicParsing -TimeoutSec 120
    $sizeMB = [math]::Round((Get-Item $k).Length / 1MB, 2)
    Write-Host "    V $k ($sizeMB MB)" -ForegroundColor Green
}
Write-Host ""

# ====== 5. Generate subset string (Node.js, encoding-safe) ======
Write-Host "[5/7] Generating character subset..." -ForegroundColor Yellow

$jsCode = @"
const chars = new Set();
for (let i = 0x20; i <= 0x7E; i++) chars.add(String.fromCharCode(i));
for (let i = 0xFF01; i <= 0xFF5E; i++) chars.add(String.fromCharCode(i));
const cp = [0x6781,0x5BA2,0x65B9,0x6848,0x4EEA,0x8868,0x76D8,0x5B9E,0x65F6,0x51FB,0x952E,0x6E29,0x6E7F,0x5EA6,0x5F8B,0x52A8,0x58C1,0x7EB8,0x84DD,0x725B,0x8BBE,0x7F6E,0x95F9,0x949F,0x5012,0x8BA1,0x65F7,0x7F51,0x7EDC,0x914D,0x952E,0x76D8,0x9F20,0x6807,0x5C4F,0x5E55,0x663E,0x793A,0x98CE,0x683C,0x5207,0x6362,0x786E,0x8BA4,0x53D6,0x6D88,0x8FD4,0x56DE,0x4E0A,0x4E0B,0x5DE6,0x53F3,0x5355,0x51FB,0x53CC,0x957F,0x6309,0x6D3B,0x52A8,0x65E5,0x671F,0x5468,0x672A,0x661F,0x671F,0x4E00,0x4E8C,0x4E09,0x56DB,0x4E94,0x516D,0x4E03,0x516B,0x4E5D,0x5341,0x70B9,0x5206,0x79D2,0x5E74,0x6708,0x5929,0x6C14,0x53EF,0x4EE5];
for (const c of cp) chars.add(String.fromCodePoint(c));
console.log([...chars].join(''));
"@

$subsetStr = node -e $jsCode 2>&1 | Out-String
$subsetStr = $subsetStr.Trim()
Write-Host "  V $($subsetStr.Length) unique chars" -ForegroundColor Green
Write-Host ""

# ====== 6. Bake fonts via Node.js (bypasses PowerShell arg parsing) ======
Write-Host "[6/7] Baking fonts via Node.js (bypassing PowerShell arg issues)..." -ForegroundColor Yellow

$lfcPath = "C:\Users\37458\AppData\Roaming\npm\node_modules\lv_font_conv\lv_font_conv.js"

$bakeJs = @"
const { spawnSync } = require('child_process');
const path = require('path');
const fs   = require('fs');

const LFC  = '$lfcPath';
const DEST = '$OUT_DIR';
const WORK = '$WORK_DIR';
const SUBSET = '$subsetStr';

const jobs = [
    ['SourceHanSansSC-Regular.otf', 16, 'lv_font_sans16.c'],
    ['SourceHanSansSC-Regular.otf', 20, 'lv_font_sans20.c'],
    ['SourceHanSansSC-Regular.otf', 28, 'lv_font_sans28.c'],
    ['SourceHanSansSC-Bold.otf',   48, 'lv_font_sans_bold48.c'],
];

for (const [font, size, outName] of jobs) {
    const outPath = path.join(DEST, outName);
    console.log('Baking ' + font + ' ' + size + 'px -> ' + outName);
    const args = [
        '--font',    path.join(WORK, font),
        '--size',    String(size),
        '--format',  'lvgl',
        '--bpp',     '4',
        '--symbols', SUBSET,
        '--no-compress',
        '-o',        outPath,
    ];
    const r = spawnSync('node', [LFC, ...args], { encoding: 'utf8' });
    if (r.status !== 0) { console.error(r.stderr); process.exit(1); }

    let c = fs.readFileSync(outPath, 'utf8');

    // Fix include path
    c = c.replace(
        /#ifdef LV_LVGL_H_INCLUDE_SIMPLE\s*\n#include "lvgl\.h"\s*\n#else\s*\n#include "lvgl\/lvgl\.h"\s*\n#endif/g,
        '#include <lvgl.h>'
    );

    // Derive unique suffix from output filename: _16, _20, _28, _bold48
    const nameWithSize = outName
        .replace('lv_font_sans', '')
        .replace('.c', '');
    const suf = '_' + nameWithSize;

    // Rename static variables to avoid cross-file redefinition collisions.
    // Strategy A — declaration lines (begin with "static"): rename every whole-
    // word occurrence on the line (the symbol is the variable being declared).
    // Strategy B — all other lines: rename only the RHS of "=" or a "&" reference.
    // This preserves struct field names such as ".glyph_bitmap" on the left of "=".
    const renames = [
        'kern_classes',
        'kern_left_class_mapping','kern_right_class_mapping','kern_class_values',
        'cmaps',
        'unicode_list_1','unicode_list_2','unicode_list_3',
        'glyph_bitmap','glyph_dsc',
        'font_dsc','cache',
    ];
    for (const n of renames) {
        const replacement = n + suf;
        c = c.split('\n').map(line => {
            if (/^\s*static\b/.test(line) && line.includes(n)) {
                // A: declaration — replace all whole-word occurrences
                const re = new RegExp(`(?<![\\w])${n}(?![\\w])`, 'g');
                return line.replace(re, replacement);
            }
            // B: assignment/initialiser — only RHS of "=" or "&" reference
            let updated = line.replace(
                new RegExp(`(?<==\\s*)${n}(?![\\w])`, 'g'),
                replacement
            );
            updated = updated.replace(
                new RegExp(`(?<![\\w.])&${n}(?![\\w])`, 'g'),
                '&' + replacement
            );
            return updated;
        }).join('\n');
    }
    fs.writeFileSync(outPath, c, 'utf8');
    const kb = Math.round(fs.statSync(outPath).size / 1024, 1);
    console.log('  V ' + outName + ' (' + kb + ' KB)');
}
console.log('All done!');
"@

Set-Content -Path "$WORK_DIR\bake.js" -Value $bakeJs -Encoding UTF8
node "$WORK_DIR\bake.js"
if ($LASTEXITCODE -ne 0) {
    Write-Host "  X Baking failed" -ForegroundColor Red
    exit 1
}
Write-Host ""

# ====== 7. Summary ======
Write-Host "[7/7] Verifying output..." -ForegroundColor Yellow
foreach ($f in @("lv_font_sans16.c","lv_font_sans20.c","lv_font_sans28.c","lv_font_sans_bold48.c")) {
    $p = Join-Path $OUT_DIR $f
    if (Test-Path $p) {
        $kb = [math]::Round((Get-Item $p).Length / 1KB, 1)
        Write-Host "    V $f ($kb KB)" -ForegroundColor Green
    } else {
        Write-Host "    X $f MISSING" -ForegroundColor Red
    }
}
Write-Host ""

Write-Host "=== Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. lv_conf.h  already updated with LV_FONT_CUSTOM_DECLARE" -ForegroundColor White
Write-Host "  2. lvgl_demo.ino already updated with #include and font替换" -ForegroundColor White
Write-Host "  3. Recompile and upload" -ForegroundColor White
Write-Host ""
