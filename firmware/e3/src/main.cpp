#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>
#include "BluetoothA2DPSource.h" // 经典蓝牙音频推流(主控连接蓝牙耳机)
#include "driver/i2s.h"

// ================= 引脚与外设定义 =================
// OLED 与 SHT31 的 I2C 引脚
#define I2C_SDA_PIN      8
#define I2C_SCL_PIN      9
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define OLED_RESET       -1

// E3 与 S3 之间的 I2S 音频传输引脚
#define I2S_BCK_PIN      4   // 位时钟 BCLK
#define I2S_WS_PIN       5   // 声道选择 LRCK / WS
#define I2S_DATA_IN_PIN  6   // 音频输入 (从S3接收声音)
#define I2S_DATA_OUT_PIN 7   // 音频输出 (传给S3声卡)

// WiFi 与 OTA 配置
const char* WIFI_SSID     = "Your_WiFi_SSID";
const char* WIFI_PASSWORD = "Your_WiFi_Password";
const char* OTA_HOSTNAME  = "MX9-E3-AudioGateway";

// 目标蓝牙耳机名称 (设置为空字符串 "" 则自动连接搜索到的第一个蓝牙耳机)
const char* TARGET_HEADPHONE_NAME = ""; 

// ================= 实例化外设 =================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
BluetoothA2DPSource a2dp_source;

// 系统全局状态
bool isBtConnected = false;
String connectedDevName = "Scanning...";
float currentTemp = 0.0;
float currentHumidity = 0.0;
unsigned long lastSensorRead = 0;
unsigned long lastOledRefresh = 0;

// ================= I2S 硬件驱动初始化 =================
void initI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = true,
        .tx_desc_auto_clear = true
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num = I2S_DATA_IN_PIN
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    Serial.println("[I2S] 驱动初始化完成 (44.1kHz/16bit 双声道)");
}

// ================= 蓝牙音频数据回调 (推流至耳机) =================
// 每次蓝牙协议栈请求音频包时，从 I2S 读取 S3 产生的声音流并送入蓝牙耳机
int32_t get_audio_data(Frame* frame, int32_t frame_count) {
    size_t bytes_read = 0;
    size_t bytes_to_read = frame_count * sizeof(Frame);
    
    // 从 I2S_NUM_0 实时读取来自 S3 的 PCM 数据
    esp_err_t res = i2s_read(I2S_NUM_0, (void*)frame, bytes_to_read, &bytes_read, 10 / portTICK_PERIOD_MS);
    
    if (res == ESP_OK && bytes_read > 0) {
        return bytes_read / sizeof(Frame);
    }
    
    // 如果无音频输入，静音填充
    memset(frame, 0, bytes_to_read);
    return frame_count;
}

// ================= 蓝牙状态与连接回调 =================
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        isBtConnected = true;
        Serial.println("[BT] 蓝牙耳机已连接成功！");
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        isBtConnected = false;
        connectedDevName = "Scanning...";
        Serial.println("[BT] 蓝牙耳机断开连接，重新开始搜索...");
    }
}

// 自动扫描与发现设备回调
bool ssid_callback(const char* ssid, esp_bd_addr_t address, int rssi) {
    Serial.printf("[BT Scan] 发现设备: %s, 信号强度: %d dBm\n", ssid, rssi);
    
    if (strlen(TARGET_HEADPHONE_NAME) > 0) {
        // 连接指定名称的耳机
        if (strcmp(ssid, TARGET_HEADPHONE_NAME) == 0) {
            connectedDevName = String(ssid);
            return true;
        }
        return false;
    } else {
        // 如果未指定名称，自动尝试配对首个有效音频设备
        if (strlen(ssid) > 0) {
            connectedDevName = String(ssid);
            return true;
        }
        return false;
    }
}

// ================= OLED 显示更新 =================
void updateDisplay() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // 标题栏
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("MX9.0 AUDIO GW");
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    // 蓝牙状态
    display.setCursor(0, 15);
    display.print("BT: ");
    if (isBtConnected) {
        display.print("Connected");
        display.setCursor(0, 26);
        display.printf("Dev: %s", connectedDevName.c_str());
    } else {
        display.print("Scanning...");
    }

    // 温湿度显示 (SHT31)
    display.setCursor(0, 42);
    display.printf("Temp: %.1f C", currentTemp);
    display.setCursor(0, 54);
    display.printf("Humi: %.1f %%", currentHumidity);

    display.display();
}

// ================= WiFi 与 OTA 初始化 =================
void initWiFiAndOTA() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("[OTA] 开始更新: " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] 更新完成，系统重启中...");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] 错误[%u]\n", error);
    });
    ArduinoOTA.begin();
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- MX9.0 E3 蓝牙音频网关启动 ---");

    // 1. 初始化 I2C (OLED + SHT31)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    
    if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(10, 25);
        display.println("MX9.0 Starting...");
        display.display();
    }

    if (!sht31.begin(0x44)) {
        Serial.println("[SHT31] 未找到传感器，请检查接线！");
    }

    // 2. 初始化 I2S 音频接口
    initI2S();

    // 3. 初始化 WiFi 与 OTA
    initWiFiAndOTA();

    // 4. 初始化蓝牙 A2DP Source 发送端（主动扫描并连接耳机）
    a2dp_source.set_auto_reconnect(true);
    a2dp_source.set_on_connection_state_changed(connection_state_changed);
    a2dp_source.set_ssid_callback(ssid_callback);
    a2dp_source.start(get_audio_data);
    Serial.println("[BT] A2DP 主机推流协议栈已就绪，正在搜索附近的蓝牙耳机...");
}

// ================= LOOP =================
void loop() {
    // 处理 WiFi OTA 任务
    ArduinoOTA.handle();

    unsigned long now = millis();

    // 每 2 秒读取一次 SHT31 温湿度
    if (now - lastSensorRead > 2000) {
        lastSensorRead = now;
        currentTemp = sht31.readTemperature();
        currentHumidity = sht31.readHumidity();
    }

    // 每 200 毫秒刷新一次 OLED
    if (now - lastOledRefresh > 200) {
        lastOledRefresh = now;
        updateDisplay();
    }
}