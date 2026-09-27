/**
 * @file lv_mem_port.h
 *
 * LVGL 对象池的分配器（配合 lv_conf.h 的 LV_MEM_CUSTOM = 1 使用）。
 *
 * 为什么要这个：
 *   之前的配置是 LV_MEM_CUSTOM=0 + LV_MEM_SIZE=64KB，也就是 LVGL 从
 *   **内部 DRAM** 里划了一块 64KB 的静态池。这块池和 BLE 协议栈、FFat、
 *   ArduinoJson、19.2KB 的帧缓冲、任务栈全部抢同一块 327KB 的内部 DRAM。
 *   加上显示端持续分配（invalidate 区域、样式、文本缓冲），内部堆会慢慢
 *   碎片化，某次分配失败就 abort/panic → 芯片复位 → Windows 重新枚举
 *   → 听到"设备重新插入"的提示音。
 *
 *   板子是 ESP32-S3-N16R8，有 8MB 八线 PSRAM 一直没用上。这里把 LVGL
 *   对象池整个搬进 PSRAM，直接给内部 DRAM 让出 64KB。
 *
 * 分工：
 *   LVGL 对象池（lv_mem_*）  → PSRAM。LVGL 只在这块内存里做指针运算和普通
 *                              读写，**不做 DMA**，放 PSRAM 安全。
 *   帧缓冲（draw_buf）      → 保持内部 DRAM。SPI 要从它 DMA 取像素，
 *                              放 PSRAM 会引入缓存同步风险，不值得省这 19.2KB。
 *
 * 为什么"首次分配时一次性选定"而不是每次都试探：
 *   realloc 必须在**同一个分配器**上做。如果这里 PSRAM 成功、那里回落 malloc，
 *   一块 PSRAM 内存被 free 之后再 realloc 就要跨类型，问题很多。所以第一次
 *   分配时问一次"PSRAM 到底有没有"，之后全项目统一走那一个，不存在混用。
 *
 * 兜底：
 *   memory_type 配错 / PSRAM 初始化失败时，检测到 PSRAM 容量为 0，
 *   整套回落 malloc。设备照样能跑，只是少 64KB 余量 —— 不会因为这个改动变砖。
 */

#ifndef LV_MEM_PORT_H
#define LV_MEM_PORT_H

#include <stddef.h>
#include <esp_heap_caps.h>

// PSRAM 到底有没有，第一次分配时问一次并记住
static inline bool lv_port_use_psram(void) {
    static bool decided = false;
    static bool usePsram = false;
    if (!decided) {
        decided = true;
        usePsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    }
    return usePsram;
}

static inline void * lv_port_alloc(size_t size) {
    if (size == 0) return NULL;
    if (lv_port_use_psram()) {
        void * p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return malloc(size);
}

static inline void * lv_port_realloc(void * p, size_t new_size) {
    if (p == NULL) return lv_port_alloc(new_size);
    if (new_size == 0) { free(p); return NULL; }
    if (lv_port_use_psram()) {
        return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return realloc(p, new_size);
}

// heap_caps_free 本身就是 free，这里统一用 free
static inline void lv_port_free(void * p) {
    if (p) free(p);
}

#endif  // LV_MEM_PORT_H
