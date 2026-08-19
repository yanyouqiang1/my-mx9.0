# E3 蓝牙音频网关实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为MX9.0键盘系统添加E3蓝牙音频网关功能，实现电脑音频通过蓝牙耳机播放，以及蓝牙耳机/本地麦克风录音

**Architecture:**
- S3作为USB声卡（播放+录音），通过I2S与E3双向传输音频，通过UART发送控制命令
- E3作为蓝牙音频网关，接收S3的I2S音频A2DP发送到蓝牙耳机，同时支持本地INMP441麦克风输入，通过I2S传回S3
- C3增加长按Mute/CPG控制S3重启/刷机模式，并支持蓝牙OTA
- E3和C3各自独立通过BLE OTA升级

**Tech Stack:** ESP32-IDF / Arduino-ESP32, A2DP Sink/Source, I2S, BLE OTA, SSD1306 OLED, SHT31

## Global Constraints

- S3↔C3 UART: 115200 8N1, S3 GPIO9(TX)↔C3 GPIO10(RX), S3 GPIO10(RX)↔C3 GPIO20(TX)
- S3↔E3 I2S: S3 GPIO11(BCLK)/GPIO12(WS)/GPIO13(DOUT)/GPIO14(DIN) ↔ E3 GPIO3(BCLK)/GPIO2(WS)/GPIO1(DOUT)/GPIO4(DIN)
- S3↔E3 UART: S3 GPIO5(TX)↔E3 GPIO6(RX), S3 GPIO6(RX)↔E3 GPIO5(TX), 115200 8N1
- E3 I2C (OLED+SHT31): GPIO7(SDA), GPIO8(SCL)
- 蓝牙设备名: YYQ-BT-Audio
- 音频采样率: 48000Hz, 16位立体声

---

## 1. E3固件开发

### 1.1 E3引脚和基础框架修正

**Files:**
- Modify: `firmware/e3/e3.ino`

**Interfaces:**
- Consumes: -
- Produces: 修正后的E3引脚定义和setup/loop框架

- [ ] **Step 1: 修正E3引脚定义**

根据设计文档，修正E3引脚:

```cpp
// ================= 引脚定义 =================
// I2S 音频接口 (连接到S3)
#define I2S_WS_PIN    2   // 字选择
#define I2S_BCK_PIN   3   // 位时钟
#define I2S_DOUT_PIN  1   // 播放数据输出 (到S3 DIN)
#define I2S_DIN_PIN   4   // 录音数据输入 (从INMP441)

// UART 控制接口 (连接到S3)
#define UART_TX_PIN   5   // → S3 GPIO6
#define UART_RX_PIN   6   // ← S3 GPIO5

// I2C 接口 (OLED + SHT31共用)
#define I2C_SDA_PIN   7
#define I2C_SCL_PIN   8

#define LED_STATUS_PIN 10  // 蓝牙状态指示灯
```

- [ ] **Step 2: 修正setup和loop结构**

```cpp
void setup() {
    Serial.begin(115200);           // 调试串口
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);  // 与S3通信
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);  // I2C for OLED & SHT31
    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);
    
    initI2S();
    initBluetooth();
    initOLED();
    initSHT31();
    
    Serial.println("E3 初始化完成");
}

void loop() {
    handleBluetoothState();
    handleUartCommands();
    updateOLED();
    handleSHT31();
}
```

- [ ] **Step 3: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "fix(e3): correct pin definitions per design spec"
```

---

### 1.2 E3 I2S双向音频接口

**Files:**
- Modify: `firmware/e3/e3.ino`

**Interfaces:**
- Consumes: -
- Produces: `initI2S()`, `i2s_rx_task_handle` (TaskHandle_t)

- [ ] **Step 1: 实现I2S RX配置（接收S3的播放音频）**

在`initI2S()`中添加I2S RX模式配置:

```cpp
void initI2S() {
    // I2S配置: Master TX (播放到蓝牙) + Master RX (接收S3音频)
    i2s_config_t i2s_tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_tx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_tx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &i2s_tx_pin_config);
    i2s_set_clk(I2S_NUM_0, 48000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // I2S RX配置 (接收S3的USB音频数据)
    i2s_config_t i2s_rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_rx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DIN_PIN  // 从S3接收音频
    };

    i2s_driver_install(I2S_NUM_1, &i2s_rx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &i2s_rx_pin_config);
    i2s_set_clk(I2S_NUM_1, 48000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // 启动RX任务
    xTaskCreate(i2s_rx_task, "i2s_rx_task", 4096, NULL, 5, &i2s_rx_task_handle);
}
```

- [ ] **Step 2: 实现I2S RX任务（接收S3音频并发送到蓝牙）**

```cpp
static TaskHandle_t i2s_rx_task_handle = NULL;

