#!/usr/bin/env bash
# 用 COM 口（USB-TTL）烧录固件 —— 不改分区表，只刷 app 本体。
#
# 为什么要这么绕（2026-10-22 实测）：
#   这块板的 COM 口只有 TX/RX + RTS→EN，**DTR 没接到 IO0**
#   （四种 DTR/RTS 组合复位后都是 boot:0x8，esptool 报 "Wrong boot mode"），
#   所以没法靠软件进下载模式。
#   但 EN 我们能控（RTS）→ 于是：
#     ① 人按住 BOOT（IO0 拉低）
#     ② 脚本不停复位，其中某一次就采到 IO0=0 → ROM 进下载模式
#     ③ esptool --before no_reset 直接刷 app（不动 bootloader / 分区表）
#     ④ 人松开 BOOT，脚本再复位一次 → 正常从 flash 启动应用
#
# 用法: tools/flash_com.sh [环境名] [端口]
#   环境名默认 esp32-s3-devkitc-1_debug
set -u

DIR="/Users/yanyouqiang/project/my-mx9.0/archives/6.0/lvgl_s3"
ENV_NAME="${1:-esp32-s3-devkitc-1_debug}"
PORT="${2:-/dev/cu.usbmodem5C4C2142561}"
BIN="$DIR/.pio/build/$ENV_NAME/firmware.bin"
ESPTOOL="/usr/bin/python3 $HOME/.platformio/packages/tool-esptoolpy/esptool.py"

cd "$DIR" || exit 1
[ -f "$BIN" ] || { echo "✗ 没有 $BIN，先编：pio run -e $ENV_NAME"; exit 1; }

echo "=== $(date '+%H:%M:%S') 固件: $ENV_NAME → $PORT ==="

# ---------- ① 进下载模式（需要人按住 BOOT）----------
echo ">>> 【请现在按住 BOOT 键不放】脚本会不停复位，最多等 180 秒"
/usr/bin/python3 -u tools/com_enter_download.py "$PORT" 900
if [ $? != "0" ]; then
  echo "✗ 没进下载模式，放弃"
  exit 1
fi

# ---------- ② 刷 app 本体（只写 0x10000，不碰 bootloader 和分区表）----------
echo "=== $(date '+%H:%M:%S') 开始烧录（只写 app，不动分区表）==="
$ESPTOOL --port "$PORT" --chip esp32s3 --before no_reset --after no_reset \
    write_flash 0x10000 "$BIN" 2>&1 | tail -8
rc=${PIPESTATUS[0]}
if [ "$rc" != "0" ]; then
  echo "✗ 烧录失败 rc=$rc"
  exit 1
fi

# ---------- ③ 松开 BOOT，复位启动应用 ----------
echo "=== $(date '+%H:%M:%S') 烧完 >>> 【请松开 BOOT 键】10 秒后我复位启动应用 ==="
sleep 10
echo "=== $(date '+%H:%M:%S') 复位启动应用，以下是 UART0 启动日志 ==="
/usr/bin/python3 -u tools/com_reset_log.py 25 "$PORT"
