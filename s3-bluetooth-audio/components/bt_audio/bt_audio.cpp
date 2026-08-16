// s3-bluetooth-audio/components/bt_audio/bt_audio.cpp
#include "bt_audio.h"
#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"

static const char* TAG = "BT_AUDIO";

BTAudioManager::BTAudioManager()
    : enabled_(false), state_(BTAudioState::OFF),
      playbackBuffer_(16384), captureBuffer_(16384) {
}

BTAudioManager::~BTAudioManager() {
    end();
}

// A2DP 事件处理
static void a2dp_event_handler(esp_a2d_cb_event_t event, esp_a2d_inc_cb_param_t* param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            auto& conn_stat = param->conn_stat;
            if (conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP 已连接");
            } else if (conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP 已断开");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            auto& audio_stat = param->audio_stat;
            if (audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP 音频开始");
            } else if (audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "A2DP 音频停止");
            }
            break;
        }
        case ESP_A2D_PROF_STATE_EVT: {
            auto& prof_state = param->a2d_prof_state;
            if (prof_state.state == ESP_A2D_INIT_SUCCESS) {
                ESP_LOGI(TAG, "A2DP Sink 初始化成功");
            }
            break;
        }
        default:
            break;
    }
}

// A2DP 数据回调（从耳机接收音频，SBC 解码后为 PCM int16_t）
// 数据通过 usbAudio.playbackBuffer_ 传递给 USB 主机（电脑）
extern USBAudioManager* usbAudioPtr;

static void a2dp_data_callback(const uint8_t* data, uint32_t len) {
    if (usbAudioPtr == nullptr) return;
    RingBuffer* buf = usbAudioPtr->getPlaybackBuffer();
    if (buf == nullptr) return;
    // SBC PCM 数据为 int16_t，写入时按字节写入 RingBuffer（uint8_t）
    buf->write(data, len);
}

void BTAudioManager::begin() {
    ESP_LOGI(TAG, "蓝牙音频模块初始化");

    if (!btStarted()) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_bt_controller_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err));
            return;
        }
        err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BT 控制器使能失败: %s", esp_err_to_name(err));
            return;
        }
    }

    if (!bt.bluedroidIsEnabled()) {
        esp_err_t err = esp_bluedroid_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(err));
            return;
        }
        err = esp_bluedroid_enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid 使能失败: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_a2d_register_callback(a2dp_event_handler);
    esp_a2d_sink_register_data_callback(a2dp_data_callback);

    esp_a2d_sink_init();

    const char* device_name = "YYQ-MX9.0-Audio";
    esp_bt_dev_set_device_name(device_name);

    ESP_LOGI(TAG, "蓝牙音频模块初始化完成");
}

void BTAudioManager::end() {
    setEnabled(false);
    esp_a2d_sink_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
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
        // A2DP 连接和音频流由 ESP-IDF A2DP Sink 栈自动处理
        // 数据通过 a2dp_data_callback 写入 playbackBuffer
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}
