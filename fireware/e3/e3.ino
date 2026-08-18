// fireware/e3/e3.ino - E3 蓝牙音频接收器固件
// 功能: 接收蓝牙A2DP/HFP音频，通过I2S输出到S3

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAudio.h>
#include <driver/i2s.h>

// ================= 引脚定义 =================
#define I2S_WS_PIN    3
#define I2S_BCK_PIN   2
#define I2S_DATA_PIN  1

#define UART_TX_PIN   5   // → S3 GPIO16
#define UART_RX_PIN   4   // ← S3 GPIO15

#define LED_STATUS_PIN 10  // 蓝牙状态指示灯

// ================= 音频参数 =================
#define I2S_SAMPLE_RATE   48000
#define I2S_BUFFER_SIZE   512

// ================= 全局状态 =================
static bool btConnected = false;
static bool isPlaying = false;
static String deviceName = "YYQ-BT-Audio";

// I2S 发送缓冲区
static int16_t i2sTxBuffer[I2S_BUFFER_SIZE * 2]; // 立体声

void setupE3() {
    // 初始化串口 (用于控制命令)
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    // 初始化状态 LED
    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);

    // 初始化 I2S (Master TX)
    initI2S();

    // 初始化蓝牙
    initBluetooth();

    Serial.println("E3 蓝牙音频接收器初始化完成");
}

void loopE3() {
    // 处理蓝牙状态
    handleBluetoothState();

    // 处理 UART 命令
    handleUartCommands();

    // 发送音频数据到 I2S
    sendAudioToI2S();
}

// ================= 存根函数 (后续任务实现) =================

void initI2S() {
    // TODO: 初始化 I2S Master TX 接口
    // 配置 WS=3, BCK=2, DATA=1
    // 设置采样率 48000
}

void initBluetooth() {
    // TODO: 初始化 BLE A2DP/HFP 蓝牙音频
    // 设置设备名称: YYQ-BT-Audio
}

void handleBluetoothState() {
    // TODO: 处理蓝牙连接状态变化
    // - 更新 btConnected 状态
    // - 控制 LED_STATUS_PIN 指示灯
    // - 处理播放/暂停状态
}

void handleUartCommands() {
    // TODO: 处理来自 S3 的 UART 命令
    // 解析命令并执行相应操作
}

void sendAudioToI2S() {
    // TODO: 将接收到的蓝牙音频数据发送到 I2S
    // 从蓝牙缓冲区读取，写入 I2S TX
}
