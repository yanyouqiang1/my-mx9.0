/**
 * @file crash_trace.h
 *
 * 崩溃 / 卡死现场记录。
 *
 * 为什么要这个：
 *   固件"跑一段时间自己重启"，开机 HUD 显示 esp_reset_reason() = ESP_RST_PANIC
 *   （真 panic，不是掉电也不是看门狗）。但**崩溃的具体位置抓不到**：
 *   panic 之后芯片复位、USB 重新枚举，Windows 侧那个串口句柄直接失效，
 *   PC 上的串口助手根本来不及收到 Guru Meditation 的回溯。这是工具限制，
 *   不是固件的问题 —— 只要不换硬件抓法，就只能靠猜。
 *
 *   所以改成"把现场留在芯片里"：用 RTC_DATA_ATTR（RTC 慢存）存一个结构体。
 *   软件复位（panic 走的就是 esp_restart → SW reset）**不会**清掉它，
 *   下次开机直接读出来打在屏幕上。
 *
 * 两类故障都能抓到：
 *   1) panic —— loop() 停在某一步，ct.stage 就是最后写的那个阶段。
 *   2) 卡死 / 死循环 —— loop() 不再推进心跳，监测任务判定 6 秒无进展，
 *      先把 ct.hang 置 1 再复位，同样留下现场。
 *   （看门狗本来也会兜住，但看门狗只会说"任务超时"，说不出是哪个阶段。）
 *
 * 代价：几个 volatile 写，纳秒级；一个 8KB 栈的 FreeRTOS 任务。
 */

#ifndef CRASH_TRACE_H
#define CRASH_TRACE_H

#include <stdint.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 阶段编号。名字表在 lvgl_s3.ino 里（那边有中文字面量，字体生成脚本才扫得到）
#define CT_S_IDLE         0   // 循环末尾（一切正常）
#define CT_S_LOOP         1   // 主循环开头
#define CT_S_BLE_DELAY    2   // 蓝牙重连的 delay(500)
#define CT_S_PING         3   // 串口心跳
#define CT_S_SHT          4   // 温湿度读取
#define CT_S_SCAN         5   // 键盘扫描（列循环）
#define CT_S_SCAN_I2C     6   // 扫描里的 I2C 探测
#define CT_S_SCAN_RECOVER 7   // I2C 总线恢复
#define CT_S_SCAN_KEY     8   // 按键分发
#define CT_S_LED          9   // 灯效引擎
#define CT_S_SLEEP       10   // 息屏
#define CT_S_MENU        11   // 菜单重建
#define CT_S_DYNAMIC     12   // 动态刷新
#define CT_S_HUD         13   // HUD 显隐
#define CT_S_NOTIF       14   // 通知队列
#define CT_S_LVGL        15   // LVGL 定时器 / 刷屏
#define CT_S_REBUILD     16   // 主屏内容重建
#define CT_S_HANG        17   // 监测任务判定：心跳停了 6 秒
#define CT_S_CFG_POST   18   // 配置 JSON 保存后摊到 loop 里的界面副作用
#define CT_S_WP_RX      19   // 壁纸 JPEG 接收中
#define CT_S_WP_DECODE  20   // 壁纸解码 / 写 /logo.bin
// 配置 JSON 的**下发处理**：解析、校验、逐键落盘、NVS 提交。
// 为什么单独分两级：实测"连蓝牙就崩"和"清空方案就崩"都发生在这一段里，
// 而整段 loop 有十几秒都不调 ct_mark()，崩在里面时 RTC 里留下的是上一次
// 有标记的地方（多半是"循环末尾"），等于什么都没说。
#define CT_S_CFG_RX     21   // 攒 CFGDATA / 收尾
#define CT_S_CFG_APPLY  22   // cfgApplyDoc：校验 + 逐键写 NVS + 提交

// 磁标：RTC 慢存里没有有效记录时用它区分（顺便挡掉随机数恰好撞上）
#define CT_MAGIC 0x4B4D5831UL   // "KMX1"

