#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

#define WDT_TIMEOUT 3

// ================= C3 引脚定义 =================
#define PIN_CPG 1
#define PIN_MUTE 2
#define PIN_LIGHT 3
#define PIN_KNOB_BTN 0
#define PIN_ENCODER_A 5
#define PIN_ENCODER_B 6

#define PIN_WS2812 21
#define PIN_INDICATOR 9

#define RX_PIN 10
#define TX_PIN 20

#define NUM_LEDS 16
#define NUM_INDICATORS 3

Adafruit_NeoPixel strip(NUM_LEDS, PIN_WS2812, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel indicatorStrip(NUM_INDICATORS, PIN_INDICATOR, NEO_GRB + NEO_KHZ800);

// ================= 蓝牙服务配置 =================
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// ================= 全局状态与控制模式 =================
enum ControlMode { MODE_CPG,
                   MODE_MUTE,
                   MODE_LIGHT,
                   MODE_KEY_COLOR };
ControlMode currentMode = MODE_LIGHT;

uint8_t brightness = 140;
uint8_t currentEffect = 1;
const uint8_t MAX_EFFECTS = 14;

bool g_forceOff = false;
char bt_alert = '\0';

enum SystemState { SYS_NORMAL,
                   SYS_NOTIFY_ME,
                   SYS_REBOOT,
                   SYS_ROOT };
SystemState sysState = SYS_NORMAL;

unsigned long lastSysUpdate = 0;
int sysFrame = 0;
bool g_forceRedraw = true;
bool g_needsShow = false;  // 【核心防护】：标记单帧刷屏，防止频繁锁中断导致编码器丢步

// ================= 编码器中断变量 (状态机消抖版) =================
volatile bool encoderMoved = false;
volatile int encoderDirection = 0;

void IRAM_ATTR encoderISR() {
  static const int8_t KNOB_STATES[] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0
  };

  static uint8_t old_AB = 0;
  uint8_t current_AB = (digitalRead(PIN_ENCODER_A) << 1) | digitalRead(PIN_ENCODER_B);
  old_AB = ((old_AB & 0x03) << 2) | current_AB;

  int8_t result = KNOB_STATES[old_AB];

  if (result != 0) {
    static int8_t pulseCounter = 0;
    pulseCounter += result;
    
    if (pulseCounter >= 4) {
      encoderDirection = -1; // 【修改处】：原为 1，改为 -1
      encoderMoved = true;
      pulseCounter = 0;
    } else if (pulseCounter <= -4) {
      encoderDirection = 1;  // 【修改处】：原为 -1，改为 1
      encoderMoved = true;
      pulseCounter = 0;
    }
  }
}
// ================= 按键反馈特效引擎 =================
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

bool s3Connected = false;
unsigned long lastS3Heartbeat = 0;

bool lastMuteState = HIGH;
unsigned long lastMuteTime = 0;
bool muteTriggered = false;
unsigned long mutePressStart = 0;
bool muteLongPressSent = false;
bool lastLightState = HIGH;
unsigned long lastLightTime = 0;
bool lightTriggered = false;
unsigned long lightPressStart = 0;
bool lightLongPressSent = false;
bool lastCpgState = HIGH;
unsigned long lastCpgTime = 0;
bool cpgTriggered = false;
unsigned long cpgPressStart = 0;
bool cpgLongPressSent = false;
bool lastKnobState = HIGH;
unsigned long lastKnobTime = 0;
bool knobTriggered = false;

unsigned long lastEffectUpdate = 0;
uint16_t effectFrame = 0;
String inputBuffer = "";

class MyBLECallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    if (rxValue.length() > 0) {
      char cmd = rxValue[0];
      if (cmd == 'R' || cmd == 'B' || cmd == 'G' || cmd == 'Y') {
        bt_alert = cmd;
        g_forceRedraw = true;
      } else if (cmd == 'S') {
        bt_alert = '\0';
        g_forceRedraw = true;
      }
    }
  }
};

