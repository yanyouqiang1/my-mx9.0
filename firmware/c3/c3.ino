#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ================= C3 引脚定义 =================
#define PIN_CPG         1   // CPG按键 (切换到CPG模式/切换灯效)
#define PIN_MUTE        2   // 静音按键 (切换到音量控制模式)
#define PIN_LIGHT       3   // 灯光按键 (短按切换到亮度控制模式 / 长按2S强制关闭所有灯光)
#define PIN_KNOB_BTN    0   // 旋转按钮按键 (短按切换到按键色彩配置模式 / 长按一键关灯)
#define PIN_ENCODER_A   5   // 左旋 (CLK)
#define PIN_ENCODER_B   6   // 右旋 (DT)

#define PIN_WS2812      21  // 主灯条数据引脚
#define PIN_INDICATOR   9   // 底部指示灯引脚 

#define RX_PIN          10  // 串口 RX 接 S3 的 9
#define TX_PIN          20  // 串口 TX 接 S3 的 10

#define NUM_LEDS        16  // 主灯珠数量
#define NUM_INDICATORS  3   // 指示灯数量 (CPG、静音、灯光)

Adafruit_NeoPixel strip(NUM_LEDS, PIN_WS2812, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel indicatorStrip(NUM_INDICATORS, PIN_INDICATOR, NEO_GRB + NEO_KHZ800);

// ================= 蓝牙服务配置 =================
// 采用 Nordic UART 服务标准，便于市面上的蓝牙串口助手免配置直接连接发送指令
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" 
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// ================= 全局状态与控制模式 =================
enum ControlMode { 
  MODE_CPG,         // CPG 灯效控制模式 (旋钮切背景效果)
  MODE_MUTE,        // 音量控制模式 (旋钮发送 V+/V-)
  MODE_LIGHT,       // 亮度控制模式 (旋钮控制全局亮度)
  MODE_KEY_COLOR    // 按键扩散色彩配置模式 (旋钮控制 N_KEY_PRESS 扩散的颜色)
};
ControlMode currentMode = MODE_LIGHT; // 默认控制灯光亮度

uint8_t brightness = 140;    
uint8_t currentEffect = 1;     // 当前灯效 
const uint8_t MAX_EFFECTS = 14; // 扩展到14种模式(0-13)

/* CPG灯效对应表：
 * 0: 关灯
 * 1-5: 常亮 (红、绿、蓝、冰蓝、白)
 * 6-9: 呼吸 (红、绿、蓝、冰蓝)
 * 10: 经典单色左右来回扫描跑马灯 (Knight Rider)
 * 11: 双色交叉左右来回扫射跑马灯 
 * 12: 彩虹
 * 13: 自动循环模式 (1-12轮流播放)
 */

// ================= 系统与优先权状态管理 =================
// 【Level 0 物理优先级】
bool g_forceOff = false;      // 长按灯光键2秒触发的物理强关状态

// 【Level 1 运行优先级：蓝牙提示灯效】
char bt_alert = '\0';         // '\0'代表无蓝牙通知，'R'红闪，'B'蓝闪，'G'绿闪，'Y'黄闪

// 【Level 2 运行优先级：系统键盘串口通知】
enum SystemState { 
  SYS_NORMAL,       // 正常运行模式
  SYS_NOTIFY_ME,    // N_ME 收到记忆更新 (爆闪)
  SYS_REBOOT,       // N_REBOT 键盘重启 (红绿蓝白过渡)
  SYS_ROOT          // N_ROOT 刷机模式 (黄灯持续闪烁)
};
SystemState sysState = SYS_NORMAL;

unsigned long lastSysUpdate = 0;
int sysFrame = 0;
bool g_forceRedraw = true;     // 全局重绘标志，防止静态灯效刷屏抢占信号

// ================= Level 3 运行优先级：按键扩散特效 =================
bool isRippleActive = false;
int rippleStep = 0;
unsigned long lastRippleUpdate = 0;
uint32_t currentRippleColor = 0;

// 按键扩散特效的可选颜色模式 (0: 自动循环切换，1-7: 固定单色)
uint8_t keypressColorMode = 0; 
const char* keypressColorNames[] = { "自动彩环", "热血红", "极光绿", "深海蓝", "冰晶蓝", "幻境紫", "柠檬黄", "纯净白" };
const uint32_t keypressColors[] = {
  0x000000,   // 占位符 (0号为自动模式)
  0xFF0000,   // 1: 红
  0x00FF00,   // 2: 绿
  0x0000FF,   // 3: 蓝
  0x00FFFF,   // 4: 冰蓝
  0xB400FF,   // 5: 紫
  0xFFFF00,   // 6: 黄
  0xFFFFFF    // 7: 白
};

// ================= 物理按键异步防抖/测时变量 =================
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

// 编码器状态
int lastClkState;
unsigned long lastEffectUpdate = 0;
uint16_t effectFrame = 0;

// 串口缓存
String inputBuffer = "";

// ================= 蓝牙写入事件回调类 =================
class MyBLECallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str(); // 兼容 core v2 / v3 版本的类型差异
    if (rxValue.length() > 0) {
      char cmd = rxValue[0];
      Serial.printf("蓝牙收到指令: %c\n", cmd);
      
      if (cmd == 'R' || cmd == 'B' || cmd == 'G' || cmd == 'Y') {
        bt_alert = cmd;
        g_forceRedraw = true; // 强制灯效引擎即时重绘，接管控制
        Serial.printf("触发蓝牙提示闪烁: %c\n", cmd);
      } 
      else if (cmd == 'S') {
        bt_alert = '\0';      // 蓝牙提示失效
        g_forceRedraw = true; // 重新触发背景或键盘动画绘制
        Serial.println("蓝牙提示关闭，恢复常规键盘与CPG控制");
      }
    }
  }
};

