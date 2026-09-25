#include <Adafruit_NeoPixel.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#define WDT_TIMEOUT 3

// ================= 引脚与硬件配置 =================
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
#define UART_BAUD 460800

#define NUM_MAIN_LEDS 16
#define NUM_IND_LEDS 3

Adafruit_NeoPixel strip(NUM_MAIN_LEDS, PIN_WS2812, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel indicatorStrip(NUM_IND_LEDS, PIN_INDICATOR, NEO_GRB + NEO_KHZ800);

// ================= 旋转编码器 (高优先级硬件中断) =================
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
      encoderDirection = -1;
      encoderMoved = true;
      pulseCounter = 0;
    } else if (pulseCounter <= -4) {
      encoderDirection = 1;
      encoderMoved = true;
      pulseCounter = 0;
    }
  }
}

// ================= 按键去抖与状态上报 =================
struct ButtonHandler {
  uint8_t pin;
  const char* clickMsg;
  const char* holdMsg;
  unsigned long holdDuration;
  bool lastState;
  unsigned long lastDebounce;
  bool triggered;
  unsigned long pressStart;
  bool holdSent;
};

ButtonHandler buttons[] = {
  { PIN_MUTE,     "BTN:MUTE",  "BTN:MUTE_HOLD",  3000, HIGH, 0, false, 0, false },
  { PIN_LIGHT,    "BTN:LIGHT", "BTN:LIGHT_HOLD", 2000, HIGH, 0, false, 0, false },
  { PIN_CPG,      "BTN:CPG",   "BTN:CPG_HOLD",   3000, HIGH, 0, false, 0, false },
  { PIN_KNOB_BTN, "BTN:KNOB",  nullptr,          0,    HIGH, 0, false, 0, false }
};

void handleButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    bool reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastState) {
      buttons[i].lastDebounce = now;
      buttons[i].lastState = reading;
    }
    if ((now - buttons[i].lastDebounce) > 40) {
      if (reading == LOW) {
        if (!buttons[i].triggered) {
          buttons[i].triggered = true;
          buttons[i].pressStart = now;
          buttons[i].holdSent = false;
        } else if (buttons[i].holdMsg && !buttons[i].holdSent && (now - buttons[i].pressStart >= buttons[i].holdDuration)) {
          buttons[i].holdSent = true;
          Serial1.println(buttons[i].holdMsg);
        }
      } else {
        if (buttons[i].triggered) {
          buttons[i].triggered = false;
          if (!buttons[i].holdSent) {
            Serial1.println(buttons[i].clickMsg);
          }
        }
      }
    }
  }
}

// ================= 极简高速推流解码状态机 =================
// 协议定义：[0xAA] [0x55] [CMD] [Data...] [0xEE]
void handleLedFrameStream() {
  static uint8_t rxState = 0;
  static uint8_t cmd = 0;
  static uint8_t dataBuffer[57];
  static uint8_t dataIdx = 0;

  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();

    switch (rxState) {
      case 0: // 等待第 1 帧头
        if (b == 0xAA) rxState = 1;
        break;
      case 1: // 等待第 2 帧头
        if (b == 0x55) rxState = 2;
        else rxState = 0;
        break;
      case 2: // 接收命令码
        cmd = b;
        if (cmd == 0x01) { // 0x01 = RGB Frame
          dataIdx = 0;
          rxState = 3;
        } else if (cmd == 0x02) { // 0x02 = Ping
          rxState = 4;
        } else {
          rxState = 0;
        }
        break;
      case 3: // 读取 57 字节 RGB 数据
        dataBuffer[dataIdx++] = b;
        if (dataIdx >= 57) rxState = 4;
        break;
      case 4: // 校验尾帧 0xEE
        if (b == 0xEE) {
          if (cmd == 0x01) {
            // 直接将点阵赋给 WS2812 驱动
            for (int i = 0; i < NUM_MAIN_LEDS; i++) {
              strip.setPixelColor(i, dataBuffer[i * 3 + 0], dataBuffer[i * 3 + 1], dataBuffer[i * 3 + 2]);
            }
            for (int i = 0; i < NUM_IND_LEDS; i++) {
              int offset = (NUM_MAIN_LEDS + i) * 3;
              indicatorStrip.setPixelColor(i, dataBuffer[offset + 0], dataBuffer[offset + 1], dataBuffer[offset + 2]);
            }
            strip.show();
            indicatorStrip.show();
          } else if (cmd == 0x02) {
            Serial1.println("PONG");
          }
        }
        rxState = 0;
        break;
      default:
        rxState = 0;
        break;
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  for (int i = 0; i < 4; i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }

  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B), encoderISR, CHANGE);

  strip.begin();
  strip.setBrightness(255); // C3 全开，由 S3 帧流直接控制每个像素实际数值
  strip.clear();
  strip.show();

  indicatorStrip.begin();
  indicatorStrip.setBrightness(255);
  indicatorStrip.clear();
  indicatorStrip.show();

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
  Serial.println("C3 纯从机驱动就绪");
}

// ================= LOOP =================
void loop() {
  esp_task_wdt_reset();

  // 1. 旋钮动作上报
  if (encoderMoved) {
    bool isRight = (encoderDirection == 1);
    encoderMoved = false;
    Serial1.println(isRight ? "ENC:+" : "ENC:-");
  }

  // 2. 按键状态机扫描上报
  handleButtons();

  // 3. 实时接收 S3 像素推流并刷新
  handleLedFrameStream();

  delayMicroseconds(50);
}