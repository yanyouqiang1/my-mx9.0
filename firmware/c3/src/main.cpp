#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Update.h>

// ================= 蓝牙OTA服务配置 =================
#define OTA_SERVICE_UUID "0000FFFF-0000-1000-8000-00805F9B34FB"
#define OTA_CHAR_UUID    "0000FF01-0000-1000-8000-00805F9B34FB"

// ================= C3 引脚定义 =================
#define PIN_CPG         1
#define PIN_MUTE        2
#define PIN_LIGHT       3
#define PIN_KNOB_BTN    0
#define PIN_ENCODER_A   5
#define PIN_ENCODER_B   6

#define PIN_WS2812      21
#define PIN_INDICATOR   9

#define RX_PIN          10
#define TX_PIN          20

#define NUM_LEDS        16
#define NUM_INDICATORS  3

Adafruit_NeoPixel strip(NUM_LEDS, PIN_WS2812, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel indicatorStrip(NUM_INDICATORS, PIN_INDICATOR, NEO_GRB + NEO_KHZ800);

// ================= 蓝牙服务配置 =================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// ================= 全局状态与控制模式 =================
enum ControlMode {
  MODE_CPG,
  MODE_MUTE,
  MODE_LIGHT,
  MODE_KEY_COLOR
};
ControlMode currentMode = MODE_LIGHT;

uint8_t brightness = 140;
uint8_t currentEffect = 1;
const uint8_t MAX_EFFECTS = 14;

enum SystemState {
  SYS_NORMAL,
  SYS_NOTIFY_ME,
  SYS_REBOOT,
  SYS_ROOT
};
SystemState sysState = SYS_NORMAL;

unsigned long lastSysUpdate = 0;
int sysFrame = 0;
bool g_forceOff = false;
char bt_alert = '\0';
bool g_forceRedraw = true;

bool isRippleActive = false;
int rippleStep = 0;
unsigned long lastRippleUpdate = 0;
uint32_t currentRippleColor = 0;

uint8_t keypressColorMode = 0;
const char* keypressColorNames[] = { "自动彩环", "热血红", "极光绿", "深海蓝", "冰晶蓝", "幻境紫", "柠檬黄", "纯净白" };
const uint32_t keypressColors[] = {
  0x000000,
  0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF, 0xB400FF, 0xFFFF00, 0xFFFFFF
};

// ================= 物理按键变量 =================
bool lastMuteState = HIGH;
unsigned long lastMuteTime = 0;
bool lastLightState = HIGH;
unsigned long lastLightTime = 0;
bool lastCpgState = HIGH;
unsigned long lastCpgTime = 0;
bool lastKnobState = HIGH;
unsigned long lastKnobTime = 0;
unsigned long knobPressStart = 0;
bool knobWasPressed = false;

int lastClkState;
unsigned long lastEffectUpdate = 0;
uint16_t effectFrame = 0;

String inputBuffer = "";

// ================= 蓝牙回调类 =================
class MyBLECallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    if (rxValue.length() > 0) {
      char cmd = rxValue[0];
      Serial.printf("蓝牙收到指令: %c\n", cmd);

      if (cmd == 'R' || cmd == 'B' || cmd == 'G' || cmd == 'Y') {
        bt_alert = cmd;
        g_forceRedraw = true;
        Serial.printf("触发蓝牙提示闪烁: %c\n", cmd);
      }
      else if (cmd == 'S') {
        bt_alert = '\0';
        g_forceRedraw = true;
        Serial.println("蓝牙提示关闭，恢复常规键盘与CPG控制");
      }
    }
  }
};

// ================= 蓝牙OTA回调类 =================
class OtaCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    std::string rxData = pCharacteristic->getValue();
    if (rxData.length() > 0) {
      if (rxData[0] == 0xF0) {
        if (Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.setAuthCode(0xF0);
          Serial.println("OTA开始接收固件...");
        }
      } else if (rxData[0] == 0xF1) {
        if (Update.end(true)) {
          Serial.println("OTA完成，正在重启...");
          delay(100);
          ESP.restart();
        }
      } else {
        Update.write((uint8_t*)rxData.data(), rxData.size());
      }
    }
  }
};

// ================= 蓝牙OTA初始化 =================
void initOTA() {
  BLEService *pOtaService = BLEDevice::createService(OTA_SERVICE_UUID);
  BLECharacteristic *pOtaChar = pOtaService->createCharacteristic(
      OTA_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY
  );
  pOtaChar->setCallbacks(new OtaCharacteristicCallbacks());
  pOtaService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
  Serial.println("BLE OTA服务已启动");
}

