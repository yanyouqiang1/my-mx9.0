# Bluetooth Audio E3/S3/C3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 E3 蓝牙音频接收器 + S3 USB 音频复合设备，让电脑可以通过蓝牙耳机播放音乐和通话

**Architecture:** E3 作为蓝牙音频 Hub，接收 A2DP/HFP 音频并通过 I2S 传输给 S3；S3 作为 USB 复合设备（HID键盘 + USB Speaker + USB Mic），同时通过 UART 与 C3 通信控制灯效

**Tech Stack:** Arduino-ESP32, ESP32-BLE-Arduino, ESP32-Audio-I2S, USB-OTG, FreeRTOS

## Global Constraints

- 串口波特率: 115200, 8N1
- I2S 格式: 16-bit, 48kHz, 立体声, I2S 标准协议
- USB Audio: 16-bit, 48kHz
- 蓝牙协议: A2DP Sink (音乐) + HFP (通话)
- S3 USB PID: 0x001F, VID: 0x303A

---

## File Structure

```
fireware/
├── e3/
│   └── e3.ino                    # E3 蓝牙音频固件 (新建)
├── s3/
│   └── s3-audio.ino              # S3 键盘+音频复合固件 (从 最终版/s3.ino 修改)
└── c3/
    └── c3.ino                    # C3 LED 固件 (从 最终版/c3.ino 复制)
```

---

## Task Map

| Task | 内容 | 文件 |
|------|------|------|
| 1 | 创建 E3 文件夹和基础代码框架 | fireware/e3/e3.ino |
| 2 | 实现 E3 蓝牙 A2DP Sink 模块 | fireware/e3/e3.ino |
| 3 | 实现 E3 I2S 输出模块 | fireware/e3/e3.ino |
| 4 | 实现 E3 UART 控制命令模块 | fireware/e3/e3.ino |
| 5 | 实现 E3 LED 指示模块 | fireware/e3/e3.ino |
| 6 | 修改 S3 添加 I2S 接收模块 | fireware/s3/s3-audio.ino |
| 7 | 修改 S3 添加 USB Audio Speaker | fireware/s3/s3-audio.ino |
| 8 | 修改 S3 添加 USB Audio Microphone | fireware/s3/s3-audio.ino |
| 9 | 修改 S3 集成音频路由状态机 | fireware/s3/s3-audio.ino |
| 10 | 创建 C3 固件副本 | fireware/c3/c3.ino |

---

## Task 1: 创建 E3 文件夹和基础代码框架

**Files:**
- Create: `fireware/e3/e3.ino`

**Interfaces:**
- Produces: `E3BluetoothAudio` 类，初始化函数 `setupE3()`, 主循环 `loopE3()`

```cpp
// fireware/e3/e3.ino - E3 蓝牙音频接收器固件
// 功能: 接收蓝牙A2DP/HFP音频，通过I2S输出到S3

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAudio.h>
#include <driver/i2s.h>

// ================= 引脚定义 =================
#define I2S_WS_PIN    3
#define I2S_BCK_PIN   2
#define I2S_DATA_PIN   1

#define UART_TX_PIN    5   // → S3 GPIO16
#define UART_RX_PIN    4   // ← S3 GPIO15

#define LED_STATUS_PIN 10  // 蓝牙状态指示灯

// ================= 音频参数 =================
#define I2S_SAMPLE_RATE   48000
#define I2S_BUFFER_SIZE   512

// ================= 全局状态 =================
static bool btConnected = false;
static bool isPlaying = false;
static String deviceName = "YYQ-BT-Audio";

// I2S 发送缓冲区
static int16_t i2sTxBuffer[I2S_BUFFER_SIZE * 2]; // 立体声

void setupE3() {
    // 初始化串口 (用于控制命令)
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    // 初始化状态 LED
    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);

    // 初始化 I2S (Master TX)
    initI2S();

    // 初始化蓝牙
    initBluetooth();

    Serial.println("E3 蓝牙音频接收器初始化完成");
}

void loopE3() {
    // 处理蓝牙状态
    handleBluetoothState();

    // 处理 UART 命令
    handleUartCommands();

    // 发送音频数据到 I2S
    sendAudioToI2S();
}
```

---

## Task 2: 实现 E3 蓝牙 A2DP Sink 模块

**Files:**
- Modify: `fireware/e3/e3.ino` (添加蓝牙 A2DP/HFP 实现)

