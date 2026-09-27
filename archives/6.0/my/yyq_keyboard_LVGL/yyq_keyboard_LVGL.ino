#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDSystemControl.h"
#include "USBHIDVendor.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <FFat.h>
#include <JPEGDEC.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_Sensor.h>
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <esp_task_wdt.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <sys/time.h>
#include <SPI.h>

// =========================================================================
//                  LVGL 8.3 驱动与显示配置 (替代原 Adafruit_ST7789 + U8g2)
// =========================================================================
#include <lvgl.h>

#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// ST7789 SPI 硬件驱动
SPIClass tftSPI(FSPI);

// LVGL 双绘制缓冲区 (240 x 30 行像素，占用内存小且配合DMA刷新极流畅)
#define DISP_BUF_SIZE (TFT_WIDTH * 30)
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_1[DISP_BUF_SIZE];
static lv_color_t buf_2[DISP_BUF_SIZE];
static lv_disp_drv_t disp_drv;

// ================= 中文字库支持 =================
// LVGL原生支持UTF-8编码。可使用内置字库 lv_font_simsun_16_cjk，
// 或使用 lv_font_conv 生成的高清点阵字库 my_font_cn_16
#if LV_FONT_SIMSUN_16_CJK
  #define FONT_CN &lv_font_simsun_16_cjk
#else
  LV_FONT_DECLARE(my_font_cn_16);
  #define FONT_CN &my_font_cn_16
#endif

// ================= USB HID & 外设对象 =================
USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
USBHIDVendor VendorHID;
Preferences preferences;
Adafruit_MCP23X17 mcp;

#define WDT_TIMEOUT 5
#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20

// SHT31 温湿度传感器
#define SHT31_SDA 17
#define SHT31_SCL 7
#define SHT31_ADDR 0x44
#define SHT31_READ_INTERVAL  900000UL   // 15 分钟
#define SHT31_DBG_INTERVAL   900000UL
#define SHT31_MEASURE_DELAY  20
TwoWire Wire_SHT(1);

#define RX_PIN 10
#define TX_PIN 9
#define UART_BAUD 460800

// ================= 键盘矩阵定义 =================
const int rowPins[] = { 1, 2, 42, 41, 40, 39, 38, 47, 21, 12 };
const int numRows = 10;
const int numCols = 16;
#define DEBOUNCE_DELAY 20
#define PRESSED_VAL LOW

#define MACRO_BASE 0x1000
#define K_M1 (MACRO_BASE + 1)
#define K_M2 (MACRO_BASE + 2)
#define K_M3 (MACRO_BASE + 3)
#define K_M4 (MACRO_BASE + 4)
#define K_M5 (MACRO_BASE + 5)
#define K_M6 (MACRO_BASE + 6)
#define K_M7 (MACRO_BASE + 7)
#define K_M8 (MACRO_BASE + 8)
#define K_M9 (MACRO_BASE + 9)
#define K_M10 (MACRO_BASE + 10)
#define K_M11 (MACRO_BASE + 11)
#define K_M12 (MACRO_BASE + 12)
#define K_MA (MACRO_BASE + 13)
#define K_MB (MACRO_BASE + 14)
#define K_MC (MACRO_BASE + 15)
#define K_MR (MACRO_BASE + 16)
#define K_ME (MACRO_BASE + 17)
#define K_LOGO (MACRO_BASE + 18) // 【LOGO 键】：用于即时切换主屏风格
#define K_NEXT (MACRO_BASE + 19)
#define K_PLAY (MACRO_BASE + 20)
#define K_PREV (MACRO_BASE + 21)
#define K_FN (MACRO_BASE + 22)

bool fnPressed = false;
bool lastState[numRows][numCols] = { false };
unsigned long lastDebounceTime[numRows][numCols] = { 0 };
uint16_t baseMatrix[numRows][numCols] = { 0 };

#define MAX_REMAP_RULES 32
struct RemapRule { uint16_t fromKey; uint16_t toKey; };
#define TOTAL_PROFILES 4
RemapRule profileRemaps[TOTAL_PROFILES][MAX_REMAP_RULES];
int remapCounts[TOTAL_PROFILES] = { 0 };
uint8_t currentProfile = 0;
const char* profileNamesCN[TOTAL_PROFILES] = { "方案1-Windows", "方案2-macOS", "方案3-游戏模式", "方案4-工作模式" };

volatile bool numLockActive = false;
volatile bool capsLockActive = false;
volatile bool scrollLockActive = false;

// ================= 系统运行模式 =================
enum KeyboardSysMode {
  SYS_MODE_NORMAL,
  SYS_MODE_MENU,           // 系统主菜单
  SYS_MODE_LIGHTING_MENU,  // 【新增】：灯光控制子菜单 (原有灯光控制移植至此)
  SYS_MODE_STYLE_PREVIEW,
  SYS_MODE_REC_SEQ,
  SYS_MODE_REC_CMB,
  SYS_MODE_SET_TIME,
  SYS_MODE_SET_ALARM,
  SYS_MODE_SET_TIMER,
  SYS_MODE_CAL_TEMP,
  SYS_MODE_SLEEP
};
KeyboardSysMode currentSysMode = SYS_MODE_NORMAL;

