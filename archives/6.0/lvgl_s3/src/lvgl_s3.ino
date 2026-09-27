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
#include "crash_trace.h"

#include <esp_heap_caps.h>

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
// 设置类模式必须连续，moveSettingField / adjustSettingField / save / cancel
// 都靠 IS_SETTING_MODE 一次性接管。新增 SYS_MODE_SET_LIGHT 时记得把区间上界一起改。
#define SYS_MODE_NORMAL    0
#define SYS_MODE_MENU      1
#define SYS_MODE_SLEEP     2
#define SYS_MODE_SET_TIME  3
#define SYS_MODE_SET_ALARM 4
#define SYS_MODE_SET_TIMER 5
#define SYS_MODE_CAL_TEMP  6
#define SYS_MODE_SET_LIGHT 7
#define SYS_MODE_REC_SEQ   8
#define SYS_MODE_REC_CMB   9
#define IS_SETTING_MODE(m) ((m) >= SYS_MODE_SET_TIME && (m) <= SYS_MODE_SET_LIGHT)
static uint8_t currentSysMode = SYS_MODE_NORMAL;
static bool menuNeedsRebuild = false;  // 菜单重建标志（在 loop 中处理）

// 显示模式
#define DISP_MODE_GEEK        0
#define DISP_MODE_BIG_CLOCK   1
#define DISP_MODE_INFO_PANEL  2
#define DISP_MODE_KEY_MON     3
#define DISP_MODE_RHYTHM      4
#define DISP_MODE_WALLPAPER   5
#define TOTAL_DISP_MODES      6

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸"
};

static uint8_t currentDispMode = DISP_MODE_GEEK;

// 控制模式
#define MODE_LIGHT  0
#define MODE_CPG     1
static uint8_t currentMode = MODE_LIGHT;

// ===========================
// 通知系统
// ===========================
enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };

#define MAX_NOTIFS 8
static uint8_t notifCount = 0;
static AlertType notifQueue[MAX_NOTIFS];
static char notifTexts[MAX_NOTIFS][128];
static lv_obj_t* scr_notif = nullptr;

// ===========================
// 全局状态
// ===========================

// 键盘矩阵
#define NUM_ROWS 10
#define NUM_COLS 16
#define DEBOUNCE_DELAY 20
static const uint8_t rowPins[NUM_ROWS] = { 1, 2, 42, 41, 40, 39, 38, 47, 21, 12 };
static bool keebMatrix[NUM_ROWS][NUM_COLS] = {0};
// 记录这颗键"按下时是否真的发给过主机"，松键时据此决定要不要补一个 release。
// 没有它的话，菜单里按 ESC 这类被界面吞掉的键会在松手时给主机发一个无头的 release。
static bool hostKeyHeld[NUM_ROWS][NUM_COLS] = {0};
static unsigned long lastDebounceTime[NUM_ROWS][NUM_COLS] = {0};
static uint32_t totalKeyCount = 0;
static uint16_t baseMatrix[NUM_ROWS][NUM_COLS] = { 0 };
static bool fnPressed = false;

// LED
// 0~15  是主背光（16 颗），按 brightness 缩放
// 16/17/18 是 C3 上的 NUM / CAPS / SCR 三颗状态指示灯，按 indBrightness 缩放，
//         不受背光总开关和主背光亮度影响 —— 这是原版 s3.ino 的行为，
//         否则把背光拧到最暗时锁状态就彻底看不见了。
#define NUM_MAIN_LEDS 16
#define TOTAL_LEDS    19
static uint8_t ledRgb[TOTAL_LEDS][3];
static uint8_t brightness = 140;      // 主背光总亮度 0~255
static uint8_t currentEffect = 1;     // 灯效编号
static bool lightOn = true;           // 主背光总开关
static unsigned long lastLedFrameTime = 0;

// 锁状态
static bool numLock = false;
static bool capsLock = false;
static bool scrollLock = false;
// 播放/暂停交替显示（按 PLAY 时翻转）
static bool mediaPlaying = false;

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
#define MENU_ITEMS 11

// 宏录制
#define MAX_REC_KEYS 64
static uint16_t recKeyBuffer[MAX_REC_KEYS];
static int recKeyCount = 0;
// 录制三段式由一个状态机驱动：
//   0 = 连续输入(SEQ) → 1 = 组合键(CMB) → 2 = 结束并取消回主屏
#define REC_STAGE_SEQ  0
#define REC_STAGE_CMB  1
#define REC_STAGE_EXIT 2
static uint8_t recStage = REC_STAGE_EXIT;
static lv_obj_t* rec_lbl_count = nullptr;
static lv_obj_t* rec_lbl_keys = nullptr;
static lv_obj_t* rec_lbl_mode = nullptr;

// 菜单项（中文）
static const char* menuItemsCN[MENU_ITEMS] = {
    "1. 返回主屏",
    "2. 切换主屏风格",
    "3. 切换配置方案",
    "4. 按键回显开关",
    "5. 灯光设置",
    "6. 设置时间",
    "7. 闹钟设置",
    "8. 倒计时",
    "9. 刷新温湿度",
    "10. 温度校准",
    "11. 计数清零"
};

// 辅助变量
static bool showKeystrokes = true;
static char lastKeyPressed[8] = "-";

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
// 布局常量：6 行 × 28 步进 = 168px，从 y=50 起排到 218，底部留 22px 给按键提示
#define MENU_VISIBLE_ITEMS 6
#define MENU_ITEM_H        26
#define MENU_PITCH         28
#define MENU_LIST_TOP      50
static int menuScrollOffset = 0;

// ===========================
// 设置界面全局变量
// ===========================
// 时间设置
static int timeEditY = 2026, timeEditMo = 1, timeEditD = 1, timeEditH = 0, timeEditMi = 0;
static int timeFieldIdx = 0;

// 闹钟设置
static bool alarmEditOn = false;
static int alarmEditH = 7, alarmEditM = 0;
static int alarmFieldIdx = 0;
static bool alarmEnabled = false;
static uint8_t alarmHour = 7, alarmMinute = 0;

// 倒计时设置
static int timerEditH = 0, timerEditM = 5, timerEditS = 0;
static int timerFieldIdx = 0;
static bool timerRunning = false;
static unsigned long timerStartMs = 0;
static uint32_t timerTotalSec = 0;
static uint32_t timerRemainSec = 0;

// 温度校准
static float calTempOriginal = 62.0f;
static int calTempField = 0;

// 灯光设置
// 背光开关 / 背光亮度 / 灯效 / 状态灯亮度 全部收进这一个页面，
// 菜单里不再有"循环一下就走的"灯效项 —— 那玩意儿按错了根本不知道按到哪一档。
#define LIGHT_FIELD_COUNT 4
static uint8_t lightFieldIdx = 0;
static bool    lightOnOriginal = true;
static uint8_t lightBrightOriginal = 140;
static uint8_t lightEffectOriginal = 1;
static uint8_t lightIndLevelOriginal = 3;
static const char* lightFieldCN[LIGHT_FIELD_COUNT]  = { "背光开关", "背光亮度", "灯效", "状态灯亮度" };
static const char* lightFieldEN[LIGHT_FIELD_COUNT]  = { "BACKLIGHT", "BRIGHTNESS", "EFFECT", "INDICATOR" };
static const char* lightSwitchCN[2] = { "开", "关" };

// 设置界面对象
static lv_obj_t* set_time_lbl_date = nullptr;
static lv_obj_t* set_time_lbl_time = nullptr;
static lv_obj_t* set_time_field_labels[5] = { nullptr };

static lv_obj_t* set_alarm_lbl_time = nullptr;
static lv_obj_t* set_alarm_sw = nullptr;
static lv_obj_t* set_alarm_state_lbl = nullptr;
static lv_obj_t* set_alarm_field_labels[2] = { nullptr };

static lv_obj_t* set_timer_lbl_time = nullptr;
static lv_obj_t* set_timer_btn = nullptr;
static lv_obj_t* set_timer_field_labels[3] = { nullptr };

static lv_obj_t* set_cal_lbl_temp = nullptr;
static lv_obj_t* set_cal_lbl_offset = nullptr;
static lv_obj_t* set_cal_field_label = nullptr;

static lv_obj_t* set_light_lbl_cap = nullptr;
static lv_obj_t* set_light_lbl_en = nullptr;
static lv_obj_t* set_light_lbl_value = nullptr;
static lv_obj_t* set_light_field_labels[LIGHT_FIELD_COUNT] = { nullptr };

// HUD
typedef struct {
    bool active;
    char title[32];   // 中文 3 字节/字，32 字节够放 10 个字
    char value[48];
    // ⚠ 这里原来存的是 uint32_t color = lv_color_to32(color)，绘制时又套了一层
    // lv_color_hex()。lv_color_to32() 返回的是 0xFFRRGGBB（带 alpha 的 32 位），
    // 再喂给 lv_color_hex() 会被当成 0xRRGGBB 解读 —— 高 8 位的 0xFF 直接串到
    // 红色通道上，青色 0x00E5FF 变成 0xFF00E5 的玫红，很多颜色还会被压暗到看不清。
    // 正确做法是原样存 lv_color_t，别来回转。
    lv_color_t color;
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
static lv_obj_t* scr_settings_light = nullptr;
static lv_obj_t* scr_recording = nullptr;
static lv_obj_t* currentScreen = nullptr;

// Screen切换函数
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    if (currentScreen == target) return;
    currentScreen = target;
    lv_scr_load(target);
}

// 整屏重建时的安全换屏：**先建新屏并切过去，再删旧屏**。
//
// 之前录制页/设置页都是 `lv_obj_del(scr_xxx); scr_xxx = lv_obj_create(NULL);`，
// 而这些页面在被重建时通常正是当前活动屏。LVGL 8.4 删掉活动屏时会把
// disp->act_scr 置成 NULL（lv_obj_tree.c 里那段 act_scr_del 分支），
// 紧接着的刷新就解引用空指针 → panic → 重启。这就是"按 MR 进录制后重启"的成因。
//
// 顺序反过来就没事：切到新屏之后旧屏已经不是活动屏，删它是安全的。
static void swapScreen(lv_obj_t** slot, lv_obj_t* next) {
    if (next == nullptr) return;
    lv_obj_t* old = *slot;
    *slot = next;
    showScreen(next);
    if (old != nullptr && old != next) lv_obj_del(old);
}

// HUD浮层
static lv_obj_t* scr_hud = nullptr;

// 通知面板
static lv_obj_t* notif_bg = nullptr;
static lv_obj_t* notif_label = nullptr;
static unsigned long notifShowMs = 0;
static unsigned long notifStartMs = 0;

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

// 信息面板
static lv_obj_t* ip_bg = nullptr;
static lv_obj_t* ip_lbl_clock = nullptr;
static lv_obj_t* ip_lbl_date = nullptr;
static lv_obj_t* ip_circle_num = nullptr;
static lv_obj_t* ip_circle_caps = nullptr;
static lv_obj_t* ip_circle_scr = nullptr;
static lv_obj_t* ip_lbl_profile = nullptr;
static lv_obj_t* ip_lbl_lastkey = nullptr;
static lv_obj_t* ip_lbl_keys = nullptr;
// 信息面板的锁状态：圆点 / 文字 / 整颗胶囊，三层一起变亮灭
static lv_obj_t* ipLockChip[3] = { nullptr, nullptr, nullptr };
static lv_obj_t* ipLockDot[3]  = { nullptr, nullptr, nullptr };
static lv_obj_t* ipLockLbl[3]  = { nullptr, nullptr, nullptr };

// 击键监控
static lv_obj_t* km_bg = nullptr;
static lv_obj_t* km_lbl_lastkey = nullptr;
static lv_obj_t* km_lbl_keys = nullptr;
static lv_obj_t* km_lbl_profile = nullptr;
static lv_obj_t* km_lbl_title = nullptr;

// 律动
static lv_obj_t* rh_bg = nullptr;
static lv_obj_t* rh_lbl_keys = nullptr;
static lv_obj_t* rh_lbl_profile = nullptr;
static lv_obj_t* rh_bars[24] = { nullptr };
static uint8_t rhythmBars[24] = {0};

// 壁纸
static lv_obj_t* wp_bg = nullptr;
static lv_obj_t* wp_img = nullptr;
static lv_obj_t* wp_lbl_time = nullptr;

// 主屏通用顶部条（6 种风格共用）
static lv_obj_t* topLockDot[3] = { nullptr, nullptr, nullptr };
static uint32_t   topLockOn[3] = { 0, 0, 0 };
static lv_obj_t* topProfileLbl = nullptr;

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
// 设计体系（Design tokens）
// ===========================
// 之前是「纯黑底 + 纯白字 + 灰字」，元素之间没有任何层次，卡片底色 0x1A1A1A
// 压在纯黑上肉眼根本分不开，所以看起来像一堆浮在黑底上的字。
// 下面这套按「底 / 面 / 描边 / 文字三级 / 强调色」分层，层级一拉开就清楚了。
// 注意底色刻意不用纯黑：240x240 的小屏上纯黑配纯白对比过硬、发灰，
// 偏蓝的深灰（0x0B0F17）观感更稳，也更像成品。

#define CLR_BG        0x0B0F17   // 页面底
#define CLR_SURFACE   0x161C2B   // 卡片面
#define CLR_SURFACE_2 0x1F2739   // 抬升面 / 选中态
#define CLR_STROKE    0x2B3549   // 1px 描边

#define CLR_TEXT      0xE6EDF7   // 主文字
#define CLR_TEXT_DIM  0x8E9BB2   // 次级文字
#define CLR_TEXT_MUTE 0x5B6579   // 三级文字 / 单位

#define CLR_ACCENT    0x22D3EE   // 主强调（青）
#define CLR_ACCENT_D  0x0E7490   // 强调色暗部
#define CLR_VIOLET    0xA78BFA   // 次强调（紫）
#define CLR_AMBER     0xFBBF24   // 温度
#define CLR_GREEN     0x34D399   // 正常
#define CLR_RED       0xF87171   // 错误

// 兼容旧名字
#define CLR_BLACK  CLR_BG
#define CLR_WHITE  CLR_TEXT
#define CLR_CYAN   CLR_ACCENT
#define CLR_GRAY   CLR_TEXT_DIM
#define CLR_DARK   CLR_SURFACE
#define CLR_ACCENT_OLD CLR_ACCENT

// 三颗锁状态灯的语义色（NUM / CAPS / SCR），
// 屏幕上的胶囊色和 C3 上 16/17/18 号物理指示灯共用这一份，两边永远一致。
static const uint32_t lockLedColor[3] = { CLR_GREEN, CLR_ACCENT, CLR_AMBER };


// ===========================
// 主屏生命周期
// ===========================
// 主屏必须是一个真正的 lv_obj_t 屏幕对象。
//
// 之前的写法：6 种主屏风格都建在 lv_scr_act() 上，而 scr_main 声明成 nullptr
// 之后再也没被赋过值。于是每一处 showScreen(scr_main) 都撞在
// `if (target == nullptr) return;` 上直接返回，屏幕上什么都没发生 ——
// 表现就是"菜单里按 ESC / 选中'返回主屏' / 再按一次 MC，全都没反应"。
// 副作用还有两个：菜单载入后 lv_scr_act() 变成菜单屏，此时再切主屏风格会把
// 仪表盘控件直接糊在菜单上；息屏删掉活动屏后 currentScreen 也成了悬垂指针。
//
// 所以这里统一由 ensureMainScreen() 负责建/取主屏，风格一律往它上面建。
static bool mainContentValid = false;

// 临时诊断开关：true = 主屏只保留一块空白底色，不建任何风格内容（见 renderCurrentDisplayBase）。
// 置 false 立即恢复原来的 6 种主屏风格。
static const bool MAIN_STYLE_DISABLED = true;
static void renderCurrentDisplayBase(void);   // 定义在文件后段，这里先声明

static lv_obj_t* ensureMainScreen(void) {
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        lv_obj_set_style_border_width(scr_main, 0, LV_PART_MAIN);
        lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    }
    return scr_main;
}