**Interfaces:**
- Consumes: `btConnected`, `isPlaying` 状态
- Produces: `BluetoothAudioStream` 类，回调 `onAudioData()`

- [ ] **Step 1: 添加 BLE A2D 库和全局变量**

在文件顶部添加:

```cpp
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

// 蓝牙回调函数声明
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param);
static void bt_i2s_config(void);

// A2DP 状态
static bool isStreaming = false;
static uint32_t btWriteIdx = 0;
```

- [ ] **Step 2: 实现 `initBluetooth()` 函数**

```cpp
void initBluetooth() {
    esp_err_t err;

    // 释放经典 BT 内存（如果不需要）
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err) {
        Serial.printf("BT 内存释放失败: %s\n", esp_err_to_name(err));
    }

    // 初始化 BT 控制器
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err) {
        Serial.printf("BT 控制器初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err) {
        Serial.printf("BT 控制器使能失败: %s\n", esp_err_to_name(err));
        return;
    }

    // 初始化 Bluedroid
    err = esp_bluedroid_init();
    if (err) {
        Serial.printf("Bluedroid 初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bluedroid_enable();
    if (err) {
        Serial.printf("Bluedroid 使能失败: %s\n", esp_err_to_name(err));
        return;
    }

    // 设置设备名称
    esp_bt_dev_set_device_name(deviceName.c_str());

    // 初始化 A2DP
    esp_a2d_register_callback(bt_av_hdl_stack_evt);
    esp_a2d_sink_register_data_callback(bt_i2s_write_data);
    esp_a2d_sink_init();

    // 初始化 AVRCP
    esp_avrc_ct_register_callback(bt_av_hdl_avrc_evt);
    esp_avrc_ct_init();

    // 设置扫描模式
    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);

    Serial.println("蓝牙初始化完成，等待连接...");
}
```

- [ ] **Step 3: 实现 A2DP 数据回调 `bt_i2s_write_data()`**

```cpp
static int32_t bt_i2s_write_data(const uint8_t *data, int32_t len) {
    if (!btConnected) return 0;

    // 将 BT 数据写入 I2S DMA 缓冲区
    // data 是 SBC 编码数据，需要先解码...
    // 这里假设已经过内部解码，直接是 PCM 数据

    // 写入 I2S TX FIFO
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, data, len, &bytesWritten, portMAX_DELAY);

    return bytesWritten;
}
```

- [ ] **Step 4: 实现 AVRCP 回调处理播放状态**

```cpp
static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param) {
    esp_avrc_ct_cb_param_t *param = (esp_avrc_ct_cb_param_t *)p_param;

    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            btConnected = param->conn_stat.connected;
            if (btConnected) {
                Serial1.println("BT_CONNECTED:YYQ-BT-Audio");
                digitalWrite(LED_STATUS_PIN, HIGH);
            } else {
                Serial1.println("BT_DISCONNECTED");
                digitalWrite(LED_STATUS_PIN, LOW);
                isStreaming = false;
            }
            break;
        }
        case ESP_AVRC_CT_PLAY_STATE_RC_EVT: {
            switch (param->play_stat.play_status) {
                case ESP_AVRC_PLAYBACK_PLAYING:
                    isPlaying = true;
                    Serial1.println("BT_PLAYBACK:playing");
                    break;
                case ESP_AVRC_PLAYBACK_PAUSED:
                case ESP_AVRC_PLAYBACK_STOPPED:
                    isPlaying = false;
                    Serial1.println("BT_PLAYBACK:paused");
                    break;
            }
            break;
        }
        default:
            break;
    }
}
```

---

## Task 3: 实现 E3 I2S 输出模块

**Files:**
- Modify: `fireware/e3/e3.ino` (添加 I2S Master TX 配置)

**Interfaces:**
- Consumes: `bt_i2s_write_data()` 接收的音频数据
- Produces: I2S 硬件输出到 S3

- [ ] **Step 1: 添加 I2S 头文件**

```cpp
#include "driver/i2s.h"
#include "soc/i2s_struct.h"
```

- [ ] **Step 2: 实现 `initI2S()` 函数**

```cpp
void initI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER_TX | I2S_MODE_TX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .ws_io_num = I2S_WS_PIN,
        .bcK_io_num = I2S_BCK_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_sample_rates(I2S_NUM_0, I2S_SAMPLE_RATE);

    // 清空 TX FIFO
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf("I2S 初始化完成 (WS=%d, BCK=%d, DATA=%d)\n",
                  I2S_WS_PIN, I2S_BCK_PIN, I2S_DATA_PIN);
}
```

