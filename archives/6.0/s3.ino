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
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
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
  SYS_MODE_SLEEP
};
KeyboardSysMode currentSysMode = SYS_MODE_NORMAL;

enum ScreenDashboardMode {
  DISP_MODE_GEEK = 0,
  DISP_MODE_BIG_CLOCK = 1,
  DISP_MODE_KEY_MON = 2,
  DISP_MODE_WALLPAPER = 3
};
uint8_t currentDispMode = DISP_MODE_GEEK;
bool screenNeedsRedraw = true; // 仅在模式变更/全屏初始化时为 true

const uint8_t TOTAL_DISP_MODES = 4;
const char* dispModeNamesCN[TOTAL_DISP_MODES] = { "极客仪表盘", "大字时钟", "实时击键监控", "自定义壁纸" };
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

// MR 免驱录制
#define MAX_REC_KEYS 64
uint16_t recKeyBuffer[MAX_REC_KEYS];
int recKeyCount = 0;

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
uint8_t currentEffect = 1;
const uint8_t MAX_EFFECTS = 13;
const char* effectNames[] = {
  "关闭", "纯红烈焰", "纯绿荧光", "纯蓝深邃", "冰蓝极光", "纯白恒星",
  "红光呼吸", "绿光呼吸", "蓝光呼吸", "冰蓝呼吸", "流光跑马", "双极彗星", "幻彩流光"
};

bool g_forceOff = false;
bool cherryLogoEnabled = false;

enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };
AlertType activeAlert = ALERT_NONE;

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
void renderCurrentDisplayBase();
void updateDynamicElements();
void renderWallpaperView(bool drawOverlayTime = true);
void triggerHud(const char* title, const char* value, int percent, uint16_t color);
void renderStylePreview();
void applyStylePreview();
void cancelStylePreview();
void handleCommand(String data);

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

// ================= 指令控制中心 =================
void handleCommand(String data) {
  data.trim();
  if (data.length() == 0) return;

  if (data.startsWith("ALERT:")) {
    String cmd = data.substring(6);
    if (cmd == "RED") {
      activeAlert = ALERT_RED;
      triggerHud("系统报警", "红灯爆闪", -1, ST77XX_RED);
    } else if (cmd == "GREEN") {
      activeAlert = ALERT_GREEN;
      triggerHud("系统通知", "绿灯闪烁", -1, ST77XX_GREEN);
    } else if (cmd == "YELLOW") {
      activeAlert = ALERT_YELLOW;
      triggerHud("系统警告", "黄灯闪烁", -1, ST77XX_YELLOW);
    } else if (cmd == "OFF" || cmd == "STOP") {
      activeAlert = ALERT_NONE;
      triggerHud("警报解除", "恢复常态", -1, ST77XX_CYAN);
    }
  }
  else if (data.startsWith("DISP_MODE:")) {
    currentDispMode = (uint8_t)data.substring(10).toInt();
    if (currentDispMode > 3) currentDispMode = 0;
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
  else if (data == "LOGO_START") {
    File f = FFat.open("/logo.bin", FILE_WRITE);
    if (f) f.close();
    triggerHud("图片传输", "开始写入...", -1, ST77XX_YELLOW);
  } else if (data.startsWith("LOGO_DATA:")) {
    File f = FFat.open("/logo.bin", FILE_APPEND);
    if (f) {
      String hex = data.substring(10);
      int len = hex.length();
      uint8_t rawBuf[256];
      int bytes = 0;
      for (int i = 0; i < len; i += 2) {
        char byteString[3] = { hex[i], hex[i + 1], '\0' };
        rawBuf[bytes++] = (uint8_t)strtol(byteString, NULL, 16);
      }
      f.write(rawBuf, bytes);
      f.close();
    }
  } else if (data == "LOGO_END") {
    triggerHud("画板更新", "传输完成", -1, ST77XX_GREEN);
    if (currentDispMode == DISP_MODE_WALLPAPER) screenNeedsRedraw = true;
  }
}

// ================= BLE 通信 =================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    handleCommand(pChar->getValue().c_str());
  }
};

// ================= HUD 浮层管理（彻底消除全屏刷新） =================
void triggerHud(const char* title, const char* value, int percent, uint16_t color) {
  hud.active = true;
  hud.dirty = true;
  hud.triggerTime = millis();
  strncpy(hud.title, title, sizeof(hud.title));
  strncpy(hud.value, value, sizeof(hud.value));
  hud.percent = percent;
  hud.color = color;
}

