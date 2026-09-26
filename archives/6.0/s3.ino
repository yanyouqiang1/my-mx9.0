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

// ================= TFT 屏幕与中文字体库 =================
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>

#ifndef ST77XX_NAVY
#define ST77XX_NAVY       0x000F
#endif
#ifndef ST77XX_DARKGREY
#define ST77XX_DARKGREY   0x39E7
#endif
#ifndef ST77XX_LIGHTGREY
#define ST77XX_LIGHTGREY  0xC618
#endif

#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2;

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
USBHIDVendor VendorHID;   // HID 厂商通道：电脑 -> 键盘 下行发指令，免驱、不占串口
Preferences preferences;
Adafruit_MCP23X17 mcp;

#define WDT_TIMEOUT 5
#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20

// ================= SHT31 温湿度传感器 =================
#define SHT31_SDA 17
#define SHT31_SCL 7
#define SHT31_ADDR 0x44
// 温度校准偏移：模块读数偏低时增大此值
// 深圳 9 月底室外 ~28°C，室内略高 ~30~31°C
// 模块原始读数 -31.8°C，偏移 +62°C 后室内约 30°C
// 如换模块或换环境，重新跑串口打印的 [SHT31] 数据微调
#define SHT31_TEMP_OFFSET 62.0f
// 读取周期：温湿度是缓变量，15 分钟一次够用了；
// 太频繁的话单次测量要 delay 20ms，会让 USB HID 帧延迟 → 键盘发"不灵敏"
#define SHT31_READ_INTERVAL  900000UL   // 15 分钟
#define SHT31_DBG_INTERVAL   900000UL   // 串口诊断打印也按 15 分钟一次
#define SHT31_MEASURE_DELAY  20         // 单次测量等待 20ms（数据手册 15ms 留点余量）
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
#define K_LOGO (MACRO_BASE + 18)
#define K_NEXT (MACRO_BASE + 19)
#define K_PLAY (MACRO_BASE + 20)
#define K_PREV (MACRO_BASE + 21)
#define K_FN (MACRO_BASE + 22)

bool fnPressed = false;
bool lastState[numRows][numCols] = { false };
unsigned long lastDebounceTime[numRows][numCols] = { 0 };
uint16_t baseMatrix[numRows][numCols] = { 0 };

// 按键重映射 Diff
#define MAX_REMAP_RULES 32
struct RemapRule {
  uint16_t fromKey;
  uint16_t toKey;
};
#define TOTAL_PROFILES 4
RemapRule profileRemaps[TOTAL_PROFILES][MAX_REMAP_RULES];
int remapCounts[TOTAL_PROFILES] = { 0 };
uint8_t currentProfile = 0;
const char* profileNamesCN[TOTAL_PROFILES] = { "方案1-Windows", "方案2-macOS", "方案3-游戏模式", "方案4-工作模式" };

// 主机键盘锁指示灯状态
volatile bool numLockActive = false;
volatile bool capsLockActive = false;
volatile bool scrollLockActive = false;
bool lastNumLock = false;
bool lastCapsLock = false;
bool lastScrollLock = false;

// 系统运行状态与屏幕展示模式
enum KeyboardSysMode {
  SYS_MODE_NORMAL,
  SYS_MODE_MENU,
  SYS_MODE_STYLE_PREVIEW,
  SYS_MODE_REC_SEQ,
  SYS_MODE_REC_CMB,
  SYS_MODE_SET_TIME,    // 设置时间（年/月/日/时/分）
  SYS_MODE_SET_ALARM,   // 闹钟设置（时/分/开关）
  SYS_MODE_SET_TIMER,   // 倒计时设置（时/分/秒 + 启停）
  SYS_MODE_SLEEP
};
KeyboardSysMode currentSysMode = SYS_MODE_NORMAL;

// 三个设置子界面是不是同一个东西？是。它们共用一套字段编辑逻辑
// （左右切字段 / 上下和旋钮改值 / 回车保存 / ESC 取消），所以判定收敛到一处，
// 免得每加一个界面就要在 scanKeyboardMatrix、handleC3Events、drawHudOverlay
// 里各补一遍，漏一处就是一个"按键在这个界面下莫名其妙没反应"的 bug。
//
// **故意写成宏，不要"顺手"改成函数。** Arduino 会把自动生成的函数原型插在
// "文件里第一个函数定义"之前。这里一旦变成第一个函数，原型块就会落到下面
// enum AlertType / RingKind / ScreenDashboardMode 的前面，于是
// `AlertType currentAlertType();` 这种原型立刻变成
// "'AlertType' does not name a type"，而且报错行号在文件顶部，极难定位。
// 宏不参与那个边界判定，最稳。
#define IS_SETTING_MODE(m) ((m) == SYS_MODE_SET_TIME || (m) == SYS_MODE_SET_ALARM || (m) == SYS_MODE_SET_TIMER)

// 主屏风格。编号顺序就是菜单和网页下拉框里的顺序：常见的放前面，壁纸垫底。
// 注意：这串数字会被存进 NVS 的 disp_mode，也是网页下拉框和 kbctl*.py 的
// DISP_NAMES 的第三处契约。**改顺序必须同时做两件事**：
//   1) 更新 setup() 里的 DISP_OLD_TO_NEW 迁移表；
//   2) 把 DISP_ORDER_VER 加一。
// 漏掉第 2 步的话迁移不会重新执行，老用户升级完开机风格会静默变成另一个。
enum ScreenDashboardMode {
  DISP_MODE_BIG_CLOCK = 0,   // 大字时钟
  DISP_MODE_GEEK = 1,        // 极客仪表盘
  DISP_MODE_INFO_PANEL = 2,  // 上中下三块：时钟日期 / 三个锁状态实心圆 / 方案图标 + 实时按键
  DISP_MODE_KEY_MON = 3,     // 实时击键监控
  DISP_MODE_RHYTHM = 4,      // 上面律动 + 键帽掉落，下面窄条三块锁状态
  DISP_MODE_WALLPAPER = 5    // 全屏壁纸
};
uint8_t currentDispMode = DISP_MODE_GEEK;
bool screenNeedsRedraw = true; // 仅在模式变更/全屏初始化时为 true

const uint8_t TOTAL_DISP_MODES = 6;
// 风格编号的"表格版本"。6.0 这一版把顺序重排了（时钟提到第一位、壁纸挪到最后），
// NVS 里存量数据还是老编号，靠 setup() 里的迁移表映射一次。
#define DISP_ORDER_VER 2
const char* dispModeNamesCN[TOTAL_DISP_MODES] = {
  "大字时钟", "极客仪表盘", "信息面板", "实时击键监控", "律动", "自定义壁纸"
};
uint8_t previewDispMode = DISP_MODE_GEEK; // 风格预览时临时选中的样式，确认后才写回 currentDispMode

uint32_t totalKeyCount = 0;
uint32_t lastDrawnKeyCount = 0xFFFFFFFF;
String customMarquee = "YYQ Studio - 极客机械大师";
int marqueeScrollX = 240;
unsigned long lastMarqueeUpdate = 0;
unsigned long lastTimeUpdate = 0;
char lastDrawnTimeStr[16] = "";

unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT_MS = 60000;

// 实时按键回显
bool showKeystrokes = true;
String lastKeyStrokeName = "";
unsigned long keyStrokeDisplayTime = 0;
bool keystrokeNeedsRedraw = false;
bool keystrokeActiveOnScreen = false;

// 媒体键只报"我在放/我在停"，主机那边回不回状态我们拿不到，所以本地记一个，
// 按播放/暂停时交替显示"播放""暂停"。
bool mediaPlaying = false;

// ================= SHT31 温湿度传感器状态 =================
// 用 Wire_SHT(1) 直接走 I2C 协议读 SHT31，避开 Adafruit_SHT31 库硬编码 Wire 的限制
float shtTemperature = 0.0f;
float shtHumidity = 0.0f;
unsigned long lastSHTRead = 0;
bool shtAvailable = false;

// 函数定义放到 enum AlertType 之后，避免 Arduino 自动生成原型时跑到 enum 前面
static bool sht31_read_raw(uint16_t &rawT, uint16_t &rawH);
static void sht31_update();

// MR 免驱录制
#define MAX_REC_KEYS 64
uint16_t recKeyBuffer[MAX_REC_KEYS];
int recKeyCount = 0;
// 录制界面整体重画标记。每收一颗键置 true，drawRecUI() 画完置 false。
// 退出录制回到 NORMAL 时也要 false，避免刚回到主屏再被闪一下。
bool recNeedsRedraw = true;

// 灯光引擎
#define NUM_MAIN_LEDS 16
#define NUM_IND_LEDS  3
#define TOTAL_LEDS    19
uint8_t frameBuffer[TOTAL_LEDS][3] = { 0 };

enum ControlMode {
  MODE_CPG,
  MODE_MUTE,
  MODE_LIGHT,
  MODE_SCREEN_BRIGHTNESS,
  MODE_KEY_COLOR
};
ControlMode currentMode = MODE_LIGHT;

uint8_t brightness = 140;
// 键盘右上那三颗状态指示灯（跟 NUM / CAPS / SCRL 一起亮的那排）单独一档亮度，
// 不跟着主背光走：晚上主背光调暗了，锁状态还得看得见。0 = 全灭。
#define IND_LEVEL_COUNT 4
const char* indLevelNames[] = { "关", "低", "中", "高" };
const uint8_t indLevelValues[] = { 0, 64, 140, 255 };
uint8_t indLevel = 3;          // 默认"高"
uint8_t indBrightness = 255;   // 实际写进灯带的 0~255
uint8_t currentEffect = 1;
const uint8_t MAX_EFFECTS = 13;
const char* effectNames[] = {
  "关闭", "纯红烈焰", "纯绿荧光", "纯蓝深邃", "冰蓝极光", "纯白恒星",
  "红光呼吸", "绿光呼吸", "蓝光呼吸", "冰蓝呼吸", "流光跑马", "双极彗星", "幻彩流光"
};

bool g_forceOff = false;
bool cherryLogoEnabled = false;

enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };

// ================= 响铃引擎（闹钟 / 倒计时共用） =================
// 到点了就是"响铃"这个状态：19 颗灯全红快闪 + 屏幕上一张常驻卡片，等用户按下去。
// 闹钟和倒计时只是文案不同，就走同一套，不再各写一份。
enum RingKind {
  RING_NONE = 0,
  RING_ALARM,
  RING_TIMER
};

uint8_t ringingKind = RING_NONE;      // 当前响的是哪种铃
unsigned long ringStartMs = 0;        // 本次响铃开始的时刻，用于 60 秒兜底
bool ringDirty = true;                // 卡片需要重画（起铃、闪烁翻转时置位）

// Fn+LOGO 的重启是延后执行的：直接 esp_restart() 的话屏幕来不及画出任何东西，
// 用户只会看到"黑了一下"。先弹提示，600ms 后由 loop 真正重启。
unsigned long pendingRestartMs = 0;

// ================= 闹钟 / 倒计时 / 时钟持久化 =================
bool alarmEnabled = false;
uint8_t alarmHour = 7;
uint8_t alarmMinute = 0;
int alarmLastFiredYday = -1;          // 已经响过的那一天（tm_yday），保证一天只响一次

bool timerRunning = false;
// 存"开始时刻"而不是"截止时刻"：剩余量用 now - timerStartMs 算，
// 无符号减法的回绕特性（millis() 每 49.7 天归零一次）会让差值天然正确；
// 而存截止时刻再写 `end > now` 的话，回绕那一刻会判断反。
unsigned long timerStartMs = 0;
uint32_t timerRemainSec = 0;          // 最近一次算出来的剩余秒，供设置界面显示
uint32_t timerTotalSec = 0;           // 本次倒计时的总时长，响铃卡片上回显用
uint8_t timerSetH = 0, timerSetM = 5, timerSetS = 0;   // 上次设的时长，进菜单能看见

// ================= 系统通知队列 =================
// 主机下发的通知按到达顺序入队。屏幕和灯光都只展示"最新的一条"，
// 用户每按一次灯光键处理掉最新的一条，前一条顶上来继续展示，直到清空。
#define MAX_NOTIFS 8
#define NOTIF_TEXT_LEN 128

struct Notification {
  AlertType type;
  char text[NOTIF_TEXT_LEN];
};

Notification notifQueue[MAX_NOTIFS];
uint8_t notifCount = 0;
bool notifDirty = true; // 通知条内容变了，下一轮重绘一次

// ================= SHT31 温湿度读取实现 =================
// 放在 enum AlertType 之后，避开 Arduino 自动生成原型插到 enum 之前的坑

// SHT31 CRC-8 多项式 0x31 (x^8 + x^5 + x^4 + 1)，初始值 0xFF
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

// 单次测量 + CRC 校验。每 15 分钟一次，阻塞 SHT31_MEASURE_DELAY (~20ms)，
// 对 USB HID 帧延迟几乎不可见。CRC 不通过直接放弃，留上次有效值。
static bool sht31_read_raw(uint16_t &rawT, uint16_t &rawH) {
  Wire_SHT.beginTransmission(SHT31_ADDR);
  Wire_SHT.write(0x2C);
  Wire_SHT.write(0x06);
  if (Wire_SHT.endTransmission() != 0) return false;
  delay(SHT31_MEASURE_DELAY);
  if (Wire_SHT.requestFrom((int)SHT31_ADDR, 6) != 6) return false;
  uint8_t buf[6];
  for (int k = 0; k < 6; k++) buf[k] = Wire_SHT.read();
  // CRC 校验：温度 2 字节 + 1 字节 CRC，湿度同理
  if (sht31_crc8(&buf[0], 2) != buf[2]) return false;
  if (sht31_crc8(&buf[3], 2) != buf[5]) return false;
  rawT = ((uint16_t)buf[0] << 8) | buf[1];
  rawH = ((uint16_t)buf[3] << 8) | buf[4];
  return true;
}

static void sht31_update() {
  uint16_t rawT, rawH;
  if (!sht31_read_raw(rawT, rawH)) return;
  float t = -45.0f + 175.0f * ((float)rawT / 65535.0f);
  float h = 100.0f * ((float)rawH / 65535.0f);
  // 串口输出原始数据，便于诊断模块是否真的有偏差
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > SHT31_DBG_INTERVAL) {
    lastDbg = millis();
    Serial.printf("[SHT31] rawT=0x%04X rawH=0x%04X  T=%.1fC  H=%.1f%%  (offset=%.1f)\n",
                  rawT, rawH, t, h, SHT31_TEMP_OFFSET);
  }
  // 合理范围检查：温度 -40 ~ 80°C，湿度 0 ~ 100%
  // 超出范围就丢弃，保留上一次的值
  if (t < -40.0f || t > 80.0f || h < 0.0f || h > 100.0f) return;
  shtTemperature = t + SHT31_TEMP_OFFSET;
  shtHumidity    = h;
}

// 当前生效的报警类型永远取队尾（最新）那条，灯和屏共用一个来源，不会各说各话
AlertType currentAlertType() {
  return (notifCount > 0) ? notifQueue[notifCount - 1].type : ALERT_NONE;
}

uint16_t alertColor(AlertType t) {
  switch (t) {
    case ALERT_RED:    return ST77XX_RED;
    case ALERT_GREEN:  return ST77XX_GREEN;
    case ALERT_YELLOW: return ST77XX_YELLOW;
    default:           return ST77XX_CYAN;
  }
}

const char* notifDefaultText(AlertType t) {
  switch (t) {
    case ALERT_RED:    return "系统报警通知";
    case ALERT_GREEN:  return "系统提示通知";
    case ALERT_YELLOW: return "系统警告通知";
    default:           return "系统通知";
  }
}

bool isReactionActive = false;
int reactionStep = 0;
unsigned long lastReactionUpdate = 0;
uint32_t currentReactionColor = 0;
uint8_t keypressStyle = 0;
uint8_t reactionType = 0;
int stackCount = 0;

#define MAX_SHOOT_PROJECTILES 8
int shootSteps[MAX_SHOOT_PROJECTILES] = { -1, -1, -1, -1, -1, -1, -1, -1 };
uint32_t shootColors[MAX_SHOOT_PROJECTILES];
const uint32_t keypressColors[] = {
  0x000000, 0xFF0000, 0x00FF00, 0x0000FF,
  0x00FFFF, 0xB400FF, 0xFFFF00, 0xFFFFFF
};

bool c3Connected = false;
unsigned long lastPingTime = 0;
unsigned long lastLedFrameTime = 0;
uint16_t effectFrame = 0;

// HUD 浮动提示条
struct HudMessage {
  bool active = false;
  bool dirty = false;
  unsigned long triggerTime = 0;
  char title[24];
  char value[24];
  int percent = -1;
  uint16_t color = ST77XX_CYAN;
};
HudMessage hud;

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

void displayStatusCN(const char* title, uint16_t color = ST77XX_GREEN);
void switchProfile(uint8_t profIdx);
void drawMenuUI();
void drawRecUI();
void renderCurrentDisplayBase();
bool updateDynamicElements();
void renderWallpaperView(bool drawOverlayTime = true);
void triggerHud(const char* title, const char* value, int percent, uint16_t color);
void renderStylePreview();
void applyStylePreview();
void cancelStylePreview();
void handleCommand(String data);
void handleLogoChunk(const uint8_t* data, size_t len);
void finishLogoUpload();
void renderInfoPanelBase();
void renderRhythmBase();
bool updateRhythmAnimation();
void rhythmNotifyKey(const String& name);
void menuMove(int delta);
void renderTimeSetUI();
void renderAlarmSetUI();
void renderTimerSetUI();
void drawRingOverlay();
void updateTimers();
void persistClock();
void startRinging(uint8_t kind);
void stopRinging();

// ================= 壁纸上传：浏览器端压好的 JPEG =================
// 网页端在本地就把任意大小的原图缩到 240x240 并压成 JPEG（十几 KB），
// 固件收齐后解码成 RGB565 落盘。落盘格式和以前逐字节一致，
// renderWallpaperView 和整条显示路径一行都不用动。
#define LOGO_RX_MAX        (256 * 1024)  // 单次上传的 JPEG 上限，防着乱来
#define LOGO_RX_TIMEOUT_MS 5000          // 传一半断线（关网页等）：这么久没动静就放弃

uint8_t*  logoRxBuf   = NULL;        // 本次上传的 JPEG 原始数据；非空 == 正在二进制接收模式
uint32_t  logoRxTotal = 0;           // 网页声明的总长度
uint32_t  logoRxGot   = 0;           // 已收到的字节数
unsigned long logoRxLastMs = 0;      // 最后一包到达时间，用于断线兜底
bool      logoRxDone  = false;       // 收齐了，交给 loop 去解码落盘

// ================= 系统通知：入队 / 出队 / 上屏 =================
void pushNotification(AlertType type, const String& text) {
  if (type == ALERT_NONE) return;

  if (notifCount >= MAX_NOTIFS) {
    // 队列满了就丢掉最旧的一条，把位置让给刚到的（新的更要紧）
    for (uint8_t i = 0; i + 1 < MAX_NOTIFS; i++) notifQueue[i] = notifQueue[i + 1];
    notifCount = MAX_NOTIFS - 1;
  }

  Notification& n = notifQueue[notifCount];
  n.type = type;

  const char* src = (text.length() > 0) ? text.c_str() : notifDefaultText(type);
  uint8_t len = 0;
  while (src[len] != '\0' && len < NOTIF_TEXT_LEN - 1) len++;
  while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--; // 别把汉字截成半个
  memcpy(n.text, src, len);
  n.text[len] = '\0';

  notifCount++;
  notifDirty = true;

  // 通知是给人看的：屏幕正在省电息屏的话，先唤醒回主屏
  lastActivityTime = millis();
  if (currentSysMode == SYS_MODE_SLEEP) {
    currentSysMode = SYS_MODE_NORMAL;
    screenNeedsRedraw = true;
  }
}

// 队列变化后让屏幕跟上。通知条是一整块不透明的卡片，擦除时没法局部还原
// 它盖掉的底图，所以"整条要抹掉"的场景直接标记重画底板。
void refreshNotifScreen(bool erasePanel) {
  notifDirty = true;
  if (currentSysMode == SYS_MODE_MENU) drawMenuUI();          // 菜单只刷右上角角标
  else if (currentSysMode == SYS_MODE_STYLE_PREVIEW) return;  // 预览界面不挂通知条
  else if (erasePanel) screenNeedsRedraw = true;
}

void clearAllNotifications() {
  if (notifCount == 0) return;
  notifCount = 0;
  refreshNotifScreen(true);
  triggerHud("通知已清空", "全部清除", -1, ST77XX_CYAN);
}

// 用户"我已知晓"：单击灯光键调用。只处理掉最新的一条，前一条顶上来继续
// 展示（灯和屏一起跟着换），全清完之后灯自然就不闪了。
bool acknowledgeAlert() {
  if (notifCount == 0) return false;

  notifCount--;
  refreshNotifScreen(notifCount == 0);

  if (notifCount == 0) {
    triggerHud("通知已清空", "已全部处理", -1, ST77XX_GREEN);
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "还剩 %d 条", notifCount);
    triggerHud("已确认", buf, -1, ST77XX_GREEN);
  }
  return true;
}

// UTF-8 字符占几个字节
static int utf8CharLen(const char* s, int off) {
  unsigned char c = (unsigned char)s[off];
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1; // 非法首字节，按单字节吃掉，保证不死循环
}

// 按像素宽度把一段中文折成最多两行，第二行也放不下就用 … 收尾。
// 调用前必须先把字体设成 wqy16，否则量出来的宽度是错的。
void layoutNotifText(const char* src, char* l1, size_t s1, char* l2, size_t s2, int maxW) {
  l1[0] = '\0';
  l2[0] = '\0';
  if (src == NULL || src[0] == '\0') return;

  char buf[NOTIF_TEXT_LEN + 8];
  char probe[NOTIF_TEXT_LEN + 16];
  int n = 0;
  bool onSecondLine = false;
  int total = (int)strlen(src);

  for (int off = 0; off < total;) {
    int cl = utf8CharLen(src, off);
    if (off + cl > total) cl = total - off;

    memcpy(probe, buf, n);
    memcpy(probe + n, src + off, cl);
    probe[n + cl] = '\0';

    if (n > 0 && u8g2.getUTF8Width(probe) > maxW) {
      if (!onSecondLine) {
        strncpy(l1, buf, s1 - 1);
        l1[s1 - 1] = '\0';
        onSecondLine = true;
        n = 0;
        buf[0] = '\0';
        continue; // 同一个字挪到第二行再判一次
      }
      strncpy(l2, buf, s2 - 1);
      l2[s2 - 1] = '\0';
      if (strlen(l2) + 4 <= s2) strcat(l2, "…");
      return;
    }

    memcpy(buf + n, src + off, cl);
    n += cl;
    buf[n] = '\0';
    off += cl;
  }

  if (!onSecondLine) {
    strncpy(l1, buf, s1 - 1);
    l1[s1 - 1] = '\0';
  } else {
    strncpy(l2, buf, s2 - 1);
    l2[s2 - 1] = '\0';
  }
}

