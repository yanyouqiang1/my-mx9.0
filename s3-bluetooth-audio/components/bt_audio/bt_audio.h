// s3-bluetooth-audio/components/bt_audio/bt_audio.h
#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdint.h>
#include "ring_buffer.h"

// Forward declaration so the A2DP data callback can access the USB Audio buffer
class USBAudioManager;
extern USBAudioManager* usbAudioPtr;

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
