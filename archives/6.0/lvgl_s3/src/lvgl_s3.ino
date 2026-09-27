/**
 * lvgl_s3.ino — YYQ-MX9.0 LVGL 核心版
 *
 * 简约纯黑风格极客仪表盘
 * - 深黑背景 + 白色/青色文字
 * - 完整键盘矩阵 + USB HID
 * - 时间 + 温湿度显示
 * - 简化菜单
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_MCP23X17.h>
#include <Preferences.h>
#include <driver/timer.h>
#include <esp_task_wdt.h>
#include <FFat.h>

// USB HID
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDSystemControl.h"
#include "USBHIDVendor.h"

// BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "lvgl_st7789_driver.h"
#include "lv_conf.h"

#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>

// ===========================
// 硬件配置
// ===========================
#define TFT_SCL    4
#define TFT_SDA   16
#define TFT_DC    15
#define TFT_CS     5
#define TFT_RST   -1

#define RX_PIN    10
#define TX_PIN     9
#define UART_BAUD 460800

#define I2C_SDA   14
#define I2C_SCL   13

#define SHT31_SDA 17
#define SHT31_SCL  7
#define SHT31_ADDR 0x44

#define MCP23017_ADDR 0x20
#define WDT_TIMEOUT  10

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
Preferences preferences;

// USB HID
USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
USBHIDVendor VendorHID;

// MCP23017
Adafruit_MCP23X17 mcp;

// SHT31 I2C
TwoWire Wire_SHT(1);

// ===========================
// 宏定义
// ===========================
#define MACRO_BASE 0xF0

// HID 键盘常量（USB HID Usage Table）
#define KEY_PRINT_SCREEN 0x46
#define KEY_SCROLL_LOCK  0x47
#define KEY_PAUSE        0x48
#define KEY_NUM_LOCK     0x53
#define KEY_KP_SLASH    0x54
#define KEY_KP_ASTERISK 0x55
#define KEY_KP_MINUS    0x56
#define KEY_KP_PLUS     0x57
#define KEY_KP_ENTER    0x58
#define KEY_KP_1        0x59
#define KEY_KP_2        0x5A
#define KEY_KP_3        0x5B
#define KEY_KP_4        0x5C
#define KEY_KP_5        0x5D
#define KEY_KP_6        0x5E
#define KEY_KP_7        0x5F
#define KEY_KP_8        0x60
#define KEY_KP_9        0x61
#define KEY_KP_0        0x62
#define KEY_KP_DOT      0x63
#define K_M1   (MACRO_BASE + 1)
#define K_M2   (MACRO_BASE + 2)
#define K_M3   (MACRO_BASE + 3)
#define K_M4   (MACRO_BASE + 4)
#define K_M5   (MACRO_BASE + 5)
#define K_M6   (MACRO_BASE + 6)
#define K_M7   (MACRO_BASE + 7)
#define K_M8   (MACRO_BASE + 8)
#define K_M9   (MACRO_BASE + 9)
#define K_M10  (MACRO_BASE + 10)
#define K_M11  (MACRO_BASE + 11)
#define K_M12  (MACRO_BASE + 12)
#define K_MA   (MACRO_BASE + 13)
#define K_MB   (MACRO_BASE + 14)
#define K_MC   (MACRO_BASE + 15)
#define K_MR   (MACRO_BASE + 16)
#define K_ME   (MACRO_BASE + 17)
#define K_LOGO (MACRO_BASE + 18)
#define K_NEXT (MACRO_BASE + 19)
#define K_PLAY (MACRO_BASE + 20)
#define K_PREV (MACRO_BASE + 21)
#define K_FN   (MACRO_BASE + 22)

// 系统模式
#define SYS_MODE_NORMAL   0
#define SYS_MODE_MENU     1
#define SYS_MODE_SLEEP    2
static uint8_t currentSysMode = SYS_MODE_NORMAL;
static bool menuNeedsRebuild = false;  // 菜单重建标志（在 loop 中处理）

// 显示模式
#define DISP_MODE_GEEK      0
#define DISP_MODE_BIG_CLOCK 1
#define TOTAL_DISP_MODES    2
static uint8_t currentDispMode = DISP_MODE_GEEK;

// 控制模式
#define MODE_LIGHT  0
#define MODE_CPG     1
static uint8_t currentMode = MODE_LIGHT;

// ===========================
// 全局状态
// ===========================

// 键盘矩阵
#define NUM_ROWS 10
#define NUM_COLS 16
#define DEBOUNCE_DELAY 20
static const uint8_t rowPins[NUM_ROWS] = { 1, 2, 42, 41, 40, 39, 38, 47, 21, 12 };
static bool keebMatrix[NUM_ROWS][NUM_COLS] = {0};
static unsigned long lastDebounceTime[NUM_ROWS][NUM_COLS] = {0};
static uint32_t totalKeyCount = 0;
static uint16_t baseMatrix[NUM_ROWS][NUM_COLS] = { 0 };
static bool fnPressed = false;

// LED
#define NUM_LEDS 16
static uint8_t ledRgb[NUM_LEDS][3];
static uint8_t brightness = 140;
static uint8_t currentEffect = 1;
static bool g_forceOff = false;
static bool cherryLogoEnabled = false;
static unsigned long lastLedFrameTime = 0;

// 锁状态
static bool numLock = false;
static bool capsLock = false;
static bool scrollLock = false;

// 温湿度
static bool shtAvailable = false;
static float shtTemp = 0.0f;
static float shtHumidity = 0.0f;
static float shtTempOffset = 62.0f;
static unsigned long lastSHTRead = 0;
#define SHT_READ_INTERVAL_MS 900000UL

// 时间
static time_t alarmLastFiredYday = -1;
static unsigned long lastActivityTime = 0;
static unsigned long pendingRestartMs = 0;
#define SLEEP_TIMEOUT_MS 60000UL

// 菜单
static uint8_t menuSel = 0;
#define MENU_ITEMS 12

// 菜单项（中文）
static const char* menuItemsCN[MENU_ITEMS] = {
    "1. 返回主屏",
    "2. 切换主屏风格",
    "3. 切换配置方案",
    "4. 按键回显开关",
    "5. 键盘背光灯效",
    "6. 状态灯亮度",
    "7. 设置时间",
    "8. 闹钟设置",
    "9. 倒计时",
    "10. 刷新温湿度",
    "11. 温度校准",
    "12. 计数清零"
};

// 辅助变量
static bool showKeystrokes = true;

// 灯效相关
static const char* effectNames[] = {
    "关闭", "纯红", "纯绿", "纯蓝", "冰蓝", "纯白",
    "红呼吸", "绿呼吸", "蓝呼吸", "冰蓝呼吸", "流光", "彗星", "幻彩"
};
#define MAX_EFFECTS 13

// 状态灯亮度
#define IND_LEVEL_COUNT 4
static const char* indLevelNames[] = { "关", "低", "中", "高" };
static const uint8_t indLevelValues[] = { 0, 64, 140, 255 };
static uint8_t indLevel = 3;
static uint8_t indBrightness = 255;

// 菜单滚动相关
#define MENU_VISIBLE_ITEMS 6
static int menuScrollOffset = 0;

// HUD
typedef struct {
    bool active;
    char title[24];
    char value[24];
    uint32_t color;  // 32-bit color (lv_color_to32)
    unsigned long showMs;
    unsigned long startMs;
} HudEntry;
static HudEntry hud = {0};

// Profile
#define TOTAL_PROFILES 4
#define MAX_REMAP_RULES 32
struct RemapRule { uint16_t fromKey; uint16_t toKey; };
static RemapRule profileRemaps[TOTAL_PROFILES][MAX_REMAP_RULES];
static int remapCounts[TOTAL_PROFILES] = { 0 };
static uint8_t currentProfile = 0;
static const char* profileNamesCN[TOTAL_PROFILES] = {
    "方案1-Windows", "方案2-macOS", "方案3-游戏模式", "方案4-工作模式"
};

// BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool deviceConnected = false;
static bool oldDeviceConnected = false;
static unsigned long lastPingTime = 0;

// ===========================
// LVGL Screen 管理
// ===========================
static lv_obj_t* scr_main = nullptr;
static lv_obj_t* scr_menu = nullptr;
static lv_obj_t* scr_settings_time = nullptr;
static lv_obj_t* scr_settings_alarm = nullptr;
static lv_obj_t* scr_settings_timer = nullptr;
static lv_obj_t* scr_settings_caltemp = nullptr;
static lv_obj_t* scr_recording = nullptr;
static lv_obj_t* currentScreen = nullptr;

// Screen切换函数
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    if (currentScreen == target) return;
    currentScreen = target;
    lv_scr_load(target);
}

// HUD浮层
static lv_obj_t* scr_hud = nullptr;

// 极客仪表盘
static lv_obj_t* gk_bg = nullptr;
static lv_obj_t* gk_lbl_clock = nullptr;
static lv_obj_t* gk_lbl_date = nullptr;
static lv_obj_t* gk_lbl_temp = nullptr;
static lv_obj_t* gk_lbl_hum = nullptr;
static lv_obj_t* gk_lbl_keys = nullptr;
static lv_obj_t* gk_led_num = nullptr;
static lv_obj_t* gk_led_caps = nullptr;
static lv_obj_t* gk_led_scr = nullptr;
static lv_obj_t* gk_lbl_profile = nullptr;

// 大时钟
static lv_obj_t* bc_bg = nullptr;
static lv_obj_t* bc_lbl_time = nullptr;
static lv_obj_t* bc_lbl_date = nullptr;

// 菜单
static lv_obj_t* menu_cont = nullptr;
static lv_obj_t* menu_items[MENU_ITEMS];
static lv_obj_t* menu_title = nullptr;
static lv_obj_t* menu_position = nullptr;

// 样式
static lv_style_t style_bg;
static lv_style_t style_text_white;
static lv_style_t style_text_cyan;
static lv_style_t style_text_gray;
static lv_style_t style_card;
static bool styles_inited = false;

// ===========================
// 颜色定义（简约纯黑）
// ===========================
#define CLR_BLACK   0x000000
#define CLR_WHITE   0xFFFFFF
#define CLR_CYAN    0x00E5FF
#define CLR_GRAY    0x808080
#define CLR_DARK    0x1A1A1A
#define CLR_ACCENT  0x00AACC

// ===========================
// 前向声明
// ===========================
static void handleCommand(const String& cmd);
static void scanKeyboardMatrix(void);
static void renderCurrentDisplayBase(void);
static void updateDynamicElements(void);
static void triggerHud(const char* title, const char* value, uint16_t color);
static void showScreen(lv_obj_t* target);
static void handleMenuSelect(void);

// ===========================
// SHT31 温湿度
// ===========================
static uint8_t sht31_crc8(const uint8_t* data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static bool sht31_read_raw(uint16_t& rawT, uint16_t& rawH) {
    Wire_SHT.beginTransmission(SHT31_ADDR);
    Wire_SHT.write(0x2C);
    Wire_SHT.write(0x06);
    if (Wire_SHT.endTransmission() != 0) return false;
    delay(20);
    if (Wire_SHT.requestFrom((int)SHT31_ADDR, 6) != 6) return false;
    uint8_t buf[6];
    for (int k = 0; k < 6; k++) buf[k] = Wire_SHT.read();
    if (sht31_crc8(&buf[0], 2) != buf[2]) return false;
    if (sht31_crc8(&buf[3], 2) != buf[5]) return false;
    rawT = ((uint16_t)buf[0] << 8) | buf[1];
    rawH = ((uint16_t)buf[3] << 8) | buf[4];
    return true;
}

static void sht31_update(void) {
    uint16_t rawT, rawH;
    if (!sht31_read_raw(rawT, rawH)) return;
    float t = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    float h = 100.0f * ((float)rawH / 65535.0f);
    shtTemp = t + shtTempOffset - 62.0f;
    shtHumidity = h;
}

// ===========================
// BLE 回调
// ===========================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String cmd = pCharacteristic->getValue().c_str();
        handleCommand(cmd);
    }
};

// ===========================
// 样式初始化
// ===========================
static void init_styles(void) {
    if (styles_inited) return;
    styles_inited = true;

    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(CLR_BLACK));
    lv_style_set_text_color(&style_bg, lv_color_hex(CLR_WHITE));

    lv_style_init(&style_text_white);
    lv_style_set_text_color(&style_text_white, lv_color_hex(CLR_WHITE));

    lv_style_init(&style_text_cyan);
    lv_style_set_text_color(&style_text_cyan, lv_color_hex(CLR_CYAN));

    lv_style_init(&style_text_gray);
    lv_style_set_text_color(&style_text_gray, lv_color_hex(CLR_GRAY));

    lv_style_init(&style_card);
    lv_style_set_bg_opa(&style_card, LV_OPA_10);
    lv_style_set_radius(&style_card, 8);
}

// ===========================
// 构建：极客仪表盘
// ===========================
static void build_style_geek(void) {
    if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }

    gk_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(gk_bg, 240, 240);
    lv_obj_set_pos(gk_bg, 0, 0);
    lv_obj_set_style_bg_color(gk_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    lv_obj_set_style_border_width(gk_bg, 0, LV_PART_MAIN);

    // 顶部状态栏
    lv_obj_t* top_bar = lv_obj_create(gk_bg);
    lv_obj_set_size(top_bar, 240, 24);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(CLR_DARK), LV_PART_MAIN);
    lv_obj_set_style_border_width(top_bar, 0, LV_PART_MAIN);

    // NUM LED
    gk_led_num = lv_led_create(top_bar);
    lv_obj_set_size(gk_led_num, 8, 8);
    lv_obj_set_pos(gk_led_num, 20, 8);
    lv_led_set_color(gk_led_num, lv_color_hex(0x00FF00));

    lv_obj_t* lbl = lv_label_create(top_bar);
    lv_label_set_text(lbl, "NUM");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 32, 7);

    // CAPS LED
    gk_led_caps = lv_led_create(top_bar);
    lv_obj_set_size(gk_led_caps, 8, 8);
    lv_obj_set_pos(gk_led_caps, 90, 8);
    lv_led_set_color(gk_led_caps, lv_color_hex(0x00FFFF));

    lbl = lv_label_create(top_bar);
    lv_label_set_text(lbl, "CAP");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 102, 7);

    // SCR LED
    gk_led_scr = lv_led_create(top_bar);
    lv_obj_set_size(gk_led_scr, 8, 8);
    lv_obj_set_pos(gk_led_scr, 160, 8);
    lv_led_set_color(gk_led_scr, lv_color_hex(0xFFFF00));

    lbl = lv_label_create(top_bar);
    lv_label_set_text(lbl, "SCR");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_set_pos(lbl, 172, 7);

    // 方案标签
    gk_lbl_profile = lv_label_create(top_bar);
    lv_label_set_text(gk_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_set_style_text_font(gk_lbl_profile, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_profile, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(gk_lbl_profile, LV_ALIGN_TOP_RIGHT, -10, 7);

    // 主时钟
    gk_lbl_clock = lv_label_create(gk_bg);
    lv_label_set_text(gk_lbl_clock, "--:--");
    lv_obj_set_style_text_font(gk_lbl_clock, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_clock, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
    lv_obj_align(gk_lbl_clock, LV_ALIGN_CENTER, 0, -30);

    // 日期
    gk_lbl_date = lv_label_create(gk_bg);
    lv_label_set_text(gk_lbl_date, "----/--/-- --");
    lv_obj_set_style_text_font(gk_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_date, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(gk_lbl_date, LV_ALIGN_CENTER, 0, 20);

    // 分隔线
    lv_obj_t* line = lv_line_create(gk_bg);
    static lv_point_t pts[] = {{20, 160}, {220, 160}};
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_line_width(line, 1, LV_PART_MAIN);

    // 温湿度标签
    gk_lbl_temp = lv_label_create(gk_bg);
    lv_label_set_text(gk_lbl_temp, "--.-C");
    lv_obj_set_style_text_font(gk_lbl_temp, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_temp, lv_color_hex(0xFF8C42), LV_PART_MAIN);
    lv_obj_align(gk_lbl_temp, LV_ALIGN_LEFT_MID, 30, 45);

    lbl = lv_label_create(gk_bg);
    lv_label_set_text(lbl, "TEMP");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 28, 62);

    gk_lbl_hum = lv_label_create(gk_bg);
    lv_label_set_text(gk_lbl_hum, "--%");
    lv_obj_set_style_text_font(gk_lbl_hum, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_hum, lv_color_hex(0x42C8FF), LV_PART_MAIN);
    lv_obj_align(gk_lbl_hum, LV_ALIGN_RIGHT_MID, -30, 45);

    lbl = lv_label_create(gk_bg);
    lv_label_set_text(lbl, "HUM");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -32, 62);

    // 击键数
    gk_lbl_keys = lv_label_create(gk_bg);
    lv_label_set_text(gk_lbl_keys, "0");
    lv_obj_set_style_text_font(gk_lbl_keys, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(gk_lbl_keys, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(gk_lbl_keys, LV_ALIGN_CENTER, 0, 45);

    lbl = lv_label_create(gk_bg);
    lv_label_set_text(lbl, "KEYS");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 70);
}

// ===========================
// 构建：大时钟
// ===========================
static void build_style_bigclock(void) {
    if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }

    bc_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bc_bg, 240, 240);
    lv_obj_set_pos(bc_bg, 0, 0);
    lv_obj_set_style_bg_color(bc_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    lv_obj_set_style_border_width(bc_bg, 0, LV_PART_MAIN);

    // 主时钟
    bc_lbl_time = lv_label_create(bc_bg);
    lv_label_set_text(bc_lbl_time, "--:--");
    lv_obj_set_style_text_font(bc_lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(bc_lbl_time, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
    lv_obj_align(bc_lbl_time, LV_ALIGN_CENTER, 0, -20);

    // 日期
    bc_lbl_date = lv_label_create(bc_bg);
    lv_label_set_text(bc_lbl_date, "----/--/-- --");
    lv_obj_set_style_text_font(bc_lbl_date, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(bc_lbl_date, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(bc_lbl_date, LV_ALIGN_CENTER, 0, 30);
}

// ===========================
// 菜单选择处理
// ===========================
static void handleMenuSelect(void) {
    switch (menuSel) {
        case 0:  // 返回主屏
            currentSysMode = SYS_MODE_NORMAL;
            showScreen(scr_main);
            break;
        case 1:  // 切换主屏风格 - 循环切换
            currentDispMode = (currentDispMode + 1) % TOTAL_DISP_MODES;
            renderCurrentDisplayBase();
            triggerHud("Style",
                currentDispMode == DISP_MODE_GEEK ? "Geek Mode" : "Big Clock",
                lv_color_hex(CLR_CYAN));
            break;
        case 2:  // 切换配置方案 - 循环切换
            switchProfile((currentProfile + 1) % TOTAL_PROFILES);
            break;
        case 3:  // 按键回显开关
            showKeystrokes = !showKeystrokes;
            triggerHud("Keystrokes", showKeystrokes ? "ON" : "OFF",
                lv_color_hex(showKeystrokes ? 0x00FF00 : 0xFF0000));
            break;
        case 4:  // 键盘背光灯效
            currentEffect = (currentEffect + 1) % MAX_EFFECTS;
            triggerHud("Light Effect", effectNames[currentEffect], lv_color_hex(0xFF00FF));
            break;
        case 5:  // 状态灯亮度
            indLevel = (indLevel + 1) % IND_LEVEL_COUNT;
            indBrightness = indLevelValues[indLevel];
            triggerHud("LED Brightness", indLevelNames[indLevel], lv_color_hex(0xFFFF00));
            break;
        case 6:  // 设置时间
            showScreen(scr_settings_time);
            break;
        case 7:  // 闹钟设置
            showScreen(scr_settings_alarm);
            break;
        case 8:  // 倒计时
            showScreen(scr_settings_timer);
            break;
        case 9:  // 刷新温湿度
            if (shtAvailable) {
                sht31_update();
                triggerHud("SHT31", "Refreshed", lv_color_hex(0x00FF00));
            } else {
                triggerHud("SHT31", "Not Found", lv_color_hex(0xFF0000));
            }
            break;
        case 10: // 温度校准
            showScreen(scr_settings_caltemp);
            break;
        case 11: // 计数清零
            totalKeyCount = 0;
            preferences.putUInt("keyCount", 0);
            triggerHud("Key Count", "Reset to 0", lv_color_hex(0x00FFFF));
            break;
    }
}

// ===========================
// 构建：菜单（支持12项滚动）
// ===========================
static void build_menu(void) {
    // 计算滚动偏移
    if (menuSel < menuScrollOffset) {
        menuScrollOffset = menuSel;
    } else if (menuSel >= menuScrollOffset + MENU_VISIBLE_ITEMS) {
        menuScrollOffset = menuSel - MENU_VISIBLE_ITEMS + 1;
    }

    if (scr_menu == nullptr) {
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BLACK), LV_PART_MAIN);

        // 标题
        menu_title = lv_label_create(scr_menu);
        lv_label_set_text(menu_title, "MENU");
        lv_obj_set_style_text_font(menu_title, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(menu_title, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
        lv_obj_align(menu_title, LV_ALIGN_TOP_MID, 0, 10);

        // 位置指示
        menu_position = lv_label_create(scr_menu);
        lv_obj_set_style_text_font(menu_position, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(menu_position, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
        lv_obj_align(menu_position, LV_ALIGN_TOP_MID, 0, 35);

        // 菜单容器
        menu_cont = lv_obj_create(scr_menu);
        lv_obj_set_size(menu_cont, 200, MENU_VISIBLE_ITEMS * 32);
        lv_obj_align(menu_cont, LV_ALIGN_CENTER, 0, 15);
        lv_obj_set_style_bg_color(menu_cont, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        lv_obj_set_style_border_width(menu_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(menu_cont, 0, LV_PART_MAIN);

        // 创建菜单项（全部12项）
        for (int i = 0; i < MENU_ITEMS; i++) {
            lv_obj_t* btn = lv_btn_create(menu_cont);
            lv_obj_set_size(btn, 190, 30);
            lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_DARK), LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, menuItemsCN[i]);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
            lv_obj_center(lbl);

            menu_items[i] = btn;
        }
    }

    // 更新位置指示
    if (menu_position) {
        static char posBuf[24];
        snprintf(posBuf, sizeof(posBuf), "%d/%d", menuSel + 1, MENU_ITEMS);
        lv_label_set_text(menu_position, posBuf);
    }

    // 更新菜单项位置和可见性
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_items[i]) {
            int yPos = i * 32;
            lv_obj_set_pos(menu_items[i], 5, yPos);

            // 选中项高亮
            bool isSelected = (i == menuSel);
            bool isVisible = (i >= menuScrollOffset && i < menuScrollOffset + MENU_VISIBLE_ITEMS);
            lv_obj_set_style_bg_color(menu_items[i],
                lv_color_hex(isSelected ? CLR_CYAN : CLR_DARK), LV_PART_MAIN);
            lv_obj_t* lbl = lv_obj_get_child(menu_items[i], 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(isSelected ? CLR_BLACK : CLR_WHITE), LV_PART_MAIN);
            }
            // 隐藏不在可见范围的项
            lv_obj_set_style_opa(menu_items[i], isVisible ? LV_OPA_100 : LV_OPA_0, LV_PART_MAIN);
            lv_obj_set_style_clip_corner(menu_items[i], true, LV_PART_MAIN);
        }
    }

    showScreen(scr_menu);
}

// ===========================
// HUD 浮层
// ===========================
static void build_hud(void) {
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }

    scr_hud = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_hud, 200, 60);
    lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(scr_hud, lv_color_hex(CLR_DARK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_hud, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(scr_hud, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_hud, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_hud, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_clear_flag(scr_hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr_hud);

    lv_obj_t* title = lv_label_create(scr_hud);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
    lv_label_set_text(title, hud.title);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* value = lv_label_create(scr_hud);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_hex(hud.color), LV_PART_MAIN);
    lv_label_set_text(value, hud.value);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void triggerHud(const char* title, const char* value, lv_color_t color) {
    hud.active = true;
    strncpy(hud.title, title, sizeof(hud.title) - 1);
    hud.title[sizeof(hud.title) - 1] = '\0';
    strncpy(hud.value, value, sizeof(hud.value) - 1);
    hud.value[sizeof(hud.value) - 1] = '\0';
    hud.color = lv_color_to32(color);
    hud.startMs = millis();
    hud.showMs = 1500;

    build_hud();
}

// ===========================
// Profile 管理
// ===========================
void loadRemapsFromStorage() {
    for (int p = 0; p < TOTAL_PROFILES; p++) {
        char key[16];
        snprintf(key, sizeof(key), "rmp_cnt_%d", p);
        remapCounts[p] = preferences.getInt(key, 0);
        if (remapCounts[p] > MAX_REMAP_RULES) remapCounts[p] = MAX_REMAP_RULES;
        for (int i = 0; i < remapCounts[p]; i++) {
            char itemKey[20];
            snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", p, i);
            uint32_t val = preferences.getUInt(itemKey, 0);
            profileRemaps[p][i].fromKey = (uint16_t)(val >> 16);
            profileRemaps[p][i].toKey = (uint16_t)(val & 0xFFFF);
        }
    }
}

void switchProfile(uint8_t profIdx) {
    if (profIdx >= TOTAL_PROFILES) return;
    currentProfile = profIdx;
    preferences.putUChar("curr_prof", profIdx);
    triggerHud("Profile", profileNamesCN[currentProfile], lv_color_hex(CLR_CYAN));
}

// ===========================
// 键盘工具函数
// ===========================
static inline void kbPress(uint8_t code) {
    if (code >= 0x80 && code < 0x88) Keyboard.pressRaw((uint8_t)(code + 0x60));
    else Keyboard.press(code);
}

static inline void kbRelease(uint8_t code) {
    if (code >= 0x80 && code < 0x88) Keyboard.releaseRaw((uint8_t)(code + 0x60));
    else Keyboard.release(code);
}

static String getMacroNameByCode(uint16_t code) {
    if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
    if (code == K_MA) return "MA";
    if (code == K_MB) return "MB";
    if (code == K_MC) return "MC";
    if (code == K_MR) return "MR";
    if (code == K_ME) return "ME";
    if (code == K_LOGO) return "LOGO";
    return "";
}

static uint16_t getMappedKey(uint16_t originalKey) {
    for (int i = 0; i < remapCounts[currentProfile]; i++) {
        if (profileRemaps[currentProfile][i].fromKey == originalKey) {
            return profileRemaps[currentProfile][i].toKey;
        }
    }
    return originalKey;
}

// ===========================
// 宏执行
// ===========================
static void executeSequenceAction(String seq) {
    int i = 0;
    while (i < seq.length()) {
        if (seq.substring(i).startsWith("SLEEP(")) {
            int endP = seq.indexOf(')', i + 6);
            if (endP != -1) {
                delay(seq.substring(i + 6, endP).toInt());
                i = endP + 1;
            } else { i++; }
        } else {
            uint8_t c = (uint8_t)seq[i];
            if (c >= 32 && c <= 126) Keyboard.write(c);
            else if (c == 0xB0) Keyboard.write(' ');
            i++;
        }
    }
}

static void executeMacro(String keyName) {
    char pKey[32];
    snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, keyName.c_str());
    String macroData = preferences.getString(pKey, "");
    if (macroData.length() == 0) return;
    if (macroData.startsWith("SEQ:")) executeSequenceAction(macroData.substring(4));
}

static void executeGlobalKey(String gKey) {
    String val = preferences.getString(("g_" + gKey).c_str(), "");
    if (val.length() == 0) {
        if (gKey == "MA") switchProfile(0);
        else if (gKey == "MB") switchProfile(1);
        return;
    }
    if (val.startsWith("SW:")) {
        int plusIdx = val.indexOf('+');
        String swPart = (plusIdx != -1) ? val.substring(3, plusIdx) : val.substring(3);
        if (swPart == "NEXT") switchProfile((currentProfile + 1) % TOTAL_PROFILES);
        else if (swPart != "NONE") switchProfile(swPart.toInt());
    }
}

// ===========================
// USB HID 事件
// ===========================
static void usbHidKeyboardEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
        auto* led_data = (arduino_usb_hid_keyboard_event_data_t*)data;
        numLock = led_data->numlock;
        capsLock = led_data->capslock;
        scrollLock = led_data->scrolllock;
    }
}

// ===========================
// 键盘矩阵扫描
// ===========================
static void initBaseMatrix(void) {
    memset(baseMatrix, 0, sizeof(baseMatrix));

    // Row 0
    baseMatrix[0][0] = KEY_LEFT_ALT;
    baseMatrix[0][1] = 0xE9; baseMatrix[0][2] = 0xE8; baseMatrix[0][3] = 0xE7;
    baseMatrix[0][4] = 0xDE; baseMatrix[0][5] = 0xDD; baseMatrix[0][6] = 0xDC;
    baseMatrix[0][7] = 0xDB; baseMatrix[0][8] = KEY_RIGHT_ARROW; baseMatrix[0][9] = KEY_DOWN_ARROW;
    baseMatrix[0][10] = KEY_LEFT_ARROW; baseMatrix[0][11] = KEY_RIGHT_CTRL;
    baseMatrix[0][12] = 0xED; baseMatrix[0][13] = K_FN; baseMatrix[0][14] = KEY_RIGHT_ALT; baseMatrix[0][15] = ' ';

    // Row 1
    baseMatrix[1][0] = 0xDF; baseMatrix[1][1] = K_MB; baseMatrix[1][2] = K_MA;
    baseMatrix[1][3] = K_NEXT; baseMatrix[1][4] = K_PLAY; baseMatrix[1][5] = K_PREV; baseMatrix[1][6] = K_LOGO;
    baseMatrix[1][7] = 0xE0; baseMatrix[1][8] = 0xEB; baseMatrix[1][9] = 0xEA;
    baseMatrix[1][10] = 0xE3; baseMatrix[1][11] = 0xE2; baseMatrix[1][12] = 0xE1;
    baseMatrix[1][13] = 0xE6; baseMatrix[1][14] = 0xE5; baseMatrix[1][15] = 0xE4;

    // Row 2
    baseMatrix[2][0] = KEY_LEFT_SHIFT; baseMatrix[2][1] = KEY_LEFT_GUI; baseMatrix[2][2] = KEY_LEFT_CTRL;
    baseMatrix[2][3] = KEY_UP_ARROW; baseMatrix[2][4] = KEY_RIGHT_SHIFT;
    baseMatrix[2][5] = '/'; baseMatrix[2][6] = '.'; baseMatrix[2][7] = ',';
    baseMatrix[2][8] = 'm'; baseMatrix[2][9] = 'n'; baseMatrix[2][10] = 'b';
    baseMatrix[2][11] = 'v'; baseMatrix[2][12] = 'c'; baseMatrix[2][13] = 'x'; baseMatrix[2][14] = 'z';

    // Row 3: 物理空行，保持为 0

    // Row 4
    baseMatrix[4][0] = KEY_END; baseMatrix[4][1] = KEY_RETURN; baseMatrix[4][2] = KEY_INSERT;
    baseMatrix[4][3] = '\''; baseMatrix[4][4] = ';'; baseMatrix[4][5] = 'l'; baseMatrix[4][6] = 'k'; baseMatrix[4][7] = 'j';
    baseMatrix[4][8] = 'h'; baseMatrix[4][9] = 'g'; baseMatrix[4][10] = 'f'; baseMatrix[4][11] = 'd';
    baseMatrix[4][12] = 's'; baseMatrix[4][13] = 'a'; baseMatrix[4][14] = KEY_CAPS_LOCK; baseMatrix[4][15] = 0xD6;

    // Row 5
    baseMatrix[5][0] = KEY_PAGE_UP; baseMatrix[5][1] = KEY_DELETE; baseMatrix[5][2] = '\\';
    baseMatrix[5][3] = ']'; baseMatrix[5][4] = '['; baseMatrix[5][5] = 'p'; baseMatrix[5][6] = 'o';
    baseMatrix[5][7] = 'i'; baseMatrix[5][8] = 'u'; baseMatrix[5][9] = 'y'; baseMatrix[5][10] = 't';
    baseMatrix[5][11] = 'r'; baseMatrix[5][12] = 'e'; baseMatrix[5][13] = 'w'; baseMatrix[5][14] = 'q';
    baseMatrix[5][15] = KEY_TAB;

    // Row 6
    baseMatrix[6][0] = '`'; baseMatrix[6][1] = KEY_HOME; baseMatrix[6][2] = KEY_INSERT;
    baseMatrix[6][3] = KEY_BACKSPACE; baseMatrix[6][4] = '='; baseMatrix[6][5] = '-';
    baseMatrix[6][6] = '0'; baseMatrix[6][7] = '9'; baseMatrix[6][8] = '8'; baseMatrix[6][9] = '7';
    baseMatrix[6][10] = '6'; baseMatrix[6][11] = '5'; baseMatrix[6][12] = '4'; baseMatrix[6][13] = '3';
    baseMatrix[6][14] = '2'; baseMatrix[6][15] = '1';

    // Row 7
    baseMatrix[7][0] = KEY_ESC;
    baseMatrix[7][1] = 0xD0; baseMatrix[7][2] = 0xCF; baseMatrix[7][3] = 0xCE;
    baseMatrix[7][4] = KEY_F12; baseMatrix[7][5] = KEY_F11; baseMatrix[7][6] = KEY_F10; baseMatrix[7][7] = KEY_F9;
    baseMatrix[7][8] = KEY_F8; baseMatrix[7][9] = KEY_F7; baseMatrix[7][10] = KEY_F6; baseMatrix[7][11] = KEY_F5;
    baseMatrix[7][12] = KEY_F4; baseMatrix[7][13] = KEY_F3; baseMatrix[7][14] = KEY_F2; baseMatrix[7][15] = KEY_F1;

    // Row 8
    baseMatrix[8][0] = K_MC; baseMatrix[8][1] = K_ME; baseMatrix[8][2] = K_ME; baseMatrix[8][3] = K_M12;
    baseMatrix[8][4] = K_M11; baseMatrix[8][5] = K_M10; baseMatrix[8][6] = K_M9; baseMatrix[8][7] = K_M8;
    baseMatrix[8][8] = K_M7; baseMatrix[8][9] = K_M6; baseMatrix[8][10] = K_M5; baseMatrix[8][11] = K_M4;
    baseMatrix[8][12] = K_M3; baseMatrix[8][13] = K_M2; baseMatrix[8][14] = K_M1; baseMatrix[8][15] = K_MR;

    // Row 9: 全 0（与原版一致）
}

static void scanKeyboardMatrix(void) {
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.digitalWrite(c, LOW);
        delayMicroseconds(20);

        for (int r = 0; r < NUM_ROWS; r++) {
            bool currentState = (digitalRead(rowPins[r]) == LOW);
            if (currentState != keebMatrix[r][c]) {
                if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
                    lastDebounceTime[r][c] = millis();
                    keebMatrix[r][c] = currentState;

                    uint16_t baseKey = baseMatrix[r][c];
                    if (baseKey == 0) continue;

                    if (currentState) {
                        // 按键按下
                        totalKeyCount++;
                        lastActivityTime = millis();

                        // 唤醒
                        if (currentSysMode == SYS_MODE_SLEEP) {
                            currentSysMode = SYS_MODE_NORMAL;
                            renderCurrentDisplayBase();
                        }

                        // Fn 键
                        if (baseKey == K_FN) {
                            fnPressed = true;
                        }
                        // MC 进入菜单
                        else if (baseKey == K_MC) {
                            currentSysMode = SYS_MODE_MENU;
                            menuSel = 0;
                            menuNeedsRebuild = true;
                            triggerHud("MENU", "System Settings", lv_color_hex(CLR_CYAN));
                        }
                        // MA / MB 全局键
                        else if (baseKey == K_MA) {
                            executeGlobalKey("MA");
                        }
                        else if (baseKey == K_MB) {
                            executeGlobalKey("MB");
                        }
                        // 宏按键
                        else if (baseKey >= MACRO_BASE) {
                            if (baseKey == K_LOGO) {
                                if (fnPressed) {
                                    triggerHud("LOGO+Fn", "Reboot...", lv_color_hex(0xFF0000));
                                    pendingRestartMs = millis() + 600;
                                }
                            } else if (baseKey == K_PLAY) {
                                if (fnPressed) {
                                    SystemControl.press(SYSTEM_CONTROL_STANDBY);
                                    SystemControl.release();
                                } else {
                                    ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                                    triggerHud("Media", "Play/Pause", lv_color_hex(0x00FF00));
                                }
                            } else if (baseKey == K_NEXT) {
                                if (!fnPressed) {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
                                    triggerHud("Media", "Next", lv_color_hex(0x00FFFF));
                                }
                            } else if (baseKey == K_PREV) {
                                if (!fnPressed) {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
                                    triggerHud("Media", "Prev", lv_color_hex(0x00FFFF));
                                }
                            } else {
                                String macroName = getMacroNameByCode(baseKey);
                                if (baseKey >= K_M1 && baseKey <= K_M12) {
                                    executeGlobalKey(macroName);
                                }
                                executeMacro(macroName);
                            }
                        }
                        // 普通按键
                        else {
                            // 菜单模式下的导航
                            if (currentSysMode == SYS_MODE_MENU) {
                                if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                                    menuSel = (menuSel + 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
                                    menuSel = (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_RETURN) {
                                    // 选择菜单项
                                    handleMenuSelect();
                                } else if (baseKey == KEY_ESC) {
                                    // ESC：返回主屏
                                    currentSysMode = SYS_MODE_NORMAL;
                                    showScreen(scr_main);
                                }
                            } else {
                                uint16_t mappedKey = getMappedKey(baseKey);
                                kbPress((uint8_t)mappedKey);
                            }
                        }
                    } else {
                        // 按键释放
                        if (baseKey == K_FN) {
                            fnPressed = false;
                        }
                        else if (baseKey >= MACRO_BASE) {
                            if (baseKey == K_PLAY || baseKey == K_NEXT || baseKey == K_PREV) {
                                ConsumerControl.release();
                            } else if (baseKey == K_LOGO && !fnPressed) {
                                cherryLogoEnabled = !cherryLogoEnabled;
                                triggerHud("LOGO", cherryLogoEnabled ? "Effect On" : "Effect Off", lv_color_hex(0xFF00FF));
                            }
                        }
                        else {
                            // 菜单模式下的导航键不发送
                            if (currentSysMode == SYS_MODE_MENU) {
                                // 仅拦截，不发送
                            } else {
                                uint16_t mappedKey = getMappedKey(baseKey);
                                kbRelease((uint8_t)mappedKey);
                            }
                        }
                    }
                }
            }
        }
        mcp.digitalWrite(c, HIGH);
    }
}

// ===========================
// I2C 恢复
// ===========================
static void recoverI2CBus(void) {
    Wire.end();
    delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);
    if (mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        for (int c = 0; c < NUM_COLS; c++) {
            mcp.pinMode(c, OUTPUT);
            mcp.digitalWrite(c, HIGH);
        }
    }
}

// ===========================
// 命令处理
// ===========================
static void handleCommand(const String& cmd) {
    if (cmd.startsWith("TIME:")) {
        time_t t = cmd.substring(5).toInt();
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        triggerHud("Time", "Synced", lv_color_hex(0x00FF00));
    }
    else if (cmd == "BTN:KNOB") {
        if (currentSysMode == SYS_MODE_MENU) {
            // 菜单选择
            handleMenuSelect();
        }
    }
    else if (cmd == "BTN:LIGHT") {
        if (currentSysMode == SYS_MODE_MENU) {
            // 返回主屏
            currentSysMode = SYS_MODE_NORMAL;
            showScreen(scr_main);
        }
    }
    else if (cmd.startsWith("ROT:")) {
        bool isRight = cmd.substring(4) == "R";
        if (currentSysMode == SYS_MODE_MENU) {
            menuSel = isRight ? (menuSel + 1) % MENU_ITEMS : (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
            build_menu();
        }
    }
    else if (cmd.startsWith("ALARM:")) {
        // 闹钟设置
    }
}

// ===========================
// 灯效引擎
// ===========================
static void renderLightingEngine(void) {
    if (g_forceOff) {
        for (int i = 0; i < NUM_LEDS; i++) {
            ledRgb[i][0] = ledRgb[i][1] = ledRgb[i][2] = 0;
        }
        return;
    }

    uint8_t hue = (millis() >> 4) & 0xFF;
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t h = (hue + i * (256 / NUM_LEDS)) & 0xFF;
        if (h < 85) {
            ledRgb[i][0] = 255 - h * 3;
            ledRgb[i][1] = h * 3;
            ledRgb[i][2] = 0;
        } else if (h < 170) {
            ledRgb[i][0] = 0;
            ledRgb[i][1] = 255 - (h - 85) * 3;
            ledRgb[i][2] = (h - 85) * 3;
        } else {
            ledRgb[i][0] = (h - 170) * 3;
            ledRgb[i][1] = 0;
            ledRgb[i][2] = 255 - (h - 170) * 3;
        }
    }
}

static void sendLedFrameToC3(void) {
    uint8_t packet[61];
    packet[0] = 0xAA;
    packet[1] = 0x55;
    packet[2] = 0x01;

    uint8_t ledScale = brightness;
    for (int i = 0; i < NUM_LEDS; i++) {
        packet[3 + i * 3 + 0] = (ledRgb[i][0] * ledScale) / 255;
        packet[3 + i * 3 + 1] = (ledRgb[i][1] * ledScale) / 255;
        packet[3 + i * 3 + 2] = (ledRgb[i][2] * ledScale) / 255;
    }
    for (int i = NUM_LEDS; i < 19; i++) {
        packet[3 + i * 3 + 0] = 0;
        packet[3 + i * 3 + 1] = 0;
        packet[3 + i * 3 + 2] = 0;
    }
    packet[60] = 0xEE;
    Serial1.write(packet, sizeof(packet));
}

// ===========================
// 动态元素更新
// ===========================
static void updateDynamicElements(void) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || ti->tm_year < 124) return;

    char time_buf[16], date_buf[32];
    static const char* WEEK[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    if (currentDispMode == DISP_MODE_GEEK) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
        snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d %s",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, WEEK[ti->tm_wday]);

        if (gk_lbl_clock) lv_label_set_text(gk_lbl_clock, time_buf);
        if (gk_lbl_date) lv_label_set_text(gk_lbl_date, date_buf);

        if (gk_lbl_temp) {
            static char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.1fC", shtTemp);
            lv_label_set_text(gk_lbl_temp, tbuf);
        }
        if (gk_lbl_hum) {
            static char hbuf[16];
            snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);
            lv_label_set_text(gk_lbl_hum, hbuf);
        }
        if (gk_lbl_keys) {
            static char kbuf[16];
            snprintf(kbuf, sizeof(kbuf), "%u", totalKeyCount);
            lv_label_set_text(gk_lbl_keys, kbuf);
        }

        // LED 状态
        if (gk_led_num) lv_led_set_brightness(gk_led_num, numLock ? 200 : 0);
        if (gk_led_caps) lv_led_set_brightness(gk_led_caps, capsLock ? 200 : 0);
        if (gk_led_scr) lv_led_set_brightness(gk_led_scr, scrollLock ? 200 : 0);

        // 方案标签
        if (gk_lbl_profile) lv_label_set_text(gk_lbl_profile, profileNamesCN[currentProfile]);
    }
    else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
        snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d %s",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, WEEK[ti->tm_wday]);

        if (bc_lbl_time) lv_label_set_text(bc_lbl_time, time_buf);
        if (bc_lbl_date) lv_label_set_text(bc_lbl_date, date_buf);
    }
}

// ===========================
// 主屏渲染（预创建模式）
// ===========================
static void renderCurrentDisplayBase(void) {
    init_styles();

    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);

        if (currentDispMode == DISP_MODE_GEEK) {
            build_style_geek();
        } else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
            build_style_bigclock();
        }
    } else {
        // 如果显示模式改变了，需要重建内容
        static uint8_t lastDispMode = 0xFF;  // 初始值无效
        if (lastDispMode != currentDispMode) {
            lastDispMode = currentDispMode;

            // 删除旧内容
            if (currentDispMode == DISP_MODE_GEEK) {
                if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }
                build_style_geek();
            } else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
                if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }
                build_style_bigclock();
            }
        }
    }

    showScreen(scr_main);
}

// ===========================
// setup()
// ===========================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

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
    VendorHID.begin();
    USB.begin();

    // SPI TFT
    tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
    tft.init(240, 240);
    tft.setSPISpeed(40000000);
    tft.setRotation(1);

    // LVGL
    lvgl_driver_init(&tft);
    Serial.println("[LVGL] Ready");

    // NVS
    preferences.begin("keyboard", false);

    // 恢复时间
    time_t savedEpoch = (time_t)preferences.getUInt("set_epoch", 0);
    if (savedEpoch > 0) {
        time_t curEpoch = time(nullptr);
        struct tm* curTm = localtime(&curEpoch);
        if (curTm != nullptr && curTm->tm_year < 120) {
            struct timeval tv = { .tv_sec = savedEpoch, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
        }
    }

    currentDispMode = preferences.getUChar("disp_mode", DISP_MODE_GEEK);
    currentProfile = preferences.getUChar("curr_prof", 0);
    totalKeyCount = preferences.getUInt("keyCount", 0);
    cherryLogoEnabled = preferences.getBool("cherryLogo", false);
    shtTempOffset = preferences.getFloat("sht_offset", 62.0f);

    // I2C / MCP23017
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        Serial.println("[MCP23017] Init failed, recovering...");
        recoverI2CBus();
    }

    // SHT31
    Wire_SHT.begin(SHT31_SDA, SHT31_SCL);
    Wire_SHT.setClock(400000);
    Wire_SHT.beginTransmission(SHT31_ADDR);
    if (Wire_SHT.endTransmission() == 0) {
        shtAvailable = true;
        sht31_update();
        lastSHTRead = millis();
    }

    // BLE
    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharacteristic->setCallbacks(new MyCallbacks());
    pService->start();
    BLEDevice::startAdvertising();

    // FFat
    FFat.begin(true);

    // 键盘矩阵
    initBaseMatrix();
    loadRemapsFromStorage();
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.pinMode(c, OUTPUT);
        mcp.digitalWrite(c, HIGH);
    }
    for (int r = 0; r < NUM_ROWS; r++) {
        pinMode(rowPins[r], INPUT_PULLUP);
    }

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

    // 初始化显示
    renderCurrentDisplayBase();
    lastActivityTime = millis();

    Serial.println("YYQ-MX9.0 LVGL Ready");
}

// ===========================
// loop()
// ===========================
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 2;

void loop() {
    esp_task_wdt_reset();

    // 延迟重启
    if (pendingRestartMs != 0 && (long)(millis() - pendingRestartMs) >= 0) {
        pendingRestartMs = 0;
        esp_restart();
    }

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

    // 温湿度
    if (shtAvailable && millis() - lastSHTRead > SHT_READ_INTERVAL_MS) {
        sht31_update();
        lastSHTRead = millis();
    }

    // 键盘扫描
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        scanKeyboardMatrix();
    }

    // 灯效
    if (millis() - lastLedFrameTime >= 20) {
        lastLedFrameTime = millis();
        renderLightingEngine();
        sendLedFrameToC3();
    }

    // 息屏
    if (currentSysMode == SYS_MODE_NORMAL && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
        currentSysMode = SYS_MODE_SLEEP;
        // 息屏时清空显示
        if (scr_main) { lv_obj_del(scr_main); scr_main = nullptr; }
    }

    // 菜单重建（在主循环中处理，避免与 LVGL 冲突）
    if (menuNeedsRebuild) {
        menuNeedsRebuild = false;
        build_menu();
    }

    // 主显示
    if (currentSysMode == SYS_MODE_NORMAL) {
        updateDynamicElements();
    }

    // HUD 消失
    if (hud.active && millis() - hud.startMs > hud.showMs) {
        hud.active = false;
        if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
    }

    // LVGL
    lvgl_driver_loop();
    delayMicroseconds(500);
}