// 主屏底部的通知条：常驻显示、不自动消失，直到用户按灯光键逐条确认
void drawNotifPanel() {
  notifDirty = false;
  if (notifCount == 0) return;

  const Notification& n = notifQueue[notifCount - 1];
  uint16_t col = alertColor(n.type);

  // 字体放大到 wqy16 后行距要 21px 才不挤，面板整体上移一点把空间匀出来。
  // 底边仍是 238，和原来一样贴到屏幕下沿，不动其它风格的布局。
  const int px = 6, py = 164, pw = 228, ph = 74;
  tft.fillRoundRect(px, py, pw, ph, 8, ST77XX_BLACK);
  tft.drawRoundRect(px, py, pw, ph, 8, col);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);

  char l1[72], l2[72];
  layoutNotifText(n.text, l1, sizeof(l1), l2, sizeof(l2), pw - 26 - 16);

  tft.fillCircle(px + 14, py + 19, 5, col);

  u8g2.setForegroundColor(col);
  u8g2.setCursor(px + 26, py + 24);
  u8g2.print(l1);

  if (l2[0] != '\0') {
    u8g2.setForegroundColor(ST77XX_WHITE);
    u8g2.setCursor(px + 26, py + 46);
    u8g2.print(l2);
  }

  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  u8g2.setCursor(px + 14, py + 68);
  u8g2.print("单击灯光键确认");

  char cnt[24];
  snprintf(cnt, sizeof(cnt), "共%d条", notifCount);
  u8g2.setForegroundColor(ST77XX_YELLOW);
  u8g2.setCursor(px + pw - 14 - u8g2.getUTF8Width(cnt), py + 68);
  u8g2.print(cnt);
}

// 菜单界面不挂整条通知条（会盖住菜单项），只在标题栏右上角亮个条数
void drawNotifBadge() {
  if (notifCount == 0) return;

  char buf[16];
  snprintf(buf, sizeof(buf), "%d", notifCount);

  tft.fillRoundRect(192, 6, 40, 20, 6, ST77XX_RED);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  u8g2.setCursor(192 + (40 - u8g2.getUTF8Width(buf)) / 2, 22);
  u8g2.print(buf);
}

// ================= 色彩与灯效辅助 =================
void setLedRGB(int index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= 0 && index < TOTAL_LEDS) {
    frameBuffer[index][0] = r;
    frameBuffer[index][1] = g;
    frameBuffer[index][2] = b;
  }
}
void setMainLedsColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, r, g, b);
}
void clearMainLeds() { setMainLedsColor(0, 0, 0); }

uint32_t colorHSV(uint16_t hue, uint8_t sat, uint8_t val) {
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

static void usbHidKeyboardEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
    arduino_usb_hid_keyboard_event_data_t *led_data = (arduino_usb_hid_keyboard_event_data_t *)data;
    numLockActive = led_data->numlock;
    capsLockActive = led_data->capslock;
    scrollLockActive = led_data->scrolllock;
  }
}

void safeDelayMs(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    delay(1);
  }
}

// ================= HID 厂商通道：电脑 -> 键盘 的指令下行 =================
// 主机往 Report ID 6 的 Output/Feature 报告里写文本（以 \n 结尾），
// 中断回调只负责落进环形缓冲，真正的解析放到主循环，避免占用 USB 事件任务栈。
#define HID_RX_BUF_SIZE 512
static volatile uint8_t hidRxBuf[HID_RX_BUF_SIZE];
static volatile uint16_t hidRxHead = 0;
static volatile uint16_t hidRxTail = 0;

static inline void hidRxPush(char c) {
  uint16_t next = (uint16_t)((hidRxHead + 1) % HID_RX_BUF_SIZE);
  if (next != hidRxTail) { // 满了就丢，不阻塞 USB
    hidRxBuf[hidRxHead] = (uint8_t)c;
    hidRxHead = next;
  }
}

static void onHidVendorEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id != ARDUINO_USB_HID_VENDOR_OUTPUT_EVENT && id != ARDUINO_USB_HID_VENDOR_SET_FEATURE_EVENT) return;
  const arduino_usb_hid_vendor_event_data_t *p = (const arduino_usb_hid_vendor_event_data_t *)data;
  if (p == NULL || p->buffer == NULL) return;
  for (uint16_t i = 0; i < p->len; i++) {
    char c = (char)p->buffer[i];
    if (c != '\0') hidRxPush(c); // 报告尾部补的零要丢掉
  }
}

void handleHidVendorCommands() {
  static String buf = "";
  while (hidRxTail != hidRxHead) {
    char c = (char)hidRxBuf[hidRxTail];
    hidRxTail = (uint16_t)((hidRxTail + 1) % HID_RX_BUF_SIZE);
    if (c == '\n' || c == '\r') {
      buf.trim();
      if (buf.length() > 0) handleCommand(buf);
      buf = "";
    } else if (buf.length() < 200) {
      buf += c;
    } else {
      buf = ""; // 超长直接丢弃，防止野数据撑爆内存
    }
  }
}

// ================= 重映射管理 =================
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

void saveProfileRemap(uint8_t prof, uint16_t fromK, uint16_t toK) {
  if (prof >= TOTAL_PROFILES) return;
  for (int i = 0; i < remapCounts[prof]; i++) {
    if (profileRemaps[prof][i].fromKey == fromK) {
      profileRemaps[prof][i].toKey = toK;
      char itemKey[20];
      snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
      preferences.putUInt(itemKey, ((uint32_t)fromK << 16) | toK);
      return;
    }
  }
  if (remapCounts[prof] < MAX_REMAP_RULES) {
    int idx = remapCounts[prof]++;
    profileRemaps[prof][idx].fromKey = fromK;
    profileRemaps[prof][idx].toKey = toK;
    char itemKey[20];
    snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, idx);
    preferences.putUInt(itemKey, ((uint32_t)fromK << 16) | toK);
    char cntKey[16];
    snprintf(cntKey, sizeof(cntKey), "rmp_cnt_%d", prof);
    preferences.putInt(cntKey, remapCounts[prof]);
  }
}

void clearProfileRemap(uint8_t prof) {
  if (prof >= TOTAL_PROFILES) return;
  remapCounts[prof] = 0;
  char cntKey[16];
  snprintf(cntKey, sizeof(cntKey), "rmp_cnt_%d", prof);
  preferences.putInt(cntKey, 0);
}

uint16_t getMappedKey(uint16_t originalKey) {
  for (int i = 0; i < remapCounts[currentProfile]; i++) {
    if (profileRemaps[currentProfile][i].fromKey == originalKey) {
      return profileRemaps[currentProfile][i].toKey;
    }
  }
  return originalKey;
}

// ================= 壁纸上传：接收 / 解码 / 落盘 =================
// 落盘格式沿用老规矩：每像素 2 字节、高位在前。故意不换成"更自然"的小端，
// 因为 renderWallpaperView 是按这个字节序读的，换了两边就错位成雪花。
static uint8_t* logoOutBuf = NULL;
// 放文件作用域而不是函数里的局部变量：JPEGDEC 实例本身有几百字节，
// 而且它内部解码还要吃栈，别去挤 loop 任务那 8KB 的栈
static JPEGDEC logoJpeg;

static int logoJpegDraw(JPEGDRAW* pDraw) {
  if (logoOutBuf == NULL) return 0;
  const uint16_t* src = (const uint16_t*)pDraw->pPixels;

  for (int row = 0; row < pDraw->iHeight; row++) {
    int dy = pDraw->y + row;
    if (dy < 0 || dy >= 240) continue;

    const uint16_t* srow = src + row * pDraw->iWidth;
    uint8_t* drow = logoOutBuf + (uint32_t)dy * 240 * 2;

    for (int col = 0; col < pDraw->iWidth; col++) {
      int dx = pDraw->x + col;
      if (dx < 0 || dx >= 240) continue;   // 尺寸不规整时 JPEG 会补到 MCU 边界，裁掉即可
      uint16_t v = srow[col];
      drow[dx * 2]     = (uint8_t)(v >> 8);
      drow[dx * 2 + 1] = (uint8_t)(v & 0xFF);
    }
  }
  return 1;
}

static void abortLogoUpload(const char* reason) {
  if (logoRxBuf) { free(logoRxBuf); logoRxBuf = NULL; }
  logoRxTotal = 0;
  logoRxGot = 0;
  logoRxDone = false;
  if (reason) triggerHud("壁纸传输", reason, -1, ST77XX_RED);
}

// 二进制接收。这段数据是 JPEG 的原始字节，里面必然出现 0x00，
// 所以只能按长度整段取，绝不能当字符串走 c_str()（会在第一个 0 处截断）。
void handleLogoChunk(const uint8_t* data, size_t len) {
  if (logoRxBuf == NULL) return;
  logoRxLastMs = millis();

  uint32_t room = logoRxTotal - logoRxGot;
  if (len > room) len = room;   // 多出来的丢掉，只认网页声明过的长度
  if (len == 0) return;

  memcpy(logoRxBuf + logoRxGot, data, len);
  logoRxGot += len;

  int pct = (int)((uint64_t)logoRxGot * 100 / logoRxTotal);
  char buf[28];
  snprintf(buf, sizeof(buf), "%u/%u KB", (unsigned)(logoRxGot / 1024), (unsigned)(logoRxTotal / 1024));
  // 每包都刷一次 HUD，顺便给卡片续命，免得传大图传到一半提示自己超时消失了
  triggerHud("壁纸传输", buf, pct, ST77XX_YELLOW);

  if (logoRxGot >= logoRxTotal) logoRxDone = true;
}

// 收齐后的收尾：解码 JPEG -> RGB565 -> 覆盖 /logo.bin。
// 解码加重写盘要几百毫秒，所以放在 loop 里跑，不占着 BLE 回调线程。
void finishLogoUpload() {
  logoRxDone = false;

  uint8_t* out = (uint8_t*)malloc(240 * 240 * 2);
  if (out == NULL) { abortLogoUpload("内存不足"); return; }

  logoOutBuf = out;
  memset(out, 0, 240 * 240 * 2);   // 先铺黑：万一 JPEG 没盖满 240x240，也不会留上一张的残影

  bool ok = false;
  if (logoJpeg.openRAM(logoRxBuf, (int)logoRxTotal, logoJpegDraw)) {
    logoJpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    ok = (logoJpeg.decode(0, 0, 0) != 0);
    logoJpeg.close();
  }
  logoOutBuf = NULL;

  if (ok) {
    File f = FFat.open("/logo.bin", FILE_WRITE);
    if (f) { f.write(out, 240 * 240 * 2); f.close(); }
    else   { ok = false; }
  }

  free(out);
  free(logoRxBuf);
  logoRxBuf = NULL;
  logoRxTotal = 0;
  logoRxGot = 0;

  if (ok) {
    triggerHud("壁纸更新", "上传完成", -1, ST77XX_GREEN);
    lastActivityTime = millis();
    screenNeedsRedraw = true;   // 壁纸模式下立刻换成新图
  } else {
    triggerHud("壁纸传输", "解码失败", -1, ST77XX_RED);
  }
}

// ================= 指令控制中心 =================
void handleCommand(String data) {
  data.trim();
  if (data.length() == 0) return;

  // 通知指令：
  //   ALERT:RED              -> 入队一条红灯报警，正文用默认名
  //   ALERT:RED:磁盘空间不足  -> 入队一条红灯报警，正文自定义（可含冒号）
  //   NOTIFY:开会了           -> 快捷写法，等同 ALERT:GREEN:开会了
  //   ALERT:OFF / CLEAR      -> 一次性清空整个队列
  bool isNotify = data.startsWith("NOTIFY:");
  if (data.startsWith("ALERT:") || isNotify) {
    String rest = data.substring(isNotify ? 7 : 6);
    rest.trim();

    // 整体就是 OFF/STOP/CLEAR/NONE 才算"全清"，避免把 NOTIFY:OFFLINE 这类正文误判
    if (rest == "OFF" || rest == "STOP" || rest == "CLEAR" || rest == "NONE") {
      clearAllNotifications();
    } else if (isNotify) {
      pushNotification(ALERT_GREEN, rest);
    } else {
      int sep = rest.indexOf(':'); // 只切第一个冒号，正文里的冒号原样保留
      String typeTok = (sep < 0) ? rest : rest.substring(0, sep);
      String body = (sep < 0) ? String("") : rest.substring(sep + 1);
      typeTok.trim();
      body.trim();
      typeTok.toUpperCase(); // 只对 ASCII 的类型段做，正文不碰

      if (typeTok == "RED" || typeTok == "R") pushNotification(ALERT_RED, body);
      else if (typeTok == "GREEN" || typeTok == "G") pushNotification(ALERT_GREEN, body);
      else if (typeTok == "YELLOW" || typeTok == "Y") pushNotification(ALERT_YELLOW, body);
      // 类型不认识就当误码忽略，别把 "ALERT:XXX" 当成正文弹出来
    }
  }
  else if (data.startsWith("DISP_MODE:")) {
    currentDispMode = (uint8_t)data.substring(10).toInt();
    if (currentDispMode >= TOTAL_DISP_MODES) currentDispMode = 0;
    preferences.putUChar("disp_mode", currentDispMode);
    screenNeedsRedraw = true;
  }
  else if (data.startsWith("SET_KEYSTROKE:")) {
    showKeystrokes = (data.substring(14).toInt() == 1);
    preferences.putBool("show_keys", showKeystrokes);
    triggerHud("按键回显", showKeystrokes ? "已开启" : "已关闭", -1, ST77XX_MAGENTA);
  }
  else if (data.startsWith("TIME:")) {
    time_t t = data.substring(5).toInt();
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    triggerHud("时间同步", "校准完成", -1, ST77XX_GREEN);
    lastDrawnTimeStr[0] = '\0';
    lastDrawnKeyCount = 0xFFFFFFFF;
    persistClock();          // 同步来的时间也存一份，下次拔电重上电不至于回到 1970
  }
  else if (data.startsWith("ALARMSET:")) {
    String v = data.substring(9);
    v.trim();
    String vUp = v;
    vUp.toUpperCase();

    if (vUp == "OFF" || vUp == "NONE") {
      alarmEnabled = false;
      preferences.putBool("alarm_on", false);
      triggerHud("闹钟设置", "已关闭", -1, ST77XX_LIGHTGREY);
    } else {
      int sep = v.indexOf(':');
      int hh = (sep > 0) ? v.substring(0, sep).toInt() : -1;
      int mm = (sep > 0) ? v.substring(sep + 1).toInt() : -1;
      if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
        alarmEnabled = true;
        alarmHour = (uint8_t)hh;
        alarmMinute = (uint8_t)mm;
        alarmLastFiredYday = -1;
        preferences.putBool("alarm_on", true);
        preferences.putUChar("alarm_h", alarmHour);
        preferences.putUChar("alarm_m", alarmMinute);

        char buf[24];
        snprintf(buf, sizeof(buf), "%02u:%02u", alarmHour, alarmMinute);
        triggerHud("闹钟设置", buf, -1, ST77XX_GREEN);
      }
      // 格式不认识就当误码忽略，别弹个红卡片吓人
    }
  }
  else if (data.startsWith("TIMERSET:")) {
    String v = data.substring(9);
    v.trim();
    String vUp = v;
    vUp.toUpperCase();

    if (vUp == "STOP" || vUp == "OFF" || vUp == "CANCEL") {
      timerRunning = false;
      timerRemainSec = 0;
      triggerHud("倒计时", "已停止", -1, ST77XX_RED);
    } else {
      // 三种写法都收：纯秒数 / MM:SS / HH:MM:SS
      int p1 = v.indexOf(':');
      uint32_t total = 0;
      if (p1 < 0) {
        total = (uint32_t)v.toInt();
      } else {
        int p2 = v.indexOf(':', p1 + 1);
        if (p2 < 0) {
          total = (uint32_t)v.substring(0, p1).toInt() * 60UL + (uint32_t)v.substring(p1 + 1).toInt();
        } else {
          total = (uint32_t)v.substring(0, p1).toInt() * 3600UL
                + (uint32_t)v.substring(p1 + 1, p2).toInt() * 60UL
                + (uint32_t)v.substring(p2 + 1).toInt();
        }
      }
      if (total == 0) {
        triggerHud("倒计时", "时长不合法", -1, ST77XX_RED);
      } else {
        if (total > 86400UL) total = 86400UL;   // 24 小时封顶，免得算进 uint8 里溢出

        timerSetH = (uint8_t)(total / 3600);
        timerSetM = (uint8_t)((total / 60) % 60);
        timerSetS = (uint8_t)(total % 60);
        preferences.putUChar("tmr_h", timerSetH);
        preferences.putUChar("tmr_m", timerSetM);
        preferences.putUChar("tmr_s", timerSetS);

        timerTotalSec = total;
        timerRemainSec = total;
        timerStartMs = millis();
        timerRunning = true;

        char buf[24];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", timerSetH, timerSetM, timerSetS);
        triggerHud("倒计时开始", buf, -1, ST77XX_GREEN);
      }
    }
  }
  else if (data.startsWith("MARQUEE:")) {
    customMarquee = data.substring(8);
    preferences.putString("marquee", customMarquee);
    triggerHud("标语更新", "保存成功", -1, ST77XX_CYAN);
  }
  else if (data.startsWith("REMAP:")) {
    int p1 = data.indexOf(':', 6);
    int p2 = data.indexOf(':', p1 + 1);
    if (p1 > 0 && p2 > 0) {
      uint8_t prof = data.substring(6, p1).toInt();
      bool doClear = (data.substring(p1 + 1, p2).toInt() == 1);
      if (doClear) clearProfileRemap(prof);

      String pairs = data.substring(p2 + 1);
      while (pairs.length() > 0) {
        int semi = pairs.indexOf(';');
        String pair = (semi == -1) ? pairs : pairs.substring(0, semi);
        int comma = pair.indexOf(',');
        if (comma > 0) {
          uint16_t fK = pair.substring(0, comma).toInt();
          uint16_t tK = pair.substring(comma + 1).toInt();
          saveProfileRemap(prof, fK, tK);
        }
        if (semi == -1) break;
        pairs = pairs.substring(semi + 1);
      }
      triggerHud("按键映射", "保存成功", -1, ST77XX_GREEN);
    }
  }
  else if (data.startsWith("GSET:")) {
    int p1 = data.indexOf(':', 5);
    if (p1 > 0) {
      String gKey = data.substring(5, p1);
      String payload = data.substring(p1 + 1);
      preferences.putString(("g_" + gKey).c_str(), payload);
      triggerHud("全局键分配", (gKey + " 已生效").c_str(), -1, ST77XX_MAGENTA);
    }
  }
  else if (data.startsWith("SET:")) {
    int firstColon = data.indexOf(':', 4);
    if (firstColon > 0) {
      String keyName = data.substring(4, firstColon);
      String payload = data.substring(firstColon + 1);
      preferences.putString(keyName.c_str(), payload);
      triggerHud("宏定义保存", keyName.c_str(), -1, ST77XX_GREEN);
    }
  }
  // 网页端一次性下发文本：直接走 executeSequenceAction() 播放给主机。
  // 不写 FFat /me_hex.txt——用户要的是"临时蓝牙发送"，不是"持久化 ME 默认文本"。
  // MTU=517 + Web Bluetooth writeValue 自带的链路层分段，一包够撑日常长度。
  else if (data.startsWith("ME_TEXT:")) {
    String text = data.substring(8);
    if (text.length() > 0) {
      triggerHud("ME 发送文本", (String(text.length()) + " 字").c_str(), -1, ST77XX_CYAN);
      executeSequenceAction(text);
    }
  }
  else if (data.startsWith("LOGO_JPEG_START:")) {
    // 网页端声明本次要发多少字节的 JPEG。收到这条就切进二进制接收模式，
    // 后面的 BLE 写包一律当原始字节处理，收满自动收尾（不需要 END 包，
    // 也就没有"二进制数据里混进命令"的歧义）。
    uint32_t total = (uint32_t)data.substring(16).toInt();

    if (logoRxBuf) { free(logoRxBuf); logoRxBuf = NULL; }   // 重传：把上一轮的残留丢掉

    if (total == 0 || total > LOGO_RX_MAX) {
      triggerHud("壁纸传输", "长度不合法", -1, ST77XX_RED);
    } else if ((logoRxBuf = (uint8_t*)malloc(total)) == NULL) {
      triggerHud("壁纸传输", "内存不足", -1, ST77XX_RED);
    } else {
      logoRxTotal  = total;
      logoRxGot    = 0;
      logoRxDone   = false;
      logoRxLastMs = millis();

      char buf[28];
      snprintf(buf, sizeof(buf), "%u KB", (unsigned)(total / 1024));
      triggerHud("壁纸传输", buf, 0, ST77XX_YELLOW);
      lastActivityTime = millis();
    }
  }
}

// ================= BLE 通信 =================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    uint8_t* raw = pChar->getData();
    size_t   len = pChar->getLength();

    // 壁纸二进制模式：这包是 JPEG 原始字节，里面有 0x00，
    // 走 getValue().c_str() 会在第一个 0 处被截断，必须按长度整段取。
    // 唯一要放回命令通道的情况：网页端重传时补发的 LOGO_JPEG_START——
    // 此时固件还卡在上一轮的接收模式里，得让它先看见这条命令才能复位。
    // 用 "LOGO_" 做暗号是安全的：JPEG 的首字节必然是 0xFF，撞不上 ASCII。
    if (logoRxBuf != NULL && !logoRxDone && !(len >= 5 && memcmp(raw, "LOGO_", 5) == 0)) {
      handleLogoChunk(raw, len);
      return;
    }

    handleCommand(pChar->getValue().c_str());
  }
};

