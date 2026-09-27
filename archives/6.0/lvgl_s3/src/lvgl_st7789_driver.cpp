/**
 * @file lvgl_st7789_driver.cpp
 *
 * LVGL -> Adafruit_ST7789 flush 适配层
 *
 * 关键细节：
 *   1. LVGL 8.x 的 flush_cb 签名是 void flush(lv_disp_drv_t*, const lv_area_t*, lv_color_t*)
 *      lv_color_t 在 COLOR_DEPTH=16 时就是 uint16_t，与 ST7789 像素格式 RGB565 完全一致
 *      直接强转指针 + memcpy 即可，零格式转换
 *
 *   2. setAddrWindow 一次设好矩形，writePixels 整块刷——比一行一行 setAddrWindow 快 10 倍
 *
 *   3. 必须调用 lv_disp_flush_ready(&disp_drv) 告诉 LVGL "这块已经刷完"
 *      否则 LVGL 不会继续推送下一块（最常见的卡屏 bug）
 *
 *   4. 用 static lv_disp_draw_buf_t + static lv_color_t buf 放 PSRAM
 *      双缓冲下需要 2*240*10*2 = 9.6KB（240x10 行高），对 8MB PSRAM 完全可以
 *      demo 选单缓冲 240x40 = 19.2KB，先稳。后续生产场景切双缓冲。
 */

#include "lvgl_st7789_driver.h"
#include <esp_heap_caps.h>

// 单缓冲：240 宽 × 40 行 × 2 字节 = 19.2 KB，放 PSRAM
// 想切双缓冲改 80 行；想更大 flush 块改 60/80
#define BUF_LINES 40
static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf_1 = NULL;
static lv_disp_drv_t disp_drv;
static lv_disp_t* disp = NULL;

// 我们持有的 tft 引用
static Adafruit_ST7789* g_tft = NULL;

/* -------- flush: LVGL -> ST7789 -------- */
static void st7789_flush_cb(lv_disp_drv_t* d, const lv_area_t* area, lv_color_t* color_p) {
    if (g_tft == NULL) {
        lv_disp_flush_ready(d);
        return;
    }

    uint16_t w = (area->x2 - area->x1 + 1);
    uint16_t h = (area->y2 - area->y1 + 1);

    g_tft->startWrite();
    g_tft->setAddrWindow(area->x1, area->y1, w, h);

    // Adafruit_ST7789 在不同版本里 "写一批像素" 的 API 名不一样：
    //   - 1.7.x 老:   pushColors(uint16_t*, uint32_t, bool)
    //   - 1.8.x+:     writePixels(uint16_t*, uint32_t, bool)  (Adafruit_SPITFT 基类方法)
    //   - 极少数老:    pushColor(uint16_t) 一次一个
    // 用户报错的版本既没 pushColors 也没定义 ARDUINO_ADAFRUIT_ST7789_HAS_WRITE_PIXELS，
    // 但 writePixels 是 Adafruit_SPITFT 基类方法，所有现代版本都有。
    // 我们用基类指针调它，保证跨版本兼容。
    uint32_t total = (uint32_t)w * h;
    uint16_t* buf16 = (uint16_t*)color_p;
    Adafruit_SPITFT* spitft = static_cast<Adafruit_SPITFT*>(g_tft);
    spitft->writePixels(buf16, total, true);

    g_tft->endWrite();

    // **必须**告诉 LVGL "这块刷完了"，否则 LVGL 卡死不再调度
    lv_disp_flush_ready(d);
}

/* -------- tick: 在 loop 里调 lvgl_driver_loop 时推进 -------- */
// LVGL 8.x 的 tick 机制有三种：
//   1) 默认：lv_tick_inc(uint32_t) 在定时器里手动调用
//   2) LV_USE_TICK_CUSTOM=1：自己设回调，但 8.4 这个 API 是函数指针不是函数，
//      `lv_tick_set_cb(my_cb)` 这种调用方式会报错
//   3) 我们这里用 (1)，在 lvgl_driver_loop 里调 lv_tick_inc() 推进时间
//      简单、100% 兼容任何 8.x 版本，不用操心 LV_USE_TICK_CUSTOM 开关
static uint32_t lvgl_last_tick_ms = 0;
void lvgl_driver_init(Adafruit_ST7789* tft_ptr) {
    g_tft = tft_ptr;

    // 1. LVGL 自身初始化（必须在任何 lv_xxx API 前调用）
    lv_init();

    // 2. 分配 framebuffer 到 PSRAM（heap_caps_malloc 优先用 PSRAM，失败回落普通 heap）
    size_t buf_bytes = 240 * BUF_LINES * sizeof(lv_color_t);
    buf_1 = (lv_color_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf_1 == NULL) {
        // PSRAM 没开 / 不够 → 回退到普通 heap
        buf_1 = (lv_color_t*)malloc(buf_bytes);
        Serial.printf("[LVGL] WARN: PSRAM alloc failed, falling back to internal heap (size=%u)\n", buf_bytes);
    } else {
        Serial.printf("[LVGL] framebuffer @ PSRAM, size=%u bytes\n", buf_bytes);
    }
    if (buf_1 == NULL) {
        Serial.println("[LVGL] FATAL: no memory for framebuffer");
        return;
    }

    // 单缓冲；想双缓冲再 malloc 一块 buf_2 然后 lv_disp_draw_buf_init(..., buf_2, ...)
    lv_disp_draw_buf_init(&draw_buf, buf_1, NULL, 240 * BUF_LINES);

    // 3. 注册显示驱动
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = st7789_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    // LVGL 8.4 已经移除了 disp_drv.user_data，要存 tft 指针用全局变量 g_tft 即可
    disp = lv_disp_drv_register(&disp_drv);

    // 4. tick 初始化（用 lv_tick_inc 方式，在 lvgl_driver_loop 里推进）
    lvgl_last_tick_ms = millis();

    Serial.println("[LVGL] driver ready");
}

void lvgl_driver_loop(void) {
    // 推进 LVGL 内部时钟：用 elapsed 毫秒数告诉 LVGL 过了多久
    uint32_t now = millis();
    uint32_t elapsed = now - lvgl_last_tick_ms;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        lvgl_last_tick_ms = now;
    }
    // 让 LVGL 处理内部定时器：动画推进、脏区调度、控件定时任务
    lv_timer_handler();
}
