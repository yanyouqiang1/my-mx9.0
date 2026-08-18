#include "ota_handler.h"
#include <Update.h>

void OTAHandlerClass::setup(AsyncWebServer* server) {
    server->on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (Update.hasError()) {
            request->send(500, "text/plain", "OTA Update failed");
        } else {
            request->send(200, "text/plain", "Update OK. Rebooting...");
        }
        delay(500);
        ESP.restart();
    }, [](AsyncWebServerRequest *request, const String& filename,
         size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            Serial.printf("OTA Update Start: %s\n", filename.c_str());

            if (!filename.endsWith(".bin")) {
                request->send(400, "text/plain", "Only .bin files are supported");
                return;
            }

            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
                return;
            }
        }

        if (len) {
            Update.write(data, len);
        }

        if (final) {
            if (Update.end(true)) {
                Serial.printf("OTA Update complete: %u bytes\n", index + len);
            } else {
                Update.printError(Serial);
            }
        }
    });
}
