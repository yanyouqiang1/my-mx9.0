#pragma once
#include <ESPAsyncWebServer.h>

class WebServerClass {
private:
    AsyncWebServer* _server;
    static bool _otaEnabled;
public:
    WebServerClass();
    ~WebServerClass();
    AsyncWebServer* getServer() { return _server; }
    void begin();
    void end();
    static bool isOTAEnabled() { return _otaEnabled; }
    static void setOTAEnabled(bool enabled) { _otaEnabled = enabled; }
};
