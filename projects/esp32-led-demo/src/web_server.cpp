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
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
    }

    // Root - HTML page
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", INDEX_HTML);
    });

    // GET /status
    _server->on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(256);
        doc["firmwareVersion"] = BUILD_FIRMWARE_VERSION;
        doc["freeMemory"] = ESP.getFreeHeap();
        doc["uptime"] = millis() / 1000;
        doc["otaEnabled"] = WebServerClass::isOTAEnabled();
        doc["chipModel"] = "ESP32-S3";

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // POST /restart
    _server->on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });

    // Setup OTA handler
    OTAHandlerClass::setup(_server);

    _server->begin();
    Serial.println("Web server started on port 80");
}

void WebServerClass::end() {
    if (_server) {
        _server->end();
    }
}