void drawHudOverlay() {
  if (!hud.active) return;
  int x = 20, y = 70, w = 200, h = 90;

  // 1.2秒超时退出，擦除 HUD 局部区域并恢复原区域底图，绝不清屏！
  if (millis() - hud.triggerTime > 1200) {
    hud.active = false;
    hud.dirty = false;
    screenNeedsRedraw = true;
    // 菜单/预览下没有差量引擎兜底，直接重画一次，避免 HUD 残影留在画面上
    if (currentSysMode == SYS_MODE_STYLE_PREVIEW) renderStylePreview();
    else if (currentSysMode == SYS_MODE_MENU) drawMenuUI();
    return;
  }

  // 仅在有改动时重画 1 次
  if (hud.dirty) {
    hud.dirty = false;
    tft.fillRoundRect(x, y, w, h, 10, ST77XX_BLACK);
    tft.drawRoundRect(x, y, w, h, 10, hud.color);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    int tw = u8g2.getUTF8Width(hud.title);
    u8g2.setCursor(x + (w - tw) / 2, y + 26);
    u8g2.print(hud.title);

    u8g2.setForegroundColor(hud.color);
    int vw = u8g2.getUTF8Width(hud.value);
    u8g2.setCursor(x + (w - vw) / 2, y + 54);
    u8g2.print(hud.value);

    if (hud.percent >= 0) {
      int barW = 160, barH = 8, barX = x + 20, barY = y + 66;
      tft.drawRoundRect(barX, barY, barW, barH, 3, ST77XX_DARKGREY);
      int fillW = (barW - 4) * constrain(hud.percent, 0, 100) / 100;
      tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, hud.color);
    }
  }
}

// ================= 状态锁指示灯局部重绘 =================
void drawLockIndicators(int startY = 32, bool forceRedraw = false) {
  if (!forceRedraw && (lastNumLock == numLockActive && lastCapsLock == capsLockActive && lastScrollLock == scrollLockActive)) {
    return;
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

// ================= 界面底框初始化（仅在切屏时执行一次） =================
void renderCurrentDisplayBase() {
  tft.fillScreen(ST77XX_BLACK);
  lastDrawnTimeStr[0] = '\0';
  lastDrawnKeyCount = 0xFFFFFFFF;
  keystrokeActiveOnScreen = false;

  if (currentDispMode == DISP_MODE_GEEK) {
    tft.fillRect(0, 0, 240, 26, 0x18C3);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    u8g2.setCursor(8, 18);
    u8g2.print("方案: ");
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.print(profileNamesCN[currentProfile]);

    drawLockIndicators(30, true);
    tft.drawFastHLine(20, 100, 200, ST77XX_DARKGREY);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(20, 128);
    u8g2.print("击键计数: ");

    tft.drawRoundRect(10, 175, 220, 52, 6, ST77XX_ORANGE);
  }
  else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
    drawLockIndicators(10, true);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    int pw = u8g2.getUTF8Width(profileNamesCN[currentProfile]);
    u8g2.setCursor((240 - pw) / 2, 210);
    u8g2.print(profileNamesCN[currentProfile]);
  }
  else if (currentDispMode == DISP_MODE_KEY_MON) {
    tft.fillRect(0, 0, 240, 28, ST77XX_NAVY);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_WHITE);
    u8g2.setCursor(60, 20);
    u8g2.print("实时击键监控台");

    tft.drawRoundRect(15, 45, 210, 100, 8, ST77XX_CYAN);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_LIGHTGREY);
    u8g2.setCursor(25, 68);
    u8g2.print("最近触发按键：");

    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(25, 180);
    u8g2.print("累计按键数: ");

    u8g2.setCursor(25, 215);
    u8g2.print("当前方案: ");
    u8g2.print(profileNamesCN[currentProfile]);
  }
  else if (currentDispMode == DISP_MODE_WALLPAPER) {
    renderWallpaperView(false);
  }
}

