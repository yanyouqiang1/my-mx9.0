/**
 * min_cdc —— 最小可观测性验证固件
 *
 * 目的只有一个：**搞清楚"看不到日志"到底是固件没跑，还是日志没出来。**
 * 所以这里只有三件事：
 *   1. 每秒往 USB CDC 打一行（带 uptime 计数）
 *   2. 每秒闪一下 GPIO48（S3-DevKitC 的板载 RGB 灯，肉眼可确认固件在跑）
 *   3. 启动时把 reset_reason 打出来
 *
 * 没有任何 BLE、没有 LVGL、没有传感器 —— 出问题就一定是 USB 通路或启动流程。
 */

#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    // 别在这里等 !Serial：HWCDC 的 operator bool() 在驱动没装上时可能一直是 false，
    // 白等 3 秒反而掩盖问题。直接开干。
    Serial.println("\n=== min_cdc 启动 ===");
    Serial.printf("reset_reason=%d\n", (int)esp_reset_reason());
    pinMode(48, OUTPUT);
}

void loop() {
    static uint32_t n = 0;
    static unsigned long last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        n++;
        digitalWrite(48, n & 1);
        Serial.printf("[min_cdc %u] uptime=%lums heap=%u\n",
                      (unsigned)n, (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    }
    delay(2);
}
