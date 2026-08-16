# S3 蓝牙音频网关

将 ESP32-S3 模拟成蓝牙耳机中转网关。

## 功能

- 蓝牙耳机连接 S3
- S3 通过 USB 声卡与电脑传输音频
- 支持双向音频（播放+录音）
- 可配置连接的蓝牙耳机

## 引脚连接

- I2C SDA: GPIO 14
- I2C SCL: GPIO 13
- UART RX: GPIO 10
- UART TX: GPIO 9

## 编译

```bash
platformio run
```

## 上传

```bash
platformio run --target upload
```