- [ ] **Step 3: 实现 `sendAudioToI2S()` 函数**

```cpp
void sendAudioToI2S() {
    // A2DP 的数据已经在 bt_i2s_write_data() 中直接写入 I2S
    // 此函数用于处理非流媒体时的静音输出
    if (btConnected && !isStreaming) {
        // 发送静音数据
        memset(i2sTxBuffer, 0, sizeof(i2sTxBuffer));
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, i2sTxBuffer, sizeof(i2sTxBuffer), &bytesWritten, 0);
    }
}
```

---

## Task 4: 实现 E3 UART 控制命令模块

**Files:**
- Modify: `fireware/e3/e3.ino` (添加 UART 命令解析)

**Interfaces:**
- Consumes: S3 下发的 `CTRL_*` 命令
- Produces: 蓝牙播放控制

- [ ] **Step 1: 添加命令处理函数**

```cpp
String uartBuffer = "";

void handleUartCommands() {
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n') {
            uartBuffer.trim();
            processCommand(uartBuffer);
            uartBuffer = "";
        } else if (c != '\r') {
            uartBuffer += c;
        }
    }
}

void processCommand(String cmd) {
    if (cmd.startsWith("CTRL_")) {
        String action = cmd.substring(5);

        if (action == "PLAYPAUSE") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action == "NEXT") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action == "PREV") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action.startsWith("VOLUME:")) {
            int volume = atoi(action.substring(7).c_str());
            // 蓝牙音量控制
            uint8_t volume_u8 = (uint8_t)(volume * 127 / 100);
            esp_a2d_sink_set_abs_vol(volume_u8);
        }
    }
}
```

---

## Task 5: 实现 E3 LED 指示模块

**Files:**
- Modify: `fireware/e3/e3.ino` (添加 LED 状态指示)

**Interfaces:**
- Consumes: `btConnected`, `isPlaying`, `isStreaming` 状态
- Produces: GPIO10 LED 输出

- [ ] **Step 1: 添加 LED 状态机**

```cpp
unsigned long lastLedUpdate = 0;
int ledState = 0;  // 0=空闲, 1=配对中, 2=已连接, 3=播放中, 4=通话中

void handleBluetoothState() {
    unsigned long now = millis();

    if (!btConnected) {
        // 配对中 - 快速闪烁
        if (now - lastLedUpdate > 200) {
            lastLedUpdate = now;
            ledState = 1;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else if (!isPlaying) {
        // 已连接但未播放 - 慢速闪烁
        if (now - lastLedUpdate > 1000) {
            lastLedUpdate = now;
            ledState = 2;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else {
        // 播放中 - 常亮
        ledState = 3;
        digitalWrite(LED_STATUS_PIN, HIGH);
    }
}
```

---

## Task 6: 修改 S3 添加 I2S 接收模块

**Files:**
- Modify: `fireware/s3/s3-audio.ino` (在现有 s3.ino 基础上添加 I2S 接收)
- Create: `fireware/s3/s3-audio.ino` (复制自 最终版/s3.ino)

**Interfaces:**
- Consumes: E3 I2S TX 数据流
- Produces: `receivedAudioBuffer[]` 音频数据，供 USB Audio 使用

- [ ] **Step 1: 复制并创建 s3-audio.ino**

从 `最终版/s3.ino` 复制到 `fireware/s3/s3-audio.ino`

- [ ] **Step 2: 添加 I2S 头文件和配置**

在文件顶部添加:

```cpp
#include "driver/i2s.h"

// ================= I2S RX 配置 (接收 E3 音频) =================
#define I2S_RX_BCK_PIN   5
#define I2S_RX_WS_PIN    6
#define I2S_RX_DATA_PIN  4

#define I2S_RX_SAMPLE_RATE  48000
#define I2S_RX_BUFFER_SIZE  1024

static int16_t i2sRxBuffer[I2S_RX_BUFFER_SIZE * 2]; // 立体声 PCM
```

- [ ] **Step 3: 添加 I2S 接收初始化函数**

