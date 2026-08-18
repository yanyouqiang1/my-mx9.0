#include <Arduino.h>
#include <WiFi.h>
#include "wifi_manager.h"
#include "web_server.h"
#include "ota_handler.h"

WiFiManagerClass WiFiManager;
WebServerClass WebServer;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("=================================");
    Serial.println("  ESP32-S3 WiFi OTA Demo");
    Serial.println("=================================");

    // 获取芯片 ID 生成唯一 SSID
    String chipID = WiFiManager.getChipID();
    String ssid = "ESP_OTA_" + chipID;

    Serial.printf("Chip ID: %s\n", chipID.c_str());
    Serial.printf("AP SSID: %s\n", ssid.c_str());
    Serial.printf("AP Password: 12345678\n");

    // 初始化 WiFi AP
    WiFiManager.begin(ssid.c_str(), "12345678");

    // 初始化 Web 服务器（会设置 OTA 处理器）
    WebServer.begin();

    Serial.println();
    Serial.println("=================================");
    Serial.println("  System Ready!");
    Serial.println("  Connect to AP and visit:");
    Serial.printf("  http://%s\n", WiFiManager.getIP().c_str());
    Serial.println("=================================");
    Serial.println();
}

void loop() {
    // 空闲处理
    delay(10);
}