void clearReactionState() {
  isReactionActive = false;
  stackCount = 0;
  reactionStep = 0;
  for (int i = 0; i < MAX_SHOOT_PROJECTILES; i++) shootSteps[i] = -1;
}

void setTxState(bool enable) {
  if (enable) {
    pinMode(TX_PIN, OUTPUT);
    esp_rom_gpio_connect_out_signal(TX_PIN, U1TXD_OUT_IDX, false, false);
  } else {
    pinMode(TX_PIN, INPUT);
  }
}

void initC3Watchdog() {
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
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  setTxState(false);

  pinMode(PIN_CPG, INPUT_PULLUP);
  pinMode(PIN_MUTE, INPUT_PULLUP);
  pinMode(PIN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_KNOB_BTN, INPUT_PULLUP);

  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);

  // 关键修改：A 和 B 都要绑定 CHANGE 中断
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B), encoderISR, CHANGE);

  delay(100);

  strip.begin();
  strip.setBrightness(brightness);
  strip.show();

  pinMode(PIN_INDICATOR, OUTPUT);
  indicatorStrip.begin();
  indicatorStrip.setBrightness(80);
  updateIndicators();

  BLEDevice::init("C3_Keyboard_BLE");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyBLECallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  initC3Watchdog();
}

void loop() {
  esp_task_wdt_reset();

  handleSerial();
  handleButtons();
  handleEncoder();
  updateLightingEffect();

  if (g_needsShow) {
    strip.show();
    g_needsShow = false;
  }

  if (s3Connected && (millis() - lastS3Heartbeat > 5000)) {
    s3Connected = false;
    setTxState(false);
  }
}

void handleSerial() {
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      inputBuffer.trim();
      processSerialCommand(inputBuffer);
      inputBuffer = "";
    } else if (c != '\r') {
      inputBuffer += c;
      if (inputBuffer.length() > 64) inputBuffer = "";
    }
  }
}

void processSerialCommand(String cmd) {
  if (cmd == "S3_PING") {
    lastS3Heartbeat = millis();
    if (!s3Connected) {
      s3Connected = true;
      setTxState(true);
    }
    Serial1.println("C3_PONG");
  } else if (cmd == "N_ME") {
    sysState = SYS_NOTIFY_ME;
    sysFrame = 0;
    lastSysUpdate = millis();
  } else if (cmd == "N_REBOT") {
    sysState = SYS_REBOOT;
    sysFrame = 0;
    lastSysUpdate = millis();
  } else if (cmd == "N_ROOT") {
    sysState = SYS_ROOT;
    sysFrame = 0;
    lastSysUpdate = millis();
  } else if (cmd == "N_KEY_PRESS") {
    if (sysState == SYS_NORMAL) triggerKeyReaction();
  }
}

void exitSystemState() {
  if (sysState != SYS_NORMAL) {
    sysState = SYS_NORMAL;
    strip.clear();
    g_needsShow = true;
    g_forceRedraw = true;
  }
}

void flashConfirm() {
  for (int i = 0; i < 3; i++) {
    indicatorStrip.fill(indicatorStrip.Color(255, 255, 255));
    indicatorStrip.show();
    delay(80);
    indicatorStrip.clear();
    indicatorStrip.show();
    delay(80);
  }
}

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
      {
        uint8_t colorIdx = keypressStyle % 8;
        uint32_t activeColor = (colorIdx == 0) ? indicatorStrip.Color(80, 0, 80) : keypressColors[colorIdx];
        uint8_t r = ((activeColor >> 16) & 0xFF) * 30 / 100;
        uint8_t g = ((activeColor >> 8) & 0xFF) * 30 / 100;
        uint8_t b = (activeColor & 0xFF) * 30 / 100;
        indicatorStrip.fill(indicatorStrip.Color(r, g, b));
        break;
      }
  }
  indicatorStrip.show();
}