void setup() {
  Serial.begin(115200);                                 // 调试用
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);    // 与 S3 通信的串口

  pinMode(PIN_CPG, INPUT_PULLUP);
  pinMode(PIN_MUTE, INPUT_PULLUP);
  pinMode(PIN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_KNOB_BTN, INPUT_PULLUP);
  
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  lastClkState = digitalRead(PIN_ENCODER_A);

  delay(100); // 稍微延时，等待板载电源电压稳定

  // 初始化主灯带
  strip.begin();
  strip.setBrightness(brightness);
  strip.show(); 

  // 初始化底部指示灯带
  pinMode(PIN_INDICATOR, OUTPUT);    // 明确设置指示灯引脚为输出模式
  indicatorStrip.begin();
  indicatorStrip.setBrightness(80);  // 保持指示灯适度亮度
  updateIndicators();                // 更新初始指示灯状态

  // 初始化蓝牙 (BLE)
  BLEDevice::init("C3_Keyboard_BLE"); // 广播名称
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  pRxCharacteristic->setCallbacks(new MyBLECallbacks());
  pService->start();
  
  // 启动蓝牙广播
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  Serial.println("蓝牙模块启动完毕，等待主控端连接...");
}

void loop() {
  handleSerial();
  handleButtons();
  handleEncoder();
  updateLightingEffect();
}

// ================= 1. 串口通信处理 (完全非阻塞) =================
void handleSerial() {
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n') {
      inputBuffer.trim();
      processSerialCommand(inputBuffer);
      inputBuffer = ""; // 清空缓冲区准备下一次接收
    } else if (c != '\r') {
      inputBuffer += c;
      if (inputBuffer.length() > 64) { // 防止溢出
        inputBuffer = "";
      }
    }
  }
}

// 解析和响应来自 S3 的指令
void processSerialCommand(String cmd) {
  if (cmd == "N_ME") {
    sysState = SYS_NOTIFY_ME;
    sysFrame = 0;
    lastSysUpdate = millis();
    Serial.println("收到ME记忆更新");
  } 
  else if (cmd == "N_REBOT") {
    sysState = SYS_REBOOT;
    sysFrame = 0;
    lastSysUpdate = millis();
    Serial.println("收到键盘重启信号");
  } 
  else if (cmd == "N_ROOT") {
    sysState = SYS_ROOT;
    sysFrame = 0;
    lastSysUpdate = millis();
    Serial.println("键盘进入刷机模式");
  } 
  else if (cmd == "N_KEY_PRESS") {
    if (sysState == SYS_NORMAL) {
      triggerRipple();
      currentMode = MODE_KEY_COLOR; // 收到按键后，自动切换为“按键色彩配置模式”
      updateIndicators();
    }
  }
}

// 安全退出系统特殊显示模式，恢复原常规灯效
void exitSystemState() {
  if (sysState != SYS_NORMAL) {
    sysState = SYS_NORMAL;
    strip.clear();
    strip.show();
    g_forceRedraw = true; // 强制重新绘制常规灯效
  }
}