// ================= HUD 浮层管理（彻底消除全屏刷新） =================
void triggerHud(const char* title, const char* value, int percent, uint16_t color) {
  hud.active = true;
  hud.dirty = true;
  hud.triggerTime = millis();
  // strncpy 在源串不短于目标缓冲区时不会补结尾的 '\0'，后面 getUTF8Width()
  // 会一路读到数组外面去。GSET:/SET: 这两条指令会把电脑发来的任意长度文本
  // 塞进 value，所以这里必须显式封口，别指望 strncpy 自己收尾。
  strncpy(hud.title, title, sizeof(hud.title) - 1);
  hud.title[sizeof(hud.title) - 1] = '\0';
  strncpy(hud.value, value, sizeof(hud.value) - 1);
  hud.value[sizeof(hud.value) - 1] = '\0';
  hud.percent = percent;
  hud.color = color;
}

void drawHudOverlay() {
  if (!hud.active) return;
  // 字体统一放大到 wqy16 之后卡片跟着长了一圈：宽高各加了 12/10，
  // 但底边必须停在 y160 以内——通知条从 y164 起画，压上去就成两块叠一起了。
  int x = 14, y = 58, w = 212, h = 100;

  // 1.2秒超时退出，擦除 HUD 局部区域并恢复原区域底图，绝不清屏！
  if (millis() - hud.triggerTime > 1200) {
    hud.active = false;
    hud.dirty = false;
    screenNeedsRedraw = true;
    // 菜单/预览/设置子界面下没有差量引擎兜底，直接重画一次，避免 HUD 残影留在画面上
    if (currentSysMode == SYS_MODE_STYLE_PREVIEW) renderStylePreview();
    else if (currentSysMode == SYS_MODE_MENU) drawMenuUI();
    else if (currentSysMode == SYS_MODE_SET_TIME) renderTimeSetUI();
    else if (currentSysMode == SYS_MODE_SET_ALARM) renderAlarmSetUI();
    else if (currentSysMode == SYS_MODE_SET_TIMER) renderTimerSetUI();
    return;
  }

  // 仅在有改动时重画 1 次
  if (hud.dirty) {
    hud.dirty = false;
    tft.fillRoundRect(x, y, w, h, 10, ST77XX_BLACK);
    tft.drawRoundRect(x, y, w, h, 10, hud.color);

    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    int tw = u8g2.getUTF8Width(hud.title);
    u8g2.setCursor(x + (w - tw) / 2, y + 30);
    u8g2.print(hud.title);

    u8g2.setForegroundColor(hud.color);
    int vw = u8g2.getUTF8Width(hud.value);
    u8g2.setCursor(x + (w - vw) / 2, y + 62);
    u8g2.print(hud.value);

    if (hud.percent >= 0) {
      int barW = 168, barH = 8, barX = x + (w - barW) / 2, barY = y + 76;
      tft.drawRoundRect(barX, barY, barW, barH, 3, ST77XX_DARKGREY);
      int fillW = (barW - 4) * constrain(hud.percent, 0, 100) / 100;
      tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, hud.color);
    }
  }
}

// ================= 状态锁指示灯局部重绘 =================
// 返回 true 表示这一轮真的往屏幕上写了东西（调用方据此决定要不要重画覆盖层）
bool drawLockIndicators(int startY = 32, bool forceRedraw = false) {
  if (!forceRedraw && (lastNumLock == numLockActive && lastCapsLock == capsLockActive && lastScrollLock == scrollLockActive)) {
    return false;
  }
  lastNumLock = numLockActive;
  lastCapsLock = capsLockActive;
  lastScrollLock = scrollLockActive;

  uint16_t numBg = numLockActive ? 0x0400 : 0x18E3;
  uint16_t numFg = numLockActive ? ST77XX_GREEN : ST77XX_DARKGREY;
  tft.fillRoundRect(10, startY, 68, 20, 4, numBg);
  tft.drawRoundRect(10, startY, 68, 20, 4, numFg);
  tft.setTextSize(2);
  tft.setTextColor(numFg, numBg);
  tft.setCursor(24, startY + 3);
  tft.print("NUM");

  uint16_t capsBg = capsLockActive ? 0x001F : 0x18E3;
  uint16_t capsFg = capsLockActive ? ST77XX_CYAN : ST77XX_DARKGREY;
  tft.fillRoundRect(86, startY, 68, 20, 4, capsBg);
  tft.drawRoundRect(86, startY, 68, 20, 4, capsFg);
  tft.setTextColor(capsFg, capsBg);
  tft.setCursor(98, startY + 3);
  tft.print("CAPS");

  uint16_t scrlBg = scrollLockActive ? 0xFD20 : 0x18E3;
  uint16_t scrlFg = scrollLockActive ? ST77XX_YELLOW : ST77XX_DARKGREY;
  tft.fillRoundRect(162, startY, 68, 20, 4, scrlBg);
  tft.drawRoundRect(162, startY, 68, 20, 4, scrlFg);
  tft.setTextColor(scrlFg, scrlBg);
  tft.setCursor(174, startY + 3);
  tft.print("SCRL");

  return true;
}

// ================= 画板与壁纸模式 =================
void renderWallpaperView(bool drawOverlayTime) {
  if (!FFat.exists("/logo.bin")) {
    tft.fillScreen(ST77XX_BLACK);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_RED);
    u8g2.setCursor(65, 120);
    u8g2.print("未发现壁纸文件");
    return;
  }
  File f = FFat.open("/logo.bin", FILE_READ);
  if (!f) return;

  uint16_t rowBuffer[240];
  tft.startWrite();
  tft.setAddrWindow(0, 0, 240, 240);
  for (int y = 0; y < 240; y++) {
    f.read((uint8_t*)rowBuffer, 240 * sizeof(uint16_t));
    tft.writePixels(rowBuffer, 240);
  }
  tft.endWrite();
  f.close();
}

// ================= 方案图标 =================
// 方案1 固定 Windows、方案2 固定 macOS，所以左下角画个能认出来的标记。
// Win 和苹果用 XBM 点阵（形状讲究，基本图形拼不出来），游戏/工作拿手柄和公文包凑合。
// XBM 规矩：按行存、每字节低位在左，和 drawBitmap 的约定一致。
static const uint8_t iconWindows[24 * 3] = {
  0x00, 0x00, 0x00,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0x00, 0x00, 0x00,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0xFC, 0xE7, 0x3F, 0xFC, 0xE7, 0x3F,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 苹果轮廓：顶上叶子 + 双肩 + 圆身。手工点的 24x24，不是官方商标图形。
static const uint8_t iconApple[24 * 3] = {
  0x00, 0xE0, 0x00,
  0x00, 0xF0, 0x03,
  0x00, 0xF0, 0x03,
  0x00, 0xE0, 0x01,
  0x00, 0xC7, 0x01,
  0xC0, 0x2F, 0x07,
  0xE0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x1F,
  0xF0, 0xFF, 0x1F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F,
  0xF0, 0xFF, 0x1F,
  0xF0, 0xFF, 0x1F,
  0xE0, 0xFF, 0x0F,
  0xC0, 0xFF, 0x07,
  0x80, 0xFF, 0x03,
  0x00, 0xFE, 0x00,
  0x00, 0x7C, 0x00,
  0x00, 0x38, 0x00
};

// 画方案图标，固定 24x24，(x,y) 是左上角
void drawProfileIcon(int x, int y, uint8_t prof, uint16_t color) {
  switch (prof) {
    case 0:
      tft.drawBitmap(x, y, iconWindows, 24, 24, color, ST77XX_BLACK);
      break;
    case 1:
      tft.drawBitmap(x, y, iconApple, 24, 24, color, ST77XX_BLACK);
      break;
    case 2:
      // 游戏模式：手柄 —— 圆角机身 + 左侧十字键 + 右边两个按键
      tft.fillRoundRect(x + 1, y + 8, 22, 11, 5, color);
      tft.fillRect(x + 5, y + 12, 6, 2, ST77XX_BLACK);
      tft.fillRect(x + 7, y + 10, 2, 6, ST77XX_BLACK);
      tft.fillCircle(x + 16, y + 12, 1, ST77XX_BLACK);
      tft.fillCircle(x + 19, y + 15, 1, ST77XX_BLACK);
      break;
    default:
      // 工作模式：公文包 —— 箱体 + 提手 + 中缝
      tft.fillRoundRect(x + 1, y + 7, 22, 14, 2, color);
      tft.drawRoundRect(x + 8, y + 3, 8, 6, 2, color);
      tft.drawFastHLine(x + 1, y + 13, 22, ST77XX_BLACK);
      break;
  }
}

// ================= 锁状态：新风格共用的三格定义 =================
// 两种新风格都是"竖向分三块、每块一个锁状态"，只是画法不同（实心圆 vs 窄条胶囊），
// 所以把顺序、颜色、代表字母收在一张表里，两边都照着它遍历，保证不会各画各的。
// bit 对应 lockStateMask()：bit0=NUM，bit1=CAPS，bit2=SCR。
struct LockSlot {
  const char* label;
  uint16_t color;
  char letter;
  uint8_t bit;
};
static const LockSlot LOCK_SLOTS[3] = {
  { "CAPS", ST77XX_CYAN,   'C', 2 },
  { "NUM",  ST77XX_GREEN,  'N', 1 },
  { "SCR",  ST77XX_YELLOW, 'S', 4 }
};

// 两种新风格共用：上次画过的锁状态位图。新风格不走 drawLockIndicators()，
// 自己记一份，和它那两个 lastXxxLock 互不干扰。
static uint8_t lastLockMask = 0xFF;

static inline uint8_t lockStateMask() {
  return (numLockActive ? 1 : 0) | (capsLockActive ? 2 : 0) | (scrollLockActive ? 4 : 0);
}

// ================= 风格5：信息面板 =================
// 上（时钟 / 日期）、中（竖向三块，每块一个实心圆表示一个锁状态）、
// 下（左：方案图标；右：实时按键 + 击键统计）
#define INFO_DIV_TOP    74
#define INFO_DIV_BOT    172
#define INFO_CIRCLE_CY  118
#define INFO_CIRCLE_R   30
#define INFO_COL_W      80   // 中区每块 80 宽，圆心分别落在 x=40/120/200

// 中区三个实心圆。开 = 整个圆刷成状态色 + 黑字母，关 = 深灰 + 中灰字母。
// 只在锁状态真的变了的时候调用。
static void drawInfoLockCircles() {
  uint8_t mask = lockStateMask();
  lastLockMask = mask;

  for (int i = 0; i < 3; i++) {
    int cx = INFO_COL_W / 2 + i * INFO_COL_W;
    bool on = (mask & LOCK_SLOTS[i].bit) != 0;
    uint16_t bg = on ? LOCK_SLOTS[i].color : 0x18E3;

    tft.fillCircle(cx, INFO_CIRCLE_CY, INFO_CIRCLE_R, bg);

    // 圆心里放个大写首字母。textSize4 的可见字形是 20x28（advance 24），
    // 所以左上角要各回退半个字形才是几何居中；带底色画，覆盖范围离圆心仍在半径内。
    char ch[2] = { LOCK_SLOTS[i].letter, '\0' };
    tft.setTextSize(4);
    tft.setTextColor(on ? ST77XX_BLACK : 0x5AEB, bg);
    tft.setCursor(cx - 10, INFO_CIRCLE_CY - 14);
    tft.print(ch);

    // 圆下方标全名，免得只靠一个字母猜
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(on ? LOCK_SLOTS[i].color : 0x39E7);
    int w = u8g2.getUTF8Width(LOCK_SLOTS[i].label);
    u8g2.setCursor(cx - w / 2, 166);
    u8g2.print(LOCK_SLOTS[i].label);
  }
}

// 下区右边那条统计。"总计 12345" 整条一起打，省得算前缀宽度。
static void drawInfoTotalLine() {
  char buf[32];
  snprintf(buf, sizeof(buf), "总计 %lu", (unsigned long)totalKeyCount);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_GREEN);
  // 基线 234 而不是 238：wqy16 的下沉是 4px，238 会让最后一行像素落到屏幕外被切掉
  u8g2.setCursor(100, 234);
  u8g2.print(buf);
}

void renderInfoPanelBase() {
  // 注意：整屏的 fillScreen 由调用方 renderCurrentDisplayBase() 做过了，
  // 这里别再清一次，否则切风格时会多闪一下全屏黑。
  tft.drawFastHLine(0, INFO_DIV_TOP, 240, 0x39E7);
  tft.drawFastHLine(0, INFO_DIV_BOT, 240, 0x39E7);

  lastLockMask = 0xFF;   // 强制重画一次
  drawInfoLockCircles();

  // 左下：方案图标 + 方案号
  drawProfileIcon(16, 182, currentProfile, ST77XX_CYAN);

  char pbuf[16];
  snprintf(pbuf, sizeof(pbuf), "方案%d", currentProfile + 1);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  int pw = u8g2.getUTF8Width(pbuf);
  u8g2.setCursor(16 + 12 - pw / 2, 226);
  u8g2.print(pbuf);

  // 右下：静态标签。键名和总计数字由 updateDynamicElements() 差量刷
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  u8g2.setCursor(100, 192);
  u8g2.print("实时按键");
  drawInfoTotalLine();
}

// ================= 风格6：律动 =================
// 上大下窄两块。上半屏是音乐起伏式的律动（柱状谱），按下的键和右上角的"字数"
// 都从上方掉下来，砸到律动上炸开；下面窄条竖向三块，各展示一个锁状态。
//
// 上半屏故意不做视觉分层：律动带没有自己的底色，柱子直接从窄条上沿长出来，
// 整个上半屏是一块连续的空间，只有"掉下来的东西"和"砸出来的动静"。
//
// 两条铁律：
//   1. 柱子只画「高度差」，绝不每帧重刷整带；
//   2. 掉落物在"够到柱子顶端"的那一帧就炸掉，绝不带着框画进柱子身上。
//      擦除时盖到的柱子列会被标脏，下一帧连整根一起重画（爆炸碎片同理）。
#define RHYTHM_FRAME_MS   33     // 约 30fps，够流畅又不会把 2ms 的键盘扫描拖垮
#define RHYTHM_BAR_N      24     // 24 根柱，柱宽 8 + 间距 2 刚好铺满 240
#define RHYTHM_BAR_W      8
#define RHYTHM_BAR_PITCH  10
#define RHYTHM_BAR_BASE   205    // 柱子底边 = 下面窄条的上沿
#define RHYTHM_BAR_MAX    95     // 柱子最高 95px，顶到 y110（也是掉落物的最低撞点）
#define RHYTHM_BAR_BG     ST77XX_BLACK  // 律动带不另铺底色，上半屏是连续的一块
#define RHYTHM_STRIP_Y    205
#define RHYTHM_DROP_H     22     // 掉落框高（textSize2 字形 10x14，上下各留 3~4 点）
#define RHYTHM_DROP_MAXW  68     // 框最宽 = 5 个字 * 12 + 8
#define RHYTHM_KEY_LANES  2      // 键帽两条道。字放大到 textSize2 之后第三条挤不下了
#define RHYTHM_DROPS      3      // 0/1 号槽是键帽，2 号槽专门给右上角掉下来的"字数"
#define RHYTHM_NUM_SLOT   2
#define RHYTHM_NUM_MIN_MS 1200   // 字数别掉太勤，不然满屏都是数字
#define RHYTHM_PARTS      12     // 一次爆炸的碎片数

// 键帽两条道的横坐标。框最宽 68，所以 76+68 = 144，不会压到 x148 起的字数卡片
static const int RHYTHM_KEY_LANE_X[RHYTHM_KEY_LANES] = { 2, 76 };

// 32 点正弦表（0..255，中值 128）。查表代替 sinf，省掉每帧几十次浮点。
static const uint8_t rhythmSine[32] = {
  128, 152, 176, 198, 218, 234, 245, 253, 255, 253, 245, 234, 218, 198, 176, 152,
  128, 102,  79,  57,  37,  21,  10,   2,   0,   2,  10,  21,  37,  57,  79, 102
};

static const uint16_t RHYTHM_DROP_COLORS[5] = {
  ST77XX_CYAN, ST77XX_GREEN, ST77XX_YELLOW, ST77XX_MAGENTA, ST77XX_ORANGE
};

// 掉落物：键帽和"字数"数字共用这一个结构，只是槽位和取值不同。
// text 要装得下最长的数字（10 位）+ 结束符。
struct RhythmDrop {
  bool active;
  char text[12];
  int  x, y, w;
  int  vy;
  uint16_t color;
};

// 爆炸碎片。用 float 存坐标是因为速度是小数，用整数会一格一格地跳。
struct RhythmParticle {
  bool active;
  float x, y, vx, vy;
  int  life;
  uint16_t color;
};

static RhythmDrop rhythmDrops[RHYTHM_DROPS];
static RhythmParticle rhythmParts[RHYTHM_PARTS];
static uint8_t    rhythmBarH[RHYTHM_BAR_N];   // 上一帧每根柱的高度，用来只画差值
static uint8_t    rhythmPhase = 0;
static uint8_t    rhythmExcite = 0;           // 0..255，击键打到 255，之后每帧衰减
static uint8_t    rhythmLaneCursor = 0;
static uint16_t   rhythmBarColor = 0;
static unsigned long rhythmLastFrame = 0;
static unsigned long rhythmNumLastMs = 0;     // 上一次放"字数"数字的时间，用来限流
static uint32_t   rhythmRng = 0x2545F491;

// 自带一个 LCG 就够：只要"每次不一样"就行，不赌 esp_random 在各种配置下的行为
static uint32_t rhythmRand() {
  rhythmRng = rhythmRng * 1664525u + 1013904223u;
  return rhythmRng >> 16;
}

// 砸中律动时炸开一圈碎片。加随机是为了每次炸得不一样，
// 整体往上偏一点才像"炸开"，否则看着像"漏下去"。
static void rhythmExplode(int cx, int cy, uint16_t baseColor) {
  for (int i = 0; i < RHYTHM_PARTS; i++) {
    float ang = (float)i * (6.2831853f / RHYTHM_PARTS) + (float)(rhythmRand() % 100) * 0.01f;
    float spd = 1.6f + (float)(rhythmRand() % 140) * 0.01f;

    RhythmParticle& p = rhythmParts[i];
    p.active = true;
    p.x  = (float)cx;
    p.y  = (float)cy;
    p.vx = cosf(ang) * spd;
    p.vy = sinf(ang) * spd - 0.8f;
    p.life = 7 + (int)(rhythmRand() % 4);
    p.color = (i & 1) ? baseColor : ST77XX_WHITE;   // 明暗相间，看着更炸
  }
}

// 越激动颜色越烫：青 -> 黄 -> 红
static uint16_t rhythmColorFor(uint8_t ex) {
  if (ex > 140) return ST77XX_RED;
  if (ex > 60)  return ST77XX_YELLOW;
  return ST77XX_CYAN;
}

// 把整条律动带刷回底色。只在画底板、以及颜色分档切换时调用。
// 顺手把 rhythmBarH 清 0，下一帧的差值逻辑就会把所有柱子按新颜色重画一遍。
static void rhythmRepaintBars() {
  tft.fillRect(0, RHYTHM_BAR_BASE - RHYTHM_BAR_MAX, 240, RHYTHM_BAR_MAX, RHYTHM_BAR_BG);
  memset(rhythmBarH, 0, sizeof(rhythmBarH));
}

// 每帧只改动高度差的那几行像素，温和波动时一帧动不了几百个点
static void rhythmStepBars() {
  for (int i = 0; i < RHYTHM_BAR_N; i++) {
    int h = 4 + (rhythmSine[(rhythmPhase + i * 3) & 31] >> 4);                     // 温和基线 4..19
    h += (rhythmSine[(rhythmPhase * 3 + i * 7) & 31] * rhythmExcite) >> 10;        // 激动时最多再叠 63
    if (h > RHYTHM_BAR_MAX) h = RHYTHM_BAR_MAX;

    int prev = rhythmBarH[i];
    if (h == prev) continue;

    int x = i * RHYTHM_BAR_PITCH + 1;
    if (h > prev) tft.fillRect(x, RHYTHM_BAR_BASE - h, RHYTHM_BAR_W, h - prev, rhythmBarColor);
    else          tft.fillRect(x, RHYTHM_BAR_BASE - prev, RHYTHM_BAR_W, prev - h, RHYTHM_BAR_BG);
    rhythmBarH[i] = h;
  }
}

// 逐帧擦旧画新，每次只碰自己那一小块，绝不整屏清。
//
// 画掉落框那段是故意内联在这里的，别抽成 rhythmPaintDrop(const RhythmDrop&)：
// Arduino 会用 ctags 扫出所有函数、在文件最前面自动插一遍函数原型，
// 原型一旦出现在 struct RhythmDrop 定义之前，就是
// "'RhythmDrop' does not name a type"。**这个 sketch 里任何函数的参数和返回值
// 都不要用自定义结构体**，否则又会被自动原型坑一次。
static void rhythmStepDrops() {
  for (int i = 0; i < RHYTHM_DROPS; i++) {
    RhythmDrop& d = rhythmDrops[i];
    if (!d.active) continue;

    tft.fillRect(d.x, d.y, d.w, RHYTHM_DROP_H, RHYTHM_BAR_BG);
    // 擦的这块已经落到律动带的高度范围里了，说明有柱子可能被这一下擦花，
    // 把它盖到的那几列标脏，下一帧 rhythmStepBars() 会连整根一起补回来。
    // 只在最后几帧才走这里，平时不花这个钱。
    if (d.y + RHYTHM_DROP_H > RHYTHM_BAR_BASE - RHYTHM_BAR_MAX) {
      for (int c = d.x / RHYTHM_BAR_PITCH; c <= (d.x + d.w - 1) / RHYTHM_BAR_PITCH; c++) {
        if (c >= 0 && c < RHYTHM_BAR_N) rhythmBarH[c] = 0;
      }
    }

    d.y += d.vy;

    // "碰到下面的律动"就炸。撞点不写死，取它正下方那根柱子此刻的顶端，
    // 于是律动安静时炸在低处、激烈时炸在高处，永远贴着频谱，不会浮在半空。
    // rhythmBarH[] 在这一帧的 rhythmStepBars() 里刚更新过，读到的就是当前高度。
    int col = (d.x + d.w / 2) / RHYTHM_BAR_PITCH;
    if (col < 0) col = 0;
    if (col >= RHYTHM_BAR_N) col = RHYTHM_BAR_N - 1;
    int barTop = RHYTHM_BAR_BASE - (int)rhythmBarH[col];

    if (d.y + RHYTHM_DROP_H >= barTop) {
      d.active = false;
      rhythmExcite = 255;
      rhythmExplode(d.x + d.w / 2, barTop, d.color);
      continue;
    }

    tft.drawRoundRect(d.x, d.y, d.w, RHYTHM_DROP_H, 4, d.color);
    tft.setTextSize(2);
    tft.setTextColor(d.color, RHYTHM_BAR_BG);
    tft.setCursor(d.x + 4, d.y + 3);
    tft.print(d.text);
  }
}