// 息屏用：把主屏整个拆掉。
// lv_obj_del(scr_main) 会连带释放全部子控件，所以 gk_bg / bc_bg / ... 这些
// 全局指针必须同时置空。否则下一次 renderCurrentDisplayBase() 里那六行
// `lv_obj_del(gk_bg)` 就是在对已释放的内存调用析构，直接踩坏堆。
static void destroyMainScreen(void) {
    gk_bg = nullptr;
    gk_lbl_clock = nullptr; gk_lbl_date = nullptr; gk_lbl_temp = nullptr;
    gk_lbl_hum = nullptr; gk_lbl_keys = nullptr; gk_lbl_profile = nullptr;
    gk_led_num = nullptr; gk_led_caps = nullptr; gk_led_scr = nullptr;

    bc_bg = nullptr;
    bc_lbl_time = nullptr; bc_lbl_date = nullptr;

    ip_bg = nullptr;
    ip_lbl_clock = nullptr; ip_lbl_date = nullptr;
    ip_circle_num = nullptr; ip_circle_caps = nullptr; ip_circle_scr = nullptr;
    ip_lbl_profile = nullptr; ip_lbl_lastkey = nullptr; ip_lbl_keys = nullptr;
    for (int i = 0; i < 3; i++) { ipLockChip[i] = nullptr; ipLockDot[i] = nullptr; ipLockLbl[i] = nullptr; }

    km_bg = nullptr;
    km_lbl_title = nullptr; km_lbl_lastkey = nullptr;
    km_lbl_keys = nullptr; km_lbl_profile = nullptr;

    rh_bg = nullptr;
    rh_lbl_keys = nullptr; rh_lbl_profile = nullptr;
    for (int i = 0; i < 24; i++) rh_bars[i] = nullptr;

    wp_bg = nullptr;
    wp_img = nullptr; wp_lbl_time = nullptr;

    for (int i = 0; i < 3; i++) topLockDot[i] = nullptr;
    topProfileLbl = nullptr;

    if (scr_main != nullptr) {
        // **删活动屏之前必须先切走**。
        //
        // LVGL 8.4 的 lv_obj_del() 在删掉的正好是活动屏时，会把
        // disp->act_scr 直接置成 NULL（lv_obj_tree.c:73 那句
        // `if(act_scr_del) disp->act_scr = NULL;`）。之后 lv_scr_act() 就一直
        // 返回 NULL，任何解引用它的操作都是野指针 panic。
        //
        // 这条路径不是理论风险：SLEEP_TIMEOUT_MS = 60000，息屏时
        // destroyMainScreen() 删的正是当时唯一那块活动屏 —— "跑着一分多钟
        // 之后自己重启"的时间点跟它对得上。旁边 swapScreen() 早就为了同一个
        // 坑把顺序改成了"先切后删"，这里漏了。
        //
        // 先造一块空白屏顶上，主屏就不再是活动屏，删它就安全了。
        if (lv_scr_act() == scr_main) {
            lv_obj_t* blank = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(blank, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
            lv_obj_clear_flag(blank, LV_OBJ_FLAG_SCROLLABLE);
            currentScreen = blank;
            lv_scr_load(blank);
        }
        lv_obj_del(scr_main);
        scr_main = nullptr;
    }
    // currentScreen 指着刚被释放的屏幕，同样作废
    currentScreen = nullptr;
    mainContentValid = false;
}

// 回到主屏的统一出口：保证主屏和主屏内容都还在，然后真正切过去。
static void gotoMainScreen(void) {
    currentSysMode = SYS_MODE_NORMAL;
    menuSel = 0;
    menuScrollOffset = 0;
    recStage = REC_STAGE_EXIT;   // 退出演录状态机，下次 MR 重新从 SEQ 开始
    lightFieldIdx = 0;

    // 息屏把主屏拆过的话，这里要把内容一并重建，否则切过去是一片黑
    if (!mainContentValid) renderCurrentDisplayBase();
    showScreen(ensureMainScreen());
}

// ===========================
// 前向声明
// ===========================
static void handleCommand(const String& cmd);
static void scanKeyboardMatrix(void);
static void recoverI2CBus(void);
void bootDetailTick(void);
static void renderCurrentDisplayBase(void);
static void updateDynamicElements(void);
static void triggerHud(const char* title, const char* value, lv_color_t color);
static void pushNotification(AlertType type, const String& text);
static void drawNotifPanel(void);
static void showScreen(lv_obj_t* target);
static void swapScreen(lv_obj_t** slot, lv_obj_t* next);
static lv_obj_t* ensureMainScreen(void);
static void gotoMainScreen(void);
static void mkCard(lv_obj_t* o, uint32_t bg, uint8_t radius);
static void mkChip(lv_obj_t* o, uint32_t bg, uint32_t fg, uint8_t radius);
static void mkLabel(lv_obj_t* l, const lv_font_t* f, uint32_t color);
static void setText(lv_obj_t* lbl, const char* txt);
static void handleMenuSelect(void);
static const char* getKeyName(uint16_t code);
static void build_settings_time(void);
static void build_settings_alarm(void);
static void build_settings_timer(void);
static void build_settings_caltemp(void);
static void build_settings_light(void);
static void moveSettingField(int dir);
static void adjustSettingField(int delta);
static void saveSettingScreen(void);
static void cancelSettingScreen(void);
static void update_setting_time_display(void);
static void update_setting_alarm_display(void);
static void update_setting_timer_display(void);
static void update_setting_caltemp_display(void);
static void update_setting_light_display(void);
static void update_recording_display(void);
static void build_recording(void);
static void finishMacroRecording(const String& targetKey);
static void enterRecording(void);
static void advanceRecordingStage(void);
static void cancelRecording(void);
static void cycleDisplayStyle(void);

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
// 样式工具
// ===========================
static void init_styles(void) {
    if (styles_inited) return;
    styles_inited = true;

    // 注意：lv_style_set_* 没有 LV_PART 参数（那是 lv_obj_set_style_* 才有的）
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(CLR_BG));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_text_color(&style_bg, lv_color_hex(CLR_TEXT));

    lv_style_init(&style_text_white);
    lv_style_set_text_color(&style_text_white, lv_color_hex(CLR_TEXT));

    lv_style_init(&style_text_cyan);
    lv_style_set_text_color(&style_text_cyan, lv_color_hex(CLR_ACCENT));

    lv_style_init(&style_text_gray);
    lv_style_set_text_color(&style_text_gray, lv_color_hex(CLR_TEXT_DIM));

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(CLR_SURFACE));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, lv_color_hex(CLR_STROKE));
    lv_style_set_radius(&style_card, 10);
    lv_style_set_pad_all(&style_card, 0);
}

// 卡片：底色 + 圆角 + 1px 描边，这一层是"好看"和"丑"的分水岭
static void mkCard(lv_obj_t* o, uint32_t bg, uint8_t radius) {
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// 小圆片（方案名 / 模式标签这类胶囊）
static void mkChip(lv_obj_t* o, uint32_t bg, uint32_t fg, uint8_t radius) {
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void mkLabel(lv_obj_t* l, const lv_font_t* f, uint32_t color) {
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
}

// **只在内容真的变了时才 set_text**
// updateDynamicElements() 是每轮 loop 都跑的，LVGL 的 lv_label_set_text 无条件
// invalidate + 重排版，文本没变也照刷 —— 结果屏幕永远 dirty，SPI 一直在推满屏。
// 时钟/温湿度/计数这些一秒才变一次的值，套上这个函数后静态画面能完全停止刷新。
static void setText(lv_obj_t* lbl, const char* txt) {
    if (lbl == nullptr) return;
    const char* cur = lv_label_get_text(lbl);
    if (cur != nullptr && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

// ===========================
// 变化检测写入（关键性能修复）
// ===========================
// LVGL 8.4 **没有**"值没变就跳过"的短路。lv_obj_set_local_style_prop() 直接
// 调 lv_obj_refresh_style()，而后者第一件事就是无条件 lv_obj_invalidate(obj)
// （lvgl/src/core/lv_obj_style.c:167-173）。lv_obj_set_size / lv_obj_set_pos 同理，
// 除了 invalidate 还会 lv_obj_mark_layout_as_dirty()。
//
// 原来的 updateDynamicElements() 是每轮 loop() 都跑的，里面却无条件地
// 每轮重设：顶部条 3 个圆点的底色、信息面板再加 9 个（三层锁灯各 3 个）、
// 节奏页再加 72 个（24 根柱子的尺寸+位置+底色）。
//
// 后果就是屏幕**永远处于 dirty 状态**：无效区列表被撑满（超过 LV_INV_BUF_SIZE
// 还会退化成整屏重绘），每 33ms 一次全屏重绘 + 6 次 SPI 满屏 flush。SPI 是
// 阻塞式的，loop() 被 LVGL 吃满 → 排在它前面的键盘扫描抢不到时间。
// 表现就是「切到要素最多的信息面板就整机卡死、按键全部失灵」。
//
// 所以下面这几个 setter 都先读回当前值比对，不同才写。读回走
// lv_obj_get_style_*（只在对象自己的样式数组里找，最多查 1~2 项），
// 比一次 invalidate + 满屏重绘便宜几个数量级。
static void setBgColor(lv_obj_t* o, uint32_t hex) {
    if (o == nullptr) return;
    lv_color_t want = lv_color_hex(hex);
    if (lv_obj_get_style_bg_color(o, LV_PART_MAIN).full == want.full) return;
    lv_obj_set_style_bg_color(o, want, LV_PART_MAIN);
}

static void setTextColor(lv_obj_t* o, uint32_t hex) {
    if (o == nullptr) return;
    lv_color_t want = lv_color_hex(hex);
    if (lv_obj_get_style_text_color(o, LV_PART_MAIN).full == want.full) return;
    lv_obj_set_style_text_color(o, want, LV_PART_MAIN);
}

// 尺寸+位置一起比。只在真的变了才写，避免节奏页每轮 24 根柱子 × 2 次几何写。
static void setSizePos(lv_obj_t* o, lv_coord_t w, lv_coord_t h, lv_coord_t x, lv_coord_t y) {
    if (o == nullptr) return;
    if (lv_obj_get_width(o) == w && lv_obj_get_height(o) == h &&
        lv_obj_get_x(o) == x && lv_obj_get_y(o) == y) return;
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
}

// ===========================
// 主屏通用：顶部状态条
// ===========================
// 三颗锁状态灯 + 方案名，6 种风格共用。灯点用 topLockDot[] 存起来，
// updateDynamicElements() 统一驱动，避免每种风格各写一份。
static void dashTopBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const char* names[3] = { "NUM", "CAP", "SCR" };
    const uint32_t onColors[3] = { lockLedColor[0], lockLedColor[1], lockLedColor[2] };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* d = lv_obj_create(bar);
        lv_obj_set_size(d, 7, 7);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 14 + i * 44, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        topLockDot[i] = d;
        topLockOn[i] = onColors[i];

        lv_obj_t* t = lv_label_create(bar);
        mkLabel(t, &lv_font_montserrat_10, CLR_TEXT_MUTE);
        lv_label_set_text(t, names[i]);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 25 + i * 44, 0);
    }

    topProfileLbl = lv_label_create(bar);
    mkLabel(topProfileLbl, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(topProfileLbl, profileNamesCN[currentProfile]);
    lv_obj_align(topProfileLbl, LV_ALIGN_RIGHT_MID, -12, 0);
}

// 小统计卡：标题 + 数值
static lv_obj_t* statCard(lv_obj_t* parent, int cx, const char* cap,
                          const lv_font_t* vf, uint32_t vcolor) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 72, 62);
    lv_obj_align(c, LV_ALIGN_CENTER, cx, 40);
    mkCard(c, CLR_SURFACE, 10);

    lv_obj_t* t = lv_label_create(c);
    mkLabel(t, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(t, cap);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* v = lv_label_create(c);
    mkLabel(v, vf, vcolor);
    lv_label_set_text(v, "--");
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -8);
    return v;
}

