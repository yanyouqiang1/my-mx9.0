#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAudio.h>
#include <driver/i2s.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>

#include "wifi_manager.h"
#include "web_server.h"

WiFiManagerClass WiFiManager;
WebServerClass WebServer;

// ================= 引脚定义 =================
#define I2S_WS_PIN    2
#define I2S_BCK_PIN   3
#define I2S_DOUT_PIN  1
#define I2S_DIN_PIN   4

#define UART_TX_PIN   5
#define UART_RX_PIN   6

#define I2C_SDA_PIN   7
#define I2C_SCL_PIN   8

#define LED_STATUS_PIN 10

#define I2S_SAMPLE_RATE   48000

// ================= 全局状态 =================
static bool btConnected = false;
static bool isPlaying = false;
static bool audioSourceLocal = false;
static String deviceName = "YYQ-BT-Audio";
static bool isStreaming = false;

static TaskHandle_t i2s_rx_task_handle = NULL;
static TaskHandle_t i2s_inmp441_task_handle = NULL;

// ================= OLED显示 =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct OledState {
    bool btConnected;
    bool isPlaying;
    bool audioSourceLocal;
    float temperature;
    float humidity;
    unsigned long lastUpdate;
};
OledState oledState = {false, false, false, 0, 0, 0};

// ================= SHT31传感器 =================
Adafruit_SHT31 sht31 = Adafruit_SHT31();
unsigned long lastSHT31Read = 0;
const unsigned long SHT31_READ_INTERVAL = 5000;

String uartBuffer = "";

unsigned long lastLedUpdate = 0;
int ledState = 0;

// ================= 蓝牙回调函数 =================
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param);
static int32_t bt_i2s_write_data(const uint8_t *data, int32_t len);

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param) {
    esp_a2d_cb_event_t a2d_event = (esp_a2d_cb_event_t)event;
    esp_a2d_cb_param_t *a2d_param = (esp_a2d_cb_param_t *)p_param;

    switch (a2d_event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            btConnected = a2d_param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            oledState.btConnected = btConnected;
            if (btConnected) {
                Serial1.println("BT_CONNECTED:YYQ-BT-Audio");
                digitalWrite(LED_STATUS_PIN, HIGH);
                Serial.println("A2DP 已连接");
            } else {
                Serial1.println("BT_DISCONNECTED");
                digitalWrite(LED_STATUS_PIN, LOW);
                isStreaming = false;
                Serial.println("A2DP 已断开");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            if (a2d_param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                isStreaming = true;
                Serial.println("A2DP 音频开始");
            } else if (a2d_param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                isStreaming = false;
                Serial.println("A2DP 音频停止");
            }
            break;
        }
        default:
            Serial.printf("A2DP 事件: %d\n", a2d_event);
            break;
    }
}

static void bt_av_hdl_avrc_evt(uint16_t event, void *p_param) {
    esp_avrc_ct_cb_event_t avrc_event = (esp_avrc_ct_cb_event_t)event;
    esp_avrc_ct_cb_param_t *avrc_param = (esp_avrc_ct_cb_param_t *)p_param;

    switch (avrc_event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            btConnected = avrc_param->conn_stat.connected;
            oledState.btConnected = btConnected;
            if (btConnected) {
                Serial1.println("BT_CONNECTED:YYQ-BT-Audio");
                digitalWrite(LED_STATUS_PIN, HIGH);
            } else {
                Serial1.println("BT_DISCONNECTED");
                digitalWrite(LED_STATUS_PIN, LOW);
                isStreaming = false;
            }
            break;
        }
        case ESP_AVRC_CT_PLAY_STATE_RC_EVT: {
            switch (avrc_param->play_stat.play_status) {
                case ESP_AVRC_PLAYBACK_PLAYING:
                    isPlaying = true;
                    oledState.isPlaying = true;
                    Serial1.println("BT_PLAYBACK:playing");
                    break;
                case ESP_AVRC_PLAYBACK_PAUSED:
                case ESP_AVRC_PLAYBACK_STOPPED:
                    isPlaying = false;
                    oledState.isPlaying = false;
                    Serial1.println("BT_PLAYBACK:paused");
                    break;
            }
            break;
        }
        default:
            break;
    }
}

static int32_t bt_i2s_write_data(const uint8_t *data, int32_t len) {
    if (!btConnected) return 0;
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, data, len, &bytesWritten, portMAX_DELAY);
    return bytesWritten;
}

