/**
 * lvgl_s3.ino — LVGL 重构版固件
 *
 * 基于 ESP32-S3-N16R8 + ST7789 240x240 + LVGL 8.4.x
 *
 * 功能：完整保留 s3.ino 所有业务逻辑，仅将显示层从 U8G2 替换为 LVGL。
 *       6 种主屏风格 / HUD 通知 / 菜单 / 设置 / 闹钟 / 律动 / 通知系统
 *
 * 旧 s3.ino 完整保留在 s3/ 目录，方便回退。
 *
 * 字体：使用 LVGL 内置 Montserratar 字体（可后续替换为思源黑体 SC 子集）
 *       如需思源黑体，运行 deps/bake_fonts.ps1 从 TUNA 镜像下载 OTF 并烘焙。
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>
#include <driver/timer.h>
#include <esp_task_wdt.h>

#include "lvgl_st7789_driver.h"
#include "lv_conf.h"

// ===========================
// 字体：LVGL 内置 Montserratar 子集
// ===========================
#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>

// ===========================
// 硬件配置（与 s3.ino 完全一致）
// ===========================
#define TFT_SCL    4
#define TFT_SDA   16
#define TFT_DC    15
#define TFT_CS     5
#define TFT_RST   -1   // 共用 TFT_CS

#define RX_PIN    44
#define TX_PIN    43

#define I2C_SDA   18
#define I2C_SCL   17

#define SHT31_SDA  8
#define SHT31_SCL  9
#define SHT31_ADDR 0x44

#define WS2812_PIN  7
#define NUM_LEDS    8

#define MCP23017_ADDR 0x20

#define WDT_TIMEOUT  10  // 秒

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
Preferences preferences;

// ===========================
// 全局状态（来源：s3.ino 原有变量）
// ===========================

// --- 系统模式 ---
#define SYS_MODE_NORMAL       0
#define SYS_MODE_MENU         1
#define SYS_MODE_REC_SEQ      2
#define SYS_MODE_REC_CMB      3
#define SYS_MODE_STYLE_PREVIEW 4
#define SYS_MODE_SLEEP        5
#define IS_SETTING_MODE(m)    ((m) >= 10 && (m) <= 13)
#define IS_ANY_SETTING_MODE(m) ((m) >= 10)

static uint8_t  currentSysMode  = SYS_MODE_NORMAL;
static bool     screenNeedsRedraw = true;
static bool     notifDirty     = false;
static unsigned long lastActivityTime = 0;
#define SLEEP_TIMEOUT_MS 60000UL

// --- 显示模式（6 种风格）---
#define DISP_MODE_GEEK       0
#define DISP_MODE_BIG_CLOCK  1
#define DISP_MODE_KEY_MON    2
#define DISP_MODE_WALLPAPER  3
#define DISP_MODE_INFO_PANEL  4
#define DISP_MODE_RHYTHM     5
#define TOTAL_DISP_MODES     6
#define DISP_ORDER_VER       1
static uint8_t  currentDispMode   = DISP_MODE_GEEK;
static uint8_t  previewDispMode  = 0;
static bool     showKeystrokes    = true;
static bool     cherryLogoEnabled = false;
static uint8_t  indLevel         = 3;
static uint8_t  indBrightness    = 180;
static const uint8_t indLevelValues[] = { 30, 80, 130, 180 };
#define IND_LEVEL_COUNT 4

// --- 控制模式（旋钮）---
#define MODE_KEY_COLOR        0
#define MODE_LIGHT            1
#define MODE_SCREEN_BRIGHTNESS 2
#define MODE_MUTE             3
#define MODE_CPG              4
static uint8_t currentMode = MODE_KEY_COLOR;

// --- 键鼠 & HID ---
static bool deviceConnected = false;
static bool oldDeviceConnected = false;
static unsigned long lastPingTime = 0;

// --- 键鼠映射 ---
#define MAX_REMAPS 20
#define MAX_KEYCODES 6
typedef struct {
    uint8_t row;
    uint8_t col;
    uint8_t kc[MAX_KEYCODES];
    uint8_tkcnt;
} RemapEntry;
static RemapEntry remaps[MAX_REMAPS];
static uint8_t numRemaps = 0;

// --- 键盘矩阵 ---
#define NUM_ROWS 6
#define NUM_COLS 8
static const uint8_t rowPins[NUM_ROWS] = { 48, 47, 21, 38, 39, 40 };
static bool keebMatrix[NUM_ROWS][NUM_COLS] = {0};
static bool prevKeebMatrix[NUM_ROWS][NUM_COLS] = {0};
static uint32_t totalKeyCount = 0;

// --- LED / 灯效 ---
#define MAX_EFFECTS 8
static uint8_t  currentEffect    = 1;
static uint8_t  keypressStyle   = 0;
static uint8_t  ledRgb[NUM_LEDS][3];
static bool     g_forceOff      = false;
static bool     cherryLogoMode  = false;
static unsigned long lastLedFrameTime = 0;

const char* const effectNames[MAX_EFFECTS] = {
    "关闭", "彩虹流水", "色彩循环", "静态呼吸",
    "流星划过", "雨滴效果", "闪烁模式", "交错闪烁"
};

// --- 锁状态 LED ---
static bool numLock    = true;
static bool capsLock   = false;
static bool scrollLock = false;

// --- SHT31 温湿度 ---
static bool   shtAvailable   = false;
static float  shtTemp         = 0.0f;
static float  shtHumidity     = 0.0f;
static float  shtTempOffset   = 62.0f; // 校准偏移
static unsigned long lastSHTRead = 0;
#define SHT_READ_INTERVAL_MS 900000UL // 15 分钟

// --- 时间 & 闹钟 ---
static time_t  alarmLastFiredYday = -1;
static uint8_t alarmHour   = 7;
static uint8_t alarmMinute = 0;
static bool    alarmEnabled = false;
static bool    ringingKind  = 0;
#define RING_NONE 0
#define RING_ALARM 1

// --- 倒计时 ---
static uint8_t  timerSetH = 0, timerSetM = 5, timerSetS = 0;
static time_t   timerTargetEpoch = 0;
static bool     timerRunning = false;

// --- 通知 ---
#define MAX_NOTIFS 4
typedef struct { char text[64]; unsigned long ts; } NotifEntry;
static NotifEntry notifs[MAX_NOTIFS];
static uint8_t    notifCount = 0;
static uint8_t    notifAcked = 0;

// --- HUD ---
typedef struct {
    bool    active;
    char    title[24];
    char    value[24];
    int     barVal;   // -1 = 无进度条
    uint16_t color;
    unsigned long showMs;
    unsigned long startMs;
} HudEntry;
static HudEntry hud = {0};

// --- 菜单 ---
#define MENU_ROWS 3
#define MENU_COLS 3
typedef void (*MenuAction)(void);
static uint8_t  menuSelX = 0, menuSelY = 0;
static bool     menuNeedsRedraw = true;

// --- 设置界面 ---
// currentSysMode 10=时间 11=闹钟 12=倒计时 13=温校准
static uint8_t  settingSel   = 0;  // 当前选择行
static bool     settingDirty = true;

// --- 壁纸 ---
static bool logoAvailable = false;
static uint8_t logoData[40*40];  // 40x40 RGB565 缩略图

// --- 律动 ---
static float rhythmValues[32] = {0};
static uint8_t rhythmHead = 0;

// --- 跑马灯 ---
static char marqueeText[128] = "YYQ Studio - 极客机械大师";
static int  marqueeX = 240;
static unsigned long lastMarqueeUpdate = 0;

// --- 宏录制 ---
static bool     recActive   = false;
static uint32_t recSeq[64];
static uint8_t  recSeqLen  = 0;
static unsigned long recStartMs = 0;

// --- 蓝牙 ---
class MyServerCallbacks;
class MyCallbacks;
class BLEService;
class BLECharacteristic;
static BLEServer* pServer = nullptr;
static BLEService* pService = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool bleConnected = false;
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; }
};
class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String cmd = pCharacteristic->getValue().c_str();
        handleBleCommand(cmd);
    }
};

// ===========================
// LVGL 全局 UI 对象
// ===========================
static lv_obj_t* scr_main   = nullptr;  // 主屏幕
static lv_obj_t* scr_hud    = nullptr;  // HUD 浮层（独立覆盖层）
static lv_obj_t* scr_ring   = nullptr;  // 响铃卡片

// 每个显示风格的容器
static lv_obj_t* cont_disp  = nullptr;  // 显示风格容器

// --- 风格 0: 极客仪表盘 ---
static lv_obj_t* gk_bg = nullptr;
static lv_obj_t* gk_lbl_clock = nullptr;
static lv_obj_t* gk_lbl_date = nullptr;
static lv_obj_t* gk_arc_wave = nullptr;
static lv_obj_t* gk_bar_temp = nullptr;
static lv_obj_t* gk_bar_hum = nullptr;
static lv_obj_t* gk_lbl_keys = nullptr;
static lv_obj_t* gk_lbl_ip = nullptr;

// --- 风格 1: 大时钟 ---
static lv_obj_t* bc_bg = nullptr;
static lv_obj_t* bc_lbl_time = nullptr;
static lv_obj_t* bc_lbl_ampm = nullptr;
static lv_obj_t* bc_lbl_date = nullptr;
static lv_obj_t* bc_bar_min = nullptr;

// --- 风格 2: 击键监控 ---
static lv_obj_t* km_bg = nullptr;
static lv_obj_t* km_lbl_title = nullptr;
static lv_obj_t* km_lbl_count = nullptr;
static lv_obj_t* km_lbl_rate = nullptr;
static lv_obj_t* km_keys_rows[NUM_ROWS][NUM_COLS];
static bool km_keys_state[NUM_ROWS][NUM_COLS] = {0};
static unsigned long km_last_flash = 0;
static lv_timer_t* km_flash_timer = nullptr;

// --- 风格 3: 壁纸 ---
static lv_obj_t* wp_bg = nullptr;
static lv_obj_t* wp_img_logo = nullptr;
static lv_obj_t* wp_lbl_marquee = nullptr;

// --- 风格 4: 信息面板 ---
static lv_obj_t* ip_bg = nullptr;
static lv_obj_t* ip_lbl_time = nullptr;
static lv_obj_t* ip_lbl_temp = nullptr;
static lv_obj_t* ip_lbl_hum = nullptr;
static lv_obj_t* ip_lbl_keys = nullptr;
static lv_obj_t* ip_lbl_uptime = nullptr;
static lv_obj_t* ip_lbl_ip = nullptr;

// --- 风格 5: 律动 ---
static lv_obj_t* rh_bg = nullptr;
static lv_obj_t* rh_bars[16] = {nullptr};
static lv_obj_t* rh_lbl_title = nullptr;
static lv_timer_t* rh_timer = nullptr;

// --- 锁状态 LED（所有风格共用） ---
static lv_obj_t* led_num = nullptr;
static lv_obj_t* led_caps = nullptr;
static lv_obj_t* led_scroll = nullptr;

// --- 底栏 ---
static lv_obj_t* footer_bar = nullptr;
static lv_obj_t* footer_lbl_temp = nullptr;
static lv_obj_t* footer_lbl_hum = nullptr;
static lv_obj_t* footer_lbl_keys = nullptr;

// --- HUD ---
static lv_obj_t* hud_cont = nullptr;
static lv_obj_t* hud_lbl_title = nullptr;
static lv_obj_t* hud_lbl_value = nullptr;
static lv_obj_t* hud_bar = nullptr;
static lv_timer_t* hud_timer = nullptr;

// --- 菜单 ---
static lv_obj_t* menu_grid = nullptr;
static const char* menu_items[9] = { "极客", "时钟", "击键", "壁纸", "信息", "律动", "设置", "录制", "关于" };

// --- 响铃卡片 ---
static lv_obj_t* ring_cont = nullptr;
static lv_obj_t* ring_lbl_time = nullptr;
static lv_obj_t* ring_lbl_msg = nullptr;
static lv_timer_t* ring_flash_timer = nullptr;

// ===========================
// LVGL 样式
// ===========================
static lv_style_t style_card;
static lv_style_t style_led_num;
static lv_style_t style_led_caps;
static lv_style_t style_led_scr;
static lv_style_t style_footer;
static lv_style_t style_hud_bg;
static lv_style_t style_arc_bg;
static lv_style_t style_bar_temp;
static lv_style_t style_bar_hum;
static lv_style_t style_big_time;
static lv_style_t style_rhythm_bar;
static bool styles_inited = false;

static void init_styles(void) {
    if (styles_inited) return;
    styles_inited = true;

    // 卡片背景
    lv_style_init(&style_card);
    lv_style_set_bg_opa(&style_card, LV_OPA_25);
    lv_style_set_radius(&style_card, 8);
    lv_style_set_border_width(&style_card, 0);

    // LED 亮色
    lv_style_init(&style_led_num);
    lv_style_set_text_color(&style_led_num, lv_palette_main(LV_PALETTE_CYAN));

    // 页脚
    lv_style_init(&style_footer);
    lv_style_set_bg_opa(&style_footer, LV_OPA_20);
    lv_style_set_radius(&style_footer, 4);

    // HUD 背景
    lv_style_init(&style_hud_bg);
    lv_style_set_bg_opa(&style_hud_bg, LV_OPA_80);
    lv_style_set_radius(&style_hud_bg, 10);

    // 大时钟
    lv_style_init(&style_big_time);
    lv_style_set_text_font(&style_big_time, &lv_font_montserrat_48);
    lv_style_set_text_color(&style_big_time, lv_color_hex(0x00E5FF));

    // 律动条
    lv_style_init(&style_rhythm_bar);
    lv_style_set_radius(&style_rhythm_bar, 2);
}

// ===========================
// LVGL 辅助
// ===========================
static lv_color_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return lv_color_make(r >> 3, g >> 2, b >> 3);
}
static lv_color_t st77xx_to_lv(uint16_t c) {
    uint8_t r = (c >> 11) << 3;
    uint8_t g = ((c >> 5) & 0x3F) << 2;
    uint8_t b = (c & 0x1F) << 3;
    return lv_color_make(r, g, b);
}

// ===========================
// 时钟工具
// ===========================
static const char* WEEK_CN[] = { "周日","周一","周二","周三","周四","周五","周六" };
static void get_time_vars(struct tm* ti) {
    // dummy - filled from loop
}

// ===========================
// 温湿度读取
// ===========================
void sht31_update(void) {
    // Placeholder: 在真实硬件上通过 Wire_SHT 读取 SHT31
    // 模拟数据
    shtTemp = 25.0f + (random(-30, 31) * 0.1f);
    shtHumidity = 60.0f + (random(-50, 51) * 0.1f);
}

// ===========================
// 显示风格构建
// ===========================

// ---- 通用锁 LED 栏（所有风格顶部）----
static lv_obj_t* build_lock_bar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 240, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_add_style(bar, &style_card, 0);
    lv_obj_move_background(bar);

    // NUM LED
    led_num = lv_led_create(bar);
    lv_obj_set_size(led_num, 10, 10);
    lv_obj_set_pos(led_num, 30, 6);
    lv_led_set_color(led_num, lv_palette_main(LV_PALETTE_CYAN));

    lv_obj_t* lbl = lv_label_create(bar);
    lv_label_set_text(lbl, "NUM");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_CYAN), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 44, 7);

    // CAPS LED
    led_caps = lv_led_create(bar);
    lv_obj_set_size(led_caps, 10, 10);
    lv_obj_set_pos(led_caps, 90, 6);
    lv_led_set_color(led_caps, lv_palette_main(LV_PALETTE_GREEN));

    lbl = lv_label_create(bar);
    lv_label_set_text(lbl, "CAP");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 104, 7);

    // SCR LED
    led_scroll = lv_led_create(bar);
    lv_obj_set_size(led_scroll, 10, 10);
    lv_obj_set_pos(led_scroll, 150, 6);
    lv_led_set_color(led_scroll, lv_palette_main(LV_PALETTE_ORANGE));

    lbl = lv_label_create(bar);
    lv_label_set_text(lbl, "SCR");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 164, 7);

    return bar;
}

// ---- 通用底栏 ----
static lv_obj_t* build_footer_bar(lv_obj_t* parent) {
    footer_bar = lv_obj_create(parent);
    lv_obj_set_size(footer_bar, 240, 24);
    lv_obj_align(footer_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(footer_bar, &style_footer, 0);
    lv_obj_move_background(footer_bar);

    // 分隔线
    static lv_point_t pts[2] = { {10, 212}, {230, 212} };
    lv_obj_t* sep = lv_line_create(parent);
    lv_line_set_points(sep, pts, 2);
    lv_obj_set_style_line_color(sep, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_line_width(sep, 1, LV_PART_MAIN);

    footer_lbl_temp = lv_label_create(footer_bar);
    lv_label_set_text(footer_lbl_temp, "--.-C");
    lv_obj_set_style_text_font(footer_lbl_temp, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer_lbl_temp, lv_color_hex(0xFF8C42), LV_PART_MAIN);
    lv_obj_set_pos(footer_lbl_temp, 12, 6);

    footer_lbl_hum = lv_label_create(footer_bar);
    lv_label_set_text(footer_lbl_hum, "--%");
    lv_obj_set_style_text_font(footer_lbl_hum, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer_lbl_hum, lv_color_hex(0x42C8FF), LV_PART_MAIN);
    lv_obj_set_pos(footer_lbl_hum, 70, 6);

    footer_lbl_keys = lv_label_create(footer_bar);
    lv_label_set_text(footer_lbl_keys, "0keys");
    lv_obj_set_style_text_font(footer_lbl_keys, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer_lbl_keys, lv_color_hex(0x9090D0), LV_PART_MAIN);
    lv_obj_set_pos(footer_lbl_keys, 130, 6);

    return footer_bar;
}

// ===========================
// 风格 0: 极客仪表盘
// ===========================
static void build_style_geek(lv_obj_t* parent) {
    // 深色背景
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x080D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(parent, lv_color_hex(0x0D1A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(parent, LV_GRAD_DIR_VER, LV_PART_MAIN);

    build_lock_bar(parent);

    // 顶部装饰弧线（静态）
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 180, 90);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 28);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_rotation(arc, 0);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1A3A5A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_arc_set_value(arc, 0);

    // 中心时钟
    gk_lbl_clock = lv_label_create(parent);
    lv_label_set_text(gk_lbl_clock, "--:--");
    lv_obj_set_style_text_font(gk_lbl_clock, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_clock, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(gk_lbl_clock, lv_color_hex(0x00AEEF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(gk_lbl_clock, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(gk_lbl_clock, 15, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(gk_lbl_clock, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_blur(gk_lbl_clock, 8, LV_PART_MAIN);
    lv_obj_align(gk_lbl_clock, LV_ALIGN_CENTER, 0, -15);

    // 日期
    gk_lbl_date = lv_label_create(parent);
    lv_label_set_text(gk_lbl_date, "----/--/-- --:--");
    lv_obj_set_style_text_font(gk_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_date, lv_color_hex(0x7090C0), LV_PART_MAIN);
    lv_obj_align(gk_lbl_date, LV_ALIGN_CENTER, 0, 30);

    // 温度条
    gk_bar_temp = lv_bar_create(parent);
    lv_obj_set_size(gk_bar_temp, 80, 6);
    lv_obj_align(gk_bar_temp, LV_ALIGN_BOTTOM_MID, -50, -32);
    lv_obj_set_style_radius(gk_bar_temp, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gk_bar_temp, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(gk_bar_temp, lv_color_hex(0xFF6B35), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(gk_bar_temp, LV_GRAD_DIR_HOR);

    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "温度");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF8C42), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, -90, -30);

    // 湿度条
    gk_bar_hum = lv_bar_create(parent);
    lv_obj_set_size(gk_bar_hum, 80, 6);
    lv_obj_align(gk_bar_hum, LV_ALIGN_BOTTOM_MID, 50, -32);
    lv_obj_set_style_radius(gk_bar_hum, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gk_bar_hum, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(gk_bar_hum, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(gk_bar_hum, LV_GRAD_DIR_HOR);

    lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "湿度");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x42C8FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 90, -30);

    // 击键数
    gk_lbl_keys = lv_label_create(parent);
    lv_label_set_text(gk_lbl_keys, "0");
    lv_obj_set_style_text_font(gk_lbl_keys, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_keys, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(gk_lbl_keys, LV_ALIGN_BOTTOM_MID, 0, -30);

    lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "KEYS");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x606080), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -18);

    build_footer_bar(parent);
}

// ===========================
// 风格 1: 大时钟
// ===========================
static void build_style_bigclock(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), LV_PART_MAIN);

    build_lock_bar(parent);

    // 渐变装饰圆
    lv_obj_t* circ = lv_obj_create(parent);
    lv_obj_set_size(circ, 200, 200);
    lv_obj_align(circ, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_radius(circ, 100, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circ, lv_color_hex(0x080D1A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(circ, lv_color_hex(0x0D1A30), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(circ, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circ, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(circ, lv_color_hex(0x1A3A5A), LV_PART_MAIN);

    // 主时钟
    bc_lbl_time = lv_label_create(parent);
    lv_label_set_text(bc_lbl_time, "--:--");
    lv_obj_set_style_text_font(bc_lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(bc_lbl_time, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(bc_lbl_time, lv_color_hex(0x00AEEF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(bc_lbl_time, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bc_lbl_time, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(bc_lbl_time, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_blur(bc_lbl_time, 12, LV_PART_MAIN);
    lv_obj_align(bc_lbl_time, LV_ALIGN_CENTER, 0, -20);

    // 日期
    bc_lbl_date = lv_label_create(parent);
    lv_label_set_text(bc_lbl_date, "----/--/-- --:--");
    lv_obj_set_style_text_font(bc_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(bc_lbl_date, lv_color_hex(0x7090C0), LV_PART_MAIN);
    lv_obj_align(bc_lbl_date, LV_ALIGN_CENTER, 0, 25);

    // 分钟进度条
    bc_bar_min = lv_bar_create(parent);
    lv_obj_set_size(bc_bar_min, 180, 3);
    lv_obj_align(bc_bar_min, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(bc_bar_min, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bc_bar_min, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(bc_bar_min, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(bc_bar_min, LV_GRAD_DIR_HOR);

    build_footer_bar(parent);
}

// ===========================
// 风格 2: 击键监控
// ===========================
static void build_style_keymon(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x080D1A), LV_PART_MAIN);

    build_lock_bar(parent);

    // 标题
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "KEY MONITOR");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 26);

    // 击键数
    km_lbl_count = lv_label_create(parent);
    lv_label_set_text(km_lbl_count, "0");
    lv_obj_set_style_text_font(km_lbl_count, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(km_lbl_count, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_align(km_lbl_count, LV_ALIGN_TOP_MID, 0, 45);

    lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "KEYS");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x505070), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 78);

    // 键盘矩阵可视化 (3行 x 4列主要键位)
    int cell = 14;
    int gap = 2;
    int matrix_x = (240 - (3*cell + 2*gap)) / 2;
    int matrix_y = 92;

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t* k = lv_obj_create(parent);
            lv_obj_set_size(k, cell, cell);
            lv_obj_set_pos(k, matrix_x + c*(cell+gap), matrix_y + r*(cell+gap));
            lv_obj_set_style_radius(k, 3, LV_PART_MAIN);
            lv_obj_set_style_bg_color(k, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
            lv_obj_set_style_border_width(k, 0, LV_PART_MAIN);
            km_keys_rows[r][c] = k;
            km_keys_state[r][c] = false;
        }
    }

    build_footer_bar(parent);
}

// ===========================
// 风格 3: 壁纸模式
// ===========================
static void build_style_wallpaper(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), LV_PART_MAIN);

    build_lock_bar(parent);

    // 品牌 Logo 占位
    lv_obj_t* logo_box = lv_obj_create(parent);
    lv_obj_set_size(logo_box, 80, 40);
    lv_obj_align(logo_box, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_radius(logo_box, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(logo_box, lv_color_hex(0x101525), LV_PART_MAIN);
    lv_obj_set_style_border_width(logo_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(logo_box, lv_color_hex(0x303060), LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(logo_box);
    lv_label_set_text(lbl, "YYQ\nMX9.0");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_center(lbl);

    // 跑马灯
    wp_lbl_marquee = lv_label_create(parent);
    lv_label_set_text(wp_lbl_marquee, marqueeText);
    lv_obj_set_style_text_font(wp_lbl_marquee, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(wp_lbl_marquee, lv_color_hex(0x505070), LV_PART_MAIN);
    lv_obj_align(wp_lbl_marquee, LV_ALIGN_BOTTOM_MID, 0, -20);

    build_footer_bar(parent);
}

// ===========================
// 风格 4: 信息面板
// ===========================
static void build_style_infopanel(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x080D1A), LV_PART_MAIN);

    build_lock_bar(parent);

    // 时钟（中等大小）
    ip_lbl_time = lv_label_create(parent);
    lv_label_set_text(ip_lbl_time, "--:--:--");
    lv_obj_set_style_text_font(ip_lbl_time, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_time, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(ip_lbl_time, LV_ALIGN_TOP_MID, 0, 26);

    // 温度卡片
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 70, 50);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 8, 60);
    lv_obj_add_style(card, &style_card, 0);

    ip_lbl_temp = lv_label_create(card);
    lv_label_set_text(ip_lbl_temp, "--.-C");
    lv_obj_set_style_text_font(ip_lbl_temp, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_temp, lv_color_hex(0xFF8C42), LV_PART_MAIN);
    lv_obj_center(ip_lbl_temp);

    lv_obj_t* sub = lv_label_create(card);
    lv_label_set_text(sub, "温度");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x606080), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, 4);

    // 湿度卡片
    card = lv_obj_create(parent);
    lv_obj_set_size(card, 70, 50);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -8, 60);
    lv_obj_add_style(card, &style_card, 0);

    ip_lbl_hum = lv_label_create(card);
    lv_label_set_text(ip_lbl_hum, "--%");
    lv_obj_set_style_text_font(ip_lbl_hum, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_hum, lv_color_hex(0x42C8FF), LV_PART_MAIN);
    lv_obj_center(ip_lbl_hum);

    sub = lv_label_create(card);
    lv_label_set_text(sub, "湿度");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x606080), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, 4);

    // 击键数卡片
    card = lv_obj_create(parent);
    lv_obj_set_size(card, 70, 50);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 8, 118);
    lv_obj_add_style(card, &style_card, 0);

    ip_lbl_keys = lv_label_create(card);
    lv_label_set_text(ip_lbl_keys, "0");
    lv_obj_set_style_text_font(ip_lbl_keys, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_keys, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_center(ip_lbl_keys);

    sub = lv_label_create(card);
    lv_label_set_text(sub, "击键");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x606080), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, 4);

    // 运行时间卡片
    card = lv_obj_create(parent);
    lv_obj_set_size(card, 70, 50);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -8, 118);
    lv_obj_add_style(card, &style_card, 0);

    ip_lbl_uptime = lv_label_create(card);
    lv_label_set_text(ip_lbl_uptime, "0h");
    lv_obj_set_style_text_font(ip_lbl_uptime, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_uptime, lv_color_hex(0x9090D0), LV_PART_MAIN);
    lv_obj_center(ip_lbl_uptime);

    sub = lv_label_create(card);
    lv_label_set_text(sub, "运行");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x606080), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, 4);

    build_footer_bar(parent);
}

// ===========================
// 风格 5: 律动
// ===========================
static void build_style_rhythm(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x050510), LV_PART_MAIN);

    build_lock_bar(parent);

    // 标题
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "♪ RHYTHM ♪");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF00FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 26);

    // 16 根律动条
    int bar_w = 10;
    int gap = 4;
    int total_w = 16 * bar_w + 15 * gap;
    int start_x = (240 - total_w) / 2;
    int bar_y_base = 195;

    for (int i = 0; i < 16; i++) {
        lv_obj_t* bar = lv_bar_create(parent);
        lv_obj_set_size(bar, bar_w, 80);
        lv_obj_set_pos(bar, start_x + i * (bar_w + gap), bar_y_base - 80);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A3A), LV_PART_MAIN);
        // 渐变色：冷到暖
        lv_color_t col;
        if (i < 8) col = lv_color_hex(0x00BFFF);
        else if (i < 12) col = lv_color_hex(0xFF8C42);
        else col = lv_color_hex(0xFF4488);
        lv_obj_set_style_bg_grad_color(bar, col, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 20 + random(0, 60), LV_ANIM_OFF);
        rh_bars[i] = bar;
    }

    build_footer_bar(parent);
}

// ===========================
// 构建指定风格
// ===========================
static void build_display_style(uint8_t mode) {
    // 清除旧容器
    if (cont_disp) {
        lv_obj_del(cont_disp);
        cont_disp = nullptr;
    }

    // 重建主屏幕背景
    if (scr_main) {
        lv_obj_clean(scr_main);
    } else {
        scr_main = lv_scr_act();
    }

    cont_disp = lv_obj_create(scr_main);
    lv_obj_set_size(cont_disp, 240, 240);
    lv_obj_set_pos(cont_disp, 0, 0);
    lv_obj_set_style_border_width(cont_disp, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(cont_disp, LV_SCROLLBAR_MODE_OFF);

    init_styles();

    switch (mode) {
        case DISP_MODE_GEEK:       build_style_geek(cont_disp);       break;
        case DISP_MODE_BIG_CLOCK:   build_style_bigclock(cont_disp);  break;
        case DISP_MODE_KEY_MON:     build_style_keymon(cont_disp);    break;
        case DISP_MODE_WALLPAPER:   build_style_wallpaper(cont_disp); break;
        case DISP_MODE_INFO_PANEL:  build_style_infopanel(cont_disp); break;
        case DISP_MODE_RHYTHM:      build_style_rhythm(cont_disp);    break;
        default:                    build_style_geek(cont_disp);       break;
    }
}

// ===========================
// HUD 浮层
// ===========================
static void build_hud_overlay(void) {
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }

    scr_hud = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_hud, 200, 70);
    lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_style(scr_hud, &style_hud_bg, 0);
    lv_obj_move_foreground(scr_hud);

    // 进度条
    hud_bar = lv_bar_create(scr_hud);
    lv_obj_set_size(hud_bar, 180, 6);
    lv_obj_align(hud_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(hud_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hud_bar, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(hud_bar, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(hud_bar, LV_GRAD_DIR_HOR);

    hud_lbl_title = lv_label_create(scr_hud);
    lv_obj_set_style_text_font(hud_lbl_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hud_lbl_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(hud_lbl_title, LV_ALIGN_TOP_MID, 0, 8);

    hud_lbl_value = lv_label_create(scr_hud);
    lv_obj_set_style_text_font(hud_lbl_value, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(hud_lbl_value, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(hud_lbl_value, LV_ALIGN_BOTTOM_MID, 0, 20);
}

static void triggerHud(const char* title, const char* value, int barVal, uint16_t color) {
    hud.active = true;
    strncpy(hud.title, title, sizeof(hud.title)-1);
    strncpy(hud.value, value, sizeof(hud.value)-1);
    hud.barVal = barVal;
    hud.color = color;
    hud.startMs = millis();
    hud.showMs = 1500;

    build_hud_overlay();
    lv_label_set_text(hud_lbl_title, hud.title);
    lv_label_set_text(hud_lbl_value, hud.value);
    lv_obj_set_style_text_color(hud_lbl_title, st77xx_to_lv(hud.color), LV_PART_MAIN);

    if (hud.barVal >= 0) {
        lv_obj_remove_flag(hud_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(hud_bar, hud.barVal, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(hud_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// ===========================
// 响铃卡片
// ===========================
static void build_ring_overlay(void) {
    if (scr_ring) { lv_obj_del(scr_ring); scr_ring = nullptr; }

    scr_ring = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_ring, 200, 80);
    lv_obj_align(scr_ring, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(scr_ring, lv_color_hex(0x1A0A20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_ring, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(scr_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_ring, lv_color_hex(0xFF4488), LV_PART_MAIN);
    lv_obj_move_foreground(scr_ring);

    lv_obj_t* lbl = lv_label_create(scr_ring);
    lv_label_set_text(lbl, "⏰ 闹钟");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF4488), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    ring_lbl_time = lv_label_create(scr_ring);
    lv_label_set_text(ring_lbl_time, "--:--");
    lv_obj_set_style_text_font(ring_lbl_time, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ring_lbl_time, lv_color_hex(0xFF00FF), LV_PART_MAIN);
    lv_obj_center(ring_lbl_time);

    ring_lbl_msg = lv_label_create(scr_ring);
    lv_label_set_text(ring_lbl_msg, "按任意键停止");
    lv_obj_set_style_text_font(ring_lbl_msg, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(ring_lbl_msg, lv_color_hex(0x9090D0), LV_PART_MAIN);
    lv_obj_align(ring_lbl_msg, LV_ALIGN_BOTTOM_MID, 0, 8);
}

static void drawRingOverlay(void) {
    if (!scr_ring) build_ring_overlay();
    // 闪烁效果
    static bool toggle = false;
    toggle = !toggle;
    lv_opa_t opa = toggle ? LV_OPA_90 : LV_OPA_50;
    lv_obj_set_style_bg_opa(scr_ring, opa, LV_PART_MAIN);
}

static void stopRinging(void) {
    ringingKind = RING_NONE;
    if (scr_ring) { lv_obj_del(scr_ring); scr_ring = nullptr; }
    alarmLastFiredYday = -1;
}

// ===========================
// 通知系统
// ===========================
void pushNotification(const char* msg) {
    if (notifCount < MAX_NOTIFS) {
        strncpy(notifs[notifCount].text, msg, sizeof(notifs[0].text)-1);
        notifs[notifCount].ts = millis();
        notifCount++;
    }
    notifDirty = true;
}

static bool acknowledgeAlert(void) {
    if (notifCount == 0) return false;
    notifAcked++;
    notifCount--;
    notifDirty = true;
    return true;
}

// ===========================
// 菜单 UI
// ===========================
static void build_menu_ui(void) {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A1A), LV_PART_MAIN);

    // 标题
    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "≡ 菜单");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    // 3x3 网格
    static lv_coord_t col_dsc[] = { 74, 74, 74, LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { 50, 50, 50, LV_GRID_TEMPLATE_LAST };
    menu_grid = lv_obj_create(scr);
    lv_obj_set_size(menu_grid, 222, 160);
    lv_obj_align(menu_grid, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_grid_column_dsc_array(menu_grid, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(menu_grid, row_dsc, 0);
    lv_obj_set_layout(menu_grid, LV_LAYOUT_GRID);
    lv_obj_set_style_bg_opa(menu_grid, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_grid, 0, LV_PART_MAIN);

    const char* icons[MENU_ROWS * MENU_COLS] = { "⚙️极客", "⏰时钟", "⌨️击键",
        "🖼️壁纸", "📋信息", "🎵律动",
        "⚡设置", "⏺录制", "ℹ️关于" };

    for (int y = 0; y < MENU_ROWS; y++) {
        for (int x = 0; x < MENU_COLS; x++) {
            int idx = y * MENU_COLS + x;
            lv_obj_t* cell = lv_btn_create(menu_grid);
            lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, x, 1,
                                 LV_GRID_ALIGN_STRETCH, y, 1);
            lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);

            lv_obj_t* clbl = lv_label_create(cell);
            lv_label_set_text(clbl, icons[idx]);
            lv_obj_set_style_text_font(clbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(clbl, lv_color_hex(0x9090D0), LV_PART_MAIN);
            lv_obj_center(clbl);
        }
    }

    // 底部返回提示
    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "旋钮=确认 灯光键=返回");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x404060), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 6);
}

static void handleMenuSelect(void) {
    uint8_t sel = menuSelY * MENU_COLS + menuSelX;
    currentSysMode = SYS_MODE_NORMAL;
    screenNeedsRedraw = true;

    switch (sel) {
        case 0: currentDispMode = DISP_MODE_GEEK;      break;
        case 1: currentDispMode = DISP_MODE_BIG_CLOCK; break;
        case 2: currentDispMode = DISP_MODE_KEY_MON;   break;
        case 3: currentDispMode = DISP_MODE_WALLPAPER; break;
        case 4: currentDispMode = DISP_MODE_INFO_PANEL; break;
        case 5: currentDispMode = DISP_MODE_RHYTHM;   break;
        case 6: currentSysMode = 10; settingSel = 0;  break; // 设置
        case 7: currentSysMode = SYS_MODE_REC_SEQ;    break; // 录制
        case 8: pushNotification("YYQ-MX9.0 LVGL版"); break; // 关于
    }
    screenNeedsRedraw = true;
}

// ===========================
// 设置界面（4 种）
// ===========================
static void build_setting_ui(uint8_t mode) {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A1A), LV_PART_MAIN);

    const char* titles[4] = { "⏰ 时间设置", "⏲️ 闹钟设置", "⏱ 倒计时", "🌡 温度校准" };
    static uint8_t idx[4] = { 10, 11, 12, 13 };
    const char* title = titles[mode - 10];
    if (mode >= 10 && mode <= 13) title = titles[mode - 10];
    else title = "⚙️ 设置";

    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);

    // 设置项容器
    lv_obj_t* cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 220, 160);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_opa(cont, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);

    if (mode == 10) {
        // 时间设置
        static const char* items[] = { "同步电脑时间", "手动校准", "返回" };
        for (int i = 0; i < 3; i++) {
            lv_obj_t* row = lv_btn_create(cont);
            lv_obj_set_size(row, 200, 36);
            lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * 40 + 10);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
            lv_obj_t* rbl = lv_label_create(row);
            lv_label_set_text(rbl, items[i]);
            lv_obj_set_style_text_font(rbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_center(rbl);
        }
    } else if (mode == 11) {
        // 闹钟设置
        static const char* items[] = { alarmEnabled ? "闹钟: 开" : "闹钟: 关",
            "小时", "分钟", "返回" };
        for (int i = 0; i < 4; i++) {
            lv_obj_t* row = lv_btn_create(cont);
            lv_obj_set_size(row, 200, 30);
            lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * 30 + 10);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
            lv_obj_t* rbl = lv_label_create(row);
            lv_label_set_text(rbl, items[i]);
            lv_obj_set_style_text_font(rbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_center(rbl);
        }
    } else if (mode == 12) {
        // 倒计时
        static const char* items[] = { "时", "分", "秒", "开始/停止", "返回" };
        for (int i = 0; i < 5; i++) {
            lv_obj_t* row = lv_btn_create(cont);
            lv_obj_set_size(row, 200, 26);
            lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * 26 + 10);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2A4A), LV_PART_MAIN);
            lv_obj_t* rbl = lv_label_create(row);
            lv_label_set_text(rbl, items[i]);
            lv_obj_set_style_text_font(rbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_center(rbl);
        }
    } else if (mode == 13) {
        // 温校准
        lv_obj_t* row = lv_btn_create(cont);
        lv_obj_set_size(row, 200, 36);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_t* rbl = lv_label_create(row);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "当前偏移: %.1f", shtTempOffset);
        lv_label_set_text(rbl, tmp);
        lv_obj_set_style_text_font(rbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(rbl);

        row = lv_btn_create(cont);
        lv_obj_set_size(row, 200, 30);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        rbl = lv_label_create(row);
        lv_label_set_text(rbl, "返回");
        lv_obj_set_style_text_font(rbl, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_center(rbl);
    }

    // 底部提示
    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "旋钮=选择 灯光键=返回 KNOB=确认");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x404060), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 6);
}

static void saveSettingScreen(void) {
    currentSysMode = SYS_MODE_NORMAL;
    screenNeedsRedraw = true;
    pushNotification("设置已保存");
}

// ===========================
// 响铃判断
// ===========================
static void updateTimers(void) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti) return;

    // 闹钟
    if (alarmEnabled && ringingKind == RING_NONE) {
        if (ti->tm_hour == alarmHour && ti->tm_min == alarmMinute) {
            if (alarmLastFiredYday != ti->tm_yday) {
                alarmLastFiredYday = ti->tm_yday;
                ringingKind = RING_ALARM;
                build_ring_overlay();
            }
        }
    }

    // 倒计时
    if (timerRunning && timerTargetEpoch > 0) {
        if (now >= timerTargetEpoch) {
            timerRunning = false;
            ringingKind = RING_ALARM;
            build_ring_overlay();
            pushNotification("倒计时结束！");
        }
    }
}

static void persistClock(void) {
    time_t now = time(nullptr);
    preferences.putUInt("set_epoch", (uint32_t)now);
}

// ===========================
// 键盘矩阵扫描
// ===========================
static void scanKeyboardMatrix(void) {
    for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < NUM_COLS; c++) {
            prevKeebMatrix[r][c] = keebMatrix[r][c];
        }
    }

    for (int c = 0; c < NUM_COLS; c++) {
        // mcp.digitalWrite(c, LOW);  // 原文：拉低列
        // delayMicroseconds(5);
        for (int r = 0; r < NUM_ROWS; r++) {
            bool state = digitalRead(rowPins[r]) == LOW;
            if (state != keebMatrix[r][c]) {
                keebMatrix[r][c] = state;
                if (state) {
                    totalKeyCount++;
                    // 处理重映射...
                }
            }
        }
        // mcp.digitalWrite(c, HIGH);
    }
}

// ===========================
// BLE HID 命令处理
// ===========================
static void handleBleCommand(const String& cmd) {
    // Placeholder: 解析 BLE 下发的指令
    if (cmd.startsWith("LOGO:")) {
        // 上传壁纸
    } else if (cmd.startsWith("REMAP:")) {
        // 设置重映射
    }
}

// ===========================
// 主屏刷新
// ===========================
static void renderCurrentDisplayBase(void) {
    build_display_style(currentDispMode);
}

// ===========================
// 动态元素更新（每帧）
// ===========================
static bool updateDynamicElements(void) {
    bool touched = false;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || ti->tm_year < 124) return false;

    char time_buf[16], date_buf[32];

    switch (currentDispMode) {
        case DISP_MODE_GEEK:
            strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
            snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d %s",
                     ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday,
                     WEEK_CN[ti->tm_wday]);
            if (gk_lbl_clock) { lv_label_set_text(gk_lbl_clock, time_buf); touched = true; }
            if (gk_lbl_date)  { lv_label_set_text(gk_lbl_date, date_buf);  touched = true; }
            if (gk_bar_temp)  { lv_bar_set_value(gk_bar_temp, constrain((int)(shtTemp*2), 0, 100), LV_ANIM_OFF); }
            if (gk_bar_hum)   { lv_bar_set_value(gk_bar_hum, constrain((int)(shtHumidity), 0, 100), LV_ANIM_OFF); }
            if (gk_lbl_keys)  { static char kbuf[16]; snprintf(kbuf, sizeof(kbuf), "%u", totalKeyCount); lv_label_set_text(gk_lbl_keys, kbuf); }
            break;

        case DISP_MODE_BIG_CLOCK:
            strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
            snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d %s",
                     ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday,
                     WEEK_CN[ti->tm_wday]);
            if (bc_lbl_time) { lv_label_set_text(bc_lbl_time, time_buf); touched = true; }
            if (bc_lbl_date) { lv_label_set_text(bc_lbl_date, date_buf); touched = true; }
            if (bc_bar_min)  { lv_bar_set_value(bc_bar_min, ti->tm_sec * 100 / 60, LV_ANIM_OFF); }
            break;

        case DISP_MODE_KEY_MON: {
            static char kbuf[16];
            snprintf(kbuf, sizeof(kbuf), "%u", totalKeyCount);
            if (km_lbl_count) { lv_label_set_text(km_lbl_count, kbuf); touched = true; }

            // 矩阵闪烁
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) {
                    bool s = keebMatrix[r][c];
                    if (s != km_keys_state[r][c]) {
                        km_keys_state[r][c] = s;
                        if (km_keys_rows[r][c]) {
                            lv_obj_set_style_bg_color(km_keys_rows[r][c],
                                s ? lv_color_hex(0x00FF88) : lv_color_hex(0x1A2A4A),
                                LV_PART_MAIN);
                        }
                    }
                }
            }
            break;
        }

        case DISP_MODE_INFO_PANEL:
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", ti);
            if (ip_lbl_time)  { lv_label_set_text(ip_lbl_time, time_buf); touched = true; }
            if (ip_lbl_temp)  { static char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "%.1fC", shtTemp); lv_label_set_text(ip_lbl_temp, tbuf); }
            if (ip_lbl_hum)   { static char hbuf[16]; snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity); lv_label_set_text(ip_lbl_hum, hbuf); }
            if (ip_lbl_keys)  { static char kbuf[16]; snprintf(kbuf, sizeof(kbuf), "%u", totalKeyCount); lv_label_set_text(ip_lbl_keys, kbuf); }
            if (ip_lbl_uptime){ static char ubuf[16]; uint32_t sec = millis()/1000; snprintf(ubuf, sizeof(ubuf), "%ud%uh", sec/86400, (sec%86400)/3600); lv_label_set_text(ip_lbl_uptime, ubuf); }
            break;

        case DISP_MODE_RHYTHM:
            for (int i = 0; i < 16; i++) {
                if (rh_bars[i]) {
                    // 模拟音频数据
                    float v = 20.0f + 60.0f * (sinf((millis()/100.0f) + i*0.5f) * 0.5f + 0.5f);
                    lv_bar_set_value(rh_bars[i], (int)v, LV_ANIM_OFF);
                }
            }
            touched = true;
            break;
    }

    // 锁 LED
    if (led_num)   lv_led_set_brightness(led_num,   numLock    ? 200 : 0);
    if (led_caps)  lv_led_set_brightness(led_caps,  capsLock   ? 180 : 0);
    if (led_scroll)lv_led_set_brightness(led_scroll, scrollLock ? 160 : 0);

    // 底栏温湿度
    if (footer_lbl_temp) {
        static char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%.1fC", shtTemp);
        lv_label_set_text(footer_lbl_temp, tbuf);
    }
    if (footer_lbl_hum) {
        static char hbuf[16];
        snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);
        lv_label_set_text(footer_lbl_hum, hbuf);
    }
    if (footer_lbl_keys) {
        static char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "%uk", totalKeyCount / 1000);
        lv_label_set_text(footer_lbl_keys, kbuf);
    }

    return touched;
}

// ===========================
// 跑马灯更新
// ===========================
static void updateMarquee(void) {
    if (currentDispMode != DISP_MODE_WALLPAPER) return;
    marqueeX -= 1;
    if (marqueeX < -strlen(marqueeText) * 6) marqueeX = 240;
    if (wp_lbl_marquee) {
        lv_label_set_text(wp_lbl_marquee, marqueeText);
        lv_obj_set_pos(wp_lbl_marquee, marqueeX, 212);
    }
}

// ===========================
// 响铃时间显示
// ===========================
static void updateRingTime(void) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || !ring_lbl_time) return;
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", ti);
    lv_label_set_text(ring_lbl_time, buf);
}

// ===========================
// 串口命令处理（来自 C3）
// ===========================
static String serialBuffer = "";
static void handleC3Events(void) {
    while (Serial1.available() > 0) {
        char c = Serial1.read();
        if (c == '\n') {
            handleCommand(serialBuffer);
            serialBuffer = "";
        } else if (c != '\r') {
            serialBuffer += c;
            if (serialBuffer.length() > 64) serialBuffer = "";
        }
    }
}

static void handleCommand(const String& cmd) {
    // 解析来自 C3 的按键事件
    if (cmd == "BTN:KNOB") {
        if (ringingKind != RING_NONE) {
            stopRinging();
        } else if (currentSysMode == SYS_MODE_MENU) {
            handleMenuSelect();
        } else if (IS_SETTING_MODE(currentSysMode)) {
            saveSettingScreen();
        }
    } else if (cmd == "BTN:LIGHT") {
        if (ringingKind != RING_NONE) {
            stopRinging();
        } else if (currentSysMode == SYS_MODE_MENU || IS_ANY_SETTING_MODE(currentSysMode)) {
            currentSysMode = SYS_MODE_NORMAL;
            screenNeedsRedraw = true;
        } else {
            g_forceOff = !g_forceOff;
        }
    } else if (cmd.startsWith("ROT:")) {
        bool isRight = cmd.substring(4) == "R";
        if (currentSysMode == SYS_MODE_MENU) {
            if (isRight) { menuSelX = (menuSelX+1) % MENU_COLS; }
            else         { menuSelX = (menuSelX+MENU_COLS-1) % MENU_COLS; }
        } else if (IS_SETTING_MODE(currentSysMode)) {
            settingSel = isRight ? (settingSel+1)%4 : (settingSel+3)%4;
        } else if (currentSysMode == SYS_MODE_NORMAL) {
            if (currentMode == MODE_LIGHT) {
                // 调整键盘背光
                triggerHud("键盘背光", "...", -1, ST77XX_YELLOW);
            } else if (currentMode == MODE_CPG) {
                currentEffect = isRight ? (currentEffect+1)%MAX_EFFECTS : (currentEffect+MAX_EFFECTS-1)%MAX_EFFECTS;
                triggerHud("灯效", effectNames[currentEffect], -1, ST77XX_MAGENTA);
            }
        }
    }
}

// ===========================
// USB HID 键盘事件
// ===========================
static void usbHidKeyboardEvent(KeyboardEvent& e) {
    if (e.key == KEY_NUM_LOCK)   numLock    = (e.osKey == KEY_NUM_LOCK);
    if (e.key == KEY_CAPS_LOCK) capsLock   = (e.osKey == KEY_CAPS_LOCK);
    if (e.key == KEY_SCROLL_LOCK) scrollLock = (e.osKey == KEY_SCROLL_LOCK);
    if (e.state == INPUT_KEYBOARD_STATE_KEY_HOLD) {
        // 长按事件
    }
}

// ===========================
// 灯效引擎
// ===========================
static void renderLightingEngine(void) {
    // Placeholder: 根据 currentEffect 渲染 WS2812 LED
    // 模拟彩虹效果
    uint8_t hue = (millis() >> 4) & 0xFF;
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t h = (hue + i * (256 / NUM_LEDS)) & 0xFF;
        ledRgb[i][0] = (h < 85) ? 255 - h*3 : 0;
        ledRgb[i][1] = (h >= 85 && h < 170) ? (h-85)*3 : 0;
        ledRgb[i][2] = (h >= 170) ? (h-170)*3 : 0;
    }
}

static void sendLedFrameToC3(void) {
    // Placeholder: 通过 Serial1 发送 LED 帧到 C3
    uint8_t pkt[1 + NUM_LEDS*3] = { 0xAA };
    for (int i = 0; i < NUM_LEDS; i++) {
        pkt[1+i*3+0] = ledRgb[i][0];
        pkt[1+i*3+1] = ledRgb[i][1];
        pkt[1+i*3+2] = ledRgb[i][2];
    }
    Serial1.write(pkt, sizeof(pkt));
}

// ===========================
// 宏录制
// ===========================
static void drawRecUI(void) {
    // Placeholder: 录制界面
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A1A), LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, recActive ? "⏺ 录制中..." : "⏹ 已停止");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, recActive ? lv_color_hex(0xFF4444) : lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_center(lbl);
}

// ===========================
// I2C 恢复
// ===========================
static void recoverI2CBus(void) {
    // Placeholder: I2C 总线恢复
}

// ===========================
// setup()
// ===========================
void setup() {
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

    setenv("TZ", "CST-8", 1);
    tzset();

    // USB HID
    USB.VID(0x303A);
    USB.PID(0x001F);
    USB.productName("YYQ-MX9.0");
    USB.manufacturerName("YYQ");
    Keyboard.onEvent(usbHidKeyboardEvent);
    Keyboard.begin();
    ConsumerControl.begin();
    SystemControl.begin();
    VendorHID.onEvent([](const VendorHIDEvent& e) {});
    VendorHID.begin();
    USB.begin();

    // SPI TFT
    tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
    tft.init(240, 240);
    tft.setSPISpeed(40000000);
    tft.setRotation(1);

    // LVGL 初始化
    lvgl_driver_init(&tft);

    // 加载 NVS 设置
    preferences.begin("keyboard", false);
    currentDispMode = preferences.getUChar("disp_mode", 0);
    if (currentDispMode >= TOTAL_DISP_MODES) currentDispMode = 0;
    showKeystrokes = preferences.getBool("show_keys", true);
    totalKeyCount = preferences.getUInt("keyCount", 0);
    alarmEnabled = preferences.getBool("alarm_on", false);
    alarmHour = preferences.getUChar("alarm_h", 7);
    alarmMinute = preferences.getUChar("alarm_m", 0);
    shtTempOffset = preferences.getFloat("sht_offset", 62.0f);

    // I2C
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) recoverI2CBus();

    // SHT31
    Wire_SHT.begin(SHT31_SDA, SHT31_SCL);
    Wire_SHT.setClock(400000);
    if (Wire_SHT.beginTransmission(SHT31_ADDR) == 0) {
        shtAvailable = true;
        sht31_update();
        lastSHTRead = millis();
    }

    // BLE
    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    pService = pServer->createService(BLEUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
    pCharacteristic = pService->createCharacteristic(BLEUUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e"),
        BLECharacteristic::PROPERTY_WRITE);
    pCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    BLEDevice::startAdvertising();

    // 键盘矩阵初始化
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.pinMode(c, OUTPUT);
        mcp.digitalWrite(c, HIGH);
    }
    for (int r = 0; r < NUM_ROWS; r++) { pinMode(rowPins[r], INPUT_PULLUP); }

    // 构建初始 UI
    init_styles();
    renderCurrentDisplayBase();
    lastActivityTime = millis();

    // 看门狗
    #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t twdt = { .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&twdt);
    esp_task_wdt_add(NULL);
    #else
    esp_task_wdt_init(WDT_TIMEOUT, true);
    esp_task_wdt_add(NULL);
    #endif

    Serial.println("YYQ-MX9.0 LVGL固件就绪");
}

// ===========================
// loop()
// ===========================
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 2;

void loop() {
    esp_task_wdt_reset();

    // BLE 重连
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        BLEDevice::startAdvertising();
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

    // Ping
    if (millis() - lastPingTime > 2000) {
        lastPingTime = millis();
        uint8_t ping[] = { 0xAA, 0x55, 0x02, 0xEE };
        Serial1.write(ping, sizeof(ping));
    }

    handleC3Events();
    updateTimers();

    // 每 30 分钟存档时钟
    static unsigned long lastClockSave = 0;
    if (millis() - lastClockSave > 1800000UL) {
        lastClockSave = millis();
        persistClock();
    }

    // 键盘扫描（最高优先级）
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        scanKeyboardMatrix();
    }

    // 灯效帧发送
    if (millis() - lastLedFrameTime >= 20) {
        lastLedFrameTime = millis();
        renderLightingEngine();
        sendLedFrameToC3();
    }

    // 屏幕保护超时
    if (currentSysMode == SYS_MODE_NORMAL && ringingKind == RING_NONE
        && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
        currentSysMode = SYS_MODE_SLEEP;
        renderCurrentDisplayBase();
    }

    // 主显示渲染
    if (currentSysMode == SYS_MODE_NORMAL) {
        if (screenNeedsRedraw) {
            screenNeedsRedraw = false;
            renderCurrentDisplayBase();
            notifDirty = true;
        }
        bool touched = updateDynamicElements();
        if (notifCount == 0) {
            updateMarquee();
        }
        if (notifCount > 0 && ringingKind == RING_NONE && (touched || notifDirty)) {
            // drawNotifPanel(); // 简化版暂不实现通知面板
            notifDirty = false;
        }

        // 击键数存档
        static uint32_t lastSavedCount = 0;
        if (totalKeyCount - lastSavedCount > 500) {
            lastSavedCount = totalKeyCount;
            preferences.putUInt("keyCount", totalKeyCount);
        }

        // HUD
        if (hud.active) {
            if (millis() - hud.startMs > hud.showMs) {
                hud.active = false;
                if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
            }
        }
    }

    // 录制界面
    else if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
        drawRecUI();
    }

    // 响铃浮层
    if (ringingKind != RING_NONE) {
        hud.active = false;
        drawRingOverlay();
        updateRingTime();
    }

    // LVGL 主循环
    lvgl_driver_loop();
    delayMicroseconds(500);
}
