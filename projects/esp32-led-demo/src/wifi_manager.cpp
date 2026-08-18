#include "wifi_manager.h"

void WiFiManagerClass::begin(const char* apName, const char* apPassword) {
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP);

    if (apPassword != nullptr && strlen(apPassword) >= 8) {
        WiFi.softAP(apName, apPassword);
    } else {
        WiFi.softAP(apName);
    }

    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
}

String WiFiManagerClass::getIP() {
    return WiFi.softAPIP().toString();
}

bool WiFiManagerClass::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiManagerClass::getChipID() {
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }
    char idStr[9];
    snprintf(idStr, sizeof(idStr), "%04X", chipId & 0xFFFF);
    return String(idStr);
}
