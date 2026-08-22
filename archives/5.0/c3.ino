#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

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
  MODE_KEY_COLOR    // 按键特效配置模式 (旋钮控制款式与颜色)
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

// 【修改点】扩展配置范围 0~23
// 0~7: 中间散开 (Ripple)
// 8~15: 左向右发射 (Shoot)
// 16~23: 堆积木 (Stack)
uint8_t keypressStyle = 0; 
uint8_t reactionType = 0; // 0=散开, 1=发射, 2=堆积木
int stackCount = 0;       // 记录堆积木的数量

const uint32_t keypressColors[] = {
  0x000000,   // 0: 自动模式 (按键循环变色)
  0xFF0000,   // 1: 红
  0x00FF00,   // 2: 绿
  0x0000FF,   // 3: 蓝
  0x00FFFF,   // 4: 冰蓝
  0xB400FF,   // 5: 紫
  0xFFFF00,   // 6: 黄
  0xFFFFFF    // 7: 白
};

// ================= 物理按键状态变量 =================
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

unsigned long cpgLongPressStart = 0;
unsigned long muteLongPressStart = 0;
bool cpgLongPressSent = false;
bool muteLongPressSent = false;

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

  // 1. 静音按键 -> 长按3S发送 N_S3_REBOT
  bool muteReading = digitalRead(PIN_MUTE);
  if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
  }
  if ((now - lastMuteTime) > 50) {
    static bool muteTriggered = false;
    if (muteReading == LOW && !muteTriggered) {
      muteTriggered = true;
      muteLongPressStart = now;
      muteLongPressSent = false;

      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
      }
      exitSystemState();
      currentMode = MODE_MUTE;
      updateIndicators();
    } else if (muteReading == HIGH) {
      if (muteTriggered) {
        // 【修改点】长按发送普通的重启指令
        if (!muteLongPressSent && (now - muteLongPressStart >= 3000)) {
          Serial1.println("N_S3_REBOT");
          flashConfirm();
          Serial.println("已发送 N_S3_REBOT 给 S3");
        }
        muteTriggered = false;
        muteLongPressSent = false;
      }
    }
  }

  // 2. 灯光按键 
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
        isReactionActive = false;
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

  // 3. CPG 按键 -> 长按3S发送 N_S3_ROOT
  bool cpgReading = digitalRead(PIN_CPG);
  if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
  }
  if ((now - lastCpgTime) > 50) {
    static bool cpgTriggered = false;
    if (cpgReading == LOW && !cpgTriggered) {
      cpgTriggered = true;
      cpgLongPressStart = now;
      cpgLongPressSent = false;

      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
      }

      exitSystemState();
      if (currentMode != MODE_CPG) {
        currentMode = MODE_CPG;
      } else {
        currentEffect = (currentEffect + 1) % MAX_EFFECTS;
        if (currentEffect == 0) currentEffect = 1;
      }

      updateIndicators();
      effectFrame = 0;
      g_forceRedraw = true;
    } else if (cpgReading == HIGH) {
      if (cpgTriggered) {
        // 【修改点】长按发送Root刷机指令
        if (!cpgLongPressSent && (now - cpgLongPressStart >= 3000)) {
          Serial1.println("N_S3_ROOT");
          flashConfirm();
          Serial.println("已发送 N_S3_ROOT 给 S3");
        }
        cpgTriggered = false;
        cpgLongPressSent = false;
      }
    }
  }

  // 4. 旋钮按键 
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

