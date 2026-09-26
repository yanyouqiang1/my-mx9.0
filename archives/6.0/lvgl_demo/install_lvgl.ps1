# YYQ-MX9.0 LVGL one-click installer
# Downloads LVGL 8.4.0 -> extracts to Arduino libraries -> enables our lv_conf.h
# Usage (PowerShell):  .\install_lvgl.ps1
# If blocked by execution policy:  Set-ExecutionPolicy -Scope Process Bypass; .\install_lvgl.ps1

$ErrorActionPreference = 'Stop'

# ====== Config ======
$LVGL_VERSION = "8.4.0"
$ARDUINO_LIBS = "$env:USERPROFILE\Documents\Arduino\libraries"
$LVGL_DEST = Join-Path $ARDUINO_LIBS "lvgl"
$DOWNLOAD_URL = "https://github.com/lvgl/lvgl/archive/refs/tags/v$LVGL_VERSION.zip"
$ZIP_PATH = Join-Path $env:TEMP "lvgl-$LVGL_VERSION.zip"
$EXTRACT_DIR = Join-Path $env:TEMP "lvgl-extract"

# Our prepared lv_conf.h (in the same folder as this script)
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$OUR_CONF = Join-Path $SCRIPT_DIR "lvgl_demo\lv_conf.h"

# ====== Header ======
Write-Host ""
Write-Host "=== YYQ-MX9.0 LVGL $LVGL_VERSION Installer ===" -ForegroundColor Cyan
Write-Host ""

# ====== 1. Environment check ======
Write-Host "[1/5] Checking environment..." -ForegroundColor Yellow
if (-not (Test-Path $ARDUINO_LIBS)) {
    Write-Host "  X Arduino libraries folder not found: $ARDUINO_LIBS" -ForegroundColor Red
    Write-Host "    Please install Arduino IDE, or edit `$ARDUINO_LIBS in this script." -ForegroundColor Red
    exit 1
}
Write-Host "  Arduino libraries: $ARDUINO_LIBS" -ForegroundColor Green

if (-not (Test-Path $OUR_CONF)) {
    Write-Host "  X Our lv_conf.h not found: $OUR_CONF" -ForegroundColor Red
    Write-Host "    Make sure install_lvgl.ps1 and lvgl_demo\lv_conf.h are in the same folder." -ForegroundColor Red
    exit 1
}
Write-Host "  Our lv_conf.h:    $OUR_CONF" -ForegroundColor Green
Write-Host ""

# ====== 2. Download LVGL ======
Write-Host "[2/5] Downloading LVGL $LVGL_VERSION (~8 MB)..." -ForegroundColor Yellow
if (Test-Path $ZIP_PATH) { Remove-Item $ZIP_PATH -Force }

try {
    $ProgressPreference = 'Continue'
    Invoke-WebRequest -Uri $DOWNLOAD_URL -OutFile $ZIP_PATH -UseBasicParsing -TimeoutSec 120
} catch {
    Write-Host "  X Download failed: $_" -ForegroundColor Red
    Write-Host "    Possible cause: no internet / GitHub blocked." -ForegroundColor Red
    Write-Host "    Manual fix: download v$LVGL_VERSION from https://github.com/lvgl/lvgl/releases" -ForegroundColor Red
    Write-Host "    Extract, rename the inner lvgl-$LVGL_VERSION folder to 'lvgl'," -ForegroundColor Red
    Write-Host "    put it under $ARDUINO_LIBS, then copy $OUR_CONF to $ARDUINO_LIBS\lvgl\lv_conf.h" -ForegroundColor Red
    exit 1
}
$zipMB = [math]::Round((Get-Item $ZIP_PATH).Length / 1MB, 2)
Write-Host "  Downloaded: $zipMB MB" -ForegroundColor Green
Write-Host ""

# ====== 3. Extract ======
Write-Host "[3/5] Extracting to $LVGL_DEST..." -ForegroundColor Yellow
if (Test-Path $EXTRACT_DIR) { Remove-Item $EXTRACT_DIR -Recurse -Force }
Expand-Archive -Path $ZIP_PATH -DestinationPath $EXTRACT_DIR -Force

$extractedDir = Get-ChildItem -Directory $EXTRACT_DIR | Where-Object { $_.Name -like "lvgl-*" } | Select-Object -First 1
if (-not $extractedDir) {
    Write-Host "  X Extracted lvgl-* folder not found" -ForegroundColor Red
    exit 1
}

if (Test-Path $LVGL_DEST) {
    $backupPath = "$LVGL_DEST.bak.$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    Write-Host "  Backing up old install to: $backupPath" -ForegroundColor DarkYellow
    Move-Item $LVGL_DEST $backupPath
}

Move-Item $extractedDir.FullName $LVGL_DEST
Write-Host "  Extracted OK: $LVGL_DEST" -ForegroundColor Green
Write-Host ""

# ====== 4. Enable lv_conf.h ======
Write-Host "[4/5] Enabling lv_conf.h..." -ForegroundColor Yellow
$targetConf = Join-Path $LVGL_DEST "lv_conf.h"
if (Test-Path $targetConf) { Remove-Item $targetConf -Force }

Copy-Item $OUR_CONF $targetConf -Force
Write-Host "  lv_conf.h installed: $targetConf" -ForegroundColor Green
Write-Host ""

# ====== 5. Verify ======
Write-Host "[5/5] Verifying..." -ForegroundColor Yellow
$lvglH = Join-Path $LVGL_DEST "src\lvgl.h"
$confH = Join-Path $LVGL_DEST "lv_conf.h"

$allOK = $true
if (Test-Path $lvglH) {
    Write-Host "  V lvgl.h found" -ForegroundColor Green
} else {
    Write-Host "  X lvgl.h NOT found!" -ForegroundColor Red
    $allOK = $false
}
if (Test-Path $confH) {
    Write-Host "  V lv_conf.h enabled" -ForegroundColor Green
} else {
    Write-Host "  X lv_conf.h NOT in place!" -ForegroundColor Red
    $allOK = $false
}
if (-not $allOK) { exit 1 }

# ====== Cleanup ======
Remove-Item $ZIP_PATH -Force -ErrorAction SilentlyContinue
Remove-Item $EXTRACT_DIR -Recurse -Force -ErrorAction SilentlyContinue

# ====== Done ======
Write-Host ""
Write-Host "=== Installation complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "LVGL $LVGL_VERSION installed at: $LVGL_DEST" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Open Arduino IDE" -ForegroundColor White
Write-Host "  2. Tools > Board: ESP32S3 Dev Module" -ForegroundColor White
Write-Host "  3. Tools > PSRAM: OPI PSRAM (REQUIRED)" -ForegroundColor White
Write-Host "  4. Tools > Flash Size: 16MB (128Mb)" -ForegroundColor White
Write-Host "  5. Tools > Partition Scheme: 16MB Flash (3MB APP/9.9MB FATFS)" -ForegroundColor White
Write-Host "  6. Open lvgl_demo.ino and click Upload" -ForegroundColor White
Write-Host ""
Write-Host "Open Serial Monitor at 115200 baud after upload. Expected output:" -ForegroundColor Yellow
Write-Host "  PSRAM: 8388608 bytes" -ForegroundColor White
Write-Host "  [LVGL] framebuffer @ PSRAM, size=19200 bytes" -ForegroundColor White
Write-Host "  [LVGL] driver ready" -ForegroundColor White
Write-Host ""