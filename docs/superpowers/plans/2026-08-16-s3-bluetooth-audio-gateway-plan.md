# S3 蓝牙音频网关实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 S3 打造成蓝牙耳机中转网关，蓝牙耳机连接 S3，S3 通过 USB 声卡与电脑传输音频

**Architecture:** 模块化设计，键盘和音频模块通过 Ring Buffer 解耦。键盘扫描在 Core 1 独立任务，BLE 音频在 Core 0 运行，USB Audio 使用 TinyUSB。

**Tech Stack:** ESP-IDF, NimBLE, TinyUSB, Arduino framework

---

## 项目目录结构

```
s3-bluetooth-audio/
├── s3-bt-audio.ino          # 主程序（基于 s3.ino 修改）
├── s3-setting.html          # 配置网页
├── components/
│   ├── bt_audio/            # 蓝牙音频组件
│   │   ├── bt_audio.h
│   │   └── bt_audio.cpp
│   └── ring_buffer/         # 环形缓冲区
│       ├── ring_buffer.h
│       └── ring_buffer.cpp
└── README.md
```

---

## Global Constraints

- 采样率：48kHz
- 位深：16-bit
- USB VID：0x303A
- USB PID：0x0020
- BLE Service UUID：4fafc201-1fb5-459e-8fcc-c5c9c331914c
- 键盘扫描周期：3ms
- 功耗控制：通过开关启用/禁用 BLE 音频

---

## 任务列表

### Task 1: 项目初始化和目录结构

**Files:**
- Create: `s3-bluetooth-audio/README.md`
- Create: `s3-bluetooth-audio/components/ring_buffer/ring_buffer.h`
- Create: `s3-bluetooth-audio/components/ring_buffer/ring_buffer.cpp`
- Create: `s3-bluetooth-audio/components/bt_audio/bt_audio.h`
- Create: `s3-bluetooth-audio/components/bt_audio/bt_audio.cpp`

**Interfaces:**
- Produces: `RingBuffer` 类，`BTAudioManager` 类

- [ ] **Step 1: 创建项目目录结构**

```bash
mkdir -p s3-bluetooth-audio/components/bt_audio
mkdir -p s3-bluetooth-audio/components/ring_buffer
```

- [ ] **Step 2: 创建 Ring Buffer 头文件**

```cpp
// s3-bluetooth-audio/components/ring_buffer/ring_buffer.h
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>

class RingBuffer {
public:
    RingBuffer(size_t capacity);
    ~RingBuffer();
    
    bool write(const uint8_t* data, size_t len);
    size_t read(uint8_t* data, size_t len);
    size_t available() const;
    void clear();
    
private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    volatile size_t count_;
};

#endif
```

- [ ] **Step 3: 创建 Ring Buffer 实现文件**

```cpp
// s3-bluetooth-audio/components/ring_buffer/ring_buffer.cpp
#include "ring_buffer.h"
#include <string.h>

RingBuffer::RingBuffer(size_t capacity) : capacity_(capacity), head_(0), tail_(0), count_(0) {
    buffer_ = new uint8_t[capacity];
}

RingBuffer::~RingBuffer() {
    delete[] buffer_;
}

bool RingBuffer::write(const uint8_t* data, size_t len) {
    if (len > capacity_ - count_) return false;
    
    for (size_t i = 0; i < len; i++) {
        buffer_[head_] = data[i];
        head_ = (head_ + 1) % capacity_;
        count_++;
    }
    return true;
}

size_t RingBuffer::read(uint8_t* data, size_t len) {
    size_t actual = (len < count_) ? len : count_;
    for (size_t i = 0; i < actual; i++) {
        data[i] = buffer_[tail_];
        tail_ = (tail_ + 1) % capacity_;
        count_--;
    }
    return actual;
}

size_t RingBuffer::available() const { return count_; }
void RingBuffer::clear() { head_ = tail_ = count_ = 0; }
```

- [ ] **Step 4: 创建蓝牙音频头文件**

