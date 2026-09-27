# LVGL 核心版固件 (lvgl_s3)

基于 ESP32-S3-N16R8 + ST7789 240x240 + LVGL 8.4.x

## 风格：简约纯黑

- 深黑背景 #000000
- 白色/青色数字文字
- 顶部 NUM/CAPS/SCROLL LED 状态
- 中央大号时钟 (Montserrat 48)
- 日期 + 温湿度 + 击键统计

## 功能（核心优先）

1. **键盘矩阵** - 10x16 完整扫描
2. **USB HID** - 键盘输入 + 媒体键
3. **时间显示** - 时钟 + 日期
4. **温湿度** - SHT31 传感器
5. **简化菜单** - 切换显示模式
6. **HUD 通知** - 操作反馈

## 显示模式

1. **极客仪表盘** - 时间/温湿度/击键
2. **大字时钟** - 简约大号时间

## 硬件

- ESP32-S3-N16R8
- ST7789 240x240 LCD
- MCP23017 键盘矩阵
- SHT31 温湿度传感器
- WS2812 LED (19颗)

## 烧录

```bash
pio run -t upload
```

## 目录结构

```
lvgl_s3/
├── src/
│   ├── lvgl_s3.ino          ← 主程序
│   ├── lv_conf.h             ← LVGL 配置
│   ├── lvgl_st7789_driver.cpp/h  ← 驱动
│   └── lv_font_simsun_16_cjk.c   ← 中文字体（预留）
├── docs/superpowers/specs/  ← 设计文档
└── platformio.ini            ← 构建配置
```