// 主屏风格模式 (按下 LOGO 键时直接顺次切换)
enum ScreenDashboardMode {
  DISP_MODE_BIG_CLOCK = 0,   // 大字时钟
  DISP_MODE_GEEK = 1,        // 极客仪表盘
  DISP_MODE_INFO_PANEL = 2,  // 状态信息面板
  DISP_MODE_KEY_MON = 3,     // 实时击键监控
  DISP_MODE_RHYTHM = 4,      // 律动与频谱
  DISP_MODE_WALLPAPER = 5    // 自定义壁纸
};
const uint8_t TOTAL_DISP_MODES = 6;
uint8_t currentDispMode = DISP_MODE_GEEK;
const char* dispModeNamesCN[TOTAL_DISP_MODES] = {
  "大字时钟", "极客仪表盘", "信息面板", "实时击键监控", "律动与频谱", "自定义壁纸"
};

uint32_t totalKeyCount = 0;
String lastKeyStrokeName = "";
unsigned long keyStrokeDisplayTime = 0;
bool showKeystrokes = true;
bool mediaPlaying = false;

// 温湿度状态
float shtTemperature = 25.0f;
float shtHumidity = 50.0f;
unsigned long lastSHTRead = 0;
bool shtAvailable = false;
float shtTempOffset = 62.0f;

// ================= 灯光控制引擎 (已收纳至设置菜单) =================
#define NUM_MAIN_LEDS 16
#define NUM_IND_LEDS  3
#define TOTAL_LEDS    19
uint8_t frameBuffer[TOTAL_LEDS][3] = { 0 };

bool g_forceOff = false;             // 背光总开关 (菜单控制)
uint8_t brightness = 140;            // 背光亮度 0-255 (菜单控制)
uint8_t currentEffect = 1;           // 背光特效编号 0-12 (菜单控制)
const uint8_t MAX_EFFECTS = 13;
const char* effectNames[] = {
  "关闭", "纯红烈焰", "纯绿荧光", "纯蓝深邃", "冰蓝极光", "纯白恒星",
  "红光呼吸", "绿光呼吸", "蓝光呼吸", "冰蓝呼吸", "流光跑马", "双极彗星", "幻彩流光"
};

#define IND_LEVEL_COUNT 4
const char* indLevelNames[] = { "关", "低", "中", "高" };
const uint8_t indLevelValues[] = { 0, 64, 140, 255 };
uint8_t indLevel = 3;
uint8_t indBrightness = 255;         // 状态指示灯亮度 (菜单控制)

uint8_t keypressStyle = 0;           // 按键动效风格 0-23 (菜单控制)
bool cherryLogoEnabled = false;      // LOGO 按键动效联动 (菜单控制)

// 闹钟与倒计时
bool alarmEnabled = false;
uint8_t alarmHour = 7, alarmMinute = 0;
int alarmLastFiredYday = -1;
bool timerRunning = false;
unsigned long timerStartMs = 0;
uint32_t timerRemainSec = 0, timerTotalSec = 0;
uint8_t timerSetH = 0, timerSetM = 5, timerSetS = 0;

enum RingKind { RING_NONE = 0, RING_ALARM, RING_TIMER };
uint8_t ringingKind = RING_NONE;
unsigned long ringStartMs = 0;
unsigned long pendingRestartMs = 0;
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT_MS = 60000;

// =========================================================================
//                         LVGL UI 控件与视图管理
// =========================================================================
static lv_obj_t* scr_main = NULL;          // 主显示屏根对象
static lv_obj_t* scr_menu = NULL;          // 主系统菜单根对象
static lv_obj_t* scr_lighting = NULL;      // 灯光设置子菜单根对象

// 6个主屏风格的容器对象
static lv_obj_t* view_containers[TOTAL_DISP_MODES] = { NULL };

// 动态更新控件指针
static lv_obj_t* lbl_clock_big = NULL;
static lv_obj_t* lbl_date_big = NULL;
static lv_obj_t* lbl_geek_time = NULL;
static lv_obj_t* lbl_geek_sht = NULL;
static lv_obj_t* lbl_geek_keys = NULL;
static lv_obj_t* lbl_geek_osd = NULL;
static lv_obj_t* bar_sht_temp = NULL;
static lv_obj_t* bar_sht_humi = NULL;

static lv_obj_t* led_lock_num = NULL;
static lv_obj_t* led_lock_caps = NULL;
static lv_obj_t* led_lock_scrl = NULL;

static lv_obj_t* lbl_mon_bigkey = NULL;
static lv_obj_t* lbl_mon_total = NULL;

static lv_obj_t* rhythm_bars[24] = { NULL };
static uint8_t   rhythm_heights[24] = { 0 };

static lv_obj_t* hud_toast = NULL;
static lv_obj_t* hud_toast_title = NULL;
static lv_obj_t* hud_toast_val = NULL;
static lv_timer_t* hud_timer = NULL;

static lv_obj_t* ring_modal = NULL;
static lv_obj_t* ring_modal_title = NULL;
static lv_obj_t* ring_modal_desc = NULL;

// 菜单列表对象
static lv_obj_t* menu_list = NULL;
static lv_obj_t* lighting_list = NULL;
int menuCursor = 0;
int lightingMenuCursor = 0;

// 函数声明
void lvgl_init_driver();
void lvgl_create_ui();
void lvgl_switch_dashboard_style(uint8_t styleIdx);
void lvgl_update_dynamic_data();
void lvgl_show_hud(const char* title, const char* value, lv_color_t color);
void lvgl_show_ringing(uint8_t kind);
void lvgl_hide_ringing();
void lvgl_open_menu();
void lvgl_open_lighting_menu();
void lvgl_close_menu();
void cycleDisplayMode();

// =========================================================================
//                       ST7789 底层 SPI 写入驱动
// =========================================================================
static void inline st7789_write_cmd(uint8_t cmd) {
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  tftSPI.transfer(cmd);
  digitalWrite(TFT_CS, HIGH);
}

