/**
 * LVGL 大字时钟 Demo
 *
 * 目标：让你的 ESP32-S3-N16R8 上的 240x240 ST7789 屏幕，立刻显示出
 *       矢量字体 + 抗锯齿 + 阴影 + 渐变背景的现代化大字时钟。
 *
 * 烧录后效果：
 *   - 上半部分：超大号时间 "12:34:56"，矢量字体边缘光滑
 *   - 下半部分：日期 "2026-09-26 周六" + 三个锁状态 LED
 *   - 背景：从深蓝到深紫的纵向渐变
 *
 * 这一版**不依赖** SHT31 / BLE / 键盘矩阵 / BLE / FFat，
 * 纯显示 demo，方便独立验证 LVGL + ST7789 是否正常工作。
 *
 * === 烧录前准备 ===
 *   1. Arduino IDE 装库:
 *      - lvgl       (库管理器搜 "lvgl" → 装 8.4.x)
 *      - Adafruit ST7789 Library
 *      - Adafruit GFX Library
 *   2. 开发板: ESP32S3 Dev Module
 *   3. 关键开关:
 *      Tools > PSRAM: "OPI PSRAM"
 *      Tools > Flash Size: "16MB (128Mb)"
 *      Tools > Partition Scheme: "16MB Flash (3MB APP/9.9MB FATFS)"
 *      Tools > USB CDC On Boot: "Enabled"  (看你偏好)
 *      Tools > Upload Speed: "921600"
 *   4. 把这个文件夹连同子文件夹一起放进 Arduino 工程根目录:
 *      - lvgl_demo.ino
 *      - lvgl_st7789_driver.h
 *      - lvgl_st7789_driver.cpp
 *      - lv_conf.h
 *      - deps/  (字体 .c 文件，不要改名或移动)
 *      → 注意 lv_conf.h 必须放到 Arduino 默认 libraries 路径下能被 lvgl 找到
 *        的位置。两种做法二选一:
 *        A) 简单: 把 lv_conf.h 复制到 D:/Users/<你>/Documents/Arduino/libraries/lvgl/lv_conf.h
 *           （覆盖 lvgl 库自带的 lv_conf_template.h 改名后的版本）
 *        B) 在 lvgl 库自带的 lv_conf_template.h 顶上 #define LV_CONF_SKIP 1
 *           之前你已经改过名字为 lv_conf.h 并且改过配置了。
 *        我们这个 demo 已经把关键开关都设好，直接用即可。
 *
 * === 烧录后 ===
 *   打开串口监视器（115200），会看到:
 *     [LVGL] PSRAM: 8388608 bytes
 *     [LVGL] framebuffer @ PSRAM, size=19200 bytes
 *     [LVGL] driver ready
 *   屏幕出现深色渐变背景 + 居中大字时钟。
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "lvgl_st7789_driver.h"

// 字体子集文件移至 deps/，避免被 Arduino 独立编译成多份。
// deps/ 在 sketch 文件夹之外，不会触发重复链接。
extern const lv_font_t lv_font_sans16;
extern const lv_font_t lv_font_sans20;
extern const lv_font_t lv_font_sans28;
extern const lv_font_t lv_font_sans_bold48;
#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include "../deps/lv_font_sans16.c"
#include "../deps/lv_font_sans20.c"
#include "../deps/lv_font_sans28.c"
#include "../deps/lv_font_sans_bold48.c"

// ================= 屏幕 SPI 接线（与 s3.ino 完全一致） =================
#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ================= 全局对象（demo 用） =================
static lv_obj_t* lbl_time     = NULL;   // 时间标签
static lv_obj_t* lbl_date     = NULL;   // 日期标签
static lv_obj_t* led_num      = NULL;   // NUM 锁
static lv_obj_t* led_caps     = NULL;
static lv_obj_t* led_scroll   = NULL;
static lv_timer_t* timer_ui   = NULL;   // 1秒定时器

// 周几中文映射
static const char* week_cn[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

// ================= 时间格式化（移植自 s3.ino 的逻辑） =================
// 注意：ESP32 没接 RTC 时默认 epoch 从 1970 起算，
// 正式接入项目后这个 time() 应该走 S3 已有的 NTP / 网页同步时间逻辑。
// Demo 模式下：如果时间未同步（year < 2024），用初始化的默认值直接走。
static void update_clock_ui(lv_timer_t* t) {
    (void)t;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);

    char time_buf[16];
    char date_buf[32];

    // 检查时间是否有效（year >= 2024 表示已同步或有默认值）
    // 否则用 demo 初始化的默认时间直接格式化（settimeofday 可能没持久化）
    bool time_valid = (ti && ti->tm_year >= 124); // 124 = 2024

    if (time_valid) {
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", ti);
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %s",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, week_cn[ti->tm_wday]);
    } else {
        // demo 默认时间：2026-09-26 23:04:06
        strcpy(time_buf, "23:04:06");
        strcpy(date_buf, "2026-09-26 周六");
    }

    lv_label_set_text(lbl_time, time_buf);
    lv_label_set_text(lbl_date, date_buf);

    // LED 颜色与亮灭（demo 固定显示状态，看效果）
    // 正式接入时改成读 USB HID 的 lock 状态（s3.ino:130-135）
    static bool toggle = false;
    toggle = !toggle;
    lv_led_set_brightness(led_num,    toggle ? 255 : 80);
    lv_led_set_brightness(led_caps,   150);
    lv_led_set_brightness(led_scroll, 80);
}

// ================= 创建大字时钟 UI =================
static void build_ui(void) {
    // 当前活动屏幕（demo 直接用默认 screen）
    lv_obj_t* scr = lv_scr_act();

    // ---------- 1. 背景：纵向深蓝→深紫渐变 ----------
    // LVGL 默认主题有渐变样式，做法：先清屏黑色，再画两条 lv_obj 做渐变叠层。
    // 简单做法：直接给 scr 背景色用 lv_obj_set_style_bg_color(scr, ...)。
    // 想要渐变：用 lv_obj_set_style_bg_grad_color + bg_grad_dir。
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1A2F), LV_PART_MAIN);     // 深蓝
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x1A0A2F), LV_PART_MAIN); // 深紫
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);

    // ---------- 2. 顶部三个 LED 锁状态 ----------
    int led_y = 18;
    int led_size = 14;

    auto make_led = [&](int x, lv_color_t c, lv_obj_t** out) {
        lv_obj_t* led = lv_led_create(scr);
        lv_obj_set_size(led, led_size, led_size);
        lv_obj_set_pos(led, x, led_y);
        lv_led_set_color(led, c);
        lv_led_set_brightness(led, 180);
        *out = led;
    };
    make_led(60,  lv_palette_main(LV_PALETTE_CYAN),    &led_num);
    make_led(113, lv_palette_main(LV_PALETTE_GREEN),   &led_caps);
    make_led(166, lv_palette_main(LV_PALETTE_ORANGE),  &led_scroll);

    // LED 下方小字 (LVGL Montserrat 矢量字 vs wqy16 点阵字 视觉对比)
    // 不用中文，因为目前 lv_conf 没烘焙中文字体；正式版会换思源黑体。
    // 这里用英文 NUM/CAPS/SCR 示意。
    auto make_caption = [&](int x, const char* txt, lv_color_t c) {
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_sans16, LV_PART_MAIN);
        lv_obj_set_pos(lbl, x + 1, led_y + led_size + 2);
    };
    make_caption(60,  "NUM", lv_palette_main(LV_PALETTE_CYAN));
    make_caption(113, "CAP", lv_palette_main(LV_PALETTE_GREEN));
    make_caption(166, "SCR", lv_palette_main(LV_PALETTE_ORANGE));

    // ---------- 3. 中间超大时间 ----------
    lbl_time = lv_label_create(scr);
    lv_label_set_text(lbl_time, "--:--:--");
    // 时钟用 48px 思源黑体 Bold
    lv_obj_set_style_text_font(lbl_time, &lv_font_sans_bold48, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0x00E5FF), LV_PART_MAIN); // 霓虹青

    // 阴影 + 透明度模拟霓虹光晕
    lv_obj_set_style_shadow_color(lbl_time, lv_color_hex(0x00B0FF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(lbl_time, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(lbl_time, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(lbl_time, 2, LV_PART_MAIN);

    // 居中：先 set_size 然后 set_align
    lv_obj_set_size(lbl_time, 220, 60);
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, -10);

    // ---------- 4. 下方日期 ----------
    lbl_date = lv_label_create(scr);
    lv_label_set_text(lbl_date, "等待同步时间...");
    lv_obj_set_style_text_font(lbl_date, &lv_font_sans20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xC0C0FF), LV_PART_MAIN);
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 40);

    // ---------- 5. 底部细线 + 副标题 ----------
    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_color(&style_line, lv_color_hex(0x3050A0));
    lv_style_set_line_width(&style_line, 1);
    lv_style_set_line_opa(&style_line, LV_OPA_60);

    lv_obj_t* line = lv_line_create(scr);
    static lv_point_t pts[2] = { {20, 200}, {220, 200} };
    lv_line_set_points(line, pts, 2);
    lv_obj_add_style(line, &style_line, 0);

    lv_obj_t* lbl_footer = lv_label_create(scr);
    lv_label_set_text(lbl_footer, "YYQ-MX9.0  LVGL");
    lv_obj_set_style_text_font(lbl_footer, &lv_font_sans16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6080C0), LV_PART_MAIN);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -10);

    // ---------- 6. 启动 1秒定时器刷时间 ----------
    timer_ui = lv_timer_create(update_clock_ui, 1000, NULL);
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n=== LVGL 大字时钟 Demo ===");

    // PSRAM 自检（让用户确认 N16R8 的 8MB PSRAM 是否启用）
    Serial.printf("PSRAM: %u bytes\n", ESP.getFreePsram());
    if (ESP.getFreePsram() == 0) {
        Serial.println("[WARN] PSRAM 没启用！Tools > PSRAM: OPI PSRAM");
    }

    // SPI + ST7789 初始化（与 s3.ino 一致）
    tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
    tft.init(240, 240);
    tft.setSPISpeed(40000000);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);

    // 设置一个起始时间，让 demo 启动后时钟立即走动。
    // 正式接入项目后，这个会被 s3.ino 已有的 NTP / 网页下发时间覆盖。
    {
        struct tm t = {0};
        t.tm_year = 2026 - 1900;   // 2026
        t.tm_mon  = 9 - 1;         // 9 月
        t.tm_mday = 26;
        t.tm_hour = 23;
        t.tm_min  = 4;
        t.tm_sec  = 6;
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        Serial.printf("[CLOCK] demo 起始时间已设: %s", ctime(&epoch));
    }

    // LVGL 启动
    lvgl_driver_init(&tft);

    // 建 UI
    build_ui();

    Serial.println("[READY] 屏幕上应该已经出现大字时钟");
}

// ================= LOOP =================
void loop() {
    lvgl_driver_loop();
    delay(5);  // ~200Hz 给 LVGL 调度；CPU 占用几乎 0
}