// 爆炸碎片。它是唯一会飞到柱子上的东西，所以擦除时要把盖到的那几列柱子标脏，
// 让下一帧 rhythmStepBars() 连整根柱子一起重画——否则柱子上会留个永久缺口。
// （标脏到重画之间有一帧的延迟，也就是 33ms 的小豁口，看不出来）
static void rhythmStepParticles() {
  for (int i = 0; i < RHYTHM_PARTS; i++) {
    RhythmParticle& p = rhythmParts[i];
    if (!p.active) continue;

    int ox = (int)p.x, oy = (int)p.y;
    tft.fillRect(ox - 1, oy - 1, 3, 3, RHYTHM_BAR_BG);
    for (int dx = -1; dx <= 1; dx++) {
      int col = (ox + dx - 1) / RHYTHM_BAR_PITCH;
      if (col >= 0 && col < RHYTHM_BAR_N) rhythmBarH[col] = 0;
    }

    p.x += p.vx;
    p.y += p.vy;
    p.vx *= 0.93f;
    p.vy = p.vy * 0.93f + 0.30f;   // 一点重力，弧线才自然
    p.life--;

    // y>200 就收：再往下就进底部窄条了，会把状态块擦花
    if (p.life <= 0 || p.y > 200.0f || p.x < -3.0f || p.x > 243.0f) {
      p.active = false;
      continue;
    }
    tft.fillRect((int)p.x - 1, (int)p.y - 1, 3, 3, p.color);
  }
}

// 底部窄条竖向三块，每块固定一个锁状态。只在状态变化时重画。
static void drawRhythmLockStrips() {
  uint8_t mask = lockStateMask();
  lastLockMask = mask;

  for (int i = 0; i < 3; i++) {
    int bx = 4 + i * 80;   // 块宽 72，落在 4 / 84 / 164
    bool on = (mask & LOCK_SLOTS[i].bit) != 0;
    uint16_t bg = on ? LOCK_SLOTS[i].color : 0x18E3;

    tft.fillRoundRect(bx, 209, 72, 27, 5, bg);
    tft.drawRoundRect(bx, 209, 72, 27, 5, on ? ST77XX_WHITE : 0x39E7);

    tft.setTextSize(2);
    tft.setTextColor(on ? ST77XX_BLACK : 0x5AEB, bg);
    int tw = (int)strlen(LOCK_SLOTS[i].label) * 12;
    tft.setCursor(bx + (72 - tw) / 2, 215);
    tft.print(LOCK_SLOTS[i].label);
  }
}

// 右上角字数统计。u8g2.print 的数字重载不赌，统一 snprintf 成串再打。
// 卡片从 88 宽加到 96、只留后 6 位：wqy16 下 "字数 123456" 正好 88px，
// 6 位时原来那个 88 宽的卡片两边已经顶死，再多一位就会溢出到擦除区之外留残影。
static void drawRhythmCounter() {
  unsigned long shown = (totalKeyCount > 999999UL) ? (totalKeyCount % 1000000UL) : totalKeyCount;

  char buf[32];
  snprintf(buf, sizeof(buf), "字数 %lu", shown);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(totalKeyCount > 0 ? ST77XX_YELLOW : ST77XX_DARKGREY);
  u8g2.setCursor(140 + (96 - u8g2.getUTF8Width(buf)) / 2, 27);
  u8g2.print(buf);
}

void renderRhythmBase() {
  // 整屏 fillScreen 由调用方负责，这里不重复清屏

  // 先把动画状态归零再画，免得带着上一屏的激动值去算柱子颜色，白刷一次整带
  for (int i = 0; i < RHYTHM_DROPS; i++) rhythmDrops[i].active = false;
  for (int i = 0; i < RHYTHM_PARTS; i++) rhythmParts[i].active = false;
  rhythmExcite = 0;

  // 右上角字数卡片
  tft.drawRoundRect(140, 6, 96, 30, 6, 0x39E7);
  drawRhythmCounter();

  // 律动带
  rhythmBarColor = rhythmColorFor(rhythmExcite);
  rhythmRepaintBars();

  // 底部窄条 + 竖向三块锁状态
  tft.fillRect(0, RHYTHM_STRIP_Y, 240, 240 - RHYTHM_STRIP_Y, 0x1082);
  tft.drawFastHLine(0, RHYTHM_STRIP_Y, 240, 0x39E7);
  lastLockMask = 0xFF;
  drawRhythmLockStrips();
}

// 右侧那个"字数"也会掉下来：数字从卡片正下方钻出来，砸到律动上一样炸开。
// 隔 RHYTHM_NUM_MIN_MS 才放一个，不然打字快的时候满屏都是数字；
// 同一时刻只允许一个，掉着的时候不重复放。
static void rhythmSpawnNumberDrop() {
  RhythmDrop& d = rhythmDrops[RHYTHM_NUM_SLOT];
  if (d.active) return;

  unsigned long now = millis();
  if (now - rhythmNumLastMs < RHYTHM_NUM_MIN_MS) return;
  rhythmNumLastMs = now;

  snprintf(d.text, sizeof(d.text), "%lu", (unsigned long)totalKeyCount);
  if (strlen(d.text) > 6) memmove(d.text, d.text + strlen(d.text) - 6, 7);  // 位数太多只留后 6 位
  d.w = (int)strlen(d.text) * 12 + 8;

  // 对着右上角的字数卡片居中；宽度上限 80，所以最宽也就落到 x148~228，不会往左压到键帽道
  d.x = 188 - d.w / 2;
  d.y = 38;                                     // 从卡片正下方出来，别去擦卡片本身
  d.vy = 6 + (int)(rhythmRand() % 4);
  d.color = ST77XX_YELLOW;
  d.active = true;
}

// 击键通知：把律动打到最激烈，丢一个键帽下来，顺手放一个"字数"数字。
// 由 scanKeyboardMatrix() 在普通按键分支调用（菜单/录制/预览模式早就 return 了，到不了这里）。
void rhythmNotifyKey(const String& name) {
  if (currentDispMode != DISP_MODE_RHYTHM) return;

  rhythmExcite = 255;
  rhythmSpawnNumberDrop();

  // 找一条空泳道。两条都在掉就这次不掉——宁可少一个，也不要两个键帽互相擦掉对方。
  int lane = -1;
  for (int i = 0; i < RHYTHM_KEY_LANES; i++) {
    int cand = (rhythmLaneCursor + i) % RHYTHM_KEY_LANES;
    if (!rhythmDrops[cand].active) { lane = cand; break; }
  }
  if (lane < 0) return;
  rhythmLaneCursor = (lane + 1) % RHYTHM_KEY_LANES;

  RhythmDrop& d = rhythmDrops[lane];
  strncpy(d.text, name.c_str(), sizeof(d.text) - 1);
  d.text[sizeof(d.text) - 1] = '\0';
  // textSize2 一字符 12px，框最宽 5 个字。"Win/Cmd" 会截成 "Win/C"，"NumLock" 截成 "NumLo"
  if (strlen(d.text) > 5) d.text[5] = '\0';

  d.w = (int)strlen(d.text) * 12 + 8;
  if (d.w > RHYTHM_DROP_MAXW) d.w = RHYTHM_DROP_MAXW;   // 和上面 5 字截断互为兜底
  d.y = -RHYTHM_DROP_H;                        // 从屏幕上方掉进来
  d.vy = 6 + (int)(rhythmRand() % 5);          // 6~10 px/帧，速度不一致才像"一个个掉"
  d.color = RHYTHM_DROP_COLORS[rhythmRand() % 5];

  // 泳道横向再加点抖动才不规则。两条道最多也就差 2px 挨上，可以忽略
  d.x = RHYTHM_KEY_LANE_X[lane] + (int)(rhythmRand() % 9) - 4;
  if (d.x < 0) d.x = 0;
  if (d.x + d.w > 140) d.x = 140 - d.w;        // 右边给字数卡片留出来

  d.active = true;
}

// 每帧推进：柱子 + 掉落。返回 true 表示这帧真的往屏幕上写过东西。
bool updateRhythmAnimation() {
  if (currentDispMode != DISP_MODE_RHYTHM) return false;
  if (hud.active || ringingKind != RING_NONE) return false;   // HUD/响铃卡片盖在上面，这会儿别去擦它

  unsigned long now = millis();
  if (now - rhythmLastFrame < RHYTHM_FRAME_MS) return false;
  rhythmLastFrame = now;

  rhythmPhase = (rhythmPhase + 1) & 31;
  rhythmExcite = (rhythmExcite * 232) >> 8;   // 约 1 秒衰减回安静

  // 颜色分档一变就整带重刷一次（一次按键大概只发生两回），
  // 否则老像素会留着上一档的颜色，柱子新旧混杂。
  uint16_t col = rhythmColorFor(rhythmExcite);
  if (col != rhythmBarColor) {
    rhythmBarColor = col;
    rhythmRepaintBars();
  }

  // 顺序不能换：柱子在最底下画，碎片最后画。
  // 碎片会飞到柱子上，它擦除时把盖到的列标脏，下一帧柱子会连整根一起补回来。
  rhythmStepBars();
  rhythmStepDrops();
  rhythmStepParticles();
  return true;
}

// ================= 界面底框初始化（仅在切屏时执行一次） =================
void renderCurrentDisplayBase() {
  tft.fillScreen(ST77XX_BLACK);
  lastDrawnTimeStr[0] = '\0';
  lastDrawnKeyCount = 0xFFFFFFFF;
  keystrokeActiveOnScreen = false;
  lastLockMask = 0xFF;   // 两种新风格的锁状态随底板一起重来

  if (currentDispMode == DISP_MODE_GEEK) {
    tft.fillRect(0, 0, 240, 26, 0x18C3);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    u8g2.setCursor(8, 18);
    u8g2.print("方案: ");
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.print(profileNamesCN[currentProfile]);

    drawLockIndicators(30, true);
    tft.drawFastHLine(20, 100, 200, ST77XX_DARKGREY);

    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(20, 128);
    u8g2.print("击键计数: ");

    // 温湿度显示区域
    tft.drawRoundRect(10, 140, 220, 30, 6, ST77XX_CYAN);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_LIGHTGREY);
    u8g2.setCursor(20, 158);
    u8g2.print("温湿度: ");
    u8g2.setForegroundColor(ST77XX_CYAN);
    u8g2.print("--.-C  --.-%");

    tft.drawRoundRect(10, 175, 220, 52, 6, ST77XX_ORANGE);
  }
  else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
    drawLockIndicators(10, true);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    int pw = u8g2.getUTF8Width(profileNamesCN[currentProfile]);
    u8g2.setCursor((240 - pw) / 2, 210);
    u8g2.print(profileNamesCN[currentProfile]);
  }
  else if (currentDispMode == DISP_MODE_KEY_MON) {
    tft.fillRect(0, 0, 240, 28, ST77XX_NAVY);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    u8g2.setCursor(60, 20);
    u8g2.print("实时击键监控台");

    tft.drawRoundRect(15, 45, 210, 100, 8, ST77XX_CYAN);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_LIGHTGREY);
    u8g2.setCursor(25, 68);
    u8g2.print("最近触发按键：");

    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(25, 180);
    u8g2.print("累计按键数: ");

    u8g2.setCursor(25, 215);
    // 从"当前方案: "缩成"方案: "：wqy16 下后者刚好放得下最长的"方案1-Windows"，
    // 前者会顶到屏幕右沿
    u8g2.print("方案: ");
    u8g2.print(profileNamesCN[currentProfile]);
  }
  else if (currentDispMode == DISP_MODE_WALLPAPER) {
    renderWallpaperView(false);
  }
  else if (currentDispMode == DISP_MODE_INFO_PANEL) {
    renderInfoPanelBase();
  }
  else if (currentDispMode == DISP_MODE_RHYTHM) {
    renderRhythmBase();
  }
}

// ================= 极速差量动态元素刷新（绝不全屏重绘） =================
// 返回 true 表示这一轮往屏幕上写过东西——通知条是盖在底图上的，
// 底图一动就可能把它冲掉，所以调用方要拿着这个标志决定是否补画一次。
bool updateDynamicElements() {
  if (hud.active || ringingKind != RING_NONE) return false; // 弹窗/响铃卡片盖在上面期间暂停底层元素刷新

  bool touched = false;

  // 0. 读取 SHT31 温湿度数据（每 SHT31_READ_INTERVAL 毫秒一次，默认 15 分钟）。
  //    拉这么长是因为每次读要 delay ~20ms，太密会让 USB HID 帧延迟 → 键盘发"不灵敏"。
  if (shtAvailable && millis() - lastSHTRead > SHT31_READ_INTERVAL) {
    sht31_update();
    lastSHTRead = millis();
  }

  // 1. 刷新锁灯状态。两种新风格不摆那排胶囊，它们各自画（实心圆 / 窄条三块），
  //    并且自己记 lastLockMask，所以这里只负责老风格。
  if (currentDispMode == DISP_MODE_INFO_PANEL) {
    if (lockStateMask() != lastLockMask) { drawInfoLockCircles(); touched = true; }
  } else if (currentDispMode == DISP_MODE_RHYTHM) {
    if (lockStateMask() != lastLockMask) { drawRhythmLockStrips(); touched = true; }
  } else {
    if (drawLockIndicators((currentDispMode == DISP_MODE_BIG_CLOCK) ? 10 : 30)) touched = true;
  }

  // 2. 局部差量刷新时间（文本颜色带底色，无需清屏）
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char currentTimeStr[16];
  // 大字时钟和信息面板只显示到分钟：面板上时钟占的位置有限，
  // 少两位也就不会和右边的日期撞上
  bool shortTime = (currentDispMode == DISP_MODE_BIG_CLOCK || currentDispMode == DISP_MODE_INFO_PANEL);
  if (t && t->tm_year > 120) {
    strftime(currentTimeStr, sizeof(currentTimeStr), shortTime ? "%H:%M" : "%H:%M:%S", t);
  } else {
    snprintf(currentTimeStr, sizeof(currentTimeStr), "%s", shortTime ? "--:--" : "--:--:--");
  }

  if (strcmp(currentTimeStr, lastDrawnTimeStr) != 0) {
    strcpy(lastDrawnTimeStr, currentTimeStr);
    touched = true;

    if (currentDispMode == DISP_MODE_GEEK) {
      tft.setTextSize(3);
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK); // 自带黑色底色，直接重绘不闪烁
      tft.setCursor(50, 64);
      tft.print(currentTimeStr);
    }
    else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
      tft.setTextSize(6);
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(35, 75);
      tft.print(currentTimeStr);

      char dateStr[32];
      if (t && t->tm_year > 120) strftime(dateStr, sizeof(dateStr), "%Y-%m-%d  %A", t);
      else snprintf(dateStr, sizeof(dateStr), "等待同步时间");

      tft.fillRect(0, 145, 240, 24, ST77XX_BLACK);
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);
      u8g2.setForegroundColor(ST77XX_LIGHTGREY);
      int dw = u8g2.getUTF8Width(dateStr);
      u8g2.setCursor((240 - dw) / 2, 160);
      u8g2.print(dateStr);
    }
    else if (currentDispMode == DISP_MODE_WALLPAPER) {
      tft.fillRoundRect(130, 200, 100, 32, 6, 0x18E3);
      tft.setTextSize(3);
      tft.setTextColor(ST77XX_WHITE, 0x18E3);
      tft.setCursor(140, 205);
      tft.print(currentTimeStr);
    }
    else if (currentDispMode == DISP_MODE_INFO_PANEL) {
      // 时钟和日期一起重画——反正一分钟才走到这儿一次，省得再拆两块脏区
      tft.fillRect(8, 8, 224, 44, ST77XX_BLACK);

      tft.setTextSize(4);
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(10, 14);
      tft.print(currentTimeStr);

      char dateStr[24];
      if (t && t->tm_year > 120) {
        static const char* weekCN[] = { "日", "一", "二", "三", "四", "五", "六" };
        snprintf(dateStr, sizeof(dateStr), "%02d-%02d 周%s",
                 t->tm_mon + 1, t->tm_mday, weekCN[t->tm_wday]);
      } else {
        snprintf(dateStr, sizeof(dateStr), "未同步");
      }
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);
      u8g2.setForegroundColor(ST77XX_LIGHTGREY);
      u8g2.setCursor(232 - u8g2.getUTF8Width(dateStr), 40);
      u8g2.print(dateStr);
    }
  }

  // 3. 局部差量刷新按键计数器
  if (totalKeyCount != lastDrawnKeyCount) {
    lastDrawnKeyCount = totalKeyCount;
    touched = true;
    if (currentDispMode == DISP_MODE_GEEK) {
      tft.setTextSize(2);
      tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
      tft.fillRect(105, 116, 120, 18, ST77XX_BLACK);
      tft.setCursor(105, 116);
      tft.print(totalKeyCount);
    } else if (currentDispMode == DISP_MODE_KEY_MON) {
      tft.setTextSize(2);
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.fillRect(120, 168, 110, 18, ST77XX_BLACK);
      tft.setCursor(120, 168);
      tft.print(totalKeyCount);
    } else if (currentDispMode == DISP_MODE_INFO_PANEL) {
      tft.fillRect(100, 222, 136, 18, ST77XX_BLACK);
      drawInfoTotalLine();
    } else if (currentDispMode == DISP_MODE_RHYTHM) {
      // 右上角卡片：只擦里子、外框照原样补回来，免得整块闪一下
      tft.fillRect(141, 7, 94, 28, ST77XX_BLACK);
      tft.drawRoundRect(140, 6, 96, 30, 6, 0x39E7);
      drawRhythmCounter();
    }
  }

  // 4. 刷新温湿度显示（极客仪表盘）
  if (currentDispMode == DISP_MODE_GEEK) {
    static float lastDrawnTemp = -999.0f;
    static float lastDrawnHum = -999.0f;
    if (shtAvailable && (abs(shtTemperature - lastDrawnTemp) > 0.5f || abs(shtHumidity - lastDrawnHum) > 1.0f)) {
      lastDrawnTemp = shtTemperature;
      lastDrawnHum = shtHumidity;
      touched = true;
      tft.fillRect(85, 144, 140, 22, ST77XX_BLACK);
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);
      u8g2.setForegroundColor(ST77XX_CYAN);
      u8g2.setCursor(85, 158);
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1fC  %.1f%%", shtTemperature, shtHumidity);
      u8g2.print(buf);
    }
  }

  // 5. 实时按键回显局部更新与超时淡出
  if (currentDispMode == DISP_MODE_GEEK && showKeystrokes) {
    if (keystrokeNeedsRedraw) {
      keystrokeNeedsRedraw = false;
      keystrokeActiveOnScreen = true;
      touched = true;
      tft.fillRoundRect(15, 142, 210, 26, 4, 0x001F);
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);
      u8g2.setForegroundColor(ST77XX_WHITE);
      u8g2.setCursor(25, 160);
      u8g2.print("键入: ");
      u8g2.setForegroundColor(ST77XX_YELLOW);
      u8g2.print(lastKeyStrokeName);
    } else if (keystrokeActiveOnScreen && millis() - keyStrokeDisplayTime >= 800) {
      keystrokeActiveOnScreen = false;
      touched = true;
      tft.fillRect(15, 142, 210, 26, ST77XX_BLACK); // 超时只清除回显框这一小块！
    }
  }
  else if (currentDispMode == DISP_MODE_KEY_MON && keystrokeNeedsRedraw) {
    keystrokeNeedsRedraw = false;
    touched = true;
    tft.fillRect(25, 85, 190, 45, ST77XX_BLACK);
    tft.setTextSize(4);
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setCursor(35, 95);
    tft.print(lastKeyStrokeName);
  }
  else if (currentDispMode == DISP_MODE_INFO_PANEL && keystrokeNeedsRedraw) {
    // 下区右边的"实时按键"。键名都是 ASCII（Ctrl/Shift/Win/Cmd/...），走 tft 自带字体没问题，
    // 不用中文字体，还能用 textSize 放大。
    keystrokeNeedsRedraw = false;
    touched = true;
    tft.fillRect(100, 196, 136, 26, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.setCursor(100, 200);
    tft.print(lastKeyStrokeName);
  }

  return touched;
}

// ================= U8g2 菜单渲染 =================
int menuCursor = 0;
int menuScroll = 0;                        // 列表第一项在数组里的下标
const int MENU_TOTAL_ITEMS = 11;
const int MENU_VISIBLE_ITEMS = 7;          // 240 高的屏一次最多摆得下 7 行
const char* menuListCN[] = {
  "1. 返回主屏",
  "2. 切换主屏风格",
  "3. 切换配置方案",
  "4. 按键回显开关",
  "5. 键盘背光灯效",
  "6. 状态灯亮度",
  "7. 设置时间",
  "8. 闹钟设置",
  "9. 倒计时",
  "10. 立即刷新温湿度",
  "11. 敲击计数清零"
};

// 光标移动统一走这里。菜单从 7 项涨到 10 项之后一屏摆不下，
// 原来散在四个地方各写一遍的 (cursor ± 1) % TOTAL 只要漏掉一处，
// 就是"光标滚到看不见的地方去了"这种最难查的 bug。
void menuMove(int delta) {
  if (delta == 0) return;

  menuCursor = (menuCursor + delta) % MENU_TOTAL_ITEMS;
  if (menuCursor < 0) menuCursor += MENU_TOTAL_ITEMS;

  // 光标跑出窗口就把窗口挪过去，让它始终停在可见范围里
  if (menuCursor < menuScroll) menuScroll = menuCursor;
  if (menuCursor >= menuScroll + MENU_VISIBLE_ITEMS) menuScroll = menuCursor - MENU_VISIBLE_ITEMS + 1;
  if (menuScroll > MENU_TOTAL_ITEMS - MENU_VISIBLE_ITEMS) menuScroll = MENU_TOTAL_ITEMS - MENU_VISIBLE_ITEMS;
  if (menuScroll < 0) menuScroll = 0;

  drawMenuUI();
}

