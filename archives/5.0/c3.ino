#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_system.h> 

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

// ================= 系统与优先权状态管理 =================
bool g_forceOff = false;      
char bt_alert = '\0';         

enum SystemState { 
  SYS_NORMAL,       
  SYS_NOTIFY_ME,    
  SYS_REBOOT,       
  SYS_ROOT          
};
SystemState sysState = SYS_NORMAL;

unsigned long lastSysUpdate = 0;
int sysFrame = 0;
bool g_forceRedraw = true;     

// ================= Level 3 运行优先级：按键反馈特效 =================
bool isReactionActive = false;
int reactionStep = 0;
unsigned long lastReactionUpdate = 0;
uint32_t currentReactionColor = 0;

// 0~7: 中间散开 (Ripple)
// 8~15: 右向左发射 (Shoot)
// 16~23: 右向左堆积木 (Stack)
uint8_t keypressStyle = 0; 
uint8_t reactionType = 0; 
int stackCount = 0;       

const uint32_t keypressColors[] = {
  0x000000,   
  0xFF0000,   
  0x00FF00,   
  0x0000FF,   
  0x00FFFF,   
  0xB400FF,   
  0xFFFF00,   
  0xFFFFFF    
};

// ================= 物理按键状态变量 =================
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
unsigned long knobPressStart = 0;
bool knobLongPressSent = false;

int lastClkState;
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
      } 
      else if (cmd == 'S') {
        bt_alert = '\0';      
        g_forceRedraw = true; 
      }
    }
  }
};

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
}