// ================= 极速差量动态元素刷新（绝不全屏重绘） =================
void updateDynamicElements() {
  if (hud.active) return; // 弹窗激活期间暂停底层元素刷新

  // 1. 刷新锁灯状态
  drawLockIndicators((currentDispMode == DISP_MODE_BIG_CLOCK) ? 10 : 30);

  // 2. 局部差量刷新时间（文本颜色带底色，无需清屏）
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char currentTimeStr[16];
  if (t && t->tm_year > 120) {
    strftime(currentTimeStr, sizeof(currentTimeStr), (currentDispMode == DISP_MODE_BIG_CLOCK) ? "%H:%M" : "%H:%M:%S", t);
  } else {
    snprintf(currentTimeStr, sizeof(currentTimeStr), "--:--:--");
  }

  if (strcmp(currentTimeStr, lastDrawnTimeStr) != 0) {
    strcpy(lastDrawnTimeStr, currentTimeStr);

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
      u8g2.setFont(u8g2_font_wqy14_t_gb2312);
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
  }

  // 3. 局部差量刷新按键计数器
  if (totalKeyCount != lastDrawnKeyCount) {
    lastDrawnKeyCount = totalKeyCount;
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
    }
  }

  // 4. 实时按键回显局部更新与超时淡出
  if (currentDispMode == DISP_MODE_GEEK && showKeystrokes) {
    if (keystrokeNeedsRedraw) {
      keystrokeNeedsRedraw = false;
      keystrokeActiveOnScreen = true;
      tft.fillRoundRect(15, 142, 210, 26, 4, 0x001F);
      u8g2.setFont(u8g2_font_wqy14_t_gb2312);
      u8g2.setForegroundColor(ST77XX_WHITE);
      u8g2.setCursor(25, 160);
      u8g2.print("键入: ");
      u8g2.setForegroundColor(ST77XX_YELLOW);
      u8g2.print(lastKeyStrokeName);
    } else if (keystrokeActiveOnScreen && millis() - keyStrokeDisplayTime >= 800) {
      keystrokeActiveOnScreen = false;
      tft.fillRect(15, 142, 210, 26, ST77XX_BLACK); // 超时只清除回显框这一小块！
    }
  }
  else if (currentDispMode == DISP_MODE_KEY_MON && keystrokeNeedsRedraw) {
    keystrokeNeedsRedraw = false;
    tft.fillRect(25, 85, 190, 45, ST77XX_BLACK);
    tft.setTextSize(4);
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setCursor(35, 95);
    tft.print(lastKeyStrokeName);
  }
}

// ================= U8g2 菜单渲染 =================
int menuCursor = 0;
const int MENU_TOTAL_ITEMS = 6;
const char* menuListCN[] = {
  "1. 返回主屏",
  "2. 切换主屏风格",
  "3. 切换配置方案",
  "4. 按键回显开关",
  "5. 键盘背光灯效",
  "6. 敲击计数清零"
};

void drawMenuUI() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 240, 32, ST77XX_NAVY);

  u8g2.setFont(u8g2_font_wqy14_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  u8g2.setCursor(55, 22);
  u8g2.print("YYQ 键盘系统 OS");

  for (int i = 0; i < MENU_TOTAL_ITEMS; i++) {
    int y = 44 + i * 31;
    if (i == menuCursor) {
      tft.fillRoundRect(8, y, 224, 28, 6, ST77XX_BLUE);
      u8g2.setForegroundColor(ST77XX_WHITE);
    } else {
      u8g2.setForegroundColor(ST77XX_LIGHTGREY);
    }
    u8g2.setCursor(20, y + 20);
    u8g2.print(menuListCN[i]);
  }
}

