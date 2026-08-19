#pragma once
#include <WiFi.h>

class WiFiManagerClass {
public:
    void begin(const char* apName, const char* apPassword = nullptr);
    void end();
    String getIP();
    bool isConnected();
    bool isEnabled() { return _enabled; }
    void setEnabled(bool enabled) { _enabled = enabled; }
    String getChipID();

private:
    bool _enabled = true;
};