void i2s_rx_task(void* param) {
    uint8_t i2s_rx_buffer[1024];
    while (1) {
        size_t bytes_read = 0;
        // 从I2S1读取S3发来的USB音频数据
        i2s_read(I2S_NUM_1, i2s_rx_buffer, sizeof(i2s_rx_buffer), &bytes_read, portMAX_DELAY);
        if (bytes_read > 0 && btConnected) {
            // 通过A2DP发送给蓝牙耳机 (回调已经在A2DP中处理)
            // 这里可以直接写入I2S0 TX发送出去
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, i2s_rx_buffer, bytes_read, &bytes_written, portMAX_DELAY);
        }
    }
}
```

- [ ] **Step 3: 添加全局变量**

```cpp
static TaskHandle_t i2s_rx_task_handle = NULL;
static TaskHandle_t i2s_inmp441_task_handle = NULL;
static bool audioSourceLocal = false;  // false=蓝牙MIC, true=INMP441
```

- [ ] **Step 4: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "feat(e3): add bidirectional I2S audio interface"
```

---

### 1.3 E3 音频源切换逻辑

**Files:**
- Modify: `firmware/e3/e3.ino`

**Interfaces:**
- Consumes: `audioSourceLocal` (bool)
- Produces: `i2s_inmp441_task_handle`, `setAudioSource(bool local)`

- [ ] **Step 1: 添加INMP441读取任务**

```cpp
void i2s_inmp441_task(void* param) {
    uint8_t inmp441_buffer[1024];
    while (1) {
        if (audioSourceLocal) {
            size_t bytes_read = 0;
            // 从INMP441读取音频 (通过I2S1)
            i2s_read(I2S_NUM_1, inmp441_buffer, sizeof(inmp441_buffer), &bytes_read, portMAX_DELAY);
            if (bytes_read > 0) {
                // 发送到S3 (通过I2S0输出...不对, E3的I2S0是连接到S3的)
                // 实际上E3的I2S0 DOUT连接到S3的DIN, 所以应该写入I2S_NUM_0
                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, inmp441_buffer, bytes_read, &bytes_written, portMAX_DELAY);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));  // 节省CPU
        }
    }
}
```

- [ ] **Step 2: 添加音频源切换函数**

```cpp
void setAudioSource(bool local) {
    audioSourceLocal = local;
    if (local) {
        Serial1.println("AUDIO_SRC_LOCAL_ACK");
        Serial.println("音频源切换: INMP441本地麦克风");
    } else {
        Serial1.println("AUDIO_SRC_BT_ACK");
        Serial.println("音频源切换: 蓝牙耳机麦克风");
    }
}
```

- [ ] **Step 3: 在UART命令处理中添加切换逻辑**

```cpp
void processCommand(String cmd) {
    if (cmd.startsWith("CTRL_")) {
        // 现有的AVRCP控制命令...
    }
    else if (cmd == "AUDIO_SRC_LOCAL") {
        setAudioSource(true);
    }
    else if (cmd == "AUDIO_SRC_BT") {
        setAudioSource(false);
    }
}
```

- [ ] **Step 4: 在initI2S末尾启动INMP441任务**

```cpp
    xTaskCreate(i2s_inmp441_task, "i2s_inmp441_task", 4096, NULL, 5, &i2s_inmp441_task_handle);
```