void handleButtons() {
  unsigned long now = millis();

  // ================= 1. 静音/重启按键 =================
  bool muteReading = digitalRead(PIN_MUTE);
  if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
  }
  if ((now - lastMuteTime) > 40) {
    if (muteReading == LOW) {
      if (!muteTriggered) {
        muteTriggered = true;
        mutePressStart = now;
        muteLongPressSent = false;
      } else if (!muteLongPressSent && (now - mutePressStart >= 3000)) {
        muteLongPressSent = true;
        if (s3Connected) Serial1.println("N_S3_REBOT");
        flashConfirm();
        ESP.restart();
      }
    } else {
      if (muteTriggered) {
        muteTriggered = false;
        if (!muteLongPressSent) {
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          }
          exitSystemState();
          clearReactionState();
          currentMode = MODE_MUTE;
          updateIndicators();
        }
      }
    }
  }

  // ================= 2. 灯光按键（长按2S关闭所有灯光） =================
  bool lightReading = digitalRead(PIN_LIGHT);
  if (lightReading != lastLightState) {
    lastLightTime = now;
    lastLightState = lightReading;
  }
  if ((now - lastLightTime) > 40) {
    if (lightReading == LOW) {
      if (!lightTriggered) {
        lightTriggered = true;
        lightPressStart = now;
        lightLongPressSent = false;
      } else if (!lightLongPressSent && (now - lightPressStart >= 2000)) {  // 长按2秒全关灯
        lightLongPressSent = true;
        g_forceOff = true;
        bt_alert = '\0';
        sysState = SYS_NORMAL;
        clearReactionState();
        currentEffect = 0;
        strip.clear();
        indicatorStrip.clear();
        indicatorStrip.show();
        g_needsShow = true;
      }
    } else {
      if (lightTriggered) {
        lightTriggered = false;
        if (!lightLongPressSent) {  // 短按：切换到灯光模式/唤醒
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          } else {
            exitSystemState();
            clearReactionState();
            currentMode = MODE_LIGHT;
            updateIndicators();
          }
        }
      }
    }
  }

  // ================= 3. CPG按键 =================
  bool cpgReading = digitalRead(PIN_CPG);
  if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
  }
  if ((now - lastCpgTime) > 40) {
    if (cpgReading == LOW) {
      if (!cpgTriggered) {
        cpgTriggered = true;
        cpgPressStart = now;
        cpgLongPressSent = false;
      } else if (!cpgLongPressSent && (now - cpgPressStart >= 3000)) {
        cpgLongPressSent = true;
        if (s3Connected) Serial1.println("N_S3_ROOT");
        flashConfirm();
      }
    } else {
      if (cpgTriggered) {
        cpgTriggered = false;
        if (!cpgLongPressSent) {
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          }
          exitSystemState();
          clearReactionState();
          if (currentMode != MODE_CPG) {
            currentMode = MODE_CPG;
            if (currentEffect == 0) currentEffect = 1;
          } else {
            currentEffect = (currentEffect + 1) % MAX_EFFECTS;
            if (currentEffect == 0) currentEffect = 1;
          }
          updateIndicators();
          effectFrame = 0;
          g_forceRedraw = true;
        }
      }
    }
  }

  // ================= 4. 旋转编码器按钮（按下进入键盘灯效调整） =================
  bool knobReading = digitalRead(PIN_KNOB_BTN);
  if (knobReading != lastKnobState) {
    lastKnobTime = now;
    lastKnobState = knobReading;
  }
  if ((now - lastKnobTime) > 40) {
    if (knobReading == LOW) {
      if (!knobTriggered) { knobTriggered = true; }
    } else {
      if (knobTriggered) {
        knobTriggered = false;
        if (g_forceOff) {
          g_forceOff = false;
          currentEffect = 1;
          g_forceRedraw = true;
        }
        exitSystemState();
        clearReactionState();
        currentMode = MODE_KEY_COLOR;  // 切换到按键彩灯调整模式
        updateIndicators();
        triggerKeyReaction();  // 触发一次按键反馈灯效
      }
    }
  }
}