static void inline st7789_write_data(uint8_t data) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  tftSPI.transfer(data);
  digitalWrite(TFT_CS, HIGH);
}

static void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  st7789_write_cmd(0x2A); // CASET
  st7789_write_data(x0 >> 8); st7789_write_data(x0 & 0xFF);
  st7789_write_data(x1 >> 8); st7789_write_data(x1 & 0xFF);
  st7789_write_cmd(0x2B); // RASET
  st7789_write_data(y0 >> 8); st7789_write_data(y0 & 0xFF);
  st7789_write_data(y1 >> 8); st7789_write_data(y1 & 0xFF);
  st7789_write_cmd(0x2C); // RAMWR
}

static void st7789_init_hw() {
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_CS, OUTPUT);
  if (TFT_RST >= 0) {
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH); delay(10);
    digitalWrite(TFT_RST, LOW);  delay(20);
    digitalWrite(TFT_RST, HIGH); delay(20);
  }
  digitalWrite(TFT_CS, HIGH);
  tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
  tftSPI.setFrequency(40000000); // 40MHz 极速传输

  st7789_write_cmd(0x01); delay(150); // SWRESET
  st7789_write_cmd(0x11); delay(120); // SLPOUT
  st7789_write_cmd(0x3A); st7789_write_data(0x55); // 16-bit RGB565
  st7789_write_cmd(0x36); st7789_write_data(0x00); // 方向
  st7789_write_cmd(0x21); // INVON
  st7789_write_cmd(0x13); // NORON
  st7789_write_cmd(0x29); delay(50);  // DISPON
}

// LVGL 显示刷新回调 (Flush Callback)
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  uint32_t len = w * h * 2;

  st7789_set_window(area->x1, area->y1, area->x2, area->y2);

  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  tftSPI.writeBytes((uint8_t*)color_p, len);
  digitalWrite(TFT_CS, HIGH);

  lv_disp_flush_ready(disp);
}

// =========================================================================
//                         LVGL UI 构建与页面实现
// =========================================================================
static void hud_timer_cb(lv_timer_t* t) {
  if (hud_toast) {
    lv_obj_add_flag(hud_toast, LV_OBJ_FLAG_HIDDEN);
  }
}

void lvgl_init_driver() {
  lv_init();
  st7789_init_hw();

  lv_disp_draw_buf_init(&draw_buf, buf_1, buf_2, DISP_BUF_SIZE);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = TFT_WIDTH;
  disp_drv.ver_res = TFT_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lvgl_create_ui();
}

