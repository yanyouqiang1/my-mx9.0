// s3-bluetooth-audio/components/bt_audio/bt_audio.h
#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdint.h>
#include <string>
#include <vector>
#include "ring_buffer.h"

// Forward declaration so the A2DP data callback can access the USB Audio buffer
class USBAudioManager;
extern USBAudioManager* usbAudioPtr;

struct BTDeviceInfo {
    std::string name;
    std::string address;
    int rssi;
};

// BLE Audio 状态
enum class BTAudioState {
    OFF = 0,
    INITIALIZING = 1,
    SCANNING = 2,
    CONNECTED = 3,
    PLAYING = 4
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
    void connect(const String& addr);
    void disconnect();

    // 获取扫描到的设备列表
    const std::vector<BTDeviceInfo>& getScanResults() const { return scanResults_; }

    // 音频数据接口
    RingBuffer* getPlaybackBuffer() { return &playbackBuffer_; }   // 耳机→电脑
    RingBuffer* getCaptureBuffer() { return &captureBuffer_; }     // 电脑→耳机

    void task(void* param);

private:
    bool enabled_;
    BTAudioState state_;
    RingBuffer playbackBuffer_;
    RingBuffer captureBuffer_;
    std::vector<BTDeviceInfo> scanResults_;
};

#endif
