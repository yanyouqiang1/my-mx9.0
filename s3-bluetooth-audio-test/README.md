# S3 蓝牙音频网关 测试版本

用于测试蓝牙音频网关功能，无需 MX9.0 键盘硬件。

## 硬件要求

- ESP32-S3 开发板 (如 ESP32-S3-DevKitC-1)
- I2S DAC (可选，如 PCM5102) 连接到 GPIO 18/19/46
  - GPIO 18: BCK
  - GPIO 19: WS
  - GPIO 46: DOUT

## 功能

- ✅ 蓝牙 A2DP Sink - 接收蓝牙耳机音频
- ✅ USB Audio Device - 模拟 USB 声卡
- ✅ 音频播放 - 蓝牙耳机 → ESP32-S3 → 电脑
- ✅ 音频录音 - 电脑 → ESP32-S3 → I2S DAC (可选)

## 快速开始

### 1. 编译
```bash
cd s3-bluetooth-audio-test
pio run
```

### 2. 上传
```bash
pio run --target upload
```

### 3. 打开串口监视器
```bash
pio device monitor
```

## 测试步骤

1. 上传后，打开串口监视器 (115200 baud)
2. 应该看到类似输出：
   ```
   ===========================================
     S3 蓝牙音频网关 测试版本
   ===========================================

   [Setup] 初始化 USB Audio...
   [Setup] 初始化 I2S 音频输出...
   [Setup] 初始化蓝牙音频...
   [Setup] 创建音频管线任务...
   [Setup] 创建蓝牙音频任务...

   ===========================================
     初始化完成！
   ===========================================
   ```

3. 用手机搜索蓝牙设备 `YYQ-MX9.0-Audio`

4. 连接后播放音乐

5. 电脑应该能看到 USB 声卡设备

## 串口命令

通过串口可以发送命令：

| 命令 | 功能 |
|------|------|
| `BTON` | 开启蓝牙音频 |
| `BTOFF` | 关闭蓝牙音频 |
| `SCAN` | 扫描蓝牙设备 |

## 接线图 (可选 I2S DAC)

```
ESP32-S3          PCM5102 DAC
--------          ----------
GPIO 18  ────────  BCK (LRCK)
GPIO 19  ────────  WS (DIN)
GPIO 46  ────────  DOUT (BCK)
3.3V   ────────   VIN
GND    ────────   GND
```

## 项目结构

```
s3-bluetooth-audio-test/
├── s3-bt-audio-test.ino    # 主程序
├── platformio.ini            # 构建配置
├── README.md
└── components/
    ├── ring_buffer/         # 线程安全环形缓冲区
    ├── bt_audio/            # 蓝牙音频模块
    ├── usb_audio/           # USB 音频模块
    └── audio_output/        # I2S 输出模块
```

## 故障排除

### 编译失败
- 确保安装了 PlatformIO
- 确保 ESP32-S3 开发板支持包已安装

### 蓝牙找不到设备
- 检查串口日志是否有错误
- 确保 ESP32-S3 的蓝牙天线连接正常

### 没有声音
- 如果连接了 I2S DAC，检查接线
- 检查 DAC 是否正确供电
- 电脑端是否选择了正确的音频设备