typedef struct {
    uint32_t magic;      // CT_MAGIC
    uint32_t ready;      // 本次运行的 setup 是否跑完（0 = 死在启动过程中）
    uint32_t stage;      // 最后执行到的阶段
    uint32_t keyCode;    // 最后按下的键码（0xFFFFFFFF = 无）
    uint32_t sysMode;    // 当时的 currentSysMode
    uint32_t dispMode;   // 当时的 currentDispMode
    uint32_t hang;       // 1 = 监测任务判定为卡死
    uint32_t uptimeMs;   // 出事时已运行的毫秒数
    uint32_t ovf;        // 1 = FreeRTOS 检到栈溢出（见 vApplicationStackOverflowHook）
    uint32_t ovfStack;   // 溢出瞬间的栈余量（字节）
    char     ovfTask[16];// 溢出的任务名（"loopTask" / "ctmon" / "IDLE" ...）
} CrashTrace;

// ⚠️ ct_rec / ct_prev / ct_heartbeat 的**唯一定义都在 lvgl_s3.ino 里**，
//   这里只做 extern 声明。
//
//   原因和 ct_heartbeat 的坑完全一样，只是后果更隐蔽：bt_link.cpp 也 include
//   本头文件，而**栈溢出钩子只在 .ino 里定义**、写的是 .ino 那份 ct_rec。
//   如果这里放 static，bt_link.cpp 会另得一份副本，从此"钩子写进去的现场"
//   和"某个读者读到的现场"可以互不相干 —— 而"溢出的到底是哪个任务"恰恰是
//   这个钩子存在的唯一理由，读到错的那份等于白做。
extern RTC_DATA_ATTR CrashTrace ct_rec;
extern CrashTrace ct_prev;
extern volatile uint32_t    ct_heartbeat;

// 写阶段。热路径里就是一条 volatile 写，可以放心每处都调。
// 顺带推进心跳，监测任务靠它判断 loop() 还活着没有。
static inline void ct_mark(uint32_t stage) {
    ct_rec.stage = stage;
    ct_heartbeat++;
}

// 只报"我还活着"、**不改**阶段号。
//
// 为什么需要它：卡死监测看的是心跳，**不是**任务看门狗。bt_link 里所有
// 长耗时的地方（cfgStoreRemap 的 NVS 删+写+回读、cfgApplyDoc 的宏/全局键
// 落盘、preferences.end() 提交、分片发包）都靠 btLinkKeepAlive() 续命，
// 而它原来只调 esp_task_wdt_reset() —— 那管的是 10 秒的任务看门狗，和 6 秒
// 的卡死监测是两套完全独立的东西。
//
// 结果就是：一次正常的、只是慢的配置保存（分区越碎越慢，能到好几秒），
// 任务看门狗被喂住了、可 6 秒监测看心跳没动，判定"卡死"，直接 esp_restart()。
// 现场表现正是"一连蓝牙就重启、一存映射就重启"。
//
// 这个函数就是给喂狗点用的：谁在替主任务续命，谁就该同时报一次活。
static inline void ct_alive(void) {
    ct_heartbeat++;
}

static inline void ct_set_ctx(uint32_t sysMode, uint32_t dispMode) {
    ct_rec.sysMode = sysMode;
    ct_rec.dispMode = dispMode;
}

static inline void ct_key(uint16_t code) {
    ct_rec.keyCode = code;
}

// setup 早期调用：校验磁标，给本次运行清零（**不动** ready 之前先读完，
// 上一次的现场要留到 showBootDiagnostics() 才用，所以这里只清本次的字段）
static inline void ct_begin(void) {
    ct_prev = ct_rec;                       // 先把上一次的现场留档
    if (ct_rec.magic != CT_MAGIC) {
        ct_rec.magic    = CT_MAGIC;
        ct_rec.keyCode  = 0xFFFFFFFFUL;
        ct_rec.sysMode  = 0;
        ct_rec.dispMode = 0;
        ct_rec.hang     = 0;
        ct_rec.uptimeMs = 0;
        ct_rec.ovf      = 0;
        ct_rec.ovfStack = 0;
        for (int i = 0; i < (int)sizeof(ct_rec.ovfTask); i++) ct_rec.ovfTask[i] = 0;
    }
    ct_rec.stage = CT_S_IDLE;
    ct_rec.ready = 0;
    ct_heartbeat = 0;
}