// ================= setup =================
void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(PIN_CPG, INPUT_PULLUP);
  pinMode(PIN_MUTE, INPUT_PULLUP);
  pinMode(PIN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_KNOB_BTN, INPUT_PULLUP);
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  lastClkState = digitalRead(PIN_ENCODER_A);

  delay(100);

  strip.begin();
  strip.setBrightness(brightness);
  strip.show();

  pinMode(PIN_INDICATOR, OUTPUT);
  indicatorStrip.begin();
  indicatorStrip.setBrightness(80);
  updateIndicators();

  // 初始化蓝牙
  BLEDevice::init("C3_Keyboard_BLE");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  pRxCharacteristic->setCallbacks(new MyBLECallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  Serial.println("蓝牙模块启动完毕，等待主控端连接...");

  // 初始化蓝牙OTA服务
  initOTA();
}

void loop() {
  handleSerial();
  handleButtons();
  handleEncoder();
  updateLightingEffect();
}

// ================= 串口通信处理 =================
void handleSerial() {
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      inputBuffer.trim();
      processSerialCommand(inputBuffer);
      inputBuffer = "";
    } else if (c != '\r') {
      inputBuffer += c;
      if (inputBuffer.length() > 64) {
        inputBuffer = "";
      }
    }
  }
}

void processSerialCommand(String cmd) {
  if (cmd == "N_ME") {
    sysState = SYS_NOTIFY_ME;
    sysFrame = 0;
    lastSysUpdate = millis();
  }
  else if (cmd == "N_REBOT") {
    sysState = SYS_REBOOT;
    sysFrame = 0;
    lastSysUpdate = millis();
  }
  else if (cmd == "N_ROOT") {
    sysState = SYS_ROOT;
    sysFrame = 0;
    lastSysUpdate = millis();
  }
  else if (cmd == "N_KEY_PRESS") {
    if (sysState == SYS_NORMAL) {
      triggerRipple();
      currentMode = MODE_KEY_COLOR;
      updateIndicators();
    }
  }
}

void exitSystemState() {
  if (sysState != SYS_NORMAL) {
    sysState = SYS_NORMAL;
    strip.clear();
    strip.show();
    g_forceRedraw = true;
  }
}

// ================= 底部指示灯更新 =================
void updateIndicators() {
  indicatorStrip.clear();
  switch (currentMode) {
    case MODE_CPG:
      indicatorStrip.setPixelColor(0, indicatorStrip.Color(120, 0, 120));
      break;
    case MODE_MUTE:
      indicatorStrip.setPixelColor(1, indicatorStrip.Color(120, 0, 0));
      break;
    case MODE_LIGHT:
      indicatorStrip.setPixelColor(2, indicatorStrip.Color(0, 120, 0));
      break;
    case MODE_KEY_COLOR:
      uint32_t activeColor = (keypressColorMode == 0) ? indicatorStrip.Color(80, 0, 80) : keypressColors[keypressColorMode];
      uint8_t r = ((activeColor >> 16) & 0xFF) * 0.3;
      uint8_t g = ((activeColor >> 8) & 0xFF) * 0.3;
      uint8_t b = (activeColor & 0xFF) * 0.3;
      indicatorStrip.fill(indicatorStrip.Color(r, g, b));
      break;
  }
  indicatorStrip.show();
}

