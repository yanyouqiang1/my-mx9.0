#include "web_server.h"
#include "ota_handler.h"
#include "wifi_manager.h"

static AsyncWebServer* _server = nullptr;

void WebServerClass::begin() {
    _server = new AsyncWebServer(80);

    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>E3 Firmware Update</title>";
        html += "<style>";
        html += "body{font-family:Arial;max-width:600px;margin:50px auto;padding:20px;}";
        html += "h1{color:#333;}";
        html += ".btn{background:#007bff;color:#fff;padding:15px 30px;border:none;border-radius:5px;";
        html += "cursor:pointer;font-size:16px;display:block;width:100%;margin:20px 0;}";
        html += ".btn:hover{background:#0056b3;}";
        html += ".info{background:#f8f9fa;padding:15px;border-radius:5px;margin:20px 0;}";
        html += "</style></head><body>";
        html += "<h1>E3 Firmware Update</h1>";
        html += "<div class='info'>";
        html += "<p><strong>Version:</strong> " + String(BUILD_FIRMWARE_VERSION) + "</p>";
        html += "<p><strong>IP:</strong> " + WiFi.softAPIP().toString() + "</p>";
        html += "</div>";
        html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
        html += "<input type='file' name='update' accept='.bin' style='margin:20px 0;'>";
        html += "<input type='submit' class='btn' value='Upload Firmware'>";
        html += "</form>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    OTAHandlerClass::setup(_server);

    _server->begin();
    Serial.println("Web server started");
}

void WebServerClass::end() {
    if (_server) {
        _server->end();
        delete _server;
        _server = nullptr;
    }
}
