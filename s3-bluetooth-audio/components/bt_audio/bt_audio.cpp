// ===== Audio Data Flow =====
// A2DP → USB TX path (headphone → computer):
//   a2dp_data_callback (A2DP ISR)
//     → usbAudioPtr->getPlaybackBuffer()->write()
//     → tud_audio_tx_complete_cb (TinyUSB task)
//     → USB TX to computer
//
// USB RX → I2S path (computer → headphone):
//   tud_audio_rx_cb (TinyUSB ISR)
//     → usbAudioPtr->getCaptureBuffer()->write()
//     → audioPipelineTask() (FreeRTOS task)
//     → AudioOutput.write() → I2S → DAC
//
// s3-bluetooth-audio/components/bt_audio/bt_audio.cpp
#include "bt_audio.h"
#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt_gap.h"
#include "esp_a2dp_api.h"
#include <string>
#include "sdkconfig.h"

static const char* TAG = "BT_AUDIO";

static BTAudioManager* btAudioMgrPtr = nullptr;
static bool scanResultsReported_ = false;

// BLE GAP 扫描回调
static void btgap_event_handler(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (btAudioMgrPtr == nullptr) return;

    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            esp_bt_gap_discovery_res_t* disc_res = &param->disc_res;
            BTDeviceInfo dev;
            char addr_str[18] = {0};
            sprintf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                disc_res->bda[0], disc_res->bda[1], disc_res->bda[2],
                disc_res->bda[3], disc_res->bda[4], disc_res->bda[5]);
            dev.address = addr_str;
            dev.rssi = disc_res->rssi;
            dev.name = "";

            // 提取设备名称
            for (int i = 0; i < disc_res->num_prop; i++) {
                if (disc_res->prop[i].type == ESP_BT_GAP_DEV_PROP_EIR) {
                    uint8_t* eir = (uint8_t*)disc_res->prop[i].val;
                    size_t eir_len = disc_res->prop[i].len;
                    // 解析 EIR 找设备名称 (type 0x09)
                    for (size_t j = 0; j < eir_len;) {
                        uint8_t eir_len_field = eir[j];
                        if (eir_len_field == 0 || j + eir_len_field + 1 > eir_len) break;
                        uint8_t eir_type = eir[j + 1];
                        if (eir_type == 0x09) { // Complete Local Name
                            dev.name = std::string((char*)&eir[j + 2], eir_len_field - 1);
                        }
                        j += eir_len_field + 1;
                    }
                } else if (disc_res->prop[i].type == ESP_BT_GAP_DEV_PROP_NAME) {
                    dev.name = std::string((char*)disc_res->prop[i].val);
                }
            }

            btAudioMgrPtr->scanResults_.push_back(dev);
            ESP_LOGI(TAG, "扫描到设备: %s [%s] RSSI: %d", dev.name.c_str(), dev.address.c_str(), dev.rssi);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_EVT: {
            if (param->disc_stt == ESP_BT_GAP_DISC_STATE_DISCOVERING) {
                ESP_LOGI(TAG, "BLE 扫描进行中...");
            } else if (param->disc_stt == ESP_BT_GAP_DISC_STATE_STOPPED) {
                ESP_LOGI(TAG, "BLE 扫描已停止，共发现 %d 台设备", (int)btAudioMgrPtr->scanResults_.size());
                btAudioMgrPtr->state_ = (btAudioMgrPtr->state_ == BTAudioState::SCANNING) ? BTAudioState::CONNECTED : btAudioMgrPtr->state_;
                scanResultsReported_ = true;
            }
            break;
        }
        default:
            break;
    }
}

BTAudioManager::BTAudioManager()
    : enabled_(false), state_(BTAudioState::OFF),
      playbackBuffer_(16384), captureBuffer_(16384) {
    btAudioMgrPtr = this;
}

BTAudioManager::~BTAudioManager() {
    btAudioMgrPtr = nullptr;
    end();
}

// A2DP 事件处理
static void a2dp_event_handler(esp_a2d_cb_event_t event, esp_a2d_inc_cb_param_t* param) {
    if (btAudioMgrPtr == nullptr) return;

    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            auto& conn_stat = param->conn_stat;
            if (conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP 已连接");
                btAudioMgrPtr->state_ = BTAudioState::CONNECTED;
            } else if (conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP 已断开");
                btAudioMgrPtr->state_ = BTAudioState::OFF;
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            auto& audio_stat = param->audio_stat;
            if (audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP 音频开始");
                btAudioMgrPtr->state_ = BTAudioState::PLAYING;
            } else if (audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "A2DP 音频停止");
                btAudioMgrPtr->state_ = BTAudioState::CONNECTED;
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

    esp_bt_gap_register_callback(btgap_event_handler);

    esp_a2d_sink_init();

    const char* device_name = "YYQ-MX9.0-Audio";
    esp_bt_dev_set_device_name(device_name);

    enabled_ = true;
    state_ = BTAudioState::INITIALIZING;

    ESP_LOGI(TAG, "蓝牙音频模块初始化完成");
}

void BTAudioManager::end() {
    if (state_ == BTAudioState::OFF) return;

    if (esp_a2d_sink_is_ready()) {
        esp_a2d_sink_deinit();
    }
    if (esp_bluedroid_is_enabled()) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
    }
    if (esp_bt_controller_is_enabled()) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }

    enabled_ = false;
    state_ = BTAudioState::OFF;
    ESP_LOGI(TAG, "蓝牙音频模块已关闭");
}

void BTAudioManager::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;

    if (enabled) {
        begin();
    } else {
        end();
    }
}

void BTAudioManager::startScan() {
    if (!enabled_) return;
    scanResults_.clear();
    scanResultsReported_ = false;
    state_ = BTAudioState::SCANNING;

    esp_bt_gap_scan_params_t scan_params = {
        .scan_type = BT_SCAN_TYPE_CONNECTABLE_DISCOVERABLE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter = ESP_BT_GAP_DISC_FILTER_BREDR,
        .scan_interval = 0x30,
        .scan_window = 0x30,
        .scan_duplicate = ESP_BT_GAP_SCAN_DUPLICATE_DISABLE
    };

    esp_err_t err = esp_bt_gap_start_discovery(BT_GAP_DISC_MODE_GENERAL, 100, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动扫描失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "开始扫描蓝牙音频设备");
    }
}

void BTAudioManager::stopScan() {
    esp_err_t err = esp_bt_gap_cancel_discovery();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "扫描已停止");
    } else {
        ESP_LOGE(TAG, "停止扫描失败: %s", esp_err_to_name(err));
    }
}

void BTAudioManager::connect(const String& addr) {
    if (!enabled_) return;
    ESP_LOGI(TAG, "正在连接: %s", addr.c_str());

    esp_bd_addr_t bda;
    std::sscanf(addr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]);

    esp_err_t err = esp_a2d_sink_connect(bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP 连接失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "A2DP 连接请求已发送");
    }
}

void BTAudioManager::disconnect() {
    ESP_LOGI(TAG, "断开 A2DP 连接");
    esp_bd_addr_t null_addr = {0};
    esp_a2d_sink_disconnect(null_addr);
}

void BTAudioManager::task(void* param) {
    while (enabled_) {
        // A2DP 连接和音频流由 ESP-IDF A2DP Sink 栈自动处理
        // 数据通过 a2dp_data_callback 写入 playbackBuffer
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}
