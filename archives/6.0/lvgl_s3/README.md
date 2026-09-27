# LVGL 重构版固件 (lvgl_s3)

基于 ESP32-S3-N16R8 + ST7789 240x240 + LVGL 8.4.x 全面重构的固件。

## 与旧代码的关系

| 目录 | 说明 |
|------|------|
| `s3/s3.ino` | **旧版固件**（U8G2 显示，170KB），完整保留用于回退 |
| `lvgl_s3/` | **新版固件**，LVGL 重构，保留全部原有功能（BLE HID / 键盘矩阵 / SHT31 / WS2812 / 闹钟 / 菜单等），仅替换显示层 |

## 主屏风格（6 种）

1. **极客仪表盘** — 顶部大时钟 + 日期 + 温湿度 + 击键数 + NUM/CAP/SCR 状态栏
2. **大时钟** — 纯大字时钟，极简风格，日期副显示
3. **击键监控** — 居中大字击键计数器，适合极客风格
4. **壁纸模式** — 居中 Logo + 滚动字幕，装饰性
5. **信息面板** — 4 格卡片（时间/温度/湿度/击键/运行时间），信息密度高
6. **律动模式** — 音乐节奏可视化（占位，未来可接入音频数据）

## 字体说明

使用 LVGL 内置 Montserrat 字体（ASCII 全覆盖），无需外部字库文件。
如需中文界面，可运行 `deps/bake_fonts.ps1` 从 TUNA 镜像下载思源黑体 SC OTF 并烘焙为 LVGL C 字库。

## 文件结构

```
lvgl_s3/
├── lvgl_s3.ino              ← 主入口（LVGL 重构版，保留全部原有功能）
├── lv_conf.h                 ← LVGL 配置（使用内置 Montserrat 字体）
├── lvgl_st7789_driver.cpp    ← ST7789 flush 适配层
└── lvgl_st7789_driver.h      ← 驱动头文件

deps/
└── bake_fonts.ps1           ← 可选：烘焙思源黑体 SC 字库（当前不使用）

s3/
└── s3.ino                   ← 旧版固件（U8G2，完整备份）
```

## 烧录准备

1. Arduino IDE 库：`lvgl` (8.4.x)、`Adafruit ST7789 Library`、`Adafruit GFX Library`
2. 开发板：ESP32S3 Dev Module
3. 关键设置：
   - PSRAM: `OPI PSRAM`
   - Flash Size: `16MB (128Mb)`
   - Partition: `16MB Flash (3MB APP/9.9MB FATFS)`
   - USB CDC On Boot: `Enabled`
   - Upload Speed: `921600`
4. 将 `lvgl_s3/` 文件夹放入 Arduino 工程目录

## lv_conf.h 放置

请将 `lv_conf.h` 复制到 `libraries/lvgl/lv_conf.h`（覆盖模板），或放在工程目录与 ino 同级。
