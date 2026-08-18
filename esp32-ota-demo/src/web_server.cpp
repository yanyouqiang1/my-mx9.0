#include "web_server.h"
#include "ota_handler.h"
#include "web_pages.h"
#include <SPIFFS.h>

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