// ================= 2. 底部指示灯更新 =================
void updateIndicators() {
  indicatorStrip.clear();
  switch (currentMode) {
    case MODE_CPG:
      indicatorStrip.setPixelColor(0, indicatorStrip.Color(120, 0, 120)); // 1号灯 紫色 (CPG模式)
      break;
    case MODE_MUTE:
      indicatorStrip.setPixelColor(1, indicatorStrip.Color(120, 0, 0));   // 2号灯 红色 (静音模式)
      break;
    case MODE_LIGHT:
      indicatorStrip.setPixelColor(2, indicatorStrip.Color(0, 120, 0));   // 3号灯 绿色 (灯光模式)
      break;
    case MODE_KEY_COLOR:
      // 3 颗指示灯一起亮起当前选择的扩散灯光颜色（调暗以防刺眼）
      uint32_t activeColor = (keypressColorMode == 0) ? indicatorStrip.Color(80, 0, 80) : keypressColors[keypressColorMode];
      uint8_t r = ((activeColor >> 16) & 0xFF) * 0.3;
      uint8_t g = ((activeColor >> 8) & 0xFF) * 0.3;
      uint8_t b = (activeColor & 0xFF) * 0.3;
      indicatorStrip.fill(indicatorStrip.Color(r, g, b));
      break;
  }
  indicatorStrip.show();
}

// ================= 3. 物理按键处理 (异步防抖/测时) =================
void handleButtons() {
  unsigned long now = millis();

  // 1. 静音按键 -> 切换音量控制模式
  bool muteReading = digitalRead(PIN_MUTE);
  if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
  }
  if ((now - lastMuteTime) > 50) {
    static bool muteTriggered = false;
    if (muteReading == LOW && !muteTriggered) {
      muteTriggered = true;
      
      // 【物理唤醒】：如果在强关状态，任何按键点击都将其唤醒并切入该模式
      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
        Serial.println("强关解除：唤醒灯光系统");
      }
      
      exitSystemState();
      currentMode = MODE_MUTE;
      updateIndicators();
      Serial.println("模式切换: 控制音量");
    } else if (muteReading == HIGH) {
      muteTriggered = false;
    }
  }

  // 2. 灯光按键 -> 短按切换亮度模式 / 长按 2 秒强制强关所有灯效
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
      
      // 检测是否长按满 2000 毫秒（2秒）
      if (!longPressTriggered && (now - lastLightTime >= 2000)) {
        longPressTriggered = true;
        
        // ------ 【最高物理优先级控制】 ------
        g_forceOff = true;       // 强制物理停用
        bt_alert = '\0';         // 强制将正在运行的蓝牙警示失效
        sysState = SYS_NORMAL;   // 清除系统警报
        isRippleActive = false;  // 关闭扩散效果
        currentEffect = 0;       // 常规灯效设为关灯
        strip.clear();
        strip.show();
        Serial.println("【最高物理优先级】：长按灯光键2秒，强制物理关闭所有灯光！");
      }
    } else {
      if (lightTriggered) {
        lightTriggered = false;
        // 如果释放时未触发长按，则判定为短按
        if (!longPressTriggered) {
          if (g_forceOff) {
            // 【物理唤醒】
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
            Serial.println("强关解除：唤醒灯光系统");
          } else {
            exitSystemState();
            currentMode = MODE_LIGHT;
            updateIndicators();
            Serial.println("模式切换: 控制亮度");
          }
        }
      }
    }
  }

  // 3. CPG 按键 -> 强制切回 CPG 模式并控制背景灯效
  bool cpgReading = digitalRead(PIN_CPG);
  if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
  }
  if ((now - lastCpgTime) > 50) {
    static bool cpgTriggered = false;
    if (cpgReading == LOW && !cpgTriggered) {
      cpgTriggered = true;
      
      // 【物理唤醒】
      if (g_forceOff) {
        g_forceOff = false;
        currentEffect = 1;
        g_forceRedraw = true;
        Serial.println("强关解除：唤醒灯光系统");
      }
      
      exitSystemState();
      if (currentMode != MODE_CPG) {
        currentMode = MODE_CPG;
        Serial.println("模式切换: CPG模式");
      } else {
        currentEffect = (currentEffect + 1) % MAX_EFFECTS;
        if (currentEffect == 0) currentEffect = 1; 
        Serial.printf("CPG模式内切换灯效: %d\n", currentEffect);
      }
      
      updateIndicators();
      effectFrame = 0;
      g_forceRedraw = true; 
    } else if (cpgReading == HIGH) {
      cpgTriggered = false;
    }
  }

  // 4. 旋钮按键 -> 短按快速配置扩散色彩，长按一键关灯
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
        
        // 【物理唤醒】
        if (g_forceOff) {
          g_forceOff = false;
          currentEffect = 1;
          g_forceRedraw = true;
          Serial.println("强关解除：唤醒灯光系统");
        }
        
        exitSystemState();
        if (pressDuration < 800) {
          // ------ 短按：切换到按键色彩模式 ------
          currentMode = MODE_KEY_COLOR;
          updateIndicators();
          Serial.println("模式切换: 按键扩散色彩配置模式");
        } else {
          // ------ 长按：一键关灯 ------
          currentEffect = 0; 
          g_forceRedraw = true; 
          Serial.println("一键关灯 (长按触发)");
        }
      }
    }
  }
}

