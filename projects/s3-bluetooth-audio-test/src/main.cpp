/*
 * S3 蓝牙音频网关测试版本
 *
 * 功能：
 * - 接收蓝牙耳机音频，通过 USB 发送到电脑（耳机 -> 电脑）
 * - 接收电脑音频，通过 I2S 发送到外接 DAC（电脑 -> 耳机/音箱）
 *
 * 硬件要求：
 * - ESP32-S3
 * - I2S DAC (如 PCM5102) 连接到 GPIO 18/19/46
 *
 * 测试步骤：
 * 1. 编译上传
 * 2. 打开串口监视器 (115200 baud)
 * 3. 用手机搜索 "YYQ-MX9.0-Audio" 蓝牙设备
 * 4. 连接后播放音乐
 * 5. 电脑应该能看到 USB 声卡设备并接收音频
 */

#include <Arduino.h>
#include "components/bt_audio/bt_audio.h"
#include "components/usb_audio/usb_audio.h"
#include "components/audio_output/audio_output.h"

// 全局实例
BTAudioManager btAudio;
USBAudioManager usbAudio;
AudioOutput audioOutput;

// 音频管线任务
void audioPipelineTask(void* param) {
    static uint8_t audio_buf[512];

    Serial.println("[Pipeline] 音频管线任务启动");

    while (true) {
        // 从 USB 录音缓冲区读取（电脑 -> 耳机方向）
        size_t len = usbAudio.getCaptureBuffer()->read(audio_buf, sizeof(audio_buf));
        if (len > 0) {
            audioOutput.write(audio_buf, len);
        } else {
            // 缓冲区空，填充静音
            audioOutput.writeSilence(384);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("===========================================");
    Serial.println("  S3 蓝牙音频网关 测试版本");
    Serial.println("===========================================");
    Serial.println();

    // 初始化 USB Audio
    Serial.println("[Setup] 初始化 USB Audio...");
    usbAudio.begin();

    // 初始化 I2S 音频输出
    Serial.println("[Setup] 初始化 I2S 音频输出...");
    if (audioOutput.begin()) {
        Serial.println("[Setup] I2S 初始化成功 (GPIO 18/19/46)");
    } else {
        Serial.println("[Setup] I2S 初始化失败 - 检查接线或跳过");
    }

    // 初始化蓝牙音频
    Serial.println("[Setup] 初始化蓝牙音频...");
    btAudio.begin();

    // 创建音频管线任务
    Serial.println("[Setup] 创建音频管线任务...");
    xTaskCreatePinnedToCore(
        audioPipelineTask,
        "audio_pipeline",
        4096,
        NULL,
        3,
        NULL,
        1  // Core 1
    );

    // 创建蓝牙音频任务
    Serial.println("[Setup] 创建蓝牙音频任务...");
    xTaskCreatePinnedToCore(
        [](void* param) { btAudio.task(param); },
        "bt_audio",
        4096,
        NULL,
        2,
        NULL,
        0  // Core 0
    );

    Serial.println();
    Serial.println("===========================================");
    Serial.println("  初始化完成！");
    Serial.println("===========================================");
    Serial.println();
    Serial.println("操作说明：");
    Serial.println("1. 手机搜索蓝牙设备 'YYQ-MX9.0-Audio'");
    Serial.println("2. 连接后播放音乐");
    Serial.println("3. 电脑应能看到 USB 声卡设备");
    Serial.println("4. I2S DAC 输出音频到耳机/音箱");
    Serial.println();
}

void loop() {
    // 打印状态信息
    static unsigned long lastStatusPrint = 0;
    if (millis() - lastStatusPrint > 5000) {
        lastStatusPrint = millis();

        BTAudioState state = btAudio.getState();
        String stateStr;
        switch (state) {
            case BTAudioState::OFF: stateStr = "关闭"; break;
            case BTAudioState::INITIALIZING: stateStr = "初始化中"; break;
            case BTAudioState::SCANNING: stateStr = "扫描中"; break;
            case BTAudioState::CONNECTED: stateStr = "已连接"; break;
            case BTAudioState::PLAYING: stateStr = "播放中"; break;
            default: stateStr = "未知";
        }

        Serial.printf("[Status] BT状态: %s | USB播放缓冲: %d bytes | USB录音缓冲: %d bytes\n",
            stateStr.c_str(),
            usbAudio.getPlaybackBuffer()->available(),
            usbAudio.getCaptureBuffer()->available()
        );
    }

    delay(100);
}
