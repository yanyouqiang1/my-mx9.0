# MX9.0 固件项目

本目录包含 MX9.0 项目的所有 ESP32 固件代码，采用 PlatformIO 构建系统。

## 目录结构

```
firmware/
├── c3/          # ESP32-C3 LED 控制器 + 蓝牙 OTA
├── s3/          # ESP32-S3 USB 声卡 + I2S 音频 + WiFi OTA
└── e3/          # ESP32-S3 蓝牙音频网关 + WiFi OTA
```

## 硬件平台

| 型号 | 芯片 | Flash | PSRAM | 用途 |
|------|------|-------|-------|------|
| C3  | ESP32-C3 | 4MB | 无 | LED 控制器 + 蓝牙 OTA |
| S3  | ESP32-S3 | 16MB | 8MB | USB 声卡 + I2S 音频 |
| E3  | ESP32-S3 | 4MB | | 蓝牙音频网关 |


E3配置信息
Auto-detected: /dev/cu.usbserial-110
Uploading .pio/build/esp32dev/firmware.bin
esptool.py v4.11.0
Serial port /dev/cu.usbserial-110
Connecting.....
Chip is ESP32-D0WD-V3 (revision v3.1)
Features: WiFi, BT, Dual Core, 240MHz, VRef calibration in efuse, Coding Scheme None
Crystal is 40MHz
MAC: 88:f1:55:a2:4d:f4

## 编译前准备

### 1. 安装 PlatformIO

推荐使用 VS Code 扩展 [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)，或使用命令行：

```bash
# 使用 pip 安装
pip install platformio

# 或使用 homebrew (macOS/Linux)
brew install platformio
```

### 2. 安装 USB 驱动

根据你的开发板，可能需要安装 USB 驱动：

- **ESP32-C3 / ESP32-S3 DevKitC-1**: 通常使用 CH340/CH343 串口芯片
  - Windows: 安装 [CH340/CH343 驱动](https://www.wch.cn/downloads/CH343SER_EXE.html)
  - macOS: 通常会自动识别
  - Linux: 通常已内置驱动

## 编译固件

### 使用 VS Code + PlatformIO 扩展

1. 用 VS Code 打开 `firmware` 目录
2. 在底部状态栏可以看到当前环境（如 `esp32-c3-devkitc-1`）
3. 点击编译按钮或使用快捷键 `Ctrl+Alt+B`（macOS: `Cmd+Alt+B`）

### 使用命令行

```bash
# 进入对应固件目录
cd firmware/c3    # C3 固件
cd firmware/s3    # S3 固件
cd firmware/e3    # E3 固件

# 编译
pio run

# 只编译（不链接）
pio run --target buildfs  # 编译文件系统
pio run --target compile  # 只编译代码
```

## 上传固件

### 方式一：USB 串口上传（推荐开发时使用）

```bash
# 上传固件到开发板
pio run --target upload

# 上传固件并打开串口监视器
pio run --target monitor
```

**串口监视器默认参数：**
- 波特率：115200
- 数据位：8
- 停止位：1
- 无校验

### 方式二：WiFi OTA 更新（适用于已部署设备）

固件内置 OTA 功能，可通过网络更新：

```bash
# 通过 OTA 上传
pio run --target upload --upload-port <设备IP地址>

# 示例
pio run --target upload --upload-port 192.168.1.100
```

### 方式三：蓝牙 OTA（仅 C3 支持）

C3 固件支持蓝牙 OTA 更新，可通过手机 App 或 BLE 命令触发。

## 各模块说明

### C3 - LED 控制器

- **功能**：控制 WS2812 LED 灯带，支持多种灯光效果
- **通信**：蓝牙 OTA 更新
- **分区**：4MB Flash，支持单 OTA 分区

### S3 - USB 声卡

- **功能**：
  - USB Audio Class 1.0/2.0 声卡
  - I2S 输入/输出音频接口
  - UART 控制协议（用于控制 E3）
- **通信**：WiFi OTA 更新
- **分区**：16MB Flash，支持双 OTA 分区 + SPIFFS

### E3 - 蓝牙音频网关

- **功能**：
  - 接收蓝牙音频（A2DP Sink）
  - 通过 I2S 输出到 S3 声卡
  - OLED 显示状态
  - 温湿度传感器（SHT31）
- **通信**：WiFi OTA 更新
- **分区**：16MB Flash，支持双 OTA 分区 + SPIFFS

## 常见问题

### Q: 编译报错 "board not found"

确保已安装正确的 PlatformIO 开发板包：

```bash
pio pkg install
```

### Q: 上传失败 "Failed to connect"

1. 检查 USB 线是否支持数据传输（部分充电线只支持充电）
2. 确保开发板处于下载模式（按住 BOOT 按钮再按 RESET）
3. 检查串口驱动是否正确安装
4. 确认串口号是否正确

### Q: 串口监视器显示乱码

检查波特率设置是否为 115200：

```bash
pio device monitor --baud 115200
```

### Q: WiFi OTA 上传失败

1. 确保设备和电脑在同一网络
2. 检查防火墙设置
3. 确认设备已连接 WiFi 并能 ping 通

## 分区表

各固件使用自定义分区表以支持 OTA：

- **C3**: 4MB Flash，APP0 + APP1 + SPIFFS
- **S3/E3**: 4MB Flash，APP0 + APP1 + SPIFFS

详细分区信息见各模块 `src/partitions.csv` 文件。

## 版本信息

固件版本定义在 `platformio.ini` 的 `build_flags` 中：

```
-DBUILD_FIRMWARE_VERSION="1.0.0"
```

## 技术参考

- [PlatformIO 文档](https://docs.platformio.org/)
- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/)
- [ESP32-S3 技术规格](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_cn.pdf)
- [ESP32-C3 技术规格](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_cn.pdf)