// ================= 4. 旋转编码器处理 (修复反向误触) =================
void handleEncoder() {
  int currentClk = digitalRead(PIN_ENCODER_A);
  static unsigned long lastEncoderTime = 0;
  
  // 只有 A相 状态发生跳变时才进入判断
  if (currentClk != lastClkState) {
    unsigned long now = millis();
    
    // 【消抖核心】：如果距离上次触发不足 5 毫秒，认为是机械毛刺，直接过滤掉
    if (now - lastEncoderTime > 5) {
      lastEncoderTime = now;
      
      // 我们只在 A相 为低电平（下降沿）时执行动作
      if (currentClk == LOW) {
        
        // 稍微等待 100 微秒，让 B相 的电平完全稳定下来再读取（不影响主程序运行）
        delayMicroseconds(100);
        int bState = digitalRead(PIN_ENCODER_B);
        
        // 唤醒强关状态
        if (g_forceOff) {
          g_forceOff = false;
          currentEffect = 1;
          g_forceRedraw = true;
          Serial.println("强关解除：唤醒灯光系统");
        }
        exitSystemState(); 

        // 判断方向 (原逻辑：B状态与A相同则为右，当前A是LOW，所以B是LOW即为右旋)
        bool isRight = (bState == LOW);

        // ------ 根据当前模式执行功能 ------
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
            keypressStyle = (keypressStyle + 1) % 24;
          } else {
            keypressStyle = (keypressStyle == 0) ? 23 : keypressStyle - 1;
          }
          updateIndicators(); 
        }
      }
    }
    // 更新上一帧的 A相 状态
    lastClkState = currentClk;
  }
}

// 【修改点】触发按键灯效引擎
void triggerKeyReaction() {
  isReactionActive = true;
  reactionStep = 0;
  lastReactionUpdate = millis();
  
  // 计算当前特效大类与颜色小类
  reactionType = keypressStyle / 8;     // 0=散开, 1=发射, 2=积木
  uint8_t colorIdx = keypressStyle % 8; // 0=自动, 1-7=指定颜色

  if (colorIdx == 0) {
    static uint8_t autoIndex = 0;
    const uint32_t autoColors[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB400FF, 0x00FFFF, 0xFFFFFF };
    currentReactionColor = autoColors[autoIndex];
    autoIndex = (autoIndex + 1) % 6;
  } else {
    currentReactionColor = keypressColors[colorIdx];
  }
}

// 按键动画执行器 (修改发射与堆叠方向为从右到左)
void updateKeyReaction() {
  unsigned long now = millis();
  if (now - lastReactionUpdate > 25) { 
    lastReactionUpdate = now;

    // 1. 拖影与背景消散处理
    for (int i = 0; i < NUM_LEDS; i++) {
      // 堆积木模式下，已固定的积木不能被变暗擦除（现在积木堆积在左侧，索引 0 到 stackCount-1）
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

    // 2. 动画帧更新
    if (reactionType == 0) {
      // 模式0：中间向两边散开 (保持不变)
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
      // 模式1：像激光一样从【右边】发射到【左边】
      if (reactionStep < NUM_LEDS) {
        // 反向索引：总数减1，再减去当前步数
        strip.setPixelColor((NUM_LEDS - 1) - reactionStep, currentReactionColor);
      }
      reactionStep++;
      if (reactionStep >= NUM_LEDS) {
        isReactionActive = false;
        g_forceRedraw = true;
      }
    } 
    else if (reactionType == 2) {
      // 模式2：从【右边】发射，到【左边】堆积
      if (reactionStep < NUM_LEDS - stackCount) {
        // 反向索引
        strip.setPixelColor((NUM_LEDS - 1) - reactionStep, currentReactionColor);
      }
      reactionStep++;
      
      // 当触碰到左侧边界或已存在的积木时
      if (reactionStep >= NUM_LEDS - stackCount) {
        isReactionActive = false;
        stackCount++;
        // 满了就清空重置
        if (stackCount >= NUM_LEDS) {
          stackCount = 0; 
        }
        g_forceRedraw = true; 
      }
    }
    strip.show();
  }
}

// 蓝牙与系统通知逻辑保持不变
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

  // 3. 按键动画层
  if (isReactionActive) {
    updateKeyReaction();
    return;
  }

// 3.5 堆积木持久显示层（修改为堆叠在左侧）
  if (reactionType == 2 && stackCount > 0) {
    // 右边未堆积的部分置空（从 stackCount 到 末尾）
    for (int i = stackCount; i < NUM_LEDS; i++) strip.setPixelColor(i, 0); 
    // 左边头部堆积的部分常亮（从 0 到 stackCount-1）
    for (int i = 0; i < stackCount; i++) strip.setPixelColor(i, currentReactionColor); 
    strip.show();
    return; // 屏蔽后续基础背景
  }

  // 4. 最底层常规背景灯效
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