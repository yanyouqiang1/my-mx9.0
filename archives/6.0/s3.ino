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
#include <Adafruit_NeoPixel.h> // 仅用于 S3 端颜色数学计算

// ================= TFT 屏幕相关库与引脚定义 =================
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
Preferences preferences;
Adafruit_MCP23X17 mcp;
#define WDT_TIMEOUT 3

#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20

#define RX_PIN 10
#define TX_PIN 9
#define UART_BAUD 460800  // 高速串口，单帧推流只需约 1ms

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
uint16_t keyMatrix[numRows][numCols] = { 0 };
bool cherryLogoEnabled = false;
bool lastState[numRows][numCols] = { false };
unsigned long lastDebounceTime[numRows][numCols] = { 0 };

// ================= 灯光与系统控制状态机 (由 S3 统管) =================
#define NUM_MAIN_LEDS 16
#define NUM_IND_LEDS  3
#define TOTAL_LEDS    19

// 渲染缓冲区：0~15为主灯，16~18为指示灯
uint8_t frameBuffer[TOTAL_LEDS][3] = { 0 };

enum ControlMode {
  MODE_CPG,
  MODE_MUTE,
  MODE_LIGHT,             // 键盘灯光亮度模式
  MODE_SCREEN_BRIGHTNESS, // 电脑屏幕亮度调节
  MODE_KEY_COLOR
};
ControlMode currentMode = MODE_LIGHT;

uint8_t brightness = 140;
uint8_t currentEffect = 1;
const uint8_t MAX_EFFECTS = 14;
bool g_forceOff = false;
char bt_alert = '\0';

enum SystemState { SYS_NORMAL, SYS_NOTIFY_ME, SYS_REBOOT, SYS_ROOT };
SystemState sysState = SYS_NORMAL;
unsigned long lastSysUpdate = 0;
int sysFrame = 0;

// 按键反馈动画引擎变量
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

// 心跳与通信
bool c3Connected = false;
unsigned long lastPingTime = 0;
unsigned long lastLedFrameTime = 0;
uint16_t effectFrame = 0;
unsigned long lastEffectUpdate = 0;

// ================= BLE 配置 =================
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    if (data.length() == 0) return;

    if (data == "ME_START") {
      File f = FFat.open("/me_hex.txt", FILE_WRITE);
      if (f) f.close();
    } else if (data == "ME_END") {
      sysState = SYS_NOTIFY_ME;
      sysFrame = 0;
      lastSysUpdate = millis();
    } else if (data.startsWith("ME_DATA:")) {
      File f = FFat.open("/me_hex.txt", FILE_APPEND);
      if (f) {
        f.print(data.substring(8));
        f.close();
      }
    } else if (data.startsWith("SET:")) {
      int firstColon = data.indexOf(':', 4);
      if (firstColon > 0) {
        String keyName = data.substring(4, firstColon);
        String payload = data.substring(firstColon + 1);
        preferences.putString(keyName.c_str(), payload);
        if (keyName == "MR") {
          sysState = SYS_NOTIFY_ME;
          sysFrame = 0;
          lastSysUpdate = millis();
        }
      }
    } 
    // 蓝牙控制红绿灯直接由 S3 状态机捕获
    else if (data == "R" || data == "G" || data == "B" || data == "Y") {
      bt_alert = data[0];
    } else if (data == "S") {
      bt_alert = '\0';
    } else if (data.startsWith("ALERT:")) {
      char c = data.charAt(6);
      if (c == 'R' || c == 'G' || c == 'B' || c == 'Y') bt_alert = c;
      else if (c == 'S') bt_alert = '\0';
    }
  }
};

