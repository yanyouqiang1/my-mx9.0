#include "web_server.h"
#include "ota_handler.h"
#include "web_pages.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

bool WebServerClass::_otaEnabled = true;

WebServerClass::WebServerClass() : _server(nullptr) {
    _server = new AsyncWebServer(80);
}

WebServerClass::~WebServerClass() {
    end();
    if (_server) {
        delete _server;
        _server = nullptr;
    }
}

void WebServerClass::begin() {
    // 初始化 SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
    }

    // 根路径 - 返回 HTML 页面
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", INDEX_HTML, processor);
    });

    // GET /status - 返回设备状态 JSON
    _server->on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(256);
        doc["firmwareVersion"] = "1.0.0";
        doc["freeMemory"] = ESP.getFreeHeap();
        doc["uptime"] = millis() / 1000;
        doc["otaEnabled"] = WebServerClass::isOTAEnabled();
        doc["chipModel"] = "ESP32-S3";
        doc["chipRevision"] = ESP.getChipRevision();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // POST /restart - 重启设备
    _server->on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });

    // POST /ota-control - OTA 开关控制
    _server->on("/ota-control", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("plain", true)) {
            String body = request->getParam("plain", true)->value();
            DynamicJsonDocument doc(128);
            DeserializationError error = deserializeJson(doc, body);
            if (!error && doc.containsKey("enabled")) {
                WebServerClass::setOTAEnabled(doc["enabled"].as<bool>());
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid request\"}");
            }
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing body\"}");
        }
    });

    // 设置 OTA 上传处理（placeholder）
    OTAHandlerClass::setup(_server);

    // 启动服务器
    _server->begin();
    Serial.println("Web server started on port 80");
}

void WebServerClass::end() {
    if (_server) {
        _server->end();
    }
}
