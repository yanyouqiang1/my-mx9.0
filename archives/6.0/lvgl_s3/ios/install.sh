#!/bin/bash
# 把 MESender 装到真机上。
#
#   ./install.sh              # 自动挑第一台可用的 iPhone
#   ./install.sh <UDID>       # 指定设备
#
# ⚠ 前置条件（只有一次）：
#   Xcode → Settings → Accounts → + → Apple Account
#   用 **374580618@qq.com** 登录一次。
#   登录之后 Xcode 才能为这个 bundle ID 自动建 provisioning profile。
#   没登录的话这一步会报 "No Accounts: Add a new account in Accounts settings"。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$HERE/MESender"
TEAM="${DEVELOPMENT_TEAM:-RQ6477DX7S}"
BUNDLE_ID="com.yanyouqiang.mx9.MESender"

# ---- 1. 挑设备 ----
UDID="${1:-}"
if [ -z "$UDID" ]; then
    # devicectl 的 identifier 和 xcodebuild 要的 udid 不是一回事，
    # 这里两个都要拿到：前者用于安装，后者用于 xcodebuild 的 destination。
    LINE=$(xcrun devicectl list devices 2>/dev/null | grep -E "iPhone" | grep -v "^--" | head -1 || true)
    if [ -z "$LINE" ]; then
        echo "✗ 没找到 iPhone。请用数据线连上手机并信任这台 Mac，或者直接传 UDID："
        echo "    ./install.sh <UDID>"
        exit 1
    fi
    # ⚠ 不能用 awk '{print $NF}' 取 Identifier —— Model 列是
    #   "iPhone 15 Pro Max (iPhone16,2)"，**中间有空格**，最后一列是
    #   "(iPhone16,2)" 不是 ID。改成扫出唯一那个 UUID 形状的字段。
    DEV_ID=$(echo "$LINE" | grep -oE '[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}' | head -1)
    # 设备名可能带空格（这台叫 "yyq2 号"），列表里按列切不准 —— 从详情里取。
    NAME=$(xcrun devicectl device info details --device "$DEV_ID" 2>/dev/null \
           | grep -oE 'name: .*' | head -1 | sed 's/^name: //')
    UDID=$(xcrun devicectl device info details --device "$DEV_ID" 2>/dev/null \
           | grep -oE 'udid: [0-9A-Fa-f-]+' | head -1 | awk '{print $2}')
    if [ -z "$UDID" ]; then
        echo "✗ 拿不到设备 UDID，请手动传：./install.sh <UDID>"
        exit 1
    fi
    echo "▸ 目标设备：$NAME  ($UDID)"
else
    # 用户直接给了 UDID（xcodebuild 要的那个），devicectl 的 id 反查一下
    DEV_ID=$(xcrun devicectl list devices 2>/dev/null \
             | grep -i "$UDID" \
             | grep -oE '[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}' | head -1)
    [ -n "$DEV_ID" ] || DEV_ID="$UDID"
    echo "▸ 目标设备 UDID：$UDID"
fi

# ---- 2. 生成 Xcode 工程 ----
# 工程是 xcodegen 从 project.yml 生成的，别手改 pbxproj（会被覆盖）。
echo "▸ 生成 Xcode 工程…"
(cd "$APP_DIR" && xcodegen generate >/dev/null)

# ---- 3. 编译 + 签名 ----
echo "▸ 编译（会顺带让 Xcode 建/刷新 provisioning profile）…"
(cd "$APP_DIR" && xcodebuild \
    -project MESender.xcodeproj \
    -scheme MESender \
    -destination "platform=iOS,id=$UDID" \
    -configuration Debug \
    -derivedDataPath "$APP_DIR/build" \
    DEVELOPMENT_TEAM="$TEAM" \
    -allowProvisioningUpdates \
    build) || {
    echo ""
    echo "✗ 编译失败。最常见的原因是 Xcode 里没有登录 Apple ID："
    echo "    Xcode → Settings → Accounts → + → Apple Account"
    echo "    用 374580618@qq.com 登录，然后再跑一次本脚本。"
    exit 1
}

APP_PATH="$APP_DIR/build/Build/Products/Debug-iphoneos/MESender.app"
[ -d "$APP_PATH" ] || { echo "✗ 找不到 $APP_PATH"; exit 1; }

# ---- 4. 安装 ----
echo "▸ 安装到手机…"
xcrun devicectl device install app --device "$DEV_ID" "$APP_PATH"

# ---- 5. 启动 ----
echo "▸ 启动…"
xcrun devicectl device process launch --device "$DEV_ID" "$BUNDLE_ID" 2>&1 | tail -3 || true

echo ""
echo "✅ 完成。手机上打开「MX9 键盘」即可。"
echo "   第一次会弹蓝牙权限 —— 一定要允许，否则扫不到键盘。"
echo ""
echo "⚠ 提醒：个人开发者（免费账号）的 provisioning profile **只有 7 天有效期**。"
echo "   大约 2026-10-30 之后如果装不上了，重跑一次本脚本刷新即可。"