// ================= 辅助函数：色彩与数学 =================
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
    case 0:
      r = val;
      g = (((val - base) * (hue % 10922)) / 10922) + base;
      b = base;
      break;
    case 1:
      r = (((val - base) * (10922 - (hue % 10922))) / 10922) + base;
      g = val;
      b = base;
      break;
    case 2:
      r = base;
      g = val;
      b = (((val - base) * (hue % 10922)) / 10922) + base;
      break;
    case 3:
      r = base;
      g = (((val - base) * (10922 - (hue % 10922))) / 10922) + base;
      b = val;
      break;
    case 4:
      r = (((val - base) * (hue % 10922)) / 10922) + base;
      g = base;
      b = val;
      break;
    default:
      r = val;
      g = base;
      b = (((val - base) * (10922 - (hue % 10922))) / 10922) + base;
      break;
  }
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ================= UI 与屏幕绘制 =================
void initScreenUI() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(20, 18);
  tft.print("YYQ Master-S3");
  tft.drawFastHLine(10, 45, 220, ST77XX_ORANGE);
  tft.drawRoundRect(15, 65, 210, 110, 10, ST77XX_BLUE);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(35, 205);
  tft.print("Matrix & LED Engine Sync");

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(75, 110);
  tft.print("READY");
}

void displayStatus(String title, uint16_t color = ST77XX_GREEN) {
  tft.fillRoundRect(17, 67, 206, 106, 8, ST77XX_BLACK);
  tft.drawRoundRect(15, 65, 210, 110, 10, color);

  int textSize = 3;
  if (title.length() > 5) textSize = 2;
  if (title.length() > 9) textSize = 1;
  tft.setTextSize(textSize);
  tft.setTextColor(color);

  int16_t textWidth = title.length() * 6 * textSize;
  int16_t textHeight = 8 * textSize;
  int16_t x = 120 - (textWidth / 2);
  int16_t y = 120 - (textHeight / 2);
  if (x < 20) x = 20;

  tft.setCursor(x, y);
  tft.print(title);
}

// ================= 灯效计算引擎 =================
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
  unsigned long now = millis();
  uint8_t displayEffect = currentEffect;
  static unsigned long lastAutoSwitchTime = 0;
  static uint8_t autoCycleIndex = 1;

  if (currentEffect == 13) {
    if (now - lastAutoSwitchTime > 8000) {
      lastAutoSwitchTime = now;
      autoCycleIndex++;
      if (autoCycleIndex > 12) autoCycleIndex = 1;
      effectFrame = 0;
    }
    displayEffect = autoCycleIndex;
  }

  switch (displayEffect) {
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
  // 指示灯在缓冲区下标为 16, 17, 18
  setLedRGB(16, 0, 0, 0);
  setLedRGB(17, 0, 0, 0);
  setLedRGB(18, 0, 0, 0);

  switch (currentMode) {
    case MODE_CPG:
      setLedRGB(16, 120, 0, 120); // 指示灯 0：紫色
      break;
    case MODE_MUTE:
      setLedRGB(17, 120, 0, 0);   // 指示灯 1：红色
      break;
    case MODE_LIGHT:
      setLedRGB(18, 0, 120, 0);   // 指示灯 2：绿色 (键盘灯光模式)
      break;
    case MODE_SCREEN_BRIGHTNESS:
      setLedRGB(18, 0, 120, 120); // 指示灯 2：青色 (屏幕亮度模式)
      break;
    case MODE_KEY_COLOR: {
      uint8_t colorIdx = keypressStyle % 8;
      uint32_t c = (colorIdx == 0) ? 0x500050 : keypressColors[colorIdx];
      uint8_t r = ((c >> 16) & 0xFF) * 30 / 100;
      uint8_t g = ((c >> 8) & 0xFF) * 30 / 100;
      uint8_t b = (c & 0xFF) * 30 / 100;
      setLedRGB(16, r, g, b);
      setLedRGB(17, r, g, b);
      setLedRGB(18, r, g, b);
      break;
    }
  }
}

