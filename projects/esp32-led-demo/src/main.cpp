#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "wifi_manager.h"
#include "web_server.h"

// ESP32-S3 DevKit N16R8:
#define LED_PIN 2  // GPIO 2 - 外接红色LED

WiFiManagerClass WiFiManager;
WebServerClass WebServer;

// LED breathing state
int breathDirection = 1;
int ledBrightness = 0;

// ==================== OTA 智能模式 ====================
// 通电后开启WiFi AP，等待连接或超时后关闭
bool wifiEnabled = true;
bool wifiWasConnected = false;
unsigned long bootTime = 0;
const unsigned long WIFI_TIMEOUT_MS = 30000;  // 30秒无连接则关闭WiFi
const unsigned long WIFI_RECHECK_MS = 5000;    // 每5秒检查一次连接状态

void disableWiFi() {
    if (!wifiEnabled) return;

    Serial.println("No connection in 30s, disabling WiFi to save power...");

    // 关闭 Web 服务器
    WebServer.end();

    // 关闭 WiFi AP
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    wifiEnabled = false;
    Serial.println("WiFi disabled. Press EN/RST button to restart.");
}

void checkWiFiConnection() {
    if (!wifiEnabled) return;

    // 检查是否有设备连接了 AP
    wifi_sta_list_t stationList;
    memset(&stationList, 0, sizeof(wifi_sta_list_t));

    esp_wifi_ap_get_sta_list(&stationList);

    if (stationList.num > 0) {
        if (!wifiWasConnected) {
            Serial.printf("Device connected! (%d device(s))\n", stationList.num);
            wifiWasConnected = true;
        }
    } else if (wifiWasConnected) {
        // 有设备断开连接，检查是否需要关闭
        Serial.println("All devices disconnected.");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    bootTime = millis();

    Serial.println();
    Serial.println("=================================");
    Serial.println("  ESP32-S3 LED + OTA Demo");
    Serial.println("  Version: " + String(BUILD_FIRMWARE_VERSION));
    Serial.println("=================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    Serial.printf("LED connected to GPIO %d\n", LED_PIN);

    String chipID = WiFiManager.getChipID();
    String ssid = "ESP_LED_" + chipID;
    Serial.printf("AP SSID: %s\n", ssid.c_str());
    Serial.printf("AP Password: 12345678\n");
    Serial.printf("WiFi will auto-disable after 30s if no connection\n");

    WiFiManager.begin(ssid.c_str(), "12345678");
    WebServer.begin();

    Serial.println();
    Serial.println("=================================");
    Serial.println("  System Ready!");
    Serial.printf("  Visit: http://%s\n", WiFiManager.getIP().c_str());
    Serial.println("=================================");
    Serial.println();
}

void loop() {
    // LED 呼吸灯效果
    ledBrightness += breathDirection * 5;
    if (ledBrightness >= 255) {
        ledBrightness = 255;
        breathDirection = -1;
    } else if (ledBrightness <= 0) {
        ledBrightness = 0;
        breathDirection = 1;
    }
    analogWrite(LED_PIN, ledBrightness);
    delay(20);

    // 智能 WiFi 管理
    if (wifiEnabled) {
        unsigned long elapsed = millis() - bootTime;

        // 超时检查
        if (elapsed >= WIFI_TIMEOUT_MS && !wifiWasConnected) {
            disableWiFi();
        }
        // 定期检查连接状态
        else if (elapsed % WIFI_RECHECK_MS < 20) {
            checkWiFiConnection();
        }
    }
}
