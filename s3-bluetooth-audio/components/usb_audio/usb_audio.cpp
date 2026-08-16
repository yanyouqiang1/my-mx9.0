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