// setup 末尾调用：本次启动顺利完成，标记一下。下次开机读到 ready==0
// 就说明上一次是**死在启动流程里**（还没进 loop 就崩了），这是两个完全
// 不同的故障，现场阶段号也有用。
static inline void ct_ready(void) {
    ct_rec.ready = 1;
}

// 监测任务：loop() 每轮都推进 ct_heartbeat。连续 3 次检查（6 秒）没推进
// 就判定卡死/死循环，把 hang 标记和当时的阶段写进 RTC 慢存再复位。
//
// 阈值 6 秒要**小于**看门狗的 10 秒（WDT_TIMEOUT），这样我们能抢在看门狗
// 之前把现场记下来；又要比最长的正常阻塞长得多 —— loop() 里最久的阻塞是
// 蓝牙重连的 delay(500) 和一次全屏 SPI flush，都是百毫秒级。
//
// 前 8 秒是宽限期：setup() 里 BLE 初始化、FFat 挂载、首屏建控件都在跑，
// 期间 loop 还没转起来，不能误判。
static void ct_monitor_task_entry(void* arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    uint32_t last = ct_heartbeat;
    uint8_t stall = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t hb = ct_heartbeat;   // 不调 esp_task_wdt_reset：本任务没订阅 WDT，调了只会刷警告
        if (hb == last) {
            if (++stall >= 3) {
                ct_rec.hang = 1;
                ct_rec.uptimeMs = (uint32_t)millis();
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_restart();
            }
        } else {
            stall = 0;
            last = hb;
        }
    }
}

// setup 末尾调用：拉起监测任务
static inline void ct_startMonitor(void) {
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreate(ct_monitor_task_entry, "ctmon", 3072, NULL, 1, NULL);
}

// ===========================
// 栈溢出钩子
// ===========================
//
// 为什么要自己写这个：
//   FreeRTOS 在 CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y（本工程就是）
//   下，每次任务切换都会检查栈尾的"金丝雀"（canary）有没有被踩。一旦踩到，
//   就调用 vApplicationStackOverflowHook(xTask, pcTaskName)。这个符号在
//   ESP-IDF 里是 **weak**（ELF 里能看到 `W vApplicationStackOverflowHook`），
//   默认实现直接 abort() → panic → 复位。
//
//   问题就在"直接 abort"：此刻正主自己的栈已经被踩烂了，栈上的现场（局部
//   变量、返回地址）全是垃圾，默认实现连"是哪个任务"都没来得及留。
//   而本工程最需要回答的问题恰恰是"是谁溢出的" —— 是 loopTask 那个几百个
//   else-if 的 handleCommand，还是我们自己的 ctmon 监测任务。
//
//   所以覆盖它：先把任务名 + 当时的栈余量写进 RTC 慢存（这段代码本身极短、
//   几乎不占栈，安全），再走原来的复位流程。下次开机就能直接看到：
//   "上次栈溢出：任务 loopTask，栈余 0B，阶段 配置落盘"。
//
// 为什么写任务名而不是只写标志位：任务名由 FreeRTOS 常量给出，拷进 RTC
//   慢存里是安全的（不指向任何易失内存）。若栈溢出发生在**别的**任务上，
//   这个名字就是唯一能区分的线索 —— "是我怀疑的 loopTask" 还是"是我从没
//   怀疑过的 ctmon"，排查方向完全不同。
//
// ⚠️ 这里的写法刻意保守：不用 snprintf / 不用 String / 不递归。
//   因为它可能在**任意**任务的栈已经损坏时被调用，任何复杂的调用链都可能
//   二次溢出，反而把唯一的机会也弄丢。strncpy + 手动补 0 是这里能用的
//   最重的手法。
static void ct_copyTaskName(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// 钩子本体在 lvgl_s3.ino 里（唯一定义，理由见该处注释）。这里只声明。
// 放在文件末尾而不是结构体旁边，是为了让上面 ct_mark / ct_begin 等
// inline 函数先看到 ct_rec 的 extern 声明。
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName);

#endif  // CRASH_TRACE_H