void loop() {
  handleSerial();
  handleButtons();
  handleEncoder();
  updateLightingEffect();
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
      triggerKeyReaction();
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

void flashConfirm() {
    for (int i = 0; i < 3; i++) {
        indicatorStrip.fill(indicatorStrip.Color(255, 255, 255));
        indicatorStrip.show();
        delay(100);
        indicatorStrip.clear();
        indicatorStrip.show();
        delay(100);
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
    case MODE_KEY_COLOR: {
      uint8_t colorIdx = keypressStyle % 8;
      uint32_t activeColor = (colorIdx == 0) ? indicatorStrip.Color(80, 0, 80) : keypressColors[colorIdx];
      uint8_t r = ((activeColor >> 16) & 0xFF) * 0.3;
      uint8_t g = ((activeColor >> 8) & 0xFF) * 0.3;
      uint8_t b = (activeColor & 0xFF) * 0.3;
      indicatorStrip.fill(indicatorStrip.Color(r, g, b));
      break;
    }
  }
  indicatorStrip.show();
}

void handleButtons() {
  unsigned long now = millis();

  // ------------------ 1. 静音按键 ------------------
  bool muteReading = digitalRead(PIN_MUTE);
  if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
  }
  if ((now - lastMuteTime) > 50) {
    if (muteReading == LOW) { 
      if (!muteTriggered) {
        muteTriggered = true;
        mutePressStart = now;
        muteLongPressSent = false;
      } else {
        if (!muteLongPressSent && (now - mutePressStart >= 3000)) {
          muteLongPressSent = true;
          Serial1.println("N_S3_REBOT");
          flashConfirm();
          delay(200);
          ESP.restart(); 
        }
      }
    } else { 
      if (muteTriggered) {
        muteTriggered = false;
        if (!muteLongPressSent) {
          if (g_forceOff) { g_forceOff = false; currentEffect = 1; g_forceRedraw = true; }
          exitSystemState();
          currentMode = MODE_MUTE; 
          updateIndicators();
        }
      }
    }
  }

  // ------------------ 2. 灯光按键 ------------------
  bool lightReading = digitalRead(PIN_LIGHT);
  if (lightReading != lastLightState) {
    lastLightTime = now;
    lastLightState = lightReading;
  }
  if ((now - lastLightTime) > 50) {
    if (lightReading == LOW) {
      if (!lightTriggered) {
        lightTriggered = true;
        lightPressStart = now;
        lightLongPressSent = false;
      } else {
        if (!lightLongPressSent && (now - lightPressStart >= 2000)) {
          lightLongPressSent = true;
          g_forceOff = true;       
          bt_alert = '\0';         
          sysState = SYS_NORMAL;   
          
          isReactionActive = false; // 清空残留
          stackCount = 0;           

          currentEffect = 0;       
          strip.clear();
          strip.show();
        }
      }
    } else {
      if (lightTriggered) {
        lightTriggered = false;
        if (!lightLongPressSent) {
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

  // ------------------ 3. CPG 按键 ------------------
  bool cpgReading = digitalRead(PIN_CPG);
  if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
  }
  if ((now - lastCpgTime) > 50) {
    if (cpgReading == LOW) {
      if (!cpgTriggered) {
        cpgTriggered = true;
        cpgPressStart = now;
        cpgLongPressSent = false;
      } else {
        if (!cpgLongPressSent && (now - cpgPressStart >= 3000)) {
          cpgLongPressSent = true;
          Serial1.println("N_S3_ROOT");
          flashConfirm();
        }
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
          
          // 【核心修复】：只要按了CPG键，瞬间清空打字特效层，让下层的CPG背景显露出来
          isReactionActive = false;
          stackCount = 0;
          
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

  // ------------------ 4. 旋钮按键 ------------------
  bool knobReading = digitalRead(PIN_KNOB_BTN);
  if (knobReading != lastKnobState) {
    lastKnobTime = now;
    lastKnobState = knobReading;
  }
  if ((now - lastKnobTime) > 50) {
    if (knobReading == LOW) {
      if (!knobTriggered) {
        knobTriggered = true;
        knobPressStart = now;
        knobLongPressSent = false;
      } else {
        if (!knobLongPressSent && (now - knobPressStart >= 800)) {
          knobLongPressSent = true;
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          } else {
            currentEffect = 0; 
            stackCount = 0; 
            isReactionActive = false;
            g_forceRedraw = true; 
          }
        }
      }
    } else {
      if (knobTriggered) {
        knobTriggered = false;
        if (!knobLongPressSent) {
          if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
          }
          exitSystemState();

          // 切换到按键特效设置时，清空之前状态
          stackCount = 0; 
          isReactionActive = false;

          currentMode = MODE_KEY_COLOR; 
          updateIndicators();
          
          // 【体验优化】切换过来的时候，自动发射一次当前特效给你预览，不用再去按键盘！
          triggerKeyReaction();
        }
      }
    }
  }
}

void handleEncoder() {
  int currentClk = digitalRead(PIN_ENCODER_A);
  static unsigned long lastEncoderTime = 0;
  
  if (currentClk != lastClkState) {
    unsigned long now = millis();
    if (now - lastEncoderTime > 5) {
      lastEncoderTime = now;
      if (currentClk == LOW) {
        delayMicroseconds(100);
        int bState = digitalRead(PIN_ENCODER_B);
        
        if (g_forceOff) { g_forceOff = false; currentEffect = 1; g_forceRedraw = true; }
        exitSystemState(); 
        bool isRight = (bState == LOW);

        if (currentMode == MODE_LIGHT) {
          if (isRight) brightness = (brightness <= 200) ? brightness + 20 : 220; 
          else brightness = (brightness >= 20) ? brightness - 20 : 0;
          strip.setBrightness(brightness);
          if (currentEffect == 0 && brightness > 0) currentEffect = 1; 

          // 调节亮度时清空遮挡，方便肉眼观察变化
          stackCount = 0;
          isReactionActive = false;

          g_forceRedraw = true; 
          strip.show();
          
        } else if (currentMode == MODE_MUTE) {
          if (isRight) Serial1.println("V+");
          else Serial1.println("V-");
          
        } else if (currentMode == MODE_CPG) {
          if (isRight) currentEffect = (currentEffect + 1) % MAX_EFFECTS;
          else currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
          
          // 旋钮切背景图时，也瞬间清除顶层的键盘特效，防止背景被挡着看不见
          stackCount = 0;
          isReactionActive = false;

          effectFrame = 0; 
          g_forceRedraw = true; 
          
        } else if (currentMode == MODE_KEY_COLOR) {
          if (isRight) keypressStyle = (keypressStyle + 1) % 24;
          else keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
          updateIndicators(); 
          
          // 【体验优化】你旋转挑选按键特效时，每次转动都会自动发射一次给你预览！
          stackCount = 0;
          triggerKeyReaction();
        }
      }
    }
    lastClkState = currentClk;
  }
}

void triggerKeyReaction() {
  // 【核心修复】：如果上一个“堆积木”特效还在飞行途中，又按下了新按键，
  // 我们直接让上一个积木“瞬间落地”结算，保证快速打字时积木不会丢失。
  if (isReactionActive && reactionType == 2) {
    stackCount++;
    if (stackCount >= NUM_LEDS) stackCount = 0; 
  }

  isReactionActive = true;
  reactionStep = 0;
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
      uint8_t r = (c >> 16) & 0xFF;
      uint8_t g = (c >> 8) & 0xFF;
      uint8_t b = c & 0xFF;
      strip.setPixelColor(i, strip.Color(r * 0.4, g * 0.4, b * 0.4));
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
    } 
    else if (reactionType == 1) {
      if (reactionStep < NUM_LEDS) {
        strip.setPixelColor((NUM_LEDS - 1) - reactionStep, currentReactionColor);
      }
      reactionStep++;
      if (reactionStep >= NUM_LEDS) {
        isReactionActive = false;
        g_forceRedraw = true;
      }
    } 
    else if (reactionType == 2) {
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
    strip.show();
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
        if (sysFrame % 2 == 0) strip.fill(strip.Color(255, 200, 0)); 
        else strip.fill(strip.Color(0, 255, 255)); 
        strip.show();
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
        strip.show();
        sysFrame++;
        if (sysFrame > 4) exitSystemState();
      }
      break;
    case SYS_ROOT: 
      if (now - lastSysUpdate > 300) { 
        lastSysUpdate = now;
        if (sysFrame % 2 == 0) strip.fill(strip.Color(255, 120, 0)); 
        else strip.clear(); 
        strip.show();
        sysFrame++;
      }
      break;
    default: break;
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
    if (g_forceRedraw) { strip.clear(); strip.show(); g_forceRedraw = false; }
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

  // 3.5 堆叠的永久层如果存在，就优先占据控制权
  if (reactionType == 2 && stackCount > 0) {
    for (int i = stackCount; i < NUM_LEDS; i++) strip.setPixelColor(i, 0); 
    for (int i = 0; i < stackCount; i++) strip.setPixelColor(i, currentReactionColor); 
    strip.show();
    return; 
  }

  if (currentEffect == 0) {
    if (g_forceRedraw) { strip.clear(); strip.show(); g_forceRedraw = false; }
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
    case 1: if (g_forceRedraw) { strip.fill(strip.Color(255, 0, 0)); strip.show(); g_forceRedraw = false; } break;    
    case 2: if (g_forceRedraw) { strip.fill(strip.Color(0, 255, 0)); strip.show(); g_forceRedraw = false; } break;    
    case 3: if (g_forceRedraw) { strip.fill(strip.Color(0, 0, 255)); strip.show(); g_forceRedraw = false; } break;    
    case 4: if (g_forceRedraw) { strip.fill(strip.Color(0, 127, 255)); strip.show(); g_forceRedraw = false; } break;  
    case 5: if (g_forceRedraw) { strip.fill(strip.Color(255, 255, 255)); strip.show(); g_forceRedraw = false; } break; 

    case 6: if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(255, 0, 0); g_forceRedraw = false; } break;
    case 7: if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 255, 0); g_forceRedraw = false; } break;
    case 8: if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 0, 255); g_forceRedraw = false; } break;
    case 9: if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 127, 255); g_forceRedraw = false; } break;

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
        strip.show(); effectFrame++; g_forceRedraw = false;
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
        strip.show(); effectFrame++; g_forceRedraw = false;
      }
      break;
      
    case 12: 
      if (now - lastEffectUpdate > 20) {
        lastEffectUpdate = now;
        for (int i = 0; i < NUM_LEDS; i++) {
          int pixelHue = effectFrame + (i * 65536L / NUM_LEDS);
          strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
        }
        strip.show(); effectFrame += 256; g_forceRedraw = false;
      }
      break;
  }
}
   