// ================= 4. 旋转编码器处理 =================
void handleEncoder() {
  int currentClk = digitalRead(PIN_ENCODER_A);
  if (currentClk != lastClkState && currentClk == LOW) {
    
    // 如果处于物理强关状态，旋钮操作也进行唤醒
    if (g_forceOff) {
      g_forceOff = false;
      currentEffect = 1;
      g_forceRedraw = true;
      Serial.println("强关解除：唤醒灯光系统");
    }

    exitSystemState(); 

    // 旋转方向逻辑 (== 代表左旋减小，右旋增加)
    bool isRight = (digitalRead(PIN_ENCODER_B) == currentClk);

    if (currentMode == MODE_LIGHT) {
      // ------ 控光模式：调节亮度 ------
      if (isRight) {
        brightness = (brightness <= 200) ? brightness + 20 : 220; 
      } else {
        brightness = (brightness >= 20) ? brightness - 20 : 0;
      }
      strip.setBrightness(brightness);
      if (currentEffect == 0 && brightness > 0) currentEffect = 1; 
      g_forceRedraw = true; 
      strip.show();
      Serial.printf("调整亮度: %d\n", brightness);
      
    } else if (currentMode == MODE_MUTE) {
      // ------ 音量模式：发送指令给S3 ------
      if (isRight) {
        Serial1.println("V+");
      } else {
        Serial1.println("V-");
      }
      
    } else if (currentMode == MODE_CPG) {
      // ------ CPG模式：旋钮切换背景灯效 ------
      if (isRight) {
        currentEffect = (currentEffect + 1) % MAX_EFFECTS;
      } else {
        currentEffect = (currentEffect == 0) ? MAX_EFFECTS - 1 : currentEffect - 1;
      }
      effectFrame = 0; 
      g_forceRedraw = true; 
      Serial.printf("旋钮切换背景灯效: %d\n", currentEffect);
      
    } else if (currentMode == MODE_KEY_COLOR) {
      // ------ 按键色彩模式：旋钮调整按键扩散的灯光颜色 ------
      if (isRight) {
        keypressColorMode = (keypressColorMode + 1) % 8;
      } else {
        keypressColorMode = (keypressColorMode == 0) ? 7 : keypressColorMode - 1;
      }
      updateIndicators(); 
      Serial.printf("旋钮修改按键扩散颜色: %s\n", keypressColorNames[keypressColorMode]);
    }
  }
  lastClkState = currentClk;
}

// ================= 5. 灯光特效实现 =================

// 开启按键扩散特效
void triggerRipple() {
  isRippleActive = true;
  rippleStep = 0;
  lastRippleUpdate = millis();
  
  // 确定本次发散的色彩
  if (keypressColorMode == 0) {
    static uint8_t autoIndex = 0;
    const uint32_t autoColors[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB400FF, 0x00FFFF, 0xFFFFFF };
    currentRippleColor = autoColors[autoIndex];
    autoIndex = (autoIndex + 1) % 6;
  } else {
    currentRippleColor = keypressColors[keypressColorMode];
  }
}

// 按键散开特效
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

// 蓝牙通知状态渲染 (双态闪烁)
void handleBluetoothAlert(unsigned long now) {
  static unsigned long lastFlashUpdate = 0;
  static bool flashOn = false;
  
  if (now - lastFlashUpdate > 400) { // 400ms 的闪烁周期
    lastFlashUpdate = now;
    flashOn = !flashOn;
    
    if (flashOn) {
      uint32_t col = 0;
      switch (bt_alert) {
        case 'R': col = strip.Color(255, 0, 0); break;     // 红灯闪烁
        case 'G': col = strip.Color(0, 255, 0); break;     // 绿灯闪烁
        case 'B': col = strip.Color(0, 0, 255); break;     // 蓝灯闪烁
        case 'Y': col = strip.Color(255, 180, 0); break;   // 黄灯闪烁
        default: break;
      }
      strip.fill(col);
    } else {
      strip.clear();
    }
    strip.show();
  }
}