// ================= 物理按键处理 =================
void handleButtons() {
  unsigned long now = millis();

  // Mute按键
  bool muteReading = digitalRead(PIN_MUTE);
  if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
  }
  if ((now - lastMuteTime) > 50) {
    static unsigned long mutePressStart = 0;
    static bool muteTriggered = false;
    static bool muteLongPressHandled = false;

    if (muteReading == LOW && !muteTriggered) {
      muteTriggered = true;
      mutePressStart = now;
      muteLongPressHandled = false;
      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
      }
      exitSystemState();
    }

    if (muteReading == LOW && muteTriggered && !muteLongPressHandled) {
      unsigned long pressDuration = now - mutePressStart;
      if (pressDuration >= 3000 && pressDuration < 8000) {
        Serial1.println("N_S3_REBOT");
        muteLongPressHandled = true;
      }
      else if (pressDuration >= 8000) {
        Serial1.println("N_S3_ROOT");
        muteLongPressHandled = true;
      }
    }

    if (muteReading == HIGH && muteTriggered) {
      unsigned long pressDuration = now - mutePressStart;
      if (pressDuration < 3000 && !muteLongPressHandled) {
        currentMode = MODE_MUTE;
        updateIndicators();
      }
      muteTriggered = false;
      muteLongPressHandled = false;
    }
  }

  // Light按键
  bool lightReading = digitalRead(PIN_LIGHT);
  if (lightReading != lastLightState) {
    lastLightTime = now;
    lastLightState = lightReading;
  }
  if ((now - lastLightTime) > 50) {
    static bool lightTriggered = false;
    static bool longPressTriggered = false;

    if (lightReading == LOW) {
      if (!lightTriggered) {
        lightTriggered = true;
        longPressTriggered = false;
      }
      if (!longPressTriggered && (now - lastLightTime >= 2000)) {
        longPressTriggered = true;
        g_forceOff = true;
        bt_alert = '\0';
        sysState = SYS_NORMAL;
        isRippleActive = false;
        currentEffect = 0;
        strip.clear();
        strip.show();
      }
    } else {
      if (lightTriggered) {
        lightTriggered = false;
        if (!longPressTriggered) {
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          } else {
            exitSystemState();
            currentMode = MODE_LIGHT;
            updateIndicators();
          }
        }
      }
    }
  }

  // CPG按键
  bool cpgReading = digitalRead(PIN_CPG);
  if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
  }
  if ((now - lastCpgTime) > 50) {
    static unsigned long cpgPressStart = 0;
    static bool cpgTriggered = false;
    static bool cpgLongPressHandled = false;

    if (cpgReading == LOW && !cpgTriggered) {
      cpgTriggered = true;
      cpgPressStart = now;
      cpgLongPressHandled = false;
      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
      }
      exitSystemState();
    }

    if (cpgReading == LOW && cpgTriggered && !cpgLongPressHandled) {
      unsigned long pressDuration = now - cpgPressStart;
      if (pressDuration >= 3000) {
        Serial1.println("N_S3_ROOT");
        cpgLongPressHandled = true;
      }
    }

    if (cpgReading == HIGH && cpgTriggered) {
      unsigned long pressDuration = now - cpgPressStart;
      if (pressDuration < 3000 && !cpgLongPressHandled) {
        if (currentMode != MODE_CPG) {
          currentMode = MODE_CPG;
        } else {
          currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          if (currentEffect == 0) currentEffect = 1;
        }
        updateIndicators();
        effectFrame = 0;
        g_forceRedraw = true;
      }
      cpgTriggered = false;
      cpgLongPressHandled = false;
    }
  }

  // 旋钮按键
  bool knobReading = digitalRead(PIN_KNOB_BTN);
  if (knobReading != lastKnobState) {
    lastKnobTime = now;
    lastKnobState = knobReading;
  }
  if ((now - lastKnobTime) > 50) {
    if (knobReading == LOW) {
      if (!knobWasPressed) {
        knobWasPressed = true;
        knobPressStart = now;
      }
    } else {
      if (knobWasPressed) {
        unsigned long pressDuration = now - knobPressStart;
        knobWasPressed = false;
        if (g_forceOff) {
          g_forceOff = false;
          currentEffect = 1;
          g_forceRedraw = true;
        }
        exitSystemState();
        if (pressDuration < 800) {
          currentMode = MODE_KEY_COLOR;
          updateIndicators();
        } else {
          currentEffect = 0;
          g_forceRedraw = true;
        }
      }
    }
  }
}

// ================= 旋转编码器处理 =================
void handleEncoder() {
  int currentClk = digitalRead(PIN_ENCODER_A);
  if (currentClk != lastClkState && currentClk == LOW) {
    if (g_forceOff) {
      g_forceOff = false;
      currentEffect = 1;
      g_forceRedraw = true;
    }
    exitSystemState();

    bool isRight = (digitalRead(PIN_ENCODER_B) == currentClk);

    if (currentMode == MODE_LIGHT) {
      if (isRight) {
        brightness = (brightness <= 200) ? brightness + 20 : 220;
      } else {
        brightness = (brightness >= 20) ? brightness - 20 : 0;
      }
      strip.setBrightness(brightness);
      if (currentEffect == 0 && brightness > 0) currentEffect = 1;
      g_forceRedraw = true;
      strip.show();
    } else if (currentMode == MODE_MUTE) {
      if (isRight) {
        Serial1.println("V+");
      } else {
        Serial1.println("V-");
      }
    } else if (currentMode == MODE_CPG) {
      if (isRight) {
        currentEffect = (currentEffect + 1) % MAX_EFFECTS;
      } else {
        currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
      }
      effectFrame = 0;
      g_forceRedraw = true;
    } else if (currentMode == MODE_KEY_COLOR) {
      if (isRight) {
        keypressColorMode = (keypressColorMode + 1) % 8;
      } else {
        keypressColorMode = (keypressColorMode == 0) ? 7 : keypressColorMode - 1;
      }
      updateIndicators();
    }
  }
  lastClkState = currentClk;
}

