/**
 * @file lvgl_st7789_driver.h
 *
 * 把现有项目里的 Adafruit_ST7789 (`tft`) 适配成 LVGL 显示驱动。
 *
 * 设计要点：
 *   1. 不改动现有 SPI / TFT 硬件接线（沿用 s3.ino 里的 TFT_SCL/TFT_SDA/TFT_DC/TFT_CS）
 *   2. 复用现有 `tft` 全局对象，不重新初始化屏幕
 *   3. flush callback 直接把 LVGL 行缓冲 DMA 到 ST7789，CPU 利用率最低
 *   4. 把 lv_tick_inc() 接到现有的 1ms 定时器（millis）——
 *      用户 loop 里每帧调一次 lv_timer_handler() 即可，不用 FreeRTOS 定时器
 *
 * 集成步骤（demo 里 lvgl_demo.ino 会展示）：
 *   - #include "lvgl_st7789_driver.h"
 *   - 在 setup() 里：
 *       tft.init(240, 240);  tft.setRotation(1);  tft.setSPISpeed(40000000);
 *       lvgl_driver_init(&tft);
 *   - 在 loop() 里：
 *       lv_timer_handler();
 *
 * 与现有 s3.ino 共存方案：
 *   - 你可以选择 "完全替换" 或 "双轨"——demo 用的是完全替换（只看 LVGL）
 *   - 后期迁移 s3.ino 时，renderCurrentDisplayBase() 调用 lv_screen_load(...)
 *     替换原来整屏 fillScreen 重画，updateDynamicElements() 改成 lv_obj_set_xxx()
 *     局部更新。原来的差量重绘代码（fillRect + setCursor）全删，LVGL 自动管脏区。
 */

#ifndef LVGL_ST7789_DRIVER_H
#define LVGL_ST7789_DRIVER_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL + 把 ST7789 接到 LVGL 显示驱动上
 * @param tft_ptr  现有项目中已初始化好的 Adafruit_ST7789 指针
 *                 （一般传全局变量 &tft）
 */
void lvgl_driver_init(Adafruit_ST7789* tft_ptr);

/**
 * 在每帧循环里调用。处理 LVGL 内部定时器（动画 / 渲染任务）
 * 调用频率建议 5–33ms 一次（demo 里 delay(5)）
 */
void lvgl_driver_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_ST7789_DRIVER_H */