void renderLightingEngine() {
  unsigned long now = millis();

  // 1. 全关模式
  if (g_forceOff) {
    for (int i = 0; i < TOTAL_LEDS; i++) setLedRGB(i, 0, 0, 0);
    return;
  }

  // 2. 蓝牙红绿灯报警
  if (bt_alert != '\0') {
    static unsigned long lastFlash = 0;
    static bool flashOn = false;
    if (now - lastFlash > 350) {
      lastFlash = now;
      flashOn = !flashOn;
    }
    if (flashOn) {
      switch (bt_alert) {
        case 'R': setMainLedsColor(255, 0, 0); break;
        case 'G': setMainLedsColor(0, 255, 0); break;
        case 'B': setMainLedsColor(0, 0, 255); break;
        case 'Y': setMainLedsColor(255, 180, 0); break;
      }
    } else {
      clearMainLeds();
    }
    renderIndicators();
    return;
  }

  // 3. 特殊系统动画
  if (sysState != SYS_NORMAL) {
    if (sysState == SYS_NOTIFY_ME) {
      if (now - lastSysUpdate > 80) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) setMainLedsColor(255, 200, 0);
        else setMainLedsColor(0, 255, 255);
        sysFrame++;
        if (sysFrame > 20) sysState = SYS_NORMAL;
      }
    } else if (sysState == SYS_REBOOT) {
      if (now - lastSysUpdate > 400) {
        lastSysUpdate = now;
        if (sysFrame == 0) setMainLedsColor(255, 0, 0);
        else if (sysFrame == 1) setMainLedsColor(0, 255, 0);
        else if (sysFrame == 2) setMainLedsColor(0, 0, 255);
        else setMainLedsColor(255, 255, 255);
        sysFrame++;
        if (sysFrame > 4) sysState = SYS_NORMAL;
      }
    } else if (sysState == SYS_ROOT) {
      if (now - lastSysUpdate > 300) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) setMainLedsColor(255, 120, 0);
        else clearMainLeds();
        sysFrame++;
      }
    }
    renderIndicators();
    return;
  }

  // 4. 正常灯效渲染
  renderMainEffects();

  // 叠加按键反馈涟漪
  if (isReactionActive) {
    updateKeyReaction();
  } else if (reactionType == 2 && stackCount > 0) {
    for (int i = 0; i < stackCount; i++) {
      setLedRGB(i, (currentReactionColor >> 16) & 0xFF, (currentReactionColor >> 8) & 0xFF, currentReactionColor & 0xFF);
    }
  }

  // 渲染指示灯
  renderIndicators();
}

// ================= 高速推流协议：S3 -> C3 =================
// 协议格式：[0xAA] [0x55] [0x01] [57字节RGB] [0xEE]
void sendLedFrameToC3() {
  uint8_t packet[61];
  packet[0] = 0xAA;
  packet[1] = 0x55;
  packet[2] = 0x01; // 命令码 0x01：LED Frame

  // 应用整体亮度（前16颗为主灯应用 brightness，后3颗指示灯亮度固定）
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

  packet[60] = 0xEE; // 尾帧校验
  Serial1.write(packet, sizeof(packet));
}

// ================= 键盘矩阵与宏执行 =================
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

  String macroData = preferences.getString(keyName.c_str(), "");
  if (macroData.length() == 0) return;

  if (macroData.startsWith("SEQ:")) {
    String seq = macroData.substring(4);
    int i = 0;
    while (i < seq.length()) {
      if (seq[i] == '[') {
        int endBracket = seq.indexOf(']', i);
        if (endBracket != -1) {
          String tag = seq.substring(i + 1, endBracket);
          bool matched = true;
          if (tag == "ENTER") Keyboard.write(KEY_RETURN);
          else if (tag == "TAB") Keyboard.write(KEY_TAB);
          else if (tag == "ESC") Keyboard.write(KEY_ESC);
          else if (tag == "BACKSPACE") Keyboard.write(KEY_BACKSPACE);
          else matched = false;
          if (matched) {
            i = endBracket + 1;
            continue;
          }
        }
      }
      Keyboard.print(seq[i++]);
      delay(5);
    }
  } else if (macroData.startsWith("CMB:")) {
    String cmb = macroData.substring(4);
    int commaIdx = 0;
    while (cmb.length() > 0) {
      commaIdx = cmb.indexOf(',');
      uint8_t kCode = (commaIdx == -1) ? cmb.toInt() : cmb.substring(0, commaIdx).toInt();
      if (kCode > 0) Keyboard.press(kCode);
      if (commaIdx == -1) break;
      cmb = cmb.substring(commaIdx + 1);
    }
    delay(50);
    Keyboard.releaseAll();
  }
}

