#!/usr/bin/env bash
# 等板子进下载模式 → 烧探针固件 → 验证串口节点常驻
#
# 为什么先烧探针：
#   产品固件用 TinyUSB HID 抢走了 S3 的 USB PHY，macOS 上看不到任何串口节点，
#   于是"产品固件在跑"这个状态下**没法再烧任何东西**（没有串口 = esptool 连不上）。
#   探针固件用 USB-Serial-JTAG（ARDUINO_USB_MODE=1），节点常驻，而且 DTR/RTS
#   的复位脉冲能随时把自己踹回下载模式 —— 烧完之后我就完全自主了。
set -u

PORT_GLOB="/dev/cu.usbmodem*"
PROBE_DIR="/Users/yanyouqiang/project/my-mx9.0/archives/6.0/lvgl_s3/tools/ble_probe"
LOG="/tmp/flash_probe.log"

echo "[watch] 等 $PORT_GLOB 出现（最长 180 秒）…" | tee -a "$LOG"
P=""
for i in $(seq 1 180); do
  P=$(ls $PORT_GLOB 2>/dev/null | head -1)
  if [ -n "$P" ]; then echo "[watch] 找到串口: $P" | tee -a "$LOG"; break; fi
  sleep 1
done
if [ -z "$P" ]; then echo "[watch] 超时，没等到串口节点" | tee -a "$LOG"; exit 1; fi

# 等一下，让 ROM 枚举稳定
sleep 2

echo "[watch] 烧探针固件到 $P …" | tee -a "$LOG"
cd "$PROBE_DIR" || exit 1
pio run -t upload --upload-port "$P" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
echo "[watch] pio upload 返回 $rc" | tee -a "$LOG"
exit $rc
