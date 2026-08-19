// firmware/e3/e3.ino - E3 蓝牙音频网关固件
// 功能: 蓝牙A2DP音频网关 + OLED屏幕 + 温湿度传感器 + 麦克风切换

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAudio.h>
#include <driver/i2s.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>

// ================= 引脚定义 =================
// I2S 音频接口 (连接到S3)
#define I2S_WS_PIN    2   // 字选择
#define I2S_BCK_PIN   3   // 位时钟
#define I2S_DOUT_PIN  1   // 播放数据输出 (到S3 DIN)
#define I2S_DIN_PIN   4   // 录音数据输入 (从INMP441或S3)

// UART 控制接口 (连接到S3)
#define UART_TX_PIN   5   // → S3 GPIO6
#define UART_RX_PIN   6   // ← S3 GPIO5

// I2C 接口 (OLED + SHT31共用)
#define I2C_SDA_PIN   7
#define I2C_SCL_PIN   8

#define LED_STATUS_PIN 10  // 蓝牙状态指示灯

// ================= 音频参数 =================
#define I2S_SAMPLE_RATE   48000
#define I2S_BUFFER_SIZE   512

// ================= 全局状态 =================
static bool btConnected = false;
static bool isPlaying = false;
static bool audioSourceLocal = false;  // false=蓝牙MIC, true=INMP441
static String deviceName = "YYQ-BT-Audio";

// A2DP 状态
static bool isStreaming = false;
static uint32_t btWriteIdx = 0;

// Task handles
static TaskHandle_t i2s_rx_task_handle = NULL;
static TaskHandle_t i2s_inmp441_task_handle = NULL;

// ================= OLED显示 =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct OledState {
    bool btConnected;
    bool isPlaying;
    bool audioSourceLocal;
    float temperature;
    float humidity;
    unsigned long lastUpdate;
};
OledState oledState = {false, false, false, 0, 0, 0};

// ================= SHT31传感器 =================
Adafruit_SHT31 sht31 = Adafruit_SHT31();
unsigned long lastSHT31Read = 0;
const unsigned long SHT31_READ_INTERVAL = 5000;  // 5秒

// ================= UART命令缓冲 =================
String uartBuffer = "";

// ================= LED状态 =================
unsigned long lastLedUpdate = 0;
int ledState = 0;  // 0=空闲, 1=配对中, 2=已连接, 3=播放中

// ================= 蓝牙回调函数声明 =================
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param);
static int32_t bt_i2s_write_data(const uint8_t *data, int32_t len);

// ================= 蓝牙回调函数实现 =================

// A2DP 栈事件回调
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param) {
    esp_a2d_cb_event_t a2d_event = (esp_a2d_cb_event_t)event;
    esp_a2d_cb_param_t *a2d_param = (esp_a2d_cb_param_t *)p_param;

    switch (a2d_event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            btConnected = a2d_param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            oledState.btConnected = btConnected;
            if (btConnected) {
                Serial1.println("BT_CONNECTED:YYQ-BT-Audio");
                digitalWrite(LED_STATUS_PIN, HIGH);
                Serial.println("A2DP 已连接");
            } else {
                Serial1.println("BT_DISCONNECTED");
                digitalWrite(LED_STATUS_PIN, LOW);
                isStreaming = false;
                Serial.println("A2DP 已断开");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            if (a2d_param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                isStreaming = true;
                Serial.println("A2DP 音频开始");
            } else if (a2d_param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                isStreaming = false;
                Serial.println("A2DP 音频停止");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            Serial.printf("A2DP 音频配置改变: 采样率已更新\n");
            break;
        }
        case ESP_A2D_PROF_STATE_EVT: {
            if (a2d_param->a2d_prof_state.state == ESP_A2D_INIT_SUCCESS) {
                Serial.println("A2DP 初始化成功");
            } else if (a2d_param->a2d_prof_state.state == ESP_A2D_DEINIT_SUCCESS) {
                Serial.println("A2DP 去初始化成功");
            }
            break;
        }
        default:
            Serial.printf("A2DP 事件: %d\n", a2d_event);
            break;
    }
}

// AVRCP 控制事件回调
static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param) {
    esp_avrc_ct_cb_event_t avrc_event = (esp_avrc_ct_cb_event_t)event;
    esp_avrc_ct_cb_param_t *avrc_param = (esp_avrc_ct_cb_param_t *)p_param;

    switch (avrc_event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            btConnected = avrc_param->conn_stat.connected;
            oledState.btConnected = btConnected;
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
            switch (avrc_param->play_stat.play_status) {
                case ESP_AVRC_PLAYBACK_PLAYING:
                    isPlaying = true;
                    oledState.isPlaying = true;
                    Serial1.println("BT_PLAYBACK:playing");
                    break;
                case ESP_AVRC_PLAYBACK_PAUSED:
                case ESP_AVRC_PLAYBACK_STOPPED:
                    isPlaying = false;
                    oledState.isPlaying = false;
                    Serial1.println("BT_PLAYBACK:paused");
                    break;
                default:
                    break;
            }
            break;
        }
        case ESP_AVRC_CT_AVRC_PLAYER_APP_SETTINGS_EVT: {
            Serial.println("AVRCP 播放器设置事件");
            break;
        }
        default:
            break;
    }
}

