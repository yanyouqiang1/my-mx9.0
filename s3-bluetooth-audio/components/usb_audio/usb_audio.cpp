// s3-bluetooth-audio/components/usb_audio/usb_audio.cpp
#include "usb_audio.h"
#include "tusb.h"

USBAudioManager* usbAudioPtr = nullptr;

USBAudioManager::USBAudioManager()
    : playbackBuffer_(16384), captureBuffer_(16384) {
    usbAudioPtr = this;
}

USBAudioManager::~USBAudioManager() {
    usbAudioPtr = nullptr;
}

void USBAudioManager::begin() {
    // TinyUSB 初始化由主程序处理
}

// 播放完成回调（TX 完成，需要填充下一批数据）
// playbackBuffer 存放的是从 A2DP 接收的 PCM 音频（耳机→电脑方向）
void USBAudioManager::tud_audio_tx_complete_cb(uint8_t itf) {
    if (usbAudioPtr == nullptr) return;

    RingBuffer* buf = usbAudioPtr->getPlaybackBuffer();
    if (buf == nullptr) return;

    // 每包 192 个 int16_t 样本（1 帧 = 192 个采样点，对应 48kHz/1ms）
    // 需要字节数 = 192 样本 * 2 字节 = 384 字节
    constexpr size_t SAMPLES_PER_FRAME = 192;
    constexpr size_t BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2;

    uint8_t raw[BYTES_PER_FRAME];
    size_t got = buf->read(raw, BYTES_PER_FRAME);

    if (got == 0) {
        // 缓冲区为空，填充静音
        memset(raw, 0, BYTES_PER_FRAME);
        tud_audio_write(raw, BYTES_PER_FRAME);
    } else if (got < BYTES_PER_FRAME) {
        // 部分数据，补零至满帧
        memset(raw + got, 0, BYTES_PER_FRAME - got);
        tud_audio_write(raw, BYTES_PER_FRAME);
    } else {
        tud_audio_write(raw, BYTES_PER_FRAME);
    }
}

// 接收电脑音频回调（RX 收到 USB 音频数据）
// captureBuffer 存放的是电脑发送的音频数据（电脑→耳机方向）
void USBAudioManager::tud_audio_rx_cb(uint8_t itf, int16_t* buf, uint16_t len) {
    if (usbAudioPtr == nullptr) return;

    // 将 int16_t PCM 数据（len 个样本）写入 captureBuffer
    // captureBuffer 为 uint8_t，按字节存储
    usbAudioPtr->getCaptureBuffer()->write(reinterpret_cast<uint8_t*>(buf), len * 2);
}