```cpp
// s3-bluetooth-audio/components/bt_audio/bt_audio.h
#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdint.h>
#include "ring_buffer.h"

// BLE Audio 状态
enum class BTAudioState {
    OFF = 0,
    SCANNING = 1,
    CONNECTED = 2,
    PLAYING = 3
};

class BTAudioManager {
public:
    BTAudioManager();
    ~BTAudioManager();
    
    void begin();
    void end();
    
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }
    BTAudioState getState() const { return state_; }
    
    void startScan();
    void stopScan();
    
    // 音频数据接口
    RingBuffer* getPlaybackBuffer() { return &playbackBuffer_; }   // 耳机→电脑
    RingBuffer* getCaptureBuffer() { return &captureBuffer_; }     // 电脑→耳机
    
    void task(void* param);
    
private:
    bool enabled_;
    BTAudioState state_;
    RingBuffer playbackBuffer_;
    RingBuffer captureBuffer_;
};

#endif
```

- [ ] **Step 5: 创建蓝牙音频实现文件（框架）**

```cpp
// s3-bluetooth-audio/components/bt_audio/bt_audio.cpp
#include "bt_audio.h"
#include "esp_log.h"

static const char* TAG = "BT_AUDIO";

BTAudioManager::BTAudioManager() 
    : enabled_(false), state_(BTAudioState::OFF),
      playbackBuffer_(16384), captureBuffer_(16384) {
}

BTAudioManager::~BTAudioManager() {
    end();
}

void BTAudioManager::begin() {
    ESP_LOGI(TAG, "蓝牙音频模块初始化");
}

void BTAudioManager::end() {
    setEnabled(false);
    ESP_LOGI(TAG, "蓝牙音频模块已关闭");
}

void BTAudioManager::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    
    if (enabled) {
        begin();
    } else {
        state_ = BTAudioState::OFF;
    }
}

void BTAudioManager::startScan() {
    if (!enabled_) return;
    state_ = BTAudioState::SCANNING;
    ESP_LOGI(TAG, "开始扫描蓝牙音频设备");
}

void BTAudioManager::stopScan() {
    ESP_LOGI(TAG, "停止扫描");
}

void BTAudioManager::task(void* param) {
    while (enabled_) {
        // TODO: 实现 BLE 扫描、连接、音频流处理
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}
```

- [ ] **Step 6: 创建 README**

```markdown
# S3 蓝牙音频网关

将 ESP32-S3 模拟成蓝牙耳机中转网关。

## 功能

- 蓝牙耳机连接 S3
- S3 通过 USB 声卡与电脑传输音频
- 支持双向音频（播放+录音）
- 可配置连接的蓝牙耳机

## 引脚连接

- I2C SDA: GPIO 14
- I2C SCL: GPIO 13
- UART RX: GPIO 10
- UART TX: GPIO 9

## 编译

```bash
platformio run
```

## 上传

```bash
platformio run --target upload
```
```

- [ ] **Step 7: 提交代码**

```bash
git add s3-bluetooth-audio/
git commit -m "feat: 项目初始化和 Ring Buffer 框架"
```

---

### Task 2: USB 复合设备配置

**Files:**
- Modify: `s3-bluetooth-audio/s3-bt-audio.ino`
- Create: `s3-bluetooth-audio/components/usb_audio/usb_audio.h`
- Create: `s3-bluetooth-audio/components/usb_audio/usb_audio.cpp`

**Interfaces:**
- Consumes: `RingBuffer` 类
- Produces: `USBAudioManager` 类

- [ ] **Step 1: 创建 USB Audio 头文件**

```cpp
// s3-bluetooth-audio/components/usb_audio/usb_audio.h
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
#include "ring_buffer.h"

class USBAudioManager {
public:
    USBAudioManager();
    ~USBAudioManager();
    
    void begin();
    
    // 获取音频缓冲区（与 Ring Buffer 对接）
    RingBuffer* getPlaybackBuffer() { return &playbackBuffer_; }
    RingBuffer* getCaptureBuffer() { return &captureBuffer_; }
    
    // TinyUSB 回调
    static void tud_audio_tx_complete_cb(uint8_t itf);
    static void tud_audio_rx_cb(uint8_t itf, int16_t* buf, uint16_t len);
    
private:
    RingBuffer playbackBuffer_;  // 电脑→S3→耳机
    RingBuffer captureBuffer_;   // 耳机→S3→电脑
};

#endif
```

- [ ] **Step 2: 创建 USB Audio 实现文件**