void handleEncoder() {
  if (!encoderMoved) return;

  bool isRight = (encoderDirection == 1);
  encoderMoved = false;

  if (g_forceOff) {
    g_forceOff = false;
    currentEffect = 1;
    g_forceRedraw = true;
  }
  exitSystemState();

  if (currentMode == MODE_LIGHT) {
    if (isRight) brightness = (brightness <= 200) ? brightness + 20 : 220;
    else brightness = (brightness >= 20) ? brightness - 20 : 0;
    strip.setBrightness(brightness);
    if (currentEffect == 0 && brightness > 0) currentEffect = 1;
    clearReactionState();
    g_forceRedraw = true;
    g_needsShow = true;

  } else if (currentMode == MODE_MUTE) {
    if (s3Connected) {
      if (isRight) Serial1.println("V+");
      else Serial1.println("V-");
    }
  } else if (currentMode == MODE_CPG) {
    if (isRight) currentEffect = (currentEffect + 1) % MAX_EFFECTS;
    else currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
    clearReactionState();
    effectFrame = 0;
    g_forceRedraw = true;

  } else if (currentMode == MODE_KEY_COLOR) {
    if (isRight) keypressStyle = (keypressStyle + 1) % 24;
    else keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
    updateIndicators();
    clearReactionState();
    triggerKeyReaction();
  }
}

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
      if (stackCount >= NUM_LEDS) stackCount = 0;
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

    for (int i = 0; i < NUM_LEDS; i++) {
      if (reactionType == 2 && i < stackCount) {
        strip.setPixelColor(i, currentReactionColor);
        continue;
      }
      uint32_t c = strip.getPixelColor(i);
      uint8_t r = ((c >> 16) & 0xFF) * 102 >> 8;
      uint8_t g = ((c >> 8) & 0xFF) * 102 >> 8;
      uint8_t b = (c & 0xFF) * 102 >> 8;
      strip.setPixelColor(i, strip.Color(r, g, b));
    }

    if (reactionType == 0) {
      int left = (NUM_LEDS / 2 - 1) - reactionStep;
      int right = (NUM_LEDS / 2) + reactionStep;
      if (left >= 0) strip.setPixelColor(left, currentReactionColor);
      if (right < NUM_LEDS) strip.setPixelColor(right, currentReactionColor);

      reactionStep++;
      if (reactionStep > NUM_LEDS / 2) {
        isReactionActive = false;
        g_forceRedraw = true;
      }
    } else if (reactionType == 1) {
      bool anyActive = false;
      for (int i = 0; i < MAX_SHOOT_PROJECTILES; i++) {
        if (shootSteps[i] >= 0) {
          int pos = (NUM_LEDS - 1) - shootSteps[i];
          if (pos >= 0 && pos < NUM_LEDS) strip.setPixelColor(pos, shootColors[i]);
          shootSteps[i]++;
          if (shootSteps[i] >= NUM_LEDS) shootSteps[i] = -1;
          else anyActive = true;
        }
      }
      if (!anyActive) {
        isReactionActive = false;
        g_forceRedraw = true;
      }
    } else if (reactionType == 2) {
      if (reactionStep < NUM_LEDS - stackCount) {
        strip.setPixelColor((NUM_LEDS - 1) - reactionStep, currentReactionColor);
      }
      reactionStep++;
      if (reactionStep >= NUM_LEDS - stackCount) {
        isReactionActive = false;
        stackCount++;
        if (stackCount >= NUM_LEDS) stackCount = 0;
        g_forceRedraw = true;
      }
    }
    g_needsShow = true;
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
    g_needsShow = true;
  }
}

