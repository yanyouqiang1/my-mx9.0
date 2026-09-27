/**
 * LVGL 大字时钟 — 全新美化版 (lvgl_s3)
 *
 * 基于 ESP32-S3-N16R8 + ST7789 240x240
 *
 * 视觉效果：
 *   - 纵向渐变背景（深蓝 → 深紫）
 *   - 中心超大时钟 + 霓虹青色光晕
 *   - 日期/星期在时钟下方，柔和蓝紫色
 *   - 顶部三个锁状态 LED (NUM/CAP/SCR) 带圆形图标
 *   - 底部信息栏：温度 / 湿度 / 品牌
 *   - 每分钟进度条（浅色细条）
 *
 * 移植自 s3.ino 的核心显示逻辑，UI 用 LVGL 重写，
 * 旧代码完整保留在 s3/ 目录，方便回退。
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "lvgl_st7789_driver.h"

// 字体子集：放在 sketch 外的 deps/，Arduino 不会重复编译
extern const lv_font_t lv_font_sans16;
extern const lv_font_t lv_font_sans20;
extern const lv_font_t lv_font_sans28;
extern const lv_font_t lv_font_sans_bold48;
#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include "../deps/lv_font_sans16.c"
#include "../deps/lv_font_sans20.c"
#include "../deps/lv_font_sans28.c"
#include "../deps/lv_font_sans_bold48.c"

// ================= 屏幕硬件接线（与 s3.ino 一致） =================
#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ================= 全局 UI 对象 =================
static lv_obj_t* lbl_time      = NULL;  // 主时钟
static lv_obj_t* lbl_date      = NULL;  // 日期
static lv_obj_t* lbl_second    = NULL;  // 秒数（小字）
static lv_obj_t* led_num        = NULL;
static lv_obj_t* led_caps       = NULL;
static lv_obj_t* led_scroll     = NULL;
static lv_obj_t* lbl_num        = NULL;
static lv_obj_t* lbl_caps       = NULL;
static lv_obj_t* lbl_scroll     = NULL;
static lv_obj_t* bar_minute     = NULL;  // 分钟进度条
static lv_obj_t* lbl_temp       = NULL;  // 温度
static lv_obj_t* lbl_humidity   = NULL;  // 湿度
static lv_obj_t* lbl_footer     = NULL;  // 底部品牌
static lv_timer_t* timer_ui     = NULL;

// 周几中文
static const char* WEEK_CN[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

// 模拟温湿度（真实数据由 s3.ino 的 SHT31 传感器提供）
static float fakeTemp = 26.5f;
static float fakeHumidity = 62.0f;

// ================= 锁状态（模拟） =================
static bool numLock    = true;
static bool capsLock   = false;
static bool scrollLock = false;

// ================= 时钟刷新 =================
static void update_clock_ui(lv_timer_t* t) {
    (void)t;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);

    char time_buf[16];
    char date_buf[32];
    char sec_buf[8];

    bool valid = (ti && ti->tm_year >= 124);

    if (valid) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d  %s",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
                 WEEK_CN[ti->tm_wday]);
        snprintf(sec_buf, sizeof(sec_buf), ":%02d", ti->tm_sec);

        // 分钟进度条：当前秒 / 60
        uint32_t sec = ti->tm_sec;
        lv_bar_set_value(bar_minute, sec * 100 / 60, LV_ANIM_OFF);
    } else {
        strcpy(time_buf, "12:00");
        strcpy(date_buf, "2026-01-01  周三");
        strcpy(sec_buf,  ":00");
        lv_bar_set_value(bar_minute, 0, LV_ANIM_OFF);
    }

    lv_label_set_text(lbl_time,    time_buf);
    lv_label_set_text(lbl_date,    date_buf);
    lv_label_set_text(lbl_second,   sec_buf);

    // 模拟温湿度小幅波动
    fakeTemp    += (random(-10, 11) * 0.01f);
    fakeHumidity += (random(-5, 6) * 0.1f);
    fakeTemp    = constrain(fakeTemp, 20.0f, 35.0f);
    fakeHumidity = constrain(fakeHumidity, 40.0f, 85.0f);

    static char tmp[16];
    static char hum[16];
    snprintf(tmp, sizeof(tmp), "%.1fC", fakeTemp);
    snprintf(hum, sizeof(hum), "%.0f%%", fakeHumidity);
    lv_label_set_text(lbl_temp,     tmp);
    lv_label_set_text(lbl_humidity, hum);

    // 锁 LED 状态（demo 模式固定闪烁）
    static bool toggle = false;
    toggle = !toggle;
    lv_led_set_brightness(led_num,    numLock    ? (toggle ? 255 : 80)  : 0);
    lv_led_set_brightness(led_caps,   capsLock   ? (toggle ? 200 : 60)  : 0);
    lv_led_set_brightness(led_scroll, scrollLock ? (toggle ? 180 : 50)  : 0);
}

// ================= 构建主界面 =================
static void build_ui(void) {
    lv_obj_t* scr = lv_scr_act();

    // ---- 1. 渐变背景 ----
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x160A25), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);

    // ---- 2. 顶部锁状态 LED ----
    int led_y = 12;
    int led_size = 12;
    int led_gap = 50;

    // 圆角背景卡片
    static lv_style_t card_style;
    lv_style_init(&card_style);
    lv_style_set_bg_opa(&card_style, LV_OPA_30);
    lv_style_set_radius(&card_style, 6);
    lv_style_set_border_width(&card_style, 0);

    lv_obj_t* led_card = lv_obj_create(scr);
    lv_obj_set_size(led_card, 150, 50);
    lv_obj_align(led_card, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_style(led_card, &card_style, 0);
    lv_obj_move_background(led_card);

    auto make_led = [&](int x_offset, lv_color_t c, lv_obj_t** out_led) {
        lv_obj_t* led = lv_led_create(led_card);
        lv_obj_set_size(led, led_size, led_size);
        lv_obj_set_pos(led, x_offset, 8);
        lv_led_set_color(led, c);
        lv_led_set_brightness(led, 180);
        *out_led = led;
    };

    int start_x = 18;
    make_led(start_x,            lv_palette_main(LV_PALETTE_CYAN),    &led_num);
    make_led(start_x + led_gap,  lv_palette_main(LV_PALETTE_GREEN),   &led_caps);
    make_led(start_x + led_gap*2, lv_palette_main(LV_PALETTE_ORANGE), &led_scroll);

    auto make_led_label = [&](int x_offset, const char* txt, lv_color_t c) {
        lv_obj_t* lbl = lv_label_create(led_card);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_sans16, LV_PART_MAIN);
        lv_obj_set_pos(lbl, x_offset - 2, 24);
    };
    make_led_label(start_x,            "NUM", lv_palette_main(LV_PALETTE_CYAN));
    make_led_label(start_x + led_gap,   "CAP", lv_palette_main(LV_PALETTE_GREEN));
    make_led_label(start_x + led_gap*2, "SCR", lv_palette_main(LV_PALETTE_ORANGE));

    // ---- 3. 主时钟标签 ----
    lbl_time = lv_label_create(scr);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_style_text_font(lbl_time, &lv_font_sans_bold48, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0x00E5FF), LV_PART_MAIN);  // 霓虹青

    // 光晕阴影
    lv_obj_set_style_shadow_color(lbl_time, lv_color_hex(0x00AEEF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(lbl_time, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(lbl_time, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(lbl_time, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_blur(lbl_time, 10, LV_PART_MAIN);

    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, -20);

    // ---- 4. 秒数（小字，跟在时钟后） ----
    lbl_second = lv_label_create(scr);
    lv_label_set_text(lbl_second, ":00");
    lv_obj_set_style_text_font(lbl_second, &lv_font_sans28, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_second, lv_color_hex(0x00AACC), LV_PART_MAIN);
    lv_obj_align_to(lbl_second, lbl_time, LV_ALIGN_OUT_RIGHT_MID, 2, -4);

    // ---- 5. 分钟进度条 ----
    bar_minute = lv_bar_create(scr);
    lv_obj_set_size(bar_minute, 200, 3);
    lv_obj_align(bar_minute, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_color(bar_minute, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(bar_minute, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(bar_minute, LV_GRAD_DIR_HOR);
    lv_obj_set_style_radius(bar_minute, 2, LV_PART_MAIN);

    // ---- 6. 日期 ----
    lbl_date = lv_label_create(scr);
    lv_label_set_text(lbl_date, "------  ----");
    lv_obj_set_style_text_font(lbl_date, &lv_font_sans20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0x9090D0), LV_PART_MAIN);
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 24);

    // ---- 7. 底栏：温度 / 湿度 / 品牌 ----
    static lv_style_t footer_style;
    lv_style_init(&footer_style);
    lv_style_set_bg_opa(&footer_style, LV_OPA_20);
    lv_style_set_radius(&footer_style, 4);
    lv_style_set_border_width(&footer_style, 0);

    lv_obj_t* footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 240, 28);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(footer, &footer_style, 0);
    lv_obj_move_background(footer);

    // 分隔线
    static lv_style_t line_style;
    lv_style_init(&line_style);
    lv_style_set_line_color(&line_style, lv_color_hex(0x2A3A5A));
    lv_style_set_line_width(&line_style, 1);
    lv_obj_t* sep = lv_line_create(scr);
    static lv_point_t pts[2] = { {20, 212}, {220, 212} };
    lv_line_set_points(sep, pts, 2);
    lv_obj_add_style(sep, &line_style, 0);

    // 温度图标（简单的 ° 字符）
    lbl_temp = lv_label_create(footer);
    lv_label_set_text(lbl_temp, "26.5C");
    lv_obj_set_style_text_font(lbl_temp, &lv_font_sans16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xFF8C42), LV_PART_MAIN); // 暖橙色
    lv_obj_set_pos(lbl_temp, 18, 6);

    // 湿度
    lbl_humidity = lv_label_create(footer);
    lv_label_set_text(lbl_humidity, "62%");
    lv_obj_set_style_text_font(lbl_humidity, &lv_font_sans16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_humidity, lv_color_hex(0x42C8FF), LV_PART_MAIN); // 浅蓝
    lv_obj_set_pos(lbl_humidity, 80, 6);

    // 品牌
    lbl_footer = lv_label_create(footer);
    lv_label_set_text(lbl_footer, "YYQ-MX9.0  LVGL");
    lv_obj_set_style_text_font(lbl_footer, &lv_font_sans16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x4A5A8A), LV_PART_MAIN);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -5);

    // ---- 8. 启动定时器 ----
    timer_ui = lv_timer_create(update_clock_ui, 1000, NULL);
}

// ================= 辅助：画圆形图标 ----
static void draw_circular_icon(lv_obj_t* parent, int x, int y, int r, lv_color_t c) {
    lv_obj_t* circle = lv_obj_create(parent);
    lv_obj_set_size(circle, r*2, r*2);
    lv_obj_set_pos(circle, x, y);
    lv_obj_set_style_radius(circle, r, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_80, LV_PART_MAIN);
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n=== LVGL S3 Clock (Beauty Edition) ===");
    Serial.printf("PSRAM: %u bytes\n", ESP.getFreePsram());

    // SPI + ST7789 初始化
    tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
    tft.init(240, 240);
    tft.setSPISpeed(40000000);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);

    // 设置一个 demo 起始时间
    {
        struct tm t = {0};
        t.tm_year = 2026 - 1900;
        t.tm_mon  = 9 - 1;
        t.tm_mday = 27;
        t.tm_hour = 8;
        t.tm_min  = 2;
        t.tm_sec  = 0;
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        Serial.printf("[CLOCK] demo time set: %s", ctime(&epoch));
    }

    // LVGL 驱动初始化
    lvgl_driver_init(&tft);

    // 构建 UI
    build_ui();

    Serial.println("[READY] Beautiful clock should be on screen");
}

// ================= LOOP =================
void loop() {
    lvgl_driver_loop();
    delay(5);  // ~200Hz，CPU 几乎 0 占用
}
