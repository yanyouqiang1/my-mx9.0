/*
 * unstick_boot.cpp —— 把一块卡在 ROM 下载模式（BOOT 一直被拉低）的板子救出来。
 *
 * 原理：
 *   芯片在下载模式时，ROM 会把 BOOT(GPIO0) 按住不放，于是任何复位都还是
 *   下载模式 —— 主机侧什么办法都没有（试过 DTR/RTS 时序、1200 波特 touch，
 *   全都不行）。
 *   但只要让**固件自己跑起来一次**，它就能把 GPIO0 拉高再复位：
 *   ROM 复位那一瞬间采到 BOOT=高 → 正常启动 → 应用起来了。
 *
 * 所以这个小东西被刷到 **app1（0x340000）**，同时把 otadata 指到 app1：
 *   芯片复位 → ROM 按 otadata 启动 app1 → 本程序：
 *     1. 把 GPIO0 配成输出并拉高
 *     2. 把启动分区改回 app0（保留真正的产品固件）
 *     3. esp_restart()
 *   → ROM 复位，采样到 BOOT=高 → 正常启动 **app0（产品固件）** ✓
 *
 * ⚠ 只在"BOOT 松不开"这种极端情况下用；正常情况直接按 EN 就行。
 *
 * 刷法：
 *   pio run -e unstick -t upload --upload-port /dev/cu.usbmodemXXXX
 *   （本工程用 arduino framework + esp32s3 分区表：app0@0x10000 app1@0x340000）
 */
#include <Arduino.h>
#include <esp_ota_ops.h>

void setup() {
    // 1) 先把 BOOT 脚拉高 —— 这一步是整个方案的关键
    pinMode(GPIO_NUM_0, OUTPUT);
    digitalWrite(GPIO_NUM_0, HIGH);

    // 2) 把启动分区指回 app0（真正的固件还在那儿）
    const esp_partition_t* app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (app0) {
        esp_ota_set_boot_partition(app0);
    }

    // 3) 等输出电平稳定，然后复位 —— ROM 采样到的就是 BOOT=高
    delay(200);
    esp_restart();
}

void loop() {}