void lvgl_create_ui() {
  // 1. 创建主屏根视图
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, lv_color_black(), 0);

  // -------------------------------------------------------------
  // Style 0: 大字时钟 (Big Clock)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_BIG_CLOCK] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_BIG_CLOCK], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_BIG_CLOCK], lv_color_black(), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_BIG_CLOCK], 0, 0);
  lv_obj_set_style_pad_all(view_containers[DISP_MODE_BIG_CLOCK], 0, 0);

  lbl_clock_big = lv_label_create(view_containers[DISP_MODE_BIG_CLOCK]);
  lv_obj_set_style_text_font(lbl_clock_big, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lbl_clock_big, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_align(lbl_clock_big, LV_ALIGN_CENTER, 0, -20);
  lv_label_set_text(lbl_clock_big, "12:00");

  lbl_date_big = lv_label_create(view_containers[DISP_MODE_BIG_CLOCK]);
  lv_obj_set_style_text_font(lbl_date_big, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_date_big, lv_color_white(), 0);
  lv_obj_align(lbl_date_big, LV_ALIGN_CENTER, 0, 30);
  lv_label_set_text(lbl_date_big, "2026-09-27 星期日");

  lv_obj_t* lbl_clk_tip = lv_label_create(view_containers[DISP_MODE_BIG_CLOCK]);
  lv_obj_set_style_text_font(lbl_clk_tip, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_clk_tip, lv_palette_main(LV_PALETTE_AMBER), 0);
  lv_obj_align(lbl_clk_tip, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_label_set_text(lbl_clk_tip, "按 LOGO 键快速切换风格");

  // -------------------------------------------------------------
  // Style 1: 极客仪表盘 (Geek Dashboard)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_GEEK] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_GEEK], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_GEEK], lv_color_black(), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_GEEK], 0, 0);
  lv_obj_set_style_pad_all(view_containers[DISP_MODE_GEEK], 6, 0);

  // 顶栏：当前方案与锁状态
  lv_obj_t* geek_top = lv_obj_create(view_containers[DISP_MODE_GEEK]);
  lv_obj_set_size(geek_top, 228, 30);
  lv_obj_set_style_bg_color(geek_top, lv_color_make(18, 24, 38), 0);
  lv_obj_set_style_border_width(geek_top, 1, 0);
  lv_obj_set_style_border_color(geek_top, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_radius(geek_top, 6, 0);
  lv_obj_set_style_pad_all(geek_top, 4, 0);
  lv_obj_align(geek_top, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* lbl_geek_prof = lv_label_create(geek_top);
  lv_obj_set_style_text_font(lbl_geek_prof, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_geek_prof, lv_palette_main(LV_PALETTE_YELLOW), 0);
  lv_obj_align(lbl_geek_prof, LV_ALIGN_LEFT_MID, 4, 0);
  lv_label_set_text(lbl_geek_prof, profileNamesCN[0]);

  // 三个指示灯胶囊
  led_lock_num = lv_led_create(geek_top);
  lv_obj_set_size(led_lock_num, 14, 14);
  lv_obj_align(led_lock_num, LV_ALIGN_RIGHT_MID, -48, 0);
  lv_led_set_color(led_lock_num, lv_palette_main(LV_PALETTE_GREEN));

  led_lock_caps = lv_led_create(geek_top);
  lv_obj_set_size(led_lock_caps, 14, 14);
  lv_obj_align(led_lock_caps, LV_ALIGN_RIGHT_MID, -26, 0);
  lv_led_set_color(led_lock_caps, lv_palette_main(LV_PALETTE_CYAN));

  led_lock_scrl = lv_led_create(geek_top);
  lv_obj_set_size(led_lock_scrl, 14, 14);
  lv_obj_align(led_lock_scrl, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_led_set_color(led_lock_scrl, lv_palette_main(LV_PALETTE_ORANGE));

  // 中部时钟与击键统计
  lbl_geek_time = lv_label_create(view_containers[DISP_MODE_GEEK]);
  lv_obj_set_style_text_font(lbl_geek_time, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lbl_geek_time, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_align(lbl_geek_time, LV_ALIGN_TOP_MID, 0, 40);
  lv_label_set_text(lbl_geek_time, "12:00:00");

  lbl_geek_keys = lv_label_create(view_containers[DISP_MODE_GEEK]);
  lv_obj_set_style_text_font(lbl_geek_keys, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_geek_keys, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_align(lbl_geek_keys, LV_ALIGN_TOP_MID, 0, 78);
  lv_label_set_text(lbl_geek_keys, "敲击计数: 0 次");

  // 温湿度卡片
  lv_obj_t* sht_card = lv_obj_create(view_containers[DISP_MODE_GEEK]);
  lv_obj_set_size(sht_card, 228, 52);
  lv_obj_set_style_bg_color(sht_card, lv_color_make(20, 24, 30), 0);
  lv_obj_set_style_border_color(sht_card, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_set_style_radius(sht_card, 8, 0);
  lv_obj_align(sht_card, LV_ALIGN_TOP_MID, 0, 108);

  lbl_geek_sht = lv_label_create(sht_card);
  lv_obj_set_style_text_font(lbl_geek_sht, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_geek_sht, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_align(lbl_geek_sht, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_text(lbl_geek_sht, "温湿度: 25.0°C  50.0%");

  bar_sht_temp = lv_bar_create(sht_card);
  lv_obj_set_size(bar_sht_temp, 96, 8);
  lv_obj_align(bar_sht_temp, LV_ALIGN_BOTTOM_LEFT, 0, -2);
  lv_bar_set_range(bar_sht_temp, 0, 50);
  lv_bar_set_value(bar_sht_temp, 25, LV_ANIM_OFF);

  bar_sht_humi = lv_bar_create(sht_card);
  lv_obj_set_size(bar_sht_humi, 96, 8);
  lv_obj_align(bar_sht_humi, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
  lv_bar_set_range(bar_sht_humi, 0, 100);
  lv_bar_set_value(bar_sht_humi, 50, LV_ANIM_OFF);

  // 底部按键实时回显 OSD
  lbl_geek_osd = lv_label_create(view_containers[DISP_MODE_GEEK]);
  lv_obj_set_style_text_font(lbl_geek_osd, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_geek_osd, lv_palette_main(LV_PALETTE_YELLOW), 0);
  lv_obj_align(lbl_geek_osd, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_label_set_text(lbl_geek_osd, "按键回显: 就绪 (按LOGO切屏)");

  // -------------------------------------------------------------
  // Style 2: 信息面板 (Info Panel)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_INFO_PANEL] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_INFO_PANEL], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_INFO_PANEL], lv_color_black(), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_INFO_PANEL], 0, 0);

  lv_obj_t* lbl_info_title = lv_label_create(view_containers[DISP_MODE_INFO_PANEL]);
  lv_obj_set_style_text_font(lbl_info_title, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_info_title, lv_color_white(), 0);
  lv_obj_align(lbl_info_title, LV_ALIGN_TOP_MID, 0, 8);
  lv_label_set_text(lbl_info_title, "键盘硬件状态面板");

  // 三个大圆形指示器
  const char* slot_names[3] = { "CAPS", "NUM", "SCRL" };
  lv_color_t slot_colors[3] = {
    lv_palette_main(LV_PALETTE_CYAN),
    lv_palette_main(LV_PALETTE_GREEN),
    lv_palette_main(LV_PALETTE_AMBER)
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_t* circle = lv_obj_create(view_containers[DISP_MODE_INFO_PANEL]);
    lv_obj_set_size(circle, 60, 60);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_make(25, 30, 40), 0);
    lv_obj_set_style_border_color(circle, slot_colors[i], 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_pos(circle, 15 + i * 75, 45);

    lv_obj_t* clbl = lv_label_create(circle);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clbl, slot_colors[i], 0);
    lv_obj_center(clbl);
    lv_label_set_text(clbl, slot_names[i]);
  }

  // 底部方案与按键信息
  lv_obj_t* lbl_info_bot = lv_label_create(view_containers[DISP_MODE_INFO_PANEL]);
  lv_obj_set_style_text_font(lbl_info_bot, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_info_bot, lv_palette_main(LV_PALETTE_TEAL), 0);
  lv_obj_align(lbl_info_bot, LV_ALIGN_BOTTOM_LEFT, 15, -45);
  lv_label_set_text(lbl_info_bot, "方案: Windows 机械模式");

  lv_obj_t* lbl_info_logo = lv_label_create(view_containers[DISP_MODE_INFO_PANEL]);
  lv_obj_set_style_text_font(lbl_info_logo, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_info_logo, lv_palette_main(LV_PALETTE_PINK), 0);
  lv_obj_align(lbl_info_logo, LV_ALIGN_BOTTOM_LEFT, 15, -15);
  lv_label_set_text(lbl_info_logo, "[LOGO] 键切风格 | [MC] 菜单");

  // -------------------------------------------------------------
  // Style 3: 实时击键监控台 (Key Monitor)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_KEY_MON] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_KEY_MON], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_KEY_MON], lv_color_black(), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_KEY_MON], 0, 0);

  lv_obj_t* mon_card = lv_obj_create(view_containers[DISP_MODE_KEY_MON]);
  lv_obj_set_size(mon_card, 210, 110);
  lv_obj_set_style_bg_color(mon_card, lv_color_make(15, 20, 30), 0);
  lv_obj_set_style_border_color(mon_card, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_set_style_border_width(mon_card, 2, 0);
  lv_obj_set_style_radius(mon_card, 12, 0);
  lv_obj_align(mon_card, LV_ALIGN_TOP_MID, 0, 20);

  lbl_mon_bigkey = lv_label_create(mon_card);
  lv_obj_set_style_text_font(lbl_mon_bigkey, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lbl_mon_bigkey, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_center(lbl_mon_bigkey);
  lv_label_set_text(lbl_mon_bigkey, "READY");

  lbl_mon_total = lv_label_create(view_containers[DISP_MODE_KEY_MON]);
  lv_obj_set_style_text_font(lbl_mon_total, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_mon_total, lv_color_white(), 0);
  lv_obj_align(lbl_mon_total, LV_ALIGN_TOP_MID, 0, 150);
  lv_label_set_text(lbl_mon_total, "累计敲击: 0 次");

  lv_obj_t* lbl_mon_tip = lv_label_create(view_containers[DISP_MODE_KEY_MON]);
  lv_obj_set_style_text_font(lbl_mon_tip, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_mon_tip, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(lbl_mon_tip, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_label_set_text(lbl_mon_tip, "敲击任意键查看毫秒级响应");

  // -------------------------------------------------------------
  // Style 4: 律动与频谱 (Rhythm Visualizer)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_RHYTHM] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_RHYTHM], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_RHYTHM], lv_color_black(), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_RHYTHM], 0, 0);

  lv_obj_t* lbl_rhythm_t = lv_label_create(view_containers[DISP_MODE_RHYTHM]);
  lv_obj_set_style_text_font(lbl_rhythm_t, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_rhythm_t, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_align(lbl_rhythm_t, LV_ALIGN_TOP_MID, 0, 10);
  lv_label_set_text(lbl_rhythm_t, "音频律动与按键掉落");

  // 24 根频谱柱状条
  for (int i = 0; i < 24; i++) {
    rhythm_bars[i] = lv_bar_create(view_containers[DISP_MODE_RHYTHM]);
    lv_obj_set_size(rhythm_bars[i], 6, 120);
    lv_obj_set_pos(rhythm_bars[i], 12 + i * 9, 60);
    lv_bar_set_range(rhythm_bars[i], 0, 100);
    lv_bar_set_value(rhythm_bars[i], 10 + (i % 6) * 12, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rhythm_bars[i], lv_color_make(20, 20, 25), 0);
    lv_obj_set_style_anim_time(rhythm_bars[i], 50, 0);
  }

  lv_obj_t* lbl_rhythm_tip = lv_label_create(view_containers[DISP_MODE_RHYTHM]);
  lv_obj_set_style_text_font(lbl_rhythm_tip, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_rhythm_tip, lv_palette_main(LV_PALETTE_YELLOW), 0);
  lv_obj_align(lbl_rhythm_tip, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_label_set_text(lbl_rhythm_tip, "击键引爆频谱能量！");

  // -------------------------------------------------------------
  // Style 5: 自定义壁纸 (Wallpaper)
  // -------------------------------------------------------------
  view_containers[DISP_MODE_WALLPAPER] = lv_obj_create(scr_main);
  lv_obj_set_size(view_containers[DISP_MODE_WALLPAPER], 240, 240);
  lv_obj_set_style_bg_color(view_containers[DISP_MODE_WALLPAPER], lv_color_make(10, 12, 18), 0);
  lv_obj_set_style_border_width(view_containers[DISP_MODE_WALLPAPER], 0, 0);

  lv_obj_t* wp_box = lv_obj_create(view_containers[DISP_MODE_WALLPAPER]);
  lv_obj_set_size(wp_box, 180, 180);
  lv_obj_center(wp_box);
  lv_obj_set_style_bg_color(wp_box, lv_color_make(25, 30, 45), 0);
  lv_obj_set_style_border_color(wp_box, lv_palette_main(LV_PALETTE_INDIGO), 0);
  lv_obj_set_style_radius(wp_box, 16, 0);

  lv_obj_t* lbl_wp = lv_label_create(wp_box);
  lv_obj_set_style_text_font(lbl_wp, FONT_CN, 0);
  lv_obj_set_style_text_color(lbl_wp, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_center(lbl_wp);
  lv_label_set_text(lbl_wp, "FFat 壁纸展示\n/logo.bin");

  // -------------------------------------------------------------
  // HUD 浮动提示条 (Toast / Notification)
  // -------------------------------------------------------------
  hud_toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(hud_toast, 210, 70);
  lv_obj_align(hud_toast, LV_ALIGN_TOP_MID, 0, 15);
  lv_obj_set_style_bg_color(hud_toast, lv_color_make(10, 15, 25), 0);
  lv_obj_set_style_border_color(hud_toast, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_set_style_border_width(hud_toast, 2, 0);
  lv_obj_set_style_radius(hud_toast, 10, 0);
  lv_obj_add_flag(hud_toast, LV_OBJ_FLAG_HIDDEN);

  hud_toast_title = lv_label_create(hud_toast);
  lv_obj_set_style_text_font(hud_toast_title, FONT_CN, 0);
  lv_obj_set_style_text_color(hud_toast_title, lv_color_white(), 0);
  lv_obj_align(hud_toast_title, LV_ALIGN_TOP_MID, 0, -2);
  lv_label_set_text(hud_toast_title, "提示");

  hud_toast_val = lv_label_create(hud_toast);
  lv_obj_set_style_text_font(hud_toast_val, FONT_CN, 0);
  lv_obj_set_style_text_color(hud_toast_val, lv_palette_main(LV_PALETTE_YELLOW), 0);
  lv_obj_align(hud_toast_val, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_label_set_text(hud_toast_val, "操作已执行");

  // -------------------------------------------------------------
  // 响铃全屏模态弹窗 (Alarm / Timer)
  // -------------------------------------------------------------
  ring_modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(ring_modal, 220, 180);
  lv_obj_center(ring_modal);
  lv_obj_set_style_bg_color(ring_modal, lv_color_make(30, 0, 0), 0);
  lv_obj_set_style_border_color(ring_modal, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_set_style_border_width(ring_modal, 3, 0);
  lv_obj_set_style_radius(ring_modal, 16, 0);
  lv_obj_add_flag(ring_modal, LV_OBJ_FLAG_HIDDEN);

  ring_modal_title = lv_label_create(ring_modal);
  lv_obj_set_style_text_font(ring_modal_title, FONT_CN, 0);
  lv_obj_set_style_text_color(ring_modal_title, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_align(ring_modal_title, LV_ALIGN_TOP_MID, 0, 5);
  lv_label_set_text(ring_modal_title, "⏰ 闹钟时间到！");

  ring_modal_desc = lv_label_create(ring_modal);
  lv_obj_set_style_text_font(ring_modal_desc, FONT_CN, 0);
  lv_obj_set_style_text_color(ring_modal_desc, lv_color_white(), 0);
  lv_obj_align(ring_modal_desc, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(ring_modal_desc, "按回车或灯光键确认");

  // 默认载入配置的风格
  lvgl_switch_dashboard_style(currentDispMode);
}

// =========================================================================
//                  主屏风格即时切换 (按 LOGO 键调用)
// =========================================================================
void lvgl_switch_dashboard_style(uint8_t styleIdx) {
  if (styleIdx >= TOTAL_DISP_MODES) styleIdx = 0;
  for (int i = 0; i < TOTAL_DISP_MODES; i++) {
    if (view_containers[i]) {
      if (i == styleIdx) {
        lv_obj_clear_flag(view_containers[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(view_containers[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

// 供按键扫描调用的公共切屏接口
void cycleDisplayMode() {
  currentDispMode = (currentDispMode + 1) % TOTAL_DISP_MODES;
  preferences.putUChar("disp_mode", currentDispMode);
  lvgl_switch_dashboard_style(currentDispMode);
  lvgl_show_hud("主屏风格切换", dispModeNamesCN[currentDispMode], lv_palette_main(LV_PALETTE_CYAN));
}

// 显示 HUD 悬浮提示
void lvgl_show_hud(const char* title, const char* value, lv_color_t color) {
  if (!hud_toast) return;
  lv_label_set_text(hud_toast_title, title);
  lv_label_set_text(hud_toast_val, value);
  lv_obj_set_style_border_color(hud_toast, color, 0);
  lv_obj_set_style_text_color(hud_toast_val, color, 0);
  lv_obj_clear_flag(hud_toast, LV_OBJ_FLAG_HIDDEN);

  if (hud_timer) lv_timer_del(hud_timer);
  hud_timer = lv_timer_create(hud_timer_cb, 1200, NULL);
  lv_timer_set_repeat_count(hud_timer, 1);
}

// =========================================================================
//                   系统菜单与新增的【灯光控制子菜单】
// =========================================================================
void lvgl_open_menu() {
  if (!scr_menu) {
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr_menu);
    lv_obj_set_style_text_font(title, FONT_CN, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(title, "YYQ 键盘系统 OS 菜单");

    menu_list = lv_list_create(scr_menu);
    lv_obj_set_size(menu_list, 230, 195);
    lv_obj_align(menu_list, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(menu_list, lv_color_make(15, 18, 25), 0);
    lv_obj_set_style_border_color(menu_list, lv_palette_main(LV_PALETTE_BLUE), 0);

    const char* items[] = {
      "1. 返回主屏",
      "2. 切换主屏风格 (或直接按LOGO键)",
      "3. 切换配置方案",
      "4. 按键回显开关",
      "5. 💡 键盘灯光设置中心 (子菜单)",  // 【重点】：原灯光控制整合于此
      "6. 设置系统时间",
      "7. 闹钟设置",
      "8. 倒计时器",
      "9. 立即刷新温湿度",
      "10. SHT31温度校准",
      "11. 敲击计数清零"
    };

    for (int i = 0; i < 11; i++) {
      lv_obj_t* btn = lv_list_add_btn(menu_list, NULL, items[i]);
      lv_obj_set_style_text_font(btn, FONT_CN, 0);
      lv_obj_set_style_bg_color(btn, (i == 4) ? lv_color_make(25, 45, 60) : lv_color_make(20, 24, 34), 0);
    }
  }

  currentSysMode = SYS_MODE_MENU;
  lv_scr_load(scr_menu);
}

// 【新增】：创建并打开灯光控制中心子菜单
void lvgl_open_lighting_menu() {
  if (!scr_lighting) {
    scr_lighting = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_lighting, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr_lighting);
    lv_obj_set_style_text_font(title, FONT_CN, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(title, "💡 键盘灯光与动效设置");

    lighting_list = lv_list_create(scr_lighting);
    lv_obj_set_size(lighting_list, 230, 195);
    lv_obj_align(lighting_list, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(lighting_list, lv_color_make(18, 20, 28), 0);

    const char* l_items[] = {
      "1. 背光总开关 [开/关]",
      "2. 背光亮度调节 (0~255)",
      "3. 键盘背光特效切换 (13种)",
      "4. 状态指示灯亮度 (关/低/中/高)",
      "5. 按键触发动效 (24种效果)",
      "6. Cherry LOGO 动效联动",
      "7. ⬅ 返回上级菜单"
    };

    for (int i = 0; i < 7; i++) {
      lv_obj_t* btn = lv_list_add_btn(lighting_list, NULL, l_items[i]);
      lv_obj_set_style_text_font(btn, FONT_CN, 0);
    }
  }

  currentSysMode = SYS_MODE_LIGHTING_MENU;
  lv_scr_load(scr_lighting);
}

void lvgl_close_menu() {
  currentSysMode = SYS_MODE_NORMAL;
  lv_scr_load(scr_main);
}

// =========================================================================
//                   键盘与外设事件处理 (LOGO 键与按键扫描)
// =========================================================================
void scanKeyboardMatrix() {
  for (int c = 0; c < numCols; c++) {
    mcp.writeGPIOAB(~(1 << c));
    delayMicroseconds(20);

    for (int r = 0; r < numRows; r++) {
      bool currentState = (digitalRead(rowPins[r]) == PRESSED_VAL);
      if (currentState != lastState[r][c]) {
        if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
          lastState[r][c] = currentState;
          lastDebounceTime[r][c] = millis();

          uint16_t baseKey = baseMatrix[r][c];
          if (baseKey == 0) continue;

          if (currentState) {
            lastActivityTime = millis();
            totalKeyCount++;

            // 0. 闹钟/倒计时响铃时，按任意功能键消铃
            if (ringingKind != RING_NONE) {
              if (baseKey == KEY_RETURN || baseKey == KEY_ESC || baseKey == K_MC || baseKey == K_LOGO) {
                ringingKind = RING_NONE;
                if (ring_modal) lv_obj_add_flag(ring_modal, LV_OBJ_FLAG_HIDDEN);
                return;
              }
            }

            // 1. 【核心修改点】：按下 LOGO 键时直接对主屏风格进行切换！
            if (baseKey == K_LOGO) {
              if (fnPressed) {
                // Fn + LOGO：触发重启保护
                lvgl_show_hud("系统控制", "键盘重启中…", lv_palette_main(LV_PALETTE_RED));
                pendingRestartMs = millis() + 600;
              } else {
                // 单击 LOGO 键：即时循环切换 6 种主屏风格
                cycleDisplayMode();
              }
              return;
            }

            // 2. MC 键：打开系统主菜单
            if (baseKey == K_MC) {
              if (currentSysMode == SYS_MODE_NORMAL) {
                lvgl_open_menu();
              } else {
                lvgl_close_menu();
              }
              return;
            }

            // 3. 菜单导航
            if (currentSysMode == SYS_MODE_MENU) {
              if (baseKey == KEY_UP_ARROW) {
                menuCursor = (menuCursor > 0) ? menuCursor - 1 : 10;
              } else if (baseKey == KEY_DOWN_ARROW) {
                menuCursor = (menuCursor < 10) ? menuCursor + 1 : 0;
              } else if (baseKey == KEY_RETURN) {
                if (menuCursor == 0) lvgl_close_menu();
                else if (menuCursor == 1) cycleDisplayMode();
                else if (menuCursor == 4) lvgl_open_lighting_menu(); // 进入灯光子菜单
                else if (menuCursor == 10) { totalKeyCount = 0; preferences.putUInt("keyCount", 0); }
              } else if (baseKey == KEY_ESC) {
                lvgl_close_menu();
              }
              return;
            }

            // 4. 灯光子菜单导航
            if (currentSysMode == SYS_MODE_LIGHTING_MENU) {
              if (baseKey == KEY_UP_ARROW) {
                lightingMenuCursor = (lightingMenuCursor > 0) ? lightingMenuCursor - 1 : 6;
              } else if (baseKey == KEY_DOWN_ARROW) {
                lightingMenuCursor = (lightingMenuCursor < 6) ? lightingMenuCursor + 1 : 0;
              } else if (baseKey == KEY_RETURN) {
                switch (lightingMenuCursor) {
                  case 0: g_forceOff = !g_forceOff; lvgl_show_hud("背光开关", g_forceOff ? "已关闭" : "已开启", lv_palette_main(LV_PALETTE_ORANGE)); break;
                  case 1: brightness = (brightness + 50 > 255) ? 50 : brightness + 50; lvgl_show_hud("背光亮度", String(brightness * 100 / 255).c_str(), lv_palette_main(LV_PALETTE_YELLOW)); break;
                  case 2: currentEffect = (currentEffect + 1) % MAX_EFFECTS; lvgl_show_hud("灯效模式", effectNames[currentEffect], lv_palette_main(LV_PALETTE_GREEN)); break;
                  case 3: indLevel = (indLevel + 1) % IND_LEVEL_COUNT; indBrightness = indLevelValues[indLevel]; lvgl_show_hud("状态灯亮度", indLevelNames[indLevel], lv_palette_main(LV_PALETTE_CYAN)); break;
                  case 4: keypressStyle = (keypressStyle + 1) % 24; lvgl_show_hud("按键动效", String(keypressStyle + 1).c_str(), lv_palette_main(LV_PALETTE_GREEN)); break;
                  case 5: cherryLogoEnabled = !cherryLogoEnabled; lvgl_show_hud("LOGO动效", cherryLogoEnabled ? "开" : "关", lv_palette_main(LV_PALETTE_PINK)); break;
                  case 6: lvgl_open_menu(); break; // 返回主菜单
                }
              } else if (baseKey == KEY_ESC) {
                lvgl_open_menu();
              }
              return;
            }

            // 5. 普通按键处理
            if (baseKey == K_FN) {
              fnPressed = true;
            } else if (baseKey < MACRO_BASE) {
              Keyboard.press((uint8_t)baseKey);
              lastKeyStrokeName = String((char)baseKey);
              if (lbl_mon_bigkey) lv_label_set_text(lbl_mon_bigkey, lastKeyStrokeName.c_str());
            }
          } else {
            if (baseKey == K_FN) fnPressed = false;
            else if (baseKey < MACRO_BASE) Keyboard.release((uint8_t)baseKey);
          }
        }
      }
    }
  }
  mcp.writeGPIOAB(0xFFFF);
}

// 动态数据定时刷新 (时钟、温湿度、锁指示灯、击键)
void lvgl_update_dynamic_data() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 200) return;
  lastUpdate = millis();

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char timeBuf[16];
  if (t && t->tm_year > 120) {
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", t);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "12:00:00");
  }

  if (lbl_clock_big) {
    char shortTime[8];
    snprintf(shortTime, sizeof(shortTime), "%.5s", timeBuf);
    lv_label_set_text(lbl_clock_big, shortTime);
  }
  if (lbl_geek_time) lv_label_set_text(lbl_geek_time, timeBuf);
  if (lbl_geek_keys) {
    char kBuf[32];
    snprintf(kBuf, sizeof(kBuf), "敲击计数: %lu 次", (unsigned long)totalKeyCount);
    lv_label_set_text(lbl_geek_keys, kBuf);
  }
  if (lbl_mon_total) {
    char kBuf[32];
    snprintf(kBuf, sizeof(kBuf), "累计敲击: %lu 次", (unsigned long)totalKeyCount);
    lv_label_set_text(lbl_mon_total, kBuf);
  }

  // 更新锁指示灯
  if (led_lock_num) numLockActive ? lv_led_on(led_lock_num) : lv_led_off(led_lock_num);
  if (led_lock_caps) capsLockActive ? lv_led_on(led_lock_caps) : lv_led_off(led_lock_caps);
  if (led_lock_scrl) scrollLockActive ? lv_led_on(led_lock_scrl) : lv_led_off(led_lock_scrl);

  // 律动柱状图更新
  if (currentDispMode == DISP_MODE_RHYTHM) {
    for (int i = 0; i < 24; i++) {
      if (rhythm_bars[i]) {
        int v = 15 + (rand() % 75);
        lv_bar_set_value(rhythm_bars[i], v, LV_ANIM_ON);
      }
    }
  }
}

// =========================================================================
//                               SETUP & LOOP
// =========================================================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  setenv("TZ", "CST-8", 1);
  tzset();

  USB.VID(0x303A);
  USB.PID(0x001F);
  USB.productName("YYQ-MX9.0 LVGL");
  USB.manufacturerName("YYQ");
  Keyboard.begin();
  ConsumerControl.begin();
  SystemControl.begin();
  VendorHID.begin();
  USB.begin();

  // I2C 矩阵扩展芯片
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  mcp.begin_I2C(MCP23017_ADDR, &Wire);
  for (int c = 0; c < numCols; c++) {
    mcp.pinMode(c, OUTPUT);
    mcp.digitalWrite(c, HIGH);
  }
  for (int r = 0; r < numRows; r++) {
    pinMode(rowPins[r], INPUT_PULLUP);
  }

  preferences.begin("keyboard", false);
  currentDispMode = preferences.getUChar("disp_mode", DISP_MODE_GEEK);
  brightness = preferences.getUChar("brightness", 140);
  currentEffect = preferences.getUChar("effect", 1);
  totalKeyCount = preferences.getUInt("keyCount", 0);

  // 初始化 LVGL 引擎与 ST7789 驱动
  lvgl_init_driver();

  Serial.println("ESP32-S3 机械键盘 LVGL 系统初始化就绪！");
}

void loop() {
  // 1. LVGL 核心时间片引擎
  static unsigned long lastTick = 0;
  unsigned long now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;
  lv_timer_handler();

  // 2. 键盘矩阵高速扫描 (2ms)
  static unsigned long lastScan = 0;
  if (millis() - lastScan >= 2) {
    lastScan = millis();
    scanKeyboardMatrix();
  }

  // 3. UI 动态数据更新
  lvgl_update_dynamic_data();

  // 4. 重启延时
  if (pendingRestartMs != 0 && millis() >= pendingRestartMs) {
    esp_restart();
  }

  delay(2);
}