// ================= 灯光特效 =================
void triggerRipple() {
  isRippleActive = true;
  rippleStep = 0;
  lastRippleUpdate = millis();

  if (keypressColorMode == 0) {
    static uint8_t autoIndex = 0;
    const uint32_t autoColors[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB400FF, 0x00FFFF, 0xFFFFFF };
    currentRippleColor = autoColors[autoIndex];
    autoIndex = (autoIndex + 1) % 6;
  } else {
    currentRippleColor = keypressColors[keypressColorMode];
  }
}

void updateRipple() {
  unsigned long now = millis();
  if (now - lastRippleUpdate > 40) {
    lastRippleUpdate = now;

    for (int i = 0; i < NUM_LEDS; i++) {
      uint32_t c = strip.getPixelColor(i);
      uint8_t r = (c >> 16) & 0xFF;
      uint8_t g = (c >> 8) & 0xFF;
      uint8_t b = c & 0xFF;
      strip.setPixelColor(i, strip.Color(r * 0.4, g * 0.4, b * 0.4));
    }

    int left = (NUM_LEDS / 2 - 1) - rippleStep;
    int right = (NUM_LEDS / 2) + rippleStep;

    if (left >= 0) strip.setPixelColor(left, currentRippleColor);
    if (right < NUM_LEDS) strip.setPixelColor(right, currentRippleColor);

    strip.show();
    rippleStep++;

    if (rippleStep > NUM_LEDS / 2) {
      isRippleActive = false;
      g_forceRedraw = true;
    }
  }
}

void handleBluetoothAlert(unsigned long now) {
  static unsigned long lastFlashUpdate = 0;
  static bool flashOn = false;

  if (now - lastFlashUpdate > 400) {
    lastFlashUpdate = now;
    flashOn = !flashOn;

    if (flashOn) {
      uint32_t col = 0;
      switch (bt_alert) {
        case 'R': col = strip.Color(255, 0, 0); break;
        case 'G': col = strip.Color(0, 255, 0); break;
        case 'B': col = strip.Color(0, 0, 255); break;
        case 'Y': col = strip.Color(255, 180, 0); break;
      }
      strip.fill(col);
    } else {
      strip.clear();
    }
    strip.show();
  }
}

void handleSystemState(unsigned long now) {
  switch (sysState) {
    case SYS_NOTIFY_ME:
      if (now - lastSysUpdate > 80) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) {
          strip.fill(strip.Color(255, 200, 0));
        } else {
          strip.fill(strip.Color(0, 255, 255));
        }
        strip.show();
        sysFrame++;
        if (sysFrame > 20) {
          exitSystemState();
        }
      }
      break;

    case SYS_REBOOT:
      if (now - lastSysUpdate > 400) {
        lastSysUpdate = now;
        if (sysFrame == 0) {
          strip.fill(strip.Color(255, 0, 0));
        } else if (sysFrame == 1) {
          strip.fill(strip.Color(0, 255, 0));
        } else if (sysFrame == 2) {
          strip.fill(strip.Color(0, 0, 255));
        } else if (sysFrame == 3) {
          strip.fill(strip.Color(255, 255, 255));
        }
        strip.show();
        sysFrame++;
        if (sysFrame > 4) {
          exitSystemState();
        }
      }
      break;

    case SYS_ROOT:
      if (now - lastSysUpdate > 300) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) {
          strip.fill(strip.Color(255, 120, 0));
        } else {
          strip.clear();
        }
        strip.show();
        sysFrame++;
      }
      break;
  }
}

void drawBreathing(uint8_t maxR, uint8_t maxG, uint8_t maxB) {
  float val = (exp(sin(effectFrame * 0.03)) - 0.36787944) * 108.0;
  float ratio = val / 255.0;
  if(ratio > 1.0) ratio = 1.0;
  if(ratio < 0.0) ratio = 0.0;
  strip.fill(strip.Color(maxR * ratio, maxG * ratio, maxB * ratio));
  strip.show();
  effectFrame++;
}

