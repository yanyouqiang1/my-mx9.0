/**
 * @file panic_trace.h
 *
 * panic 现场留存 —— "黑匣子"。
 *
 * 为什么要做这个：
 *   2026-10-23 反复出现的故障是"固件在某个动作里随机 panic"，但**看不到
 *   任何崩溃现场**：panic 的回溯（Guru Meditation / assert 内容 / Backtrace）
 *   默认只走 UART0（CONFIG_ESP_CONSOLE_UART_DEFAULT），而这块板子的
 *   USB 口已经被 HID 键盘占死，没插 USB-TTL 就是一个字都看不到。
 *   没有现场就只能猜 —— 而猜出来的结论经常是错的。
 *
 * 所以做两件事：
 *   ① **接管 ROM 的 putchar**：把 IDF 的日志 / assert / panic 回溯全部收进
 *      一块内存环形缓冲。
 *   ② **接管 panic 钩子**：panic 发生时把这块环的尾巴（也就是 panic 的那几行
 *      + 回溯）抄进 **RTC 慢存**（跨复位不丢），下次开机直接打印出来。
 *
 * ⚠⚠⚠ **为什么不直接用 Serial.setDebugOutput(true)**（这一版踩过）：
 *   它内部是 `ets_install_putc2(cdc0_write_char)` —— 把 ROM 的 putchar 直接
 *   接到 **TinyUSB 的 CDC TX FIFO**。而那个 FIFO 不是线程安全的：
 *     · 主循环的日志镜像任务 → USBCDC::write → tud_cdc_n_write（带 tx_lock）
 *     · 蓝牙协议栈任务的 log_d() → ROM printf → tud_cdc_n_write_char（**不带锁**）
 *   两边并发写同一个 FIFO → 写坏 → 症状是"CDC 通道整个死掉，一个字节都收不到"，
 *   再往后就是随机 panic。
 *   实测：开了它之后 CDC 全哑，而且传输途中设备随机重启。
 *
 *   所以这里的结构是：
 *       ROM putchar ──► s_romRing（内存环，8KB）
 *                            │
 *                            └─► 日志镜像任务（**唯一**写 TinyUSB 的地方）──► CDC
 *
 * ⚠ 为什么这个文件是独立的头文件、而不是直接写在 .ino 里：
 *   Arduino 的 .ino 会被"自动原型生成器"扫一遍，把所有函数定义变成原型
 *   提到文件最前面。`extern "C" void (*g_panic_abort)(void);` 这种**函数指针
 *   变量**声明会让它解析错乱，生成的原型整段错位，编译直接炸
 *   （报错还指向完全不相关的地方，"lv_obj_t does not name a type"）。
 *   头文件不参与原型生成，所以把这套东西整体搬到这里。
 */

#ifndef PANIC_TRACE_H
#define PANIC_TRACE_H

// ⚠ 整套黑匣子只在 **debug 构建**（LOG_CDC_MIRROR=1）里生效。
//   原因：它要接管 ROM 的 putchar（把 IDF 日志和 panic 回溯收进内存环），
//   而产品固件 ARDUINO_USB_MODE=1 没有 CDC 节点 —— 接管了也搬不出去，
//   反而白白占掉 8KB 内存环 + 2KB RTC 慢存，还把 UART0 的 console 顶掉
//   （产品固件接 USB-TTL 时，panic 回溯本来就该照旧走 UART0）。
//
//   产品固件要抓现场的办法：接 USB-TTL，看 UART0 上的 Guru Meditation 回溯。
#ifndef LOG_CDC_MIRROR
#define LOG_CDC_MIRROR 0
#endif

#if LOG_CDC_MIRROR

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_debug_helpers.h>
#include <rom/ets_sys.h>

// 内存环：留最近 8KB 的 ROM 输出
#define ROMRING_CAP   8192
// 存进 RTC 慢存的 panic 文本长度
#define PANIC_CAP     2048
// "PANI"
#define PANIC_MAGIC   0x50414E49UL

// ============================================================ ROM 输出环

static char               s_romRing[ROMRING_CAP];
static volatile uint32_t  s_romLen = 0;      // 累计写入量（单调递增）

// ROM 的 putchar。只做一次内存写，不碰任何外设 —— 可以在任意上下文调用。
static void IRAM_ATTR romPutc(char c) {
    uint32_t i = s_romLen;
    s_romRing[i % ROMRING_CAP] = c;
    s_romLen = i + 1;
}

// 把环里的新内容搬进 USB CDC。**整个固件里只有这一处写 TinyUSB。**
// 一次最多搬 256 字节，别把日志镜像任务堵死。
//
// ⚠ 同时也往 UART0 抄一份：USB-TTL（CH343）接哪块板，那块板的 UART0
//   就实时看到 panic 的回溯 —— 不依赖 USB CDC，也就不依赖主机端有没有
//   正确地打开 CDC 口。
static void romRingDrain(void) {
    static uint32_t drained = 0;
    uint32_t len = s_romLen;
    if (len - drained > ROMRING_CAP) drained = len - ROMRING_CAP;   // 太老的丢掉
    while (len - drained > 0) {
        uint32_t start = drained % ROMRING_CAP;
        uint32_t n = len - drained;
        if (n > ROMRING_CAP - start) n = ROMRING_CAP - start;
        if (n > 256) n = 256;
        Serial0.write((const uint8_t*)(s_romRing + start), n);
        Serial.write((const uint8_t*)(s_romRing + start), n);
        drained += n;
    }
}

// ============================================================ panic 现场

typedef struct {
    uint32_t magic;                  // PANIC_MAGIC
    uint32_t len;                    // text 里有多少有效字节
    char     text[PANIC_CAP];
} PanicTrace;

// RTC 慢存：panic 之后芯片复位，PSRAM 和普通 RAM 全清零，只有它活下来。
static RTC_DATA_ATTR PanicTrace s_panic;

// IDF 5.1 的 panic handler 收尾时会调 `g_panic_abort`（一个全局函数指针，
// libesp_system 里导出，nm 里能查到符号）。
extern "C" void (*g_panic_abort)(void);

static void panicHook(void) {
    // 先把回溯也追加进环（ROM printf 已被接管 → 会写进 s_romRing）。
    // 深度 24 足够定位到具体函数/行号，再多就超出慢存能装的量了。
    esp_backtrace_print(24);
    uint32_t len = s_romLen;
    uint32_t start = (len > PANIC_CAP) ? (len - PANIC_CAP) : 0;
    uint32_t n = len - start;
    if (n > PANIC_CAP - 1) n = PANIC_CAP - 1;
    for (uint32_t i = 0; i < n; i++) {
        s_panic.text[i] = s_romRing[(start + i) % ROMRING_CAP];
    }
    s_panic.text[n] = '\0';
    s_panic.len = n;
    s_panic.magic = PANIC_MAGIC;
}

// 装上接管（setup 里调一次）。
static inline void panicTraceInit(void) {
    ets_install_putc2(romPutc);
    g_panic_abort = panicHook;
}

// 上次 panic 的现场。**打印交给调用方**（.ino 里用 LOG_PORT 打）——
// 这个头文件不能自己用 LOG_PORT：LOG_PORT 宏是在 .ino 里定义的，
// 而头文件在它之前就被 include 了。
static inline bool panicTraceHas(void)     { return s_panic.magic == PANIC_MAGIC; }
static inline const char* panicTraceText(void) { return s_panic.text; }
static inline uint32_t panicTraceLen(void) { return s_panic.len; }
static inline void panicTraceClear(void)   { s_panic.magic = 0; }

#endif  // LOG_CDC_MIRROR

#endif  // PANIC_TRACE_H