void drawMenuUI() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 240, 32, ST77XX_NAVY);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  const char* menuTitle = "YYQ 键盘系统 OS";
  u8g2.setCursor((240 - u8g2.getUTF8Width(menuTitle)) / 2, 22);
  u8g2.print(menuTitle);

  // 左上角位置指示 3/10：项数超过一屏之后，不告诉用户下面还有东西，
  // 他会以为滚不动了（右上角那个位置留给通知条数角标）
  char posBuf[16];
  snprintf(posBuf, sizeof(posBuf), "%d/%d", menuCursor + 1, MENU_TOTAL_ITEMS);
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  u8g2.setCursor(8, 22);
  u8g2.print(posBuf);

  for (int i = 0; i < MENU_VISIBLE_ITEMS; i++) {
    int idx = menuScroll + i;
    if (idx >= MENU_TOTAL_ITEMS) break;

    int y = 37 + i * 27;
    if (idx == menuCursor) {
      tft.fillRoundRect(8, y, 224, 26, 6, ST77XX_BLUE);
      u8g2.setForegroundColor(ST77XX_WHITE);
    } else {
      u8g2.setForegroundColor(ST77XX_LIGHTGREY);
    }
    u8g2.setCursor(20, y + 18);
    u8g2.print(menuListCN[idx]);
  }

  drawNotifBadge(); // 待处理通知条数
}

// ================= 设置子界面：三个界面共用的一套字段编辑逻辑 =================
// 约定（三个界面完全一致，用户学会一个就会全部）：
//   ← →   切换字段        ↑ ↓ / 旋钮   调整当前字段的值
//   回车    保存            ESC / MC     取消
//
// 这里所有函数的参数和返回值都只用 int / char* / uint16_t。
// 这个 sketch 有个坑：Arduino 会用 ctags 扫出全部函数、在文件最前面插一遍原型，
// 原型一旦出现在自定义 struct 的定义之前，就是 "'Xxx' does not name a type"。
// 所以**任何函数的签名里都不要出现自定义结构体**。

// textSize 下按固定位数画一个数字（补前导零）。带底色画，重绘不闪。
static void drawFieldNum(int x, int y, int value, int digits, uint16_t fg, uint16_t bg) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%0*d", digits, value);
  tft.setTextColor(fg, bg);
  tft.setCursor(x, y);
  tft.print(buf);
}

// 三个界面底部那两行固定提示，抽出来免得三份里有一份写错字
static void drawSetFooter() {
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);

  const char* hint = "左右换字段 上下调值";
  u8g2.setForegroundColor(ST77XX_YELLOW);
  u8g2.setCursor((240 - u8g2.getUTF8Width(hint)) / 2, 206);
  u8g2.print(hint);

  const char* hint2 = "回车保存   ESC 取消";
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  u8g2.setCursor((240 - u8g2.getUTF8Width(hint2)) / 2, 228);
  u8g2.print(hint2);
}

// ---------------------------------------------------------------- 设置时间
// 字段 0..4 = 年 / 月 / 日 / 时 / 分
int timeFieldIdx = 0;
int timeEditY = 2026, timeEditMo = 1, timeEditD = 1, timeEditH = 0, timeEditMi = 0;

static int daysInMonth(int y, int m) {
  static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m < 1 || m > 12) return 31;
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return d[m - 1];
}

// 越界就绕到另一端（而不是夹住不动）：`日` 从 1 再往上一格直接跳到当月最后一天，
// 用起来才像在"翻数字"。日的上限按当月实际天数算，二月不会出现 30 号。
static void clampTimeEdit() {
  if (timeEditY < 2024) timeEditY = 2099;
  if (timeEditY > 2099) timeEditY = 2024;

  if (timeEditMo < 1) timeEditMo = 12;
  if (timeEditMo > 12) timeEditMo = 1;

  int dmax = daysInMonth(timeEditY, timeEditMo);
  if (timeEditD < 1) timeEditD = dmax;
  if (timeEditD > dmax) timeEditD = 1;

  if (timeEditH < 0) timeEditH = 23;
  if (timeEditH > 23) timeEditH = 0;

  if (timeEditMi < 0) timeEditMi = 59;
  if (timeEditMi > 59) timeEditMi = 0;
}

static void adjustSettingField(int delta);
static void applyTimerAction();

