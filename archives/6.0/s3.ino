#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDSystemControl.h"
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

// ================= TFT 屏幕与官方中文字体库 =================
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

// 系统运行状态
enum KeyboardSysMode {
  SYS_MODE_NORMAL,
  SYS_MODE_MENU,
  SYS_MODE_REC_SEQ,
  SYS_MODE_REC_CMB,
  SYS_MODE_LOGO_VIEW
};
KeyboardSysMode currentSysMode = SYS_MODE_NORMAL;

uint32_t totalKeyCount = 0;
String customMarquee = "YYQ Studio - 极客机械大师";
int marqueeScrollX = 240;
unsigned long lastMarqueeUpdate = 0;
unsigned long lastTimeUpdate = 0;

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
const uint8_t MAX_EFFECTS = 14;
bool g_forceOff = false;
char bt_alert = '\0';
bool cherryLogoEnabled = false;

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

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

void displayStatusCN(const char* title, uint16_t color = ST77XX_GREEN);
void switchProfile(uint8_t profIdx);
void initScreenUI();
void drawMenuUI();
void renderMainDashboard();
void renderLogoFromFile();
void executeMacro(String keyName);
void executeGlobalKey(String gKey);

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

// ================= BLE 通信 =================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    if (data.length() == 0) return;

    if (data.startsWith("TIME:")) {
      time_t t = data.substring(5).toInt();
      struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      displayStatusCN("时间同步成功", ST77XX_GREEN);
    }
    else if (data.startsWith("MARQUEE:")) {
      customMarquee = data.substring(8);
      preferences.putString("marquee", customMarquee);
      displayStatusCN("标语更新成功", ST77XX_CYAN);
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
        displayStatusCN("映射保存成功", ST77XX_GREEN);
      }
    }
    else if (data.startsWith("GSET:")) {
      // 格式: GSET:MA:SW:1+CMB:128,116 或 GSET:MB:CMB:128,99
      int p1 = data.indexOf(':', 5);
      if (p1 > 0) {
        String gKey = data.substring(5, p1);
        String payload = data.substring(p1 + 1);
        preferences.putString(("g_" + gKey).c_str(), payload);
        displayStatusCN("全局键已生效", ST77XX_MAGENTA);
      }
    }
    else if (data.startsWith("SET:")) {
      int firstColon = data.indexOf(':', 4);
      if (firstColon > 0) {
        String keyName = data.substring(4, firstColon);
        String payload = data.substring(firstColon + 1);
        preferences.putString(keyName.c_str(), payload);
        displayStatusCN("宏配置已保存", ST77XX_GREEN);
      }
    }
    else if (data == "LOGO_START") {
      File f = FFat.open("/logo.bin", FILE_WRITE);
      if (f) f.close();
    } else if (data.startsWith("LOGO_DATA:")) {
      File f = FFat.open("/logo.bin", FILE_APPEND);
      if (f) {
        String hex = data.substring(10);
        int len = hex.length();
        uint8_t rawBuf[128];
        int bytes = 0;
        for (int i = 0; i < len; i += 2) {
          char byteString[3] = { hex[i], hex[i + 1], '\0' };
          rawBuf[bytes++] = (uint8_t)strtol(byteString, NULL, 16);
        }
        f.write(rawBuf, bytes);
        f.close();
      }
    } else if (data == "LOGO_END") {
      displayStatusCN("图案接收就绪", ST77XX_YELLOW);
    }
  }
};