- [ ] **Step 5: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "feat(e3): add audio source switching between BT MIC and INMP441"
```

---

### 1.4 E3 OLED显示驱动 (SSD1306 128x64)

**Files:**
- Create: `firmware/e3/libraries/SSD1306.h` and `SSD1306.cpp` (or use Adafruit_SSD1306 library)

**Interfaces:**
- Consumes: -
- Produces: `initOLED()`, `updateOLED()`, `oled_display`

- [ ] **Step 1: 添加OLED显示状态结构体**

```cpp
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// OLED显示状态
struct OledState {
    bool btConnected;
    bool isPlaying;
    bool audioSourceLocal;  // false=BT MIC, true=Local MIC
    float temperature;
    float humidity;
    unsigned long lastUpdate;
};
OledState oledState = {false, false, false, 0, 0, 0};
```

- [ ] **Step 2: 实现initOLED()**

```cpp
void initOLED() {
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 OLED初始化失败");
        return;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.display();
    Serial.println("OLED初始化成功");
}
```

- [ ] **Step 3: 实现updateOLED()**

```cpp
void updateOLED() {
    if (millis() - oledState.lastUpdate < 1000) return;  // 每秒更新一次
    oledState.lastUpdate = millis();

    display.clearDisplay();
    display.setCursor(0, 0);

    // 蓝牙状态
    display.print("BT: ");
    display.println(oledState.btConnected ? "Connected" : "Disconnected");

    // 播放状态
    display.print("Status: ");
    display.println(oledState.isPlaying ? "Playing" : "Paused");

    // 音频源
    display.print("MIC: ");
    display.println(oledState.audioSourceLocal ? "Local (INMP441)" : "Bluetooth");

    // 温湿度
    display.printf("Temp: %.1f C\n", oledState.temperature);
    display.printf("Humi: %.1f %%", oledState.humidity);

    display.display();
}
```

- [ ] **Step 4: 在蓝牙回调中更新OLED状态**

在`bt_av_hdl_stack_evt`的`ESP_A2D_CONNECTION_STATE_EVT` case中添加:
```cpp
oledState.btConnected = btConnected;
```

在`bt_av_hdl_avrc_evt`的`ESP_AVRC_CT_PLAY_STATE_RC_EVT` case中添加:
```cpp
oledState.isPlaying = (avrc_param->play_stat.play_status == ESP_AVRC_PLAYBACK_PLAYING);
```

在`setAudioSource()`中添加:
```cpp
oledState.audioSourceLocal = local;
```

- [ ] **Step 5: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "feat(e3): add SSD1306 OLED display support"
```

---

### 1.5 E3 温湿度传感器驱动 (SHT31)

**Files:**
- Modify: `firmware/e3/e3.ino`

**Interfaces:**
- Consumes: -
- Produces: `initSHT31()`, `handleSHT31()`, `readSHT31()`

- [ ] **Step 1: 添加SHT31相关定义**

```cpp
#include <Adafruit_SHT31.h>

Adafruit_SHT31 sht31 = Adafruit_SHT31();

unsigned long lastSHT31Read = 0;
const unsigned long SHT31_READ_INTERVAL = 5000;  // 5秒
```

- [ ] **Step 2: 实现initSHT31()**

```cpp
void initSHT31() {
    if (!sht31.begin(0x44)) {  // SHT31默认地址0x44
        Serial.println("SHT31初始化失败");
        return;
    }
    Serial.println("SHT31初始化成功");
}
```

- [ ] **Step 3: 实现handleSHT31()**

```cpp
void handleSHT31() {
    if (millis() - lastSHT31Read < SHT31_READ_INTERVAL) return;
    lastSHT31Read = millis();

    oledState.temperature = sht31.readTemperature();
    oledState.humidity = sht31.readHumidity();

    Serial.printf("SHT31: Temp=%.1f C, Humi=%.1f %%\n", oledState.temperature, oledState.humidity);
}
```

- [ ] **Step 4: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "feat(e3): add SHT31 temperature/humidity sensor support"
```

---

### 1.6 E3 蓝牙OTA升级

**Files:**
- Modify: `firmware/e3/e3.ino`

**Interfaces:**
- Consumes: -
- Produces: BLE OTA service with UUID `0000FFFF-0000-1000-8000-00805F9B34FB`

- [ ] **Step 1: 添加OTA命令处理**

在`processCommand()`中添加:
```cpp
else if (cmd == "OTA_START") {
    Serial.println("进入OTA模式...");
    Serial1.println("OTA_START_ACK");
    // 触发OTA重启
    delay(100);
    ESP.restart();
}
```

- [ ] **Step 2: 在setup中配置OTA**

```cpp
// 检查是否是OTA启动
preferences.begin("e3", false);
if (preferences.getBool("ota_mode", false)) {
    preferences.putBool("ota_mode", false);
    Serial.println("OTA模式启动");
    // 执行OTA更新逻辑
}
preferences.end();
```

- [ ] **Step 3: 使用标准ESP32 BLE OTA库**

添加BLE OTA服务:
```cpp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Update.h>

