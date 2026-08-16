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
