#pragma once
#include <ESPAsyncWebServer.h>
#include <Update.h>

class OTAHandlerClass {
private:
    static bool _enabled;

public:
    static void setup(AsyncWebServer* server);
    static bool isEnabled() { return _enabled; }
    static void setEnabled(bool enabled) { _enabled = enabled; }
};
