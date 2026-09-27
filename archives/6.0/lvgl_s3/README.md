# LVGL 大字时钟 — 全新美化版 (lvgl_s3)

基于 ESP32-S3-N16R8 + ST7789 240x240 + LVGL 8.4.x 的现代化大字时钟固件。

## 与旧代码的关系

| 目录 | 说明 |
|------|------|
| `s3/s3.ino` | **旧版固件**（完整功能：BLE HID / 键盘矩阵 / SHT31 / WS2812 / ST7789），U8G2 显示，保留用于回退 |
| `lvgl_s3/` | **新版固件**，LVGL 重构，仅保留显示相关逻辑，界面全面美化 |

## 视觉效果

- **渐变背景**：纵向深蓝 → 深紫
- **主时钟**：48px 思源黑体 Bold，霓虹青色 + 光晕
- **秒数**：28px 小字，跟在时钟右下方
- **分钟进度条**：细长横条，显示当前分钟进度
- **日期**：20px 柔和蓝紫色
- **锁状态 LED**：NUM / CAP / SCR 圆形图标 + 标签
- **底部信息栏**：温度（暖橙）/ 湿度（浅蓝）/ 品牌

## 文件结构

```
lvgl_s3/
├── lvgl_s3.ino              ← 主入口（美化版时钟 UI）
├── lv_conf.h                 ← LVGL 配置
├── lvgl_st7789_driver.cpp    ← ST7789 flush 适配层
├── lvgl_st7789_driver.h      ← 驱动头文件
└── ../deps/                  ← 思源黑体 SC 子集（共享目录）
    ├── lv_font_sans16.c
    ├── lv_font_sans20.c
    ├── lv_font_sans28.c
    └── lv_font_sans_bold48.c
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

## lv_conf.h 放位

同 lvgl_demo，请将 `lv_conf.h` 复制到 `libraries/lvgl/lv_conf.h`（覆盖模板）。

## 与 s3.ino 的集成

lvgl_s3 是纯显示 demo，验证 LVGL + ST7789 正常工作后，可将 `lvgl_st7789_driver.cpp/h` 合并到 s3.ino，并逐步将 `renderCurrentDisplayBase()` 迁移到 LVGL 驱动的显示函数。