String getMacroNameByCode(uint16_t code) {
  switch (code) {
    case K_M1: return "M1";
    case K_M2: return "M2";
    case K_M3: return "M3";
    case K_M4: return "M4";
    case K_M5: return "M5";
    case K_M6: return "M6";
    case K_M7: return "M7";
    case K_M8: return "M8";
    case K_M9: return "M9";
    case K_M10: return "M10";
    case K_M11: return "M11";
    case K_M12: return "M12";
    case K_MA: return "MA";
    case K_MB: return "MB";
    case K_MC: return "MC";
    case K_MR: return "MR";
    case K_ME: return "ME";
    case K_LOGO: return "LOGO";
    default: return "";
  }
}

String getKeyDisplayName(uint16_t code) {
  if (code >= MACRO_BASE) {
    if (code == K_NEXT) return "NEXT";
    if (code == K_PLAY) return "PLAY";
    if (code == K_PREV) return "PREV";
    if (code == K_FN) return "FN";
    String mName = getMacroNameByCode(code);
    if (mName.length() > 0) return mName;
    return "MACRO";
  }
  switch (code) {
    case KEY_LEFT_CTRL:   return "L-Ctrl";
    case KEY_LEFT_SHIFT:  return "L-Shift";
    case KEY_LEFT_ALT:    return "L-Alt";
    case KEY_LEFT_GUI:    return "Win";
    case KEY_RIGHT_CTRL:  return "R-Ctrl";
    case KEY_RIGHT_SHIFT: return "R-Shift";
    case KEY_RIGHT_ALT:   return "R-Alt";
    case KEY_UP_ARROW:    return "UP";
    case KEY_DOWN_ARROW:  return "DOWN";
    case KEY_LEFT_ARROW:  return "LEFT";
    case KEY_RIGHT_ARROW: return "RIGHT";
    case KEY_RETURN:      return "Enter";
    case KEY_ESC:         return "ESC";
    case KEY_BACKSPACE:   return "BackSp";
    case KEY_TAB:         return "Tab";
    case KEY_CAPS_LOCK:   return "Caps";
    case ' ':             return "Space";
    default: break;
  }
  if (code >= 33 && code <= 126) return String((char)code);
  char hexBuf[10];
  snprintf(hexBuf, sizeof(hexBuf), "0x%02X", code);
  return String(hexBuf);
}

