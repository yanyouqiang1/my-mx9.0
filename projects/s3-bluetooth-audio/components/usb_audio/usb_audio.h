// s3-bluetooth-audio/components/usb_audio/usb_audio.h
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
#include <atomic>
#include "ring_buffer.h"

class USBAudioManager {
public:
    USBAudioManager();
    ~USBAudioManager();

    void begin();

    // 获取音频缓冲区（与 Ring Buffer 对接）
    RingBuffer* getPlaybackBuffer() { return &playbackBuffer_; }
    RingBuffer* getCaptureBuffer() { return &captureBuffer_; }

    bool isDying() const { return dying_.load(std::memory_order_acquire); }

    // TinyUSB 回调
    static void tud_audio_tx_complete_cb(uint8_t itf);
    static void tud_audio_rx_cb(uint8_t itf, int16_t* buf, uint16_t len);

private:
    std::atomic<bool> dying_{false};
    RingBuffer playbackBuffer_;  // 电脑→S3→耳机
    RingBuffer captureBuffer_;   // 耳机→S3→电脑
};

#endif