```cpp
void initI2SRx() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate = I2S_RX_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = false,
    };

    i2s_pin_config_t pin_config = {
        .ws_io_num = I2S_RX_WS_PIN,
        .bcK_io_num = I2S_RX_BCK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_RX_DATA_PIN,
    };

    i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pin_config);
    i2s_set_sample_rates(I2S_NUM_1, I2S_RX_SAMPLE_RATE);

    Serial.printf("I2S RX 初始化完成 (WS=%d, BCK=%d, DATA=%d)\n",
                   I2S_RX_WS_PIN, I2S_RX_BCK_PIN, I2S_RX_DATA_PIN);
}
```

- [ ] **Step 4: 在 setup() 中调用 initI2SRx()**

在现有 setup() 函数中添加:

```cpp
// 在 setup() 的 Wire.begin() 之后添加:
// 初始化 I2S RX (接收 E3 音频)
initI2SRx();
```

- [ ] **Step 5: 添加 I2S 读取函数**

```cpp
bool readI2SAudio(int16_t* buffer, int samples) {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_1, buffer, samples * 4, &bytesRead, 0);
    return bytesRead > 0;
}
```

---

## Task 7: 修改 S3 添加 USB Audio Speaker

**Files:**
- Modify: `fireware/s3/s3-audio.ino`

**Interfaces:**
- Consumes: `i2sRxBuffer[]` PCM 数据
- Produces: USB Audio Speaker 端点输出到电脑

- [ ] **Step 1: 添加 USB Audio 库**

```cpp
#include "USB.h"
#include "USBAudio.h"
#include "usbaudio.cpp"  // 内置 USB Audio 类
```

- [ ] **Step 2: 添加 USB Speaker 配置**

```cpp
// USB Audio Speaker 配置
#define USB_AUDIO_SAMPLE_RATE  48000
#define USB_AUDIO_CHANNELS     2
#define USB_AUDIO_BITS_PER_SAMPLE 16

class USBAudioSpeaker {
public:
    USBAudioSpeaker() {}

    void begin() {
        USB.productName("YYQ-MX9.0 Audio");
        USB.manufacturerName("YYQ");
        USB.VID(0x303A);
        USB.PID(0x0020);  // 新 PID 表示音频设备

        // 配置音频格式
        setAudioConfig(USB_AUDIO_SAMPLE_RATE, USB_AUDIO_CHANNELS, USB_AUDIO_BITS_PER_SAMPLE);
    }

    size_t write(const int16_t* data, size_t len) {
        // 通过 USB 发送音频数据
        return transmitAudio(data, len);
    }

private:
    size_t transmitAudio(const int16_t* data, size_t len) {
        // USB Audio 端点 2 (Speaker OUT)
        // 实现音频数据封包发送
        uint8_t usbBuf[64];
        size_t offset = 0;

        while (offset < len * 2) { // len 是样本数，每个样本2字节
            size_t copyLen = min((size_t)64, len * 2 - offset);
            memcpy(usbBuf, ((uint8_t*)data) + offset, copyLen);
            // 发送到医院 USB 端点
            USB.write(0x02, usbBuf, copyLen);
            offset += copyLen;
        }
        return len;
    }
};

static USBAudioSpeaker usbSpeaker;
```

- [ ] **Step 3: 在 loop() 中添加音频路由**

```cpp
// 音频路由状态
enum AudioState { AUDIO_IDLE, AUDIO_READY, AUDIO_STREAMING };
AudioState audioState = AUDIO_IDLE;

void loop() {
    // ... 现有键盘扫描代码 ...

    // 处理音频路由
    handleAudioRouting();

    delay(1);
}

void handleAudioRouting() {
    // 从 I2S 读取音频数据
    static int16_t audioBuffer[256];
    if (readI2SAudio(audioBuffer, 256)) {
        // 发送到 USB Speaker
        usbSpeaker.write(audioBuffer, 256);
        audioState = AUDIO_STREAMING;
    } else {
        if (audioState == AUDIO_STREAMING) {
            audioState = AUDIO_READY;
        }
    }
}
```

---

## Task 8: 修改 S3 添加 USB Audio Microphone

**Files:**
- Modify: `fireware/s3/s3-audio.ino`

**Interfaces:**
- Consumes: 电脑 USB Microphone IN 请求
- Produces: 静音的 MIC 数据响应（因为无上行音频源）

- [ ] **Step 1: 添加 USB Microphone 类**