// ================= U8g2 原生中文界面渲染 =================
int menuCursor = 0;
const int MENU_TOTAL_ITEMS = 6;
const char* menuListCN[] = {
  "1. 返回主屏",
  "2. 切换配置方案",
  "3. 播放图案画板",
  "4. 键盘背光灯效",
  "5. 按键反馈动效",
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

void drawLockIndicators(bool forceRedraw = false) {
  if (!forceRedraw && (lastNumLock == numLockActive && lastCapsLock == capsLockActive && lastScrollLock == scrollLockActive)) {
    return;
  }
  lastNumLock = numLockActive;
  lastCapsLock = capsLockActive;
  lastScrollLock = scrollLockActive;

  uint16_t numBg = numLockActive ? 0x0400 : 0x18E3;
  uint16_t numFg = numLockActive ? ST77XX_GREEN : ST77XX_DARKGREY;
  tft.fillRoundRect(10, 34, 68, 22, 4, numBg);
  tft.drawRoundRect(10, 34, 68, 22, 4, numFg);
  tft.setTextSize(2);
  tft.setTextColor(numFg, numBg);
  tft.setCursor(22, 38);
  tft.print("NUM");

  uint16_t capsBg = capsLockActive ? 0x001F : 0x18E3;
  uint16_t capsFg = capsLockActive ? ST77XX_CYAN : ST77XX_DARKGREY;
  tft.fillRoundRect(86, 34, 68, 22, 4, capsBg);
  tft.drawRoundRect(86, 34, 68, 22, 4, capsFg);
  tft.setTextColor(capsFg, capsBg);
  tft.setCursor(95, 38);
  tft.print("CAPS");

  uint16_t scrlBg = scrollLockActive ? 0xFD20 : 0x18E3;
  uint16_t scrlFg = scrollLockActive ? ST77XX_YELLOW : ST77XX_DARKGREY;
  tft.fillRoundRect(162, 34, 68, 22, 4, scrlBg);
  tft.drawRoundRect(162, 34, 68, 22, 4, scrlFg);
  tft.setTextColor(scrlFg, scrlBg);
  tft.setCursor(171, 38);
  tft.print("SCRL");
}

void renderMainDashboard() {
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 240, 28, 0x18C3);
  u8g2.setFont(u8g2_font_wqy14_t_gb2312);
  u8g2.setForegroundColor(ST77XX_WHITE);
  u8g2.setCursor(8, 20);
  u8g2.print("配置: ");
  u8g2.setForegroundColor(ST77XX_YELLOW);
  u8g2.print(profileNamesCN[currentProfile]);

  drawLockIndicators(true);

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char timeStr[16];
  if (timeinfo && timeinfo->tm_year > 120) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);
  else snprintf(timeStr, sizeof(timeStr), "--:--:--");

  tft.setTextSize(3);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(50, 68);
  tft.print(timeStr);

  tft.drawFastHLine(20, 108, 200, ST77XX_DARKGREY);

  u8g2.setFont(u8g2_font_wqy14_t_gb2312);
  u8g2.setForegroundColor(ST77XX_YELLOW);
  u8g2.setCursor(25, 138);
  u8g2.print("今日敲击: ");

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(125, 124);
  tft.print(totalKeyCount);

  tft.drawRoundRect(10, 168, 220, 56, 6, ST77XX_ORANGE);
}