void renderTimeSetUI() {
  tft.fillScreen(ST77XX_BLACK);
  if (hud.active) hud.dirty = true;   // 整屏重画把 HUD 卡片冲掉了，让它下一轮自己补回来
  tft.fillRect(0, 0, 240, 30, ST77XX_NAVY);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  const char* title = "设置时间";
  u8g2.setCursor((240 - u8g2.getUTF8Width(title)) / 2, 21);
  u8g2.print(title);

  // 日期行：textSize2 每字符 12 宽。"2026-09-26" 整串 120 宽，从 x=60 起正好居中。
  // 每个字段的横向区段是写死的常量，高亮框和数字用的是同一组数，不会出现框和数字错位。
  const int dateY = 76;
  tft.setTextSize(2);

  // 先把几块高亮底色一次铺好，再统一写数字和分隔符。顺序反过来就出问题：
  // 5x7 字体里 '-' 是满格横线，正好压在后一个高亮框的边上，会被啃掉一截。
  tft.fillRoundRect(57, dateY - 4, 54, 22, 3, timeFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(117, dateY - 4, 30, 22, 3, timeFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(153, dateY - 4, 30, 22, 3, timeFieldIdx == 2 ? ST77XX_BLUE : ST77XX_BLACK);

  drawFieldNum(60, dateY, timeEditY, 4, timeFieldIdx == 0 ? ST77XX_WHITE : ST77XX_CYAN, timeFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(108, dateY);
  tft.print("-");

  drawFieldNum(120, dateY, timeEditMo, 2, timeFieldIdx == 1 ? ST77XX_WHITE : ST77XX_CYAN, timeFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(144, dateY);
  tft.print("-");

  drawFieldNum(156, dateY, timeEditD, 2, timeFieldIdx == 2 ? ST77XX_WHITE : ST77XX_CYAN, timeFieldIdx == 2 ? ST77XX_BLUE : ST77XX_BLACK);

  // 时间行：textSize4 每字符 24 宽。"14:30" 整串 120 宽，同样从 x=60 起居中。
  const int timeY = 118;
  tft.setTextSize(4);

  tft.fillRoundRect(56, timeY - 6, 60, 40, 4, timeFieldIdx == 3 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(128, timeY - 6, 60, 40, 4, timeFieldIdx == 4 ? ST77XX_BLUE : ST77XX_BLACK);

  drawFieldNum(60, timeY, timeEditH, 2, timeFieldIdx == 3 ? ST77XX_WHITE : ST77XX_GREEN, timeFieldIdx == 3 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(108, timeY);
  tft.print(":");

  drawFieldNum(132, timeY, timeEditMi, 2, timeFieldIdx == 4 ? ST77XX_WHITE : ST77XX_GREEN, timeFieldIdx == 4 ? ST77XX_BLUE : ST77XX_BLACK);

  // 只靠一个蓝框看不出选的是"日"还是"月"，用中文说一遍
  static const char* fieldNamesCN[5] = { "年", "月", "日", "时", "分" };
  char line[32];
  snprintf(line, sizeof(line), "正在调整: %s", fieldNamesCN[timeFieldIdx]);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_YELLOW);
  u8g2.setCursor((240 - u8g2.getUTF8Width(line)) / 2, 186);
  u8g2.print(line);

  drawSetFooter();
}

// ---------------------------------------------------------------- 闹钟设置
// 字段 0..2 = 时 / 分 / 开关
int alarmFieldIdx = 0;
bool alarmEditOn = false;
int alarmEditH = 7, alarmEditM = 0;

void renderAlarmSetUI() {
  tft.fillScreen(ST77XX_BLACK);
  if (hud.active) hud.dirty = true;   // 整屏重画把 HUD 卡片冲掉了，让它下一轮自己补回来
  tft.fillRect(0, 0, 240, 30, ST77XX_NAVY);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  const char* title = "闹钟设置";
  u8g2.setCursor((240 - u8g2.getUTF8Width(title)) / 2, 21);
  u8g2.print(title);

  // "07:30" textSize4 整串 120 宽，从 x=60 起居中
  const int timeY = 72;
  tft.setTextSize(4);

  tft.fillRoundRect(56, timeY - 6, 60, 40, 4, alarmFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(128, timeY - 6, 60, 40, 4, alarmFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);

  drawFieldNum(60, timeY, alarmEditH, 2, alarmFieldIdx == 0 ? ST77XX_WHITE : ST77XX_ORANGE, alarmFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(108, timeY);
  tft.print(":");

  drawFieldNum(132, timeY, alarmEditM, 2, alarmFieldIdx == 1 ? ST77XX_WHITE : ST77XX_ORANGE, alarmFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);

  // 开关：整块一个胶囊，选中时外面再套一圈，一眼能看出光标停在开关上
  const int swW = 140, swH = 40, swX = (240 - swW) / 2, swY = 150;
  uint16_t swBg = alarmEditOn ? 0x0400 : 0x18E3;
  tft.fillRoundRect(swX, swY, swW, swH, 8, swBg);
  tft.drawRoundRect(swX, swY, swW, swH, 8,
                    alarmFieldIdx == 2 ? ST77XX_WHITE : (alarmEditOn ? ST77XX_GREEN : ST77XX_DARKGREY));
  if (alarmFieldIdx == 2) tft.drawRoundRect(swX - 2, swY - 2, swW + 4, swH + 4, 10, ST77XX_BLUE);

  const char* swText = alarmEditOn ? "闹钟 开" : "闹钟 关";
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(alarmEditOn ? ST77XX_GREEN : ST77XX_LIGHTGREY);
  u8g2.setCursor(swX + (swW - u8g2.getUTF8Width(swText)) / 2, swY + 26);
  u8g2.print(swText);

  drawSetFooter();
}

// ---------------------------------------------------------------- 倒计时设置
// 字段 0..2 = 时 / 分 / 秒，字段 3 是动作按钮（启动 / 停止）
int timerFieldIdx = 0;
int timerEditH = 0, timerEditM = 5, timerEditS = 0;

void renderTimerSetUI() {
  tft.fillScreen(ST77XX_BLACK);
  if (hud.active) hud.dirty = true;   // 整屏重画把 HUD 卡片冲掉了，让它下一轮自己补回来
  tft.fillRect(0, 0, 240, 30, ST77XX_NAVY);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  const char* title = "倒计时";
  u8g2.setCursor((240 - u8g2.getUTF8Width(title)) / 2, 21);
  u8g2.print(title);

  // "00:05:00" textSize4 整串 8 字符 = 192 宽，从 x=24 起居中
  const int timeY = 66;
  const int hx = 24, mx = 96, sx = 168;   // 时 / 分 / 秒 各自的起点
  tft.setTextSize(4);

  // 同样先铺三块高亮底色，再把两个冒号和三组数字写上去
  tft.fillRoundRect(hx - 4, timeY - 6, 60, 40, 4, timerFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(mx - 4, timeY - 6, 60, 40, 4, timerFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);
  tft.fillRoundRect(sx - 4, timeY - 6, 60, 40, 4, timerFieldIdx == 2 ? ST77XX_BLUE : ST77XX_BLACK);

  drawFieldNum(hx, timeY, timerEditH, 2, timerFieldIdx == 0 ? ST77XX_WHITE : ST77XX_CYAN, timerFieldIdx == 0 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(72, timeY);
  tft.print(":");

  drawFieldNum(mx, timeY, timerEditM, 2, timerFieldIdx == 1 ? ST77XX_WHITE : ST77XX_CYAN, timerFieldIdx == 1 ? ST77XX_BLUE : ST77XX_BLACK);

  tft.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(144, timeY);
  tft.print(":");

  drawFieldNum(sx, timeY, timerEditS, 2, timerFieldIdx == 2 ? ST77XX_WHITE : ST77XX_CYAN, timerFieldIdx == 2 ? ST77XX_BLUE : ST77XX_BLACK);

  // 动作按钮。倒计时已经在跑的时候按钮变"停止"，并且显示的是真实剩余时间，
  // 免得用户以为刚才按的启动没生效。
  bool running = timerRunning;
  const int btnW = 168, btnH = 42, btnX = (240 - btnW) / 2, btnY = 130;
  uint16_t btnBg = running ? 0x7800 : 0x0260;   // 跑着=暗红，待机=暗绿
  tft.fillRoundRect(btnX, btnY, btnW, btnH, 8, btnBg);
  tft.drawRoundRect(btnX, btnY, btnW, btnH, 8,
                    timerFieldIdx == 3 ? ST77XX_WHITE : (running ? ST77XX_RED : ST77XX_GREEN));
  if (timerFieldIdx == 3) tft.drawRoundRect(btnX - 2, btnY - 2, btnW + 4, btnH + 4, 10, ST77XX_BLUE);

  char btnText[40];
  if (running) {
    uint32_t r = timerRemainSec;
    snprintf(btnText, sizeof(btnText), "停止  剩 %02u:%02u:%02u",
             (unsigned)(r / 3600), (unsigned)((r / 60) % 60), (unsigned)(r % 60));
  } else {
    snprintf(btnText, sizeof(btnText), "开始倒计时");
  }
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(running ? ST77XX_RED : ST77XX_GREEN);
  u8g2.setCursor(btnX + (btnW - u8g2.getUTF8Width(btnText)) / 2, btnY + 28);
  u8g2.print(btnText);

  drawSetFooter();
}

// 三个界面共用的字段调整。delta 是 ±1。
static void adjustSettingField(int delta) {
  if (currentSysMode == SYS_MODE_SET_TIME) {
    switch (timeFieldIdx) {
      case 0: timeEditY += delta; break;
      case 1: timeEditMo += delta; break;
      case 2: timeEditD += delta; break;
      case 3: timeEditH += delta; break;
      default: timeEditMi += delta; break;
    }
    clampTimeEdit();
    renderTimeSetUI();
  } else if (currentSysMode == SYS_MODE_SET_ALARM) {
    if (alarmFieldIdx == 2) {
      alarmEditOn = !alarmEditOn;
    } else if (alarmFieldIdx == 0) {
      alarmEditH = (alarmEditH + delta + 24) % 24;
    } else {
      alarmEditM = (alarmEditM + delta + 60) % 60;
    }
    renderAlarmSetUI();
  } else if (currentSysMode == SYS_MODE_SET_TIMER) {
    if (timerFieldIdx == 3) {
      // 光标停在按钮上时，上下键也当"确认/取消"使，省得非要挪回回车
      if (delta > 0) applyTimerAction();
    } else if (timerFieldIdx == 0) {
      timerEditH = (timerEditH + delta + 24) % 24;
      renderTimerSetUI();
    } else if (timerFieldIdx == 1) {
      timerEditM = (timerEditM + delta + 60) % 60;
      renderTimerSetUI();
    } else {
      timerEditS = (timerEditS + delta + 60) % 60;
      renderTimerSetUI();
    }
  }
}

// 设置界面里左右切字段。切完整屏重画——一屏也就二十来行，比算脏区省心且不会留残影。
static void moveSettingField(int delta) {
  if (currentSysMode == SYS_MODE_SET_TIME) {
    timeFieldIdx = (timeFieldIdx + delta + 5) % 5;
    renderTimeSetUI();
  } else if (currentSysMode == SYS_MODE_SET_ALARM) {
    alarmFieldIdx = (alarmFieldIdx + delta + 3) % 3;
    renderAlarmSetUI();
  } else if (currentSysMode == SYS_MODE_SET_TIMER) {
    timerFieldIdx = (timerFieldIdx + delta + 4) % 4;
    renderTimerSetUI();
  }
}

// 倒计时界面那个动作按钮：没在跑就按当前设定时长启动，在跑就停。
static void applyTimerAction() {
  if (timerRunning) {
    timerRunning = false;
    timerRemainSec = 0;
    triggerHud("倒计时", "已停止", -1, ST77XX_RED);
    renderTimerSetUI();
    return;
  }

  uint32_t total = (uint32_t)timerEditH * 3600UL + (uint32_t)timerEditM * 60UL + (uint32_t)timerEditS;
  if (total == 0) {
    triggerHud("倒计时", "时长不能为 0", -1, ST77XX_RED);
    return;
  }

  preferences.putUChar("tmr_h", (uint8_t)timerEditH);
  preferences.putUChar("tmr_m", (uint8_t)timerEditM);
  preferences.putUChar("tmr_s", (uint8_t)timerEditS);

  timerRunning = true;
  timerTotalSec = total;
  timerRemainSec = total;
  timerStartMs = millis();

  char buf[24];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)timerEditH, (unsigned)timerEditM, (unsigned)timerEditS);
  triggerHud("倒计时开始", buf, -1, ST77XX_GREEN);
  renderTimerSetUI();
}

// ================= 设置界面的保存 / 取消 =================
// 把当前时间落到 NVS。ESP32 的 RTC 只在软复位后还活着，拔电就没了：
// 手动设完时间、从电脑同步完，都来这里存一份；跑起来之后再每 30 分钟补一次，
// 于是"关机了多久"这段误差最多半小时，而不是一拔电就回到 1970 年。
void persistClock() {
  time_t now = time(nullptr);
  if (now <= 0) return;
  struct tm* t = localtime(&now);
  if (t == NULL || t->tm_year < 120) return;   // 时间还没对上，别把垃圾存进去
  preferences.putUInt("set_epoch", (uint32_t)now);
}

// 回车：把当前界面的值落盘，退回菜单
void saveSettingScreen() {
  if (currentSysMode == SYS_MODE_SET_TIME) {
    struct tm t = {};
    t.tm_year = timeEditY - 1900;
    t.tm_mon  = timeEditMo - 1;
    t.tm_mday = timeEditD;
    t.tm_hour = timeEditH;
    t.tm_min  = timeEditMi;
    t.tm_sec  = 0;          // 秒归零：秒这一位没有 UI，留着旧值会显得"设完就偏了几秒"
    t.tm_isdst = -1;        // 交给 mktime 按 TZ 自己判断，写死 0 反而会错

    time_t epoch = mktime(&t);
    if (epoch <= 0) {
      triggerHud("设置时间", "日期无效", -1, ST77XX_RED);
      return;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    lastDrawnTimeStr[0] = '\0';        // 逼差量引擎下一轮把钟面重画一遍
    lastDrawnKeyCount = 0xFFFFFFFF;
    persistClock();
    triggerHud("设置时间", "已保存", -1, ST77XX_GREEN);
  }
  else if (currentSysMode == SYS_MODE_SET_ALARM) {
    alarmEnabled = alarmEditOn;
    alarmHour = (uint8_t)alarmEditH;
    alarmMinute = (uint8_t)alarmEditM;
    alarmLastFiredYday = -1;
    preferences.putBool("alarm_on", alarmEnabled);
    preferences.putUChar("alarm_h", alarmHour);
    preferences.putUChar("alarm_m", alarmMinute);

    char buf[24];
    if (alarmEnabled) snprintf(buf, sizeof(buf), "%02u:%02u", alarmHour, alarmMinute);
    else              snprintf(buf, sizeof(buf), "已关闭");
    triggerHud("闹钟设置", buf, -1, alarmEnabled ? ST77XX_GREEN : ST77XX_LIGHTGREY);
  }
  else if (currentSysMode == SYS_MODE_SET_TIMER) {
    preferences.putUChar("tmr_h", (uint8_t)timerEditH);
    preferences.putUChar("tmr_m", (uint8_t)timerEditM);
    preferences.putUChar("tmr_s", (uint8_t)timerEditS);
    triggerHud("倒计时", "时长已保存", -1, ST77XX_GREEN);
  }

  currentSysMode = SYS_MODE_MENU;
  drawMenuUI();
}

void cancelSettingScreen() {
  currentSysMode = SYS_MODE_MENU;
  drawMenuUI();
}

// ================= 响铃：闹钟和倒计时到点共用 =================
// 到点了就是"响铃"这么一个状态：19 颗灯全红快闪 + 屏幕上一张常驻卡片，等用户按下去。
// 两种铃只有文案不同，走同一套，不再各写一份。
void startRinging(uint8_t kind) {
  if (ringingKind != RING_NONE) return;   // 已经在响了就别打断，也别重置 60 秒兜底

  ringingKind = kind;
  ringStartMs = millis();
  ringDirty = true;

  hud.active = false;                     // 响铃卡片比 HUD 重要，把浮层让出来
  hud.dirty = false;

  // 屏幕可能正息着。只闪灯不看屏等于没提醒，所以把屏幕叫醒。
  lastActivityTime = millis();
  if (currentSysMode == SYS_MODE_SLEEP) {
    currentSysMode = SYS_MODE_NORMAL;
    screenNeedsRedraw = true;
  }
}

void stopRinging() {
  if (ringingKind == RING_NONE) return;
  ringingKind = RING_NONE;

  // 卡片擦掉之后底下是花的，底板必须重画。菜单和设置界面不吃底板重画这一套
  // （它们的主渲染只在 SYS_MODE_NORMAL 分支里跑），各自手动重画一次。
  screenNeedsRedraw = true;
  if (currentSysMode == SYS_MODE_MENU) drawMenuUI();
  else if (currentSysMode == SYS_MODE_STYLE_PREVIEW) renderStylePreview();
  else if (currentSysMode == SYS_MODE_SET_TIME) renderTimeSetUI();
  else if (currentSysMode == SYS_MODE_SET_ALARM) renderAlarmSetUI();
  else if (currentSysMode == SYS_MODE_SET_TIMER) renderTimerSetUI();
}

// 响铃卡片。常驻不自动消失，边框每 500ms 闪一次——
// 屏幕在闪 + 灯在闪，两个一起才够"闹钟"的份量。
void drawRingOverlay() {
  if (ringingKind == RING_NONE) return;

  unsigned long now = millis();

  // 60 秒还没人理：停铃，但留一条通知，不算静默丢失。
  // 不这么兜底的话，一颗红灯能闪一整晚。
  if (now - ringStartMs > 60000UL) {
    const char* what = (ringingKind == RING_ALARM) ? "闹钟未确认" : "倒计时未确认";
    stopRinging();
    pushNotification(ALERT_RED, what);
    return;
  }

  static unsigned long lastBlink = 0;
  static bool blinkOn = true;
  if (now - lastBlink > 500) {
    lastBlink = now;
    blinkOn = !blinkOn;
    ringDirty = true;
  }
  if (!ringDirty) return;
  ringDirty = false;

  const int x = 10, y = 50, w = 220, h = 140;
  uint16_t edge = blinkOn ? ST77XX_RED : 0x7800;   // 亮红 / 暗红交替
  tft.fillRoundRect(x, y, w, h, 12, ST77XX_BLACK);
  tft.drawRoundRect(x, y, w, h, 12, edge);
  tft.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 10, edge);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);

  const char* title = (ringingKind == RING_ALARM) ? "闹钟时间到" : "倒计时结束";
  u8g2.setForegroundColor((ringingKind == RING_ALARM) ? ST77XX_RED : ST77XX_ORANGE);
  u8g2.setCursor(x + (w - u8g2.getUTF8Width(title)) / 2, y + 34);
  u8g2.print(title);

  // 中间大字：闹钟报"本该几点响"，倒计时报"刚才设了多久"
  char big[24];
  if (ringingKind == RING_ALARM) {
    snprintf(big, sizeof(big), "%02u:%02u", (unsigned)alarmHour, (unsigned)alarmMinute);
  } else if (timerTotalSec >= 3600) {
    snprintf(big, sizeof(big), "%02u:%02u:%02u",
             (unsigned)(timerTotalSec / 3600), (unsigned)((timerTotalSec / 60) % 60), (unsigned)(timerTotalSec % 60));
  } else {
    snprintf(big, sizeof(big), "%02u:%02u", (unsigned)(timerTotalSec / 60), (unsigned)(timerTotalSec % 60));
  }

  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(x + (w - (int)strlen(big) * 18) / 2, y + 70);
  tft.print(big);

  const char* tip = "按灯光键确认";
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  u8g2.setCursor(x + (w - u8g2.getUTF8Width(tip)) / 2, y + 122);
  u8g2.print(tip);
}

// 每秒跑一遍：闹钟到点判定 + 倒计时推进。放主循环里，不占任何中断。
void updateTimers() {
  unsigned long now = millis();

  // ---- 闹钟：只在"当前这一分钟"命中，再配合 yday 保证一天只响一次 ----
  time_t t = time(nullptr);
  struct tm* lt = localtime(&t);
  if (lt != NULL && lt->tm_year > 120 && alarmEnabled && ringingKind == RING_NONE) {
    int nowMin = lt->tm_hour * 60 + lt->tm_min;
    if (nowMin == alarmHour * 60 + alarmMinute && lt->tm_yday != alarmLastFiredYday) {
      alarmLastFiredYday = lt->tm_yday;
      startRinging(RING_ALARM);
    }
  }

  // ---- 倒计时 ----
  if (!timerRunning) return;

  // 剩余时间从"开始时刻"反算，而不是每秒自减：中间被宏的 SLEEP()
  // 或者壁纸解码阻塞过几百毫秒，也不会越欠越多。
  // 无符号减法让 millis() 回绕（49.7 天一次）天然正确。
  uint32_t totalMs = timerTotalSec * 1000UL;
  uint32_t elapsedMs = (uint32_t)(now - timerStartMs);
  uint32_t remain = (elapsedMs >= totalMs) ? 0 : (totalMs - elapsedMs + 999UL) / 1000UL;
  bool changed = (remain != timerRemainSec);
  timerRemainSec = remain;

  if (remain == 0) {
    timerRunning = false;
    startRinging(RING_TIMER);
    return;
  }
  if (!changed) return;

  if (currentSysMode == SYS_MODE_SET_TIMER) {
    // 设置界面正开着，把读数刷到界面上，免得显示的剩余时间是死的
    renderTimerSetUI();
  } else if (remain <= 60) {
    // 最后一分钟每秒报一次。再长就不打扰了——键盘首先还是键盘，
    // 一张 HUD 卡片糊在屏幕中间五分钟不现实。
    char buf[16];
    // 用 分:秒 而不是"还剩 N 秒"：remain 正好等于 60 那一秒写成 %02u 会变成 "00:60"
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(remain / 60), (unsigned)(remain % 60));
    triggerHud("倒计时", buf, -1, ST77XX_ORANGE);
  }
}

// ================= 主屏风格预览（左右切换，二次确认才生效） =================
void renderStylePreview() {
  uint8_t savedDispMode = currentDispMode;
  currentDispMode = previewDispMode;

  // 完整画出该风格的真实底板，再补一次动态内容，预览不会是一片空白
  renderCurrentDisplayBase();
  updateDynamicElements();

  // 底部预览提示条。字体放大到 wqy16 之后两行要 44px 才不挤，
  // 所以整条往上挪：原来 y202 起，现在 y190 起，仍然贴着屏幕底沿。
  const int barY = 190;
  tft.fillRect(0, barY, 240, 240 - barY, ST77XX_BLACK);
  tft.drawFastHLine(0, barY, 240, ST77XX_CYAN);

  char line1[40];
  snprintf(line1, sizeof(line1), "预览 %d/%d: %s",
           previewDispMode + 1, TOTAL_DISP_MODES, dispModeNamesCN[previewDispMode]);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(ST77XX_CYAN);
  int w1 = u8g2.getUTF8Width(line1);
  u8g2.setCursor((240 - w1) / 2, 209);
  u8g2.print(line1);

  // 这行是按像素宽度卡着 240 写的：wqy16 下"< > 切换  回车确认  ESC取消"约 216 宽，
  // 再多一个空格就会溢出屏幕，别随手往里加字
  const char* hint = "< > 切换  回车确认  ESC取消";
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  int w2 = u8g2.getUTF8Width(hint);
  u8g2.setCursor((240 - w2) / 2, 231);
  u8g2.print(hint);

  currentDispMode = savedDispMode;
}

void applyStylePreview() {
  currentDispMode = previewDispMode;
  preferences.putUChar("disp_mode", currentDispMode);
  currentSysMode = SYS_MODE_NORMAL;
  screenNeedsRedraw = true;
}

void cancelStylePreview() {
  currentSysMode = SYS_MODE_MENU;
  drawMenuUI();
}

void updateMarquee() {
  if (currentDispMode != DISP_MODE_GEEK || hud.active || ringingKind != RING_NONE) return;
  unsigned long now = millis();
  if (now - lastMarqueeUpdate > 35) {
    lastMarqueeUpdate = now;
    tft.fillRect(15, 185, 210, 34, ST77XX_BLACK);

    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(marqueeScrollX, 207);
    u8g2.print(customMarquee);

    marqueeScrollX -= 3;
    int strPixelLen = u8g2.getUTF8Width(customMarquee.c_str());
    if (marqueeScrollX < -strPixelLen) marqueeScrollX = 220;
  }
}

void displayStatusCN(const char* title, uint16_t color) {
  tft.fillRoundRect(17, 67, 206, 106, 8, ST77XX_BLACK);
  tft.drawRoundRect(15, 65, 210, 110, 10, color);

  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(color);
  int w = u8g2.getUTF8Width(title);
  int x = 120 - w / 2;
  if (x < 22) x = 22;
  u8g2.setCursor(x, 128);
  u8g2.print(title);
}

// 录制模式专属 UI：顶部状态条 + 中部键流 + 底部提示。
// 调用者保证进入函数时只有 recNeedsRedraw==true 才会真正写屏。
// 240×240 屏的可用高度：标题 y10~30，中部 y40~200，底部 y210~230。
void drawRecUI() {
  if (!recNeedsRedraw) return;
  recNeedsRedraw = false;

  tft.fillScreen(ST77XX_BLACK);

  // 1. 顶部状态条
  uint16_t titleColor = (currentSysMode == SYS_MODE_REC_SEQ) ? ST77XX_YELLOW : ST77XX_MAGENTA;
  const char* titleStr = (currentSysMode == SYS_MODE_REC_SEQ) ? "录制击键流" : "录制组合模式";

  tft.fillRoundRect(4, 6, 232, 28, 6, 0x0000);
  tft.drawRoundRect(4, 6, 232, 28, 6, titleColor);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2.setForegroundColor(titleColor);
  u8g2.setCursor(14, 26);
  u8g2.print(titleStr);

  // 右上角：已录/上限
  char cntBuf[16];
  snprintf(cntBuf, sizeof(cntBuf), "%d/%d", recKeyCount, MAX_REC_KEYS);
  u8g2.setForegroundColor(ST77XX_CYAN);
  u8g2.setCursor(190, 26);
  u8g2.print(cntBuf);

  // 2. 分隔线
  tft.drawFastHLine(8, 40, 224, 0x39E7);

  // 3. 中部键流。把 recKeyBuffer[] 拼成一行按键名（SEQ 模式）或数字（CMB 模式），
  //    用 u8g2 的 UTF8 测量宽度，超过一行的部分折到下一行；行/列超出可视区就截断。
  u8g2.setForegroundColor(ST77XX_WHITE);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);

  const int TEXT_LEFT = 10;
  const int TEXT_TOP  = 60;     // 第一行基线 y
  const int LINE_H    = 22;     // 每行像素高（wqy16 + 行距）
  const int VISIBLE_LINES = 6;  // y60..y192 容纳 6 行
  const int LINE_MAX_W = 230;

  String line = "";
  int lines = 0;
  bool hasMore = false;   // recKeyCount 比可见区能装下的还多

  // 先把已录键全部序列化成字符串。SEQ 走人类可读，CMB 走纯数字列表。
  for (int i = 0; i < recKeyCount && lines < VISIBLE_LINES; i++) {
    String piece;
    if (currentSysMode == SYS_MODE_REC_SEQ) {
      piece = getKeyName(recKeyBuffer[i]);
    } else {
      piece = String(recKeyBuffer[i]);
    }
    if (i > 0) piece = "+" + piece;

    if (u8g2.getUTF8Width((line + piece).c_str()) > LINE_MAX_W) {
      // 当前行装不下 → 先画这一行，开新行
      u8g2.setCursor(TEXT_LEFT, TEXT_TOP + lines * LINE_H);
      u8g2.print(line);
      line = "";
      lines++;
      if (lines >= VISIBLE_LINES) { hasMore = (i + 1 < recKeyCount); break; }
      // 新行如果首片就超长，就直接放进去，下一行再继续
    }
    line += piece;
  }
  // 收尾的最后一行（如果循环没满）
  if (lines < VISIBLE_LINES && line.length() > 0) {
    u8g2.setCursor(TEXT_LEFT, TEXT_TOP + lines * LINE_H);
    u8g2.print(line);
    lines++;
  }
  // 看看是不是还有键没画上
  if (lines >= VISIBLE_LINES && !hasMore) {
    hasMore = (recKeyCount > 0);  // 兜底：主循环用 lines 提前 break 时已置位
  }

  // 还有没显示的键，底部加个省略号提示
  if (hasMore) {
    u8g2.setForegroundColor(ST77XX_DARKGREY);
    u8g2.setCursor(TEXT_LEFT, TEXT_TOP + (VISIBLE_LINES - 1) * LINE_H);
    u8g2.print("…");
  }

  // 4. 底部固定提示
  tft.drawFastHLine(8, 200, 224, 0x39E7);
  u8g2.setForegroundColor(ST77XX_GREEN);
  u8g2.setCursor(10, 220);
  u8g2.print("MR切模式/取消 M1-M12收尾");
}

void switchProfile(uint8_t profIdx) {
  if (profIdx >= TOTAL_PROFILES) return;
  currentProfile = profIdx;
  preferences.putUChar("curr_prof", profIdx);
  displayStatusCN(profileNamesCN[profIdx], ST77XX_GREEN);
  delay(300);
  screenNeedsRedraw = true;
}

// ================= 灯效与报警引擎 =================
void triggerKeyReaction() {
  isReactionActive = true;
  lastReactionUpdate = millis();
  reactionType = keypressStyle / 8;
  uint8_t colorIdx = keypressStyle % 8;

  if (colorIdx == 0) {
    static uint8_t autoIndex = 0;
    const uint32_t autoColors[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB400FF, 0x00FFFF, 0xFFFFFF };
    currentReactionColor = autoColors[autoIndex];
    autoIndex = (autoIndex + 1) % 6;
  } else {
    currentReactionColor = keypressColors[colorIdx];
  }

  if (reactionType == 1) {
    bool spawned = false;
    for (int i = 0; i < MAX_SHOOT_PROJECTILES; i++) {
      if (shootSteps[i] == -1) {
        shootSteps[i] = 0;
        shootColors[i] = currentReactionColor;
        spawned = true;
        break;
      }
    }
    if (!spawned) {
      shootSteps[0] = 0;
      shootColors[0] = currentReactionColor;
    }
  } else if (reactionType == 2) {
    if (reactionStep > 0) {
      stackCount++;
      if (stackCount >= NUM_MAIN_LEDS) stackCount = 0;
    }
    reactionStep = 0;
  } else {
    reactionStep = 0;
  }
}

void updateKeyReaction() {
  unsigned long now = millis();
  if (now - lastReactionUpdate > 25) {
    lastReactionUpdate = now;

    for (int i = 0; i < NUM_MAIN_LEDS; i++) {
      if (reactionType == 2 && i < stackCount) {
        setLedRGB(i, (currentReactionColor >> 16) & 0xFF, (currentReactionColor >> 8) & 0xFF, currentReactionColor & 0xFF);
        continue;
      }
      uint8_t r = frameBuffer[i][0] * 102 >> 8;
      uint8_t g = frameBuffer[i][1] * 102 >> 8;
      uint8_t b = frameBuffer[i][2] * 102 >> 8;
      setLedRGB(i, r, g, b);
    }

    uint8_t cr = (currentReactionColor >> 16) & 0xFF;
    uint8_t cg = (currentReactionColor >> 8) & 0xFF;
    uint8_t cb = currentReactionColor & 0xFF;

    if (reactionType == 0) {
      int left = (NUM_MAIN_LEDS / 2 - 1) - reactionStep;
      int right = (NUM_MAIN_LEDS / 2) + reactionStep;
      if (left >= 0) setLedRGB(left, cr, cg, cb);
      if (right < NUM_MAIN_LEDS) setLedRGB(right, cr, cg, cb);
      reactionStep++;
      if (reactionStep > NUM_MAIN_LEDS / 2) isReactionActive = false;
    } else if (reactionType == 1) {
      bool anyActive = false;
      for (int i = 0; i < MAX_SHOOT_PROJECTILES; i++) {
        if (shootSteps[i] >= 0) {
          int pos = (NUM_MAIN_LEDS - 1) - shootSteps[i];
          if (pos >= 0 && pos < NUM_MAIN_LEDS) {
            setLedRGB(pos, (shootColors[i] >> 16) & 0xFF, (shootColors[i] >> 8) & 0xFF, shootColors[i] & 0xFF);
          }
          shootSteps[i]++;
          if (shootSteps[i] >= NUM_MAIN_LEDS) shootSteps[i] = -1;
          else anyActive = true;
        }
      }
      if (!anyActive) isReactionActive = false;
    } else if (reactionType == 2) {
      if (reactionStep < NUM_MAIN_LEDS - stackCount) {
        setLedRGB((NUM_MAIN_LEDS - 1) - reactionStep, cr, cg, cb);
      }
      reactionStep++;
      if (reactionStep >= NUM_MAIN_LEDS - stackCount) {
        isReactionActive = false;
        stackCount++;
        if (stackCount >= NUM_MAIN_LEDS) stackCount = 0;
      }
    }
  }
}

void drawBreathing(uint8_t maxR, uint8_t maxG, uint8_t maxB) {
  float val = (exp(sin(effectFrame * 0.03)) - 0.36787944) * 108.0;
  float ratio = val / 255.0;
  if (ratio > 1.0) ratio = 1.0;
  if (ratio < 0.0) ratio = 0.0;
  setMainLedsColor(maxR * ratio, maxG * ratio, maxB * ratio);
  effectFrame++;
}

void renderMainEffects() {
  switch (currentEffect) {
    case 1: setMainLedsColor(255, 0, 0); break;
    case 2: setMainLedsColor(0, 255, 0); break;
    case 3: setMainLedsColor(0, 0, 255); break;
    case 4: setMainLedsColor(0, 127, 255); break;
    case 5: setMainLedsColor(255, 255, 255); break;
    case 6: drawBreathing(255, 0, 0); break;
    case 7: drawBreathing(0, 255, 0); break;
    case 8: drawBreathing(0, 0, 255); break;
    case 9: drawBreathing(0, 127, 255); break;
    case 10: {
      clearMainLeds();
      int totalSteps = (NUM_MAIN_LEDS - 1) * 2;
      int step = effectFrame % totalSteps;
      int pos = (step < NUM_MAIN_LEDS) ? step : (totalSteps - step);
      setLedRGB(pos, 255, 0, 50);
      for (int i = 0; i < NUM_MAIN_LEDS; i++) {
        int diff = abs(i - pos);
        if (diff == 1) setLedRGB(i, 80, 0, 15);
        else if (diff == 2) setLedRGB(i, 20, 0, 3);
      }
      effectFrame++;
      break;
    }
    case 11: {
      clearMainLeds();
      int totalSteps = (NUM_MAIN_LEDS - 1) * 2;
      int step = effectFrame % totalSteps;
      int pos1 = (step < NUM_MAIN_LEDS) ? step : (totalSteps - step);
      int pos2 = (step < NUM_MAIN_LEDS) ? (NUM_MAIN_LEDS - 1 - step) : (step - NUM_MAIN_LEDS + 1);
      setLedRGB(pos1, 180, 0, 255);
      setLedRGB(pos2, 0, 180, 255);
      for (int i = 0; i < NUM_MAIN_LEDS; i++) {
        if (abs(i - pos1) == 1) setLedRGB(i, 50, 0, 80);
        if (abs(i - pos2) == 1) setLedRGB(i, 0, 50, 80);
      }
      effectFrame++;
      break;
    }
    case 12: {
      for (int i = 0; i < NUM_MAIN_LEDS; i++) {
        uint32_t col = colorHSV(effectFrame + (i * 65536L / NUM_MAIN_LEDS), 255, 255);
        setLedRGB(i, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
      }
      effectFrame += 256;
      break;
    }
    default: clearMainLeds(); break;
  }
}

void renderIndicators() {
  // 16/17/18 是三颗状态指示灯。sendLedFrameToC3 只给 0~15 乘主背光亮度，
  // 所以这三颗的缩放得在这里自己做，用的是菜单里那一档 indBrightness。
  uint8_t b = indBrightness;
  setLedRGB(16, (uint8_t)((numLockActive ? 255 : 0) * b / 255), (uint8_t)((numLockActive ? 180 : 0) * b / 255), 0);
  setLedRGB(17, 0, (uint8_t)((capsLockActive ? 150 : 0) * b / 255), (uint8_t)((capsLockActive ? 255 : 0) * b / 255));
  setLedRGB(18, (uint8_t)((scrollLockActive ? 255 : 0) * b / 255), (uint8_t)((scrollLockActive ? 50 : 0) * b / 255), 0);
}

void renderLightingEngine() {
  unsigned long now = millis();

  // 响铃优先于一切，包括"背光总开关"：闹钟到点必须闪灯是需求原文，
  // 氛围灯的总开关不该把提醒本身吃掉。19 颗一起红闪，错过的可能性最小。
  // （系统通知的报警闪烁仍然照旧排在 g_forceOff 之后，行为不变）
  if (ringingKind != RING_NONE) {
    static unsigned long lastRingFlash = 0;
    static bool ringToggle = false;
    if (now - lastRingFlash > 120) {
      lastRingFlash = now;
      ringToggle = !ringToggle;
    }
    for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, ringToggle ? 255 : 0, 0, 0);
    return;
  }

  if (g_forceOff) {
    for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, 0, 0, 0);
    return;
  }

  // 灯光跟着通知队列的最新一条走：每确认掉一条，就自动换成前一条的颜色
  AlertType alert = currentAlertType();
  if (alert != ALERT_NONE) {
    static unsigned long lastAlertFlash = 0;
    static bool alertToggle = false;
    if (now - lastAlertFlash > 180) {
      lastAlertFlash = now;
      alertToggle = !alertToggle;
    }
    if (alertToggle) {
      uint8_t r = 0, g = 0, b = 0;
      if (alert == ALERT_RED) r = 255;
      else if (alert == ALERT_GREEN) g = 255;
      else if (alert == ALERT_YELLOW) { r = 255; g = 180; }
      for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, r, g, b);
    } else {
      clearMainLeds();
    }
    renderIndicators();
    return;
  }

  if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
    static unsigned long lastRecFlash = 0;
    static bool recFlashState = false;
    if (now - lastRecFlash > 200) {
      lastRecFlash = now;
      recFlashState = !recFlashState;
    }
    if (recFlashState) {
      uint8_t fr = (currentSysMode == SYS_MODE_REC_SEQ) ? 255 : 220;
      uint8_t fg = (currentSysMode == SYS_MODE_REC_SEQ) ? 180 : 0;
      uint8_t fb = (currentSysMode == SYS_MODE_REC_SEQ) ? 0 : 255;
      for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, fr, fg, fb);
    } else {
      for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, 0, 0, 0);
    }
    return;
  }

  if (isReactionActive) {
    updateKeyReaction();
  } else if (reactionType == 2 && stackCount > 0) {
    clearMainLeds();
    for (int i = 0; i < stackCount; i++) {
      setLedRGB(i, (currentReactionColor >> 16) & 0xFF, (currentReactionColor >> 8) & 0xFF, currentReactionColor & 0xFF);
    }
  } else {
    renderMainEffects();
  }

  renderIndicators();
}

