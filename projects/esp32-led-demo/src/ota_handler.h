#pragma once
#include <ESPAsyncWebServer.h>

class OTAHandlerClass {
public:
    static void setup(AsyncWebServer* server);
    static bool isEnabled() { return true; }
    static void setEnabled(bool enabled) {}
};