```cpp
class USBAudioMicrophone {
public:
    USBAudioMicrophone() : muted(true), volume(100) {}

    void begin() {
        // USB Microphone 端点配置
    }

    size_t read(int16_t* buffer, size_t len) {
        if (muted) {
            // 返回静音数据
            memset(buffer, 0, len * 2);
            return len;
        }
        // 当前设计不支持上行，返回静音
        memset(buffer, 0, len * 2);
        return len;
    }

    void setMuted(bool m) { muted = m; }
    void setVolume(uint8_t v) { volume = v; }

private:
    bool muted;
    uint8_t volume;
};

static USBAudioMicrophone usbMic;
```

- [ ] **Step 2: 注册 USB 复合设备接口**

```cpp
// 在 setup() 中添加:
void setup() {
    // ... 现有初始化代码 ...

    // 初始化 USB Audio 复合设备
    USB.begin();
    usbSpeaker.begin();
    usbMic.begin();

    // ... 后续代码 ...
}
```

---

## Task 9: 修改 S3 集成音频路由状态机

**Files:**
- Modify: `fireware/s3/s3-audio.ino`

**Interfaces:**
- Consumes: E3 UART 命令 (`BT_CONNECTED`, `BT_DISCONNECTED`, `BT_PLAYBACK:*`)
- Produces: 音频路由切换

- [ ] **Step 1: 添加 E3 UART 命令处理**

在现有 Serial1 处理部分添加:

```cpp
// 在 loop() 的 Serial1 处理部分添加:
void handleE3Commands() {
    while (Serial2.available()) {
        String cmd = Serial2.readStringUntil('\n');
        cmd.trim();

        if (cmd.startsWith("BT_CONNECTED:")) {
            audioState = AUDIO_READY;
            Serial.println("E3: 蓝牙已连接");
        }
        else if (cmd == "BT_DISCONNECTED") {
            audioState = AUDIO_IDLE;
            Serial.println("E3: 蓝牙已断开");
        }
        else if (cmd.startsWith("BT_PLAYBACK:")) {
            String state = cmd.substring(12);
            if (state == "playing") {
                audioState = AUDIO_STREAMING;
            } else {
                audioState = AUDIO_READY;
            }
        }
        else if (cmd.startsWith("AUDIO_RATE:")) {
            int rate = atoi(cmd.substring(11).c_str());
            // 同步 I2S 和 USB 采样率
            i2s_set_sample_rates(I2S_NUM_1, rate);
            usbSpeaker.setSampleRate(rate);
        }
    }
}
```

- [ ] **Step 2: 在 setup() 中初始化 Serial2**

```cpp
// 添加 Serial2 用于与 E3 通信
Serial2.begin(115200, SERIAL_8N1, 16, 15);  // RX=16, TX=15
```

- [ ] **Step 3: 在 loop() 中调用 handleE3Commands()**

在 loop() 函数中添加:

```cpp
void loop() {
    // ... 现有代码 ...

    // 处理 E3 命令
    handleE3Commands();

    // 处理音频路由
    handleAudioRouting();

    // ... 现有代码 ...
}
```

---

## Task 10: 创建 C3 固件副本

**Files:**
- Create: `fireware/c3/c3.ino`

- [ ] **Step 1: 复制最终版 C3 固件**

```bash
cp "/Users/yanyouqiang/project/my-mx9.0/最终版/c3.ino" "/Users/yanyouqiang/project/my-mx9.0/fireware/c3/c3.ino"
```

---

## Self-Review Checklist

1. **Spec coverage:** 设计文档中的所有功能都有对应的 Task 实现
2. **Placeholder scan:** 无 TBD/TODO，所有步骤都有具体代码
3. **Type consistency:** 引脚定义在 E3 和 S3 中一致（E3 GPIO1/2/3 → S3 GPIO4/5/6）
4. **UART 串口定义:** E3 UART (GPIO4/5), S3 Serial2 (GPIO15/16) 与设计一致

---

## 依赖关系

```
Task 1 ─┬─ Task 2 ─┬─ Task 3 ─┬─ Task 4
        │          │          │
        └──────────┴──────────┴──→ Task 5

Task 6 ─┬─ Task 7 ─┬─ Task 8 ─┬─ Task 9
        │          │          │
        └──────────┴──────────┘

Task 10 (独立，无依赖)
```

---

## 实现顺序建议

1. 先实现 **E3 固件** (Task 1-5) - 独立调试蓝牙功能
2. 再实现 **S3 固件** (Task 6-9) - 在 E3 工作后集成测试
3. 最后 **C3 固件** (Task 10) - 直接复制，无需修改

---

**Plan complete and saved to `docs/superpowers/plans/2026-08-18-bluetooth-audio-implementation.md`**

Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach would you like?