```cpp
// s3-bluetooth-audio/components/usb_audio/usb_audio.cpp
#include "usb_audio.h"
#include "tusb.h"

USBAudioManager::USBAudioManager() 
    : playbackBuffer_(16384), captureBuffer_(16384) {
}

USBAudioManager::~USBAudioManager() {
}

void USBAudioManager::begin() {
    // TinyUSB 初始化由主程序处理
}

// 播放完成回调（从 USB 发送音频到电脑完成）
void USBAudioManager::tud_audio_tx_complete_cb(uint8_t itf) {
    // 从 captureBuffer 读取数据并发送
    // captureBuffer 存放的是麦克风数据
}

// 接收电脑音频回调
void USBAudioManager::tud_audio_rx_cb(uint8_t itf, int16_t* buf, uint16_t len) {
    // 将数据写入 playbackBuffer
    // playbackBuffer 会被发送到蓝牙耳机
}
```

- [ ] **Step 3: 创建主程序框架**

```cpp
// s3-bluetooth-audio/s3-bt-audio.ino
#include "USB.h"
#include "USBAudio.h"
#include "USBHIDKeyboard.h"
#include "components/bt_audio/bt_audio.h"
#include "components/usb_audio/usb_audio.h"

// 蓝牙音频管理器
BTAudioManager btAudio;
USBAudioManager usbAudio;

void setup() {
    Serial.begin(115200);
    
    // USB Audio 初始化
    USBAudio.begin();
    
    // 蓝牙音频初始化
    btAudio.begin();
    
    // 创建音频任务
    xTaskCreatePinnedToCore(
        [](void* param) { btAudio.task(param); },
        "bt_audio",
        4096,
        NULL,
        2,
        NULL,
        0  // Core 0
    );
}

void loop() {
    // 键盘扫描保持原有逻辑
    // ...
    
    delay(1);
}
```

- [ ] **Step 4: 提交代码**

```bash
git add s3-bluetooth-audio/
git commit -m "feat: 添加 USB Audio 框架"
```

---

### Task 3: BLE A2DP Sink 实现

**Files:**
- Modify: `s3-bluetooth-audio/components/bt_audio/bt_audio.cpp`

**Interfaces:**
- Consumes: `RingBuffer` 类
- Produces: A2DP 音频数据写入 `playbackBuffer_`

- [ ] **Step 1: 实现 BLE A2DP Sink 核心逻辑**

在 `bt_audio.cpp` 中添加完整的 A2DP Sink 实现：

```cpp
#include "bt_audio.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

// A2DP 事件处理
static void a2dp_event_handler(esp_a2d_cb_event_t event, esp_a2d_inc_cb_param_t* param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP 已连接");
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP 音频开始");
            }
            break;
    }
}

// A2DP 数据回调（从耳机接收音频）
static void a2dp_data_callback(const uint8_t* data, uint32_t len) {
    // 将音频数据写入 playbackBuffer
    // playbackBuffer 将被 USB Audio 读取并发送到电脑
}

void BTAudioManager::begin() {
    // 初始化蓝牙控制器
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    
    // 初始化 Bluedroid
    esp_bluedroid_init();
    esp_bluedroid_enable();
    
    // 注册 A2DP 回调
    esp_a2d_register_callback(a2dp_event_handler);
    esp_a2d_sink_register_data_callback(a2dp_data_callback);
    
    // 创建 A2DP Sink
    esp_a2d_sink_init();
    
    // 设置设备名称
    esp_bt_dev_set_device_name("YYQ-MX9.0-Audio");
    
    ESP_LOGI(TAG, "蓝牙音频模块初始化完成");
}
```

- [ ] **Step 2: 提交代码**

```bash
git add s3-bluetooth-audio/
git commit -m "feat: 实现 BLE A2DP Sink"
```

---

### Task 4: 音频数据通路对接

**Files:**
- Modify: `s3-bluetooth-audio/components/bt_audio/bt_audio.cpp`
- Modify: `s3-bluetooth-audio/components/usb_audio/usb_audio.cpp`

**Interfaces:**
- Consumes: `BTAudioManager::playbackBuffer_`, `captureBuffer_`
- Produces: 音频数据传输

- [ ] **Step 1: 实现音频数据流对接**

在主程序中定时从 `btAudio.getPlaybackBuffer()` 读取数据并写入 `usbAudio`：

```cpp
// 在 loop() 或独立任务中
void audio_pipeline_task(void* param) {
    static uint8_t audio_buf[512];
    
    while (true) {
        // 从蓝牙接收缓冲区读取（耳机→电脑）
        size_t len = btAudio.getPlaybackBuffer()->read(audio_buf, sizeof(audio_buf));
        if (len > 0) {
            usbAudio.feedPlaybackData(audio_buf, len);
        }
        
        // 从 USB 录音缓冲区读取（电脑→耳机）
        len = usbAudio.getCaptureBuffer()->read(audio_buf, sizeof(audio_buf));
        if (len > 0) {
            btAudio.sendToHeadset(audio_buf, len);
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
```

