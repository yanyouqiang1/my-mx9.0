#include "ota_handler.h"

bool OTAHandlerClass::_enabled = true;

void OTAHandlerClass::setup(AsyncWebServer* server) {
    // OTA 上传端点
    server->on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!OTAHandlerClass::isEnabled()) {
            request->send(503, "text/plain", "OTA is disabled");
            return;
        }

        if (Update.hasError()) {
            request->send(500, "text/plain", "OTA Update failed");
        } else {
            request->send(200, "text/plain", "Update OK. Rebooting...");
        }
        delay(500);
        ESP.restart();
    }, [](AsyncWebServerRequest *request, const String& filename,
         size_t index, uint8_t *data, size_t len, bool final) {
        // 上传数据处理
        if (!OTAHandlerClass::isEnabled()) {
            return;
        }

        if (!index) {
            // 开始上传
            Serial.printf("OTA Update Start: %s\n", filename.c_str());

            // 验证文件类型
            if (!filename.endsWith(".bin")) {
                request->send(400, "text/plain", "Only .bin files are supported");
                return;
            }

            // 开始更新
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
                return;
            }
        }

        // 写入数据
        if (len) {
            Update.write(data, len);
        }

        if (final) {
            // 完成
            if (Update.end(true)) {
                Serial.printf("OTA Update complete: %u bytes\n", index + len);
            } else {
                Update.printError(Serial);
            }
        }
    });
}
