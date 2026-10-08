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
} CrashTrace;

static RTC_DATA_ATTR CrashTrace ct_rec = { 0, 0, 0, 0xFFFFFFFFUL, 0, 0, 0, 0 };

// 心跳计数**不能是 static** —— bt_link.cpp 也 include 这个头文件（那里要在
// btLinkKeepAlive() 里报活），static 会让每个 .cpp 各得一份自己的副本，
// 主任务推进的那份和监测任务读的那份就成了两个变量，卡死监测会永远失灵。
// 唯一的定义放在 lvgl_s3.ino 里，这里只做声明。
extern volatile uint32_t    ct_heartbeat;

// 上一次运行的现场快照。ct_begin() 会把 ct_rec 清成本次的状态，
// 所以开机诊断（showBootDiagnostics）要读的是这份快照。
static CrashTrace ct_prev = { 0, 0, 0, 0xFFFFFFFFUL, 0, 0, 0, 0 };

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

#endif  // CRASH_TRACE_H