// ===========================
// 构建：极客仪表盘
// ===========================
static void build_style_geek(void) {
    if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }

    gk_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(gk_bg, 240, 240);
    lv_obj_set_pos(gk_bg, 0, 0);
    lv_obj_set_style_bg_color(gk_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(gk_bg, 0, LV_PART_MAIN);

    dashTopBar(gk_bg);

    // 主时钟
    gk_lbl_clock = lv_label_create(gk_bg);
    mkLabel(gk_lbl_clock, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(gk_lbl_clock, "--:--");
    lv_obj_align(gk_lbl_clock, LV_ALIGN_TOP_MID, 0, 44);

    // 日期
    gk_lbl_date = lv_label_create(gk_bg);
    mkLabel(gk_lbl_date, &lv_font_montserrat_14, CLR_TEXT_DIM);
    lv_label_set_text(gk_lbl_date, "----/--/-- --");
    lv_obj_align(gk_lbl_date, LV_ALIGN_TOP_MID, 0, 104);

    // 时钟下方一条强调线，视觉上把主区和数据区分开
    lv_obj_t* line = lv_obj_create(gk_bg);
    lv_obj_set_size(line, 40, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    // 三张统计卡
    gk_lbl_temp = statCard(gk_bg, -76, "TEMP",  &lv_font_montserrat_20, CLR_AMBER);
    gk_lbl_hum  = statCard(gk_bg,   0, "HUMI",  &lv_font_montserrat_20, CLR_ACCENT);
    gk_lbl_keys = statCard(gk_bg,  76, "KEYS",  &lv_font_montserrat_20, CLR_VIOLET);
}

// ===========================
// 构建：大时钟
// ===========================
static void build_style_bigclock(void) {
    if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }

    bc_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(bc_bg, 240, 240);
    lv_obj_set_pos(bc_bg, 0, 0);
    lv_obj_set_style_bg_color(bc_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(bc_bg, 0, LV_PART_MAIN);

    dashTopBar(bc_bg);

    bc_lbl_time = lv_label_create(bc_bg);
    mkLabel(bc_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(bc_lbl_time, "--:--");
    lv_obj_align(bc_lbl_time, LV_ALIGN_CENTER, 0, -18);

    // 强调线
    lv_obj_t* line = lv_obj_create(bc_bg);
    lv_obj_set_size(line, 56, 3);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    bc_lbl_date = lv_label_create(bc_bg);
    mkLabel(bc_lbl_date, &lv_font_montserrat_16, CLR_TEXT_DIM);
    lv_label_set_text(bc_lbl_date, "----/--/-- --");
    lv_obj_align(bc_lbl_date, LV_ALIGN_CENTER, 0, 44);

    // 底部方案胶囊
    lv_obj_t* chip = lv_obj_create(bc_bg);
    lv_obj_set_size(chip, 116, 24);
    lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -22);
    mkChip(chip, CLR_SURFACE, CLR_ACCENT, 12);
    lv_obj_t* cl = lv_label_create(chip);
    mkLabel(cl, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(cl, profileNamesCN[currentProfile]);
    lv_obj_center(cl);
}

// ===========================
// 构建：信息面板
// ===========================
static void build_style_info_panel(void) {
    if (ip_bg) { lv_obj_del(ip_bg); ip_bg = nullptr; }

    ip_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(ip_bg, 240, 240);
    lv_obj_set_pos(ip_bg, 0, 0);
    lv_obj_set_style_bg_color(ip_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(ip_bg, 0, LV_PART_MAIN);

    dashTopBar(ip_bg);

    // 时钟 + 日期主卡
    lv_obj_t* card = lv_obj_create(ip_bg);
    lv_obj_set_size(card, 212, 84);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);
    mkCard(card, CLR_SURFACE, 12);

    ip_lbl_clock = lv_label_create(card);
    mkLabel(ip_lbl_clock, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(ip_lbl_clock, "--:--");
    lv_obj_align(ip_lbl_clock, LV_ALIGN_CENTER, 0, -10);

    ip_lbl_date = lv_label_create(card);
    mkLabel(ip_lbl_date, &lv_font_montserrat_14, CLR_TEXT_DIM);
    lv_label_set_text(ip_lbl_date, "----/--/--");
    lv_obj_align(ip_lbl_date, LV_ALIGN_CENTER, 0, 26);

    // 三个锁状态胶囊
    // 这三颗对应键盘上 C3 的 NUM / CAPS / SCR 指示灯，用户明确要求"更明显"：
    // 圆点从 8px 放大到 14px、字号 12→14、亮时整颗胶囊底色也跟着抬起来，
    // 灭时圆点压到描边色、文字退到三级灰 —— 亮/灭在余光里也能分辨。
    const char* lockNames[3] = { "NUM", "CAPS", "SCR" };
    const int lockX[3] = { -70, 0, 70 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = lv_obj_create(ip_bg);
        lv_obj_set_size(c, 66, 36);
        lv_obj_align(c, LV_ALIGN_TOP_MID, lockX[i], 126);
        mkChip(c, CLR_SURFACE, CLR_TEXT_MUTE, 10);
        ipLockChip[i] = c;

        lv_obj_t* d = lv_obj_create(c);
        lv_obj_set_size(d, 14, 14);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 9, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        ipLockDot[i] = d;

        lv_obj_t* t = lv_label_create(c);
        mkLabel(t, &lv_font_montserrat_14, CLR_TEXT_MUTE);
        lv_label_set_text(t, lockNames[i]);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, -8, 0);
        ipLockLbl[i] = t;
    }
    ip_circle_num = ipLockDot[0]; ip_circle_caps = ipLockDot[1]; ip_circle_scr = ipLockDot[2];

    // 底部：最近按键 + 击键数
    lv_obj_t* card2 = lv_obj_create(ip_bg);
    lv_obj_set_size(card2, 212, 54);
    lv_obj_align(card2, LV_ALIGN_BOTTOM_MID, 0, -8);
    mkCard(card2, CLR_SURFACE, 12);

    lv_obj_t* cap = lv_label_create(card2);
    mkLabel(cap, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(cap, "LAST KEY");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 14, 8);

    ip_lbl_lastkey = lv_label_create(card2);
    mkLabel(ip_lbl_lastkey, &lv_font_montserrat_20, CLR_ACCENT);
    lv_label_set_text(ip_lbl_lastkey, "-");
    lv_obj_align(ip_lbl_lastkey, LV_ALIGN_BOTTOM_LEFT, 14, -6);

    lv_obj_t* cap2 = lv_label_create(card2);
    mkLabel(cap2, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(cap2, "KEYS");
    lv_obj_align(cap2, LV_ALIGN_TOP_RIGHT, -14, 8);

    ip_lbl_keys = lv_label_create(card2);
    mkLabel(ip_lbl_keys, &lv_font_montserrat_20, CLR_VIOLET);
    lv_label_set_text(ip_lbl_keys, "0");
    lv_obj_align(ip_lbl_keys, LV_ALIGN_BOTTOM_RIGHT, -14, -6);
}

// ===========================
// 构建：击键监控
// ===========================
static void build_style_keymon(void) {
    if (km_bg) { lv_obj_del(km_bg); km_bg = nullptr; }

    km_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(km_bg, 240, 240);
    lv_obj_set_pos(km_bg, 0, 0);
    lv_obj_set_style_bg_color(km_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(km_bg, 0, LV_PART_MAIN);

    dashTopBar(km_bg);

    km_lbl_title = lv_label_create(km_bg);
    mkLabel(km_lbl_title, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(km_lbl_title, "最近按键");
    lv_obj_align(km_lbl_title, LV_ALIGN_TOP_MID, 0, 44);

    // 最近按键：走 getKeyName()，显示 Space / Enter / A 而不是 20 / 1D / 04
    km_lbl_lastkey = lv_label_create(km_bg);
    mkLabel(km_lbl_lastkey, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(km_lbl_lastkey, "-");
    lv_obj_set_width(km_lbl_lastkey, 220);
    lv_label_set_long_mode(km_lbl_lastkey, LV_LABEL_LONG_DOT);
    lv_obj_align(km_lbl_lastkey, LV_ALIGN_TOP_MID, 0, 66);

    // 击键统计卡
    lv_obj_t* card = lv_obj_create(km_bg);
    lv_obj_set_size(card, 212, 58);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -34);
    mkCard(card, CLR_SURFACE, 12);

    km_lbl_keys = lv_label_create(card);
    mkLabel(km_lbl_keys, &lv_font_montserrat_28, CLR_VIOLET);
    lv_label_set_text(km_lbl_keys, "0");
    lv_obj_align(km_lbl_keys, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* unit = lv_label_create(card);
    mkLabel(unit, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(unit, "TOTAL KEYS");
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -16, 0);

    km_lbl_profile = lv_label_create(km_bg);
    mkLabel(km_lbl_profile, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(km_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_align(km_lbl_profile, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ===========================
// 构建：律动
// ===========================
static void build_style_rhythm(void) {
    if (rh_bg) { lv_obj_del(rh_bg); rh_bg = nullptr; }

    rh_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(rh_bg, 240, 240);
    lv_obj_set_pos(rh_bg, 0, 0);
    lv_obj_set_style_bg_color(rh_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(rh_bg, 0, LV_PART_MAIN);

    dashTopBar(rh_bg);

    // 频谱容器，柱子从底往上长
    lv_obj_t* stage = lv_obj_create(rh_bg);
    lv_obj_set_size(stage, 236, 110);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(stage, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 24; i++) {
        rh_bars[i] = lv_obj_create(stage);
        lv_obj_set_size(rh_bars[i], 6, 4);
        lv_obj_set_pos(rh_bars[i], 2 + i * 10, 106);
        lv_obj_set_style_bg_color(rh_bars[i], lv_color_hex(CLR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rh_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(rh_bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(rh_bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(rh_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        rhythmBars[i] = 0;
    }

    // 基线
    lv_obj_t* base = lv_obj_create(rh_bg);
    lv_obj_set_size(base, 224, 1);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(base, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cap = lv_label_create(rh_bg);
    mkLabel(cap, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(cap, "击键律动");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 156);

    // 击键数卡
    lv_obj_t* card = lv_obj_create(rh_bg);
    lv_obj_set_size(card, 212, 44);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);
    mkCard(card, CLR_SURFACE, 12);

    rh_lbl_keys = lv_label_create(card);
    mkLabel(rh_lbl_keys, &lv_font_montserrat_20, CLR_VIOLET);
    lv_label_set_text(rh_lbl_keys, "0");
    lv_obj_align(rh_lbl_keys, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* unit = lv_label_create(card);
    mkLabel(unit, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(unit, "TOTAL KEYS");
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -16, 0);
}

// ===========================
// 构建：壁纸模式
// ===========================
static void build_style_wallpaper(void) {
    if (wp_bg) { lv_obj_del(wp_bg); wp_bg = nullptr; }

    wp_bg = lv_obj_create(ensureMainScreen());
    lv_obj_set_size(wp_bg, 240, 240);
    lv_obj_set_pos(wp_bg, 0, 0);
    lv_obj_set_style_bg_color(wp_bg, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(wp_bg, 0, LV_PART_MAIN);

    // 壁纸铺满
    wp_img = lv_img_create(wp_bg);
    lv_obj_set_size(wp_img, 240, 240);
    lv_obj_set_pos(wp_img, 0, 0);
    lv_obj_set_style_bg_color(wp_img, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_width(wp_img, 0, LV_PART_MAIN);
    // 没上传壁纸时给一块深色底，不至于全黑
    lv_img_set_src(wp_img, NULL);

    // 压一层半透明黑，保证上面的字在任意壁纸上都读得出来
    lv_obj_t* scrim = lv_obj_create(wp_bg);
    lv_obj_set_size(scrim, 240, 240);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(scrim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    // 时钟叠加在正中
    lv_obj_t* plate = lv_obj_create(wp_bg);
    lv_obj_set_size(plate, 168, 64);
    lv_obj_align(plate, LV_ALIGN_CENTER, 0, 0);
    mkCard(plate, CLR_SURFACE_2, 12);
    lv_obj_set_style_bg_opa(plate, LV_OPA_80, LV_PART_MAIN);

    wp_lbl_time = lv_label_create(plate);
    mkLabel(wp_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(wp_lbl_time, "--:--");
    lv_obj_center(wp_lbl_time);
}

// ===========================
// 菜单选择处理
// ===========================
// 切换主屏风格的统一入口
// LOGO 键（主屏短按）和菜单第 2 项都走这里，保证"HUD 文案 + 持久化 + 重建 + 切屏"
// 这一套永远是一份，不会出现两个入口行为不一致。
static void cycleDisplayStyle(void) {
    currentDispMode = (currentDispMode + 1) % TOTAL_DISP_MODES;
    preferences.putUChar("disp_mode", currentDispMode);
    renderCurrentDisplayBase();
    showScreen(ensureMainScreen());
    triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_ACCENT));
}

static void handleMenuSelect(void) {
    switch (menuSel) {
        case 0:  // 返回主屏
            gotoMainScreen();
            break;
        case 1:  // 切换主屏风格 - 循环切换
            cycleDisplayStyle();
            break;
        case 2:  // 切换配置方案 - 循环切换
            switchProfile((currentProfile + 1) % TOTAL_PROFILES);
            break;
        case 3:  // 按键回显开关
            showKeystrokes = !showKeystrokes;
            triggerHud("按键回显", showKeystrokes ? "开启" : "关闭",
                lv_color_hex(showKeystrokes ? CLR_GREEN : CLR_RED));
            break;
        case 4:  // 灯光设置（背光开关 / 亮度 / 灯效 / 状态灯亮度 都在里面）
            build_settings_light();
            break;
        case 5:  // 设置时间
            build_settings_time();
            break;
        case 6:  // 闹钟设置
            build_settings_alarm();
            break;
        case 7:  // 倒计时
            build_settings_timer();
            break;
        case 8:  // 刷新温湿度
            if (shtAvailable) {
                sht31_update();
                triggerHud("温湿度", "已刷新", lv_color_hex(CLR_GREEN));
            } else {
                triggerHud("温湿度", "未找到", lv_color_hex(CLR_RED));
            }
            break;
        case 9:  // 温度校准
            build_settings_caltemp();
            break;
        case 10: // 计数清零
            totalKeyCount = 0;
            preferences.putUInt("keyCount", 0);
            triggerHud("击键计数", "已清零", lv_color_hex(CLR_ACCENT));
            break;
    }
}

// ===========================
// 构建：菜单（12 项，真滚动）
// ===========================
// 之前滚动是假的：yPos 恒等于 i*32，menuScrollOffset 只用来改透明度。
// 滚到第二页时，可见窗口是第 1~6 项、y 落在 32~192，而容器只有 192 高，
// 第 4/5/6 项直接被容器裁掉 —— 所以看起来"一翻页就全黑了"。
// 现在 yPos 跟着 menuScrollOffset 走，是真的把列表窗口往上滚。
static void build_menu(void) {
    // 计算滚动偏移（保证选中项始终在可见窗口内，且不越界）
    if (menuSel < menuScrollOffset) {
        menuScrollOffset = menuSel;
    } else if (menuSel >= menuScrollOffset + MENU_VISIBLE_ITEMS) {
        menuScrollOffset = menuSel - MENU_VISIBLE_ITEMS + 1;
    }
    int maxOffset = MENU_ITEMS - MENU_VISIBLE_ITEMS;
    if (maxOffset < 0) maxOffset = 0;
    if (menuScrollOffset > maxOffset) menuScrollOffset = maxOffset;
    if (menuScrollOffset < 0) menuScrollOffset = 0;

    if (scr_menu == nullptr) {
        init_styles();
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr_menu, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr_menu, 0, LV_PART_MAIN);

        // ---- 顶部标题栏 ----
        menu_title = lv_label_create(scr_menu);
        mkLabel(menu_title, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(menu_title, "系统菜单");
        lv_obj_align(menu_title, LV_ALIGN_TOP_LEFT, 16, 12);

        // 标题左侧的强调色竖条
        lv_obj_t* tbar = lv_obj_create(scr_menu);
        lv_obj_set_size(tbar, 3, 14);
        lv_obj_align(tbar, LV_ALIGN_TOP_LEFT, 8, 13);
        lv_obj_set_style_bg_color(tbar, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(tbar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(tbar, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

        // 右上角页码胶囊
        lv_obj_t* pos_chip = lv_obj_create(scr_menu);
        lv_obj_set_size(pos_chip, 44, 20);
        lv_obj_align(pos_chip, LV_ALIGN_TOP_RIGHT, -16, 10);
        mkChip(pos_chip, CLR_SURFACE_2, CLR_TEXT_DIM, 10);

        menu_position = lv_label_create(pos_chip);
        mkLabel(menu_position, &lv_font_montserrat_12, CLR_TEXT_DIM);
        lv_label_set_text(menu_position, "1/12");
        lv_obj_center(menu_position);

        // ---- 列表容器（只做裁剪，不画底）----
        menu_cont = lv_obj_create(scr_menu);
        lv_obj_set_size(menu_cont, 216, MENU_VISIBLE_ITEMS * MENU_PITCH);
        lv_obj_align(menu_cont, LV_ALIGN_TOP_MID, 0, MENU_LIST_TOP);
        lv_obj_set_style_bg_opa(menu_cont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(menu_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(menu_cont, 0, LV_PART_MAIN);

        // ---- 12 个菜单项（只建一次，之后只改位置和配色）----
        for (int i = 0; i < MENU_ITEMS; i++) {
            lv_obj_t* btn = lv_obj_create(menu_cont);
            lv_obj_set_size(btn, 212, MENU_ITEM_H);
            mkCard(btn, CLR_SURFACE, 7);

            // 选中态左侧强调条
            lv_obj_t* sel = lv_obj_create(btn);
            lv_obj_set_size(sel, 3, MENU_ITEM_H - 8);
            lv_obj_set_pos(sel, 0, 4);
            lv_obj_set_style_bg_color(sel, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sel, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(sel, 2, LV_PART_MAIN);
            lv_obj_set_style_border_width(sel, 0, LV_PART_MAIN);
            lv_obj_clear_flag(sel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* lbl = lv_label_create(btn);
            mkLabel(lbl, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(lbl, menuItemsCN[i]);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);

            // 右侧序号
            lv_obj_t* num = lv_label_create(btn);
            mkLabel(num, &lv_font_montserrat_12, CLR_TEXT_MUTE);
            static char numBuf[MENU_ITEMS][4];
            snprintf(numBuf[i], sizeof(numBuf[i]), "%02d", i + 1);
            lv_label_set_text(num, numBuf[i]);
            lv_obj_align(num, LV_ALIGN_RIGHT_MID, -12, 0);

            menu_items[i] = btn;
        }
    }

    // ---- 更新页码 ----
    if (menu_position) {
        static char posBuf[16];
        snprintf(posBuf, sizeof(posBuf), "%d/%d", menuSel + 1, MENU_ITEMS);
        setText(menu_position, posBuf);
    }

    // ---- 更新每项的位置（真滚动）与配色 ----
    for (int i = 0; i < MENU_ITEMS; i++) {
        lv_obj_t* btn = menu_items[i];
        if (btn == nullptr) continue;

        int slot = i - menuScrollOffset;                 // 在窗口里的第几行
        bool isVisible = (slot >= 0 && slot < MENU_VISIBLE_ITEMS);
        bool isSelected = (i == menuSel);

        if (isVisible) lv_obj_set_pos(btn, 0, slot * MENU_PITCH);
        // 窗口外的项移出容器并隐藏（不删，重建成本高）
        lv_obj_set_style_opa(btn, isVisible ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);

        lv_obj_set_style_bg_color(btn,
            lv_color_hex(isSelected ? CLR_SURFACE_2 : CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn,
            lv_color_hex(isSelected ? CLR_ACCENT_D : CLR_STROKE), LV_PART_MAIN);

        // 左侧强调条：只有选中项点亮
        lv_obj_t* sel = lv_obj_get_child(btn, 0);
        if (sel) {
            lv_obj_set_style_bg_opa(sel,
                isSelected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        }
        // 文本 / 序号
        lv_obj_t* lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(isSelected ? CLR_TEXT : CLR_TEXT_DIM), LV_PART_MAIN);
        }
        lv_obj_t* num = lv_obj_get_child(btn, 2);
        if (num) {
            lv_obj_set_style_text_color(num,
                lv_color_hex(isSelected ? CLR_ACCENT : CLR_TEXT_MUTE), LV_PART_MAIN);
        }
    }

    // ---- 底部按键提示 ----
    static lv_obj_t* hint = nullptr;
    if (hint == nullptr) {
        hint = lv_label_create(scr_menu);
        mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(hint, "上下选择 · 回车确认 · ESC 返回");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    showScreen(scr_menu);
}

// ===========================
// HUD 浮层（操作反馈提示条）
// ===========================
// 这张卡是"操作有没有生效"的唯一反馈，所以按可读性优先设计：
//   纯黑底 + 2px 语义色描边 —— 240x240 上对比最强的组合，不管底下是什么画面都压得住；
//   标题（三级色）与数值（主文字色）分层，数值用中文字体最大号；
//   标题和数值都水平居中，余光扫一眼就能读到。
// 之前的版本是暗面卡片 + 左侧竖条，底色和主屏卡片几乎同色，
// 提示"看起来像没有"，用户根本不知道刚才那下按没按上。
#define HUD_H 64
static void build_hud(void) {
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }

    scr_hud = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_hud, 216, HUD_H);
    lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, 30);
    mkCard(scr_hud, 0x000000, 12);
    lv_obj_set_style_border_width(scr_hud, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_hud, hud.color, LV_PART_MAIN);
    lv_obj_move_foreground(scr_hud);

    // 左侧语义色竖条：颜色跟着提示语义走，一眼能分辨成功/警告/错误
    lv_obj_t* bar = lv_obj_create(scr_hud);
    lv_obj_set_size(bar, 5, HUD_H - 12);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(bar, hud.color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 标题（小、三级色、居中）
    lv_obj_t* title = lv_label_create(scr_hud);
    mkLabel(title, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(title, hud.title);
    lv_obj_set_width(title, 184);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 9);

    // 数值（大、主文字色、居中）—— 卡片主体
    lv_obj_t* value = lv_label_create(scr_hud);
    mkLabel(value, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(value, hud.value);
    lv_obj_set_width(value, 184);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void triggerHud(const char* title, const char* value, lv_color_t color) {
    hud.active = true;
    strncpy(hud.title, title, sizeof(hud.title) - 1);
    hud.title[sizeof(hud.title) - 1] = '\0';
    strncpy(hud.value, value, sizeof(hud.value) - 1);
    hud.value[sizeof(hud.value) - 1] = '\0';
    hud.color = color;          // 原样存 lv_color_t，别再 lv_color_to32 来回转
    hud.startMs = millis();
    hud.showMs = 1500;

    build_hud();
}

// ===========================
// 通知系统实现
// ===========================
static void pushNotification(AlertType type, const String& text) {
    if (type == ALERT_NONE) return;
    if (notifCount >= MAX_NOTIFS) {
        // 移除最老的通知
        for (int i = 0; i < notifCount - 1; i++) {
            notifQueue[i] = notifQueue[i + 1];
            strncpy(notifTexts[i], notifTexts[i + 1], 127);
        }
        notifCount--;
    }

    notifQueue[notifCount] = type;
    strncpy(notifTexts[notifCount], text.c_str(), 127);
    notifTexts[notifCount][127] = '\0';
    notifCount++;

    drawNotifPanel();
}

static void drawNotifPanel(void) {
    if (notifCount == 0) {
        if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }
        return;
    }

    // 删除旧的通知面板
    if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }

    // 语义色：错误=红 / 正常=绿 / 警告=琥珀 / 普通=青
    AlertType latestType = notifQueue[notifCount - 1];
    uint32_t accent;
    switch (latestType) {
        case ALERT_RED:    accent = CLR_RED;   break;
        case ALERT_GREEN:  accent = CLR_GREEN; break;
        case ALERT_YELLOW: accent = CLR_AMBER; break;
        default:           accent = CLR_ACCENT; break;
    }

    // 和 HUD 同一套卡片语言：纯黑底 + 2px 语义描边 + 左侧竖条 + 状态圆点
    scr_notif = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_notif, 232, 58);
    lv_obj_align(scr_notif, LV_ALIGN_TOP_MID, 0, 6);
    mkCard(scr_notif, 0x000000, 12);
    lv_obj_set_style_border_width(scr_notif, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_notif, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_move_foreground(scr_notif);

    lv_obj_t* bar = lv_obj_create(scr_notif);
    lv_obj_set_size(bar, 5, 46);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 状态圆点 + 文本（主机下发的通知可能是中文）
    lv_obj_t* dot = lv_obj_create(scr_notif);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 20, 14);
    lv_obj_set_style_bg_color(dot, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    notif_label = lv_label_create(scr_notif);
    mkLabel(notif_label, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(notif_label, notifTexts[notifCount - 1]);
    lv_obj_set_width(notif_label, 186);
    lv_label_set_long_mode(notif_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(notif_label, LV_ALIGN_TOP_LEFT, 38, 12);

    // 通知显示时间（3秒）
    notifStartMs = millis();
    notifShowMs = 3000;
}

static void clearNotifications(void) {
    notifCount = 0;
    if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }
}

// ===========================
// 设置界面显示更新函数
// ===========================
static void update_setting_time_display(void) {
    static char dbuf[32], tbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", timeEditY, timeEditMo, timeEditD);
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", timeEditH, timeEditMi);
    setText(set_time_lbl_date, dbuf);
    setText(set_time_lbl_time, tbuf);
    for (int i = 0; i < 5; i++) markField(set_time_field_labels[i], i == timeFieldIdx);
}

static void update_setting_alarm_display(void) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", alarmEditH, alarmEditM);
    setText(set_alarm_lbl_time, buf);
    if (set_alarm_sw) {
        if (alarmEnabled) lv_obj_add_state(set_alarm_sw, LV_STATE_CHECKED);
        else               lv_obj_clear_state(set_alarm_sw, LV_STATE_CHECKED);
    }
    setText(set_alarm_state_lbl, alarmEnabled ? "已开启" : "已关闭");
    if (set_alarm_state_lbl) {
        lv_obj_set_style_text_color(set_alarm_state_lbl,
            lv_color_hex(alarmEnabled ? CLR_GREEN : CLR_TEXT_MUTE), LV_PART_MAIN);
    }
    for (int i = 0; i < 2; i++) markField(set_alarm_field_labels[i], i == alarmFieldIdx);
}

static void update_setting_timer_display(void) {
    static char buf[32];
    uint32_t h = timerRemainSec / 3600;
    uint32_t m = (timerRemainSec % 3600) / 60;
    uint32_t s = timerRemainSec % 60;
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
    setText(set_timer_lbl_time, buf);
    if (set_timer_btn) {
        lv_obj_set_style_bg_color(set_timer_btn,
            lv_color_hex(timerRunning ? CLR_RED : CLR_ACCENT), LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(set_timer_btn, 0);
        setText(lbl, timerRunning ? "停止" : "开始");
    }
    for (int i = 0; i < 3; i++) markField(set_timer_field_labels[i], i == timerFieldIdx);
}

static void update_setting_caltemp_display(void) {
    static char tbuf[32], obuf[32];
    snprintf(tbuf, sizeof(tbuf), "%.1f °C", shtTemp);
    snprintf(obuf, sizeof(obuf), "偏移 %+.1f", shtTempOffset);
    setText(set_cal_lbl_temp, tbuf);
    setText(set_cal_lbl_offset, obuf);
}

// ===========================
// 灯光设置：显示函数
// ===========================
// 四个字段共用一块"大字预览卡"：上面一行是当前字段的中文名，
// 中间是超大号数值，下面一行英文小字。纯数字的字段（背光亮度）用
// Montserrat 28 显得更锐利，中文/英文混排的用中文字体。
static void update_setting_light_display(void) {
    if (set_light_lbl_value == nullptr) return;

    static char nbuf[24];
    const char* value;
    bool numeric = false;

    switch (lightFieldIdx) {
        case 0:
            value = lightOn ? lightSwitchCN[0] : lightSwitchCN[1];
            break;
        case 1:
            snprintf(nbuf, sizeof(nbuf), "%d%%", (int)brightness * 100 / 255);
            value = nbuf;
            numeric = true;
            break;
        case 2:
            value = effectNames[currentEffect];
            break;
        default:
            value = indLevelNames[indLevel];
            break;
    }

    setText(set_light_lbl_cap, lightFieldCN[lightFieldIdx]);
    setText(set_light_lbl_en, lightFieldEN[lightFieldIdx]);
    setText(set_light_lbl_value, value);
    lv_obj_set_style_text_font(set_light_lbl_value,
        numeric ? &lv_font_montserrat_28 : &lv_font_simsun_16_cjk, LV_PART_MAIN);

    for (int i = 0; i < LIGHT_FIELD_COUNT; i++) {
        markField(set_light_field_labels[i], i == lightFieldIdx);
    }
}

// ===========================
// 设置界面构建函数
// ===========================
// 全部改成「建一次，之后只刷标签」。
// 原因：原来每次进来都 lv_obj_del(旧屏) + lv_obj_create(NULL)，而旧屏往往正是
// 当前活动屏 —— LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL，紧接着的刷新就
// 解引用空指针 → panic → 重启（MR 进录制必崩就是这个）。顺带整屏删建也太重。
static lv_obj_t* settingShell(lv_obj_t* scr, const char* titleCN, const char* titleEn) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 3, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 14, 13);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(scr);
    mkLabel(t, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(t, titleCN);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 12);

    lv_obj_t* sub = lv_label_create(scr);
    mkLabel(sub, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(sub, titleEn);
    lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -16, 16);

    lv_obj_t* hint = lv_label_create(scr);
    mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(hint, "←→ 切换 · ↑↓ 调整 · 回车保存 · ESC 取消");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    return scr;
}

// 字段标签（当前字段点亮成强调色）。返回里面的 label，供 update_* 改配色。
static lv_obj_t* fieldChip(lv_obj_t* parent, int x, int y, int w, const char* text, bool active) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, 22);
    lv_obj_align(c, LV_ALIGN_CENTER, x, y);
    mkChip(c, active ? CLR_SURFACE_2 : CLR_SURFACE,
              active ? CLR_ACCENT : CLR_TEXT_MUTE, 8);
    lv_obj_t* l = lv_label_create(c);
    mkLabel(l, &lv_font_simsun_16_cjk, active ? CLR_ACCENT : CLR_TEXT_MUTE);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return l;
}

// 高亮某个字段标签：文字变强调色，底下的胶囊也一起点亮
static void markField(lv_obj_t* lbl, bool active) {
    if (lbl == nullptr) return;
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(active ? CLR_ACCENT : CLR_TEXT_MUTE), LV_PART_MAIN);
    lv_obj_t* chip = lv_obj_get_parent(lbl);
    if (chip) {
        lv_obj_set_style_bg_color(chip,
            lv_color_hex(active ? CLR_SURFACE_2 : CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, active ? 1 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(chip, lv_color_hex(CLR_ACCENT_D), LV_PART_MAIN);
    }
}

static void build_settings_time(void) {
    // 编辑值从当前 RTC 取初值
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (ti && ti->tm_year >= 124) {
        timeEditY = ti->tm_year + 1900;
        timeEditMo = ti->tm_mon + 1;
        timeEditD = ti->tm_mday;
        timeEditH = ti->tm_hour;
        timeEditMi = ti->tm_min;
    }
    timeFieldIdx = 0;

    if (scr_settings_time == nullptr) {
        scr_settings_time = lv_obj_create(NULL);
        settingShell(scr_settings_time, "设置时间", "SET TIME");

        // 主卡片：日期 + 时间
        lv_obj_t* card = lv_obj_create(scr_settings_time);
        lv_obj_set_size(card, 200, 120);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -14);
        mkCard(card, CLR_SURFACE, 12);

        set_time_lbl_date = lv_label_create(card);
        mkLabel(set_time_lbl_date, &lv_font_montserrat_20, CLR_TEXT_DIM);
        lv_label_set_text(set_time_lbl_date, "2026-01-01");
        lv_obj_align(set_time_lbl_date, LV_ALIGN_TOP_MID, 0, 14);

        set_time_lbl_time = lv_label_create(card);
        mkLabel(set_time_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_time_lbl_time, "00:00");
        lv_obj_align(set_time_lbl_time, LV_ALIGN_CENTER, 0, 16);

        const char* fieldNames[5] = { "年", "月", "日", "时", "分" };
        int fieldX[5] = { -92, -46, 0, 46, 92 };
        for (int i = 0; i < 5; i++) {
            set_time_field_labels[i] = fieldChip(scr_settings_time, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_time_display();
    showScreen(scr_settings_time);
    currentSysMode = SYS_MODE_SET_TIME;
}

static void build_settings_alarm(void) {
    alarmEditH = alarmHour;
    alarmEditM = alarmMinute;
    alarmFieldIdx = 0;

    if (scr_settings_alarm == nullptr) {
        scr_settings_alarm = lv_obj_create(NULL);
        settingShell(scr_settings_alarm, "闹钟设置", "ALARM");

        lv_obj_t* card = lv_obj_create(scr_settings_alarm);
        lv_obj_set_size(card, 200, 118);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -14);
        mkCard(card, CLR_SURFACE, 12);

        set_alarm_lbl_time = lv_label_create(card);
        mkLabel(set_alarm_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_alarm_lbl_time, "07:00");
        lv_obj_align(set_alarm_lbl_time, LV_ALIGN_TOP_MID, 0, 18);

        // 开关 + 文字状态
        set_alarm_sw = lv_switch_create(card);
        lv_obj_set_size(set_alarm_sw, 52, 28);
        lv_obj_align(set_alarm_sw, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_TEXT), LV_PART_KNOB);

        set_alarm_state_lbl = lv_label_create(card);
        mkLabel(set_alarm_state_lbl, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(set_alarm_state_lbl, "已关闭");
        lv_obj_align(set_alarm_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

        const char* fieldNames[2] = { "时", "分" };
        int fieldX[2] = { -30, 30 };
        for (int i = 0; i < 2; i++) {
            set_alarm_field_labels[i] = fieldChip(scr_settings_alarm, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_alarm_display();
    showScreen(scr_settings_alarm);
    currentSysMode = SYS_MODE_SET_ALARM;
}

static void build_settings_timer(void) {
    timerFieldIdx = 0;
    if (timerTotalSec > 0) {
        timerEditH = timerTotalSec / 3600;
        timerEditM = (timerTotalSec % 3600) / 60;
        timerEditS = timerTotalSec % 60;
    }
    if (!timerRunning) timerRemainSec = timerTotalSec;

    if (scr_settings_timer == nullptr) {
        scr_settings_timer = lv_obj_create(NULL);
        settingShell(scr_settings_timer, "倒计时", "TIMER");

        lv_obj_t* card = lv_obj_create(scr_settings_timer);
        lv_obj_set_size(card, 200, 108);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -16);
        mkCard(card, CLR_SURFACE, 12);

        set_timer_lbl_time = lv_label_create(card);
        mkLabel(set_timer_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_timer_lbl_time, "00:05:00");
        lv_obj_align(set_timer_lbl_time, LV_ALIGN_TOP_MID, 0, 16);

        set_timer_btn = lv_obj_create(card);
        lv_obj_set_size(set_timer_btn, 108, 30);
        lv_obj_align(set_timer_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
        mkChip(set_timer_btn, CLR_ACCENT, CLR_BG, 15);
        lv_obj_t* btn_lbl = lv_label_create(set_timer_btn);
        mkLabel(btn_lbl, &lv_font_simsun_16_cjk, CLR_BG);
        lv_label_set_text(btn_lbl, "开始");
        lv_obj_center(btn_lbl);

        const char* fieldNames[3] = { "时", "分", "秒" };
        int fieldX[3] = { -52, 0, 52 };
        for (int i = 0; i < 3; i++) {
            set_timer_field_labels[i] = fieldChip(scr_settings_timer, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_timer_display();
    showScreen(scr_settings_timer);
    currentSysMode = SYS_MODE_SET_TIMER;
}

static void build_settings_caltemp(void) {
    calTempOriginal = shtTempOffset;

    if (scr_settings_caltemp == nullptr) {
        scr_settings_caltemp = lv_obj_create(NULL);
        settingShell(scr_settings_caltemp, "温度校准", "CALIBRATE");

        lv_obj_t* card = lv_obj_create(scr_settings_caltemp);
        lv_obj_set_size(card, 200, 110);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -18);
        mkCard(card, CLR_SURFACE, 12);

        lv_obj_t* cap = lv_label_create(card);
        mkLabel(cap, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(cap, "当前温度");
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 14);

        set_cal_lbl_temp = lv_label_create(card);
        mkLabel(set_cal_lbl_temp, &lv_font_montserrat_48, CLR_AMBER);
        lv_label_set_text(set_cal_lbl_temp, "--.- C");
        lv_obj_align(set_cal_lbl_temp, LV_ALIGN_CENTER, 0, 6);

        set_cal_lbl_offset = lv_label_create(card);
        mkLabel(set_cal_lbl_offset, &lv_font_simsun_16_cjk, CLR_ACCENT);
        lv_label_set_text(set_cal_lbl_offset, "偏移 +0.0");
        lv_obj_align(set_cal_lbl_offset, LV_ALIGN_BOTTOM_MID, 0, -12);

        lv_obj_t* hint2 = lv_label_create(scr_settings_caltemp);
        mkLabel(hint2, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(hint2, "↑↓ 调整偏移量");
        lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 92);
    }
    update_setting_caltemp_display();
    showScreen(scr_settings_caltemp);
    currentSysMode = SYS_MODE_CAL_TEMP;
}

// ===========================
// 构建：灯光设置
// ===========================
// 背光开关 / 背光亮度 / 灯效 / 状态灯亮度 四项集中在这里。
// 这几项以前散在菜单里"按一下循环一档"，问题是按错一次既不知道现在在哪一档、
// 也退不回去；做成设置页后每项都能看到当前值，←→ 选、↑↓ 调、回车才生效。
static void build_settings_light(void) {
    // 记一份原值，ESC 取消时原样还回去（灯效这类循环量不记就回不去了）
    lightOnOriginal = lightOn;
    lightBrightOriginal = brightness;
    lightEffectOriginal = currentEffect;
    lightIndLevelOriginal = indLevel;
    lightFieldIdx = 0;

    if (scr_settings_light == nullptr) {
        scr_settings_light = lv_obj_create(NULL);
        settingShell(scr_settings_light, "灯光设置", "LIGHTING");

        // 大字预览卡
        lv_obj_t* card = lv_obj_create(scr_settings_light);
        lv_obj_set_size(card, 204, 88);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -22);
        mkCard(card, CLR_SURFACE, 12);

        set_light_lbl_cap = lv_label_create(card);
        mkLabel(set_light_lbl_cap, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
        lv_label_set_text(set_light_lbl_cap, lightFieldCN[0]);
        lv_obj_align(set_light_lbl_cap, LV_ALIGN_TOP_MID, 0, 12);

        set_light_lbl_value = lv_label_create(card);
        mkLabel(set_light_lbl_value, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(set_light_lbl_value, "开");
        lv_obj_set_width(set_light_lbl_value, 184);
        lv_label_set_long_mode(set_light_lbl_value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(set_light_lbl_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(set_light_lbl_value, LV_ALIGN_CENTER, 0, 12);

        set_light_lbl_en = lv_label_create(card);
        mkLabel(set_light_lbl_en, &lv_font_montserrat_10, CLR_TEXT_MUTE);
        lv_label_set_text(set_light_lbl_en, lightFieldEN[0]);
        lv_obj_align(set_light_lbl_en, LV_ALIGN_BOTTOM_MID, 0, -12);

        // 四个字段胶囊排成 2x2
        const int chipX[2] = { -52, 52 };
        const int chipY[2] = { 48, 76 };
        for (int i = 0; i < LIGHT_FIELD_COUNT; i++) {
            set_light_field_labels[i] = fieldChip(scr_settings_light,
                chipX[i % 2], chipY[i / 2], 96, lightFieldCN[i], false);
        }
    }
    update_setting_light_display();
    showScreen(scr_settings_light);
    currentSysMode = SYS_MODE_SET_LIGHT;
}

// ===========================
// 宏录制界面（MR 三段式）
// ===========================
// MR 第一次按 = 连续输入(SEQ)，第二次按 = 组合键(CMB)，第三次按 = 取消回主屏。
// 三段状态机只有 recStage 一个变量，录制页也就只建一次、之后只刷 label ——
// 原来每录一颗键就 lv_obj_del(scr_recording) 整屏重建，而它正是活动屏，
// LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL，下一帧刷新直接空指针 → panic → 重启。
static const char* recStageTitleCN[2] = { "录制 · 连续输入", "录制 · 组合键" };
static const char* recStageTag[2]     = { "SEQ", "CMB" };

static void update_recording_display(void) {
    if (rec_lbl_count == nullptr) return;

    static char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d / %d", recKeyCount, MAX_REC_KEYS);
    setText(rec_lbl_count, cbuf);

    // 键流用按键名而不是十六进制码，和主屏"最近按键"保持一致
    static char kbuf[256];
    size_t used = 0;
    kbuf[0] = '\0';
    int shown = 0;
    for (int i = 0; i < recKeyCount && shown < 8; i++) {
        const char* nm = getKeyName(recKeyBuffer[i]);
        int n = snprintf(kbuf + used, sizeof(kbuf) - used, "%s%s",
                         shown ? " · " : "", nm);
        if (n < 0 || (size_t)n >= sizeof(kbuf) - used) break;
        used += n;
        shown++;
    }
    if (recKeyCount == 0) {
        snprintf(kbuf, sizeof(kbuf), "%s",
                 (recStage == REC_STAGE_SEQ) ? "依次按下要录制的按键…"
                                             : "按下要同时按住的组合键…");
    }
    setText(rec_lbl_keys, kbuf);
}

static void build_recording(void) {
    const bool seq = (recStage == REC_STAGE_SEQ);
    uint32_t accent = seq ? CLR_AMBER : CLR_VIOLET;

    if (scr_recording == nullptr) {
        scr_recording = lv_obj_create(NULL);
        settingShell(scr_recording, recStageTitleCN[0], "MACRO REC");

        // 标题竖条颜色随模式变（SEQ 琥珀 / CMB 紫）
        lv_obj_t* shell_bar = lv_obj_get_child(scr_recording, 0);
        if (shell_bar) {
            lv_obj_set_style_bg_color(shell_bar, lv_color_hex(accent), LV_PART_MAIN);
        }
        // 标题文字
        lv_obj_t* shell_title = lv_obj_get_child(scr_recording, 1);
        if (shell_title) setText(shell_title, recStageTitleCN[0]);

        // 计数
        rec_lbl_count = lv_label_create(scr_recording);
        mkLabel(rec_lbl_count, &lv_font_montserrat_48, accent);
        lv_label_set_text(rec_lbl_count, "0 / 64");
        lv_obj_align(rec_lbl_count, LV_ALIGN_TOP_MID, 0, 52);

        // 已录按键流
        lv_obj_t* keys_bg = lv_obj_create(scr_recording);
        lv_obj_set_size(keys_bg, 204, 76);
        lv_obj_align(keys_bg, LV_ALIGN_CENTER, 0, 6);
        mkCard(keys_bg, CLR_SURFACE, 10);

        rec_lbl_keys = lv_label_create(keys_bg);
        mkLabel(rec_lbl_keys, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(rec_lbl_keys, "");
        lv_obj_set_width(rec_lbl_keys, 184);
        lv_label_set_long_mode(rec_lbl_keys, LV_LABEL_LONG_WRAP);
        lv_obj_align(rec_lbl_keys, LV_ALIGN_TOP_MID, 0, 10);

        // 模式标签（SEQ / CMB）
        rec_lbl_mode = lv_label_create(scr_recording);
        mkLabel(rec_lbl_mode, &lv_font_montserrat_12, accent);
        lv_label_set_text(rec_lbl_mode, recStageTag[0]);
        lv_obj_align(rec_lbl_mode, LV_ALIGN_TOP_RIGHT, -16, 16);

        // 底部按键提示：settingShell 默认写的是设置页那套，这里换成录制页的
        lv_obj_t* shell_hint = lv_obj_get_child(scr_recording, 3);
        if (shell_hint) {
            setText(shell_hint, "M1-M12 保存 · MR 下一步 · ESC 取消");
            lv_obj_set_width(shell_hint, 204);
            lv_label_set_long_mode(shell_hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(shell_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
    } else {
        // 已存在：只切标题 / 配色
        lv_obj_t* shell_bar = lv_obj_get_child(scr_recording, 0);
        if (shell_bar) lv_obj_set_style_bg_color(shell_bar, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_t* shell_title = lv_obj_get_child(scr_recording, 1);
        if (shell_title) setText(shell_title, recStageTitleCN[seq ? 0 : 1]);
        setText(rec_lbl_mode, recStageTag[seq ? 0 : 1]);
        lv_obj_set_style_text_color(rec_lbl_mode, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_set_style_text_color(rec_lbl_count, lv_color_hex(accent), LV_PART_MAIN);
    }

    update_recording_display();
    showScreen(scr_recording);
    currentSysMode = seq ? SYS_MODE_REC_SEQ : SYS_MODE_REC_CMB;
}

// 从主屏进录制：永远从"连续输入"这一档开始
static void enterRecording(void) {
    recStage = REC_STAGE_SEQ;
    recKeyCount = 0;
    triggerHud("宏录制", "连续输入", lv_color_hex(CLR_AMBER));
    build_recording();
}

// 录制态里再按一次 MR：SEQ → CMB → 取消，三段走完
static void advanceRecordingStage(void) {
    switch (recStage) {
        case REC_STAGE_SEQ:
            recStage = REC_STAGE_CMB;
            recKeyCount = 0;      // 换模式就清空键流，两种模式的键流语义不同
            triggerHud("宏录制", "组合键", lv_color_hex(CLR_VIOLET));
            build_recording();
            break;

        case REC_STAGE_CMB:
        default:
            recStage = REC_STAGE_EXIT;
            recKeyCount = 0;
            triggerHud("宏录制", "已取消", lv_color_hex(CLR_RED));
            gotoMainScreen();
            break;
    }
}

static void cancelRecording(void) {
    recStage = REC_STAGE_EXIT;
    recKeyCount = 0;
    triggerHud("宏录制", "已取消", lv_color_hex(CLR_RED));
    gotoMainScreen();
}

static void finishMacroRecording(const String& targetKey) {
    if (recKeyCount == 0) {
        triggerHud("宏录制", "没有录到按键", lv_color_hex(CLR_RED));
        gotoMainScreen();
        return;
    }

    // 构建宏数据字符串
    String macroData;
    if (recStage == REC_STAGE_SEQ) {
        // SEQ: 击键序列
        macroData = "SEQ:";
        for (int i = 0; i < recKeyCount; i++) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%c", (char)recKeyBuffer[i]);
            macroData += buf;
        }
    } else {
        // CMB: 组合键（同时按下的键）
        macroData = "CMB:";
        for (int i = 0; i < recKeyCount; i++) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02X,", recKeyBuffer[i]);
            macroData += buf;
        }
    }

    // 保存宏
    char pKey[32];
    snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, targetKey.c_str());
    preferences.putString(pKey, macroData);

    triggerHud("宏已保存", targetKey.c_str(), lv_color_hex(CLR_GREEN));

    // 清理录制状态
    recKeyCount = 0;
    gotoMainScreen();
}

// ===========================
// 设置界面控制函数
// ===========================
static void moveSettingField(int dir) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME:
            timeFieldIdx = (timeFieldIdx + dir + 5) % 5;
            update_setting_time_display();
            break;
        case SYS_MODE_SET_ALARM:
            alarmFieldIdx = (alarmFieldIdx + dir + 2) % 2;
            update_setting_alarm_display();
            break;
        case SYS_MODE_SET_TIMER:
            if (!timerRunning) {
                timerFieldIdx = (timerFieldIdx + dir + 3) % 3;
                update_setting_timer_display();
            }
            break;
        case SYS_MODE_SET_LIGHT:
            lightFieldIdx = (uint8_t)((lightFieldIdx + dir + LIGHT_FIELD_COUNT) % LIGHT_FIELD_COUNT);
            update_setting_light_display();
            break;
    }
}

static void adjustSettingField(int delta) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME: {
            switch (timeFieldIdx) {
                case 0: timeEditY = constrain(timeEditY + delta, 2020, 2099); break;
                case 1: timeEditMo = constrain(timeEditMo + delta, 1, 12); break;
                case 2: timeEditD = constrain(timeEditD + delta, 1, 31); break;
                case 3: timeEditH = constrain(timeEditH + delta, 0, 23); break;
                case 4: timeEditMi = constrain(timeEditMi + delta, 0, 59); break;
            }
            update_setting_time_display();
            break;
        }
        case SYS_MODE_SET_ALARM: {
            switch (alarmFieldIdx) {
                case 0: alarmEditH = constrain(alarmEditH + delta, 0, 23); break;
                case 1: alarmEditM = constrain(alarmEditM + delta, 0, 59); break;
            }
            update_setting_alarm_display();
            break;
        }
        case SYS_MODE_SET_TIMER: {
            if (!timerRunning) {
                switch (timerFieldIdx) {
                    case 0: timerEditH = constrain(timerEditH + delta, 0, 23); break;
                    case 1: timerEditM = constrain(timerEditM + delta, 0, 59); break;
                    case 2: timerEditS = constrain(timerEditS + delta, 0, 59); break;
                }
                timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
                timerRemainSec = timerTotalSec;
                update_setting_timer_display();
            }
            break;
        }
        case SYS_MODE_CAL_TEMP: {
            shtTempOffset = constrain(shtTempOffset + delta * 0.5f, 50.0f, 80.0f);
            update_setting_caltemp_display();
            break;
        }
        case SYS_MODE_SET_LIGHT: {
            switch (lightFieldIdx) {
                case 0:
                    lightOn = !lightOn;
                    break;
                case 1:
                    // 20/255 一档，和原版 ↑↓ 调背光的手感一致
                    brightness = (uint8_t)constrain((int)brightness + delta * 20, 0, 255);
                    break;
                case 2:
                    currentEffect = (uint8_t)((currentEffect + (delta > 0 ? 1 : MAX_EFFECTS - 1))
                                              % MAX_EFFECTS);
                    break;
                default:
                    indLevel = (uint8_t)constrain((int)indLevel + delta, 0, IND_LEVEL_COUNT - 1);
                    indBrightness = indLevelValues[indLevel];
                    break;
            }
            update_setting_light_display();
            break;
        }
    }
}

static void saveSettingScreen(void) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME: {
            struct tm t = {0};
            t.tm_year = timeEditY - 1900;
            t.tm_mon = timeEditMo - 1;
            t.tm_mday = timeEditD;
            t.tm_hour = timeEditH;
            t.tm_min = timeEditMi;
            t.tm_sec = 0;
            time_t epoch = mktime(&t);
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            preferences.putUInt("set_epoch", epoch);
            triggerHud("时间", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
        case SYS_MODE_SET_ALARM: {
            alarmHour = alarmEditH;
            alarmMinute = alarmEditM;
            alarmEnabled = lv_obj_has_state(set_alarm_sw, LV_STATE_CHECKED);
            preferences.putUChar("alarm_h", alarmHour);
            preferences.putUChar("alarm_m", alarmMinute);
            preferences.putBool("alarm_on", alarmEnabled);
            triggerHud("闹钟", alarmEnabled ? "已开启" : "已关闭",
                lv_color_hex(alarmEnabled ? CLR_GREEN : CLR_TEXT_DIM));
            break;
        }
        case SYS_MODE_SET_TIMER: {
            timerRunning = false;
            timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
            timerRemainSec = timerTotalSec;
            triggerHud("倒计时", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
        case SYS_MODE_CAL_TEMP: {
            preferences.putFloat("sht_offset", shtTempOffset);
            triggerHud("温度校准", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
        case SYS_MODE_SET_LIGHT: {
            preferences.putBool("light_on", lightOn);
            preferences.putUChar("brightness", brightness);
            preferences.putUChar("effect", currentEffect);
            preferences.putUChar("ind_level", indLevel);
            triggerHud("灯光", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
    }
    gotoMainScreen();
}

static void cancelSettingScreen(void) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME:
            triggerHud("时间", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_SET_ALARM:
            triggerHud("闹钟", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_SET_TIMER:
            timerRunning = false;
            triggerHud("倒计时", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_CAL_TEMP:
            shtTempOffset = calTempOriginal;
            triggerHud("温度校准", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_SET_LIGHT:
            // 四项全部原样还回去：灯效/亮度这类循环量不记原值就退不回去了
            lightOn = lightOnOriginal;
            brightness = lightBrightOriginal;
            currentEffect = lightEffectOriginal;
            indLevel = lightIndLevelOriginal;
            indBrightness = indLevelValues[indLevel];
            triggerHud("灯光", "已取消", lv_color_hex(CLR_AMBER));
            break;
    }
    gotoMainScreen();
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
    triggerHud("配置方案", profileNamesCN[currentProfile], lv_color_hex(CLR_VIOLET));
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

// ===========================
// 按键名（给"最近按键"和录制列表用）
// ===========================
// 之前这里是直接 snprintf("%02X") 十六进制码，界面上就是一串数字 —— 空格显示 20、
// 字母显示 6B，纯粹没法看。照搬 s3.ino 原版的 getKeyName()。
static const char* getKeyName(uint16_t code) {
    if (code >= 'a' && code <= 'z') { static char b[2]; b[0] = (char)(code - 32); b[1] = 0; return b; }
    if (code >= 'A' && code <= 'Z') { static char b[2]; b[0] = (char)code; b[1] = 0; return b; }
    if (code >= '0' && code <= '9') { static char b[2]; b[0] = (char)code; b[1] = 0; return b; }
    switch (code) {
        case KEY_LEFT_CTRL: case KEY_RIGHT_CTRL:   return "Ctrl";
        case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return "Shift";
        case KEY_LEFT_ALT: case KEY_RIGHT_ALT:     return "Alt";
        case KEY_LEFT_GUI: case KEY_RIGHT_GUI:      return "Win";
        case KEY_RETURN:      return "Enter";
        case KEY_ESC:         return "Esc";
        case KEY_BACKSPACE:   return "Back";
        case KEY_TAB:         return "Tab";
        case ' ':             return "Space";
        case KEY_UP_ARROW:    return "UP";
        case KEY_DOWN_ARROW:  return "DOWN";
        case KEY_LEFT_ARROW:  return "LEFT";
        case KEY_RIGHT_ARROW: return "RIGHT";
        case KEY_INSERT:      return "Ins";
        case KEY_DELETE:      return "Del";
        case KEY_HOME:        return "Home";
        case KEY_END:         return "End";
        case KEY_PAGE_UP:     return "PgUp";
        case KEY_PAGE_DOWN:   return "PgDn";
        case KEY_CAPS_LOCK:   return "Caps";
        case 0x65:            return "Menu";      // 菜单键，USBHIDKeyboard.h 里没有对应宏
        case KEY_PRINT_SCREEN:return "PrtSc";
        case KEY_SCROLL_LOCK: return "Scrlk";
        case KEY_PAUSE:       return "Pause";
        case KEY_NUM_LOCK:    return "NumLk";
        case KEY_KP_SLASH:    return "Num /";
        case KEY_KP_ASTERISK: return "Num *";
        case KEY_KP_MINUS:    return "Num -";
        case KEY_KP_PLUS:     return "Num +";
        case KEY_KP_ENTER:    return "NumEnt";
        case KEY_KP_0: case KEY_KP_1: case KEY_KP_2: case KEY_KP_3: case KEY_KP_4:
        case KEY_KP_5: case KEY_KP_6: case KEY_KP_7: case KEY_KP_8: case KEY_KP_9: {
            static char b[6];
            snprintf(b, sizeof(b), "Num %c", (code == KEY_KP_0) ? '0' : (char)('1' + (code - KEY_KP_1)));
            return b;
        }
        case KEY_KP_DOT:      return "Num .";
        // 下面这组符号键 USBHIDKeyboard.h 里没有宏，直接用 HID Usage ID
        case 0x2F: return "[";       // [
        case 0x30: return "]";       // ]
        case 0x31: return "\\";      // \
        case 0x33: return ";";       // ;
        case 0x34: return "'";       // '
        case 0x35: return "`";       // `
        case 0x36: return ",";       // ,
        case 0x37: return ".";       // .
        case 0x38: return "/";       // /
        case 0x2D: return "-";
        case 0x2E: return "=";
        default: break;
    }
    if (code >= KEY_F1 && code <= KEY_F12) {
        static char b[4];
        snprintf(b, sizeof(b), "F%u", (unsigned)(code - KEY_F1 + 1));
        return b;
    }
    // 宏键 / 功能键
    if (code == K_LOGO) return "LOGO";
    if (code == K_PLAY) return "Play";
    if (code == K_NEXT) return "Next";
    if (code == K_PREV) return "Prev";
    if (code == K_FN)   return "Fn";
    if (code == K_MR)   return "MR";
    if (code == K_MC)   return "MC";
    if (code == K_ME)   return "ME";
    if (code == K_MA)   return "MA";
    if (code == K_MB)   return "MB";
    if (code >= K_M1 && code <= K_M12) {
        static char b[4];
        snprintf(b, sizeof(b), "M%u", (unsigned)(code - K_M1 + 1));
        return b;
    }
    // 0xC0~0xDE 是消费级控制键，落在 matrix 里的那些
    if (code >= 0xC0 && code <= 0xDF) {
        static char b[10];
        snprintf(b, sizeof(b), "Ctl%02X", (unsigned)(code - 0xC0));
        return b;
    }
    static char b[8];
    snprintf(b, sizeof(b), "%02X", (unsigned)code);
    return b;
}

static String getMacroNameByCode(uint16_t code) {
    if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
    if (code == K_MA) return "MA";
    if (code == K_MB) return "MB";
    if (code == K_MC) return "MC";    if (code == K_MR) return "MR";
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
    // I2C 健康检查：原版 s3.ino:3484-3491 每 500ms 探测一次 MCP23017，
    // 失败就 recoverI2CBus() 重新初始化整条总线。LVGL 版把这一段整个丢了，
    // 后果是 I2C 一旦被干扰（干扰按键灯/插拔 USB/BLE 突发），键盘就永久失灵，
    // 只能断电重启才恢复。补回来。
    static unsigned long lastI2CCheck = 0;
    if (millis() - lastI2CCheck > 500) {
        lastI2CCheck = millis();
        ct_mark(CT_S_SCAN_I2C);
        Wire.beginTransmission(MCP23017_ADDR);
        if (Wire.endTransmission() != 0) {
            ct_mark(CT_S_SCAN_RECOVER);
            recoverI2CBus();
            return;
        }
    }

    for (int c = 0; c < NUM_COLS; c++) {
        // 选列只用一次寄存器写。
        //
        // 原来的 mcp.digitalWrite(c, LOW) 在 Adafruit 库里是**读-改-写**：
        // Adafruit_MCP23XXX::digitalWrite 走 RegisterBits::write()，先读回整个
        // 端口字节、改一位、再写回 —— 一次 digitalWrite = 2 次 I2C 事务。
        // 16 列 × (LOW + HIGH) = 32 次 digitalWrite = **64 次 I2C 事务**，
        // 400kHz 下单次事务 ~112µs，光 I2C 就要 ~7ms，再加上 16×20µs 的列稳定
        // 等待，一次完整扫描 ~7.3ms。而 loop() 里 SCAN_INTERVAL 是 2ms ——
        // **需求是硬件能力的 3.5 倍**。扫描是阻塞 I2C，又排在 lvgl_driver_loop()
        // 前面，loop() 永远补不回来 → 界面停更（看起来卡死）+ 扫描本身也跑不满
        // （按键失灵）。这就是"切到要素最多的信息面板就整机卡死"的主因。
        //
        // MCP23017 引脚 0~7 = GPIOA、8~15 = GPIOB（Adafruit 库里 MCP_PORT(pin) = pin>>3）。
        // 每轮开始时两个端口都是 0xFF（全部 HIGH 空闲），所以选一列只需要改
        // 它所在的那一个端口：每列 1 次事务，一次扫描降到 ~1.8ms。
        if (c < 8) mcp.writeGPIOA((uint8_t)(0xFF & ~(1 << c)));
        else       mcp.writeGPIOB((uint8_t)(0xFF & ~(1 << (c - 8))));
        delayMicroseconds(20);

        for (int r = 0; r < NUM_ROWS; r++) {
            bool currentState = (digitalRead(rowPins[r]) == LOW);
            if (currentState != keebMatrix[r][c]) {
                if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
                    lastDebounceTime[r][c] = millis();
                    keebMatrix[r][c] = currentState;

                    uint16_t baseKey = baseMatrix[r][c];
                    if (baseKey == 0) continue;

                    // 现场记录：记下这一刻的阶段和键码。
                    // 「按某个键就崩」这类问题靠 panic 回溯很难对上号，
                    // 但只要开机 HUD 能报出"上次死在 按键分发，键 XXX"，
                    // 就能直接拿这个键去复现。
                    ct_mark(CT_S_SCAN_KEY);
                    ct_key(baseKey);
                    ct_set_ctx(currentSysMode, currentDispMode);

                    if (currentState) {
                        // 按键按下
                        totalKeyCount++;
                        lastActivityTime = millis();

                        // 唤醒
                        if (currentSysMode == SYS_MODE_SLEEP) {
                            gotoMainScreen();
                        }

                        // ================= 按键分发 =================
                        // 顺序必须和 s3.ino 原版一致：界面态（菜单 → 设置 → 录制）
                        // 先接管，K_MC 放最后。原来 K_MC 排在整条链的最前面，
                        // 于是"在菜单里再按一次 MC"会命中它 → menuSel 归零 + 重新
                        // build_menu()，视觉上等于没反应，永远出不去。
                        // 原版里 K_MC 在界面态的语义是"退出"，只有回到主界面才是"进入菜单"。
                        const bool inRecMode = (currentSysMode == SYS_MODE_REC_SEQ
                                                || currentSysMode == SYS_MODE_REC_CMB);
                        const bool inUiMode = (currentSysMode == SYS_MODE_MENU)
                                              || IS_SETTING_MODE(currentSysMode)
                                              || inRecMode;

                        if (inUiMode) {
                            // ---- 界面态：这一整块把按键吃干净，一律不发到主机 ----

                            // 菜单：方向键移动 / 回车选中 / ESC·MC 退回主屏
                            if (currentSysMode == SYS_MODE_MENU) {
                                if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                                    menuSel = (menuSel + 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
                                    menuSel = (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_RETURN) {
                                    handleMenuSelect();
                                } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                                    gotoMainScreen();
                                }
                            }
                            // 设置子界面：左右切字段 / 上下调值 / 回车保存 / ESC·MC 取消
                            else if (IS_SETTING_MODE(currentSysMode)) {
                                if (baseKey == KEY_LEFT_ARROW) moveSettingField(-1);
                                else if (baseKey == KEY_RIGHT_ARROW) moveSettingField(1);
                                else if (baseKey == KEY_UP_ARROW) adjustSettingField(1);
                                else if (baseKey == KEY_DOWN_ARROW) adjustSettingField(-1);
                                else if (baseKey == KEY_RETURN) saveSettingScreen();
                                else if (baseKey == KEY_ESC || baseKey == K_MC) cancelSettingScreen();
                            }
                            // 宏录制：M1-M12 保存 / MR 走三段 / ESC·MC 取消 / 其余记进键流
                            else {
                                if (baseKey == K_MC || baseKey == KEY_ESC) {
                                    cancelRecording();
                                } else if (baseKey >= K_M1 && baseKey <= K_M12) {
                                    finishMacroRecording(getMacroNameByCode(baseKey));
                                } else if (baseKey == K_MR) {
                                    // 三段：连续输入 → 组合键 → 取消
                                    advanceRecordingStage();
                                } else if (baseKey == K_FN) {
                                    fnPressed = true;   // 录制中也得能识别 Fn 修饰
                                } else if (baseKey < MACRO_BASE
                                           && baseKey != K_MA && baseKey != K_MB
                                           && baseKey != K_ME) {
                                    if (recKeyCount < MAX_REC_KEYS) {
                                        recKeyBuffer[recKeyCount++] = baseKey;
                                        update_recording_display();
                                    }
                                }
                            }
                        }
                        // ---- 主界面：普通键盘 + 功能键 ----
                        else {
                            // Fn 键
                            if (baseKey == K_FN) {
                                fnPressed = true;
                            }
                            // MC 进入菜单
                            else if (baseKey == K_MC) {
                                currentSysMode = SYS_MODE_MENU;
                                menuSel = 0;
                                menuScrollOffset = 0;
                                menuNeedsRebuild = true;
                                triggerHud("系统菜单", "请选择功能", lv_color_hex(CLR_ACCENT));
                            }
                            // MA / MB 全局键
                            else if (baseKey == K_MA) {
                                executeGlobalKey("MA");
                            }
                            else if (baseKey == K_MB) {
                                executeGlobalKey("MB");
                            }
                            // MR 键：第一次按 = 进入"连续输入"录制（必须排在 >= MACRO_BASE 之前）
                            else if (baseKey == K_MR) {
                                enterRecording();
                            }
                            // 宏按键
                            else if (baseKey >= MACRO_BASE) {
                                if (baseKey == K_LOGO) {
                                    // LOGO 短按 = 切换主屏风格；Fn+LOGO 仍然保留重启
                                    if (fnPressed) {
                                        triggerHud("系统重启", "请稍候", lv_color_hex(CLR_RED));
                                        pendingRestartMs = millis() + 600;
                                    } else {
                                        cycleDisplayStyle();
                                    }
                                } else if (baseKey == K_PLAY) {
                                    if (fnPressed) {
                                        SystemControl.press(SYSTEM_CONTROL_STANDBY);
                                        SystemControl.release();
                                    } else {
                                        ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                                        mediaPlaying = !mediaPlaying;
                                        triggerHud("媒体播放", mediaPlaying ? "播放" : "暂停",
                                            lv_color_hex(CLR_GREEN));
                                    }
                                } else if (baseKey == K_NEXT) {
                                    if (!fnPressed) {
                                        ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
                                        mediaPlaying = true;
                                        triggerHud("媒体播放", "下一曲", lv_color_hex(CLR_ACCENT));
                                    }
                                } else if (baseKey == K_PREV) {
                                    if (!fnPressed) {
                                        ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
                                        mediaPlaying = true;
                                        triggerHud("媒体播放", "上一曲", lv_color_hex(CLR_ACCENT));
                                    }
                                } else {
                                    String macroName = getMacroNameByCode(baseKey);
                                    if (baseKey >= K_M1 && baseKey <= K_M12) {
                                        executeGlobalKey(macroName);
                                    }
                                    executeMacro(macroName);
                                }
                            }
                            else {
                                uint16_t mappedKey = getMappedKey(baseKey);
                                // 最近按键显示：走 getKeyName，空格显示 "Space" 而不是 "20"
                                snprintf(lastKeyPressed, sizeof(lastKeyPressed), "%s", getKeyName(baseKey));
                                // 触发律动效果
                                uint8_t barIdx = totalKeyCount % 24;
                                rhythmBars[barIdx] = 60;
                                hostKeyHeld[r][c] = true;
                                kbPress((uint8_t)mappedKey);
                            }
                        }
                    } else {
                        // 按键释放
                        if (baseKey == K_FN) {
                            fnPressed = false;
                        }
                        else if (baseKey >= MACRO_BASE) {
                            // 消费级键松开才发 release。
                            // LOGO 的切风格动作已经挪到"按下"时做了（press 更跟手，
                            // 放在 release 上会让人以为没反应），这里不用再管。
                            if (baseKey == K_PLAY || baseKey == K_NEXT || baseKey == K_PREV) {
                                ConsumerControl.release();
                            }
                        }
                        else if (hostKeyHeld[r][c]) {
                            // 只有"按下时确实发给过主机"的键才需要补一个松键。
                            // 界面态里被吞掉的键（菜单导航、ESC、设置字段…）按下时没发过
                            // press，这里再发 release 就会给主机一个无头的松键事件。
                            hostKeyHeld[r][c] = false;
                            uint16_t mappedKey = getMappedKey(baseKey);
                            kbRelease((uint8_t)mappedKey);
                        }
                    }
                }
            }
        }
        // 全部列恢复空闲高电平（同时保住"下一轮开始时两个端口都是 0xFF"这个前提）
        mcp.writeGPIOA(0xFF);
        mcp.writeGPIOB(0xFF);
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
    // DISP_MODE:n - Switch display mode
    if (cmd.startsWith("DISP_MODE:")) {
        int mode = cmd.substring(10).toInt();
        if (mode >= 0 && mode < TOTAL_DISP_MODES) {
            currentDispMode = mode;
            preferences.putUChar("disp_mode", currentDispMode);
            renderCurrentDisplayBase();
            // 主机切风格时用户可能正停在菜单/设置界面，别把他踢出当前界面
            if (currentSysMode == SYS_MODE_NORMAL) showScreen(ensureMainScreen());
            triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_ACCENT));
        }
    }
    // TIME:epoch - Sync time
    else if (cmd.startsWith("TIME:")) {
        time_t t = cmd.substring(5).toInt();
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        preferences.putUInt("set_epoch", (uint32_t)t);
        triggerHud("时间", "已同步", lv_color_hex(CLR_GREEN));
    }
    // ALARMSET:HH:MM - Set alarm
    else if (cmd.startsWith("ALARMSET:")) {
        String timeStr = cmd.substring(9);
        int colonIdx = timeStr.indexOf(':');
        if (colonIdx > 0) {
            alarmHour = (uint8_t)timeStr.substring(0, colonIdx).toInt();
            alarmMinute = (uint8_t)timeStr.substring(colonIdx + 1).toInt();
            alarmEnabled = true;
            preferences.putUChar("alarm_h", alarmHour);
            preferences.putUChar("alarm_m", alarmMinute);
            preferences.putBool("alarm_on", alarmEnabled);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", alarmHour, alarmMinute);
            triggerHud("闹钟", buf, lv_color_hex(CLR_GREEN));
        }
    }
    // TIMERSET:HH:MM:SS or TIMERSET:STOP
    else if (cmd.startsWith("TIMERSET:")) {
        String timeStr = cmd.substring(9);
        if (timeStr == "STOP") {
            timerRunning = false;
            timerRemainSec = timerTotalSec;
            triggerHud("倒计时", "已停止", lv_color_hex(CLR_AMBER));
        } else {
            int firstColon = timeStr.indexOf(':');
            int secondColon = timeStr.lastIndexOf(':');
            if (firstColon > 0 && secondColon > firstColon) {
                timerEditH = timeStr.substring(0, firstColon).toInt();
                timerEditM = timeStr.substring(firstColon + 1, secondColon).toInt();
                timerEditS = timeStr.substring(secondColon + 1).toInt();
                timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
                timerRemainSec = timerTotalSec;
                timerRunning = true;
                timerStartMs = millis();
                char buf[16];
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timerEditH, timerEditM, timerEditS);
                triggerHud("倒计时", buf, lv_color_hex(CLR_ACCENT));
            }
        }
    }
    // MARQUEE:text - Set marquee text
    else if (cmd.startsWith("MARQUEE:")) {
        String text = cmd.substring(8);
        // 存储标语文本用于显示
        static char marqueeText[256] = {0};
        strncpy(marqueeText, text.c_str(), sizeof(marqueeText) - 1);
        marqueeText[sizeof(marqueeText) - 1] = '\0';
        triggerHud("滚动字幕", text.substring(0, min(16u, text.length())).c_str(), lv_color_hex(CLR_ACCENT));
        // 可以在此添加标语显示逻辑
    }
    // REMAP:prof:clear:rules - Key remapping
    else if (cmd.startsWith("REMAP:")) {
        String params = cmd.substring(6);
        int firstColon = params.indexOf(':');
        int secondColon = params.indexOf(':', firstColon + 1);
        if (firstColon > 0 && secondColon > firstColon) {
            int prof = params.substring(0, firstColon).toInt();
            String clearCmd = params.substring(firstColon + 1, secondColon);
            String rules = params.substring(secondColon + 1);

            if (prof >= 0 && prof < TOTAL_PROFILES) {
                if (clearCmd == "clear") {
                    remapCounts[prof] = 0;
                    char key[16];
                    snprintf(key, sizeof(key), "rmp_cnt_%d", prof);
                    preferences.putInt(key, 0);
                    triggerHud("按键重映射", "已清空", lv_color_hex(CLR_AMBER));
                }
                // 解析映射规则: fromKey,toKey;fromKey,toKey;...
                if (rules.length() > 0 && remapCounts[prof] < MAX_REMAP_RULES) {
                    int start = 0;
                    while (start < rules.length() && remapCounts[prof] < MAX_REMAP_RULES) {
                        int semicolon = rules.indexOf(';', start);
                        String rule = (semicolon > 0) ? rules.substring(start, semicolon) : rules.substring(start);
                        int comma = rule.indexOf(',');
                        if (comma > 0) {
                            profileRemaps[prof][remapCounts[prof]].fromKey = (uint16_t)rule.substring(0, comma).toInt();
                            profileRemaps[prof][remapCounts[prof]].toKey = (uint16_t)rule.substring(comma + 1).toInt();

                            char itemKey[20];
                            snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, remapCounts[prof]);
                            uint32_t val = ((uint32_t)profileRemaps[prof][remapCounts[prof]].fromKey << 16) | profileRemaps[prof][remapCounts[prof]].toKey;
                            preferences.putUInt(itemKey, val);
                            remapCounts[prof]++;
                        }
                        start = (semicolon > 0) ? semicolon + 1 : rules.length();
                    }
                    char key[16];
                    snprintf(key, sizeof(key), "rmp_cnt_%d", prof);
                    preferences.putInt(key, remapCounts[prof]);
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d rules", remapCounts[prof]);
                    triggerHud("按键重映射", buf, lv_color_hex(CLR_GREEN));
                }
            }
        }
    }
    // SET:name:value - Macro definition
    else if (cmd.startsWith("SET:")) {
        String params = cmd.substring(4);
        int colonIdx = params.indexOf(':');
        if (colonIdx > 0) {
            String name = params.substring(0, colonIdx);
            String value = params.substring(colonIdx + 1);
            char pKey[32];
            snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, name.c_str());
            preferences.putString(pKey, value);
            triggerHud("已写入宏", name.c_str(), lv_color_hex(CLR_GREEN));
        }
    }
    // GSET:name:value - Global key assignment
    else if (cmd.startsWith("GSET:")) {
        String params = cmd.substring(5);
        int colonIdx = params.indexOf(':');
        if (colonIdx > 0) {
            String name = params.substring(0, colonIdx);
            String value = params.substring(colonIdx + 1);
            char gKey[32];
            snprintf(gKey, sizeof(gKey), "g_%s", name.c_str());
            preferences.putString(gKey, value);
            triggerHud("已写入全局键", name.c_str(), lv_color_hex(CLR_ACCENT));
        }
    }
    // ME_TEXT:text - ME text send (macro/execute text)
    else if (cmd.startsWith("ME_TEXT:")) {
        String text = cmd.substring(8);
        executeSequenceAction(text);
        triggerHud("文本已发送", "完成", lv_color_hex(CLR_GREEN));
    }
    // LOGO_JPEG_START:size - Wallpaper upload start
    else if (cmd.startsWith("LOGO_JPEG_START:")) {
        int size = cmd.substring(16).toInt();
        // 预留壁纸上传接口
        triggerHud("壁纸", "正在接收", lv_color_hex(CLR_AMBER));
        // 实际数据通过BLECharacteristic的二进制数据接收
    }
    // NOTIFY:text -> ALERT_GREEN
    else if (cmd.startsWith("NOTIFY:")) {
        String text = cmd.substring(7);
        pushNotification(ALERT_GREEN, text);
    }
    // ALERT: - Notification alerts
    else if (cmd.startsWith("ALERT:")) {
        String sub = cmd.substring(6);
        if (sub == "OFF" || sub == "CLEAR") {
            clearNotifications();
        }
        else if (sub.startsWith("RED")) {
            String text = sub.startsWith("RED:") ? sub.substring(4) : "Alert";
            pushNotification(ALERT_RED, text);
        }
        else if (sub.startsWith("GREEN")) {
            String text = sub.startsWith("GREEN:") ? sub.substring(6) : "Alert";
            pushNotification(ALERT_GREEN, text);
        }
        else if (sub.startsWith("YELLOW")) {
            String text = sub.startsWith("YELLOW:") ? sub.substring(7) : "Alert";
            pushNotification(ALERT_YELLOW, text);
        }
    }
    else if (cmd == "BTN:KNOB") {
        if (currentSysMode == SYS_MODE_MENU) {
            handleMenuSelect();
        }
    }
    else if (cmd == "BTN:LIGHT") {
        if (currentSysMode == SYS_MODE_MENU) {
            gotoMainScreen();
        }
    }
    else if (cmd.startsWith("ROT:")) {
        bool isRight = cmd.substring(4) == "R";
        if (currentSysMode == SYS_MODE_MENU) {
            menuSel = isRight ? (menuSel + 1) % MENU_ITEMS : (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
            build_menu();
        }
    }
}

// ===========================
// 灯效引擎
// ===========================
static uint32_t colorHSV(uint16_t hue, uint8_t sat, uint8_t val) {
    uint8_t r, g, b;
    uint8_t base = ((255 - sat) * val) >> 8;
    switch ((hue / 10922) % 6) {
        case 0: r = val; g = (((val - base) * (hue % 10922)) / 10922) + base; b = base; break;
        case 1: r = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; g = val; b = base; break;
        case 2: r = base; g = val; b = (((val - base) * (hue % 10922)) / 10922) + base; break;
        case 3: r = base; g = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; b = val; break;
        case 4: r = (((val - base) * (hue % 10922)) / 10922) + base; g = base; b = val; break;
        default: r = val; g = base; b = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void setLedRGB(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= 0 && index < TOTAL_LEDS) {
        ledRgb[index][0] = r;
        ledRgb[index][1] = g;
        ledRgb[index][2] = b;
    }
}

static void clearAllLeds() {
    for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, 0, 0, 0);
}

static void setMainLedsColor(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < 15; i++) setLedRGB(i, r, g, b);
}

// 三颗状态指示灯（对应 C3 上 16/17/18 号物理灯位）
// 亮色沿用原版 s3.ino 的取值：NUM 橙 / CAPS 蓝 / SCR 琥珀红。
// 这三颗的缩放在这里自己做，用灯光设置页里的"状态灯亮度"那一档 ——
// sendLedFrameToC3 只给 0~15 乘主背光 brightness，混进去的话
// 背光拧到最暗时锁状态会一起消失，而锁状态恰恰是必须一直看得见的信息。
static void renderIndicators(void) {
    uint8_t b = indBrightness;
    setLedRGB(16, (numLock    ? (uint8_t)(255 * b / 255) : 0),
                     (numLock    ? (uint8_t)(180 * b / 255) : 0), 0);
    setLedRGB(17, 0, (capsLock   ? (uint8_t)(150 * b / 255) : 0),
                     (capsLock   ? (uint8_t)(255 * b / 255) : 0));
    setLedRGB(18, (scrollLock ? (uint8_t)(255 * b / 255) : 0),
                     (scrollLock ? (uint8_t)( 50 * b / 255) : 0), 0);
}

static void drawBreathing(uint8_t maxR, uint8_t maxG, uint8_t maxB) {
    static uint16_t effectFrame = 0;
    float val = (exp(sin(effectFrame * 0.03)) - 0.36787944) * 108.0;
    float ratio = val / 255.0;
    if (ratio > 1.0) ratio = 1.0;
    if (ratio < 0.0) ratio = 0.0;
    setMainLedsColor(maxR * ratio, maxG * ratio, maxB * ratio);
    effectFrame++;
}

static void renderLightingEngine(void) {
    // 背光总开关只管 0~15；16~18 三颗锁状态灯始终由 renderIndicators 驱动，
    // 不受背光开关和背光亮度影响（见 renderIndicators 的注释）
    if (!lightOn) {
        clearAllLeds();
    } else {
        // 根据 currentEffect 执行对应灯效
        switch (currentEffect) {
        case 0: clearAllLeds(); break;  // 关闭
        case 1: setMainLedsColor(255, 0, 0); break;      // 纯红
        case 2: setMainLedsColor(0, 255, 0); break;      // 纯绿
        case 3: setMainLedsColor(0, 0, 255); break;      // 纯蓝
        case 4: setMainLedsColor(0, 127, 255); break;     // 冰蓝
        case 5: setMainLedsColor(255, 255, 255); break;    // 纯白
        case 6: drawBreathing(255, 0, 0); break;         // 红呼吸
        case 7: drawBreathing(0, 255, 0); break;         // 绿呼吸
        case 8: drawBreathing(0, 0, 255); break;         // 蓝呼吸
        case 9: drawBreathing(0, 127, 255); break;        // 冰蓝呼吸
        case 10: { // 彗星
            static uint16_t effectFrame = 0;
            clearAllLeds();
            int totalSteps = 28;  // (15-1)*2
            int step = effectFrame % totalSteps;
            int pos = (step < 15) ? step : (totalSteps - step);
            setLedRGB(pos, 255, 0, 50);
            for (int i = 0; i < 15; i++) {
                int diff = abs(i - pos);
                if (diff == 1) setLedRGB(i, 80, 0, 15);
                else if (diff == 2) setLedRGB(i, 20, 0, 3);
            }
            effectFrame++;
            break;
        }
        case 11: { // 双彗星
            static uint16_t effectFrame = 0;
            clearAllLeds();
            int totalSteps = 28;
            int step = effectFrame % totalSteps;
            int pos1 = (step < 15) ? step : (totalSteps - step);
            int pos2 = (step < 15) ? (14 - step) : (step - 14);
            setLedRGB(pos1, 180, 0, 255);
            setLedRGB(pos2, 0, 180, 255);
            for (int i = 0; i < 15; i++) {
                if (abs(i - pos1) == 1) setLedRGB(i, 50, 0, 80);
                if (abs(i - pos2) == 1) setLedRGB(i, 0, 50, 80);
            }
            effectFrame++;
            break;
        }
        case 12: { // 彩虹流光
            static uint16_t effectFrame = 0;
            for (int i = 0; i < 15; i++) {
                uint32_t col = colorHSV(effectFrame + (i * 65536L / 15), 255, 255);
                setLedRGB(i, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
            }
            effectFrame += 256;
            break;
        }
        default: clearAllLeds(); break;
        }
    }

    // 三颗锁状态灯：无论背光是开是关都要写，锁状态是必须一直看得见的信息
    renderIndicators();
}

static void sendLedFrameToC3(void) {
    uint8_t packet[61];
    packet[0] = 0xAA;
    packet[1] = 0x55;
    packet[2] = 0x01;

    // 0~15 主背光：按 brightness 缩放
    uint8_t ledScale = brightness;
    for (int i = 0; i < NUM_MAIN_LEDS; i++) {
        packet[3 + i * 3 + 0] = (ledRgb[i][0] * ledScale) / 255;
        packet[3 + i * 3 + 1] = (ledRgb[i][1] * ledScale) / 255;
        packet[3 + i * 3 + 2] = (ledRgb[i][2] * ledScale) / 255;
    }
    // 16~18 状态指示灯：renderIndicators() 已经乘过 indBrightness，这里原样发
    for (int i = NUM_MAIN_LEDS; i < TOTAL_LEDS; i++) {
        packet[3 + i * 3 + 0] = ledRgb[i][0];
        packet[3 + i * 3 + 1] = ledRgb[i][1];
        packet[3 + i * 3 + 2] = ledRgb[i][2];
    }
    packet[60] = 0xEE;
    Serial1.write(packet, sizeof(packet));
}

// ===========================
// 动态元素更新
// ===========================
// 100ms 跑一次就够了。这里的值全是慢变量：时钟只显示到分钟、日期一天变一次、
// 温湿度 15 分钟才读一次、击键数/锁状态是用户动作触发的。原来是每轮 loop()
// 都跑，而 loop() 里排着阻塞式 I2C 的键盘扫描，循环频率高达几百 Hz ——
// 就算下面的 setText/setBgColor 都做了变化检测，每轮仍然要白跑一遍
// time()/localtime()/strftime()（localtime 在 ESP32 上要算时区，并不便宜）。
#define DYNAMIC_REFRESH_MS 100UL
static void updateDynamicElements(void) {
    static unsigned long lastRunMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastRunMs < DYNAMIC_REFRESH_MS) return;
    lastRunMs = nowMs;

    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || ti->tm_year < 124) return;

    static char time_buf[16], date_buf[32], num_buf[24];
    static const char* WEEK[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
    snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d %s",
             ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, WEEK[ti->tm_wday]);
    snprintf(num_buf, sizeof(num_buf), "%u", totalKeyCount);

    // 顶部条的锁灯 + 方案名（6 种风格共用）
    const bool locks[3] = { numLock, capsLock, scrollLock };
    for (int i = 0; i < 3; i++) {
        setBgColor(topLockDot[i], locks[i] ? topLockOn[i] : CLR_STROKE);
    }
    setText(topProfileLbl, profileNamesCN[currentProfile]);

    switch (currentDispMode) {
        case DISP_MODE_GEEK: {
            setText(gk_lbl_clock, time_buf);
            setText(gk_lbl_date, date_buf);

            static char tbuf[16], hbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.1fC", shtTemp);
            snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);
            setText(gk_lbl_temp, tbuf);
            setText(gk_lbl_hum, hbuf);
            setText(gk_lbl_keys, num_buf);
            break;
        }

        case DISP_MODE_BIG_CLOCK: {
            setText(bc_lbl_time, time_buf);
            setText(bc_lbl_date, date_buf);
            break;
        }

        case DISP_MODE_INFO_PANEL: {
            setText(ip_lbl_clock, time_buf);
            setText(ip_lbl_date, date_buf);
            setText(ip_lbl_keys, num_buf);
            // 菜单第 4 项「按键回显开关」控制的就是这里：关掉后不再显示具体按键名
            setText(ip_lbl_lastkey, showKeystrokes ? lastKeyPressed : "已关闭");
            // 锁状态三层一起变：圆点亮语义色、文字提到主文字色、整颗胶囊底色抬起来。
            // 之前 ip_circle_* 建完之后根本没人刷新，三颗灯从头到尾都是灭的。
            // setBgColor/setTextColor 会先读回比对，值没变就不写 —— 这是关键，
            // 见文件上方 setBgColor 的注释（LVGL 8.4 无条件 invalidate）。
            for (int i = 0; i < 3; i++) {
                bool on = locks[i];
                setBgColor(ipLockDot[i],  on ? lockLedColor[i] : CLR_STROKE);
                setTextColor(ipLockLbl[i], on ? CLR_TEXT : CLR_TEXT_MUTE);
                setBgColor(ipLockChip[i], on ? CLR_SURFACE_2 : CLR_SURFACE);
            }
            break;
        }

        case DISP_MODE_KEY_MON: {
            setText(km_lbl_lastkey, showKeystrokes ? lastKeyPressed : "已关闭");
            setText(km_lbl_keys, num_buf);
            setText(km_lbl_profile, profileNamesCN[currentProfile]);
            break;
        }

        case DISP_MODE_RHYTHM: {
            // 柱子从基线往上长：高度 4~100，颜色随高度从青渐变到紫
            // 这里是全项目最热的一处：24 根柱子 × (尺寸 + 位置 + 底色) = 72 次写入。
            // 之前是无条件写，等于每轮 loop 都把屏幕重新 invalidate 一遍 72 次，
            // 节奏页会重演信息面板那种"卡死 + 按键失灵"。setSizePos/setBgColor
            // 会先读回比对，静止时一次 LVGL 调用都不产生。
            for (int i = 0; i < 24; i++) {
                if (!rh_bars[i]) continue;
                uint8_t hgt = rhythmBars[i];
                if (hgt > 0) rhythmBars[i] = hgt - 1;

                uint8_t v = (hgt > 25) ? 25 : hgt;
                int barH = 4 + v * 4;
                setSizePos(rh_bars[i], 6, barH, 2 + i * 10, 110 - barH);

                if (hgt == 0) {
                    setBgColor(rh_bars[i], CLR_SURFACE_2);
                } else {
                    // 颜色随高度在 青(0x22D3EE) → 紫(0xA78BFA) 之间线性插值
                    uint16_t t = (uint16_t)(v * 255 / 25);
                    uint8_t r = (uint8_t)(0x22 + (0xA7 - 0x22) * t / 255);
                    uint8_t g = (uint8_t)(0xD3 + (0x8B - 0xD3) * t / 255);
                    uint8_t b = (uint8_t)(0xEE + (0xFA - 0xEE) * t / 255);
                    setBgColor(rh_bars[i], ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
                }
            }
            setText(rh_lbl_keys, num_buf);
            break;
        }

        case DISP_MODE_WALLPAPER: {
            setText(wp_lbl_time, time_buf);
            break;
        }
    }
}

// ===========================
// 主屏渲染（预创建模式）
// ===========================
// 重建主屏内容（**不**切屏）
// ===========================
// 故意不在这里切屏：这个函数有两类调用方 ——
//   1) "我就是要回主屏"（ESC 退出、设置保存/取消、录制结束、开机、串口切风格）
//      → 由调用方显式 showScreen(ensureMainScreen())
//   2) "我只是换一种主屏风格，用户还停在菜单里"（菜单里的"切换主屏风格"）
//      → 保持在菜单，切屏会把用户从菜单里踢出去
// 之前这里无条件 showScreen(scr_main)，第 2 类调用方会被误踢出菜单。
static void renderCurrentDisplayBase(void) {
    ct_mark(CT_S_REBUILD);
    init_styles();
    ensureMainScreen();

    // 删除旧显示组件
    if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }
    if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }
    if (ip_bg) { lv_obj_del(ip_bg); ip_bg = nullptr; }
    if (km_bg) { lv_obj_del(km_bg); km_bg = nullptr; }
    if (rh_bg) { lv_obj_del(rh_bg); rh_bg = nullptr; }
    if (wp_bg) { lv_obj_del(wp_bg); wp_bg = nullptr; }

    // ------------------------------------------------------------------
    // 临时诊断：主屏风格整体停用
    // ------------------------------------------------------------------
    // 现象：进主屏风格后整机卡死、按键全部失灵。
    // 这里只跳过"建内容"这一步，主屏仍然是 ensureMainScreen() 那块合法屏幕，
    // 所以切屏、息屏、菜单、设置、录制、按键扫描等外围逻辑都保持原样。
    //
    // 停用是安全的：所有 gk_/bc_/ip_/km_/rh_/wp_ 指针此时全是 nullptr，而
    // updateDynamicElements() 里对这些指针的写入全部经过 setText/setBgColor/
    // setTextColor/setSizePos，这四个 setter 第一行就是 if (o == nullptr) return。
    // 顶部条 topLockDot[] 同理。所以屏幕全黑，但键盘应当恢复正常响应。
    //
    // 想恢复：把下面这行改成 false，六种风格立刻全部回来。
    if (MAIN_STYLE_DISABLED) { mainContentValid = true; return; }

    switch (currentDispMode) {
        case DISP_MODE_GEEK: build_style_geek(); break;
        case DISP_MODE_BIG_CLOCK: build_style_bigclock(); break;
        case DISP_MODE_INFO_PANEL: build_style_info_panel(); break;
        case DISP_MODE_KEY_MON: build_style_keymon(); break;
        case DISP_MODE_RHYTHM: build_style_rhythm(); break;
        case DISP_MODE_WALLPAPER: build_style_wallpaper(); break;
    }
    mainContentValid = true;
}

// ===========================
// 开机诊断：把"为什么重启"直接打在屏幕上
// ===========================
// 之前遇到"用一段时间自己重启"只能靠猜：芯片复位后 USB 会重新枚举，
// Windows 侧的串口句柄随之失效，PC 上抓不到 panic 回溯（这是工具限制，不是固件问题）。
// 所以这里把 esp_reset_reason() 做成开机 HUD：真再重启时一眼就能分清是
//   掉电/上电、brownout 掉电压、任务看门狗超时、还是 panic（空指针/断言/内存耗尽）。
// 另外把 PSRAM 实际大小和内部堆余量也报出来 —— 内存耗尽正是之前的重启主因之一。
static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "上电启动";
        case ESP_RST_EXT:        return "外部复位";
        case ESP_RST_SW:         return "软件重启";
        case ESP_RST_PANIC:      return "异常崩溃";
        case ESP_RST_INT_WDT:    return "中断看门狗";
        case ESP_RST_TASK_WDT:   return "任务看门狗";
        case ESP_RST_WDT:        return "其它看门狗";
        case ESP_RST_DEEPSLEEP:  return "深度睡眠唤醒";
        case ESP_RST_BROWNOUT:   return "电压掉电";
        case ESP_RST_SDIO:       return "SDIO 复位";
        default:                 return "未知原因";
    }
}

// 崩溃现场：把 RTC 慢存里留的阶段号翻成中文
static const char* ctStageName(uint32_t s) {
    switch (s) {
        case CT_S_IDLE:         return "循环末尾";
        case CT_S_LOOP:         return "主循环开头";
        case CT_S_BLE_DELAY:    return "蓝牙重连延时";
        case CT_S_PING:         return "串口心跳";
        case CT_S_SHT:          return "温湿度读取";
        case CT_S_SCAN:         return "键盘扫描";
        case CT_S_SCAN_I2C:     return "总线探测";
        case CT_S_SCAN_RECOVER: return "总线恢复";
        case CT_S_SCAN_KEY:     return "按键分发";
        case CT_S_LED:          return "灯效引擎";
        case CT_S_SLEEP:        return "自动息屏";
        case CT_S_MENU:         return "菜单重建";
        case CT_S_DYNAMIC:      return "动态刷新";
        case CT_S_HUD:          return "提示框显隐";
        case CT_S_NOTIF:        return "通知队列";
        case CT_S_LVGL:         return "屏幕刷屏";
        case CT_S_REBUILD:      return "主屏重建";
        case CT_S_HANG:         return "心跳中断";
        default:                return "未知阶段";
    }
}

// 第二张 HUD（内存详情）延迟 1.4s 弹出，让第一张"重启原因"先被看到
// 开机诊断 HUD 队列：三张依次弹出（原因 → 内存 → 上次现场）。
// 之前只支持一张，第二个字段只能靠额外的 pending 标志硬塞；换成队列后
// 后面再加诊断项直接 push 就行。
#define BOOT_HUD_MAX 3
struct BootHudItem {
    unsigned long atMs;
    char title[16];
    char value[40];
    uint32_t color;
};
static BootHudItem bootHud[BOOT_HUD_MAX];
static int bootHudCount = 0;
static int bootHudNext = 0;

static void pushBootHud(unsigned long delayMs, const char* title,
                        const char* value, uint32_t color) {
    if (bootHudCount >= BOOT_HUD_MAX) return;
    BootHudItem& it = bootHud[bootHudCount++];
    it.atMs = millis() + delayMs;
    snprintf(it.title, sizeof(it.title), "%s", title);
    snprintf(it.value, sizeof(it.value), "%s", value);
    it.color = color;
}

void bootDetailTick(void) {
    if (bootHudNext >= bootHudCount) return;
    BootHudItem& it = bootHud[bootHudNext];
    if ((long)(millis() - it.atMs) < 0) return;
    bootHudNext++;
    triggerHud(it.title, it.value, lv_color_hex(it.color));
}

static void showBootDiagnostics(void) {
    esp_reset_reason_t why = esp_reset_reason();
    // PSRAM 容量用 heap_caps 查：这套 Arduino 工具链（IDF 4.4）里没有 esp_psram.h
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t heapFree = esp_get_free_heap_size();
    uint32_t heapMin = esp_get_minimum_free_heap_size();

    // 累计开机次数，用来区分"断电重上电"和"跑着跑着自己重启"。
    // NVS 自带磨损均衡，开机写一次不会伤 flash。
    uint32_t boots = preferences.getUInt("boot_cnt", 0) + 1;
    preferences.putUInt("boot_cnt", boots);

    Serial.printf("[BOOT] reason=%d (%s) psram=%u heap=%u minHeap=%u boots=%u\n",
                  (int)why, resetReasonName(why), (unsigned)psram,
                  (unsigned)heapFree, (unsigned)heapMin, (unsigned)boots);

    // 第一张：重启原因。非上电的用琥珀色，一眼能看出"这次不是正常启动"
    bool abnormal = (why != ESP_RST_POWERON && why != ESP_RST_DEEPSLEEP);
    triggerHud(abnormal ? "重启原因" : "开机", resetReasonName(why),
               lv_color_hex(abnormal ? CLR_AMBER : CLR_GREEN));

    // 第二张：PSRAM 与内部堆余量（内存耗尽是之前重启的主因之一）
    char buf[40];
    if (psram > 0) {
        snprintf(buf, sizeof(buf), "PSRAM%uM 余%uK 第%u次",
                 (unsigned)(psram / (1024 * 1024)), (unsigned)(heapFree / 1024), (unsigned)boots);
    } else {
        // PSRAM 没起来时 LVGL 对象池回落内部堆，余量会小很多
        snprintf(buf, sizeof(buf), "无PSRAM 余%uK 第%u次",
                 (unsigned)(heapFree / 1024), (unsigned)boots);
    }
    pushBootHud(1400, "内存", buf, CLR_ACCENT);

    // 第三张：上次运行是怎么结束的。
    // 关键在于 panic 之后 USB 会重新枚举，PC 侧串口句柄失效，崩溃回溯抓不到，
    // 所以现场必须留在芯片里（RTC 慢存，见 crash_trace.h），这里回放出来。
    if (ct_prev.magic == CT_MAGIC) {
        char keyName[16];
        if (ct_prev.keyCode == 0xFFFFFFFFUL) {
            snprintf(keyName, sizeof(keyName), "无");
        } else {
            snprintf(keyName, sizeof(keyName), "%s", getKeyName((uint16_t)ct_prev.keyCode));
        }
        if (ct_prev.ready == 0) {
            // 上一次没跑完 setup 就没了 —— 启动流程里就崩了，跟运行期是两回事
            snprintf(buf, sizeof(buf), "启动未完成 %s", ctStageName(ct_prev.stage));
            pushBootHud(2900, "上次死在", buf, CLR_RED);
        } else {
            snprintf(buf, sizeof(buf), "%s%s键%s",
                     ctStageName(ct_prev.stage), ct_prev.hang ? " 卡死" : "", keyName);
            pushBootHud(2900, ct_prev.hang ? "上次卡在" : "上次死在", buf,
                        ct_prev.hang ? CLR_AMBER : CLR_RED);
        }
    } else {
        // 没有记录 = 断电重启（RTC 慢存会掉电清零），不是软件崩溃
        snprintf(buf, sizeof(buf), "断电或首次烧录");
        pushBootHud(2900, "上次", buf, CLR_TEXT_DIM);
    }

    Serial.printf("[BOOT] prev: hang=%u stage=%u(%s) key=%s uptimeMs=%u\n",
                  (unsigned)ct_prev.hang, (unsigned)ct_prev.stage,
                  ctStageName(ct_prev.stage),
                  (ct_prev.keyCode == 0xFFFFFFFFUL) ? "none" : getKeyName((uint16_t)ct_prev.keyCode),
                  (unsigned)ct_prev.uptimeMs);
}

// ===========================
// setup()
// ===========================
void setup() {
    // 崩溃现场记录：必须**第一时间**初始化。setup() 里后面任何一步 panic，
    // 现场也已经留在 RTC 慢存里了，下次开机能显示出来。
    ct_begin();

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

    // 灯光四项：与"灯光设置"页一一对应，回车保存时才写 NVS
    lightOn       = preferences.getBool("light_on", true);
    brightness    = preferences.getUChar("brightness", 140);
    currentEffect = preferences.getUChar("effect", 1);
    indLevel      = preferences.getUChar("ind_level", 3);
    if (currentEffect >= MAX_EFFECTS) currentEffect = 1;
    if (indLevel >= IND_LEVEL_COUNT) indLevel = 3;
    indBrightness = indLevelValues[indLevel];

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
    gotoMainScreen();
    lastActivityTime = millis();

    showBootDiagnostics();
    ct_ready();
    ct_startMonitor();

    Serial.println("YYQ-MX9.0 LVGL Ready");
}

// ===========================
// loop()
// ===========================
unsigned long lastScanTime = 0;
// 125Hz 扫描。机械键盘的按键去抖是 20ms，8ms 的轮询周期能给出 ~40Hz 的
// 有效去抖分辨率，手感上和原来 2ms 没有可感知差别。
//
// 为什么不能再用 2ms：改成单次寄存器写选列之后，一次完整扫描（16 列 ×
// 1 次 I2C 写 + 16×20µs 稳定等待）仍要 ~2.1ms。2ms 的调度周期等于要求
// CPU 100% 全程给 I2C，loop() 里排在后面的 LVGL 一帧都跑不到 —— 屏幕停更
// 看起来就是卡死。8ms 让扫描只占约 26% 的 CPU，剩下的留给显示和 BLE。
const unsigned long SCAN_INTERVAL = 8;

void loop() {
    esp_task_wdt_reset();
    ct_mark(CT_S_LOOP);

    // 开机诊断弹窗队列（重启原因 / 内存 / 上次现场）
    bootDetailTick();

    // 延迟重启
    if (pendingRestartMs != 0 && (long)(millis() - pendingRestartMs) >= 0) {
        pendingRestartMs = 0;
        esp_restart();
    }

    // BLE 重连
    if (!deviceConnected && oldDeviceConnected) {
        ct_mark(CT_S_BLE_DELAY);
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
        ct_mark(CT_S_SHT);
        sht31_update();
        lastSHTRead = millis();
    }

    // 键盘扫描
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        ct_mark(CT_S_SCAN);
        scanKeyboardMatrix();
    }

    // 灯效
    if (millis() - lastLedFrameTime >= 20) {
        lastLedFrameTime = millis();
        ct_mark(CT_S_LED);
        renderLightingEngine();
        sendLedFrameToC3();
    }

    // 息屏
    if (currentSysMode == SYS_MODE_NORMAL && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
        currentSysMode = SYS_MODE_SLEEP;
        ct_mark(CT_S_SLEEP);
        // 拆主屏：连带作废所有指向子控件的全局指针，唤醒时 gotoMainScreen() 会重建
        destroyMainScreen();
    }

    // 菜单重建（在主循环中处理，避免与 LVGL 冲突）
    if (menuNeedsRebuild) {
        menuNeedsRebuild = false;
        ct_mark(CT_S_MENU);
        build_menu();
    }

    // 主显示
    if (currentSysMode == SYS_MODE_NORMAL) {
        ct_mark(CT_S_DYNAMIC);
        updateDynamicElements();
    }

    // HUD 消失
    ct_mark(CT_S_HUD);
    if (hud.active && millis() - hud.startMs > hud.showMs) {
        hud.active = false;
        if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
    }

    // 通知消失（自动移除最老的通知）
    ct_mark(CT_S_NOTIF);
    if (notifCount > 0 && millis() - notifStartMs > notifShowMs) {
        // 移除最老的通知
        for (int i = 0; i < notifCount - 1; i++) {
            notifQueue[i] = notifQueue[i + 1];
            strncpy(notifTexts[i], notifTexts[i + 1], 127);
        }
        notifCount--;
        drawNotifPanel();
    }

    // LVGL
    ct_mark(CT_S_LVGL);
    lvgl_driver_loop();
    delayMicroseconds(500);

    // 完整跑完一轮：心跳 +1，监测任务据此判断有没有卡死
    ct_mark(CT_S_IDLE);
}
