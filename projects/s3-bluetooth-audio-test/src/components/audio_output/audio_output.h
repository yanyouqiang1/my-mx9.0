// s3-bluetooth-audio/components/audio_output/audio_output.h
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

// I2S 引脚定义（ESP32-S3 可用引脚，不与 MX9.0 键盘引脚冲突）
// BCK: GPIO 18, WS: GPIO 19, SDOUT: GPIO 46
#define I2S_BCK_PIN   18
#define I2S_WS_PIN    19
#define I2S_SDOUT_PIN 46

// 音频参数（与 USB Audio Class 48kHz stereo int16_t 对齐）
#define AUDIO_SAMPLE_RATE        48000
#define AUDIO_CHANNELS           2        // stereo
#define AUDIO_BITS_PER_SAMPLE    16
#define AUDIO_BYTES_PER_SAMPLE   4        // stereo int16_t = 4 bytes/sample
#define AUDIO_FRAME_BYTES        (AUDIO_SAMPLE_RATE / 1000 * AUDIO_BYTES_PER_SAMPLE)  // 384 bytes @ 1ms

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // 初始化 I2S 总线并使能 TX 通道
    bool begin();

    // 写入 PCM 数据到 I2S DMA（len 应为 AUDIO_FRAME_BYTES 的整数倍）
    // 返回实际写入字节数，非阻塞
    size_t write(const uint8_t* data, size_t len);

    // 填充静音数据（USB 音频暂停时使用）
    size_t writeSilence(size_t len);

    bool isReady() const { return ready_; }

    // 获取 I2S 通道句柄（供音频管线任务使用）
    i2s_chan_handle_t getHandle() const { return tx_handle_; }

private:
    i2s_chan_handle_t tx_handle_ = nullptr;
    bool ready_ = false;
};

#endif  // AUDIO_OUTPUT_H