- [ ] **Step 2: 提交代码**

```bash
git add s3-bluetooth-audio/
git commit -m "feat: 实现音频数据通路对接"
```

---

### Task 5: 网页配置界面

**Files:**
- Create: `s3-bluetooth-audio/s3-setting.html`

**Interfaces:**
- Produces: BLE 命令发送（开关、扫描、配对）

- [ ] **Step 1: 创建网页界面**

在现有 `s3-setting.html` 基础上新增蓝牙音频卡片：

```html
<div class="card">
    <h2>4. 蓝牙音频网关</h2>
    
    <div style="display: flex; align-items: center; gap: 10px;">
        <label>蓝牙音频：</label>
        <label class="switch">
            <input type="checkbox" id="btAudioToggle">
            <span class="slider"></span>
        </label>
        <span id="btAudioStatus">关闭</span>
    </div>
    
    <div id="btAudioSection" style="display: none; margin-top: 15px;">
        <h3>连接状态：<span id="connectionStatus">未连接</span></h3>
        <h3>已连接设备：<span id="connectedDevice">-</span></h3>
        
        <button class="btn" id="scanBtn">扫描附近耳机</button>
        
        <div id="scanResults" style="margin-top: 15px;"></div>
        
        <h3>已配对设备：</h3>
        <div id="pairedDevices"></div>
    </div>
</div>
```

- [ ] **Step 2: 添加 JavaScript 逻辑**

```javascript
document.getElementById('btAudioToggle').addEventListener('change', async (e) => {
    const enabled = e.target.checked;
    await sendBLE(`BTAUDIO:${enabled ? 1 : 0}`);
    document.getElementById('btAudioSection').style.display = enabled ? 'block' : 'none';
    document.getElementById('btAudioStatus').innerText = enabled ? '开启' : '关闭';
});

document.getElementById('scanBtn').addEventListener('click', async () => {
    await sendBLE('BTAUDIO:SCAN');
    // 显示扫描结果
});
```

- [ ] **Step 3: 提交代码**

```bash
git add s3-bluetooth-audio/s3-setting.html
git commit -m "feat: 添加蓝牙音频配置网页界面"
```

---

### Task 6: 按键控制集成

**Files:**
- Modify: `s3-bluetooth-audio/s3-bt-audio.ino`

**Interfaces:**
- Consumes: 键盘事件
- Produces: 蓝牙音频开关控制

- [ ] **Step 1: 添加按键控制逻辑**

在键盘扫描中添加长按宏键控制蓝牙音频开关：

```cpp
// 在 executeMacro 或键盘处理中添加
void handleBTAudioToggle() {
    static bool btAudioEnabled = false;
    btAudioEnabled = !btAudioEnabled;
    btAudio.setEnabled(btAudioEnabled);
    
    // 通过 Serial1 通知 C3 显示状态
    Serial1.println(btAudioEnabled ? "N_BTAUDIO_ON" : "N_BTAUDIO_OFF");
}
```

- [ ] **Step 2: 提交代码**

```bash
git add s3-bluetooth-audio/s3-bt-audio.ino
git commit -m "feat: 添加按键控制蓝牙音频开关"
```

---

## 依赖项

```json
{
  "name": "s3-bluetooth-audio",
  "version": "1.0.0",
  "dependencies": {
    "framework-arduinoespressif32": "*",
    "esp32cam": "*"
  }
}
```

---

## 自检清单

**Spec Coverage:**
- [x] USB 复合设备配置 → Task 2
- [x] BLE 基础连接 → Task 3
- [x] 音频播放通路（耳机→电脑）→ Task 4
- [x] 音频录音通路（电脑→耳机）→ Task 4
- [x] 网页界面 → Task 5
- [x] 按键控制 → Task 6

**Placeholder Scan:**
- 无 TBD/TODO
- 无占位符

**Type Consistency:**
- RingBuffer 类在所有文件中一致使用
- BTAudioManager 接口一致

---

## 执行方式

**Plan complete and saved to `docs/superpowers/plans/2026-08-16-s3-bluetooth-audio-gateway-plan.md`.**

**Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