#define OTA_SERVICE_UUID "0000FFFF-0000-1000-8000-00805F9B34FB"
#define OTA_CHAR_UUID    "0000FF01-0000-1000-8000-00805F9B34FB"

class OtaCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        Serial.println("OTA客户端连接");
    }
    void onDisconnect(BLEServer* pServer) {
        Serial.println("OTA客户端断开");
    }
};

class OtaCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string rxData = pCharacteristic->getValue();
        if (rxData.length() > 0) {
            if (rxData[0] == 0xF0) {  // OTA开始命令
                if (Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.setAuthCode(0xF0);
                    Serial.println("OTA开始接收");
                }
            } else if (rxData[0] == 0xF1) {  // OTA结束命令
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
```

在`setup()`中添加OTA服务初始化:
```cpp
void initOTA() {
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pOtaService = pServer->createService(OTA_SERVICE_UUID);
    BLECharacteristic *pOtaChar = pOtaService->createCharacteristic(
        OTA_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY
    );
    pOtaChar->setCallbacks(new OtaCharacteristicCallbacks());
    pOtaService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
    pAdvertising->start();
    Serial.println("BLE OTA服务已启动");
}
```

- [ ] **Step 4: 提交**

```bash
git add firmware/e3/e3.ino
git commit -m "feat(e3): add BLE OTA firmware update support"
```

---

## 2. S3固件更新

### 2.1 S3 USB声卡功能

**Files:**
- Modify: `firmware/s3/s3.ino`

**Interfaces:**
- Consumes: -
- Produces: USB Audio Class support ( playback + recording)

- [ ] **Step 1: 添加USB音频库**

在`setup()`之前添加:
```cpp
#include <USBAudio.h>
#include <usbh_audio.h>

// USB Audio状态
static bool usbAudioConnected = false;
static uint8_t usbAudioVolume = 100;
```

- [ ] **Step 2: 初始化USB Audio**

在`setup()`的USB初始化部分添加:
```cpp
// USB Audio初始化
USB.begin();
USBAudio.begin();
USBAudio.setVolume(usbAudioVolume);
```

- [ ] **Step 3: 添加USB音频任务**

```cpp
void usbAudioTask(void* param) {
    uint8_t audioBuffer[512];
    while (1) {
        if (USBAudio.isConnected()) {
            usbAudioConnected = true;
            // 读取USB音频数据并发送到E3
            size_t bytesRead = USBAudio.read(audioBuffer, sizeof(audioBuffer));
            if (bytesRead > 0) {
                // 通过I2S发送 到 E3
                size_t bytesWritten = 0;
                i2s_write(I2S_NUM_0, audioBuffer, bytesRead, &bytesWritten, portMAX_DELAY);
            }
        } else {
            usbAudioConnected = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
```

- [ ] **Step 4: 提交**

```bash
git add firmware/s3/s3.ino
git commit -m "feat(s3): add USB Audio Class support"
```

---

### 2.2 S3 I2S音频接口

**Files:**
- Modify: `firmware/s3/s3.ino`

**Interfaces:**
- Consumes: -
- Produces: `initI2S_S3()`, `i2s_tx_task`, `i2s_rx_task`

- [ ] **Step 1: 添加S3的I2S引脚定义**

```cpp
// I2S接口 (连接到E3)
#define I2S_BCK_PIN   11
#define I2S_WS_PIN    12
#define I2S_DOUT_PIN  13  // 播放数据输出 -> E3 DIN
#define I2S_DIN_PIN   14  // 录音数据输入 <- E3 DOUT

// UART接口 (控制E3)
#define E3_UART_TX_PIN 5
#define E3_UART_RX_PIN 6
```

- [ ] **Step 2: 实现initI2S_S3()**

```cpp
void initI2S_S3() {
    // I2S TX配置 (播放)
    i2s_config_t i2s_tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t i2s_tx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_tx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &i2s_tx_pin_config);
    i2s_set_clk(I2S_NUM_0, 48000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // I2S RX配置 (录音)
    i2s_config_t i2s_rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
    };

    i2s_pin_config_t i2s_rx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DIN_PIN
    };

    i2s_driver_install(I2S_NUM_1, &i2s_rx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &i2s_rx_pin_config);
    i2s_set_clk(I2S_NUM_1, 48000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // 启动I2S任务
    xTaskCreate(i2s_tx_task, "i2s_tx_task", 4096, NULL, 5, NULL);
    xTaskCreate(i2s_rx_task, "i2s_rx_task", 4096, NULL, 5, NULL);
}
```

- [ ] **Step 3: 实现I2S任务函数**

```cpp
void i2s_tx_task(void* param) {
    uint8_t txBuffer[512];
    while (1) {
        size_t bytesRead = 0;
        // 从USB读取音频数据
        if (USBAudio.isConnected()) {
            size_t len = USBAudio.read(txBuffer, sizeof(txBuffer));
            if (len > 0) {
                size_t written = 0;
                i2s_write(I2S_NUM_0, txBuffer, len, &written, portMAX_DELAY);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void i2s_rx_task(void* param) {
    uint8_t rxBuffer[512];
    while (1) {
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_1, rxBuffer, sizeof(rxBuffer), &bytesRead, portMAX_DELAY);
        if (bytesRead > 0) {
            // 将录音数据发送到USB
            USBAudio.write(rxBuffer, bytesRead);
        }
    }
}
```

- [ ] **Step 4: 在setup()中调用initI2S_S3()**

在`setup()`的Wire和MCP初始化之后添加:
```cpp
initI2S_S3();
```

- [ ] **Step 5: 提交**

```bash
git add firmware/s3/s3.ino
git commit -m "feat(s3): add I2S audio interface to E3"
```

---

### 2.3 S3 UART控制E3

**Files:**
- Modify: `firmware/s3/s3.ino`

**Interfaces:**
- Consumes: -
- Produces: `Serial2` (E3 UART), `sendAudioSourceCommand(bool local)`

- [ ] **Step 1: 添加E3 UART定义**

```cpp
#define E3_SERIAL Serial2
```

- [ ] **Step 2: 在setup()中初始化E3 UART**

```cpp
E3_SERIAL.begin(115200, SERIAL_8N1, E3_UART_RX_PIN, E3_UART_TX_PIN);
```

- [ ] **Step 3: 添加音频源切换命令函数**

```cpp
void sendAudioSourceCommand(bool local) {
    if (local) {
        E3_SERIAL.println("AUDIO_SRC_LOCAL");
        Serial.println("发送命令: 切换到本地麦克风");
    } else {
        E3_SERIAL.println("AUDIO_SRC_BT");
        Serial.println("发送命令: 切换到蓝牙麦克风");
    }
}
```

- [ ] **Step 4: 在loop中处理E3的UART响应**

```cpp
// 处理来自E3的UART响应
while (E3_SERIAL.available()) {
    String cmd = E3_SERIAL.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("BT_CONNECTED")) {
        Serial.println("E3: 蓝牙已连接");
    } else if (cmd.startsWith("BT_DISCONNECTED")) {
        Serial.println("E3: 蓝牙已断开");
    } else if (cmd == "AUDIO_SRC_LOCAL_ACK") {
        Serial.println("E3: 音频源已切换到本地麦克风");
    } else if (cmd == "AUDIO_SRC_BT_ACK") {
        Serial.println("E3: 音频源已切换到蓝牙麦克风");
    }
}
```

- [ ] **Step 5: 提交**

```bash
git add firmware/s3/s3.ino
git commit -m "feat(s3): add UART control for E3 audio source switching"
```

---

## 3. C3固件更新

### 3.1 C3 长按Mute/CPG控制S3

**Files:**
- Modify: `firmware/c3/c3.ino`

**Interfaces:**
- Consumes: -
- Produces: 长按检测逻辑修改

- [ ] **Step 1: 修改Mute按钮长按检测逻辑**

找到现有的Mute按钮处理代码（大约在`handleButtons()`函数中），修改为:

```cpp
// 1. 静音按键 -> 静音切换 / 长按重启S3
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
    }

    if (muteReading == LOW && muteTriggered && !muteLongPressHandled) {
        unsigned long pressDuration = now - mutePressStart;

        // 长按 3-8秒: 发送 N_S3_REBOT
        if (pressDuration >= 3000 && pressDuration < 8000) {
            Serial1.println("N_S3_REBOT");
            Serial.println("Mute长按3-8秒: 发送N_S3_REBOT");
            muteLongPressHandled = true;  // 防止重复发送
        }
        // 长按 8秒以上: 发送 N_S3_ROOT
        else if (pressDuration >= 8000) {
            Serial1.println("N_S3_ROOT");
            Serial.println("Mute长按8秒+: 发送N_S3_ROOT");
            muteLongPressHandled = true;
        }
    }

    if (muteReading == HIGH && muteTriggered) {
        unsigned long pressDuration = now - mutePressStart;

        // 短按 (< 3秒): 静音切换
        if (pressDuration < 3000 && !muteLongPressHandled) {
            // 静音切换逻辑
            currentMode = MODE_MUTE;
            updateIndicators();
            Serial.println("Mute短按: 切换到音量控制模式");
        }

        muteTriggered = false;
        muteLongPressHandled = false;
    }
}
```

- [ ] **Step 2: 修改CPG按钮长按检测逻辑**

找到CPG按钮处理代码，修改为:

```cpp
// 3. CPG 按键 -> 短按切换灯效 / 长按3秒+ 进入刷机模式
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
    }

    if (cpgReading == LOW && cpgTriggered && !cpgLongPressHandled) {
        unsigned long pressDuration = now - cpgPressStart;

        // 长按 3秒以上: 发送 N_S3_ROOT
        if (pressDuration >= 3000) {
            Serial1.println("N_S3_ROOT");
            Serial.println("CPG长按3秒+: 发送N_S3_ROOT");
            cpgLongPressHandled = true;
        }
    }

    if (cpgReading == HIGH && cpgTriggered) {
        unsigned long pressDuration = now - cpgPressStart;

        // 短按 (< 3秒): 切换灯效
        if (pressDuration < 3000 && !cpgLongPressHandled) {
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
            Serial.println("CPG短按: 切换灯效");
        }

        cpgTriggered = false;
        cpgLongPressHandled = false;
    }
}
```

- [ ] **Step 3: 提交**

```bash
git add firmware/c3/c3.ino
git commit -m "feat(c3): add long-press mute/CPG to control S3 reboot/flashing"
```

---

### 3.2 C3 蓝牙OTA升级

**Files:**
- Modify: `firmware/c3/c3.ino`

**Interfaces:**
- Consumes: -
- Produces: BLE OTA service

- [ ] **Step 1: 在setup中添加OTA服务初始化**

```cpp
void initOTA() {
    // C3的BLE OTA服务
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pOtaService = pServer->createService(OTA_SERVICE_UUID);
    BLECharacteristic *pOtaChar = pOtaService->createCharacteristic(
        OTA_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY
    );
    pOtaChar->setCallbacks(new OtaCharacteristicCallbacks());
    pOtaService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
    pAdvertising->start();
}

class OtaCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string rxData = pCharacteristic->getValue();
        if (rxData.length() > 0) {
            if (rxData[0] == 0xF0) {
                if (Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.setAuthCode(0xF0);
                }
            } else if (rxData[0] == 0xF1) {
                if (Update.end(true)) {
                    ESP.restart();
                }
            } else {
                Update.write((uint8_t*)rxData.data(), rxData.size());
            }
        }
    }
};
```

- [ ] **Step 2: 添加OTA相关include**

```cpp
#include <Update.h>