void updateMarquee() {
  unsigned long now = millis();
  if (now - lastMarqueeUpdate > 30) {
    lastMarqueeUpdate = now;
    tft.fillRect(15, 178, 210, 36, ST77XX_BLACK);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setForegroundColor(ST77XX_YELLOW);
    u8g2.setCursor(marqueeScrollX, 202);
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

void renderLogoFromFile() {
  if (!FFat.exists("/logo.bin")) {
    displayStatusCN("未发现图案", ST77XX_RED);
    delay(800);
    currentSysMode = SYS_MODE_NORMAL;
    renderMainDashboard();
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

void switchProfile(uint8_t profIdx) {
  if (profIdx >= TOTAL_PROFILES) return;
  currentProfile = profIdx;
  preferences.putUChar("curr_prof", profIdx);
  displayStatusCN(profileNamesCN[profIdx], ST77XX_GREEN);
  delay(300);
  renderMainDashboard();
}

// ================= 灯效引擎 =================
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

// 【关键增强 1】：触发按键特效时跳过常亮层渲染，清空底色，确保特效清晰可见
void renderLightingEngine() {
  unsigned long now = millis();

  if (g_forceOff) {
    for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, 0, 0, 0);
    return;
  }

  // MR 录制爆闪指示
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

  // 若处于按键动效激活中，则不渲染常亮背景，防止特效被遮盖
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

// 辅助：执行 CMB 组合键动作
void executeComboAction(String cmb) {
  while (cmb.length() > 0) {
    int comma = cmb.indexOf(',');
    uint8_t code = (comma == -1) ? cmb.toInt() : cmb.substring(0, comma).toInt();
    if (code > 0) Keyboard.press(code);
    if (comma == -1) break;
    cmb = cmb.substring(comma + 1);
  }
  delay(50);
  Keyboard.releaseAll();
}

// 辅助：执行 SEQ 击键流
void executeSequenceAction(String seq) {
  int i = 0;
  while (i < seq.length()) {
    if (seq[i] == '[') {
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
    delay(5);
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

// 【关键增强 2】：支持切换方案同时触发组合键
void executeGlobalKey(String gKey) {
  String val = preferences.getString(("g_" + gKey).c_str(), "");
  if (val.length() == 0) {
    if (gKey == "MA") switchProfile(0);
    else if (gKey == "MB") switchProfile(1);
    return;
  }

  // 1. 处理方案切换部分
  if (val.startsWith("SW:")) {
    int plusIdx = val.indexOf('+');
    String swPart = (plusIdx != -1) ? val.substring(3, plusIdx) : val.substring(3);
    if (swPart == "NEXT") switchProfile((currentProfile + 1) % TOTAL_PROFILES);
    else if (swPart != "NONE") switchProfile(swPart.toInt());

    // 若后面还拼接了组合键 (如 SW:0+CMB:128,130,116)
    if (plusIdx != -1) {
      String extra = val.substring(plusIdx + 1);
      if (extra.startsWith("CMB:")) {
        executeComboAction(extra.substring(4));
      } else if (extra.startsWith("SEQ:")) {
        executeSequenceAction(extra.substring(4));
      }
    }
    return;
  }

  // 2. 纯组合按键或击键流
  if (val.startsWith("CMB:")) {
    executeComboAction(val.substring(4));
  } else if (val.startsWith("SEQ:")) {
    executeSequenceAction(val.substring(4));
  }
}

// 【关键增强 3 & 4】：M1~M12 宏支持打字 (SEQ:) 与 任意组合键 (CMB:)
void executeMacro(String keyName) {
  if (keyName == "ME") {
    if (FFat.exists("/me_hex.txt")) {
      File f = FFat.open("/me_hex.txt", FILE_READ);
      if (f) {
        Keyboard.print("[HEXS]");
        delay(20);
        while (f.available()) {
          Keyboard.print((char)f.read());
          delay(1);
          esp_task_wdt_reset();
        }
        f.close();
        delay(20);
        Keyboard.print("[HEXE]");
        return;
      }
    }
  }

  char pKey[32];
  snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, keyName.c_str());
  String macroData = preferences.getString(pKey, "");
  if (macroData.length() == 0) return;

  if (macroData.startsWith("SEQ:")) {
    executeSequenceAction(macroData.substring(4));
  } else if (macroData.startsWith("CMB:")) {
    executeComboAction(macroData.substring(4));
  }
}

// 【关键增强 5】：MR 现场录制在组合键模式（CMB）下，自动保存为 CMB 格式
void finishMacroRecording(String targetKey) {
  if (recKeyCount == 0) {
    currentSysMode = SYS_MODE_NORMAL;
    displayStatusCN("取消录制", ST77XX_RED);
    delay(300);
    renderMainDashboard();
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
    // 录制为组合按键，触发时所有键同时按下并释放
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
  renderMainDashboard();
}

void handleMenuSelect();

// ================= 键盘扫描与事件处理 =================
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
            totalKeyCount++;

            // 1. 菜单模式接管
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
                renderMainDashboard();
              }
              return;
            }

            // 2. MR 宏现场录制
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
                  renderMainDashboard();
                }
                return;
              }
              if (recKeyCount < MAX_REC_KEYS && baseKey < MACRO_BASE) {
                recKeyBuffer[recKeyCount++] = baseKey;
              }
              return;
            }

            // 3. MC 系统菜单键
            if (baseKey == K_MC) {
              currentSysMode = SYS_MODE_MENU;
              menuCursor = 0;
              drawMenuUI();
              return;
            }

            // 4. MA / MB 全局键
            if (baseKey == K_MA) {
              executeGlobalKey("MA");
              return;
            } else if (baseKey == K_MB) {
              executeGlobalKey("MB");
              return;
            }

            // 5. MR 录制键
            if (baseKey == K_MR) {
              currentSysMode = SYS_MODE_REC_SEQ;
              recKeyCount = 0;
              displayStatusCN("录制击键流", ST77XX_YELLOW);
              return;
            }

            if (currentSysMode == SYS_MODE_LOGO_VIEW) {
              currentSysMode = SYS_MODE_NORMAL;
              renderMainDashboard();
            }

            if (cherryLogoEnabled) triggerKeyReaction();

            // 6. 普通按键及小键盘
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
            if (currentSysMode == SYS_MODE_MENU || currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
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
      renderMainDashboard();
      break;
    case 1:
      switchProfile((currentProfile + 1) % TOTAL_PROFILES);
      break;
    case 2:
      currentSysMode = SYS_MODE_LOGO_VIEW;
      renderLogoFromFile();
      break;
    case 3:
      currentEffect = (currentEffect + 1) % MAX_EFFECTS;
      if (currentEffect == 0) currentEffect = 1;
      drawMenuUI();
      break;
    case 4:
      keypressStyle = (keypressStyle + 1) % 24;
      triggerKeyReaction();
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

      if (serialBuffer == "PONG") {
        c3Connected = true;
      }
      else if (serialBuffer.startsWith("ENC:")) {
        bool isRight = (serialBuffer == "ENC:+");
        if (currentSysMode == SYS_MODE_MENU) {
          if (isRight) menuCursor = (menuCursor + 1) % MENU_TOTAL_ITEMS;
          else menuCursor = (menuCursor == 0) ? MENU_TOTAL_ITEMS - 1 : menuCursor - 1;
          drawMenuUI();
        } else {
          if (g_forceOff) g_forceOff = false;
          if (currentMode == MODE_LIGHT) {
            if (isRight) brightness = (brightness <= 200) ? brightness + 20 : 220;
            else brightness = (brightness >= 20) ? brightness - 20 : 0;
          } else if (currentMode == MODE_SCREEN_BRIGHTNESS) {
            if (isRight) { ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_INCREMENT); ConsumerControl.release(); }
            else { ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_DECREMENT); ConsumerControl.release(); }
          } else if (currentMode == MODE_MUTE) {
            if (isRight) { ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT); ConsumerControl.release(); }
            else { ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT); ConsumerControl.release(); }
          } else if (currentMode == MODE_CPG) {
            if (isRight) currentEffect = (currentEffect + 1) % MAX_EFFECTS;
            else currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
          } else if (currentMode == MODE_KEY_COLOR) {
            if (isRight) keypressStyle = (keypressStyle + 1) % 24;
            else keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
            triggerKeyReaction();
          }
        }
      }
      else if (serialBuffer == "BTN:KNOB") {
        if (currentSysMode == SYS_MODE_MENU) {
          handleMenuSelect();
        } else {
          if (g_forceOff) g_forceOff = false;
          currentMode = MODE_KEY_COLOR;
          triggerKeyReaction();
        }
      }
      else if (serialBuffer == "BTN:LIGHT") {
        if (g_forceOff) {
          g_forceOff = false;
          currentMode = MODE_LIGHT;
        } else {
          currentMode = (currentMode == MODE_LIGHT) ? MODE_SCREEN_BRIGHTNESS : MODE_LIGHT;
        }
      } else if (serialBuffer == "BTN:LIGHT_HOLD") {
        g_forceOff = !g_forceOff;
      } else if (serialBuffer == "BTN:MUTE") {
        if (g_forceOff) g_forceOff = false;
        currentMode = MODE_MUTE;
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
  USB.begin();

  tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
  tft.init(240, 240);
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

  renderMainDashboard();

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
  Serial.println("YYQ S3 系统就绪：按键特效背景自动避让与双重动作宏已就绪");
}

unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 2;

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

  handleC3Events();

  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    lastScanTime = millis();
    scanKeyboardMatrix();
  }

  if (millis() - lastLedFrameTime >= 20) {
    lastLedFrameTime = millis();
    renderLightingEngine();
    sendLedFrameToC3();
  }

  if (currentSysMode == SYS_MODE_NORMAL) {
    drawLockIndicators();
    updateMarquee();
    if (millis() - lastTimeUpdate > 1000) {
      lastTimeUpdate = millis();

      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      char timeStr[16];
      if (timeinfo && timeinfo->tm_year > 120) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);
      else snprintf(timeStr, sizeof(timeStr), "--:--:--");

      tft.fillRect(45, 65, 150, 30, ST77XX_BLACK);
      tft.setTextSize(3);
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(50, 68);
      tft.print(timeStr);

      tft.fillRect(125, 122, 90, 22, ST77XX_BLACK);
      tft.setTextSize(2);
      tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
      tft.setCursor(125, 124);
      tft.print(totalKeyCount);

      static uint32_t lastSavedCount = 0;
      if (totalKeyCount - lastSavedCount > 500) {
        lastSavedCount = totalKeyCount;
        preferences.putUInt("keyCount", totalKeyCount);
      }
    }
  }

  delay(1);
}