void sendLedFrameToC3() {
  uint8_t packet[61];
  packet[0] = 0xAA;
  packet[1] = 0x55;
  packet[2] = 0x01;

  // 响铃时必须忽略背光总亮度：主背光的 0..15 号灯是按 brightness 缩放的，
  // 用户要是把背光拧到了最低，闹钟的"全红快闪"就是全黑——最需要它响的夜里恰好失效。
  // 响铃不是氛围灯，是提醒，所以强制满亮度。
  uint8_t ledScale = (ringingKind != RING_NONE) ? 255 : brightness;

  for (int i = 0; i < NUM_MAIN_LEDS; i++) {
    packet[3 + i * 3 + 0] = (uint8_t)((frameBuffer[i][0] * ledScale) / 255);
    packet[3 + i * 3 + 1] = (uint8_t)((frameBuffer[i][1] * ledScale) / 255);
    packet[3 + i * 3 + 2] = (uint8_t)((frameBuffer[i][2] * ledScale) / 255);
  }
  for (int i = NUM_MAIN_LEDS; i < TOTAL_LEDS; i++) {
    packet[3 + i * 3 + 0] = frameBuffer[i][0];
    packet[3 + i * 3 + 1] = frameBuffer[i][1];
    packet[3 + i * 3 + 2] = frameBuffer[i][2];
  }

  packet[60] = 0xEE;
  Serial1.write(packet, sizeof(packet));
}

void initBaseMatrix() {
  memset(baseMatrix, 0, sizeof(baseMatrix));

  baseMatrix[0][0] = KEY_LEFT_ALT;
  baseMatrix[0][1] = 0xE9; baseMatrix[0][2] = 0xE8; baseMatrix[0][3] = 0xE7;
  baseMatrix[0][4] = 0xDE; baseMatrix[0][5] = 0xDD; baseMatrix[0][6] = 0xDC;
  baseMatrix[0][7] = 0xDB; baseMatrix[0][8] = KEY_RIGHT_ARROW; baseMatrix[0][9] = KEY_DOWN_ARROW;
  baseMatrix[0][10] = KEY_LEFT_ARROW; baseMatrix[0][11] = KEY_RIGHT_CTRL;
  baseMatrix[0][12] = 0xED; baseMatrix[0][13] = K_FN; baseMatrix[0][14] = KEY_RIGHT_ALT; baseMatrix[0][15] = ' ';

  baseMatrix[1][0] = 0xDF; baseMatrix[1][1] = K_MB; baseMatrix[1][2] = K_MA;
  baseMatrix[1][3] = K_NEXT; baseMatrix[1][4] = K_PLAY; baseMatrix[1][5] = K_PREV; baseMatrix[1][6] = K_LOGO;
  baseMatrix[1][7] = 0xE0;  baseMatrix[1][8] = 0xEB;  baseMatrix[1][9] = 0xEA;
  baseMatrix[1][10] = 0xE3; baseMatrix[1][11] = 0xE2; baseMatrix[1][12] = 0xE1;
  baseMatrix[1][13] = 0xE6; baseMatrix[1][14] = 0xE5; baseMatrix[1][15] = 0xE4;

  baseMatrix[2][0] = KEY_LEFT_SHIFT; baseMatrix[2][1] = KEY_LEFT_GUI; baseMatrix[2][2] = KEY_LEFT_CTRL;
  baseMatrix[2][3] = KEY_UP_ARROW; baseMatrix[2][4] = KEY_RIGHT_SHIFT;
  baseMatrix[2][5] = '/'; baseMatrix[2][6] = '.'; baseMatrix[2][7] = ',';
  baseMatrix[2][8] = 'm'; baseMatrix[2][9] = 'n'; baseMatrix[2][10] = 'b';
  baseMatrix[2][11] = 'v'; baseMatrix[2][12] = 'c'; baseMatrix[2][13] = 'x'; baseMatrix[2][14] = 'z';

  baseMatrix[4][0] = KEY_END; baseMatrix[4][1] = KEY_RETURN; baseMatrix[4][3] = '\'';
  baseMatrix[4][4] = ';'; baseMatrix[4][5] = 'l'; baseMatrix[4][6] = 'k'; baseMatrix[4][7] = 'j';
  baseMatrix[4][8] = 'h'; baseMatrix[4][9] = 'g'; baseMatrix[4][10] = 'f'; baseMatrix[4][11] = 'd';
  baseMatrix[4][12] = 's'; baseMatrix[4][13] = 'a'; baseMatrix[4][14] = KEY_CAPS_LOCK;
  baseMatrix[4][15] = 0xD6;

  baseMatrix[5][0] = KEY_PAGE_UP; baseMatrix[5][1] = KEY_DELETE; baseMatrix[5][2] = '\\';
  baseMatrix[5][3] = ']'; baseMatrix[5][4] = '['; baseMatrix[5][5] = 'p'; baseMatrix[5][6] = 'o';
  baseMatrix[5][7] = 'i'; baseMatrix[5][8] = 'u'; baseMatrix[5][9] = 'y'; baseMatrix[5][10] = 't';
  baseMatrix[5][11] = 'r'; baseMatrix[5][12] = 'e'; baseMatrix[5][13] = 'w'; baseMatrix[5][14] = 'q';
  baseMatrix[5][15] = KEY_TAB;

  baseMatrix[6][0] = '`'; baseMatrix[6][1] = KEY_HOME; baseMatrix[6][2] = KEY_INSERT;
  baseMatrix[6][3] = KEY_BACKSPACE; baseMatrix[6][4] = '='; baseMatrix[6][5] = '-';
  baseMatrix[6][6] = '0'; baseMatrix[6][7] = '9'; baseMatrix[6][8] = '8'; baseMatrix[6][9] = '7';
  baseMatrix[6][10] = '6'; baseMatrix[6][11] = '5'; baseMatrix[6][12] = '4'; baseMatrix[6][13] = '3';
  baseMatrix[6][14] = '2'; baseMatrix[6][15] = '1';

  baseMatrix[7][0] = KEY_ESC;
  baseMatrix[7][1] = 0xD0; baseMatrix[7][2] = 0xCF; baseMatrix[7][3] = 0xCE;
  baseMatrix[7][4] = KEY_F12; baseMatrix[7][5] = KEY_F11; baseMatrix[7][6] = KEY_F10; baseMatrix[7][7] = KEY_F9;
  baseMatrix[7][8] = KEY_F8; baseMatrix[7][9] = KEY_F7; baseMatrix[7][10] = KEY_F6; baseMatrix[7][11] = KEY_F5;
  baseMatrix[7][12] = KEY_F4; baseMatrix[7][13] = KEY_F3; baseMatrix[7][14] = KEY_F2; baseMatrix[7][15] = KEY_F1;

  baseMatrix[8][0] = K_MC; baseMatrix[8][2] = K_ME; baseMatrix[8][3] = K_M12;
  baseMatrix[8][4] = K_M11; baseMatrix[8][5] = K_M10; baseMatrix[8][6] = K_M9; baseMatrix[8][7] = K_M8;
  baseMatrix[8][8] = K_M7; baseMatrix[8][9] = K_M6; baseMatrix[8][10] = K_M5; baseMatrix[8][11] = K_M4;
  baseMatrix[8][12] = K_M3; baseMatrix[8][13] = K_M2; baseMatrix[8][14] = K_M1; baseMatrix[8][15] = K_MR;
}

void recoverI2CBus() {
  Wire.end();
  delay(10);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(25);
  if (mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
    for (int c = 0; c < numCols; c++) {
      mcp.pinMode(c, OUTPUT);
      mcp.digitalWrite(c, HIGH);
    }
  }
}

String getKeyName(uint16_t code) {
  if (code >= 'a' && code <= 'z') return String((char)(code - 32));
  if (code >= '0' && code <= '9') return String((char)code);
  switch (code) {
    case KEY_LEFT_CTRL: case KEY_RIGHT_CTRL: return "Ctrl";
    case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return "Shift";
    case KEY_LEFT_ALT: case KEY_RIGHT_ALT: return "Alt";
    case KEY_LEFT_GUI: return "Win/Cmd";
    case KEY_RETURN: return "Enter";
    case KEY_ESC: return "Esc";
    case KEY_BACKSPACE: return "Back";
    case KEY_TAB: return "Tab";
    case ' ': return "Space";
    case KEY_UP_ARROW: return "UP";
    case KEY_DOWN_ARROW: return "DOWN";
    case KEY_LEFT_ARROW: return "LEFT";
    case KEY_RIGHT_ARROW: return "RIGHT";
    // --- 编辑/导航区 ---
    case KEY_INSERT: return "Ins";
    case KEY_DELETE: return "Del";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case KEY_PAGE_UP: return "PgUp";
    case KEY_PAGE_DOWN: return "PgDn";
    case KEY_CAPS_LOCK: return "Caps";
    case KEY_MENU: return "Menu";
    case KEY_PRINT_SCREEN: return "PrtSc";
    case KEY_SCROLL_LOCK: return "Scrlk";
    case KEY_PAUSE: return "Pause";
    // --- 小键盘（右侧数字区）---
    case KEY_NUM_LOCK: return "NumLock";
    case KEY_KP_SLASH: return "Num /";
    case KEY_KP_ASTERISK: return "Num *";
    case KEY_KP_MINUS: return "Num -";
    case KEY_KP_PLUS: return "Num +";
    case KEY_KP_ENTER: return "Num Ent";
    case KEY_KP_0: return "Num 0";
    case KEY_KP_DOT: return "Num .";
    // --- 本机自定义键 ---
    case K_FN: return "Fn";
    case K_LOGO: return "Logo";
    case K_PREV: return "Prev";
    case K_PLAY: return "Play";
    case K_NEXT: return "Next";
    case K_MA: return "MA";
    case K_MB: return "MB";
    case K_MC: return "MC";
    case K_MR: return "MR";
    case K_ME: return "ME";
    default:
      if (code >= KEY_F1 && code <= KEY_F12) return "F" + String(code - KEY_F1 + 1);
      if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
      if (code >= KEY_KP_1 && code <= KEY_KP_9) return "Num " + String(code - KEY_KP_1 + 1);
      return "Key";
  }
}

void executeComboAction(String cmb) {
  while (cmb.length() > 0) {
    int comma = cmb.indexOf(',');
    uint8_t code = (comma == -1) ? cmb.toInt() : cmb.substring(0, comma).toInt();
    if (code > 0) Keyboard.press(code);
    if (comma == -1) break;
    cmb = cmb.substring(comma + 1);
  }
  safeDelayMs(50);
  Keyboard.releaseAll();
}

void executeSequenceAction(String seq) {
  int i = 0;
  while (i < seq.length()) {
    if (seq.substring(i).startsWith("SLEEP(")) {
      int endP = seq.indexOf(')', i + 6);
      if (endP != -1) {
        int sleepMs = seq.substring(i + 6, endP).toInt();
        if (sleepMs > 0) safeDelayMs(sleepMs);
        i = endP + 1;
        continue;
      }
    }
    else if (seq.substring(i).startsWith("[SLEEP:")) {
      int endB = seq.indexOf(']', i + 7);
      if (endB != -1) {
        int sleepMs = seq.substring(i + 7, endB).toInt();
        if (sleepMs > 0) safeDelayMs(sleepMs);
        i = endB + 1;
        continue;
      }
    }
    else if (seq[i] == '[') {
      int endB = seq.indexOf(']', i);
      if (endB != -1) {
        String tag = seq.substring(i + 1, endB);
        bool matched = true;
        if (tag == "ENTER") Keyboard.write(KEY_RETURN);
        else if (tag == "TAB") Keyboard.write(KEY_TAB);
        else if (tag == "ESC") Keyboard.write(KEY_ESC);
        else if (tag == "BACKSPACE") Keyboard.write(KEY_BACKSPACE);
        else matched = false;
        if (matched) { i = endB + 1; continue; }
      }
    }
    Keyboard.print(seq[i++]);
    safeDelayMs(5);
  }
}

String getMacroNameByCode(uint16_t code) {
  if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
  if (code == K_MA) return "MA";
  if (code == K_MB) return "MB";
  if (code == K_MC) return "MC";
  if (code == K_MR) return "MR";
  if (code == K_ME) return "ME";
  if (code == K_LOGO) return "LOGO";
  return "";
}

void executeGlobalKey(String gKey) {
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

    if (plusIdx != -1) {
      String extra = val.substring(plusIdx + 1);
      if (extra.startsWith("CMB:")) executeComboAction(extra.substring(4));
      else if (extra.startsWith("SEQ:")) executeSequenceAction(extra.substring(4));
    }
    return;
  }

  if (val.startsWith("CMB:")) executeComboAction(val.substring(4));
  else if (val.startsWith("SEQ:")) executeSequenceAction(val.substring(4));
}

void executeMacro(String keyName) {
  if (keyName == "ME") {
    if (FFat.exists("/me_hex.txt")) {
      File f = FFat.open("/me_hex.txt", FILE_READ);
      if (f) {
        Keyboard.print("[HEXS]");
        safeDelayMs(20);
        while (f.available()) {
          Keyboard.print((char)f.read());
          safeDelayMs(1);
        }
        f.close();
        safeDelayMs(20);
        Keyboard.print("[HEXE]");
        return;
      }
    }
  }

  char pKey[32];
  snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, keyName.c_str());
  String macroData = preferences.getString(pKey, "");
  if (macroData.length() == 0) return;

  if (macroData.startsWith("SEQ:")) executeSequenceAction(macroData.substring(4));
  else if (macroData.startsWith("CMB:")) executeComboAction(macroData.substring(4));
}

void finishMacroRecording(String targetKey) {
  if (recKeyCount == 0) {
    currentSysMode = SYS_MODE_NORMAL;
    recNeedsRedraw = false;            // 退出录制，UI 不再刷
    displayStatusCN("取消录制", ST77XX_RED);
    delay(300);
    screenNeedsRedraw = true;
    return;
  }

  String payload = "";
  if (currentSysMode == SYS_MODE_REC_SEQ) {
    payload = "SEQ:";
    for (int i = 0; i < recKeyCount; i++) {
      uint16_t c = recKeyBuffer[i];
      if (c == KEY_RETURN) payload += "[ENTER]";
      else if (c == KEY_TAB) payload += "[TAB]";
      else if (c == KEY_ESC) payload += "[ESC]";
      else if (c == KEY_BACKSPACE) payload += "[BACKSPACE]";
      else if (c >= 32 && c <= 126) payload += (char)c;
    }
  } else if (currentSysMode == SYS_MODE_REC_CMB) {
    payload = "CMB:";
    for (int i = 0; i < recKeyCount; i++) {
      payload += String(recKeyBuffer[i]);
      if (i < recKeyCount - 1) payload += ",";
    }
  }

  char pKey[32];
  snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, targetKey.c_str());
  preferences.putString(pKey, payload);

  currentSysMode = SYS_MODE_NORMAL;
  recNeedsRedraw = false;              // 退出录制，UI 不再刷
  displayStatusCN("录制已保存", ST77XX_GREEN);
  delay(400);
  screenNeedsRedraw = true;
}

void handleMenuSelect();

// ================= 键盘扫描（最高优先级） =================
void scanKeyboardMatrix() {
  static unsigned long lastI2CCheck = 0;
  if (millis() - lastI2CCheck > 500) {
    lastI2CCheck = millis();
    Wire.beginTransmission(MCP23017_ADDR);
    if (Wire.endTransmission() != 0) {
      recoverI2CBus();
      return;
    }
  }

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

          // ---------------- 按键按下 ----------------
          if (currentState) {
            lastActivityTime = millis();
            if (currentSysMode == SYS_MODE_SLEEP) {
              currentSysMode = SYS_MODE_NORMAL;
              screenNeedsRedraw = true;
              return;
            }

            // 0. 响铃优先。确认键直接消铃；非主屏的模式一律冻住，只放行主屏下的打字。
            //
            //    这里用 != SYS_MODE_NORMAL 而不是逐个枚举模式：宏录制（REC_SEQ /
            //    REC_CMB）漏掉的话，响铃期间按的键会被静默写进 recKeyBuffer，
            //    而响铃卡片正好盖住录制提示，用户完全看不到，录出来的宏是脏的。
            if (ringingKind != RING_NONE) {
              if (baseKey == KEY_RETURN || baseKey == KEY_ESC || baseKey == K_MC) {
                stopRinging();
                return;
              }
              if (currentSysMode != SYS_MODE_NORMAL) return;

              // 主屏下打字照放，但 Fn 那几个系统级组合键要挡住：被闹钟吵醒时
              // 乱拍键盘，很容易顺手把电脑关了（Fn+上一曲）或者把键盘重启了（Fn+LOGO）
              if (fnPressed && (baseKey == K_LOGO || baseKey == K_PREV
                                || baseKey == K_PLAY || baseKey == K_NEXT)) {
                return;
              }
            }

            totalKeyCount++;

            // 触发局部重绘标记，绝不全屏刷新
            lastKeyStrokeName = getKeyName(baseKey);
            keyStrokeDisplayTime = millis();
            keystrokeNeedsRedraw = true;

            // 1. 风格预览接管：左右对比，回车二次确认，ESC/MC 放弃
            if (currentSysMode == SYS_MODE_STYLE_PREVIEW) {
              if (baseKey == KEY_LEFT_ARROW || baseKey == KEY_UP_ARROW) {
                previewDispMode = (previewDispMode == 0) ? TOTAL_DISP_MODES - 1 : previewDispMode - 1;
                renderStylePreview();
              } else if (baseKey == KEY_RIGHT_ARROW || baseKey == KEY_DOWN_ARROW) {
                previewDispMode = (previewDispMode + 1) % TOTAL_DISP_MODES;
                renderStylePreview();
              } else if (baseKey == KEY_RETURN) {
                applyStylePreview();
              } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                cancelStylePreview();
              }
              return;
            }

            // 2. 菜单模式接管
            if (currentSysMode == SYS_MODE_MENU) {
              if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
                menuMove(-1);
              } else if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                menuMove(1);
              } else if (baseKey == KEY_RETURN) {
                handleMenuSelect();
              } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                currentSysMode = SYS_MODE_NORMAL;
                screenNeedsRedraw = true;
              }
              return;
            }

            // 2.5 三个设置子界面接管：左右切字段、上下调值、回车保存、ESC 取消
            if (IS_SETTING_MODE(currentSysMode)) {
              if (baseKey == KEY_LEFT_ARROW)        moveSettingField(-1);
              else if (baseKey == KEY_RIGHT_ARROW)  moveSettingField(1);
              else if (baseKey == KEY_UP_ARROW)     adjustSettingField(1);
              else if (baseKey == KEY_DOWN_ARROW)   adjustSettingField(-1);
              else if (baseKey == KEY_RETURN)       saveSettingScreen();
              else if (baseKey == KEY_ESC || baseKey == K_MC) cancelSettingScreen();
              return;
            }

            // 3. MR 宏现场录制
            if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
              if (baseKey >= K_M1 && baseKey <= K_M12) {
                finishMacroRecording(getMacroNameByCode(baseKey));
                return;
              }
              if (baseKey == K_MR) {
                if (currentSysMode == SYS_MODE_REC_SEQ) {
                  currentSysMode = SYS_MODE_REC_CMB;
                  recKeyCount = 0;
                  recNeedsRedraw = true;             // 模式切换 + 清空 → 重画
                  displayStatusCN("录制组合模式", ST77XX_MAGENTA);
                } else {
                  currentSysMode = SYS_MODE_NORMAL;
                  recNeedsRedraw = false;            // 取消录制 → 不再刷
                  displayStatusCN("录制已取消", ST77XX_RED);
                  delay(300);
                  screenNeedsRedraw = true;
                }
                return;
              }
              if (recKeyCount < MAX_REC_KEYS && baseKey < MACRO_BASE) {
                recKeyBuffer[recKeyCount++] = baseKey;
                recNeedsRedraw = true;               // 每收一颗键就追加键流
              }
              return;
            }

            // 4. MC 系统菜单键
            if (baseKey == K_MC) {
              currentSysMode = SYS_MODE_MENU;
              menuCursor = 0;
              menuScroll = 0;
              drawMenuUI();
              return;
            }

            // 5. MA / MB 全局键
            if (baseKey == K_MA) {
              executeGlobalKey("MA");
              return;
            } else if (baseKey == K_MB) {
              executeGlobalKey("MB");
              return;
            }

            // 6. MR 录制键
            if (baseKey == K_MR) {
              currentSysMode = SYS_MODE_REC_SEQ;
              recKeyCount = 0;
              recNeedsRedraw = true;                 // 进入录制 → 重画键流屏
              displayStatusCN("录制击键流", ST77XX_YELLOW);
              return;
            }

            if (cherryLogoEnabled) triggerKeyReaction();

            // 律动屏靠这个把律动打到最激烈、并丢一个键帽下来。
            // 这一行只在普通模式的普通按键上可达：菜单/预览/录制模式上面早就 return 了。
            rhythmNotifyKey(lastKeyStrokeName);

            // 7. 普通按键及媒体按键
            if (baseKey == K_FN) {
              fnPressed = true;
            } else if (baseKey >= MACRO_BASE) {
              if (baseKey == K_LOGO && fnPressed) {
                // 别在这里直接 esp_restart()：屏幕来不及画出任何东西，
                // 用户只会看到"黑了一下"。先弹提示，600ms 后由 loop 真正重启。
                triggerHud("LOGO + Fn", "系统重启中…", -1, ST77XX_RED);
                pendingRestartMs = millis() + 600;
              } else if (baseKey == K_PLAY) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_STANDBY); SystemControl.release(); }
                else {
                  ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                  mediaPlaying = !mediaPlaying;
                  triggerHud("媒体播放", mediaPlaying ? "播放" : "暂停", -1, ST77XX_GREEN);
                }
              } else if (baseKey == K_NEXT) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_WAKE_HOST); SystemControl.release(); Keyboard.write(' '); }
                else {
                  ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
                  mediaPlaying = true;
                  triggerHud("媒体播放", "下一曲", -1, ST77XX_CYAN);
                }
              } else if (baseKey == K_PREV) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_POWER_OFF); SystemControl.release(); }
                else {
                  ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
                  mediaPlaying = true;
                  triggerHud("媒体播放", "上一曲", -1, ST77XX_CYAN);
                }
              } else if (baseKey != K_LOGO) {
                // M1-M12 走两层：先按 g_<KEY> 配置执行全局动作（切方案+发组合/打字），
                // 再按方案宏 p<prof>_<KEY> 播放用户录的击键流/组合键。
                // 两层都靠 preferences 空串 fallback：无配置即跳过，互不打架。
                String macroName = getMacroNameByCode(baseKey);
                if (baseKey >= K_M1 && baseKey <= K_M12) {
                  executeGlobalKey(macroName);
                }
                executeMacro(macroName);
              }
            } else {
              uint16_t mappedKey = getMappedKey(baseKey);
              Keyboard.press((uint8_t)mappedKey);
            }
          }
          // ---------------- 按键释放 ----------------
          else {
            if (currentSysMode == SYS_MODE_MENU || currentSysMode == SYS_MODE_STYLE_PREVIEW
                || currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB
                || IS_SETTING_MODE(currentSysMode)) {
              return;
            }
            if (baseKey == K_FN) {
              fnPressed = false;
            } else if (baseKey < MACRO_BASE) {
              uint16_t mappedKey = getMappedKey(baseKey);
              Keyboard.release((uint8_t)mappedKey);
            } else {
              if (baseKey == K_PLAY || baseKey == K_NEXT || baseKey == K_PREV) {
                ConsumerControl.release();
              } else if (baseKey == K_LOGO) {
                if (!fnPressed) {
                  cherryLogoEnabled = !cherryLogoEnabled;
                  preferences.putBool("cherryLogo", cherryLogoEnabled);
                  // 这个键以前按下去屏幕毫无反馈，用户根本不知道它干了什么
                  triggerHud("LOGO 键", cherryLogoEnabled ? "动效 开启" : "动效 关闭", -1, ST77XX_MAGENTA);
                  executeMacro("LOGO");
                }
              }
            }
          }
        }
      }
    }
  }
  mcp.writeGPIOAB(0xFFFF);
}