// ================= 主屏风格预览（左右切换，二次确认才生效） =================
void renderStylePreview() {
  uint8_t savedDispMode = currentDispMode;
  currentDispMode = previewDispMode;

  // 完整画出该风格的真实底板，再补一次动态内容，预览不会是一片空白
  renderCurrentDisplayBase();
  updateDynamicElements();

  // 底部预览提示条
  const int barY = 202;
  tft.fillRect(0, barY, 240, 240 - barY, ST77XX_BLACK);
  tft.drawFastHLine(0, barY, 240, ST77XX_CYAN);

  char line1[40];
  snprintf(line1, sizeof(line1), "预览 %d/%d: %s",
           previewDispMode + 1, TOTAL_DISP_MODES, dispModeNamesCN[previewDispMode]);

  u8g2.setFont(u8g2_font_wqy14_t_gb2312);
  u8g2.setForegroundColor(ST77XX_CYAN);
  int w1 = u8g2.getUTF8Width(line1);
  u8g2.setCursor((240 - w1) / 2, 219);
  u8g2.print(line1);

  const char* hint = "< > 切换   回车确认   ESC取消";
  u8g2.setForegroundColor(ST77XX_LIGHTGREY);
  int w2 = u8g2.getUTF8Width(hint);
  u8g2.setCursor((240 - w2) / 2, 236);
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
  if (currentDispMode != DISP_MODE_GEEK || hud.active) return;
  unsigned long now = millis();
  if (now - lastMarqueeUpdate > 35) {
    lastMarqueeUpdate = now;
    tft.fillRect(15, 185, 210, 34, ST77XX_BLACK);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
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
  setLedRGB(16, numLockActive ? 255 : 0, numLockActive ? 180 : 0, 0);
  setLedRGB(17, 0, capsLockActive ? 150 : 0, capsLockActive ? 255 : 0);
  setLedRGB(18, scrollLockActive ? 255 : 0, scrollLockActive ? 50 : 0, 0);
}

void renderLightingEngine() {
  unsigned long now = millis();

  if (g_forceOff) {
    for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, 0, 0, 0);
    return;
  }

  if (activeAlert != ALERT_NONE) {
    static unsigned long lastAlertFlash = 0;
    static bool alertToggle = false;
    if (now - lastAlertFlash > 180) {
      lastAlertFlash = now;
      alertToggle = !alertToggle;
    }
    if (alertToggle) {
      uint8_t r = 0, g = 0, b = 0;
      if (activeAlert == ALERT_RED) r = 255;
      else if (activeAlert == ALERT_GREEN) g = 255;
      else if (activeAlert == ALERT_YELLOW) { r = 255; g = 180; }
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

  for (int i = 0; i < NUM_MAIN_LEDS; i++) {
    packet[3 + i * 3 + 0] = (uint8_t)((frameBuffer[i][0] * brightness) / 255);
    packet[3 + i * 3 + 1] = (uint8_t)((frameBuffer[i][1] * brightness) / 255);
    packet[3 + i * 3 + 2] = (uint8_t)((frameBuffer[i][2] * brightness) / 255);
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
    case 0xDB: return "NumLock";
    default:
      if (code >= KEY_F1 && code <= KEY_F12) return "F" + String(code - KEY_F1 + 1);
      if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
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
                menuCursor = (menuCursor == 0) ? MENU_TOTAL_ITEMS - 1 : menuCursor - 1;
                drawMenuUI();
              } else if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                menuCursor = (menuCursor + 1) % MENU_TOTAL_ITEMS;
                drawMenuUI();
              } else if (baseKey == KEY_RETURN) {
                handleMenuSelect();
              } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                currentSysMode = SYS_MODE_NORMAL;
                screenNeedsRedraw = true;
              }
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
                  displayStatusCN("录制组合模式", ST77XX_MAGENTA);
                } else {
                  currentSysMode = SYS_MODE_NORMAL;
                  displayStatusCN("录制已取消", ST77XX_RED);
                  delay(300);
                  screenNeedsRedraw = true;
                }
                return;
              }
              if (recKeyCount < MAX_REC_KEYS && baseKey < MACRO_BASE) {
                recKeyBuffer[recKeyCount++] = baseKey;
              }
              return;
            }

            // 4. MC 系统菜单键
            if (baseKey == K_MC) {
              currentSysMode = SYS_MODE_MENU;
              menuCursor = 0;
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
              displayStatusCN("录制击键流", ST77XX_YELLOW);
              return;
            }

            if (cherryLogoEnabled) triggerKeyReaction();

            // 7. 普通按键及媒体按键
            if (baseKey == K_FN) {
              fnPressed = true;
            } else if (baseKey >= MACRO_BASE) {
              if (baseKey == K_LOGO && fnPressed) {
                esp_restart();
              } else if (baseKey == K_PLAY) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_STANDBY); SystemControl.release(); }
                else ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
              } else if (baseKey == K_NEXT) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_WAKE_HOST); SystemControl.release(); Keyboard.write(' '); }
                else ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
              } else if (baseKey == K_PREV) {
                if (fnPressed) { SystemControl.press(SYSTEM_CONTROL_POWER_OFF); SystemControl.release(); }
                else ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
              } else if (baseKey != K_LOGO) {
                executeMacro(getMacroNameByCode(baseKey));
              }
            } else {
              uint16_t mappedKey = getMappedKey(baseKey);
              Keyboard.press((uint8_t)mappedKey);
            }
          }
          // ---------------- 按键释放 ----------------
          else {
            if (currentSysMode == SYS_MODE_MENU || currentSysMode == SYS_MODE_STYLE_PREVIEW || currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
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
    case 5:
      totalKeyCount = 0;
      preferences.putUInt("keyCount", 0);
      displayStatusCN("计数已清零", ST77XX_GREEN);
      delay(400);
      drawMenuUI();
      break;
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
        if (currentSysMode == SYS_MODE_MENU) {
          if (isRight) menuCursor = (menuCursor + 1) % MENU_TOTAL_ITEMS;
          else menuCursor = (menuCursor == 0) ? MENU_TOTAL_ITEMS - 1 : menuCursor - 1;
          drawMenuUI();
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
        if (currentSysMode == SYS_MODE_MENU) {
          handleMenuSelect();
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
        if (g_forceOff) {
          g_forceOff = false;
          currentMode = MODE_LIGHT;
        } else {
          currentMode = (currentMode == MODE_LIGHT) ? MODE_SCREEN_BRIGHTNESS : MODE_LIGHT;
        }
        triggerHud("控制目标", (currentMode == MODE_LIGHT) ? "键盘背光" : "显示器亮度", -1, ST77XX_YELLOW);
      } else if (serialBuffer == "BTN:LIGHT_HOLD") {
        g_forceOff = !g_forceOff;
        triggerHud("背光总开关", g_forceOff ? "已关闭" : "已开启", -1, ST77XX_RED);
      } else if (serialBuffer == "BTN:MUTE") {
        if (g_forceOff) g_forceOff = false;
        currentMode = MODE_MUTE;
        ConsumerControl.press(CONSUMER_CONTROL_MUTE);
        ConsumerControl.release();
        triggerHud("静音控制", "静音切换", -1, ST77XX_GREEN);
      } else if (serialBuffer == "BTN:MUTE_HOLD") {
        delay(200);
        esp_restart();
      } else if (serialBuffer == "BTN:CPG") {
        if (g_forceOff) g_forceOff = false;
        if (currentMode != MODE_CPG) currentMode = MODE_CPG;
        else {
          currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          if (currentEffect == 0) currentEffect = 1;
        }
        triggerHud("灯效切换", effectNames[currentEffect], -1, ST77XX_MAGENTA);
      } else if (serialBuffer == "BTN:CPG_HOLD") {
        delay(200);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
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
  u8g2.setFont(u8g2_font_wqy14_t_gb2312);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(25);
  if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) recoverI2CBus();

  preferences.begin("keyboard", false);
  currentProfile = preferences.getUChar("curr_prof", 0);
  if (currentProfile >= TOTAL_PROFILES) currentProfile = 0;
  currentDispMode = preferences.getUChar("disp_mode", 0);
  if (currentDispMode > 3) currentDispMode = 0;
  showKeystrokes = preferences.getBool("show_keys", true);
  cherryLogoEnabled = preferences.getBool("cherryLogo", false);
  totalKeyCount = preferences.getUInt("keyCount", 0);
  customMarquee = preferences.getString("marquee", "YYQ Studio - 极客机械大师");

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

  // 3. 屏幕保护超时判断
  if (currentSysMode == SYS_MODE_NORMAL && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
    currentSysMode = SYS_MODE_SLEEP;
    renderWallpaperView(true);
  }

  // 4. 屏幕显示渲染（全差量，彻底消灭全屏重绘闪烁）
  if (currentSysMode == SYS_MODE_NORMAL) {
    // 仅在首次启动或模式改变时才画一次底板
    if (screenNeedsRedraw) {
      screenNeedsRedraw = false;
      renderCurrentDisplayBase();
    }

    // 局部差量更新动态数据（时钟、击键数、按键OSD）
    updateDynamicElements();
    updateMarquee();

    // 敲击数持久化存档
    static uint32_t lastSavedCount = 0;
    if (totalKeyCount - lastSavedCount > 500) {
      lastSavedCount = totalKeyCount;
      preferences.putUInt("keyCount", totalKeyCount);
    }
  }

  // 5. HUD 弹窗卡片单次渲染（绝无循环塞满 SPI 的情况）
  if (hud.active) {
    drawHudOverlay();
  }

  delayMicroseconds(500);
}