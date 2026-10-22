#!/usr/bin/env bash
# 一键流程：烧固件 → 【拔线再插线】→ 自动扫到 BLE → 验证三个现象
#
# 用法：tools/flash_and_verify.sh [环境名]
#   默认环境 esp32-s3-devkitc-1（产品固件）
#
# 需要人配合的只有一步：
#   **烧完之后把 USB 线拔掉再插回去**（不要按 EN！）
#
# ⚠ 为什么必须拔插而不是按 EN：
#   实测（用 stub 读寄存器确认）：ESP32-S3 的 USB 会话在**复位那一瞬间**
#   会把 IO0 压低，芯片于是又落回 ROM 下载模式 —— 插着线按 EN 也是一样。
#   拔线再插是上电复位，IO0 是物理高电平（GPIO_IN bit0=1 实测确认），
#   芯片才会正常从 flash 启动应用。
set -u

ENV_NAME="${1:-esp32-s3-devkitc-1}"
DIR="/Users/yanyouqiang/project/my-mx9.0/archives/6.0/lvgl_s3"
LOG="/tmp/flash_verify.log"

# 实时把输出打到屏幕上，同时存档
exec > >(tee -a "$LOG") 2>&1
echo "=== $(date '+%H:%M:%S') 开始（环境 $ENV_NAME）==="

# 端口上不能有别的进程占着，否则 esptool 会报 "multiple access on port"
pkill -f serial_logger.py 2>/dev/null
pkill -f ttl_logger.py 2>/dev/null
pkill -f serial_console.py 2>/dev/null
pkill -f diag_ble.py 2>/dev/null
sleep 1

cd "$DIR" || exit 1

# ---------- ① 找端口 ----------
P=""
for i in $(seq 1 60); do
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  [ -n "$P" ] && break
  sleep 1
done
if [ -z "$P" ]; then
  echo "✗ 没找到 /dev/cu.usbmodem* —— 芯片不在 USB 下载模式。"
  echo "  办法：按住 BOOT + 按一下 EN，然后放开 BOOT；再跑一次本脚本。"
  exit 1
fi
echo "=== $(date '+%H:%M:%S') 串口 $P，开始烧录 ==="

# ---------- ② 烧录 ----------
pio run -e "$ENV_NAME" -t upload --upload-port "$P" 2>&1 | tail -8
rc=${PIPESTATUS[0]}
if [ "$rc" != "0" ]; then
  echo "✗ 烧录失败 rc=$rc"
  exit 1
fi
echo "=== $(date '+%H:%M:%S') 烧录完成 ==============="

# ---------- ③ 等应用起来 ----------
echo ">>> 现在请把 **USB 线拔掉再插回去**（不要按 EN）"
echo ">>> 脚本在扫 BLE，检测到应用起来就会自动继续……"
/usr/bin/python3 tools/ble_wait.py 180
if [ $? != "0" ]; then
  echo "✗ 拔插之后没扫到设备 —— 应用没起来"
  exit 1
fi
sleep 3

# ---------- ④ 验证三个现象 ----------
echo "=== $(date '+%H:%M:%S') 开始验证 ==="
/usr/bin/python3 tools/verify_fix.py 2>&1 | tail -60
echo "=== $(date '+%H:%M:%S') 验证结束 ==="