void handleMenuSelect() {
  switch (menuCursor) {
    case 0:
      currentSysMode = SYS_MODE_NORMAL;
      screenNeedsRedraw = true;
      break;
    case 1:
      // 不做直接切换，先带着当前风格进入预览，让用户左右对比后再确认
      previewDispMode = currentDispMode;
      hud.active = false;
      currentSysMode = SYS_MODE_STYLE_PREVIEW;
      renderStylePreview();
      break;
    case 2:
      switchProfile((currentProfile + 1) % TOTAL_PROFILES);
      break;
    case 3:
      showKeystrokes = !showKeystrokes;
      preferences.putBool("show_keys", showKeystrokes);
      displayStatusCN(showKeystrokes ? "按键回显:开" : "按键回显:关", ST77XX_CYAN);
      delay(400);
      drawMenuUI();
      break;
    case 4:
      currentEffect = (currentEffect + 1) % MAX_EFFECTS;
      if (currentEffect == 0) currentEffect = 1;
      drawMenuUI();
      break;
    case 5: {
      // 状态指示灯亮度：关/低/中/高 循环。灯效引擎每 20ms 会自己重发一帧灯带数据，
      // 所以这里改完不用手动推送，下一帧就生效。
      indLevel = (indLevel + 1) % IND_LEVEL_COUNT;
      indBrightness = indLevelValues[indLevel];
      preferences.putUChar("ind_level", indLevel);
      char indBuf[24];
      snprintf(indBuf, sizeof(indBuf), "状态灯: %s", indLevelNames[indLevel]);
      displayStatusCN(indBuf, ST77XX_WHITE);
      delay(400);
      drawMenuUI();
      break;
    }
    case 6: {
      // 进界面时把手上的编辑值填成"当前时间"，用户看到的是现状而不是一堆零
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);
      if (t != NULL && t->tm_year > 120) {
        timeEditY  = t->tm_year + 1900;
        timeEditMo = t->tm_mon + 1;
        timeEditD  = t->tm_mday;
        timeEditH  = t->tm_hour;
        timeEditMi = t->tm_min;
      } else {
        // 从来没对过时：给个像样的起点，省得从 1970 年往上翻
        timeEditY = 2026; timeEditMo = 1; timeEditD = 1; timeEditH = 0; timeEditMi = 0;
      }
      clampTimeEdit();
      timeFieldIdx = 0;
      hud.active = false;
      currentSysMode = SYS_MODE_SET_TIME;
      renderTimeSetUI();
      break;
    }
    case 7:
      alarmEditOn = alarmEnabled;
      alarmEditH = alarmHour;
      alarmEditM = alarmMinute;
      alarmFieldIdx = 0;
      hud.active = false;
      currentSysMode = SYS_MODE_SET_ALARM;
      renderAlarmSetUI();
      break;
    case 8:
      timerEditH = timerSetH;
      timerEditM = timerSetM;
      timerEditS = timerSetS;
      timerFieldIdx = 0;
      hud.active = false;
      currentSysMode = SYS_MODE_SET_TIMER;
      renderTimerSetUI();
      break;
    case 9:
      totalKeyCount = 0;
      preferences.putUInt("keyCount", 0);
      displayStatusCN("计数已清零", ST77XX_GREEN);
      delay(400);
      drawMenuUI();
      break;
    case 10: {
      // 主动拉一次温湿度，并把下次定时器起点对齐，避免读完立刻又触发一次。
      // 失败也要给用户一个状态提示，免得点了之后以为没生效。
      if (!shtAvailable) {
        displayStatusCN("SHT31 未连接", ST77XX_RED);
      } else {
        sht31_update();
        lastSHTRead = millis();
        // 屏幕上的极客仪表盘可能没在显示，这里直接弹一行温度湿度让用户看到结果
        char th[32];
        snprintf(th, sizeof(th), "T=%.1fC  H=%.1f%%", shtTemperature, shtHumidity);
        displayStatusCN(th, ST77XX_CYAN);
      }
      delay(600);
      drawMenuUI();
      break;
    }
  }
}

void handleC3Events() {
  static String serialBuffer = "";
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      serialBuffer.trim();
      lastActivityTime = millis();
      if (currentSysMode == SYS_MODE_SLEEP) {
        currentSysMode = SYS_MODE_NORMAL;
        screenNeedsRedraw = true;
      }

      if (serialBuffer == "PONG") {
        c3Connected = true;
      }
      else if (serialBuffer.startsWith("ENC:")) {
        bool isRight = (serialBuffer == "ENC:+");
        if (ringingKind != RING_NONE) {
          // 响铃期间旋钮故意不作数：手肘碰到旋钮就把闹钟按掉，那还不如不响。
          // 消铃只认"按下去"这类明确动作（灯光键 / 旋钮按下 / 回车）。
        } else if (currentSysMode == SYS_MODE_MENU) {
          menuMove(isRight ? 1 : -1);
        } else if (IS_SETTING_MODE(currentSysMode)) {
          adjustSettingField(isRight ? 1 : -1);
        } else if (currentSysMode == SYS_MODE_STYLE_PREVIEW) {
          if (isRight) previewDispMode = (previewDispMode + 1) % TOTAL_DISP_MODES;
          else previewDispMode = (previewDispMode == 0) ? TOTAL_DISP_MODES - 1 : previewDispMode - 1;
          renderStylePreview();
        } else {
          if (g_forceOff) g_forceOff = false;
          if (currentMode == MODE_LIGHT) {
            if (isRight) brightness = (brightness <= 235) ? brightness + 20 : 255;
            else brightness = (brightness >= 20) ? brightness - 20 : 0;
            triggerHud("键盘背光", String(brightness * 100 / 255).c_str(), brightness * 100 / 255, ST77XX_YELLOW);
          } else if (currentMode == MODE_SCREEN_BRIGHTNESS) {
            if (isRight) { ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_INCREMENT); ConsumerControl.release(); }
            else { ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_DECREMENT); ConsumerControl.release(); }
            triggerHud("屏幕亮度", isRight ? "+ 调亮" : "- 调暗", -1, ST77XX_ORANGE);
          } else if (currentMode == MODE_MUTE) {
            if (isRight) { ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT); ConsumerControl.release(); }
            else { ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT); ConsumerControl.release(); }
            triggerHud("系统音量", isRight ? "+ 增加" : "- 减少", -1, ST77XX_GREEN);
          } else if (currentMode == MODE_CPG) {
            if (isRight) currentEffect = (currentEffect + 1) % MAX_EFFECTS;
            else currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
            triggerHud("灯效切换", effectNames[currentEffect], -1, ST77XX_MAGENTA);
          } else if (currentMode == MODE_KEY_COLOR) {
            if (isRight) keypressStyle = (keypressStyle + 1) % 24;
            else keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
            triggerKeyReaction();
            triggerHud("按键动效", String(keypressStyle + 1).c_str(), -1, ST77XX_CYAN);
          }
        }
      }
      else if (serialBuffer == "BTN:KNOB") {
        if (ringingKind != RING_NONE) {
          stopRinging();          // 旋钮按下去也算确认
        } else if (currentSysMode == SYS_MODE_MENU) {
          handleMenuSelect();
        } else if (IS_SETTING_MODE(currentSysMode)) {
          saveSettingScreen();
        } else if (currentSysMode == SYS_MODE_STYLE_PREVIEW) {
          applyStylePreview(); // 二次确认，正式切换
        } else {
          if (g_forceOff) g_forceOff = false;
          currentMode = MODE_KEY_COLOR;
          triggerKeyReaction();
          triggerHud("旋钮模式", "按键动效选择", -1, ST77XX_CYAN);
        }
      }
      else if (serialBuffer == "BTN:LIGHT") {
        // 响铃排在通知前面：闹钟响着的时候这一下是"我听见了"，
        // 不该被理解成"确认掉最新那条通知"。
        if (ringingKind != RING_NONE) {
          stopRinging();
        }
        // 有通知待处理时，单击灯光键 = "我已知晓"：只处理掉最新的一条，
        // 剩下的继续显示，全清完灯自己就灭了。这一下不切换控制目标，
        // 免得通知刚确认完旋钮就调错东西。
        else if (acknowledgeAlert()) {
          // 已确认，本次点击到此为止
        } else if (g_forceOff) {
          g_forceOff = false;
          currentMode = MODE_LIGHT;
          triggerHud("控制目标", "键盘背光", -1, ST77XX_YELLOW);
        } else {
          currentMode = (currentMode == MODE_LIGHT) ? MODE_SCREEN_BRIGHTNESS : MODE_LIGHT;
          triggerHud("控制目标", (currentMode == MODE_LIGHT) ? "键盘背光" : "显示器亮度", -1, ST77XX_YELLOW);
        }
      } else if (serialBuffer == "BTN:LIGHT_HOLD") {
        // 响铃时"长按灯光键"是最自然的"让它停下"手势。不特判的话这里只会翻转
        // g_forceOff 并弹一个 HUD，而响铃期间 HUD 被压制，用户看不到任何反馈，
        // 铃照响、背光却悄悄变成了全灭。
        if (ringingKind != RING_NONE) {
          stopRinging();
        } else {
          g_forceOff = !g_forceOff;
          triggerHud("背光总开关", g_forceOff ? "已关闭" : "已开启", -1, ST77XX_RED);
        }
      } else if (serialBuffer == "BTN:MUTE") {
        if (g_forceOff) g_forceOff = false;
        currentMode = MODE_MUTE;
        ConsumerControl.press(CONSUMER_CONTROL_MUTE);
        ConsumerControl.release();
        triggerHud("静音控制", "静音切换", -1, ST77XX_GREEN);
      } else if (serialBuffer == "BTN:MUTE_HOLD") {
        // 响铃期间长按不当重启用：重启后 alarmLastFiredYday 归零，而墙钟还落在
        // 同一个闹钟分钟内，开机会再响一次——长按几下就成死循环了。
        // 先消铃；真要重启，再长按一次。
        if (ringingKind != RING_NONE) {
          stopRinging();
        } else {
          delay(200);
          esp_restart();
        }
      } else if (serialBuffer == "BTN:CPG") {
        if (g_forceOff) g_forceOff = false;
        if (currentMode != MODE_CPG) currentMode = MODE_CPG;
        else {
          currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          if (currentEffect == 0) currentEffect = 1;
        }
        triggerHud("灯效切换", effectNames[currentEffect], -1, ST77XX_MAGENTA);
      } else if (serialBuffer == "BTN:CPG_HOLD") {
        // 同上：响应铃时先消铃，别直接进下载模式重启
        if (ringingKind != RING_NONE) {
          stopRinging();
        } else {
          delay(200);
          REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
          esp_restart();
        }
      }

      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
      if (serialBuffer.length() > 64) serialBuffer = "";
    }
  }
}

void handleUsbSerialCommands() {
  static String usbBuffer = "";
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      handleCommand(usbBuffer);
      usbBuffer = "";
    } else if (c != '\r') {
      usbBuffer += c;
      if (usbBuffer.length() > 256) usbBuffer = "";
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  setenv("TZ", "CST-8", 1);
  tzset();

  USB.VID(0x303A);
  USB.PID(0x001F);
  USB.productName("YYQ-MX9.0");
  USB.manufacturerName("YYQ");

  Keyboard.onEvent(usbHidKeyboardEvent);
  Keyboard.begin();
  ConsumerControl.begin();
  SystemControl.begin();
  VendorHID.onEvent(onHidVendorEvent);
  VendorHID.begin();
  USB.begin();

  // 1. 初始化高速 40MHz SPI 通道，杜绝总线传输延迟
  tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
  tft.init(240, 240);
  tft.setSPISpeed(40000000); // 40MHz 极速通讯
  tft.setRotation(1);

  u8g2.begin(tft);
  u8g2.setFontMode(1);
  u8g2.setFont(u8g2_font_wqy16_t_gb2312);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(25);
  if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) recoverI2CBus();

  // 初始化 SHT31 温湿度传感器（使用独立 I2C 总线 Wire_SHT）
  Wire_SHT.begin(SHT31_SDA, SHT31_SCL);
  Wire_SHT.setClock(400000);
  Wire_SHT.beginTransmission(SHT31_ADDR);
  if (Wire_SHT.endTransmission() == 0) {
    shtAvailable = true;
    Serial.println("SHT31 温湿度传感器初始化成功");
    // 开机主动拉一次：避免开机到第一次 15 分钟定时器触发之间的空窗
    // 屏幕上一直显示 "0.0C 0.0%"。lastSHTRead 顺手对齐，下次自动读在 15 分钟后。
    sht31_update();
    lastSHTRead = millis();
  } else {
    Serial.println("SHT31 温湿度传感器未找到");
  }

  preferences.begin("keyboard", false);
  currentProfile = preferences.getUChar("curr_prof", 0);
  if (currentProfile >= TOTAL_PROFILES) currentProfile = 0;

  // 风格顺序在这一版重排过，NVS 里存的是旧编号。按旧表映射一次，
  // 老用户升级完开机不会发现"我原来的风格怎么自己变了"。只做一次，做完记版本号。
  currentDispMode = preferences.getUChar("disp_mode", 0);
  if (preferences.getUChar("disp_ver", 0) < DISP_ORDER_VER) {
    if (currentDispMode < TOTAL_DISP_MODES) {
      // 旧表：0极客 1大字时钟 2击键监控 3壁纸 4信息面板 5律动
      static const uint8_t DISP_OLD_TO_NEW[6] = {
        DISP_MODE_GEEK, DISP_MODE_BIG_CLOCK, DISP_MODE_KEY_MON,
        DISP_MODE_WALLPAPER, DISP_MODE_INFO_PANEL, DISP_MODE_RHYTHM
      };
      currentDispMode = DISP_OLD_TO_NEW[currentDispMode];
    }
    preferences.putUChar("disp_mode", currentDispMode);
    preferences.putUChar("disp_ver", DISP_ORDER_VER);
  }
  if (currentDispMode >= TOTAL_DISP_MODES) currentDispMode = 0;

  showKeystrokes = preferences.getBool("show_keys", true);
  cherryLogoEnabled = preferences.getBool("cherryLogo", false);
  totalKeyCount = preferences.getUInt("keyCount", 0);
  customMarquee = preferences.getString("marquee", "YYQ Studio - 极客机械大师");
  indLevel = preferences.getUChar("ind_level", 3);
  if (indLevel >= IND_LEVEL_COUNT) indLevel = 3;
  indBrightness = indLevelValues[indLevel];

  // 闹钟 / 倒计时。越界的存档一律回落到默认值——NVS 里的东西不可全信。
  alarmEnabled = preferences.getBool("alarm_on", false);
  alarmHour = preferences.getUChar("alarm_h", 7);
  alarmMinute = preferences.getUChar("alarm_m", 0);
  if (alarmHour > 23) alarmHour = 7;
  if (alarmMinute > 59) alarmMinute = 0;

  timerSetH = preferences.getUChar("tmr_h", 0);
  timerSetM = preferences.getUChar("tmr_m", 5);
  timerSetS = preferences.getUChar("tmr_s", 0);
  if (timerSetH > 23) timerSetH = 0;
  if (timerSetM > 59) timerSetM = 5;
  if (timerSetS > 59) timerSetS = 0;

  // 拔电重上电之后 RTC 是空的（time() 只会返回"开机了多少秒"，年份是 1970）。
  // 之前手动设过或从电脑同步过的话，把存档捞回来当基准——最多慢"关机了多久"，
  // 而 loop 里每 30 分钟会刷新这个存档，误差有上限。
  time_t savedEpoch = (time_t)preferences.getUInt("set_epoch", 0);
  if (savedEpoch > 0) {
    time_t curEpoch = time(nullptr);
    struct tm* curTm = localtime(&curEpoch);
    if (curTm != NULL && curTm->tm_year < 120) {
      struct timeval tv = { .tv_sec = savedEpoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      Serial.printf("已从存档恢复时间: %ld\n", (long)savedEpoch);
    }
  }

  FFat.begin(true);
  initBaseMatrix();
  loadRemapsFromStorage();

  for (int c = 0; c < numCols; c++) {
    mcp.pinMode(c, OUTPUT);
    mcp.digitalWrite(c, HIGH);
  }
  for (int r = 0; r < numRows; r++) { pinMode(rowPins[r], INPUT_PULLUP); }

  lastActivityTime = millis();
  renderCurrentDisplayBase();

  BLEDevice::init("YYQ-MX9.0");
  BLEDevice::setMTU(517);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  BLEDevice::startAdvertising();

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
#endif
  Serial.println("YYQ S3 系统就绪：40MHz差量局部无闪烁引擎已启动");
}

unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 2; // 2ms 键盘极速高频扫描

// ================= LOOP =================
void loop() {
  esp_task_wdt_reset();

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    BLEDevice::startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

  if (millis() - lastPingTime > 2000) {
    lastPingTime = millis();
    uint8_t pingPacket[] = { 0xAA, 0x55, 0x02, 0xEE };
    Serial1.write(pingPacket, sizeof(pingPacket));
  }

  handleUsbSerialCommands();
  handleHidVendorCommands();
  handleC3Events();

  // 闹钟到点判定 + 倒计时推进。放主循环，不占任何中断。
  updateTimers();

  // 时间定期存档：拔电后恢复的精度就取决于这个间隔，30 分钟够用，
  // 对 NVS 的写入压力也可以忽略（一天 48 次）。
  static unsigned long lastClockSave = 0;
  if (millis() - lastClockSave > 1800000UL) {
    lastClockSave = millis();
    persistClock();
  }

  // Fn+LOGO 的延后重启：先让 HUD 把"系统重启中…"画出来，再真正重启
  if (pendingRestartMs != 0 && (long)(millis() - pendingRestartMs) >= 0) {
    esp_restart();
  }

  // 壁纸上传收尾：解码 + 落盘要几百毫秒，放主循环里做，别占着 BLE 回调线程
  if (logoRxDone) finishLogoUpload();
  // 传一半就断线（关网页、走出范围）：超时后必须把缓冲放掉并退出二进制模式，
  // 否则后面所有文本指令都会被当成 JPEG 数据吃掉，键盘就"失联"了
  if (logoRxBuf != NULL && !logoRxDone && millis() - logoRxLastMs > LOGO_RX_TIMEOUT_MS) {
    abortLogoUpload("传输中断");
  }

  // 1. 键盘扫描：拥有最高优先级，保持绝对跟手
  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    lastScanTime = millis();
    scanKeyboardMatrix();
  }

  // 2. 灯效帧发送
  if (millis() - lastLedFrameTime >= 20) {
    lastLedFrameTime = millis();
    renderLightingEngine();
    sendLedFrameToC3();
  }

  // 3. 屏幕保护超时判断。响铃期间不睡：起铃本身会刷新 lastActivityTime，
  //    而 60 秒兜底和 60 秒息屏是同一个时刻，不挡一下会在那一帧先闪一下壁纸。
  if (currentSysMode == SYS_MODE_NORMAL && ringingKind == RING_NONE
      && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
    currentSysMode = SYS_MODE_SLEEP;
    renderWallpaperView(true);
  }

  // 4. 屏幕显示渲染（全差量，彻底消灭全屏重绘闪烁）
  if (currentSysMode == SYS_MODE_NORMAL) {
    // 仅在首次启动或模式改变时才画一次底板
    if (screenNeedsRedraw) {
      screenNeedsRedraw = false;
      renderCurrentDisplayBase();
      notifDirty = true; // 底板是整屏重画的，通知条被盖掉了，得补一次
    }

    // 局部差量更新动态数据（时钟、击键数、按键OSD）
    bool screenTouched = updateDynamicElements();

    // 有通知时把跑马灯那条位置让出来，两者不抢同一块像素。
    // 律动屏的动画同理：它每帧重画律动带和底部那条窄边，正好和通知条是同一块地方，
    // 有通知时先停下让位（通知清空会置 screenNeedsRedraw，底板重画后动画自然接上）
    if (notifCount == 0) {
      updateMarquee();
      if (updateRhythmAnimation()) screenTouched = true;
    }

    // 通知条：常驻显示，只在内容变了或底层刚动过时重画一次。
    // 响铃期间让位——两张卡片在 y164~190 是重叠的，画上去会互相啃边。
    if (notifCount > 0 && ringingKind == RING_NONE && (screenTouched || notifDirty)) drawNotifPanel();

    // 敲击数持久化存档
    static uint32_t lastSavedCount = 0;
    if (totalKeyCount - lastSavedCount > 500) {
      lastSavedCount = totalKeyCount;
      preferences.putUInt("keyCount", totalKeyCount);
    }
  }
  // 4.5 录制界面：MR 进入 SEQ/CMB 后接管整个屏幕（dashboard 暂停更新）
  else if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
    drawRecUI();
  }

  // 5. 浮层：响铃卡片常驻（不自动消失，等用户按），没在响铃时才轮到 HUD 弹窗。
  //    响铃期间冒出来的 HUD 一律作废——它不会被画（下面的 else if 走不到），
  //    却会留着 hud.active，铃停之后突然弹出一个跟当下无关的旧提示。
  if (ringingKind != RING_NONE) {
    hud.active = false;
    drawRingOverlay();
  } else if (hud.active) {
    drawHudOverlay();
  }

  delayMicroseconds(500);
}