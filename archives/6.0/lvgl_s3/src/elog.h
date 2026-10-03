/**
 * @file elog.h
 *
 * 板载错误日志（Error Log）—— 为"只有一个 USB 口"这件事而设计。
 *
 * 为什么要这个：
 *   这块板子只有一个 USB 口。刷成 HID 键盘之后，这唯一的口就被键盘占死了，
 *   PC 上既看不到 UART0 的串口日志、也没有第二个口能插 USB-TTL。
 *   结果就是：**固件出了错，屏上什么都不显示，用户只能靠猜**
 *   ——"按某个键就重启""灯不亮""屏幕黑掉"，全都没法定位。
 *   之前唯一的排查手段是网页那套 BLE 日志面板（LOG:on / LOG:dump），
 *   但它要求电脑连着蓝牙、开着网页，现场根本用不上。
 *
 *   所以把日志搬到**芯片自己的 flash**上：出错就写进 SPIFFS，
 *   重启之后从屏幕菜单里直接翻出来看。不依赖 USB、不依赖蓝牙、不依赖电脑。
 *
 * 分三层，各管一件事：
 *   1) RAM 环形缓冲（elog.h 的 recs 数组）
 *      所有条目先落这里，屏幕直接读它。**不碰 flash**，所以记录一条错误
 *      的开销是几十纳秒级的 memcpy —— I2C 每 500ms 探一次、失败就写一条
 *      这种高频路径，甩 flash 是绝对不行的。
 *   2) 去重（elog_add 里比对最新一条）
 *      同一条错误反复出现时**不新增条目**，只把它的 count 加一。
 *      "总线失联 x127 次"一条就够，占一条槽位而不是把 64 条全刷掉。
 *   3) 落盘（elog_flush，写 /elog.txt）
 *      写 flash 有磨损，所以只在两种情况下写：
 *        · 距离上次落盘超过 ELOG_FLUSH_MIN_MS（默认 60 秒）**且**有变化
 *        · 明确要求时（清空日志、用户主动重启前）
 *      而且"有变化"的判定是**按次数的二次幂**：第 1、2、4、8、16… 次才写。
 *      于是一个持续报错的设备，一天也只落盘几次，而不是每 60 秒一次。
 *
 * 崩溃怎么记：
 *   panic / 卡死发生的那一刻，flash 是写不进去的（芯片直接复位）。
 *   所以现场仍然靠 crash_trace.h 的 RTC 慢存兜底；等**下次开机**时
 *   elog_begin() 之后由 elog_notePrevRun() 把"上次是怎么死的"补写成一条
 *   普通日志。复位原因（异常崩溃 / 看门狗 / 卡死）+ 阶段 + 当时按的键
 *   都在这一条里 —— 菜单里翻到它，就等于拿到了一次崩溃的完整描述。
 *
 * 磁盘格式（纯文本，方便用工具直接 cat 出来看）：
 *   #ELOG1
 *   <epoch秒>|<级别0-2>|<次数>|<标签>|<正文>
 *   ……按时间从旧到新
 *   读取时逐行解析，**任何一行不合规就跳过**，所以写到一半掉电也不会
 *   导致整个日志读不出来（最多丢最后一条）。
 *
 * 线程安全：只允许主任务（loop）调用。刻意不做互斥锁——
 * elog_add 在 I2C 探测这类高频路径上，锁的开销不划算；而真正会跨任务的
 * 崩溃现场（ct_monitor_task）只写 RTC 慢存，不走这里。
 */

#ifndef ELOG_H
#define ELOG_H

#include <Arduino.h>
#include <stdint.h>

// 级别。数值直接写进磁盘文件，加新级别只能**追加**在后面，不能插中间，
// 否则老日志读出来级别会错位。
#define EL_LVL_ERR   0   // 出错了，功能已经不可用
#define EL_LVL_WARN  1   // 还能用，但有东西不对
#define EL_LVL_INFO  2   // 状态变化记录（启动、退出、配置被改……）

// 条目容量。64 × 92 字节 ≈ 5.9KB，放内部 DRAM 静态数组里。
// 刻意用静态数组而不是 ps_malloc：启动早期的错误（USB 初始化失败、
// SPIFFS 挂不上）恰好是最该记下来的那批，那时 PSRAM 还没确认可用，
// 而且静态数组不存在分配失败 → 日志静默失效的路径。
#define ELOG_CAP        64
#define ELOG_TAG_MAX    8      // ASCII 标签，如 I2C / SHT / FS / USB
#define ELOG_MSG_MAX    72     // 正文，UTF-8 中文按 3 字节/字算，够 20 多字
#define ELOG_FILE       "/elog.txt"

// elog_begin() 会把"SPIFFS 挂载之前已经记进内存的那几条"暂存下来、
// 读完盘再接回末尾。挂载前只可能记 USB / I2C / 温湿度 / NVS 这几处初始化
// 失败，8 条绰绰有余；用静态数组而不是栈上数组，是为了不给 setup() 添风险。
#define ELOG_PRE_MAX    8

// 自动落盘的最小间隔。60 秒 × 只在有"值得记"的变化时写 ≈ 每天几十次上限，
// 对 1.5MB 的 spiffs 分区来说磨损可以忽略。
#define ELOG_FLUSH_MIN_MS 60000UL

typedef struct {
    uint32_t stampMs;                 // 记录时的 millis()
    uint32_t epoch;                   // 墙钟秒数；0 = 当时系统时间还没校准
    uint16_t count;                   // 同一条被合并的次数（去重计数）
    uint8_t  level;                   // EL_LVL_*
    char     tag[ELOG_TAG_MAX];       // ASCII，'\0' 结尾
    char     msg[ELOG_MSG_MAX];       // UTF-8，'\0' 结尾
} ELogRec;                            // 实测 92 字节

// 从 SPIFFS 把上次的日志读进 RAM 环形缓冲。必须在 SPIFFS.begin() 之后调。
// 文件不存在 / 损坏都只是"日志为空"，不会返回错误。
void elog_begin(void);

// 记一条。tag 用 ASCII（模块名），msg 是 UTF-8 正文，支持 printf 格式化。
// 和最新一条 tag+msg 完全相同时不新增条目，只把它的 count 加一。
void elog_add(uint8_t level, const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

// loop() 里定期调：到点且有值得记的变化时才真正写 flash。
void elog_tick(void);

// 立刻把当前全部条目写盘（清空日志、主动重启前用）。
void elog_flush(void);

// 清空 RAM 环形缓冲并删掉磁盘文件。
void elog_clear(void);

// 有效条目数 / 第 i 条（0 = 最老）。返回的指针指向模块内部缓冲，
// **只在主任务里、且在下一次 elog_add/elog_clear 之前**有效。
int  elog_count(void);
const ELogRec* elog_get(int idx);

// 其中 EL_LVL_ERR 的条数 —— 菜单角标显示用。
int elog_error_count(void);

// 墙钟是否可信（系统时间校过）。不可信时屏幕上显示"运行 +N 分"而不是时间戳，
// 免得看到一排 1970-01-01 还以为日志坏了。
bool elog_wallValid(void);

// 便捷宏
#define ELERR(tag, ...)  elog_add(EL_LVL_ERR,  tag, __VA_ARGS__)
#define ELWARN(tag, ...)  elog_add(EL_LVL_WARN, tag, __VA_ARGS__)
#define ELINFO(tag, ...) elog_add(EL_LVL_INFO, tag, __VA_ARGS__)

#endif  // ELOG_H