void initKeyMatrix() {
  keyMatrix[0][0] = KEY_LEFT_ALT;
  keyMatrix[0][1] = 0xE9;
  keyMatrix[0][2] = 0xE8;
  keyMatrix[0][3] = 0xE7;
  keyMatrix[0][4] = 0xDE;
  keyMatrix[0][5] = 0xDD;
  keyMatrix[0][6] = 0xDC;
  keyMatrix[0][7] = 0xDB;
  keyMatrix[0][8] = KEY_RIGHT_ARROW;
  keyMatrix[0][9] = KEY_DOWN_ARROW;
  keyMatrix[0][10] = KEY_LEFT_ARROW;
  keyMatrix[0][11] = KEY_RIGHT_CTRL;
  keyMatrix[0][12] = 0xED;
  keyMatrix[0][13] = K_FN;
  keyMatrix[0][14] = KEY_RIGHT_ALT;
  keyMatrix[0][15] = ' ';

  keyMatrix[1][0] = 0xDF;
  keyMatrix[1][1] = K_MB;
  keyMatrix[1][2] = K_MA;
  keyMatrix[1][3] = K_NEXT;
  keyMatrix[1][4] = K_PLAY;
  keyMatrix[1][5] = K_PREV;
  keyMatrix[1][6] = K_LOGO;
  keyMatrix[1][7] = 0xE0;
  keyMatrix[1][8] = 0xEB;
  keyMatrix[1][9] = 0xEA;
  keyMatrix[1][10] = 0xE3;
  keyMatrix[1][11] = 0xE2;
  keyMatrix[1][12] = 0xE1;
  keyMatrix[1][13] = 0xE6;
  keyMatrix[1][14] = 0xE5;
  keyMatrix[1][15] = 0xE4;

  keyMatrix[2][0] = KEY_LEFT_SHIFT;
  keyMatrix[2][1] = KEY_LEFT_GUI;
  keyMatrix[2][2] = KEY_LEFT_CTRL;
  keyMatrix[2][3] = KEY_UP_ARROW;
  keyMatrix[2][4] = KEY_RIGHT_SHIFT;
  keyMatrix[2][5] = '/';
  keyMatrix[2][6] = '.';
  keyMatrix[2][7] = ',';
  keyMatrix[2][8] = 'm';
  keyMatrix[2][9] = 'n';
  keyMatrix[2][10] = 'b';
  keyMatrix[2][11] = 'v';
  keyMatrix[2][12] = 'c';
  keyMatrix[2][13] = 'x';
  keyMatrix[2][14] = 'z';

  keyMatrix[4][0] = KEY_END;
  keyMatrix[4][1] = KEY_RETURN;
  keyMatrix[4][3] = '\'';
  keyMatrix[4][4] = ';';
  keyMatrix[4][5] = 'l';
  keyMatrix[4][6] = 'k';
  keyMatrix[4][7] = 'j';
  keyMatrix[4][8] = 'h';
  keyMatrix[4][9] = 'g';
  keyMatrix[4][10] = 'f';
  keyMatrix[4][11] = 'd';
  keyMatrix[4][12] = 's';
  keyMatrix[4][13] = 'a';
  keyMatrix[4][14] = KEY_CAPS_LOCK;
  keyMatrix[4][15] = 0xD6;

  keyMatrix[5][0] = KEY_PAGE_UP;
  keyMatrix[5][1] = KEY_DELETE;
  keyMatrix[5][2] = '\\';
  keyMatrix[5][3] = ']';
  keyMatrix[5][4] = '[';
  keyMatrix[5][5] = 'p';
  keyMatrix[5][6] = 'o';
  keyMatrix[5][7] = 'i';
  keyMatrix[5][8] = 'u';
  keyMatrix[5][9] = 'y';
  keyMatrix[5][10] = 't';
  keyMatrix[5][11] = 'r';
  keyMatrix[5][12] = 'e';
  keyMatrix[5][13] = 'w';
  keyMatrix[5][14] = 'q';
  keyMatrix[5][15] = KEY_TAB;

  keyMatrix[6][0] = '`';
  keyMatrix[6][1] = KEY_HOME;
  keyMatrix[6][2] = KEY_INSERT;
  keyMatrix[6][3] = KEY_BACKSPACE;
  keyMatrix[6][4] = '=';
  keyMatrix[6][5] = '-';
  keyMatrix[6][6] = '0';
  keyMatrix[6][7] = '9';
  keyMatrix[6][8] = '8';
  keyMatrix[6][9] = '7';
  keyMatrix[6][10] = '6';
  keyMatrix[6][11] = '5';
  keyMatrix[6][12] = '4';
  keyMatrix[6][13] = '3';
  keyMatrix[6][14] = '2';
  keyMatrix[6][15] = '1';

  keyMatrix[7][0] = KEY_ESC;
  keyMatrix[7][1] = 0xD0;
  keyMatrix[7][2] = 0xCF;
  keyMatrix[7][3] = 0xCE;
  keyMatrix[7][4] = KEY_F12;
  keyMatrix[7][5] = KEY_F11;
  keyMatrix[7][6] = KEY_F10;
  keyMatrix[7][7] = KEY_F9;
  keyMatrix[7][8] = KEY_F8;
  keyMatrix[7][9] = KEY_F7;
  keyMatrix[7][10] = KEY_F6;
  keyMatrix[7][11] = KEY_F5;
  keyMatrix[7][12] = KEY_F4;
  keyMatrix[7][13] = KEY_F3;
  keyMatrix[7][14] = KEY_F2;
  keyMatrix[7][15] = KEY_F1;

  keyMatrix[8][0] = K_MC;
  keyMatrix[8][2] = K_ME;
  keyMatrix[8][3] = K_M12;
  keyMatrix[8][4] = K_M11;
  keyMatrix[8][5] = K_M10;
  keyMatrix[8][6] = K_M9;
  keyMatrix[8][7] = K_M8;
  keyMatrix[8][8] = K_M7;
  keyMatrix[8][9] = K_M6;
  keyMatrix[8][10] = K_M5;
  keyMatrix[8][11] = K_M4;
  keyMatrix[8][12] = K_M3;
  keyMatrix[8][13] = K_M2;
  keyMatrix[8][14] = K_M1;
  keyMatrix[8][15] = K_MR;
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

          uint16_t keycode = keyMatrix[r][c];
          if (keycode == 0) continue;

          if (currentState) {
            String kName = getKeyDisplayName(keycode);
            displayStatus(kName, ST77XX_GREEN);

            // 本地零延迟直接触发按键反馈灯效
            if (cherryLogoEnabled) triggerKeyReaction();

            if (keycode == K_FN) {
              fnPressed = true;
            } else if (keycode >= MACRO_BASE) {
              if (keycode == K_LOGO && fnPressed) {
                sysState = SYS_REBOOT;
                delay(100);
                esp_restart();
              } else if (keycode == K_PLAY) {
                if (fnPressed) {
                  SystemControl.press(SYSTEM_CONTROL_STANDBY);
                  SystemControl.release();
                } else ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
              } else if (keycode == K_NEXT) {
                if (fnPressed) {
                  SystemControl.press(SYSTEM_CONTROL_WAKE_HOST);
                  SystemControl.release();
                  Keyboard.press(' ');
                  Keyboard.release(' ');
                } else ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
              } else if (keycode == K_PREV) {
                if (fnPressed) {
                  SystemControl.press(SYSTEM_CONTROL_POWER_OFF);
                  SystemControl.release();
                } else ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
              } else if (keycode != K_LOGO) {
                executeMacro(getMacroNameByCode(keycode));
              }
            } else {
              Keyboard.press((uint8_t)keycode);
            }
          } else {
            if (keycode == K_FN) {
              fnPressed = false;
            } else if (keycode < MACRO_BASE) {
              Keyboard.release((uint8_t)keycode);
            } else {
              if (keycode == K_PLAY || keycode == K_NEXT || keycode == K_PREV) {
                ConsumerControl.release();
              } else if (keycode == K_LOGO) {
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

// ================= C3 事件接收与统一决策中心 =================
void handleC3Events() {
  static String serialBuffer = "";
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer == "PONG") {
        c3Connected = true;
      } 
      // 1. 旋钮动作处理
      else if (serialBuffer.startsWith("ENC:")) {
        bool isRight = (serialBuffer == "ENC:+");
        if (g_forceOff) g_forceOff = false;

        if (currentMode == MODE_LIGHT) {
          if (isRight) brightness = (brightness <= 200) ? brightness + 20 : 220;
          else brightness = (brightness >= 20) ? brightness - 20 : 0;
          displayStatus("LED " + String((brightness * 100) / 220) + "%", ST77XX_MAGENTA);
        } else if (currentMode == MODE_SCREEN_BRIGHTNESS) {
          if (isRight) {
            ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_INCREMENT);
            ConsumerControl.release();
            displayStatus("SCR BRT +", ST77XX_CYAN);
          } else {
            ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_DECREMENT);
            ConsumerControl.release();
            displayStatus("SCR BRT -", ST77XX_CYAN);
          }
        } else if (currentMode == MODE_MUTE) {
          if (isRight) {
            ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
            ConsumerControl.release();
            displayStatus("VOL +", ST77XX_RED);
          } else {
            ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
            ConsumerControl.release();
            displayStatus("VOL -", ST77XX_RED);
          }
        } else if (currentMode == MODE_CPG) {
          if (isRight) currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          else currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
          displayStatus("EFFECT " + String(currentEffect), ST77XX_ORANGE);
        } else if (currentMode == MODE_KEY_COLOR) {
          if (isRight) keypressStyle = (keypressStyle + 1) % 24;
          else keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
          displayStatus("KEY STYLE " + String(keypressStyle), ST77XX_YELLOW);
          triggerKeyReaction();
        }
      }
      // 2. 按键动作处理
      else if (serialBuffer == "BTN:LIGHT") {
        if (g_forceOff) {
          g_forceOff = false;
          currentMode = MODE_LIGHT;
          displayStatus("LIGHTS ON", ST77XX_GREEN);
        } else {
          // 单击：在键盘亮度与屏幕亮度之间循环切换
          if (currentMode == MODE_LIGHT) {
            currentMode = MODE_SCREEN_BRIGHTNESS;
            displayStatus("SCR BRIGHT", ST77XX_CYAN);
          } else {
            currentMode = MODE_LIGHT;
            displayStatus("LED BRIGHT", ST77XX_GREEN);
          }
        }
      } else if (serialBuffer == "BTN:LIGHT_HOLD") {
        g_forceOff = !g_forceOff;
        displayStatus(g_forceOff ? "ALL OFF" : "ALL ON", ST77XX_RED);
      } else if (serialBuffer == "BTN:MUTE") {
        if (g_forceOff) g_forceOff = false;
        currentMode = MODE_MUTE;
        displayStatus("MUTE MODE", ST77XX_RED);
      } else if (serialBuffer == "BTN:MUTE_HOLD") {
        displayStatus("REBOOT...", ST77XX_RED);
        delay(100);
        esp_restart();
      } else if (serialBuffer == "BTN:CPG") {
        if (g_forceOff) g_forceOff = false;
        if (currentMode != MODE_CPG) {
          currentMode = MODE_CPG;
        } else {
          currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          if (currentEffect == 0) currentEffect = 1;
        }
        displayStatus("CPG EFF " + String(currentEffect), ST77XX_ORANGE);
      } else if (serialBuffer == "BTN:CPG_HOLD") {
        displayStatus("ROOT BOOT", ST77XX_RED);
        delay(100);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
      } else if (serialBuffer == "BTN:KNOB") {
        if (g_forceOff) g_forceOff = false;
        currentMode = MODE_KEY_COLOR;
        displayStatus("KEY COLOR", ST77XX_YELLOW);
        triggerKeyReaction();
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

  USB.VID(0x303A);
  USB.PID(0x001F);
  USB.productName("YYQ-MX9.0");
  USB.manufacturerName("YYQ");

  Keyboard.begin();
  ConsumerControl.begin();
  SystemControl.begin();
  USB.begin();

  tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
  tft.init(240, 240);
  tft.setRotation(1);
  initScreenUI();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(25);
  if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) recoverI2CBus();

  preferences.begin("macros", false);
  cherryLogoEnabled = preferences.getBool("cherryLogo", false);
  FFat.begin(true);
  initKeyMatrix();

  for (int c = 0; c < numCols; c++) {
    mcp.pinMode(c, OUTPUT);
    mcp.digitalWrite(c, HIGH);
  }
  for (int r = 0; r < numRows; r++) { pinMode(rowPins[r], INPUT_PULLUP); }

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
  Serial.println("S3 主控全流程启动完毕");
}

unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 3;

// ================= LOOP =================
void loop() {
  esp_task_wdt_reset();

  // 1. 蓝牙维护
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    BLEDevice::startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

  // 2. 心跳监测
  if (millis() - lastPingTime > 2000) {
    lastPingTime = millis();
    uint8_t pingPacket[] = { 0xAA, 0x55, 0x02, 0xEE };
    Serial1.write(pingPacket, sizeof(pingPacket));
  }

  // 3. 处理 C3 上报的物理事件
  handleC3Events();

  // 4. 键盘矩阵扫描 (按键极速响应)
  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    lastScanTime = millis();
    scanKeyboardMatrix();
  }

  // 5. 50 FPS 灯光引擎计算与推流 (每 20ms 一帧)
  if (millis() - lastLedFrameTime >= 20) {
    lastLedFrameTime = millis();
    renderLightingEngine();
    sendLedFrameToC3();
  }

  delay(1);
}