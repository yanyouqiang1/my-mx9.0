#pragma once
#include <WiFi.h>

class WiFiManagerClass {
public:
    void begin(const char* apName, const char* apPassword = nullptr);
    String getIP();
    bool isConnected();
    String getChipID();
};
