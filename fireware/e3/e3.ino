// fireware/e3/e3.ino - E3 蓝牙音频接收器固件
// 功能: 接收蓝牙A2DP/HFP音频，通过I2S输出到S3

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAudio.h>
#include <driver/i2s.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

// ================= 引脚定义 =================
#define I2S_WS_PIN    3
#define I2S_BCK_PIN   2
#define I2S_DATA_PIN  1

#define UART_TX_PIN   5   // → S3 GPIO16
#define UART_RX_PIN   4   // ← S3 GPIO15

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

// A2DP 状态
static bool isStreaming = false;
static uint32_t btWriteIdx = 0;

// 蓝牙回调函数声明
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
                    Serial1.println("BT_PLAYBACK:playing");
                    break;
                case ESP_AVRC_PLAYBACK_PAUSED:
                case ESP_AVRC_PLAYBACK_STOPPED:
                    isPlaying = false;
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

    // 将 BT 数据写入 I2S DMA 缓冲区
    // data 是 SBC 编码数据，ESP32 A2DP Sink 内部已解码为 PCM 数据
    size_t bytesWritten = 0;

    // 写入 I2S TX FIFO
    i2s_write(I2S_NUM_0, data, len, &bytesWritten, portMAX_DELAY);

    btWriteIdx += bytesWritten;

    return bytesWritten;
}

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

// ================= 存根函数 (后续任务实现) =================

void initI2S() {
    // 初始化 I2S Master TX 接口
    // 配置 WS=3, BCK=2, DATA=1
    // 设置采样率 48000
    i2s_config_t i2s_config = {
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

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

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

void handleBluetoothState() {
    // TODO: 处理蓝牙连接状态变化
    // - 更新 btConnected 状态
    // - 控制 LED_STATUS_PIN 指示灯
    // - 处理播放/暂停状态
}

void handleUartCommands() {
    // TODO: 处理来自 S3 的 UART 命令
    // 解析命令并执行相应操作
}

void sendAudioToI2S() {
    // TODO: 将接收到的蓝牙音频数据发送到 I2S
    // 从蓝牙缓冲区读取，写入 I2S TX
}