void handleSystemState(unsigned long now) {
  switch (sysState) {
    case SYS_NOTIFY_ME:
      if (now - lastSysUpdate > 80) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) strip.fill(strip.Color(255, 200, 0));
        else strip.fill(strip.Color(0, 255, 255));
        g_needsShow = true;
        sysFrame++;
        if (sysFrame > 20) exitSystemState();
      }
      break;
    case SYS_REBOOT:
      if (now - lastSysUpdate > 400) {
        lastSysUpdate = now;
        if (sysFrame == 0) strip.fill(strip.Color(255, 0, 0));
        else if (sysFrame == 1) strip.fill(strip.Color(0, 255, 0));
        else if (sysFrame == 2) strip.fill(strip.Color(0, 0, 255));
        else if (sysFrame == 3) strip.fill(strip.Color(255, 255, 255));
        g_needsShow = true;
        sysFrame++;
        if (sysFrame > 4) exitSystemState();
      }
      break;
    case SYS_ROOT:
      if (now - lastSysUpdate > 300) {
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) strip.fill(strip.Color(255, 120, 0));
        else strip.clear();
        g_needsShow = true;
        sysFrame++;
      }
      break;
    default: break;
  }
}

void drawBreathing(uint8_t maxR, uint8_t maxG, uint8_t maxB) {
  float val = (exp(sin(effectFrame * 0.03)) - 0.36787944) * 108.0;
  float ratio = val / 255.0;
  if (ratio > 1.0) ratio = 1.0;
  if (ratio < 0.0) ratio = 0.0;
  strip.fill(strip.Color(maxR * ratio, maxG * ratio, maxB * ratio));
  g_needsShow = true;
  effectFrame++;
}

void updateLightingEffect() {
  unsigned long now = millis();

  if (g_forceOff) {
    if (g_forceRedraw) {
      strip.clear();
      g_needsShow = true;
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
  if (isReactionActive) {
    updateKeyReaction();
    return;
  }

  if (reactionType == 2 && stackCount > 0) {
    for (int i = stackCount; i < NUM_LEDS; i++) strip.setPixelColor(i, 0);
    for (int i = 0; i < stackCount; i++) strip.setPixelColor(i, currentReactionColor);
    g_needsShow = true;
    return;
  }

  if (currentEffect == 0) {
    if (g_forceRedraw) {
      strip.clear();
      g_needsShow = true;
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
      if (g_forceRedraw) {
        strip.fill(strip.Color(255, 0, 0));
        g_needsShow = true;
        g_forceRedraw = false;
      }
      break;
    case 2:
      if (g_forceRedraw) {
        strip.fill(strip.Color(0, 255, 0));
        g_needsShow = true;
        g_forceRedraw = false;
      }
      break;
    case 3:
      if (g_forceRedraw) {
        strip.fill(strip.Color(0, 0, 255));
        g_needsShow = true;
        g_forceRedraw = false;
      }
      break;
    case 4:
      if (g_forceRedraw) {
        strip.fill(strip.Color(0, 127, 255));
        g_needsShow = true;
        g_forceRedraw = false;
      }
      break;
    case 5:
      if (g_forceRedraw) {
        strip.fill(strip.Color(255, 255, 255));
        g_needsShow = true;
        g_forceRedraw = false;
      }
      break;

    case 6:
      if (now - lastEffectUpdate > 15) {
        lastEffectUpdate = now;
        drawBreathing(255, 0, 0);
        g_forceRedraw = false;
      }
      break;
    case 7:
      if (now - lastEffectUpdate > 15) {
        lastEffectUpdate = now;
        drawBreathing(0, 255, 0);
        g_forceRedraw = false;
      }
      break;
    case 8:
      if (now - lastEffectUpdate > 15) {
        lastEffectUpdate = now;
        drawBreathing(0, 0, 255);
        g_forceRedraw = false;
      }
      break;
    case 9:
      if (now - lastEffectUpdate > 15) {
        lastEffectUpdate = now;
        drawBreathing(0, 127, 255);
        g_forceRedraw = false;
      }
      break;

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
        g_needsShow = true;
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
        g_needsShow = true;
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
        g_needsShow = true;
        effectFrame += 256;
        g_forceRedraw = false;
      }
      break;
  }
}