# YYQ-MX9.0 LVGL 核心版设计

## 目标
在 ESP32-S3 键盘上用 LVGL 实现简约美观的极客仪表盘，保留键盘矩阵和时间显示功能。

## 硬件
- ESP32-S3-N16R8 + ST7789 240x240
- MCP23017 键盘矩阵
- SHT31 温湿度传感器

## 显示：极客仪表盘（简约纯黑）
- 深黑背景 #000000
- 白色/青色数字文字
- 顶部：NUM/CAPS/SCROLL LED 状态
- 中央：大号时钟 (Montserrat 48)
- 日期：YYYY/MM/DD 星期
- 温湿度：底部小字显示
- 击键统计：底部小字显示

## 功能（核心优先）
1. 键盘矩阵扫描（10x16）
2. USB HID 键盘输入
3. 时间显示（NTP同步/手动设置）
4. 温湿度显示（SHT31）
5. 简化菜单（3项）
6. HUD 通知提示

## 字体
- Montserrat 字体（英文数字）
- simsun 16px（中文标签）

## 文件结构
- lvgl_s3/src/lvgl_s3.ino - 主程序
- lvgl_s3/src/lv_conf.h - LVGL配置
- lvgl_s3/src/lvgl_st7789_driver.cpp/h - 驱动
- lvgl_s3/src/lv_font_simsun_16_cjk.c - 中文字体

## 状态
- [x] 设计完成
- [ ] 实现中