// A2DP 数据回调 - 写入 I2S
static int32_t bt_i2s_write_data(const uint8_t *data, int32_t len) {
    if (!btConnected) return 0;

    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, data, len, &bytesWritten, portMAX_DELAY);
    btWriteIdx += bytesWritten;

    return bytesWritten;
}

// ================= I2S初始化 (双向) =================
void initI2S() {
    // I2S TX配置 (播放到S3)
    i2s_config_t i2s_tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = I2S_SAMPLE_RATE,
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
    i2s_set_clk(I2S_NUM_0, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // I2S RX配置 (从S3接收音频)
    i2s_config_t i2s_rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
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
        .data_in_num = I2S_DIN_PIN
    };

    i2s_driver_install(I2S_NUM_1, &i2s_rx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &i2s_rx_pin_config);
    i2s_set_clk(I2S_NUM_1, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    // 启动I2S RX任务
    xTaskCreate(i2s_rx_task, "i2s_rx_task", 4096, NULL, 5, &i2s_rx_task_handle);

    // 启动INMP441麦克风任务
    xTaskCreate(i2s_inmp441_task, "i2s_inmp441_task", 4096, NULL, 5, &i2s_inmp441_task_handle);

    Serial.println("I2S初始化完成");
}

// I2S RX任务 - 从S3接收USB音频并发送到蓝牙耳机
void i2s_rx_task(void* param) {
    uint8_t i2s_rx_buffer[1024];
    while (1) {
        size_t bytes_read = 0;
        i2s_read(I2S_NUM_1, i2s_rx_buffer, sizeof(i2s_rx_buffer), &bytes_read, portMAX_DELAY);
        if (bytes_read > 0 && btConnected) {
            size_t bytes_written = 0;
            // 写入I2S0 TX，发送音频到蓝牙耳机
            i2s_write(I2S_NUM_0, i2s_rx_buffer, bytes_read, &bytes_written, portMAX_DELAY);
        }
    }
}

// INMP441麦克风任务 - 从本地麦克风录音并发送到S3
void i2s_inmp441_task(void* param) {
    uint8_t inmp441_buffer[1024];
    while (1) {
        if (audioSourceLocal) {
            size_t bytes_read = 0;
            // 从INMP441读取音频 (通过I2S1)
            i2s_read(I2S_NUM_1, inmp441_buffer, sizeof(inmp441_buffer), &bytes_read, portMAX_DELAY);
            if (bytes_read > 0) {
                // 发送到S3 (通过I2S0 DOUT)
                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, inmp441_buffer, bytes_read, &bytes_written, portMAX_DELAY);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));  // 节省CPU
        }
    }
}

// ================= 蓝牙初始化 =================
void initBluetooth() {
    esp_err_t err;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err) {
        Serial.printf("BT 内存释放失败: %s\n", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err) {
        Serial.printf("BT 控制器初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (err) {
        Serial.printf("BT 控制器使能失败: %s\n", esp_err_to_name(err));
        return;
    }

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

// ================= OLED初始化和显示 =================
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

// ================= SHT31初始化和读取 =================
void initSHT31() {
    if (!sht31.begin(0x44)) {  // SHT31默认地址0x44
        Serial.println("SHT31初始化失败");
        return;
    }
    Serial.println("SHT31初始化成功");
}

void handleSHT31() {
    if (millis() - lastSHT31Read < SHT31_READ_INTERVAL) return;
    lastSHT31Read = millis();

    oledState.temperature = sht31.readTemperature();
    oledState.humidity = sht31.readHumidity();

    Serial.printf("SHT31: Temp=%.1f C, Humi=%.1f %%\n", oledState.temperature, oledState.humidity);
}

// ================= 音频源切换 =================
void setAudioSource(bool local) {
    audioSourceLocal = local;
    oledState.audioSourceLocal = local;
    if (local) {
        Serial1.println("AUDIO_SRC_LOCAL_ACK");
        Serial.println("音频源切换: INMP441本地麦克风");
    } else {
        Serial1.println("AUDIO_SRC_BT_ACK");
        Serial.println("音频源切换: 蓝牙耳机麦克风");
    }
}

// ================= 蓝牙状态处理 =================
void handleBluetoothState() {
    unsigned long now = millis();

    if (!btConnected) {
        // 配对中 - 快速闪烁 (200ms间隔)
        if (now - lastLedUpdate > 200) {
            lastLedUpdate = now;
            ledState = 1;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else if (!isPlaying) {
        // 已连接但未播放 - 慢速闪烁 (1000ms间隔)
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

// ================= UART命令处理 =================
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
            uint8_t volume_u8 = (uint8_t)(volume * 127 / 100);
            esp_a2d_sink_set_abs_vol(volume_u8);
        }
    }
    else if (cmd == "AUDIO_SRC_LOCAL") {
        setAudioSource(true);
    }
    else if (cmd == "AUDIO_SRC_BT") {
        setAudioSource(false);
    }
}

// ================= setup和loop =================
void setup() {
    Serial.begin(115200);           // 调试串口
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);  // 与S3通信

    // 初始化I2C (OLED + SHT31)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // 初始化状态LED
    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);

    // 初始化I2S
    initI2S();

    // 初始化蓝牙
    initBluetooth();

    // 初始化OLED
    initOLED();

    // 初始化SHT31
    initSHT31();

    Serial.println("E3 初始化完成");
}

void loop() {
    handleBluetoothState();
    handleUartCommands();
    updateOLED();
    handleSHT31();
}