void updateLightingEffect() {
  unsigned long now = millis();

  if (g_forceOff) {
    if (g_forceRedraw) {
      strip.clear();
      strip.show();
      g_forceRedraw = false;
    }
    return;
  }

  if (bt_alert != '\0') {
    handleBluetoothAlert(now);
    return;
  }

  if (sysState != SYS_NORMAL) {
    handleSystemState(now);
    return;
  }

  if (isRippleActive) {
    updateRipple();
    return;
  }

  if (currentEffect == 0) {
    if (g_forceRedraw) {
      strip.clear();
      strip.show();
      g_forceRedraw = false;
    }
    return;
  }

  uint8_t displayEffect = currentEffect;
  static unsigned long lastAutoSwitchTime = 0;
  static uint8_t autoCycleIndex = 1;

  if (currentEffect == 13) {
    if (now - lastAutoSwitchTime > 8000) {
      lastAutoSwitchTime = now;
      autoCycleIndex++;
      if (autoCycleIndex > 12) autoCycleIndex = 1;
      effectFrame = 0;
      g_forceRedraw = true;
    }
    displayEffect = autoCycleIndex;
  }

  switch (displayEffect) {
    case 1:
      if (g_forceRedraw) { strip.fill(strip.Color(255, 0, 0)); strip.show(); g_forceRedraw = false; } break;
    case 2:
      if (g_forceRedraw) { strip.fill(strip.Color(0, 255, 0)); strip.show(); g_forceRedraw = false; } break;
    case 3:
      if (g_forceRedraw) { strip.fill(strip.Color(0, 0, 255)); strip.show(); g_forceRedraw = false; } break;
    case 4:
      if (g_forceRedraw) { strip.fill(strip.Color(0, 127, 255)); strip.show(); g_forceRedraw = false; } break;
    case 5:
      if (g_forceRedraw) { strip.fill(strip.Color(255, 255, 255)); strip.show(); g_forceRedraw = false; } break;
    case 6:
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(255, 0, 0); g_forceRedraw = false; } break;
    case 7:
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 255, 0); g_forceRedraw = false; } break;
    case 8:
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 0, 255); g_forceRedraw = false; } break;
    case 9:
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 127, 255); g_forceRedraw = false; } break;
    case 10:
      if (now - lastEffectUpdate > 50) {
        lastEffectUpdate = now;
        strip.clear();
        int totalSteps = (NUM_LEDS - 1) * 2;
        int step = effectFrame % totalSteps;
        int pos = (step < NUM_LEDS) ? step : (totalSteps - step);
        strip.setPixelColor(pos, strip.Color(255, 0, 50));
        for (int i = 0; i < NUM_LEDS; i++) {
          int diff = abs(i - pos);
          if (diff == 1) strip.setPixelColor(i, strip.Color(80, 0, 15));
          else if (diff == 2) strip.setPixelColor(i, strip.Color(20, 0, 3));
        }
        strip.show();
        effectFrame++;
        g_forceRedraw = false;
      }
      break;
    case 11:
      if (now - lastEffectUpdate > 50) {
        lastEffectUpdate = now;
        strip.clear();
        int totalSteps = (NUM_LEDS - 1) * 2;
        int step = effectFrame % totalSteps;
        int pos1 = (step < NUM_LEDS) ? step : (totalSteps - step);
        int pos2 = (step < NUM_LEDS) ? (NUM_LEDS - 1 - step) : (step - NUM_LEDS + 1);
        strip.setPixelColor(pos1, strip.Color(180, 0, 255));
        strip.setPixelColor(pos2, strip.Color(0, 180, 255));
        for (int i = 0; i < NUM_LEDS; i++) {
          if (abs(i - pos1) == 1) strip.setPixelColor(i, strip.Color(50, 0, 80));
          if (abs(i - pos2) == 1) strip.setPixelColor(i, strip.Color(0, 50, 80));
        }
        strip.show();
        effectFrame++;
        g_forceRedraw = false;
      }
      break;
    case 12:
      if (now - lastEffectUpdate > 20) {
        lastEffectUpdate = now;
        for (int i = 0; i < NUM_LEDS; i++) {
          int pixelHue = effectFrame + (i * 65536L / NUM_LEDS);
          strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
        }
        strip.show();
        effectFrame += 256;
        g_forceRedraw = false;
      }
      break;
  }
}