// 系统紧急和串口通知状态渲染
void handleSystemState(unsigned long now) {
  switch (sysState) {
    case SYS_NOTIFY_ME: // N_ME 记忆爆闪
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

    case SYS_REBOOT: // N_REBOT 键盘重启倒计时 (红、绿、蓝、白)
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

    case SYS_ROOT: // N_ROOT 刷机模式
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
      
    default:
      break;
  }
}

// 辅助函数：处理呼吸算法
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

  // 0. 最高物理强关状态 (物理长按灯光键 2S 触发)
  if (g_forceOff) {
    if (g_forceRedraw) {
      strip.clear();
      strip.show();
      g_forceRedraw = false;
    }
    return;
  }

  // 1. 第一运行优先级：蓝牙提示闪烁状态 (任何常规和串口灯效都无法覆盖它)
  if (bt_alert != '\0') {
    handleBluetoothAlert(now);
    return;
  }

  // 2. 第二运行优先级：系统串口提醒动画 (N_ME, N_REBOT, N_ROOT)
  if (sysState != SYS_NORMAL) {
    handleSystemState(now);
    return;
  }

  // 3. 第三运行优先级：按键散开反馈动画 (N_KEY_PRESS)
  if (isRippleActive) {
    updateRipple();
    return;
  }

  // 4. 第四运行优先级：最底层常规背景灯效
  if (currentEffect == 0) {
    if (g_forceRedraw) {
      strip.clear();
      strip.show();
      g_forceRedraw = false;
    }
    return; 
  }

  // 自动循环逻辑：处于模式 13 时，自动轮流播放 1-12 号模式
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

  // 执行对应的常规灯效渲染
  switch (displayEffect) {
    case 1: 
      if (g_forceRedraw) { strip.fill(strip.Color(255, 0, 0)); strip.show(); g_forceRedraw = false; } break;     // 常亮红
    case 2: 
      if (g_forceRedraw) { strip.fill(strip.Color(0, 255, 0)); strip.show(); g_forceRedraw = false; } break;     // 常亮绿
    case 3: 
      if (g_forceRedraw) { strip.fill(strip.Color(0, 0, 255)); strip.show(); g_forceRedraw = false; } break;     // 常亮蓝
    case 4: 
      if (g_forceRedraw) { strip.fill(strip.Color(0, 127, 255)); strip.show(); g_forceRedraw = false; } break;   // 常亮冰蓝
    case 5: 
      if (g_forceRedraw) { strip.fill(strip.Color(255, 255, 255)); strip.show(); g_forceRedraw = false; } break; // 常亮白色

    // ---- 呼吸系列 ----
    case 6: // 呼吸红
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(255, 0, 0); g_forceRedraw = false; } break;
    case 7: // 呼吸绿
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 255, 0); g_forceRedraw = false; } break;
    case 8: // 呼吸蓝
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 0, 255); g_forceRedraw = false; } break;
    case 9: // 呼吸冰蓝
      if (now - lastEffectUpdate > 15) { lastEffectUpdate = now; drawBreathing(0, 127, 255); g_forceRedraw = false; } break;

    // ---- 动态扫描与扫射系列 ----
    case 10: // 左右来回跑马灯 (经典单色红色 Knight Rider 扫射扫回效果)
      if (now - lastEffectUpdate > 50) {
        lastEffectUpdate = now;
        strip.clear();
        
        int totalSteps = (NUM_LEDS - 1) * 2; 
        int step = effectFrame % totalSteps;
        int pos = (step < NUM_LEDS) ? step : (totalSteps - step); 
        
        strip.setPixelColor(pos, strip.Color(255, 0, 50)); 
        for (int i = 0; i < NUM_LEDS; i++) {
          int diff = abs(i - pos);
          if (diff == 1) {
            strip.setPixelColor(i, strip.Color(80, 0, 15));
          } else if (diff == 2) {
            strip.setPixelColor(i, strip.Color(20, 0, 3));
          }
        }
        strip.show();
        effectFrame++;
        g_forceRedraw = false;
      }
      break;

    case 11: // 电竞双色交叉来回跑马灯 (霓虹紫与冰蓝双向交叉扫射)
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
      
    case 12: // 彩虹
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