#define OTA_SERVICE_UUID "0000FFFF-0000-1000-8000-00805F9B34FB"
#define OTA_CHAR_UUID    "0000FF01-0000-1000-8000-00805F9B34FB"
```

- [ ] **Step 3: 在setup()末尾调用initOTA()**

- [ ] **Step 4: 提交**

```bash
git add firmware/c3/c3.ino
git commit -m "feat(c3): add BLE OTA firmware update support"
```

---

## 4. 系统集成

### 4.1 完整固件编译测试

**Files:**
- All firmwares

- [ ] **Step 1: 编译S3固件**

```bash
cd firmware/s3
pio run -e s3
```

预期: 编译成功，无错误

- [ ] **Step 2: 编译C3固件**

```bash
cd firmware/c3
pio run -e c3
```

预期: 编译成功，无错误

- [ ] **Step 3: 编译E3固件**

```bash
cd firmware/e3
pio run -e e3
```

预期: 编译成功，无错误

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "feat: complete E3 audio gateway implementation for MX9.0"
```

---

## 实现顺序建议

1. **E3固件** (最复杂，先完成)
   - 1.1 引脚修正
   - 1.2 I2S双向音频
   - 1.3 音频源切换
   - 1.4 OLED显示
   - 1.5 SHT31
   - 1.6 BLE OTA

2. **S3固件**
   - 2.1 USB声卡
   - 2.2 I2S接口
   - 2.3 UART控制E3

3. **C3固件**
   - 3.1 长按逻辑
   - 3.2 BLE OTA

4. **集成测试**
   - 4.1 编译测试