// ================= I2S初始化 =================
void initI2S() {
    i2s_config_t i2s_tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_tx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_tx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &i2s_tx_pin_config);
    i2s_set_clk(I2S_NUM_0, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    i2s_config_t i2s_rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_rx_pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DIN_PIN
    };

    i2s_driver_install(I2S_NUM_1, &i2s_rx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &i2s_rx_pin_config);
    i2s_set_clk(I2S_NUM_1, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    xTaskCreate(i2s_rx_task, "i2s_rx_task", 4096, NULL, 5, &i2s_rx_task_handle);
    xTaskCreate(i2s_inmp441_task, "i2s_inmp441_task", 4096, NULL, 5, &i2s_inmp441_task_handle);

    Serial.println("I2S初始化完成");
}

void i2s_rx_task(void* param) {
    uint8_t i2s_rx_buffer[1024];
    while (1) {
        size_t bytes_read = 0;
        i2s_read(I2S_NUM_1, i2s_rx_buffer, sizeof(i2s_rx_buffer), &bytes_read, portMAX_DELAY);
        if (bytes_read > 0 && btConnected) {
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, i2s_rx_buffer, bytes_read, &bytes_written, portMAX_DELAY);
        }
    }
}

void i2s_inmp441_task(void* param) {
    uint8_t inmp441_buffer[1024];
    while (1) {
        if (audioSourceLocal) {
            size_t bytes_read = 0;
            i2s_read(I2S_NUM_1, inmp441_buffer, sizeof(inmp441_buffer), &bytes_read, portMAX_DELAY);
            if (bytes_read > 0) {
                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, inmp441_buffer, bytes_read, &bytes_written, portMAX_DELAY);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ================= 蓝牙初始化 =================
void initBluetooth() {
    esp_err_t err;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err) {
        Serial.printf("BT 内存释放失败: %s\n", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err) {
        Serial.printf("BT 控制器初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (err) {
        Serial.printf("BT 控制器使能失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bluedroid_init();
    if (err) {
        Serial.printf("Bluedroid 初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    err = esp_bluedroid_enable();
    if (err) {
        Serial.printf("Bluedroid 使能失败: %s\n", esp_err_to_name(err));
        return;
    }

    esp_bt_dev_set_device_name(deviceName.c_str());

    esp_a2d_register_callback(bt_av_hdl_stack_evt);
    esp_a2d_sink_register_data_callback(bt_i2s_write_data);
    esp_a2d_sink_init();

    esp_avrc_ct_register_callback(bt_av_hdl_avrc_evt);
    esp_avrc_ct_init();

    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);

    Serial.println("蓝牙初始化完成，等待连接...");
}

// ================= OLED =================
void initOLED() {
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 OLED初始化失败");
        return;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.display();
    Serial.println("OLED初始化成功");
}

void updateOLED() {
    if (millis() - oledState.lastUpdate < 1000) return;
    oledState.lastUpdate = millis();

    display.clearDisplay();
    display.setCursor(0, 0);

    display.print("BT: ");
    display.println(oledState.btConnected ? "Connected" : "Disconnected");

    display.print("Status: ");
    display.println(oledState.isPlaying ? "Playing" : "Paused");

    display.print("MIC: ");
    display.println(oledState.audioSourceLocal ? "Local (INMP441)" : "Bluetooth");

    display.printf("Temp: %.1f C\n", oledState.temperature);
    display.printf("Humi: %.1f %%", oledState.humidity);

    display.display();
}

// ================= SHT31 =================
void initSHT31() {
    if (!sht31.begin(0x44)) {
        Serial.println("SHT31初始化失败");
        return;
    }
    Serial.println("SHT31初始化成功");
}

void handleSHT31() {
    if (millis() - lastSHT31Read < SHT31_READ_INTERVAL) return;
    lastSHT31Read = millis();

    oledState.temperature = sht31.readTemperature();
    oledState.humidity = sht31.readHumidity();

    Serial.printf("SHT31: Temp=%.1f C, Humi=%.1f %%\n", oledState.temperature, oledState.humidity);
}

// ================= 音频源切换 =================
void setAudioSource(bool local) {
    audioSourceLocal = local;
    oledState.audioSourceLocal = local;
    if (local) {
        Serial1.println("AUDIO_SRC_LOCAL_ACK");
        Serial.println("音频源切换: INMP441本地麦克风");
    } else {
        Serial1.println("AUDIO_SRC_BT_ACK");
        Serial.println("音频源切换: 蓝牙耳机麦克风");
    }
}

// ================= LED状态 =================
void handleBluetoothState() {
    unsigned long now = millis();

    if (!btConnected) {
        if (now - lastLedUpdate > 200) {
            lastLedUpdate = now;
            ledState = 1;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else if (!isPlaying) {
        if (now - lastLedUpdate > 1000) {
            lastLedUpdate = now;
            ledState = 2;
            digitalWrite(LED_STATUS_PIN, !digitalRead(LED_STATUS_PIN));
        }
    } else {
        ledState = 3;
        digitalWrite(LED_STATUS_PIN, HIGH);
    }
}

// ================= UART命令 =================
void handleUartCommands() {
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n') {
            uartBuffer.trim();
            processCommand(uartBuffer);
            uartBuffer = "";
        } else if (c != '\r') {
            uartBuffer += c;
        }
    }
}

void processCommand(String cmd) {
    if (cmd.startsWith("CTRL_")) {
        String action = cmd.substring(5);

        if (action == "PLAYPAUSE") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action == "NEXT") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action == "PREV") {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (action.startsWith("VOLUME:")) {
            int volume = atoi(action.substring(7).c_str());
            uint8_t volume_u8 = (uint8_t)(volume * 127 / 100);
            esp_a2d_sink_set_abs_vol(volume_u8);
        }
    }
    else if (cmd == "AUDIO_SRC_LOCAL") {
        setAudioSource(true);
    }
    else if (cmd == "AUDIO_SRC_BT") {
        setAudioSource(false);
    }
}

// ================= WiFi-OTA 智能模式 =================
const unsigned long WIFI_TIMEOUT_MS = 30000;
const unsigned long WIFI_RECHECK_MS = 5000;
bool wifiEnabled = true;
bool wifiWasConnected = false;
unsigned long bootTime = 0;

void disableWiFi() {
    if (!wifiEnabled) return;

    Serial.println("No connection in 30s, disabling WiFi to save power...");
    WebServer.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiEnabled = false;
    Serial.println("WiFi disabled.");
}

void checkWiFiConnection() {
    if (!wifiEnabled) return;

    wifi_sta_list_t stationList;
    memset(&stationList, 0, sizeof(wifi_sta_list_t));
    esp_wifi_ap_get_sta_list(&stationList);

    if (stationList.num > 0) {
        if (!wifiWasConnected) {
            Serial.printf("Device connected! (%d device(s))\n", stationList.num);
            wifiWasConnected = true;
        }
    } else if (wifiWasConnected) {
        Serial.println("All devices disconnected.");
    }
}

// ================= setup =================
void setup() {
    Serial.begin(115200);
    delay(1000);

    bootTime = millis();

    Serial.println();
    Serial.println("=================================");
    Serial.println("  E3 Firmware v" + String(BUILD_FIRMWARE_VERSION));
    Serial.println("  ESP32-S3 Bluetooth Audio Gateway + WiFi-OTA");
    Serial.println("=================================");

    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    pinMode(LED_STATUS_PIN, OUTPUT);
    digitalWrite(LED_STATUS_PIN, LOW);

    initI2S();

    // 初始化WiFi AP + Web服务器 (WiFi-OTA)
    String chipID = WiFiManager.getChipID();
    String ssid = "E3_" + chipID;
    Serial.printf("AP SSID: %s\n", ssid.c_str());
    Serial.printf("AP Password: 12345678\n");
    Serial.printf("WiFi will auto-disable after 30s if no connection\n");

    WiFiManager.begin(ssid.c_str(), "12345678");
    WebServer.begin();

    Serial.printf("OTA URL: http://%s\n", WiFiManager.getIP().c_str());

    initBluetooth();

    initOLED();

    initSHT31();

    Serial.println();
    Serial.println("=================================");
    Serial.println("  System Ready!");
    Serial.println("=================================");
}

void loop() {
    handleBluetoothState();
    handleUartCommands();
    updateOLED();
    handleSHT31();

    // WiFi-OTA 智能管理
    if (wifiEnabled) {
        unsigned long elapsed = millis() - bootTime;

        if (elapsed >= WIFI_TIMEOUT_MS && !wifiWasConnected) {
            disableWiFi();
        }
        else if (elapsed % WIFI_RECHECK_MS < 20) {
            checkWiFiConnection();
        }
    }
}
