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
#include <esp_task_wdt.h>  // 引入硬件看门狗库

// ================= TFT 屏幕相关库与引脚定义 =================
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_SCL   4
#define TFT_SDA  16
#define TFT_DC   15
#define TFT_CS    5
#define TFT_RST  -1  // 如果屏幕有RST引脚且接了GPIO，请修改此处(如接了GPIO某脚则写对应编号)

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
Preferences preferences;
Adafruit_MCP23X17 mcp;
#define WDT_TIMEOUT 3  // 看门狗超时时间设置为 3 秒

// ================= 引脚与通讯定义 =================
#define I2C_SDA 14
#define I2C_SCL 13
#define MCP23017_ADDR 0x20

#define RX_PIN 10
#define TX_PIN 9

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

// 心跳与串口控制变量
bool c3Connected = false;
unsigned long lastPingTime = 0;

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

bool lastState[numRows][numCols] = { false };
unsigned long lastDebounceTime[numRows][numCols] = { 0 };

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic;

bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    if (data.length() == 0) return;

    if (data == "ME_START") {
      File f = FFat.open("/me_hex.txt", FILE_WRITE);
      if (f) f.close();
    } else if (data == "ME_END") {
      Serial1.println("N_ME");
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
        if (keyName == "MR") Serial1.println("N_ME");
      }
    }
  }
};

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
          esp_task_wdt_reset();  // 避免大文件读取打印时看门狗超时重启
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

// ================= 按键名称转换函数 =================
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
    case KEY_PAGE_UP:     return "PgUp";
    case KEY_PAGE_DOWN:   return "PgDn";
    case KEY_DELETE:      return "Del";
    case KEY_HOME:        return "Home";
    case KEY_END:         return "End";
    case KEY_INSERT:      return "Ins";
    case KEY_F1:  return "F1";
    case KEY_F2:  return "F2";
    case KEY_F3:  return "F3";
    case KEY_F4:  return "F4";
    case KEY_F5:  return "F5";
    case KEY_F6:  return "F6";
    case KEY_F7:  return "F7";
    case KEY_F8:  return "F8";
    case KEY_F9:  return "F9";
    case KEY_F10: return "F10";
    case KEY_F11: return "F11";
    case KEY_F12: return "F12";
    case ' ':     return "Space";
    default: break;
  }

  // 常见 ASCII 可打印字符
  if (code >= 33 && code <= 126) {
    return String((char)code);
  }

  // 未命名的特殊键码输出十六进制
  char hexBuf[10];
  snprintf(hexBuf, sizeof(hexBuf), "0x%02X", code);
  return String(hexBuf);
}

// ================= 屏幕绘制与显示函数 =================
void initScreenUI() {
  tft.fillScreen(ST77XX_BLACK);
  
  // 顶部标题栏
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(20, 18);
  tft.print("YYQ Keyboard");
  tft.drawFastHLine(10, 45, 220, ST77XX_ORANGE);

  // 中间按键显示主卡片框
  tft.drawRoundRect(15, 65, 210, 110, 10, ST77XX_BLUE);

  // 底部提示
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(40, 205);
  tft.print("Matrix: 10x16 Active");

  // 默认待机显示
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(75, 110);
  tft.print("READY");
}

void displayKeyPressed(String keyName) {
  // 只局部清除卡片内部区域，杜绝整屏刷新带来的闪烁与性能损耗
  tft.fillRoundRect(17, 67, 206, 106, 8, ST77XX_BLACK);
  tft.drawRoundRect(15, 65, 210, 110, 10, ST77XX_GREEN);

  // 根据文字长度动态调整字号，保持居中
  int textSize = 3;
  if (keyName.length() > 5) textSize = 2;
  if (keyName.length() > 9) textSize = 1;
  tft.setTextSize(textSize);
  tft.setTextColor(ST77XX_GREEN);

  int16_t textWidth = keyName.length() * 6 * textSize;
  int16_t textHeight = 8 * textSize;
  int16_t x = 120 - (textWidth / 2);
  int16_t y = 120 - (textHeight / 2);
  if (x < 20) x = 20;

  tft.setCursor(x, y);
  tft.print(keyName);
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
            // 【核心新增功能】：按键按下，在 1.54 寸屏幕上显示按键名称
            String kName = getKeyDisplayName(keycode);
            displayKeyPressed(kName);

            if (cherryLogoEnabled) Serial1.println("N_KEY_PRESS");

            if (keycode == K_FN) {
              fnPressed = true;
            } else if (keycode >= MACRO_BASE) {
              if (keycode == K_LOGO && fnPressed) {
                Serial1.println("N_REBOT");
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

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  USB.VID(0x303A);
  USB.PID(0x001F);
  USB.productName("YYQ-MX9.0");
  USB.manufacturerName("YYQ");

  Keyboard.begin();
  ConsumerControl.begin();
  SystemControl.begin();
  USB.begin();

  // ================= 屏幕初始化 =================
  tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
  tft.init(240, 240);  // 1.54寸 IPS 分辨率通常为 240x240
  tft.setRotation(1);  // 若方向不正确，可调整为 1, 2 或 3
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

  // ================= 初始化硬件看门狗 =================
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
  Serial.println("看门狗安全机制已开启 (3秒无响应自动重启)");
}

unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 3;

void loop() {
  esp_task_wdt_reset();  // 喂狗

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    BLEDevice::startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

  // 硬件异步心跳：每 2 秒广播一次 PING
  if (millis() - lastPingTime > 2000) {
    lastPingTime = millis();
    Serial1.println("S3_PING");
  }

  // 非阻塞串口接收解析
  static String serialBuffer = "";
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer == "C3_PONG") {
        c3Connected = true;
      } else if (serialBuffer == "V+") {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
        ConsumerControl.release();
      } else if (serialBuffer == "V-") {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
        ConsumerControl.release();
      } else if (serialBuffer == "N_S3_REBOT") {
        delay(100);
        esp_restart();
      } else if (serialBuffer == "N_S3_ROOT") {
        delay(100);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
      }
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
      if (serialBuffer.length() > 64) serialBuffer = "";
    }
  }

  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    lastScanTime = millis();
    scanKeyboardMatrix();
  }
  delay(1);
}