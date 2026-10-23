/**
 * lvgl_s3.ino — YYQ-MX9.0 LVGL 核心版
 *
 * 简约纯黑风格极客仪表盘
 * - 深黑背景 + 白色/青色文字
 * - 完整键盘矩阵 + USB HID
 * - 时间 + 温湿度显示
 * - 简化菜单
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_MCP23X17.h>
#include <Preferences.h>
#include <driver/timer.h>
#include <driver/rtc_io.h>   // rtc_gpio_hold：深睡期间按住 BOOT 脚（BOOTDL 用）
#include <esp_task_wdt.h>   // 只为了 setup() 里那三行"拆看门狗"，别处不再碰它
#include <SPIFFS.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>          // 配置 JSON 的序列化/解析（v7 API）
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// 蓝牙链路统一收发层。**蓝牙的收和发都只应该经过它**：
// 发的时候它负责按 20 字节上限分片、片间留缝、关键响应开独占窗口，并且把
// 发包摊到多次 loop 上（不然一条 8KB 回拉会把任务看门狗喂爆重启）；
// 收的时候它负责在 BLE 回调里喂狗、把裸字节旁路给二进制协议、按行攒指令，
// 再在 loop() 里派发 —— 所有 LVGL 操作仍然只发生在主任务。
// 协议细节和用法见 src/bt_link.h。
#include "bt_link.h"

// 诊断日志走 UART0（板载 USB-TTL，烧录用的那个口，PC 上是 COM4）。
//
// 为什么不直接用 Serial：platformio.ini 里设了 ARDUINO_USB_CDC_ON_BOOT=1，
// 这时 Serial 指的是芯片**原生 USB** 的虚拟串口。原生 USB 只有在
// USB-CDC 的第一个描述符握手完成之后才会枚举出来，Windows 往往要十几秒
// 才认这个口，而这期间所有日志都丢了 —— 表现为"监听 90 秒一行都没有"。
// UART0 是烧录口，一直在线，插上就能抓。
//
// 但刷成 HID Keyboard 后**两个 USB 口都被占用**(或者烧录用的 USB-TTL
// 在很多笔记本上根本不亮 COM 口)，调试时基本看不到任何日志。退路是
// LogMirror：把 LOG_PORT 包成 Print 代理，所有 printf 字节既写到 UART0，
// 又按行落进 PSRAM 环形缓冲，再通过 BLE notify (`LOG:<text>` /
// `LOGDUMP:<chunk>`) 实时回传到网页日志面板。详见 pushLogLine /
// handleCommand 里 LOG:on / LOG:off / LOG:dump / LOG:clear 四个分支。
// 这里只声明 LogMirror 并把 LOG_PORT 指向它，全文 LOG_PORT.printf 不动。
//
// BLE 一条通知能带多少字节 = 协商后的 ATT MTU - 3。协商失败时（手机端
// 浏览器很常见）只有 23-3 = 20 字节，**超出的不是被截断而是整条被协议栈丢掉**。
// 分片、收尾标记、片间间隔全在 bt_link 里，这里不再各写一份。

// LogMirror::write() 收完一行就调 pushLogLine()。后者函数体在 BLE 全局段
// 之后（因为要用 btLinkStream），这里先前置声明。
static void pushLogLine(const char* line);

// ---------------------------------------------------------------- USB CDC 日志镜像
//
// 为什么要有它：
//   LOG_PORT 走的是 **UART0（GPIO43/44）**，没接 USB-TTL 就什么都看不见。
//   而这块板子的 USB 口已经被 HID 键盘占着，"再烧一个纯 HID 程序"就更没法看日志。
//   这里把同一份日志**再镜像一份**到 USB CDC（ARDUINO_USB_MODE=1 时 Serial
//   就是 USB-Serial-JTAG），于是插一根 USB 线就能看到全部日志。
//
// ⚠⚠ **绝对不能直接在主循环里 Serial.write()**：
//   HWCDC::write() 在环形缓冲满、而主机又不读的时候，会**阻塞最多
//   tx_timeout_ms（100ms）**。一行日志一次阻塞 100ms，主循环直接被卡死 ——
//   那就等于为了看日志把"键盘卡死"这个 bug 重新引入了一遍。
//   所以：生产者只做一次非阻塞的入队，真正的 write 交给一个**独立任务**。
//
// ⚠⚠⚠ **这里只能有声明，不能有函数定义！**
//   Arduino 构建器生成 .cpp 时，会把全文件的函数原型插到
//   **"第一个函数定义之前的最后一个 #include"** 后面。
//   本文件在那一带还**没有** #include <lvgl.h>（它在文件更后面），
//   所以只要在 include 之前放一个函数定义，原型就会被插到 lvgl.h 之前 ——
//   那批原型里有 lv_obj_t 形参，于是**整个文件都编译不过**
//   （报 "'lv_obj_t' does not name a type"，看起来像是别处坏了）。
//   所以：定义一律放到所有 include 之后（见本文件后面的 logCdcTask/logCdcPush）。
//
// 队列满了就**丢掉这一段**（xQueueSend 满了直接返回 false）——
// 日志的价值在"最近发生了什么"，但一串日志比一条都看不到更糟，所以满了宁可丢。
// 一段 128 字节 × 32 段 = 4KB，落在 .bss。
// ⚠⚠ **默认关掉**（LOG_CDC_MIRROR = 0），原因有三个：
//
//   ① 在**当前这套编译配置**下它根本没有用：
//      platformio.ini 里是 ARDUINO_USB_CDC_ON_BOOT=1，于是框架里
//      `Serial` 就是 **UART0**（HardwareSerial Serial(0)），
//      而 USB 被 TinyUSB 的 HID 占着 —— macOS 上**看不到任何 CDC 节点**，
//      /dev/cu.usbmodem* 直接消失。这段镜像写的是 UART0，不是 CDC。
//   ② 它让 UART0 上的日志**每个字节出现两次**（见 LogMirror::write(buf,size)）。
//   ③ 它让**两个任务**同时写同一个 HardwareSerial（主任务 + logCdcTask），
//      而 Arduino 的 HardwareSerial 不是线程安全的 —— 两个写者会互相把
//      缓冲写花，日志变成乱码。
//
//   要用的话：把下面这行改成 1，**并且**把 platformio.ini 的
//   ARDUINO_USB_MODE 换成 0（TinyUSB CDC），那时 Serial 才是真的 USB CDC。
//   届时机身上的 USB 会变成 HID+CDC 复合设备，插上就能看到日志。
//
// ⚠ 用 #ifndef 包起来，是为了能用编译选项覆盖：
//   platformio.ini 里加 -DLOG_CDC_MIRROR=1 就能临时开，不用改源码。
#ifndef LOG_CDC_MIRROR
#define LOG_CDC_MIRROR 0
#endif

#define LOG_CDC_BLOCK   128
#define LOG_CDC_BLOCKS  32

#if LOG_CDC_MIRROR
static QueueHandle_t s_logCdcQueue = nullptr;

static void logCdcTask(void* arg);
static void logCdcPush(const char* data, size_t len);
#endif

class LogMirror : public Print {
    HardwareSerial* real;
    char    lineBuf[BT_RX_BUF_SIZE];
    size_t  lineLen;
public:
    LogMirror(HardwareSerial* r) : real(r), lineLen(0) {}

    // Serial0.begin() 这种成员转发：LOG_PORT.begin(115200) 必须能落到 UART0
    void begin(unsigned long baud) { if (real) real->begin(baud); }
    void end() { if (real) real->end(); }

    // 行组装：把一个字节并进当前行，满一行就交给 BLE 日志通道。
    // 抽成私有函数，是为了让 write(buf,size) **不要**去调 write(uint8_t) ——
    // 那条路会把每个字节**再写一遍** real，UART0 上每个字节都会出现两次
    // （整块写一次 + 逐字节再写一次）。以前就是这么写的。
    void feedChar(char c) {
        if (c == '\n') {
            // 遇到 \n 才提交一行；\r 单独出现也提交一次（兼容 println 的 \r\n）
            lineBuf[lineLen] = '\0';
            pushLogLine(lineBuf);
            lineLen = 0;
        } else if (c == '\r') {
            // 忽略，\n 那一拍已经处理过
        } else if (lineLen < sizeof(lineBuf) - 2) {
            lineBuf[lineLen++] = c;
        } else {
            // 行太长，截断并把当前缓冲当成完整一行推出去（兜底，防止 lineLen 溢到下一行）
            lineBuf[sizeof(lineBuf) - 2] = '\0';
            pushLogLine(lineBuf);
            lineBuf[0] = c;
            lineLen = 1;
        }
    }

    size_t write(uint8_t c) override {
        if (real) real->write(c);
#if LOG_CDC_MIRROR
        logCdcPush((const char*)&c, 1);      // ← USB CDC 镜像（非阻塞）
#endif
        feedChar((char)c);
        return 1;
    }

    size_t write(const uint8_t* buf, size_t size) override {
        if (real && size) real->write(buf, size);
#if LOG_CDC_MIRROR
        logCdcPush((const char*)buf, size);   // ← USB CDC 镜像（非阻塞）
#endif
        // 行组装自己走一遍，**不调 write(uint8_t)**（那会把字节写两遍）。
        for (size_t i = 0; i < size; i++) feedChar((char)buf[i]);
        return size;
    }
};

// 全局唯一代理：所有 LOG_PORT 都走它，再转发到 Serial0 + 落环形缓冲
static LogMirror LogPortProxy(&Serial0);
#undef  LOG_PORT
#define LOG_PORT LogPortProxy

// panic 现场留存（ROM 输出接管 + RTC 慢存）见 panic_trace.h。

// === BLE 日志通道缓冲 ===
// PSRAM 上分一块 8KB 环形缓冲（按行存，超出时丢最老的整行）。
// 板子是 N16R8 = 8MB OPI PSRAM，8KB 占比 0.1%，随便用。
#define LOG_BUF_CAP     8192
static char*  logRingBuf   = nullptr;   // ps_malloc，setup() 里建
// 日志环缓冲的互斥锁。
//
// ⚠ 为什么必须要有：写日志的**不止主循环**——
//   BLE 的 GAP / GATT 回调跑在**蓝牙协议栈自己的任务**里，
//   那里一打日志就会和主循环并发改 logRingLen、并发 memmove 整个缓冲，
//   轻则日志错乱（UART0 上已经能看到两行字符交错的现象），
//   重则写越界踩坏 PSRAM —— 这正是"偶尔重启"的一类成因。
//
//   拿不到锁就**丢这一行**：日志晚点还会再来一条，
//   但卡住蓝牙协议栈任务（会拖慢整个 BLE 栈）严重得多。
static SemaphoreHandle_t s_logMutex = nullptr;
static size_t logRingLen   = 0;
static bool   logStreamOn  = false;     // LOG:on / LOG:off 控制

// === LOG:dump 的回拉状态机 ===
//
// 环形缓冲会被新日志不断改写（满了就 memmove 丢最老的整行），而回拉要花
// 1~2 秒 —— 这期间新日志还在往里写，直接对着 logRingBuf 读，读到的位置
// 早就被顶歪了，回拉出来是错位的行。所以开始时整段 memcpy 一份快照
// （同样放 PSRAM，8KB），回拉期间只读快照。
//
// ⚠ 必须放 PSRAM，不能放 .bss：内部 DRAM 只有 327KB，而项目历史上最主要的
//   重启原因就是内部堆被吃干（见 lv_mem_port.h 和 elog 的 MEM 告警）。
static char*  logDumpSnap  = nullptr;   // ps_malloc，setup() 里建
static size_t logDumpLen   = 0;         // 快照里有多少字节要发
static size_t logDumpPos   = 0;         // 已经喂给 bt_link 多少字节
static bool   logDumpActive = false;    // true = 正在流式回拉

// 每轮 loop 推一段快照给 bt_link。缓冲一满就返回，下一轮再来 —— 于是
// 8KB 回拉变成"每轮喂 ~160 字节、发 ~3 片"，主循环全程还在扫键盘、刷屏幕、
// 喂看门狗。函数体在 btLinkStreamWrite 之后（下面 LOG 控制那一段）。
static void logDumpPump(void);

// ============================ 配置 JSON（CFG*）===========================
//
// 为什么要有这一层：在它出现之前，网页上每一个小设置都是一条**独立**的 BLE
// 指令（DISP_MODE: / TIME: / ALARMSET: / TIMERSET: / REMAP: …），对应 NVS 里
// 30+ 个独立键。后果有三个，而且都绕不开：
//   1. 网页连上键盘后看到的永远是页面初始默认值，不是键盘里的真实值
//      —— 因为根本没有哪一条指令能一次把"所有设置"读回来
//   2. 想给键盘做份备份只能一条条抓，漏一条就是一份残缺的备份
//   3. 每加一个设置就要加一条指令 + 一个 NVS 键，两边都得跟着改
//
// 这一层把"小配置项"收敛成**一个 JSON**：固件能一次吐出来（CFGGET），
// 网页改完再一次塞回去（CFGBEGIN / CFGDATA / CFGEND）。
//
// ⚠ NVS 键**一个都没动** —— 还是 disp_mode / brightness / alarm_h 那套。
//   换固件、回滚、刷分区都还认原来那些键；这里只是多了一条"整体读写"的路，
//   不是把存储格式换掉。（真要换成单键 JSON blob 是下一步的事，现在先别动。）
//
// ---- 线上格式 ----
// 主 → 键盘：
//     CFGBEGIN:<总字节数>      开一段接收，后面跟若干片
//     CFGDATA:<正文片段>       每片正文 ≤ CFG_RX_CHUNK_MAX 字节
//     CFGEND#<请求号>         收尾。固件解析+落盘后回
//                             CFGSAVE:<请求号>:OK:<字段数>
//                             或 CFGSAVE:<请求号>:ERR:<原因>
// 键盘 → 主：
//     CFGDUMP:<请求号>:<json片段> ×N ，末尾跟 CFGDUMP:<请求号>:END
//
// ---- 增量（只改其中几项）----
//
//     CFGPATCH:<总字节数>     和 CFGBEGIN 一样开一段，区别只在收尾
//     CFGDATA:<正文片段>
//     CFGPATCHEND#<请求号>    收尾，正文是**整份 JSON 的一个子集**，语义
//                             逐条与整份完全一致（见 cfgApplyDoc）：
//                               · 标量键出现 = 写这一项
//                               · <块>.remap   出现 = 整份替换该方案的映射
//                               · <块>.macros.<键> 出现 = 写/删这一个宏
//                               · gkeys.<槽>       出现 = 写/删这一个全局键
//                                 （MA / MB / MAa / MAb / MBa / MBb / M1~M12）
//                             没提到的键一律**不动**
//
// ⚠ 增量**不带 version**，也不做版本闸门：补丁的语义本来就是"只动我点名的
//   那些"，网页拿旧版页面的补丁打过来顶多是少改几项，不该整份拒掉。真要对齐
//   版本，走整份那条路（version 对不上会明确报错）。
//
// 为什么值得单独开一条路：改一项（比如只切显示风格）本来要传几 KB——键盘那边
//   收几百毫秒，网页这边传完还要再整份拉回来确认一趟。改成补丁后就是几十字节，
//   而且**不带 disp_mode 就不会挂整屏重建**，"改个亮度键盘自己跳到设置页"
//   这件事在根上就没有了。
//
// ⚠ CFGDATA 的正文里**绝对不能有 \n 和 \r**：接收侧按行攒（见 bt_link 的
//   btLinkOnWrite），混进换行会被切成两条指令，JSON 被劈成两半，两边都只会
//   报"解析失败"而看不出真因。所以网页发之前必须压成一行（JS 那边用
//   JSON.stringify 不带缩进）。这一条比看上去要紧：一个缩进过的 400 字节
//   JSON 直发必然失败。
// v1 = 十三个标量小配置。
// v2 = 加上**方案那一整套**：remap（4 方案 × 最多 32 条）、macros（4×12）、
//      gkeys（MA/MB 双状态 + 相位）。也就是"除了 ME 文本和壁纸，
//      键盘里存的配置全部走这一份 JSON"。
#define CFG_SCHEMA_VERSION    3

// 缓冲大小。
//
// ⚠⚠ 这里原来是 .bss 上的 1536 / 2048 固定数组，**加进方案就不够了**：
//   48 个宏 × (键名 7 + 引号 2 + 正文上限 CFG_MACRO_MAX) ≈ 12.7KB
//   4 方案 × 32 条 remap × `{"from":255,"to":255}` ≈ 3.3KB
//   gkeys 18 个 payload × CFG_GKEY_MAX ≈ 4.5KB
//   标量 ≈ 0.4KB
//   → 最坏 ~18KB。而 18KB × 2（收发各一份）硬塞进 .bss 会白吃 36KB 静态 RAM，
//   这块板子内部 DRAM 本来就紧（构建时报 29.5%）。所以改成**按需 ps_malloc**。
//
// 20KB 是够的：上面算出来的最坏值约 18KB，留了两成余量。分配失败会明确回
// CFGERR，绝不"截断成一份看着挺像样的半份 JSON"再发出去 —— 那种错在网页那边
// 报成 "unexpected end of input"，极难查（见 cfgBuildJson 的注释）。
#define CFG_BUF_SIZE          20480

// 单个宏 / 单个全局动作 payload 的正文上限。
// 取 256 是因为**收侧一行的物理上限就是 256 字节**（BT_RX_BUF_SIZE）——
// 老协议一条 SET: 指令最多也就带 256 字节，所以磁盘上不可能有更长的。
// 这里照同一个数收，两边口径一致；超了直接报错，不静默截断。
#define CFG_MACRO_MAX         256
#define CFG_GKEY_MAX          256

static char* cfgTxBuf = nullptr;    // 组 JSON 用的缓冲（ps_malloc）
static char* cfgRxBuf = nullptr;    // 收 JSON 用的缓冲（ps_malloc）
// cfgBuildJson 失败时的原因。**必须**带出来而不是只回一句"生成失败"：
// 失败原因有好几种（文档装不下 / 缓冲不够 / 含换行），对应的处理完全不同，
// 而网页过去把 CFGERR 整个丢掉了，最后统一显示成"一个字节都没收到"。
static char  cfgBuildErr[64] = {0};

// 收发两个缓冲各要一份，首次用到时分配、之后一直留着（进程生命周期内不释放：
// 释放了又得处理"释放后有人还在用"的窗口，收益为零）。
// 函数体在下面"配置 JSON：一张表驱动"那一段（elog.h 是后段才 include 的，
// 这里用不了 ELERR）。

// 一条 CFGDATA 能带多少正文。收侧单行上限是 BT_RX_BUF_SIZE(256)，
// 去掉 "CFGDATA:"(8) 和结尾的 '\0'，实际能装 247 —— 取 200 留足余量，
// 顺便让网页那边按 200 切的时候不必贴着上限算。
#define CFG_RX_CHUNK_MAX      200
// 和 ME 上传一样：网页发一半跑了，不能一直占着 cfgRxBuf。
#define CFG_RX_TIMEOUT_MS     5000UL

static uint32_t     cfgRxLen    = 0;    // 已攒字节
static uint32_t     cfgRxTotal = 0;    // CFGBEGIN 声明的总长
static bool         cfgRxActive = false;
// 这次收的是**增量**（CFGPATCH 开的）还是**整份**（CFGBEGIN 开的）。
// 收包流程两者一模一样，只有收尾时调的应用函数不同，所以单独记一笔。
static bool         cfgRxPatch  = false;
static unsigned long cfgRxLastMs = 0;

// 下载侧：cfgTxBuf 的哪一段还没喂给 bt_link。
static size_t cfgTxPos = 0;
static size_t cfgTxLen = 0;
static bool   cfgTxActive = false;

// set_epoch 原来只有 NVS 键、没有内存副本（每次用都现读一遍 NVS）。配置 JSON 要
// 把"当前时间设置"当成一个普通字段读出来，就得有这份副本。
static uint32_t epochCache = 0;

// 待执行的界面副作用（位掩码）。
//
// ⚠ 这几个为什么**不能**在 CFGEND 的处理分支里一口气跑完 —— 这是本模块第一版
//   把键盘搞崩溃的直接原因：
//
//   一份"整份拉回来的配置"里 disp_mode / saver_mode / curr_prof / set_epoch
//   全都带着，于是第一条指令就会连着做：
//     applyDispMode()  → renderCurrentDisplayBase()：删掉 7 个风格容器再整个
//                        按当前风格重建（整块屏里最重的一件事）
//     setScreensaverMode() → 可能再 destroyMainScreen() + enterScreensaver()
//                        + build_menu()（13 项菜单又是重建一整屏）
//     switchProfile() → updateTopBarProfile()
//   两三次全屏拆建 + 13 次 NVS 页写入挤在同一条指令里，而 loop() 身上压着
//   10 秒任务看门狗（trigger_panic = true）—— 中间**一次狗都没喂**，于是
//   "保存配置 → 键盘重启"。网页那边只看到 CFGSAVE 等不到回应，就是这个。
//
// 所以改成：赋值 + 落盘在指令里做完（每次写之间喂狗），界面重建**摊到后面的
// loop 里，一轮只做一件**，中间让键盘扫描、LVGL、看门狗都轮得到。
#define CFG_PEND_CLOCK   0x01
#define CFG_PEND_DISP    0x02
// ⚠ 原来还有 CFG_PEND_PROF 0x04。curr_prof 摘出 CFG_FIELDS 之后已经没人
//   置它了，位也空出来留给下一个。方案切换的 UI 刷新归 switchProfile() 自己管。
#define CFG_PEND_SAVER   0x08
// 映射表"没存住"的红色提示。它不是上面那几个副作用之一，但同样是**界面**动作，
// 同样必须在 cfgPostPump 里出，不能在配置下发的那条指令里直接建 HUD —— 理由见下面
// cfgQueueRemapHud 的注释。
#define CFG_PEND_RMHUD   0x10
static uint8_t cfgPend = 0;

// remap 的 HUD 正文。只有 CFG_PEND_RMHUD 挂着的时候才有意义，存不下就截断。
static char cfgRemapHudMsg[48] = { 0 };

// 映射落盘失败时，**只记正文、不建 HUD**。
//
// 为什么不能在这里直接 triggerHud()：本文件里所有"配置保存后的界面动作"都被刻意
// 挪到了 cfgPostPump（见 :248 那段），因为把界面工作内联进 CFGEND 的处理分支正是
// 本模块第一版把键盘搞崩溃的直接原因。cfgStoreRemap 是最后一条漏网的 —— 它在
// handleCommand 的调用栈里（此时 ArduinoJson 的 doc 还占着几 KB 内部堆）直接
// lv_obj_del + lv_obj_create 建 HUD。
//
// 现场表现和它完全吻合：**只有清空方案 / 删一条映射会崩**，别的保存都不崩 ——
// 因为只有这条路径会在下发过程中建 HUD，而它又只在 NVS 写不下时才走到。
// （宏和全局键写不下时只 ELWARN，不建 HUD，所以那两条路没事。）
//
// ⚠⚠ 这里只有前置声明，函数体在 cfgPostPump 旁边。位置不是随便挑的：本文件靠
// Arduino 的自动原型生成器补声明，而它把原型块插在**第一个函数定义之前**。
// 把函数定义放在这里就等于告诉生成器"从这儿开始插" —— 而 `#include "lvgl.h"`
// 还在几百行之后，于是生成器插出来的那一堆 `void setBgColor(lv_obj_t*)` 全部
// 编译不过（'lv_obj_t' does not name a type，报的行号正好落在这一段）。
// 文件里其它函数也都遵守这条：靠前的一律只留声明。
static void cfgQueueRemapHud(const char* msg);

// 实现都在 handleCommand 前面那一段（"配置 JSON"），loop() 里只调 cfgJsonPump
// 和 cfgPostPump。
static bool   cfgBufEnsure(void);
static size_t cfgBuildJson(char* out, size_t cap);
// isPatch=false：整份（必须带 version，做版本闸门）
// isPatch=true ：增量（不带 version，只动正文里点名的那些项）
static int    cfgApplyDoc(const char* json, size_t len, bool isPatch, char* err, size_t errCap);
static void   cfgJsonPump(void);
static void   cfgPostPump(void);

// ⚠ 存不进去的项累积在这里，cfgApplyDoc 填、handleCommand 读，最后**如实报
//   给网页**。
//   Preferences::put* 写不下新键时只返回 0、不抛异常（见 setup() 里的可写性
//   自检：nvs 只有 20KB，且是只增不减的日志结构）。而"页面显示已保存、断电
//   后配置全没了"是这个功能从一开始就在消灭的那类"界面说成功、其实没生效"，
//   所以这里绝不能把失败吞掉 —— 收件方必须能区分"存住了"和"以为存住了"。
//
//   做成文件级而不是给 cfgApplyDoc 加返回参数：那个函数已经返回"应用了几项"
//   这一个 int 了，装不下"存了 N 项、其中 M 项没存住"；为捎一句话改签名会让
//   每一个调用点都得跟着改。
static int  nNvsFail = 0;                       // 没存住的新键数量
static char cfgFailNames[160] = {0};            // 没存住的键名，逗号分隔

// 往失败名单里追加一个键名。
//
// 做成宏而不是函数：.ino 的 Arduino 预处理是拿正则切函数的，函数体放在
// 前向声明区会被当成"这里该有个函数定义"，后面整段全被当成嵌套定义直接
// 编译不过（a function-definition is not allowed here）。宏没这个问题。
//
// ⚠ 名单满了就**只截断名单，计数照加**。用户真正要知道的数字是"有 N 项没
//   存住"，名单只是点名几个给他看；反过来把计数卡在 160 字节以内，那才是
//   真的把问题藏起来了。
//   顺带一个坑：u 超出数组长度时 `sizeof(...) - u` 是 **size_t 下溢**，
//   会变成天文数字，于是 snprintf 拿着一个越界指针和"随便写多少"的尺寸
//   去写 —— 直接把内存踩烂。所以这里必须先判 u 再算剩余空间。
#define CFG_FAIL_ADD(name)  do {                                           \
        nNvsFail++;                                                        \
        size_t _u = strlen(cfgFailNames);                                  \
        if (_u + 1 < sizeof(cfgFailNames))                                 \
            snprintf(cfgFailNames + _u, sizeof(cfgFailNames) - _u,         \
                     "%s%s", _u ? ", " : "", (name));                      \
    } while (0)

// cfgApplyDoc 要跑显示风格的副作用，而 applyDispMode 的定义在它后面。
static void   applyDispMode(uint8_t mode);


// 推一行进环形缓冲；若 LOG:on 已开，按行推一次流转发（节流在 bt_link 里）
// 函数体在文件下方，这里先声明供 LogMirror::write 调用。
static void pushLogLine(const char* line);

// USB HID
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDSystemControl.h"
#include "USBHIDVendor.h"

// BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "lvgl_st7789_driver.h"
#include "lv_conf.h"
#include "crash_trace.h"
#include "panic_trace.h"
#include "elog.h"
// 锁屏那把大锁。RGB565 数组直接躺在 flash 里（gen_lock_img.py 生成），
// lv_img_dsc.data 一指就完事 —— 不解码、不拷贝、不占 PSRAM。
#include "lock_img.h"

// ct_heartbeat 的**唯一定义**。crash_trace.h 里只有 extern 声明，因为
// bt_link.cpp 也要靠它报活（见 ct_alive 的注释）—— 放成 static 就废了：
// 每个 .cpp 会各得一份副本，主任务推进的和监测任务读的就成了两个变量。
volatile uint32_t ct_heartbeat = 0;

// ct_rec / ct_prev 的唯一定义，原因同上（栈溢出钩子要写 ct_rec，
// 而 bt_link.cpp 也 include 了 crash_trace.h）。
RTC_DATA_ATTR CrashTrace ct_rec = { 0, 0, 0, 0xFFFFFFFFUL, 0, 0, 0, 0, 0, 0, {0} };
CrashTrace ct_prev = { 0, 0, 0, 0xFFFFFFFFUL, 0, 0, 0, 0, 0, 0, {0} };

#include <esp_heap_caps.h>
#include <nvs.h>          // nvs_get_stats：开机报告分区还剩多少槽位

#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>
#include "profile_icons.h"   // 4 方案 × 3 档 (S/M/L) RGB565 图标，由 mmx 生成
// 「方案+按键」页专用的一套图标：白色咬口苹果 + 蓝色 Windows 四格旗。
// 与 profile_icons.h 的区别：那套是 mmx 生成位图，48px 下苹果是一坨认不出的
// 白团、Windows 还是青绿渐变；这套是**矢量路径栅格化**出来的，边缘绝对干净，
// 而且配色是用户点名要的白 / 官方蓝 #0078D4。生成器是 gen_style_icons.py。
#include "style_icons.h"

// ===========================
// USB CDC 日志镜像的实现
// ===========================
//
// ⚠ 必须放在**所有 include 之后**：见前面 logCdcPush 声明处的那段警告 ——
//   Arduino 构建器会把函数原型插到"第一个函数定义之前的最后一个 #include"后面，
//   在这里放定义就等于把原型的位置推到 lvgl.h 之前，整份文件都编译不过。
#if LOG_CDC_MIRROR
static void logCdcTask(void* arg) {
    (void)arg;
    char block[LOG_CDC_BLOCK];
    while (true) {
        // ROM 的 printf（IDF 日志 / assert / panic 回溯）先进内存环，
        // 这里**统一**搬进 CDC —— 整个固件只有这一处写 TinyUSB，
        // 不会出现"日志镜像任务和蓝牙协议栈并发写 CDC FIFO"的竞争。
        romRingDrain();
        // 阻塞等：有新日志就写出去；队列空了就睡，不占 CPU
        if (xQueueReceive(s_logCdcQueue, block, portMAX_DELAY) == pdTRUE) {
            size_t n = strnlen(block, LOG_CDC_BLOCK);
            if (n > 0) Serial.write((const uint8_t*)block, n);
        }
        // ⚠ 不喂狗也不出错：日志任务挂了不该影响键盘
        vTaskDelay(1);
    }
}

// 把一段字节塞进 CDC 队列（非阻塞）。队列满 = 丢，绝不阻塞主循环。
static void logCdcPush(const char* data, size_t len) {
    if (s_logCdcQueue == nullptr || data == nullptr) return;
    char block[LOG_CDC_BLOCK];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > LOG_CDC_BLOCK - 1) n = LOG_CDC_BLOCK - 1;
        memcpy(block, data + off, n);
        block[n] = '\0';
        if (xQueueSend(s_logCdcQueue, block, 0) != pdTRUE) {
            // 队列满：丢掉这一段，绝不阻塞主循环
            return;
        }
        off += n;
    }
}
#endif  // LOG_CDC_MIRROR

// ===========================
// 硬件配置
// ===========================
#define TFT_SCL    4
#define TFT_SDA   16
#define TFT_DC    15
#define TFT_CS     5
#define TFT_RST   -1

#define RX_PIN    10
#define TX_PIN     9
#define UART_BAUD 460800

#define I2C_SDA   14
#define I2C_SCL   13

#define SHT31_SDA 17
#define SHT31_SCL  7
#define SHT31_ADDR 0x44

#define MCP23017_ADDR 0x20

// MCP23017 的**硬件复位脚**：SOIC/TSSOP-18 封装是 18 脚，QFN-28 封装是 14 脚。
// 低有效，施密特触发输入（数据手册 D031/D041：VIL≤0.2VDD、VIH≥0.8VDD）。
//
// 板上接法（三件事，缺一不可）：
//   1. MCP23017 的 RESET 脚 → S3 的这个 IO
//   2. RESET 脚 → 10kΩ → 3.3V 上拉      ← 关键，原因见下面「必须有上拉」
//   3. （可选）RESET 脚 → 100nF → GND 做 RC 抗干扰，不加也能工作
//
// 为什么必须接 GPIO：芯片内部状态机卡死时会**一直拉着 SDA 不放**，敲多少个
// 时钟都松不开 —— 时钟救援只对「还在正常走协议、只是总线停在半个字节中间」
// 有效。可数据手册 TABLE 1-2 写死了：RESET 拉低后 **1µs 内所有输出脚进入
// 高阻**（TIOZ ≤ 1µs）。也就是说 RESET 是唯一能让它主动松手的手段，而且
// **不需要断电**，S3 自己一个 IO 就完成了，等于随时能给芯片重新上一次电。
//
// 为什么上拉必须有：S3 自己复位/上电期间这个 IO 是高阻，RESET 就浮空了。
// 数据手册原文 "Must be externally biased" —— 浮空时芯片会被随机复位，表现
// 就是键盘毫无征兆地失灵，正是要治的病。另外 RESET 是**低有效**，接 GND 等于
// 永久按住复位，比接 VCC 严重得多。
//
// ===========================
// MCP23017 的硬件复位脚：接 S3 的 GPIO11。
//
// 接法（两件事，缺一不可）：
//   1. RESET → GPIO11
//   2. RESET → 10kΩ → **3.3V** 上拉   ← 必须接，而且只能接 3.3V
//
// 为什么值得接 GPIO：芯片状态机卡死时会**一直拉着 SDA 不放**，敲多少个时钟都
// 松不开 —— 时钟救援只对「还在正常走协议、只是总线停在半个字节中间」有效。
// 数据手册 TABLE 1-2 写死：RESET 拉低后 **1µs 内所有输出脚进入高阻**。也就是说
// RESET 是唯一能让它主动松手的手段，而且**不需要断电**，S3 自己一个 IO 就完成了。
// mcpHardwareReset() 干的就是这件事：它走复位脚、不经过 I2C，总线已经废了照样生效。
//
// 为什么必须有上拉：S3 在 bootloader 阶段、以及 setup() 里那行 digitalWrite 之前，
// 这个 IO 是高阻。芯片内部只有 **100kΩ 的弱上拉**，节点会长时间悬在中间电压，
// 而 RESET 是施密特触发输入（VIL≤0.2VDD、VIH≥0.8VDD）—— 落在中间就是随机进出复位。
// 表现就是键盘毫无征兆地失灵。10k 才能把这个窗口彻底关掉。
//
// ⚠⚠ 上拉只能接 **3.3V**，绝对不要接模块自己的 VCC。
//   这台机器上踩过：为了"加强上拉"给 SDA/SCL 各加了一个 4.7k 到模块 VCC，
//   结果键盘直接全哑，害得排查方向整个跑偏到复位脚上。那个 VCC 未必是 3.3V，
//   而 ESP32-S3 的 GPIO **不耐 5V（上限 3.6V）** —— 拉到 5V 等于把 5V 灌进主控引脚。
//
//   同理 SDA/SCL **不要**外加上拉：模块板上本来就自带，I2C 能正常工作就是证据，
//   并联更多上拉不会让总线更稳，只会让低电平驱动更吃力。
#define MCP_RESET_PIN 11

// 看门狗已经整体拆掉了（理由见 crash_trace.h 的文件头）。
// 这个常量只为 setup() 里那三行"拆狗"动作留着：万一 TWDT 还被别人初始化过，
// 就把它改成"1 小时超时 + 不 panic"，让"触发即复位"这件事彻底不可能发生。
#define WDT_DISARM_TIMEOUT_S 3600

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
Preferences preferences;

// USB HID
USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDSystemControl SystemControl;
USBHIDVendor VendorHID;

// MCP23017
Adafruit_MCP23X17 mcp;

// SHT31 I2C
TwoWire Wire_SHT(1);

// ===========================
// 宏定义
// ===========================
#define MACRO_BASE 0xF0

// HID 键盘常量（USB HID Usage Table）
#define KEY_PRINT_SCREEN 0x46
#define KEY_SCROLL_LOCK  0x47
#define KEY_PAUSE        0x48
#define KEY_NUM_LOCK     0x53
#define KEY_KP_SLASH    0x54
#define KEY_KP_ASTERISK 0x55
#define KEY_KP_MINUS    0x56
#define KEY_KP_PLUS     0x57
#define KEY_KP_ENTER    0x58
#define KEY_KP_1        0x59
#define KEY_KP_2        0x5A
#define KEY_KP_3        0x5B
#define KEY_KP_4        0x5C
#define KEY_KP_5        0x5D
#define KEY_KP_6        0x5E
#define KEY_KP_7        0x5F
#define KEY_KP_8        0x60
#define KEY_KP_9        0x61
#define KEY_KP_0        0x62
#define KEY_KP_DOT      0x63
// ⚠ 这里**故意没有**"矩阵静音键"这个东西。
// 静音键物理上挂在 C3 那颗小 MCU 上，走 Serial1 的 "BTN:MUTE" 文本命令
// （handleC3Command，见文件中部"C3 串口通道"一节），矩阵里根本没有这一格。
//
// 曾经在这儿写过 `#define KEY_MUTE_USAGE 0xE2`，再拿 baseKey 跟它比。
// Consumer 页的 Mute 确实是 0xE2，但**矩阵用的是另一套编码**：
// Arduino 的 "0x88 + HID usage"。0xE2 = 0x88 + 0x5A = **小键盘 2**。
// 于是小键盘 2 一按就变静音，而且它被那个分支吃掉了，
// 再也落不到下面的普通键盘分支，键盘 2 这个键等于没了。
//
// 别再加回来：10x16 的矩阵里每个有效格子都有主（0~2、4~8 行，行 3 / 行 9 是物理空行），
// 没有多余的键位可以给静音。
#define K_M1   (MACRO_BASE + 1)
#define K_M2   (MACRO_BASE + 2)
#define K_M3   (MACRO_BASE + 3)
#define K_M4   (MACRO_BASE + 4)
#define K_M5   (MACRO_BASE + 5)
#define K_M6   (MACRO_BASE + 6)
#define K_M7   (MACRO_BASE + 7)
#define K_M8   (MACRO_BASE + 8)
#define K_M9   (MACRO_BASE + 9)
#define K_M10  (MACRO_BASE + 10)
#define K_M11  (MACRO_BASE + 11)
#define K_M12  (MACRO_BASE + 12)
#define K_MA   (MACRO_BASE + 13)
#define K_MB   (MACRO_BASE + 14)
#define K_MC   (MACRO_BASE + 15)
#define K_MR   (MACRO_BASE + 16)
#define K_ME   (MACRO_BASE + 17)
#define K_LOGO (MACRO_BASE + 18)
#define K_NEXT (MACRO_BASE + 19)
#define K_PLAY (MACRO_BASE + 20)
#define K_PREV (MACRO_BASE + 21)
#define K_FN   (MACRO_BASE + 22)

// 系统模式
// 设置类模式必须连续，moveSettingField / adjustSettingField / save / cancel
// 都靠 IS_SETTING_MODE 一次性接管。新增 SYS_MODE_SET_LIGHT 时记得把区间上界一起改。
#define SYS_MODE_NORMAL    0
#define SYS_MODE_MENU      1
#define SYS_MODE_SLEEP     2
#define SYS_MODE_SET_TIME  3
#define SYS_MODE_SET_ALARM 4
#define SYS_MODE_SET_TIMER 5
#define SYS_MODE_CAL_TEMP  6
#define SYS_MODE_SET_LIGHT 7
#define SYS_MODE_REC_SEQ   8
#define SYS_MODE_REC_CMB   9
// 错误日志查看页。不是"设置模式"：它没有可调字段、没有保存/取消语义，
// 所以**故意排在 IS_SETTING_MODE 区间（3~7）之外**，别被 moveSettingField /
// adjustSettingField 那套接管。它自己在按键分发里占一个分支。
#define SYS_MODE_ELOG      10
// 11 曾经是 SYS_MODE_PET（键盘宠物「小橘」），该功能已整体移除。
// 这个号**留空不复用**：万一有旧固件把 currentSysMode 落过盘，
// 复用了 11 就会让老状态直接变成新界面的入口。
// 按键速查对话框。追加在末尾，同样**不是**"设置模式"（理由同宠物页）：
// 它没有可调字段，全屏的按键只用来翻页和关闭。
// 这次它**要**进 scanKeyboardMatrix() 的 inUiMode —— 速查表是纯查阅界面，
// 在里面打字没有任何意义，矩阵键照常发主机只会把主机屏幕搞得莫名其妙。
#define SYS_MODE_HELP      12
#define IS_SETTING_MODE(m) ((m) >= SYS_MODE_SET_TIME && (m) <= SYS_MODE_SET_LIGHT)
static uint8_t currentSysMode = SYS_MODE_NORMAL;
static bool menuNeedsRebuild = false;  // 菜单重建标志（在 loop 中处理）

// 显示模式
#define DISP_MODE_GEEK            0
#define DISP_MODE_BIG_CLOCK       1
#define DISP_MODE_INFO_PANEL      2
#define DISP_MODE_KEY_MON         3
#define DISP_MODE_RHYTHM          4
#define DISP_MODE_WALLPAPER       5
#define DISP_MODE_HIGH_CONTRAST   6
#define DISP_MODE_PLAN_KEY        7     // 「方案+按键」：左列方案图标 + 右侧按键名
#define TOTAL_DISP_MODES          8

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸", "高对比度", "方案按键"
};

static uint8_t currentDispMode = DISP_MODE_GEEK;

// ===========================
// 控制模式（C3 灯光键 / 旋钮的作用目标）
// ===========================
// 旋钮本身只会发"转了 +1 / -1"，转到什么上由这个模式决定：
// 背光亮度是本机自己算的，屏幕亮度 / 音量 / 静音都是 Consumer 页丢给主机的。
// 这套是 s3.ino 原版就有的交互，LVGL 重写时整段漏掉了 ——
// 结果 C3 上的灯光键、旋钮、CPG 键全部无反应（只往 Serial1 发，没往回读）。
#define MODE_LIGHT               0   // 键盘背光亮度（本机）
#define MODE_SCREEN_BRIGHTNESS   1   // 主机屏幕亮度（Consumer）
#define MODE_MUTE                2   // 主机系统音量（Consumer）
#define MODE_CPG                 3   // 灯效切换（本机）
#define MODE_COUNT               4
static uint8_t currentMode = MODE_LIGHT;
static const char* modeNamesCN[MODE_COUNT] = { "键盘背光", "屏幕亮度", "系统音量", "灯效" };

// ===========================
// 通知系统
// ===========================
enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };

#define MAX_NOTIFS 8
static uint8_t notifCount = 0;
static AlertType notifQueue[MAX_NOTIFS];
static char notifTexts[MAX_NOTIFS][128];
static lv_obj_t* scr_notif = nullptr;

// ===========================
// 全局状态
// ===========================

// 键盘矩阵
#define NUM_ROWS 10
#define NUM_COLS 16
#define DEBOUNCE_DELAY 20
static const uint8_t rowPins[NUM_ROWS] = { 1, 2, 42, 41, 40, 39, 38, 47, 21, 12 };
// keebMatrix = **已确认**的稳定状态（只有消抖通过才写这里）。
static bool keebMatrix[NUM_ROWS][NUM_COLS] = {0};
// 消抖用的候选状态 + 它"第一次被读到"的时刻。
//
// ⚠ 之前这里是 lastDebounceTime[]，只有一个时间戳，写法是
//   `状态和稳定态不同 && 距上次认账 > 20ms → 直接认账`。
//   这**不是消抖，是给状态翻转限速**：它不要求新状态先稳定下来。
//   于是机械抖动会这样穿透（时间轴在下面 KEYS DEBOUNCE 注释里）：
//     t=0    按下        → 认账，keeb=true  → 发一次 press（正常）
//     t=25   抖动断开   → 距上次认账 >20ms → **认账**，keeb=false → 发一次 release
//     t=30   触点重新闭合，但距上次认账才 5ms → 不认账，keeb 停在 false
//     t=45   键**还按着**，raw 仍是 true 而 keeb 是 false → 又满足条件 → 再发一次 press
//   用户物理上只按了一次，主机却收到 press/release/press，表现为"有时打两下"。
//   空格尤其明显：键帽最大、行程最长、触点面积最大，抖动时间天然比别键久。
//
// 现在是标准三态消抖：raw → cand（候选，带起始时刻）→ 稳定态。
// 候选必须**连续保持满 DEBOUNCE_DELAY** 才提升为稳定态，中途抖回去就重新计时，
// 于是任何短于 DEBOUNCE_DELAY 的抖动都被整个吃掉，一次按下只可能认账一次。
static bool debounceCand[NUM_ROWS][NUM_COLS] = {0};
static unsigned long debounceCandMs[NUM_ROWS][NUM_COLS] = {0};
// 记录这颗键"按下时是否真的发给过主机"，松键时据此决定要不要补一个 release。
// 没有它的话，菜单里按 ESC 这类被界面吞掉的键会在松手时给主机发一个无头的 release。
static bool hostKeyHeld[NUM_ROWS][NUM_COLS] = {0};

// ⚠⚠ 记录"按下时**实际发出去的那个码点**"，松键时必须原样松掉它，不能重查映射表。
//
//   松键以前是 `getMappedKey(baseKey)` 重查一遍。那次查表用的是**松手那一刻**的
//   currentProfile —— 只要按住某键期间方案变了（按一下 MA/MB，或 M1 上配了
//   SW:NEXT 之后每按一次都翻），两次查表就会落到**不同的表**、给出不同的码点。
//
//   现场就是这样变成"方案不区分"的：macOS 方案里配了 ctrl->win、win->ctrl，
//   按住 Ctrl → 发出的是 Win(0xE3)；还没松手时按 M1 翻了方案；松手时新表是空的，
//   getMappedKey 原样返回 128 → 松掉的是 Ctrl(0xE0)。**Win 从此再也松不掉**，
//   主机那边从此所有组合键都像按着 Cmd —— 用户看到的就是"切到另一个方案后，
//   另一个方案的映射反而生效了、两个方案都生效"，而配置页读回来的两边数据
//   其实完全正确。
//
//   所以这里存"发出去什么就松什么"，让松键和按键严格成对。代价是每个矩阵位多
//   一个字节，换来的是任何方案切换时机下都不会漏松。
static uint8_t hostKeySent[NUM_ROWS][NUM_COLS] = {0};

// 列选通路上一次自检的结果（每 500ms 更新一次，见 scanKeyboardMatrix 里的检查）。
//
// 为什么扫描要受它管：总线半死不活的时候，列选信号是断断续续的 —— 有几次
// 写进去了、列脚确实拉低了，行脚就读到"按着"；后几次没写进去，列脚还是高，
// 行脚读回"松开"。每一次切换都稳定超过 DEBOUNCE_DELAY(20ms) 的话，三态消抖
// 就会认它一次账 —— 于是**按一个键连发六七次**，然后总线彻底死掉。
// 现场看到的就是这个顺序：先乱码式连发，再全哑。
//
// 所以自检一旦判死就一直跳过扫描，直到自检通过为止：不盲扫就不会产生
// 幽灵按键。反正这时候本来一个键也扫不出来。
static bool mcpHealthy = true;

// ---------------------------------------------------------------------------
// 死机探测（每拍都跑，8ms 级）
//
// 为什么需要这一层：原来只有 500ms 一次的全量自检（mcpColumnStrobeOk），
// 它抓得住"已经彻底死透"，抓不住"正在半死"。
//
// 现场症状是：按住空格（或 A、I）→ 主机收到 6~7 次 → 然后全哑。而"6~7 次"
// 本身需要 12~14 次电平翻转，每次都得稳定满 DEBOUNCE_DELAY(20ms) 才认一次
// 账，也就是 **240~280ms**。这个时长和 500ms 的自检周期是同一量级，能不能
// 赶在连发发出去之前检测到**全看自检 tick 落在哪里** —— 这正是它偶发的原因：
// 运气好一次连发都没有，运气差 6~7 次全发给主机，然后才判死。
//
// 换成每拍都跑之后，最坏 2 拍（16ms）就能判死，连发根本发不出去。
//
// 两路探测互相补盲：
//   ① 空闲态行脚检查 —— 抓"某一列被芯片死死按低"（连发的直接根源）
//   ② 同键连发计数   —— 抓故障时好时坏、撑不满一整轮的短命故障
// ---------------------------------------------------------------------------

// ① 空闲态自检：连续几拍读到"所有列都放开时仍有行脚为低"。
// 要连续 MCP_IDLE_STRIKE_N 拍才判死 —— 列脚刚从驱动切回高阻的那一瞬间
// 可能还在建立，单拍读数不作数。
#define MCP_IDLE_STRIKE_N 2
static uint8_t g_mcpIdleStrike = 0;

// 最近一次判死时"空闲态仍被拉低"的行脚位图（bit0=行0 … bit9=行9）。
// 留进日志是为了能直接对上是哪几行 → 排查时缩小到具体某一列。
static uint16_t g_mcpIdleLowRows = 0;

// ② 同键连发计数：同一颗键在 GHOST_WINDOW_MS 内被认下 GHOST_BURST_N 次
// 按下 —— 人手做不到（最快也就 ~10 键/秒 = 同键 ~100ms 一次），
// 而半死的总线正好会造出这个形状。
//
// 只存计数不存时间戳：整个窗口共用一个起点 ghostWinMs，过期就整块清零。
// 省掉 160×4 字节的时间戳数组，代价只是每 400ms 一次 160 字节 memset。
#define GHOST_WINDOW_MS 400
#define GHOST_BURST_N    4
static uint8_t           ghostPresses[NUM_ROWS][NUM_COLS];
static unsigned long     ghostWinMs = 0;

// 自动复位成功次数。**跨重启保存**（NVS）：用户遇到"键盘失灵"多半伴随一次
// 重启，不落盘的话开机就归零，看到的永远是 0，等于没记。
static uint32_t g_mcpRecoverN = 0;

// 判死之后的重试节奏。
static unsigned long mcpRetryMs = 0;
// 恢复间隔：正常 300ms；一旦出现"刚恢复就又坏"的连击，退到 2s。
// 不退的话最坏情况就是每 300ms 敲一次 I2C，而总线真废掉时每次
// begin_I2C 都要吃满 25ms 超时 —— 主循环被反复打断，界面跟着一起卡。
#define MCP_RETRY_MS        300
#define MCP_RETRY_MS_SLOW  2000
#define MCP_STRIKE_SLOW      3
static uint8_t  g_mcpRefault = 0;    // 连续"恢复后立刻又判死"的次数
static unsigned long mcpStableMs = 0;

static uint32_t totalKeyCount = 0;
static uint16_t baseMatrix[NUM_ROWS][NUM_COLS] = { 0 };
static bool fnPressed = false;

// LOGO 键的按下状态。矩阵键本来没有长按检测（_HOLD 全来自 C3 小 MCU），
// 这两个标志位用来分辨"松手这一次到底算不算一次短按"。
static bool logoHoldFired = false;
static bool logoIsDown     = false;

// LED
// 0~15  是主背光（16 颗），按 brightness 缩放
// 16/17/18 是 C3 上的 NUM / CAPS / SCR 三颗状态指示灯，按 indBrightness 缩放，
//         不受背光总开关和主背光亮度影响 —— 这是原版 s3.ino 的行为，
//         否则把背光拧到最暗时锁状态就彻底看不见了。
#define NUM_MAIN_LEDS 16
#define TOTAL_LEDS    19
static uint8_t ledRgb[TOTAL_LEDS][3];
static uint8_t brightness = 140;      // 主背光总亮度 0~255
static uint8_t currentEffect = 1;     // 灯效编号
static bool lightOn = true;           // 主背光总开关
static unsigned long lastLedFrameTime = 0;

// ---- 按键特效（按键触发的叠加层，从原版 s3.ino 的 triggerKeyReaction 搬回来）----
// 原版按下任意键：0~15 主背光先整体压暗到 ~40%，再在上面跑一段动画。
// LVGL 移植时整块丢了，所以只剩下"静态灯效"，按键毫无反馈。
// 三种形式就是原版那三种（原版用 keypressStyle/8 选，这里拆成独立设置项）：
//   涟漪 从灯条正中向两侧扩散 / 发射 从一端扫一排"子弹"过去 / 堆叠 从一端一格格填满
// 颜色每按一次自动换一种 —— 原版 colorIdx==0 的"自动"档就是这个配色。
#define KEYFX_OFF    0
#define KEYFX_RIPPLE 1
#define KEYFX_SHOOT  2
#define KEYFX_STACK  3
#define KEYFX_COUNT  4
static const char* keyFxNames[KEYFX_COUNT] = { "关闭", "涟漪", "发射", "堆叠" };
static uint8_t keyFxStyle = KEYFX_RIPPLE;   // 默认开涟漪（灯光设置里可关）
#define KEYFX_MAX_SHOTS 8
static bool     keyFxActive = false;
static uint8_t  keyFxStep = 0;                                   // 涟漪/堆叠的推进步
static uint8_t  keyFxStack = 0;                                  // 堆叠已经填了几格
static int8_t   keyFxShotStep[KEYFX_MAX_SHOTS] =                  // 发射的每发子弹走到哪了
    { -1, -1, -1, -1, -1, -1, -1, -1 };
static uint32_t keyFxColor = 0xFF0000;
static uint8_t  keyFxColorIdx = 0;

// 锁状态
static bool numLock = false;
static bool capsLock = false;
static bool scrollLock = false;
// USB HID LED 事件回调里只置位，loop() 里看到就立即推一帧 LED + 跑一次屏幕刷新。
// 之所以不能直接在回调里调 renderIndicators()/sendLedFrameToC3()：
//   1. ARDUINO_USB_HID_KEYBOARD_LED_EVENT 走的是 USB 任务/中断上下文；
//   2. Serial1.write 是阻塞的，硬塞进去会和 USB 任务抢时间；
//   3. LVGL 这边更不能在中断里碰（screen 对象指针 + 互斥问题）。
// 而且原来的 20ms 灯效节拍 + 100ms 屏幕节拍，最坏要 100ms 才看到屏幕变，
// 对"刚按了一下 Caps Lock"的反馈来说太慢 —— 这个脏位把延迟压到下一轮 loop()。
static volatile bool lockStateDirty = false;
// 播放/暂停交替显示（按 PLAY 时翻转）
static bool mediaPlaying = false;

// 温湿度
//
// 温度偏移的单位就是摄氏度，直接加在传感器读数上（shtTemp = t + shtTempOffset）。
//
// 之前这里是个 62.0f 的"内部基准"，而 sht31_update() 里写的是
//     shtTemp = t + shtTempOffset - 62.0f;
// 两处 62 正好抵消 —— 偏移从头到尾都是 0：
//   · 屏上显示的 "偏移 +62.0" 跟实际含义对不上（那是基准值，不是偏移量）
//   · 在校准页按 ↑↓ 改了值也看不出一丝变化（64.0 和 62.0 减完都是 0）
//   · 传感器受主板发热影响偏高时，用户没有任何手段把读数调回来
// 现在统一成"偏移就是偏移"：默认给实测补偿值，范围 ±20°C，屏上显示多少就真的加多少。
//
// 默认值 -5.3 是实测标定出来的：屏显 29.3°C 时参考温度计是 24.0°C，
// 即本机 SHT31 受主板热源影响读数偏高约 5.3°C。每台机器发热情况不同，
// 用菜单里的"温度校准"可以在 ±20°C 内任意微调。
#define SHT_TEMP_OFFSET_DEFAULT  (-5.3f)
#define SHT_TEMP_OFFSET_MIN      (-20.0f)
#define SHT_TEMP_OFFSET_MAX      ( 20.0f)

static bool shtAvailable = false;
static float shtTemp = 0.0f;
static float shtHumidity = 0.0f;
static float shtTempOffset = SHT_TEMP_OFFSET_DEFAULT;
static unsigned long lastSHTRead = 0;
#define SHT_READ_INTERVAL_MS 900000UL

// 时间
static time_t alarmLastFiredYday = -1;
static unsigned long lastActivityTime = 0;
static unsigned long pendingRestartMs = 0;
#define SLEEP_TIMEOUT_MS 60000UL

// ---- 新仪表盘数据：B-1 高对比度风格中部数据带 ----
// KPM：环形缓冲区，记录最近 60 秒每秒的落键数。当前 KPM = sum(buf)。
#define KPM_WINDOW_SEC 60
static uint8_t kpmRing[KPM_WINDOW_SEC] = {0};  // 每秒 0..255 落键
static uint8_t kpmRingHead = 0;                 // 当前秒落点
static unsigned long kpmRingLastSec = 0;        // 上次累加定时器定位
// TODAY：今日累计落键。lastTodayDate 用于判定跨日清零。
static uint32_t todayKeyCount = 0;
static int      todayDateYmd = -1;              // YYYYMMDD
// ACTIVE：分钟级空闲时长，idleMinutes = (millis()-lastActivityTime)/60000。
// 推算，运行时不必持久化。

// 菜单
//
// 两级：menuInSub = false 时选的是 menuGroups[] 里的大类，
// = true 时选的是那一组下面的具体项（menuItemSel）。
// 一级项数 = MENU_GROUP_COUNT，二级项数 = 那一组的 count，两者都取最大
// 值 MENU_ROW_MAX 建一行行对象，切换层级时只改可见性和文字、不重建，
// 免得进二级时整屏删建（LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL）。
static uint8_t menuSel = 0;          // 兼容旧名字：现在指一级大类下标
static uint8_t menuItemSel = 0;      // 二级：组内项下标
static bool   menuInSub = false;     // 当前在二级

// 一行菜单对应的对象数。
//
// ⚠️ **必须 >= 一级的大类个数（MENU_GROUP_COUNT）**。这个数组只建这么多行对象，
// 渲染循环也是 `for (i = 0; i < MENU_ROW_MAX; i++)`，所以超出这个数的项
// **既不会被画出来也不会报错** —— 表现是"菜单项存在、能选中、能进去，
// 但屏幕上就是看不见那一行"，非常像被什么盖住了。
// 加第 7 个大类「宠物」时这里只加到 6，症状就是这个。改大它。
// 第 8 个大类「帮助」加进来之后是 8，这里留到 10 留点余量。
#define MENU_ROW_MAX 10

// 列表最多能同时显示几行（一级 8 个大类，二级最多 5 项，6 够用 —— 第 7/8 个
// 大类靠 menuScrollOffset 滚进窗口）
#define MENU_VISIBLE_ITEMS 6

// 宏录制
#define MAX_REC_KEYS 64
static uint16_t recKeyBuffer[MAX_REC_KEYS];
static int recKeyCount = 0;
// 录制三段式由一个状态机驱动：
//   0 = 连续输入(SEQ) → 1 = 组合键(CMB) → 2 = 结束并取消回主屏
#define REC_STAGE_SEQ  0
#define REC_STAGE_CMB  1
#define REC_STAGE_EXIT 2
static uint8_t recStage = REC_STAGE_EXIT;
static lv_obj_t* rec_lbl_count = nullptr;
static lv_obj_t* rec_lbl_keys = nullptr;
static lv_obj_t* rec_lbl_mode = nullptr;

// 菜单项要执行什么。用枚举而不是"下标 → switch"，是因为菜单已经改成两级：
// 下标会随分组重排变，而"这一项做什么"必须稳定。
enum MenuAction {
    MA_CYCLE_DISP,      // 切换主屏风格
    MA_CYCLE_SAVER,     // 切换屏保风格
    MA_OPEN_LIGHT,      // 灯光设置
    MA_OPEN_TIME,       // 设置时间
    MA_OPEN_ALARM,      // 闹钟设置
    MA_OPEN_TIMER,      // 倒计时
    MA_CYCLE_PROFILE,   // 切换配置方案
    MA_REFRESH_SHT,     // 刷新温湿度
    MA_OPEN_CALTEMP,    // 温度校准
    MA_CLEAR_COUNTERS,  // 计数清零
    MA_OPEN_ELOG,       // 错误日志
    MA_OPEN_HELP        // 按键速查（这一块板子上的功能速查表）
};

struct MenuEntry {
    const char* cn;
    MenuAction   act;
};

// 菜单分组。一级只列这 6 个大类，回车进去才看到具体项。
// 之前是 13 项一长条平铺，动作项（刷新温湿度 / 计数清零）和子页面
// （灯光 / 时间 / 闹钟 / 倒计时 / 校准 / 日志）混排在一张列表里，
// 既看不出哪几项是一类的，找一个设置还得上下翻大半屏。
// 分组顺序按"多久会用到一次"排：显示 → 灯光 → 时间 → 键盘 → 温湿度 → 系统。
static const MenuEntry menuEntriesDisplay[] = {
    { "切换主屏风格", MA_CYCLE_DISP },
    { "切换屏保风格", MA_CYCLE_SAVER }
};
static const MenuEntry menuEntriesLight[] = {
    { "灯光设置",     MA_OPEN_LIGHT }
};
static const MenuEntry menuEntriesTime[] = {
    { "设置时间",     MA_OPEN_TIME },
    { "闹钟设置",     MA_OPEN_ALARM },
    { "倒计时",       MA_OPEN_TIMER }
};
static const MenuEntry menuEntriesKeyboard[] = {
    { "切换配置方案", MA_CYCLE_PROFILE }
};
static const MenuEntry menuEntriesSensor[] = {
    { "刷新温湿度",   MA_REFRESH_SHT },
    { "温度校准",     MA_OPEN_CALTEMP }
};
static const MenuEntry menuEntriesSystem[] = {
    { "计数清零",     MA_CLEAR_COUNTERS },
    { "错误日志",     MA_OPEN_ELOG }
};
// 帮助组**追加在末尾**（第 8 个）。理由和宠物组一样：分组编号是用户的肌肉记忆，
// 插在中间会让原来"第 N 组"的位置全变。
// 里面只放一项：一张按键速查表。这块板子上的功能已经多到记不住了
// （灯光键短按切控制目标、长按切背光、静音键双击锁键盘、CPG 键双击重启扩展芯片……），
// 全靠记忆迟早要出事，所以直接列出来。
static const MenuEntry menuEntriesHelp[] = {
    { "按键速查",     MA_OPEN_HELP }
};

struct MenuGroup {
    const char*     cn;
    const char*     en;
    const MenuEntry* items;
    uint8_t          count;
};

#define MENU_GROUP_COUNT 7
static const MenuGroup menuGroups[MENU_GROUP_COUNT] = {
    { "显示",   "DISPLAY",  menuEntriesDisplay,  sizeof(menuEntriesDisplay)  / sizeof(MenuEntry) },
    { "灯光",   "LIGHTING", menuEntriesLight,    sizeof(menuEntriesLight)    / sizeof(MenuEntry) },
    { "时间",   "TIME",     menuEntriesTime,     sizeof(menuEntriesTime)     / sizeof(MenuEntry) },
    { "键盘",   "KEYBOARD", menuEntriesKeyboard, sizeof(menuEntriesKeyboard) / sizeof(MenuEntry) },
    { "温湿度", "SENSOR",   menuEntriesSensor,   sizeof(menuEntriesSensor)   / sizeof(MenuEntry) },
    { "系统",   "SYSTEM",   menuEntriesSystem,   sizeof(menuEntriesSystem)   / sizeof(MenuEntry) },
    { "帮助",   "HELP",     menuEntriesHelp,     sizeof(menuEntriesHelp)     / sizeof(MenuEntry) }
};

// 辅助变量
//
// 原来这里还有一个 showKeystrokes（菜单里的"按键回显开关"），已经删掉：
// 它是纯内存变量、**不落 NVS**，所以每次开机都被初始化回 true —— 用户白天关掉、
// 拔电或重启一次就自己开回来了，等于一个骗人的开关。而它唯一的用途就是让
// 四块主屏风格里那颗"最后按下的键"显示 "--"；对一块键盘来说，把刚才按了什么都
// 盖住没有任何好处。真要隐藏按键名，直接把 lastKeyPressed 喂 "--" 就行。
static char lastKeyPressed[8] = "-";

// ===========================
// 息屏 / 屏保风格
// ===========================
// 原来 SLEEP_TIMEOUT_MS 到了就 destroyMainScreen() 把主屏整个拆掉，屏幕全黑。
// 现在给三种屏保 + 一个"完全不进屏保"：
//   SAVER_WALL 壁纸铺满 + 底部通栏信息条 —— 一直是图片，同时一直看得到时间
//   SAVER_INFO 只显示信息面板（居中一张卡片），不铺壁纸
//   SAVER_OFF  整个屏保功能不启用：空闲到点什么都不做，主屏原样留着
// 三种屏保共用同一块 `sv_panel`（时间 / 日期 / 温湿度都收在这一块里，
// 不再是四个散落在屏幕四角的 label）。
// 屏保是一块独立屏幕（scr_saver），和主屏并存，唤醒时直接切回主屏即可。
//
// ⚠ 档位编号是 NVS 持久化的，**只能往后追加、不能往前插**。
// 原来 SAVER_OFF=0 的含义是"黑屏"（拆主屏全黑），那个语义现在归 SAVER_BLACK。
// 如果把"关闭"插成 0，老用户开机就变成"屏保关着"，属于静默改设置。
// 所以 SAVER_OFF=3 追加在末尾，0/1/2 三个旧值的含义一字不变。
#define SAVER_BLACK 0   // 黑屏（拆主屏，屏幕全黑）—— 老 SAVER_OFF 的语义
#define SAVER_WALL  1   // 壁纸 + 底部信息条
#define SAVER_INFO  2   // 信息面板
#define SAVER_OFF   3   // 关闭：空闲也不息屏
// 4 曾经是 SAVER_PET（屏保里的小橘），该功能已整体移除。编号**留空不复用**：
// 屏保档位是 NVS 持久化的，插在中间会让老用户开机就变成另一个屏保。
#define TOTAL_SAVER_MODES 4
static const char* saverModeNames[] = { "黑屏", "壁纸", "信息面板", "关闭" };
static uint8_t saverMode = SAVER_OFF;   // 默认不自动息屏
static lv_obj_t* scr_saver = nullptr;
static lv_obj_t* sv_img = nullptr;
static lv_obj_t* sv_panel = nullptr;      // 信息面板容器（时间/日期/温湿度都在里面）
static lv_obj_t* sv_lbl_time = nullptr;
static lv_obj_t* sv_lbl_date = nullptr;
static lv_obj_t* sv_lbl_temp = nullptr;
static lv_obj_t* sv_lbl_hum = nullptr;

// 灯效相关
static const char* effectNames[] = {
    "关闭", "纯红", "纯绿", "纯蓝", "冰蓝", "纯白",
    "红呼吸", "绿呼吸", "蓝呼吸", "冰蓝呼吸", "流光", "彗星", "幻彩"
};
#define MAX_EFFECTS 13

// 状态灯亮度
#define IND_LEVEL_COUNT 4
static const char* indLevelNames[] = { "关", "低", "中", "高" };
static const uint8_t indLevelValues[] = { 0, 64, 140, 255 };
static uint8_t indLevel = 3;
static uint8_t indBrightness = 255;

// 菜单滚动相关
// 纵向预算（屏 240 高）：
//   12~31    标题「系统菜单」+ 右上角页码胶囊
//   42~196   列表：6 行 × 24 高、步进 26（行间 2px）
//   199~237  底部两行按键提示
// 行高/步进是从原来的 26/28 收下来过的：底部提示改成两行后要占 38px
// （原来一行只占 19px），原来的 50+6×28=218 直接压到提示上面。
// 一级现在是 7 个大类，一屏放得下 6 行，第 7 行靠 menuScrollOffset 滚进来。
// （定义在前面 MENU_ROW_MAX 那段）
#define MENU_ITEM_H        24
#define MENU_PITCH         26
#define MENU_LIST_TOP      42
static int menuScrollOffset = 0;

// ===========================
// 设置界面全局变量
// ===========================
// 时间设置
static int timeEditY = 2026, timeEditMo = 1, timeEditD = 1, timeEditH = 0, timeEditMi = 0;
static int timeFieldIdx = 0;

// 闹钟设置
static bool alarmEditOn = false;
static int alarmEditH = 7, alarmEditM = 0;
static int alarmFieldIdx = 0;
static bool alarmEnabled = false;
static uint8_t alarmHour = 7, alarmMinute = 0;

// 倒计时设置
static int timerEditH = 0, timerEditM = 5, timerEditS = 0;
static int timerFieldIdx = 0;
static bool timerRunning = false;
static unsigned long timerStartMs = 0;
static uint32_t timerTotalSec = 0;
static uint32_t timerRemainSec = 0;

// ===========================
// 响铃引擎（闹钟 / 倒计时到点共用）
// ===========================
// 为什么要有这一层：闹钟和倒计时到点之后要做的事**完全一样** ——
// 主背光整排黄灯快闪 + 屏幕上一张常驻卡片，等用户按灯光键/静音键停。
// 只有文案和大字不一样（闹钟报"本该几点响"，倒计时报"刚才设了多久"），
// 所以走同一套，不再各写一份。
//
// 这一整块以前是**没有**的：alarmHour/alarmMinute/alarmEnabled 三个变量
// 存得进 NVS、网页上"设置成功"也照样弹绿条，但全工程没有任何一处去比对
// 当前时间（alarmLastFiredYday 在 604 行声明了却一次都没被读过），
// timerRemainSec 也只被赋值、从没被减过 —— 所以"设了闹钟不响"、
// "设了倒计时不走"都是同一个病因：只有状态，没有驱动它们的状态机。
enum RingKind { RING_NONE = 0, RING_ALARM, RING_TIMER };
static RingKind ringingKind = RING_NONE;   // 当前响的是哪种铃
static unsigned long ringStartMs = 0;      // 本次起铃时刻，用于兜底超时
static bool ringBlinkOn = true;            // 卡片描边的亮/暗，用于重画节流

// 响铃卡片。挂在 lv_layer_top() 上而不是做成一块独立屏幕：这样菜单、设置页、
// 息屏中的任何一种底下都能直接盖住，不用先想办法退回主屏。
static lv_obj_t* ring_win = nullptr;
static lv_obj_t* ring_lbl_title = nullptr;
static lv_obj_t* ring_lbl_big = nullptr;

// ===========================
// 倒计时全屏
// ===========================
// 用户要的是"整屏只显示这一个倒计时，文字很大"（见 README「倒计时全屏」）。
// 所以它是**一块独立屏幕**（lv_obj_create(NULL)）而不是浮层：底层什么都不画，
// 240x240 全部让给那一个数字。
//
// countdownShown = 用户是否允许这块屏接管显示。刚设上倒计时时置 true；
// 按住灯光键退出接管后置 false，但**倒计时本身继续在后台跑**（用户原话：
// 「按住灯光键可以退出全屏。然后后台继续跑」）。false 期间随时可以再设一次
// 把它叫回来，或者干脆等它跑完响铃。
static lv_obj_t* scr_countdown = nullptr;
static lv_obj_t* cd_lbl_main = nullptr;
static bool countdownShown = false;

// 温度校准
static float calTempOriginal = SHT_TEMP_OFFSET_DEFAULT;
static int calTempField = 0;

// 灯光设置
// 背光开关 / 背光亮度 / 灯效 / 状态灯亮度 / 按键灯效 全部收进这一个页面，
// 菜单里不再有"循环一下就走的"灯效项 —— 那玩意儿按错了根本不知道按到哪一档。
#define LIGHT_FIELD_COUNT 5
static uint8_t lightFieldIdx = 0;
static bool    lightOnOriginal = true;
static uint8_t lightBrightOriginal = 140;
static uint8_t lightEffectOriginal = 1;
static uint8_t lightIndLevelOriginal = 3;
static uint8_t lightKeyFxOriginal = KEYFX_RIPPLE;
static const char* lightFieldCN[LIGHT_FIELD_COUNT]  = { "背光开关", "背光亮度", "灯效", "状态灯亮度", "按键灯效" };
static const char* lightFieldEN[LIGHT_FIELD_COUNT]  = { "BACKLIGHT", "BRIGHTNESS", "EFFECT", "INDICATOR", "KEY FX" };
static const char* lightSwitchCN[2] = { "开", "关" };

// 设置界面对象
static lv_obj_t* set_time_lbl_date = nullptr;
static lv_obj_t* set_time_lbl_time = nullptr;
static lv_obj_t* set_time_field_labels[5] = { nullptr };

static lv_obj_t* set_alarm_lbl_time = nullptr;
static lv_obj_t* set_alarm_sw = nullptr;
static lv_obj_t* set_alarm_state_lbl = nullptr;
// 三个字段：0=时 1=分 2=开关。原来只有 2 个，那个开关是**只能看不能碰**的
// 摆设 —— 见 build_settings_alarm 里的注释。
static lv_obj_t* set_alarm_field_labels[3] = { nullptr };

static lv_obj_t* set_timer_lbl_time = nullptr;
static lv_obj_t* set_timer_btn = nullptr;
static lv_obj_t* set_timer_field_labels[3] = { nullptr };

static lv_obj_t* set_cal_lbl_temp = nullptr;
static lv_obj_t* set_cal_lbl_offset = nullptr;
static lv_obj_t* set_cal_field_label = nullptr;

static lv_obj_t* set_light_lbl_cap = nullptr;
static lv_obj_t* set_light_lbl_en = nullptr;
static lv_obj_t* set_light_lbl_value = nullptr;
static lv_obj_t* set_light_field_labels[LIGHT_FIELD_COUNT] = { nullptr };

// HUD
typedef struct {
    bool active;
    bool big;         // true = 大提示（必须看见的），false = 小提示（例行确认）
    char title[32];   // 中文 3 字节/字，32 字节够放 10 个字
    char value[48];
    // ⚠ 这里原来存的是 uint32_t color = lv_color_to32(color)，绘制时又套了一层
    // lv_color_hex()。lv_color_to32() 返回的是 0xFFRRGGBB（带 alpha 的 32 位），
    // 再喂给 lv_color_hex() 会被当成 0xRRGGBB 解读 —— 高 8 位的 0xFF 直接串到
    // 红色通道上，青色 0x00E5FF 变成 0xFF00E5 的玫红，很多颜色还会被压暗到看不清。
    // 正确做法是原样存 lv_color_t，别来回转。
    lv_color_t color;
    unsigned long showMs;
    unsigned long startMs;
} HudEntry;
static HudEntry hud = {0};

// Profile
//
// ⚠ 4 → 2：方案这一维度从"四个玩法档"改成"按操作系统分"。
//   留下的两个是 Windows 和 macOS —— 这也正好对上 profile_icons.h 里
//   prof_icon_win / prof_icon_mac 那两个（那两个图标本来就是 win 和 mac，
//   game/work 是另外两档，现在用不上了；profile_icon_get 自带越界保护，
//   currentProfile 只会在 0~1 之间，所以不用动那个头文件）。
//
//   **NVS 键名一个都没改**：方案 0 还是 p0 / rmp_0_*，方案 1 还是 p1 / rmp_1_*。
//   也就是说"方案 0 = windows、方案 1 = mac"只是给已有的 0/1 重新起了名字，
//   里面已经配好的 remap 和宏原样接着用。p2/p3 的数据留在 NVS 里不再读取
//   （用户明确要求"彻底废掉"），键还占着分区，但 NVS 本来就只增不减、
//   新键写不进去才叫问题，留着旧键不占新键的位置。
#define TOTAL_PROFILES 2
#define MAX_REMAP_RULES 32
struct RemapRule { uint16_t fromKey; uint16_t toKey; };
static RemapRule profileRemaps[TOTAL_PROFILES][MAX_REMAP_RULES];
static int remapCounts[TOTAL_PROFILES] = { 0 };
static uint8_t currentProfile = 0;
static const char* profileNamesCN[TOTAL_PROFILES] = {
    "Windows", "macOS"
};
// 方案在配置 JSON 里的对象名。**JSON 的键名以这里为准**，改这里就等于改线上格式。
static const char* CFG_PROF_NAMES[TOTAL_PROFILES] = { "windows", "mac" };

// BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool oldDeviceConnected = false;
// 断链后"下一次尝试重启广播"的时间点等广播状态机的变量，
// 统一放在 myGapHandler 那一段（见那里关于广播状态机的说明）。
static unsigned long lastPingTime = 0;

// 这里**不再**有任何"怎么发"的代码。
//
// 原来这一段里有 bleNotifyCritical() 和 bleNotifyRemapDump() 两个手写的发送口，
// 外加 LOG 流自己那一份 notify，5 个动词 5 套分片规则，抄错一处就丢一次包
// （丢包的表现是网页只能等超时，然后误以为键盘里是 0 条配置）。现在统一走
// bt_link，全文只有这四种用法：
//
//   btLinkReplyC("REMAPDUMP", body)        处理请求的分支里回话（key = 请求号）
//   btLinkReplyf("GKEYPHASE", "%u", ph)    printf 风格
//   btLinkStream("LOG", line)              日志流：内部 10Hz 节流 + 给响应让路
//   btLinkStreamBegin/Write/End            8KB 日志回拉这种大块（正文不进队列）
//
// 业务代码只管"我要说什么"，不再关心一片能带几个字节、片间要不要留缝、
// 要不要补收尾标记、发包会不会把主循环按住。三件事的完整理由见 src/bt_link.h。

// 节流：100ms / 10Hz。BLE 写一次 attribute + notify 约 7~15ms,
// 50Hz 节流在 NimBLE 那边的发送队列里还是会堆几十包,流转发 5 秒就把
// 队列填满,后面 REMAPDUMP / LOGOSTATUS 等关键响应发不出。
// 10Hz 留出充裕的"空档",丢一些日志无所谓,关键时刻(按键路径)能挤进来。
static void pushLogLine(const char* line) {
    if (line == nullptr || line[0] == '\0') return;
    if (logRingBuf == nullptr) return;     // setup() 还没分配好缓冲
    size_t n = strlen(line);
    if (n == 0) return;

    // ★ 加锁：主循环和蓝牙协议栈任务可能同时进来（见 s_logMutex 的说明）。
    //   拿不到锁就丢这一行 —— 绝不阻塞蓝牙任务。
    bool locked = false;
    if (s_logMutex != nullptr) {
        locked = (xSemaphoreTake(s_logMutex, pdMS_TO_TICKS(5)) == pdTRUE);
        if (!locked) return;
    }

    // 满时丢最老的整行，循环直到能塞下
    while (logRingLen + n + 1 > LOG_BUF_CAP) {
        size_t drop = 0;
        while (drop < logRingLen && logRingBuf[drop] != '\n') drop++;
        if (drop < logRingLen) drop++;          // 把那个 \n 也吞掉
        if (drop == 0 || drop > logRingLen) {   // 极端：单行比缓冲还大，强制清零
            logRingLen = 0;
            break;
        }
        memmove(logRingBuf, logRingBuf + drop, logRingLen - drop);
        logRingLen -= drop;
    }

    memcpy(logRingBuf + logRingLen, line, n);
    logRingBuf[logRingLen + n] = '\n';
    logRingLen += n + 1;

    // 流转发。节流 10Hz（100ms/条）、给关键响应让路、分片收尾，全在 bt_link 里。
    // 这里只管"要不要把这一行推上通道"，一个字节的协议细节都不碰。
    if (logStreamOn) btLinkStream("LOG", line);

    if (locked) xSemaphoreGive(s_logMutex);
}

static void handleCommand(const String& cmd);   // 真正定义在文件后段的命令解析入口

// 指令队列、按行攒包、空闲兜底提交，全都在 bt_link 里，这里一份副本都不留。
// 原因（踩过的坑）都搬进了 src/bt_link.h：
//   · "一个包 = 一条命令"会把长指令（整张映射表、SET:… 宏）切成两半，
//     handleCommand 拿到的都是残缺指令，什么也不匹配 —— 而短的（ALERT:RED）
//     反而好使，症状是"M1 相关的下发一律失败"。
//   · 空闲兜底从 15ms 放宽到 80ms：Web Bluetooth 的 writeValue 会按协商出来的
//     ATT MTU 拆包（协商失败时 20~185 字节），15ms 会在**同一个 writeValue 的
//     两个包之间**误判成"这条发完了"，于是 29 字节的
//     `REMAP:0:clear:224,227;227,224` 被当成两条命令收下，"下发 2 条只存进 1 条"。


// ===========================
// C3 串口通道（灯光键 / 静音键 / CPG 键 / 旋钮）
// ===========================
// C3 是同一块板上管灯和那几个侧键的小 MCU，它和本机的对话全走 Serial1：
// 本机 -> C3 是 0xAA 0x55 包（灯帧 + 2 秒一次的 ping），
// C3 -> 本机 是以 \n 结尾的文本行（PONG / ENC:+ / ENC:- / BTN:LIGHT / BTN:MUTE ...）。
//
// LVGL 重写时只保留了"发"的那一半，**从来没读过 Serial1**，
// 于是 C3 上的灯光键、静音键、CPG 键、旋钮全部无反应 ——
// 表现就是"告警面板关不掉"和"亮度/音量/灯效都调不动"。
//
// 解析放在 loop() 里逐行做，和 BLE / USB HID 通道同一个纪律：
// 串口缓冲、LVGL 调用都在主任务，函数本身不做任何阻塞读。
static bool c3Connected = false;
// 开机至今从 C3 收到的字节数。
//
// 为什么单独计数而不是只看 c3Connected：PONG 要等 C3 先被问才会来，而"灯效和
// 灯光键全无反应"这条症状既可能是 C3 没接（一个字节都没有），也可能是接了但
// 不回话（有杂字节、没 PONG）。这两个在界面上看起来一模一样，只能靠字节数区分。
// 同样地，这也**不是**匹配日志措辞，是硬件侧可观测的事实。
static uint32_t c3RxBytes = 0;
static void handleC3Command(const String& cmd);   // 定义在文件后段
static void knobAdjust(int dir);                  // 定义在文件后段
static void handleC3Events(void) {
    static String serialBuffer = "";
    while (Serial1.available() > 0) {
        char c = (char)Serial1.read();
        c3RxBytes++;
        if (c == '\n') {
            serialBuffer.trim();
            if (serialBuffer.length() > 0) {
                handleC3Command(serialBuffer);
                serialBuffer = "";
            }
        } else if (c != '\r') {
            if (serialBuffer.length() < 48) serialBuffer += c;   // 掐断超长行，别被灌爆
        }
    }
}

// ===========================
// USB HID 厂商通道：电脑 -> 键盘 的下行指令
// ===========================
// 主机往 Report ID 6 的 Output / Feature 报告里写一段以 \n 结尾的文本，
// 语法和 BLE 那条通道完全一样：ALERT:RED、ALERT:GREEN:磁盘空间不足、
// NOTIFY:开会了、ALERT:OFF、DISP_MODE:n …，PC 端脚本是同目录的 kbctl_hid.py。
//
// 这条路在 s3.ino 里是通的，搬进 LVGL 版时漏了：setup() 只写了 VendorHID.begin()，
// 没注册 onHidVendorEvent，主机写过来的报告根本没人收 —— 表现就是"HID 发红绿灯没反应"。
// 蓝牙能弹通知、HID 不能，两条通道的指令文本却一模一样，差的就是这个回调。
//
// 纪律和 BLE 的 onWrite 完全一样：USB 中断回调只把字节丢进环形缓冲，
// 解析放在 loop()（handleHidVendorCommands），绝不在回调里碰 LVGL。
#define HID_RX_BUF_SIZE 512
static volatile uint8_t  hidRxBuf[HID_RX_BUF_SIZE];
static volatile uint16_t hidRxHead = 0;
static volatile uint16_t hidRxTail = 0;

static inline void hidRxPush(char c) {
    uint16_t next = (uint16_t)((hidRxHead + 1) % HID_RX_BUF_SIZE);
    if (next != hidRxTail) {   // 满了就丢最新的，绝不阻塞 USB 事件任务
        hidRxBuf[hidRxHead] = (uint8_t)c;
        hidRxHead = next;
    }
}

static void onHidVendorEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id != ARDUINO_USB_HID_VENDOR_OUTPUT_EVENT && id != ARDUINO_USB_HID_VENDOR_SET_FEATURE_EVENT) return;
    const arduino_usb_hid_vendor_event_data_t* p =
        (const arduino_usb_hid_vendor_event_data_t*)data;
    if (p == NULL || p->buffer == NULL) return;
    for (uint16_t i = 0; i < p->len; i++) {
        char c = (char)p->buffer[i];
        if (c != '\0') hidRxPush(c);   // 报告尾部补的 0 是凑长度的，不是结束符
    }
}

static void handleHidVendorCommands(void) {
    static String buf = "";
    while (hidRxTail != hidRxHead) {
        char c = (char)hidRxBuf[hidRxTail];
        hidRxTail = (uint16_t)((hidRxTail + 1) % HID_RX_BUF_SIZE);
        if (c == '\n' || c == '\r') {
            buf.trim();
            if (buf.length() > 0) handleCommand(buf);
            buf = "";
        } else if (buf.length() < 200) {
            buf += c;
        } else {
            buf = "";   // 超长直接丢弃，防止野数据把内存撑爆
        }
    }
}

// ===========================
// LVGL Screen 管理
// ===========================
static lv_obj_t* scr_main = nullptr;
static lv_obj_t* scr_menu = nullptr;
static lv_obj_t* scr_settings_time = nullptr;
static lv_obj_t* scr_settings_alarm = nullptr;
static lv_obj_t* scr_settings_timer = nullptr;
static lv_obj_t* scr_settings_caltemp = nullptr;
static lv_obj_t* scr_settings_light = nullptr;
static lv_obj_t* scr_recording = nullptr;
static lv_obj_t* currentScreen = nullptr;

// ===========================
// 键盘锁定
// ===========================
// 详细行为见文件后段"键盘锁定（双击静音键 = 锁定）"那一节。这里只放状态和
// 句柄，因为紧接着的 showScreen() 就要判断"锁定期间只有锁屏能上"。
//
// ⚠ 锁定**不进** currentSysMode。
//   currentSysMode 说的是"当前是哪个界面"，锁定说的是"整台键盘不往主机发字"，
//   两者正交 —— 锁着的时候可能正在菜单里、可能在宠物页、可能正要息屏。
//   塞进 currentSysMode 的话，一换界面锁定就"掉了"，那不叫锁定。
static bool kbLocked = false;
static lv_obj_t* scr_lock = nullptr;

// Screen切换函数
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    // 锁定期间**只有锁屏能上**。
    //
    // 这一条必须在这里统一挡，不能指望各个调用点自己判断：息屏、宠物探头、
    // 倒计时全屏、菜单、设置页……全都会各自调 showScreen()，分散去挡一定
    // 会漏一处，漏一处就表现为"锁着锁着屏幕自己变了"——而锁屏还好端端在后台，
    // 看起来就像"锁失效了"。
    //
    // 解锁的顺序是"先清 kbLocked 再 showScreen()"，所以解锁不受这条拦。
    if (kbLocked && target != scr_lock) return;
    if (currentScreen == target) return;
    currentScreen = target;
    lv_scr_load(target);
}

// 整屏重建时的安全换屏：**先建新屏并切过去，再删旧屏**。
//
// 之前录制页/设置页都是 `lv_obj_del(scr_xxx); scr_xxx = lv_obj_create(NULL);`，
// 而这些页面在被重建时通常正是当前活动屏。LVGL 8.4 删掉活动屏时会把
// disp->act_scr 置成 NULL（lv_obj_tree.c 里那段 act_scr_del 分支），
// 紧接着的刷新就解引用空指针 → panic → 重启。这就是"按 MR 进录制后重启"的成因。
//
// 顺序反过来就没事：切到新屏之后旧屏已经不是活动屏，删它是安全的。
static void swapScreen(lv_obj_t** slot, lv_obj_t* next) {
    if (next == nullptr) return;
    lv_obj_t* old = *slot;
    *slot = next;
    showScreen(next);
    if (old != nullptr && old != next) lv_obj_del(old);
}

// HUD浮层
static lv_obj_t* scr_hud = nullptr;

// 通知面板
static lv_obj_t* notif_bg = nullptr;
static lv_obj_t* notif_label = nullptr;
static unsigned long notifShowMs = 0;
static unsigned long notifStartMs = 0;

// 极客仪表盘
static lv_obj_t* gk_bg = nullptr;
static lv_obj_t* gk_lbl_clock = nullptr;
static lv_obj_t* gk_lbl_date = nullptr;
static lv_obj_t* gk_lbl_temp = nullptr;
static lv_obj_t* gk_lbl_hum = nullptr;
static lv_obj_t* gk_lbl_keys = nullptr;
static lv_obj_t* gk_lbl_lastkey = nullptr;   // 中间的"最近按键"反馈
static lv_obj_t* gk_led_num = nullptr;
static lv_obj_t* gk_led_caps = nullptr;
static lv_obj_t* gk_led_scr = nullptr;
static lv_obj_t* gk_lbl_profile = nullptr;

// 大时钟
static lv_obj_t* bc_bg = nullptr;
static lv_obj_t* bc_lbl_time = nullptr;
static lv_obj_t* bc_lbl_date = nullptr;

// 信息面板
static lv_obj_t* ip_bg = nullptr;
static lv_obj_t* ip_lbl_clock = nullptr;
static lv_obj_t* ip_lbl_date = nullptr;
static lv_obj_t* ip_circle_num = nullptr;
static lv_obj_t* ip_circle_caps = nullptr;
static lv_obj_t* ip_circle_scr = nullptr;
static lv_obj_t* ip_lbl_profile = nullptr;
static lv_obj_t* ip_lbl_lastkey = nullptr;
static lv_obj_t* ip_lbl_keys = nullptr;
// 信息面板的锁状态：圆点 / 文字 / 整颗胶囊，三层一起变亮灭
static lv_obj_t* ipLockChip[3] = { nullptr, nullptr, nullptr };
static lv_obj_t* ipLockDot[3]  = { nullptr, nullptr, nullptr };
static lv_obj_t* ipLockLbl[3]  = { nullptr, nullptr, nullptr };

// 击键监控
static lv_obj_t* km_bg = nullptr;
static lv_obj_t* km_lbl_lastkey = nullptr;
static lv_obj_t* km_lbl_keys = nullptr;
static lv_obj_t* km_lbl_profile = nullptr;
static lv_obj_t* km_lbl_title = nullptr;

// 律动
static lv_obj_t* rh_bg = nullptr;
static lv_obj_t* rh_lbl_keys = nullptr;
static lv_obj_t* rh_lbl_profile = nullptr;
static lv_obj_t* rh_bars[24] = { nullptr };
static uint8_t rhythmBars[24] = {0};

// 壁纸
static lv_obj_t* wp_bg = nullptr;
static lv_obj_t* wp_img = nullptr;
static lv_obj_t* wp_lbl_time = nullptr;
static lv_obj_t* wp_lbl_date = nullptr;

// ===========================================================
// 底部通栏条（壁纸主屏 + 屏保共用）
// ===========================================================
// 高度 72 / 起点 168 是**和壁纸图一起定死**的：
//   wallpaper/gen_wallpaper_240_v2.py 里把表盘圆心放在 (120,92)、R=76，
//   弧线最低点正好 y=168，再往下 72px（168..240）整片留空给这条。
// 两边任何一边改了坐标，另一边必须同步改，否则条会压到弧线或者露出空隙。
#define BAR_H 72
#define BAR_Y (240 - BAR_H)

// 高对比度（B-1「横向战舰」纯黑 + 横向驾驶舱：LED顶栏 / 时-日卡 / 键名卡 / 数据带 / 状态行）
static lv_obj_t* hc_bg = nullptr;
// 顶栏 3 颗 LED 圆点 + 下方数字标（亮 = 锁色实心，灭 = 描边空圈）
static lv_obj_t* hc_led_dot[3]   = { nullptr, nullptr, nullptr };
static lv_obj_t* hc_lbl_lock[3]  = { nullptr, nullptr, nullptr };
static uint32_t  hc_lockOn[3]    = { 0, 0, 0 };
// 顶栏右侧：方案图标（M 档 32x32）+ 方案名
static lv_obj_t* hc_img_profile = nullptr;
static lv_obj_t* hc_lbl_profile = nullptr;
// 主卡片 1（y=52..118, w=220, h=66）：时间 + 日期
static lv_obj_t* hc_lbl_time = nullptr;
static lv_obj_t* hc_lbl_date = nullptr;
// 主卡片 2（y=130..196, w=220, h=66）：大红色按键名（双字号预创建，按字符数切换）
static lv_obj_t* hc_lbl_lastkey_48 = nullptr;
static lv_obj_t* hc_lbl_lastkey_28 = nullptr;
// 主卡片左装饰条（青 / 红）
static lv_obj_t* hc_card_time_strip = nullptr;
static lv_obj_t* hc_card_key_strip  = nullptr;
// 数据带 3 列（y=202..234, w=220, h=32）：标签 + 主值 + 进度条背景 + 进度条实条
static lv_obj_t* hc_lbl_kpm         = nullptr;
static lv_obj_t* hc_lbl_kpm_num     = nullptr;
static lv_obj_t* hc_bar_kpm         = nullptr;   // 60×4 灰色背景条
static lv_obj_t* hc_bar_kpm_fill    = nullptr;   // 0..60 宽 实条（实时变长 + 变颜色）
static lv_obj_t* hc_lbl_today       = nullptr;
static lv_obj_t* hc_lbl_today_num   = nullptr;
static lv_obj_t* hc_bar_today       = nullptr;
static lv_obj_t* hc_bar_today_fill  = nullptr;
static lv_obj_t* hc_lbl_active      = nullptr;
static lv_obj_t* hc_lbl_active_num  = nullptr;
static lv_obj_t* hc_bar_active      = nullptr;
static lv_obj_t* hc_bar_active_fill = nullptr;
// 底部状态条（y=178..234, w=220, h=56）：左侧 2/3 是温度 / 湿度两根进度条，
// 右侧 1/3 是字数。进度条是「槽 + 填充」两层 lv_obj，不是 lv_bar ——
// 这个项目通篇没引过 lv_bar（见 build_style_high_contrast 的说明）。
static lv_obj_t* hc_lbl_strip_t     = nullptr;   // "温度" 说明
static lv_obj_t* hc_lbl_strip_t_val = nullptr;   // 温度数值
static lv_obj_t* hc_lbl_strip_chars = nullptr;   // "字数" 说明
static lv_obj_t* hc_lbl_strip_chars_val = nullptr; // 字数数值
// 底部状态条那 12 个平铺对象里的两个：装饰框和竖分隔线。
// 原来是 build_style_high_contrast 里 { } 块中的局部变量、出了函数就没句柄，
// 探头的"底部让位"要隐藏它们，只能提为全局。
static lv_obj_t* hc_bot_box = nullptr;
static lv_obj_t* hc_bot_div = nullptr;
static lv_obj_t* hc_lbl_strip_h     = nullptr;   // "湿度" 说明
static lv_obj_t* hc_lbl_strip_h_val = nullptr;   // 湿度数值
static lv_obj_t* hc_bar_t_track     = nullptr;   // 温度进度条槽（底）
static lv_obj_t* hc_bar_t_fill      = nullptr;   // 温度进度条填充（0..槽宽）
static lv_obj_t* hc_bar_h_track     = nullptr;   // 湿度进度条槽
static lv_obj_t* hc_bar_h_fill      = nullptr;   // 湿度进度条填充

// 「方案+按键」页（DISP_MODE_PLAN_KEY）
// 版面：顶栏是这一页自己的（左上标题 + 右上三颗长方形锁灯），**不建** dashTopBar()；
// 主体左边 62px 放**当前方案那一个**图标，右边 164px 放按键卡；底部两条窄条是温湿度。
static lv_obj_t* pk_bg          = nullptr;
static lv_obj_t* pk_lbl_title   = nullptr;   // "YYQ键盘"
static lv_obj_t* pk_lock[3]     = { nullptr, nullptr, nullptr };  // 三颗长方形灯的底
static lv_obj_t* pk_lockLbl[3]  = { nullptr, nullptr, nullptr };  // 灯上的 NUM/CAPS/SCR
static lv_obj_t* pk_icon        = nullptr;   // 当前方案的图标（mac=苹果 / win=Windows）
static lv_obj_t* pk_iconCard    = nullptr;   // 图标后面那块高亮底卡
static lv_obj_t* pk_card        = nullptr;   // 右侧按键卡
static lv_obj_t* pk_lbl_key     = nullptr;   // 按键名（字号动态选）
static lv_obj_t* pk_lbl_todayCap= nullptr;   // "TODAY"
static lv_obj_t* pk_lbl_today   = nullptr;   // 今日按键数
static lv_obj_t* pk_lbl_tCap    = nullptr;   // "温度"
static lv_obj_t* pk_lbl_tVal    = nullptr;   // 温度值
static lv_obj_t* pk_lbl_hCap    = nullptr;   // "湿度"
static lv_obj_t* pk_lbl_hVal    = nullptr;   // 湿度值
static lv_obj_t* pk_bar_tTrack  = nullptr;   // 温度条槽
static lv_obj_t* pk_bar_tFill   = nullptr;   // 温度条填充
static lv_obj_t* pk_bar_hTrack  = nullptr;   // 湿度条槽
static lv_obj_t* pk_bar_hFill   = nullptr;   // 湿度条填充
// 记着"当前贴的是哪个方案的图标"。lv_img_set_src() 每次调用都会 invalidate
// + 重排版，无脑每 100ms 调一次等于屏幕永远 dirty —— 所以只在方案真的变了
// 的时候才动。（resetStylePointers / build 时会把它重置成不可能值。）
static uint8_t pkShownProfile = 0xFF;
// 记着"上一次按下的键"。用来判断按键名要不要重挑字号。
// ⚠ 必须是**全局**而不是函数里的 static：函数 static 的生命周期等于整个
// 程序，跨「主屏重建」都不会清。主屏一重建（息屏 60 秒后唤醒、或者切一次
// 显示风格）pk_lbl_key 就回到 build 时写死的 48 号，而这时的 pkLastKey
// 仍等于 lastKeyPressed → 守卫不成立 → 不会重挑字号 → "NumEnt" 用 48 号
// 渲染是 207px，撑破卡片、压住左列图标、还跑出屏幕右边缘。
// 所以：build 时直接按当前键名定一次字号 + 这里重建时清掉缓存。
static char pkLastKey[8] = "";

// ===========================
// 壁纸：JPEG 上传 + 解码缓冲
// ===========================
// 网页端本地已经把原图缩到 240x240 并压成 JPEG（十几 KB），BLE 写的是
// LOGO_JPEG_START:<长度> + 一串裸字节。固件收齐后用 JPEGDEC 解成 RGB565，
// 落到 PSRAM 里的一张常驻缓冲，同时再写一份 /logo.bin 到 SPIFFS —— 重启后
// 直接从盘上读回，不用再传一遍。
//
// 缓冲是 PSRAM 而不是内部 DRAM：240*240*2 = 115KB，内部 DRAM 装不下。
#define WP_W 240
#define WP_H 240
#define WP_PIXELS (WP_W * WP_H)

static uint16_t*    wpPixels  = nullptr;   // 解码后的 RGB565，LVGL 的图片源
static lv_img_dsc_t wpImgDsc;              // 喂给 lv_img_set_src 的描述符
static bool         wpReady   = false;     // wpPixels 里有有效像素

// 上传中的 JPEG 原始字节。logoRxBuf 非空 == 正处于二进制接收模式，
// 此时 BLE 写进来的东西一律当 JPEG 数据，不当文本指令。
#define LOGO_RX_MAX        (256 * 1024)
#define LOGO_RX_TIMEOUT_MS 5000

static uint8_t*     logoRxBuf   = nullptr;
static uint32_t     logoRxCap   = 0;            // logoRxBuf 的**真实**容量（字节）
static uint32_t     logoRxTotal = 0;
static uint32_t     logoRxGot   = 0;
static bool         logoRxDone  = false;
static bool         logoRxActive = false;    // 只有这个为 true 才处于二进制接收模式
static unsigned long logoRxLastMs = 0;
static uint16_t*    logoOutBuf  = nullptr;  // JPEGDEC 的绘制目标（解码期间有效）
static uint32_t     meHexBytes  = 0;         // ME_DATA 累计写进去的十六进制字符数（诊断用)

// ===========================
// RAW 传输：解码放在电脑端，板子只收 + 展示
// ===========================
//
// 这条路和网页端那条（JPEG → 板子上用 JPEGDEC 解）**并列**，不是替代：
//   · 网页端 s3-setting.html 继续走 JPEG（十几 KB，省流量）
//   · 命令行/自动化走 RAW（电脑端解成 RGB565 直接发）
//
// 为什么值得多这一条：
//   ① 板子上**彻底不需要 JPEG 解码器**了。JPEGDEC 那个对象有 12.6KB
//      （usUnalignedPixels / usHuffAC / ucHuffDC / ucFileBuf 四张大数组），
//      静态占着内部 DRAM；而且它的解析过程要在输入缓冲里大量跳转 ——
//      输入缓冲一旦在 PSRAM 上（缓存行跨步 + 未对齐访问）就不稳，
//      之前"上传壁纸必崩"就是踩在这上面。
//   ② RAW 的长度是**固定**的 240*240*2 = 115200 字节，不存在 JPEG
//      那种"声明多少就收多少"的可变长度，也就没有了越界那一类问题。
//   ③ 电脑端想怎么压都行（PNG/JPEG/WebP 随便读），板子上只管显示，
//      "发什么显示什么"这条链路更短，出问题时更好定位。
//
// 字节序（硬约定，两边必须一致）：
//   LV_COLOR_DEPTH=16 且 **没有**开 LV_COLOR_16_SWAP ⇒ RGB565 就是
//   ESP32 上的 native 小端。所以电脑端按 little-endian 打包 uint16 即可，
//   和 JPEGDEC 的 RGB565_LITTLE_ENDIAN 落在内存里的样子完全一样。
#define WP_RAW_BYTES  (WP_PIXELS * 2)

static uint16_t*    logoRawBuf  = nullptr;  // RAW 接收目标：115200 字节，PSRAM 常驻
static bool         logoRxRaw   = false;    // 当前是不是 RAW 模式（JPEG 模式为 false）

// ME 存储改成"ME_START 开一次 fd、ME_DATA 只写、ME_END 关"（原因见 handleCommand
// 里 ME_START 那段注释）。这三个变量配合 loop() 里的超时兜底：
// 网页发一半跑掉时不能把 fd 一直攥着 —— SPIFFS 默认只允许 10 个同时打开的文件，
// 漏几个之后连 /logo.bin 都打不开了。
static File         meFile;                  // 非空 = 正在收 ME 文本
static uint32_t     meWriteErrors = 0;       // 长写/短写次数，ME_END 时据此报错
static unsigned long meLastDataMs = 0;       // 最后一片 ME_DATA 的时间，供超时兜底
#define ME_RX_TIMEOUT_MS 5000UL

// 实现都在"构建：壁纸模式"那一段；onWrite 跑在 BLE 主机任务里、位置更靠前，
// 这里先声明，壁纸上传的分流才能在那儿用上。
static int  logoJpegDraw(JPEGDRAW* d);
static void handleLogoChunk(const uint8_t* data, size_t len);
static void finishLogoUpload(void);
static void abortLogoUpload(const char* reason);
static void loadWallpaperFromDisk(void);
static uint8_t* logoRxAlloc(uint32_t total);

// 主屏通用顶部条（6 种风格共用）
//   左边：3 颗锁状态灯，只画圆点、不写 NUM/CAP/SCR 文字
//   右边：当前方案 —— 1/2 号方案是 Windows / macOS，直接画系统图标；3/4 只显序号
static lv_obj_t* topLockDot[3] = { nullptr, nullptr, nullptr };
static lv_obj_t* topLockRing[3] = { nullptr, nullptr, nullptr };   // 点亮时的外圈
static uint32_t   topLockOn[3] = { 0, 0, 0 };
// 上一帧的三颗锁状态，用来检测"哪一颗刚刚被切换了"（只为了弹一次 HUD）
static bool     lockPrev[3] = { false, false, false };
static bool     lockPrevValid = false;
static lv_obj_t* topProfileNum = nullptr;      // 方案序号 1~4
static lv_obj_t* topIconBox    = nullptr;      // 系统图标的槽位（lv_img，S 档 16×16）

// 菜单
static lv_obj_t* menu_cont = nullptr;
static lv_obj_t* menu_items[MENU_ROW_MAX];
static lv_obj_t* menu_items_val[MENU_ROW_MAX];
static lv_obj_t* menu_title = nullptr;
static lv_obj_t* menu_position = nullptr;
static lv_obj_t* menu_hint1 = nullptr;   // 底部按键提示第一行
static lv_obj_t* menu_hint2 = nullptr;   // 第二行

// 样式
static lv_style_t style_bg;
static lv_style_t style_text_white;
static lv_style_t style_text_cyan;
static lv_style_t style_text_gray;
static lv_style_t style_card;
static bool styles_inited = false;

// ===========================
// 设计体系（Design tokens）
// ===========================
// 之前是「纯黑底 + 纯白字 + 灰字」，元素之间没有任何层次，卡片底色 0x1A1A1A
// 压在纯黑上肉眼根本分不开，所以看起来像一堆浮在黑底上的字。
// 下面这套按「底 / 面 / 描边 / 文字三级 / 强调色」分层，层级一拉开就清楚了。
// 注意底色刻意不用纯黑：240x240 的小屏上纯黑配纯白对比过硬、发灰，
// 偏蓝的深灰（0x0B0F17）观感更稳，也更像成品。

#define CLR_BG        0x0B0F17   // 页面底
#define CLR_SURFACE   0x161C2B   // 卡片面
#define CLR_SURFACE_2 0x1F2739   // 抬升面 / 选中态
#define CLR_STROKE    0x2B3549   // 1px 描边

#define CLR_TEXT      0xE6EDF7   // 主文字
#define CLR_TEXT_DIM  0x8E9BB2   // 次级文字
#define CLR_TEXT_MUTE 0x5B6579   // 三级文字 / 单位

#define CLR_ACCENT    0x22D3EE   // 主强调（青）
#define CLR_ACCENT_D  0x0E7490   // 强调色暗部
#define CLR_VIOLET    0xA78BFA   // 次强调（紫）
#define CLR_AMBER     0xFBBF24   // 温度
#define CLR_GREEN     0x34D399   // 正常
#define CLR_RED       0xF87171   // 错误

// 兼容旧名字
#define CLR_BLACK  CLR_BG
#define CLR_WHITE  CLR_TEXT
#define CLR_CYAN   CLR_ACCENT
#define CLR_GRAY   CLR_TEXT_DIM
#define CLR_DARK   CLR_SURFACE
#define CLR_ACCENT_OLD CLR_ACCENT

// 三颗锁状态灯的语义色（NUM / CAPS / SCR），
// 屏幕上的胶囊色和 C3 上 16/17/18 号物理指示灯共用这一份，两边永远一致。
static const uint32_t lockLedColor[3] = { CLR_GREEN, CLR_ACCENT, CLR_AMBER };


// ===========================
// 主屏生命周期
// ===========================
// 主屏必须是一个真正的 lv_obj_t 屏幕对象。
//
// 之前的写法：6 种主屏风格都建在 lv_scr_act() 上，而 scr_main 声明成 nullptr
// 之后再也没被赋过值。于是每一处 showScreen(scr_main) 都撞在
// `if (target == nullptr) return;` 上直接返回，屏幕上什么都没发生 ——
// 表现就是"菜单里按 ESC / 选中'返回主屏' / 再按一次 MC，全都没反应"。
// 副作用还有两个：菜单载入后 lv_scr_act() 变成菜单屏，此时再切主屏风格会把
// 仪表盘控件直接糊在菜单上；息屏删掉活动屏后 currentScreen 也成了悬垂指针。
//
// 所以这里统一由 ensureMainScreen() 负责建/取主屏，风格一律往它上面建。
static bool mainContentValid = false;

// 诊断开关：哪些主屏风格允许构建（见 renderCurrentDisplayBase）。
// 整组停用后整机恢复正常 -> 卡死来自风格渲染，这里改成位掩码逐个试，
// 一次只放开一两种就能定位到具体是哪个 build_style_* 把机器拖死。
// 六个位全打开 = 完全恢复原来的 6 种主屏风格。
#define STYLE_BIT_GEEK            (1u << DISP_MODE_GEEK)
#define STYLE_BIT_BIG_CLOCK       (1u << DISP_MODE_BIG_CLOCK)
#define STYLE_BIT_INFO_PANEL      (1u << DISP_MODE_INFO_PANEL)
#define STYLE_BIT_KEY_MON         (1u << DISP_MODE_KEY_MON)
#define STYLE_BIT_RHYTHM          (1u << DISP_MODE_RHYTHM)
#define STYLE_BIT_WALLPAPER       (1u << DISP_MODE_WALLPAPER)
#define STYLE_BIT_HIGH_CONTRAST   (1u << DISP_MODE_HIGH_CONTRAST)
#define STYLE_BIT_PLAN_KEY        (1u << DISP_MODE_PLAN_KEY)
#define STYLE_BIT_ALL             (STYLE_BIT_GEEK | STYLE_BIT_BIG_CLOCK | STYLE_BIT_INFO_PANEL | \
                                   STYLE_BIT_KEY_MON | STYLE_BIT_RHYTHM | STYLE_BIT_WALLPAPER | \
                                   STYLE_BIT_HIGH_CONTRAST | STYLE_BIT_PLAN_KEY)
// 六种风格全开。之前的"全开就整机卡死"不是风格本身重，是 topLockDot[] 这组
// 共用指针在切换时没清干净，updateDynamicElements() 一直在写已释放的内存
// （详见 resetStylePointers 里的注释）。这个坑填掉之后掩码就不再是必需品了。
static const uint32_t MAIN_STYLE_MASK = STYLE_BIT_ALL;
static void renderCurrentDisplayBase(void);   // 定义在文件后段，这里先声明

static lv_obj_t* ensureMainScreen(void) {
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        lv_obj_set_style_border_width(scr_main, 0, LV_PART_MAIN);
        lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    }
    return scr_main;
}

// 息屏用：把主屏整个拆掉。
// lv_obj_del(scr_main) 会连带释放全部子控件，所以 gk_bg / bc_bg / ... 这些
// 全局指针必须同时置空。否则下一次 renderCurrentDisplayBase() 里那六行
// `lv_obj_del(gk_bg)` 就是在对已释放的内存调用析构，直接踩坏堆。
static void resetStylePointers(void) {
    gk_lbl_clock = nullptr; gk_lbl_date = nullptr; gk_lbl_temp = nullptr;
    gk_lbl_hum = nullptr; gk_lbl_keys = nullptr; gk_lbl_profile = nullptr;
    gk_lbl_lastkey = nullptr;
    gk_led_num = nullptr; gk_led_caps = nullptr; gk_led_scr = nullptr;

    bc_lbl_time = nullptr; bc_lbl_date = nullptr;

    ip_lbl_clock = nullptr; ip_lbl_date = nullptr;
    ip_circle_num = nullptr; ip_circle_caps = nullptr; ip_circle_scr = nullptr;
    ip_lbl_profile = nullptr; ip_lbl_lastkey = nullptr; ip_lbl_keys = nullptr;
    for (int i = 0; i < 3; i++) { ipLockChip[i] = nullptr; ipLockDot[i] = nullptr; ipLockLbl[i] = nullptr; }

    km_lbl_title = nullptr; km_lbl_lastkey = nullptr;
    km_lbl_keys = nullptr; km_lbl_profile = nullptr;

    rh_lbl_keys = nullptr; rh_lbl_profile = nullptr;
    for (int i = 0; i < 24; i++) rh_bars[i] = nullptr;

    wp_img = nullptr; wp_lbl_time = nullptr; wp_lbl_date = nullptr;

    // 高对比度（B-1「横向战舰」所有指针）
    hc_lbl_time = nullptr; hc_lbl_date = nullptr;
    hc_lbl_lastkey_48 = nullptr; hc_lbl_lastkey_28 = nullptr;
    for (int i = 0; i < 3; i++) { hc_led_dot[i] = nullptr; hc_lbl_lock[i] = nullptr; }
    hc_img_profile = nullptr; hc_lbl_profile = nullptr;
    hc_card_time_strip = nullptr; hc_card_key_strip = nullptr;
    hc_lbl_kpm = nullptr; hc_lbl_kpm_num = nullptr;
    hc_bar_kpm = nullptr; hc_bar_kpm_fill = nullptr;
    hc_lbl_today = nullptr; hc_lbl_today_num = nullptr;
    hc_bar_today = nullptr; hc_bar_today_fill = nullptr;
    hc_lbl_active = nullptr; hc_lbl_active_num = nullptr;
    hc_bar_active = nullptr; hc_bar_active_fill = nullptr;
    hc_lbl_strip_t = nullptr; hc_lbl_strip_t_val = nullptr;
    hc_lbl_strip_chars = nullptr; hc_lbl_strip_chars_val = nullptr;
    hc_bot_box = nullptr; hc_bot_div = nullptr;
    hc_lbl_strip_h = nullptr; hc_lbl_strip_h_val = nullptr;
    hc_bar_t_track = nullptr; hc_bar_t_fill = nullptr;
    hc_bar_h_track = nullptr; hc_bar_h_fill = nullptr;

    // 「方案+按键」页的指针。**一个都不能漏** —— 漏一个就是上面注释里
    // 说的那种野指针：切走风格后 updateDynamicElements() 每 100ms 往已释放
    // 的地址上写一次，直接踩坏堆。
    pk_lbl_title = nullptr; pk_card = nullptr;
    pk_lbl_key = nullptr;
    pk_lbl_todayCap = nullptr; pk_lbl_today = nullptr;
    pk_lbl_tCap = nullptr; pk_lbl_tVal = nullptr;
    pk_lbl_hCap = nullptr; pk_lbl_hVal = nullptr;
    pk_bar_tTrack = nullptr; pk_bar_tFill = nullptr;
    pk_bar_hTrack = nullptr; pk_bar_hFill = nullptr;
    for (int i = 0; i < 3; i++) { pk_lock[i] = nullptr; pk_lockLbl[i] = nullptr; }
    pk_icon = nullptr; pk_iconCard = nullptr;
    pk_bg = nullptr;   // 容器本身也清一下（两个调用点其实已经先清过了，
                       // 这里只是双保险 —— 这个文件的注释里已经为同类坑
                       // 写过血书，漏一个就是整机重启）
    pkShownProfile = 0xFF;   // 图标被释放了，下次重建必须重新贴图
    pkLastKey[0] = '\0';     // 按键名的字号缓存也失效了，下次重建必须重挑

    // 顶部条是 6 种风格**共用**的一套全局指针，dashTopBar() 建谁就指向谁。
    // 之前漏在这里清理，就踩了和 ipLockDot[] 一模一样的坑，而且这次更隐蔽：
    //
    //   从"已启用的风格"切到"被掩码关掉的风格"时，上面刚把 gk_bg 之类的容器删了，
    //   这里却把 topLockDot[] 留在已释放的地址上；紧接着 renderCurrentDisplayBase()
    //   因为掩码命中而提前 return，dashTopBar() 不会再来覆盖它们。于是
    //   updateDynamicElements() 每 100ms 就往 4 个野指针上写 —— 必崩。
    //
    // 这正是"切到律动就崩"（以及之前"切到击键监控就崩"）的真凶：崩的不是新风格，
    // 是切过去之后仍在刷新上一风格的顶部条。setBgColor 的 nullptr 检查拦不住，
    // 因为它不是 nullptr，是被释放后又被复用的地址。
    for (int i = 0; i < 3; i++) topLockDot[i] = nullptr;
    for (int i = 0; i < 3; i++) topLockRing[i] = nullptr;
    lockPrevValid = false;
    topProfileNum = nullptr;
    topIconBox = nullptr;
}
static void destroyMainScreen(void) {
    gk_bg = nullptr; bc_bg = nullptr; ip_bg = nullptr;
    km_bg = nullptr; rh_bg = nullptr; wp_bg = nullptr;
    hc_bg = nullptr;
    pk_bg = nullptr;
    resetStylePointers();   // 顶部条指针在这里一并清掉

    if (scr_main != nullptr) {
        // **删活动屏之前必须先切走**。
        //
        // LVGL 8.4 的 lv_obj_del() 在删掉的正好是活动屏时，会把
        // disp->act_scr 直接置成 NULL（lv_obj_tree.c:73 那句
        // `if(act_scr_del) disp->act_scr = NULL;`）。之后 lv_scr_act() 就一直
        // 返回 NULL，任何解引用它的操作都是野指针 panic。
        //
        // 这条路径不是理论风险：SLEEP_TIMEOUT_MS = 60000，息屏时
        // destroyMainScreen() 删的正是当时唯一那块活动屏 —— "跑着一分多钟
        // 之后自己重启"的时间点跟它对得上。旁边 swapScreen() 早就为了同一个
        // 坑把顺序改成了"先切后删"，这里漏了。
        //
        // 先造一块空白屏顶上，主屏就不再是活动屏，删它就安全了。
        if (lv_scr_act() == scr_main) {
            lv_obj_t* blank = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(blank, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
            lv_obj_clear_flag(blank, LV_OBJ_FLAG_SCROLLABLE);
            currentScreen = blank;
            lv_scr_load(blank);
        } else if (currentScreen == scr_main) {
            // currentScreen 指着要删的主屏，但它此刻不是活动屏（活动的是
            // 倒计时全屏 / 菜单 / 设置页）。先摘掉这个指针，别让它悬在
            // 已释放的地址上，后面谁拿它去 lv_scr_load 就是 use-after-free。
            currentScreen = nullptr;
        }
        lv_obj_del(scr_main);
        scr_main = nullptr;
    }
    mainContentValid = false;
}

// 屏保的实现在文件后段，这里先声明（gotoMainScreen() 要用）
static void destroyScreensaver(void);

// 回到主屏的统一出口：保证主屏和主屏内容都还在，然后真正切过去。
static void gotoMainScreen(void) {
    currentSysMode = SYS_MODE_NORMAL;
    // 退出菜单时把两级状态都归零。下次按 MC 进菜单是从大类列表开始，
    // 而不是停在上次翻到的那一层的第几项。
    menuSel = 0;
    menuItemSel = 0;
    menuInSub = false;
    menuScrollOffset = 0;
    recStage = REC_STAGE_EXIT;   // 退出演录状态机，下次 MR 重新从 SEQ 开始
    lightFieldIdx = 0;

    // 息屏把主屏拆过的话，这里要把内容一并重建，否则切过去是一片黑
    if (!mainContentValid) renderCurrentDisplayBase();
    // 顺序有讲究：先真的把主屏切上去，屏保才不再是活动屏，
    // 下面销毁它才安全（LVGL 8.4 删掉活动屏会把 disp->act_scr 置 NULL）。
    showScreen(ensureMainScreen());
    destroyScreensaver();
}

// ===========================
// 前向声明
// ===========================
static void handleCommand(const String& cmd);
static void scanKeyboardMatrix(void);
static bool recoverI2CBus(void);
static bool mcpColumnStrobeOk(void);
static uint8_t mcpReadIODIR(void);
static void i2cStrobe(int pulses, uint16_t halfUs);
static void mcpApplyColumnConfig(void);
static void mcpResetMatrixState(void);
static void mcpHardwareReset(void);
static bool mcpRestartChip(void);
static bool mcpIdleRowsClean(void);
static bool mcpGhostBurstAt(int r, int c);
static void mcpTripFault(void);
static uint32_t mcpAutoRecoverCount(void);
void bootDetailTick(void);
static void renderCurrentDisplayBase(void);
static void updateDynamicElements(void);
static void tickRhythm(void);
static void pulseRhythm(void);
static void triggerHud(const char* title, const char* value, lv_color_t color);
static void triggerHudBig(const char* title, const char* value, lv_color_t color);
static void pushNotification(AlertType type, const String& text);
static void drawNotifPanel(void);
static void showScreen(lv_obj_t* target);
static void swapScreen(lv_obj_t** slot, lv_obj_t* next);
// 删屏前先换屏的统一出口。定义在文件后段（挨着倒计时全屏那块），
// 但 destroyScreensaver() 在前面就要用。.ino 由 Arduino 预处理成 .cpp 时
// 会自动生成原型，所以不声明也能编过 —— 但那依赖工具链的自动补声明行为，
// 一旦这个函数被搬进 .cpp 就立刻编不过。自己写上，不给编译期留惊喜。
static lv_obj_t* swapAwayIfActive(lv_obj_t* doomed, lv_obj_t* replacement);
static lv_obj_t* ensureMainScreen(void);
static void gotoMainScreen(void);
static void mkCard(lv_obj_t* o, uint32_t bg, uint8_t radius);
static void mkChip(lv_obj_t* o, uint32_t bg, uint32_t fg, uint8_t radius);
static void mkLabel(lv_obj_t* l, const lv_font_t* f, uint32_t color);
static void setText(lv_obj_t* lbl, const char* txt);
static void destroyMainScreen(void);
static void destroyScreensaver(void);
static void enterScreensaver(void);
static void cycleScreensaverMode(void);
static void updateScreensaver(bool force);
static void triggerKeyReaction(void);
static void handleMenuSelect(void);
// build_menu() 在定义之前就被 setScreensaverMode() 调到了（改屏保模式要刷新菜单角标）。
// Arduino 把 .ino 预处理成 .cpp 时会自动补原型，能编过；但那依赖工具链行为，
// 显式写上，不给编译期留惊喜。
static void build_menu(void);
static const MenuGroup* curMenuGroup(void);
static const char* getKeyName(uint16_t code);
static void build_settings_time(void);
static void build_settings_alarm(void);
static void build_settings_timer(void);
static void build_settings_caltemp(void);
static void build_settings_light(void);
static void moveSettingField(int dir);
static void adjustSettingField(int delta);
// 响铃 / 倒计时全屏（见「响铃引擎」处的注释）
static void startRinging(RingKind kind);
static void stopRinging(void);
static void drawRingOverlay(void);
static void updateTimers(void);
static void startCountdown(uint32_t totalSec);
static void stopCountdown(bool silent);
static void toggleCountdownFromKeyboard(void);
static void updateCountdownVisibility(void);
static void destroyCountdownScreen(void);
static void buildCountdownScreen(void);
static void updateCountdownScreenText(void);
static void saveSettingScreen(void);
static void cancelSettingScreen(void);
static void update_setting_time_display(void);
static void update_setting_alarm_display(void);
static void update_setting_timer_display(void);
static void update_setting_caltemp_display(void);
static void update_setting_light_display(void);
static void update_recording_display(void);
static void build_recording(void);
static void build_elog(void);
static void elogKey(uint16_t baseKey);
static void elog_notePrevRun(void);
static void build_help(void);
static void helpKey(uint16_t baseKey);
static void helpScrollBy(int d);
static void helpClose(void);
static void finishMacroRecording(const String& targetKey);
static void enterRecording(void);
static void advanceRecordingStage(void);
static void cancelRecording(void);
static void cycleDisplayStyle(void);

// ===========================
// SHT31 温湿度
// ===========================
static uint8_t sht31_crc8(const uint8_t* data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static bool sht31_read_raw(uint16_t& rawT, uint16_t& rawH) {
    Wire_SHT.beginTransmission(SHT31_ADDR);
    Wire_SHT.write(0x2C);
    Wire_SHT.write(0x06);
    if (Wire_SHT.endTransmission() != 0) return false;
    delay(20);
    if (Wire_SHT.requestFrom((int)SHT31_ADDR, 6) != 6) return false;
    uint8_t buf[6];
    for (int k = 0; k < 6; k++) buf[k] = Wire_SHT.read();
    if (sht31_crc8(&buf[0], 2) != buf[2]) return false;
    if (sht31_crc8(&buf[3], 2) != buf[5]) return false;
    rawT = ((uint16_t)buf[0] << 8) | buf[1];
    rawH = ((uint16_t)buf[3] << 8) | buf[4];
    return true;
}

static void sht31_update(void) {
    uint16_t rawT, rawH;
    if (!sht31_read_raw(rawT, rawH)) {
        // 读取失败（总线错 / 返回字节数不对 / CRC 不过）一律记一条。
        // 去重会把它合并成一条并累加次数，所以 15 分钟一次的周期读失败
        // 不会刷屏，但"这颗传感器其实一直读不出来"会变成一条醒目的记录。
        ELWARN("SHT", "读取失败 温湿度保持上次数值");
        return;
    }
    float t = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    float h = 100.0f * ((float)rawH / 65535.0f);
    // 偏移直接加，不要再叠一个基准值 —— 见 SHT_TEMP_OFFSET_DEFAULT 上面的说明
    shtTemp = t + shtTempOffset;
    shtHumidity = h;
}

// ===========================
// BLE 回调
// ===========================
//
// 连接状态交给 bt_link：它要在断链时把正在排队的响应清掉（发出去也没人收，
// 还会把下一条挤掉），所以别在 .ino 里另存一份 deviceConnected。
//
// ---- 广播状态跟踪 ----
//
// 为什么需要它：Arduino 的 BLEAdvertising **没有** isAdvertising()，
// 而 esp_ble_gap_get_state() 在这个 SDK（IDF 4.4）里也不存在。
// 不跟踪就不知道"现在到底在不在广播"，于是只能盲目重试 ——
// 而盲目重试的代价是每次都会重新配一遍广播数据、打断正在进行的广播，
// 反而让主机更难连上。
//
// BLEDevice::setCustomGapHandler 是**追加**在内部处理之后的（见 BLEDevice.cpp
// 第 277 行），不会抢走库自己的逻辑，所以这里可以安全地挂一个自己的处理器，
// 只看广播开始/结束这两个事件。
//
// ⚠ 关键：Arduino 那套封装**不能**用来判断"广播起来没有"。
//   BLEAdvertising::start()（BLEAdvertising.cpp:186）里有两处静默失败：
//     · :216 esp_ble_gap_config_adv_data() 出错 → log_e 后**直接 return**，
//       底下的 esp_ble_gap_start_advertising() 根本不会被调用；
//     · :247 esp_ble_gap_start_advertising() 出错 → log_e 后**直接 return**。
//   两条路都不往上抛、也没有返回值 —— 调用方完全看不出失败。
//   于是"重启广播"这一句调用如果正好踩中其中一条，设备就永远不再广播，
//   主机侧只看到"扫不到"，而板子上没有任何异常。
//
//   另外 BLEServer 在 ESP_GATTS_DISCONNECT_EVT 里**不会**自动重启广播
//   （BLEServer.cpp:206 的注释写着 "we also want to start advertising again"，
//   但代码里根本没做）—— 断链后广播是停掉的，必须我们自己拉起来。
//
// 所以这里改成一个小状态机，判据全部来自协议栈事件，不靠"调用过就算成功"：
//   ADV_IDLE    确定没在广播
//   ADV_PENDING 已经发起启动，等协议栈确认
//   ADV_RUNNING 协议栈确认在广播（ADV_START_COMPLETE, status=SUCCESS）
//   ADV_FAILED  协议栈明确拒了（status≠SUCCESS）或等确认超时
enum AdvState { ADV_IDLE = 0, ADV_PENDING, ADV_RUNNING, ADV_FAILED, ADV_STOPPING };

static volatile int   g_advState;          // 前置声明在 UART 指令通道那边（ADV_IDLE==0，零初始化即可）
static unsigned long  advRetryAtMs  = 0;      // 下次尝试重启广播的时刻
static unsigned long  advDeadlineMs = 0;      // PENDING 状态的确认超时
static uint8_t        advFailStreak;      // 连续失败次数（决定退避间隔）

// 我们自己留一份广播参数：和 Arduino BLEAdvertising 的默认值**完全一致**
// （见 BLEAdvertising.cpp 构造函数：adv_int_min=0x20 / adv_int_max=0x40 /
//   ADV_TYPE_IND / PUBLIC / ADV_CHNL_ALL / ALLOW_SCAN_ANY_CON_ANY），
// 区别是自己持有 —— 重启广播时直接调 IDF 接口，能拿到真实错误码。
static esp_ble_adv_params_t s_advParams = {};

static void myGapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT) {
        // status 不为 SUCCESS 时广播根本没起来 —— 必须让 loop() 知道，
        // 否则设备就"看不见了"，而现场什么日志都没有。
        // （字段名在这个 SDK（IDF 4.4）里是 adv_start_cmpl，不是 adv_start_complete）
        bool ok = (param != nullptr &&
                   param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        g_advState = ok ? ADV_RUNNING : ADV_FAILED;
        if (ok) advFailStreak = 0;
        LOG_PORT.printf("[ADV] start_complete ok=%d\n", ok ? 1 : 0);
    } else if (event == ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT) {
        // 这是**我们自己**发的 stop 的回执 —— 状态机自己知道在等什么，
        // 所以这里不改状态（改了会把 ADV_STOPPING 冲掉，下一轮又去 stop）。
        LOG_PORT.println("[ADV] stop_complete");
    } else if (event == ESP_GAP_BLE_ADV_TERMINATED_EVT) {
        // 广播被**终止**（最常见就是主机连上来了，控制器自动停广播）。
        // 这时状态要改回 IDLE，否则看护逻辑会以为"还在广播"而不再管。
        if (g_advState != ADV_STOPPING) g_advState = ADV_IDLE;
        LOG_PORT.printf("[ADV] terminated state=%d\n", (int)g_advState);
    }
}

// 第 1 步：先让控制器把广播停下来。
//
// 为什么必须先 stop 再 start：
//   实测（2026-10-22，UART0 日志）：主机连上时控制器自动停了广播，
//   但它内部"广播已启用"的标志**不一定**被清掉。这时候直接调
//   esp_ble_gap_start_advertising() 会**返回 ESP_OK 却什么都不做** ——
//     · 不报错（所以日志里一条 [ADV] 错误都没有）
//     · 不发 ADV_START_COMPLETE（所以也看不到"启动成功"）
//     · 也不会真的开始广播（所以设备再也扫不到）
//   当时的现场：第 1、2 轮断链后都成功重启了广播，第 3 轮就再也起不来。
//
//   先 stop 一次把控制器的状态清干净，再 start，才是确定可靠的做法。
//   stop 是异步的，所以拆成两段：这里只发 stop，真正 start 放到
//   loop() 里等 ADV_STOPPING 的期限到了再做（不能在 loop 里干等）。
static void btAdvStop() {
    esp_ble_gap_stop_advertising();     // 失败也无所谓（本来就没在广播）
    g_advState = ADV_STOPPING;
    advDeadlineMs = millis() + 120;     // 等控制器把 stop 收完
    LOG_PORT.println("[ADV] 发 stop，等收尾");
}

// 第 2 步：真正启动广播。
//
// 为什么**不**用 Arduino 的 BLEDevice::startAdvertising()：
//   ① 它每次都会重新 esp_ble_gap_config_adv_data() 配一遍广播数据，
//      等于把广播整个重建 —— 主机会看到设备"闪一下又没了"。
//   ② 它失败时是静默 return（BLEAdvertising.cpp:219 和 :250），拿不到错误码。
//
// 改成直接调 IDF 的 esp_ble_gap_start_advertising()：
//   · 广播数据在 setup 里已经由 Arduino 配过一次（设备名 + service UUID），
//     这里复用、不重复配，广播不会被重建；
//   · 拿得到真实错误码，可以写进日志（BLE 的 LOG:dump 能回拉出来）。
static esp_err_t btAdvStart() {
    esp_err_t err = esp_ble_gap_start_advertising(&s_advParams);
    g_advState    = ADV_PENDING;
    advDeadlineMs = millis() + 2000;   // 2 秒内没有协议栈确认就算失败
    if (err != ESP_OK) {
        advFailStreak++;
        LOG_PORT.printf("[ADV] start rc=%d streak=%u\n",
                        (int)err, (unsigned)advFailStreak);
    } else {
        LOG_PORT.println("[ADV] start 已发起，等确认");
    }
    return err;
}

class MyServerCallbacks : public BLEServerCallbacks {
    // pServer 要转给 bt_link：协商后的 MTU 只在 server 上（characteristic 的
    // getService() 是私有的，拿不到），拿不到就一直按 20 字节保守发。
    void onConnect(BLEServer* pServer)    { btLinkSetConnected(true, pServer); }
    void onDisconnect(BLEServer* pServer) { btLinkSetConnected(false); }

    // ★ 协商好的 MTU 由**蓝牙协议栈任务自己**报过来 ★
    //
    // 为什么不能由主循环去问 BLEServer：
    //   协商结果放在 BLEServer::m_connectedServersMap 这张 std::map 里，
    //   而这张表**正是蓝牙任务在改**（CONNECT_EVT 插、MTU_EVT 改、
    //   DISCONNECT_EVT 删）。主循环这时候去拷贝/查找它 =
    //   一边遍历一边被改 —— std::map 不是线程安全的，
    //   结果是踩坏堆、随机 panic。
    //   而且 Arduino 的 getPeerMTU() **没有 end() 检查**，
    //   conn_id 不存在时解引用 end() 是未定义行为，读出来的是垃圾 MTU
    //   → 按它算出来的包长会超 → 协议栈整条丢掉 → 静默丢片。
    //
    // onMtuChanged 是 ESP_GATTS_MTU_EVT 触发的，param 里直接带着协商结果，
    // 而且它跑在蓝牙任务里 —— 在这里取，天然没有竞争。
    void onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
        if (param != nullptr && param->mtu.mtu >= 23) {
            btLinkSetPeerMtu(param->mtu.mtu);
        }
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    // **这里绝不能直接调 handleCommand()**
    //
    // onWrite 跑在蓝牙协议栈自己的任务里，而 LVGL 的全部对象都归 loop() 那个
    // Arduino 任务所有。handleCommand() 一进去就是 triggerHud() /
    // drawNotifPanel() / renderCurrentDisplayBase()，全都直接 lv_obj_create /
    // lv_obj_del，既没拿 lvgl_port_lock，又和 loop() 里的
    // updateDynamicElements() / lvgl_driver_loop() 并发操作同一堆对象 ——
    // 两个任务同时改 LVGL 堆，坏掉的内存布局直接变成 panic 重启。
    //
    // 症状：主机发 ALERT:RED / ALERT:GREEN（或者 DISP_MODE:n 切风格）在弹通知的
    // 瞬间整机死机，而单独用键盘切风格一切正常 —— 差别就在"谁发起的"。
    //
    // 所以这个回调体只剩一行：把原始字节交给 bt_link，剩下的（喂狗、二进制
    // 分流、按行攒包、入队）都在那边，而解析和 LVGL 操作仍然留在 loop()
    // （btLinkPoll() → handleCommand），和键盘触发的路径在同一个任务里。
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string raw = pCharacteristic->getValue();
        if (raw.empty()) return;
        btLinkOnWrite((const uint8_t*)raw.data(), raw.size());
    }
};

// ===========================
// 蓝牙二进制旁路（壁纸 JPEG）
// ===========================
//
// 这一段是 bt_link 的"二进制协议"钩子，形状和上面那条文本通道是并列的：
// 蓝牙发过来的东西到底是**指令**还是**裸数据**，由这里判定，bt_link 负责在
// 文本通道之前先问一句。新增一种二进制协议（比如以后做固件推送）只要再挂
// 一组 probe/sink，不用去改 bt_link，也不用碰 onWrite。
//
// ⚠ probe / sink 都跑在蓝牙协议栈任务里：不许阻塞、不许碰 LVGL / NVS / SPI。
//
// JPEG 原始字节里必然有 0x00，走按行拼的文本通道会被在第一个 0 处截碎，
// 所以必须在它之前分流。唯一要放回命令通道的是网页端重传时补发的
// LOGO_JPEG_START —— 此时固件还卡在上一轮的接收模式里，得让它先看见这条
// 命令才复位。用 "LOGO_" 做暗号是安全的：JPEG 首字节必然是 0xFF，撞不上 ASCII。
//
// ⚠ 判断条件必须用 logoRxActive，不能用 "logoRxBuf != nullptr"：
//   接收缓冲现在是常驻的（传完也不释放），用指针判断会导致传完之后
//   蓝牙进来的**所有文本指令**都被当成 JPEG 字节吃掉 —— 表现为
//   上传一次壁纸之后，ME 键和其它蓝牙配置全部失灵。
static bool logoRxProbe(const uint8_t* data, size_t len) {
    if (!logoRxActive || logoRxDone) return false;
    return !(len >= 5 && memcmp(data, "LOGO_", 5) == 0);
}
static void logoRxSink(const uint8_t* data, size_t len) {
    handleLogoChunk(data, len);
}


// ===========================
// 样式工具
// ===========================
static void init_styles(void) {
    if (styles_inited) return;
    styles_inited = true;

    // 注意：lv_style_set_* 没有 LV_PART 参数（那是 lv_obj_set_style_* 才有的）
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(CLR_BG));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_text_color(&style_bg, lv_color_hex(CLR_TEXT));

    lv_style_init(&style_text_white);
    lv_style_set_text_color(&style_text_white, lv_color_hex(CLR_TEXT));

    lv_style_init(&style_text_cyan);
    lv_style_set_text_color(&style_text_cyan, lv_color_hex(CLR_ACCENT));

    lv_style_init(&style_text_gray);
    lv_style_set_text_color(&style_text_gray, lv_color_hex(CLR_TEXT_DIM));

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(CLR_SURFACE));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, lv_color_hex(CLR_STROKE));
    lv_style_set_radius(&style_card, 10);
    lv_style_set_pad_all(&style_card, 0);
}

// 卡片：底色 + 圆角 + 1px 描边，这一层是"好看"和"丑"的分水岭
static void mkCard(lv_obj_t* o, uint32_t bg, uint8_t radius) {
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// 小圆片（方案名 / 模式标签这类胶囊）
static void mkChip(lv_obj_t* o, uint32_t bg, uint32_t fg, uint8_t radius) {
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void mkLabel(lv_obj_t* l, const lv_font_t* f, uint32_t color) {
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
}

// ===========================
// 底部按键提示：统一两行 + 硬性宽度上限
// ===========================
// 为什么必须改：提示行是用 16px 中文字体渲染的，屏只有 240px 宽。
// 原来那些提示有多宽是量出来的（见 text_width.py，adv_w 是 1/16px 单位）：
//   "←→ 切换 · ↑↓ 调整 · 回车保存 · ESC 取消"   368px ← 设置页，超出 128px
//   "←→ 切换 · ↑↓ 调整 · 回车开始/停止 · ESC 取消" 408px ← 倒计时页，超出 168px
//   "↑↓ 翻看 · DEL 清空 · ESC 返回"              264px ← 日志页，超出 24px
//   "上下选择 · 回车确认 · ESC 返回"              256px ← 菜单页，超出 16px
// 居中对齐之后左右各裁掉一半，用户看到的就是"底部的字跑出屏幕"。
//
// 字库是 Windows simhei.ttf 生成的 16px 固定字号，重生成小一号中文字库需要
// Windows 字体源，不在开发机上，所以只能靠拆行 + 砍字。
// 两行是唯一能**一条按键说明都不丢**的办法（ESC 是设置页唯一的"不保存退出"
// 路径，压成一行就得砍掉它）。
#define HINT_W        224   // 屏宽 240，两侧各留 8px 余量
#define HINT_LINE_H   19    // = lv_font_simsun_16_cjk 的 line_height
// 两行提示占据的高度，以及它离屏幕底边留多少
#define HINT_BLOCK_H  (HINT_LINE_H * 2)
#define HINT_BOTTOM   3
#define HINT_LINE1_Y  (240 - HINT_BOTTOM - HINT_BLOCK_H)   // ≈ 199
#define HINT_LINE2_Y  (HINT_LINE1_Y + HINT_LINE_H)          // ≈ 218

// 建一行提示。width / LV_LABEL_LONG_DOT 是**兜底**：即使以后有人加了一条
// 超宽文案，超出的部分也会变成省略号，而不是画到屏幕外去。
static lv_obj_t* mkHintLine(lv_obj_t* parent, lv_coord_t y, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    mkLabel(l, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_obj_set_width(l, HINT_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// 改一行提示的文案（页面已建好、只刷文字时用）
static void setHintText(lv_obj_t* l, const char* text) {
    if (l == nullptr) return;
    setText(l, text);
    lv_obj_set_width(l, HINT_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

// ===========================
// 全屏风格容器（6 种主屏风格统一走这里）
// ===========================
// 必须显式把 padding 清成 0，否则整个界面会整体往右下偏 13px。
//
// 原因：LVGL 默认主题会给**普通 lv_obj** 套一份 card 样式，其中
// `pad_all = PAD_DEF`，在 240x240 / DPI 130 下 PAD_DEF = lv_disp_dpx(16) = 13。
// 而 lv_obj_set_pos() 定位子对象时走的是 lv_obj_move_to()，后者会把
// **父对象的 pad_left / pad_top + border 叠进子对象坐标**
// （lvgl/src/core/lv_obj_pos.c 的 lv_obj_move_to，713-727 行）。
//
// 于是"贴在 (0,0)"的东西全部右下偏 13px：
//   · dashTopBar() 的顶部条建在风格容器上 → 6 种风格的顶栏都右移+下移 13px，
//     右侧 13px 被裁掉，屏幕左边露出一条底色 —— 就是"整体好像往右移了一点"；
//   · 壁纸模式的 wp_img / scrim 建在 wp_bg 上 → 满屏图右移下移 13px，
//     左边一条黑边、右边和底部各缺 13px，所以"展示壁纸的时候特别明显"。
// 用 lv_obj_align() 的子对象不受影响（对齐算的是 content 区，pad 对称时中心不变），
// 所以只有"贴边"的元素露馅 —— 这跟旋转没有关系。
static lv_obj_t* makeRootPanel(lv_obj_t* parent, uint32_t bg) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, 240, 240);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// **只在内容真的变了时才 set_text**
// updateDynamicElements() 是每轮 loop 都跑的，LVGL 的 lv_label_set_text 无条件
// invalidate + 重排版，文本没变也照刷 —— 结果屏幕永远 dirty，SPI 一直在推满屏。
// 时钟/温湿度/计数这些一秒才变一次的值，套上这个函数后静态画面能完全停止刷新。
static void setText(lv_obj_t* lbl, const char* txt) {
    if (lbl == nullptr) return;
    const char* cur = lv_label_get_text(lbl);
    if (cur != nullptr && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

// ===========================
// 变化检测写入（关键性能修复）
// ===========================
// LVGL 8.4 **没有**"值没变就跳过"的短路。lv_obj_set_local_style_prop() 直接
// 调 lv_obj_refresh_style()，而后者第一件事就是无条件 lv_obj_invalidate(obj)
// （lvgl/src/core/lv_obj_style.c:167-173）。lv_obj_set_size / lv_obj_set_pos 同理，
// 除了 invalidate 还会 lv_obj_mark_layout_as_dirty()。
//
// 原来的 updateDynamicElements() 是每轮 loop() 都跑的，里面却无条件地
// 每轮重设：顶部条 3 个圆点的底色、信息面板再加 9 个（三层锁灯各 3 个）、
// 节奏页再加 72 个（24 根柱子的尺寸+位置+底色）。
//
// 后果就是屏幕**永远处于 dirty 状态**：无效区列表被撑满（超过 LV_INV_BUF_SIZE
// 还会退化成整屏重绘），每 33ms 一次全屏重绘 + 6 次 SPI 满屏 flush。SPI 是
// 阻塞式的，loop() 被 LVGL 吃满 → 排在它前面的键盘扫描抢不到时间。
// 表现就是「切到要素最多的信息面板就整机卡死、按键全部失灵」。
//
// 所以下面这几个 setter 都先读回当前值比对，不同才写。读回走
// lv_obj_get_style_*（只在对象自己的样式数组里找，最多查 1~2 项），
// 比一次 invalidate + 满屏重绘便宜几个数量级。
static void setBgColor(lv_obj_t* o, uint32_t hex) {
    if (o == nullptr) return;
    lv_color_t want = lv_color_hex(hex);
    if (lv_obj_get_style_bg_color(o, LV_PART_MAIN).full == want.full) return;
    lv_obj_set_style_bg_color(o, want, LV_PART_MAIN);
}

static void setTextColor(lv_obj_t* o, uint32_t hex) {
    if (o == nullptr) return;
    lv_color_t want = lv_color_hex(hex);
    if (lv_obj_get_style_text_color(o, LV_PART_MAIN).full == want.full) return;
    lv_obj_set_style_text_color(o, want, LV_PART_MAIN);
}

// 尺寸+位置一起比。只在真的变了才写，避免节奏页每轮 24 根柱子 × 2 次几何写。
static void setSizePos(lv_obj_t* o, lv_coord_t w, lv_coord_t h, lv_coord_t x, lv_coord_t y) {
    if (o == nullptr) return;
    if (lv_obj_get_width(o) == w && lv_obj_get_height(o) == h &&
        lv_obj_get_x(o) == x && lv_obj_get_y(o) == y) return;
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
}

// 显示/隐藏。同样只在状态真的变了才动：lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)
// 会**无条件** lv_obj_invalidate()，每 100ms 无脑调一次就等于屏幕永远 dirty
// （和上面 setBgColor / setText 是同一个道理）。
static void setHidden(lv_obj_t* o, bool hidden) {
    if (o == nullptr) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// 给顶部条里的小图形用的：一块纯色小方块/圆片。
// 顶部条的这些图标是"贴边画的"，所以别用 mkCard/mkChip（那两个带 1px 描边），
// 统一走这里，省得每个都要写 6 行 set_style。
static lv_obj_t* iconRect(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                          lv_align_t align, lv_coord_t dx, lv_coord_t dy,
                          uint32_t color, lv_coord_t radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, align, dx, dy);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// 中文日期："9月27日 星期六"。
// 以前是 "%04d/%02d/%02d %s" + Sun/Mon/… 满屏中文里夹一行英文缩写很跳，
// 中文星期也比 Sat 短，英文占的宽度纯属浪费。
// ⚠ 这个串只能用中文字库渲染（lv_font_simsun_16_cjk）——Montserrat 里没有汉字，
// 也没有到 CJK 的回退，挂在 montserrat_* 的 label 上会直接画成空白。
//
// 参数收成三个 int 而不是 `const struct tm*`：Arduino 会给 .ino 里每个函数
// 自动生成一份原型插在"第一个函数定义"之前，签名里带自定义/结构体类型
// 容易在那一步出问题。传三个 int 谁都不会有意见。
static void formatDateCN(char* out, size_t n, int mon, int mday, int wday) {
    static const char* weekCN[7] = { "日", "一", "二", "三", "四", "五", "六" };
    if (wday < 0 || wday > 6) wday = 0;
    snprintf(out, n, "%d月%d日 星期%s", mon, mday, weekCN[wday]);
}

// ===========================
// 主屏通用：顶部状态条
// ===========================
// 左：3 颗锁状态灯（NUM / CAP / SCR）。**只画圆点，不写文字** ——
//     原来"圆点 + NUM/CAP/SCR 小字"占掉左边 ~165px，而右边方案名
//     （"方案1-Windows" 约 108px，右对齐到 -12）从 x≈120 就开始，两者直接叠在一起，
//     这就是"上面挤了一点"。去掉文字后左边只占 ~54px，锁状态靠"第几颗圆点 + 颜色"
//     区分（NUM=绿 / CAP=青 / SCR=琥珀，和 C3 上 16/17/18 号物理指示灯同色）。
// 右：当前方案。方案 1/2 是 Windows / macOS，直接画系统图标；方案 3/4 不按系统分，
//     只显序号。图标全部用基本图形拼（圆角矩形 + 圆），不占 Flash，
//     也不用指望字库 / 字体符号里正好有苹果和四格窗这两个字形
//     —— LVGL 内置符号表和这份中文字库里都没有它们，靠字符是画不出来的。
static void dashTopBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 240, 28);
    lv_obj_set_pos(bar, 0, 0);
    // 顶栏底色改成纯黑，与极客仪表盘/高对比度风格的纯黑页底融合；
    // 下面那条 1px 分隔线也压到 0x1A1A1A，免得在黑底上看起来像灰条。
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 三颗锁：外圈（亮起时才显形）+ 圆点。
    // 圆点 10→12、外环 14→18（边框加粗到 2px）、中心间距 22→28。
    // 顶栏 28px 高度放得下 18px 的环；三颗右沿到 ~81px，右半边
    // 方案序号 + 系统图标（>=200px 才开始）完全不受影响。
    // OFF 态的圆点从 CLR_STROKE（深灰 0x2B3549）改成 CLR_TEXT_MUTE
    // （浅灰 0x5B6579）—— 黑底上深灰圆点几乎看不见，灭/亮分辨不出。
    const uint32_t onColors[3] = { lockLedColor[0], lockLedColor[1], lockLedColor[2] };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* ring = lv_obj_create(bar);
        lv_obj_set_size(ring, 18, 18);
        lv_obj_align(ring, LV_ALIGN_LEFT_MID, 16 + i * 28, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, lv_color_hex(onColors[i]), LV_PART_MAIN);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        topLockRing[i] = ring;

        lv_obj_t* d = iconRect(bar, 12, 12, LV_ALIGN_LEFT_MID, 19 + i * 28, 0,
                               CLR_TEXT_MUTE, LV_RADIUS_CIRCLE);
        topLockDot[i] = d;
        topLockOn[i] = onColors[i];
    }

    // 方案序号（1~4），贴着右边
    topProfileNum = lv_label_create(bar);
    mkLabel(topProfileNum, &lv_font_montserrat_14, CLR_ACCENT);
    lv_label_set_text(topProfileNum, "1");
    lv_obj_align(topProfileNum, LV_ALIGN_RIGHT_MID, -10, 0);

    // 系统图标槽位：直接挂一个 lv_img（S 档 16×16），src 由 updateTopBarProfile()
    // 按 currentProfile 实时换。原来那套"画 8 块矩形模拟 Win/Mac 标志"的活人肉画法
    // 全部废弃——统一用 profile_icon_get(profile, PROF_ICON_S) 出图。
    topIconBox = lv_img_create(bar);
    lv_img_set_src(topIconBox, profile_icon_get(currentProfile, PROF_ICON_S));
    lv_obj_align(topIconBox, LV_ALIGN_RIGHT_MID, -32, 0);
}

// 顶部条右侧的方式指示（序号 + 系统图标）单独抽出来，两个地方都要用：
//   · switchProfile() —— 换方案的瞬间立刻更新，不用等下面那 100ms 的慢刷新
//   · updateDynamicElements() —— 万一顶部条是在别的方案下建出来的，这里兜住
// 尤其要注意 updateDynamicElements() 开头有"系统时间还没校准就 return"的判断，
// 只靠它会出现"换了方案图标不变"。
static void updateTopBarProfile(void) {
    static char profBuf[4];
    snprintf(profBuf, sizeof(profBuf), "%u", (unsigned)(currentProfile + 1));
    setText(topProfileNum, profBuf);
    if (topIconBox) {
        lv_img_set_src(topIconBox, profile_icon_get(currentProfile, PROF_ICON_S));
    }
}

// 极客页底部那两颗温湿度卡：贴底、更宽（104 而不是 72）、更扁（48）。
// 数值还是 20px，"23.5C" 一行放得下不折行。
static lv_obj_t* statCardBottom(lv_obj_t* parent, int cx, const char* cap,
                                uint32_t vcolor) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 104, 48);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, cx, -6);
    mkCard(c, CLR_SURFACE, 10);

    lv_obj_t* t = lv_label_create(c);
    mkLabel(t, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(t, cap);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* v = lv_label_create(c);
    mkLabel(v, &lv_font_montserrat_20, vcolor);
    lv_label_set_text(v, "--");
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 4);
    return v;
}

// 小统计卡：标题 + 数值
// ===========================
// 构建：极客仪表盘
// ===========================
static void build_style_geek(void) {
    if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }

    // 极客仪表盘：纯黑底（与高对比度风格统一）。深蓝灰(CLR_BG)在小屏上
    // 对比发灰，数字/温湿度这种低饱和内容铺上去反而显得闷。
    gk_bg = makeRootPanel(ensureMainScreen(), 0x000000);

    dashTopBar(gk_bg);

    // 版面（240x240，各行给的是 label 的**行盒**范围，实际字形比行盒窄）：
    //   0..28    顶部条
    //   30..82   时钟     montserrat_48，行高 52
    //   86..105  中文日期 simsun_16，行高 19
    //   112..180 按键反馈卡（200x68）
    //              ├ 118..137 "最近按键"  simsun_16
    //              ├ 右上角累计次数     montserrat_10
    //              └ 144..174 键名      montserrat_28，行高 30
    //   186..234 温湿度两张卡（104x48，贴底）
    //
    // 中间这块原来是"三张统计卡摆一排 + 一条强调线"，现在换成一张反馈卡：
    // 温湿度下沉到贴底、让出中间，中间正好够放按键反馈。
    // 强调线去掉了 —— 上下都有卡片，那条 2px 的线已经不起分隔作用，白占 6px。
    gk_lbl_clock = lv_label_create(gk_bg);
    mkLabel(gk_lbl_clock, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(gk_lbl_clock, "--:--");
    lv_obj_align(gk_lbl_clock, LV_ALIGN_TOP_MID, 0, 30);

    // 日期。**必须用中文字库**：formatDateCN() 出的是"9月27日 星期六"，
    // montserrat_* 里没有汉字，也没有到 CJK 的回退，挂错了就是一整行空白。
    gk_lbl_date = lv_label_create(gk_bg);
    mkLabel(gk_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(gk_lbl_date, "1月1日 星期一");
    lv_obj_align(gk_lbl_date, LV_ALIGN_TOP_MID, 0, 86);

    // ---- 按键反馈卡 ----
    lv_obj_t* keycard = lv_obj_create(gk_bg);
    lv_obj_set_size(keycard, 200, 68);
    lv_obj_align(keycard, LV_ALIGN_TOP_MID, 0, 112);
    mkCard(keycard, CLR_SURFACE, 12);

    lv_obj_t* keycap = lv_label_create(keycard);
    mkLabel(keycap, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(keycap, "最近按键");
    lv_obj_align(keycap, LV_ALIGN_TOP_MID, 0, 6);

    // 累计击键次数缩在右上角：它是个慢变量，不该占一整行去和"刚刚按了什么"抢视线
    gk_lbl_keys = lv_label_create(keycard);
    mkLabel(gk_lbl_keys, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(gk_lbl_keys, "0");
    lv_obj_align(gk_lbl_keys, LV_ALIGN_TOP_RIGHT, -10, 10);

    // 键名用 getKeyName()：Space / Enter / Num 7 / M1 …
    // 全 ASCII，所以挂 montserrat 没问题（中文只出现在"最近按键"这个 simsun 的标题上）。
    gk_lbl_lastkey = lv_label_create(keycard);
    mkLabel(gk_lbl_lastkey, &lv_font_montserrat_28, CLR_ACCENT);
    lv_label_set_text(gk_lbl_lastkey, "-");
    lv_obj_set_width(gk_lbl_lastkey, 180);
    lv_label_set_long_mode(gk_lbl_lastkey, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(gk_lbl_lastkey, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(gk_lbl_lastkey, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 温湿度贴底
    gk_lbl_temp = statCardBottom(gk_bg, -58, "TEMP", CLR_AMBER);
    gk_lbl_hum  = statCardBottom(gk_bg,  58, "HUMI", CLR_ACCENT);
}

// ===========================
// 构建：大时钟
// ===========================
static void build_style_bigclock(void) {
    if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }

    bc_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    dashTopBar(bc_bg);

    bc_lbl_time = lv_label_create(bc_bg);
    mkLabel(bc_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(bc_lbl_time, "--:--");
    lv_obj_align(bc_lbl_time, LV_ALIGN_CENTER, 0, -18);

    // 强调线
    lv_obj_t* line = lv_obj_create(bc_bg);
    lv_obj_set_size(line, 56, 3);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    bc_lbl_date = lv_label_create(bc_bg);
    mkLabel(bc_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(bc_lbl_date, "1月1日 星期一");
    lv_obj_align(bc_lbl_date, LV_ALIGN_CENTER, 0, 44);

    // 底部方案胶囊
    lv_obj_t* chip = lv_obj_create(bc_bg);
    lv_obj_set_size(chip, 116, 24);
    lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -22);
    mkChip(chip, CLR_SURFACE, CLR_ACCENT, 12);
    lv_obj_t* cl = lv_label_create(chip);
    mkLabel(cl, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(cl, profileNamesCN[currentProfile]);
    lv_obj_center(cl);
}

// ===========================
// 构建：信息面板
// ===========================
static void build_style_info_panel(void) {
    if (ip_bg) { lv_obj_del(ip_bg); ip_bg = nullptr; }

    ip_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    dashTopBar(ip_bg);

    // 时钟 + 日期主卡
    lv_obj_t* card = lv_obj_create(ip_bg);
    lv_obj_set_size(card, 212, 84);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);
    mkCard(card, CLR_SURFACE, 12);

    ip_lbl_clock = lv_label_create(card);
    mkLabel(ip_lbl_clock, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(ip_lbl_clock, "--:--");
    lv_obj_align(ip_lbl_clock, LV_ALIGN_CENTER, 0, -10);

    ip_lbl_date = lv_label_create(card);
    mkLabel(ip_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(ip_lbl_date, "1月1日 星期一");
    lv_obj_align(ip_lbl_date, LV_ALIGN_CENTER, 0, 26);

    // 三个锁状态胶囊
    // 这三颗对应键盘上 C3 的 NUM / CAPS / SCR 指示灯，用户明确要求"更明显"：
    // 圆点从 8px 放大到 14px、字号 12→14、亮时整颗胶囊底色也跟着抬起来，
    // 灭时圆点压到描边色、文字退到三级灰 —— 亮/灭在余光里也能分辨。
    const char* lockNames[3] = { "NUM", "CAPS", "SCR" };
    const int lockX[3] = { -70, 0, 70 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = lv_obj_create(ip_bg);
        lv_obj_set_size(c, 66, 36);
        lv_obj_align(c, LV_ALIGN_TOP_MID, lockX[i], 126);
        mkChip(c, CLR_SURFACE, CLR_TEXT_MUTE, 10);
        ipLockChip[i] = c;

        lv_obj_t* d = lv_obj_create(c);
        lv_obj_set_size(d, 14, 14);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 9, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        ipLockDot[i] = d;

        lv_obj_t* t = lv_label_create(c);
        mkLabel(t, &lv_font_montserrat_14, CLR_TEXT_MUTE);
        lv_label_set_text(t, lockNames[i]);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, -8, 0);
        ipLockLbl[i] = t;
    }
    ip_circle_num = ipLockDot[0]; ip_circle_caps = ipLockDot[1]; ip_circle_scr = ipLockDot[2];

    // 底部：最近按键 + 击键数
    lv_obj_t* card2 = lv_obj_create(ip_bg);
    lv_obj_set_size(card2, 212, 54);
    lv_obj_align(card2, LV_ALIGN_BOTTOM_MID, 0, -8);
    mkCard(card2, CLR_SURFACE, 12);

    lv_obj_t* cap = lv_label_create(card2);
    mkLabel(cap, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(cap, "LAST KEY");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 14, 8);

    ip_lbl_lastkey = lv_label_create(card2);
    mkLabel(ip_lbl_lastkey, &lv_font_montserrat_20, CLR_ACCENT);
    lv_label_set_text(ip_lbl_lastkey, "-");
    lv_obj_align(ip_lbl_lastkey, LV_ALIGN_BOTTOM_LEFT, 14, -6);

    lv_obj_t* cap2 = lv_label_create(card2);
    mkLabel(cap2, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(cap2, "KEYS");
    lv_obj_align(cap2, LV_ALIGN_TOP_RIGHT, -14, 8);

    ip_lbl_keys = lv_label_create(card2);
    mkLabel(ip_lbl_keys, &lv_font_montserrat_20, CLR_VIOLET);
    lv_label_set_text(ip_lbl_keys, "0");
    lv_obj_align(ip_lbl_keys, LV_ALIGN_BOTTOM_RIGHT, -14, -6);
}

// ===========================
// 构建：击键监控
// ===========================
static void build_style_keymon(void) {
    if (km_bg) { lv_obj_del(km_bg); km_bg = nullptr; }

    km_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    dashTopBar(km_bg);

    km_lbl_title = lv_label_create(km_bg);
    mkLabel(km_lbl_title, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(km_lbl_title, "最近按键");
    lv_obj_align(km_lbl_title, LV_ALIGN_TOP_MID, 0, 44);

    // 最近按键：走 getKeyName()，显示 Space / Enter / A 而不是 20 / 1D / 04
    km_lbl_lastkey = lv_label_create(km_bg);
    mkLabel(km_lbl_lastkey, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(km_lbl_lastkey, "-");
    lv_obj_set_width(km_lbl_lastkey, 220);
    lv_label_set_long_mode(km_lbl_lastkey, LV_LABEL_LONG_DOT);
    lv_obj_align(km_lbl_lastkey, LV_ALIGN_TOP_MID, 0, 66);

    // 击键统计卡
    lv_obj_t* card = lv_obj_create(km_bg);
    lv_obj_set_size(card, 212, 58);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -34);
    mkCard(card, CLR_SURFACE, 12);

    km_lbl_keys = lv_label_create(card);
    mkLabel(km_lbl_keys, &lv_font_montserrat_28, CLR_VIOLET);
    lv_label_set_text(km_lbl_keys, "0");
    lv_obj_align(km_lbl_keys, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* unit = lv_label_create(card);
    mkLabel(unit, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(unit, "TOTAL KEYS");
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -16, 0);

    km_lbl_profile = lv_label_create(km_bg);
    mkLabel(km_lbl_profile, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(km_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_align(km_lbl_profile, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ===========================
// 构建：律动
// ===========================
static void build_style_rhythm(void) {
    if (rh_bg) { lv_obj_del(rh_bg); rh_bg = nullptr; }

    rh_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    dashTopBar(rh_bg);

    // 频谱容器，柱子从底往上长
    lv_obj_t* stage = lv_obj_create(rh_bg);
    lv_obj_set_size(stage, 236, 110);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(stage, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 24; i++) {
        rh_bars[i] = lv_obj_create(stage);
        lv_obj_set_size(rh_bars[i], 6, 4);
        lv_obj_set_pos(rh_bars[i], 2 + i * 10, 106);
        lv_obj_set_style_bg_color(rh_bars[i], lv_color_hex(CLR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rh_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(rh_bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(rh_bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(rh_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        rhythmBars[i] = 0;
    }

    // 基线
    lv_obj_t* base = lv_obj_create(rh_bg);
    lv_obj_set_size(base, 224, 1);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(base, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cap = lv_label_create(rh_bg);
    mkLabel(cap, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(cap, "击键律动");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 156);

    // 击键数卡
    lv_obj_t* card = lv_obj_create(rh_bg);
    lv_obj_set_size(card, 212, 44);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);
    mkCard(card, CLR_SURFACE, 12);

    rh_lbl_keys = lv_label_create(card);
    mkLabel(rh_lbl_keys, &lv_font_montserrat_20, CLR_VIOLET);
    lv_label_set_text(rh_lbl_keys, "0");
    lv_obj_align(rh_lbl_keys, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* unit = lv_label_create(card);
    mkLabel(unit, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(unit, "TOTAL KEYS");
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -16, 0);
}

// ===========================
// 壁纸：上传接收 + JPEG 解码 + 落盘 / 回读
// ===========================
// JPEGDEC 是"边解码边往 logoOutBuf 画"的：每解出一块像素就回调一次。
// 目标固定是 240x240 的 RGB565，网页端已经按这个尺寸缩过了。
static int logoJpegDraw(JPEGDRAW* d) {
    if (logoOutBuf == nullptr) return 1;
    // pPixels 指向的是"当前这一块"自己的像素起点，不是整张图的左上角，
    // 所以每一行都得自己加偏移。宽度要用 iWidthUsed（被边界裁剪后的实际
    // 宽度），边缘那几块 iWidthUsed < iWidth，用 iWidth 会读到块外的数据。
    for (int y = 0; y < d->iHeight; y++) {
        int py = d->y + y;
        if (py < 0 || py >= WP_H) continue;
        uint16_t* row = logoOutBuf + (uint32_t)py * WP_W;
        for (int x = 0; x < d->iWidthUsed; x++) {
            int px = d->x + x;
            if (px < 0 || px >= WP_W) continue;
            row[px] = d->pPixels[y * d->iWidth + x];
        }
    }
    return 1;
}

// 接收缓冲只分配一次，之后永不释放。
//
// 之前是每轮 LOGO_JPEG_START 都 malloc、每次收尾/超时都 free。而收数据的
// handleLogoChunk() 跑在 NimBLE 的**主机任务**里，free() 跑在 loop 任务里 ——
// 两者没有任何同步。网页端 5 次重传之间只隔 300ms，BLE 任务完全可能在
// loop 已经 free 掉之后才处理到上一轮尾巴上那一包，接着往已释放的地址里 memcpy：
// 踩坏堆，表现为"一上传壁纸键盘就重启"。常驻一块 PSRAM 缓冲把这个整类问题消掉。
// 重传时不清空内容，只把 logoRxGot 归零重新覆盖。
//
// ⚠⚠ 但"常驻"必须配一个**固定容量**，不能拿第一次的 total 来定大小。
//   这里原来写的是 heap_caps_malloc(total)，于是缓冲区的容量被**第一次**上传的
//   图片大小永久钉死：先传过一张 2KB 的图，之后再传 4KB 的图时，
//   logoRxAlloc() 看到 logoRxBuf != nullptr 就直接返回那块 2KB 的内存，
//   而 logoRxTotal 已经是 4096 —— handleLogoChunk() 拿 total 当边界，
//   于是在第 2049 字节之后一路 memcpy 越界，把 PSRAM 里紧邻的分配
//   （解码输出缓冲 / LVGL 的对象池）全踩烂。
//   表现就是"第一张图好好的，换张大一点的图传到一半就重启"，
//   而且崩在别人的内存上，日志里只会看到"异常复位"，连阶段号都是旧的
//   —— 上一轮上传留下的小缓冲把人引到"是不是解码炸了"，真凶在这儿。
//   所以：按 LOGO_RX_MAX(256KB) 一次性分配，把容量和每次声明的长度彻底解耦。
//   代价是 8MB PSRAM 里常驻 256KB（3%），换来"任何合法长度的图都不会越界"。
static uint8_t* logoRxAlloc(uint32_t need) {
    if (logoRxBuf != nullptr) return logoRxBuf;   // 常驻，永不释放、永不改容量
    uint32_t want = LOGO_RX_MAX;
    if (want < need) want = need;                 // 理论上不会，兜底
    logoRxBuf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (logoRxBuf == nullptr) logoRxBuf = (uint8_t*)malloc(want);   // PSRAM 不可用时回落
    if (logoRxBuf == nullptr) { logoRxCap = 0; return nullptr; }
    logoRxCap = want;
    memset(logoRxBuf, 0, want);
    LOG_PORT.printf("[WALLPAPER] rx buffer %u bytes (cap %u)\n",
                    (unsigned)want, (unsigned)logoRxCap);
    return logoRxBuf;
}

static void abortLogoUpload(const char* reason) {
    // 故意不 free(logoRxBuf)：见 logoRxAlloc() 上面的说明，释放会和 BLE 主机任务
    // 里的 handleLogoChunk() 抢同一块内存，释放出去的地址还会被 memcpy 进去。
    logoRxRaw   = false;
    logoRxTotal = 0;
    logoRxGot   = 0;
    logoRxDone  = false;
    logoRxActive = false;
    LOG_PORT.printf("[WALLPAPER] abort: %s\n", reason);
    // 传输中断是"用户/主机那边出问题"，但对排查很有价值：能看出
    // 壁纸是不是经常传到一半就断（多半是蓝牙距离或网页被关掉了）。
    ELWARN("WALL", "壁纸传输中断 %s", reason);
    triggerHudBig("壁纸传输", reason, lv_color_hex(CLR_RED));
    // 失败也要回报，否则网页只能干等到超时。
    // ⚠ 网页在传输期间会高频轮 LOGO_STATUS，多发一条同内容的通知会被 BLE
    //   合并掉（通知不排队），所以这里只发这一条，别再顺手 LOG 一遍。
    btLinkReplyf("LOGOSTATUS", "FAIL:%.40s", reason ? reason : "?");
}

// 二进制接收：这段是 JPEG 原始字节，里面必然有 0x00，
// 只能按长度整段取，绝不能走 c_str()（会在第一个 0 处截断）。
static void handleLogoChunk(const uint8_t* data, size_t len) {
    if (logoRxRaw) {
        // RAW 模式：电脑端已经解好 RGB565，这里就是往 logoRawBuf 里搬字节。
        // 长度是**固定**的 WP_RAW_BYTES，边界只有一个，不存在 JPEG 那种
        // "声明多少就收多少"的越界可能。
        if (logoRawBuf == nullptr) return;
        logoRxLastMs = millis();
        size_t cap = (size_t)WP_RAW_BYTES;
        if (logoRxGot >= cap) return;
        size_t room = cap - logoRxGot;
        if (len > room) len = room;
        if (len == 0) return;
        memcpy((uint8_t*)logoRawBuf + logoRxGot, data, len);
        logoRxGot += (uint32_t)len;
        // 每 64 片报一次**内部堆**的剩余量。
        // 为什么盯这个：BLE 库每收一个写事件都要 new 一份 std::string，
        // 万一那里漏了，表现就是"传到第 N 片突然 panic"——
        // 内部堆被吃光和栈溢出在症状上很像，但修法完全不同。
        // 崩在第 300 片左右（2026-10-23 实测，两次都在那儿），
        // 这个数能直接证伪/证实"堆被吃光"这条。
        if ((logoRxGot % (64 * 180)) < len) {
            LOG_PORT.printf("[WALLPAPER] 收到 %u 字节，内部堆剩 %u 字节\n",
                            (unsigned)logoRxGot, (unsigned)ESP.getFreeHeap());
        }
        if (logoRxGot >= cap) logoRxDone = true;
        return;
    }

    if (logoRxBuf == nullptr) return;
    logoRxLastMs = millis();

    // 两道边界都要卡：logoRxTotal 是网页声明的长度，logoRxCap 才是这块内存
    // 真实的容量。正常情况下 total <= cap（LOGO_JPEG_START 已经验过），
    // 但这函数跑在蓝牙任务里、拿不到 START 的上下文，边界必须自己再兜一次 ——
    // 越界写进 PSRAM 是静默的内存破坏，不会当场报错，只会在几十毫秒后
    // 以"莫名其妙重启"的形式出现，那是最难查的一类故障。
    if (logoRxGot >= logoRxCap) return;
    uint32_t room = logoRxTotal - logoRxGot;
    if (room > logoRxCap - logoRxGot) room = logoRxCap - logoRxGot;
    if (len > room) len = room;      // 多出来的丢掉，只认网页声明过的长度
    if (len == 0) return;

    memcpy(logoRxBuf + logoRxGot, data, len);
    logoRxGot += len;
    if (logoRxGot >= logoRxTotal) logoRxDone = true;
}

// 把 wpPixels 这张缓冲注册成 LVGL 图片源。解码完、开机从盘读回后都要走这里，
// 两处共用一份描述符，免得字段填漏了画成一片黑。
static void bindWallpaperSource(void) {
    if (wpPixels == nullptr) return;
    wpImgDsc.header.always_zero = 0;
    wpImgDsc.header.w          = WP_W;
    wpImgDsc.header.h          = WP_H;
    wpImgDsc.data_size         = WP_PIXELS * 2;
    wpImgDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    wpImgDsc.data              = (const uint8_t*)wpPixels;
    wpReady = true;
}

// ===========================
// SPIFFS 上的壁纸存储：分片
// ===========================
//
// ⚠⚠⚠ **为什么必须分片** ⚠⚠⚠
//
// SPIFFS 的硬限制是「**一个文件不能跨越一个 block**」。
// Arduino 用的是 ESP-IDF 的 VFS SPIFFS，配置是
//     CONFIG_SPIFFS_PAGE_SIZE = 256
//     log_block_size = 4096（默认）
// ⇒ **单个文件最大 4096 字节**。
//
// 而一张 240x240 的壁纸是 240*240*2 = **115200 字节** —— 差了 28 倍。
// 直接往 /logo.bin 里写 115200 字节是**注定写不进去**的：
// spiffs_write 会返回 SPIFFS_ERR_FULL，写到 4096 就卡住。
// 这就是「壁纸重启就丢」的真正原因（跟分区表选 FFat 还是 SPIFFS 无关 ——
// 就算 SPIFFS 挂上了，单文件也装不下）。
//
// 而且现象比"存不进去"更糟：写一半失败之后，**每次开机都会再写一次**
// （elog 落盘、壁纸回写……），于是变成 boot loop —— 崩溃 → 重启 → 又崩。
//
// 所以改成**分片存储**：
//   /logo000.bin, /logo001.bin, … 每片 ≤ 2048 字节（比 4096 上限还保守一半）
//   /logo.ok 里写着片数 —— **它存在才说明整张图写完了**
//   （写一半崩掉时没有 .ok，重启后不会把半张图当成有效的）
//
// 代价：开机读回要开 57 次文件（几百毫秒，换来"开机时画对图"）。
#define LOGO_CHUNK_SIZE   2048
#define LOGO_CHUNK_MAX    256
#define LOGO_OK_FILE      "/logo.ok"

// 写整张图。返回 false = 没写成功（调用方必须如实报错，不能假装成功）。
static bool writeWallpaperChunked(const uint8_t* data, size_t total) {
    size_t nChunks = (total + LOGO_CHUNK_SIZE - 1) / LOGO_CHUNK_SIZE;
    if (nChunks == 0 || nChunks > LOGO_CHUNK_MAX) return false;

    // 先把"有效"标记删掉：写一半崩了的话，重启后不会把半截图当成完整图。
    if (SPIFFS.exists(LOGO_OK_FILE)) SPIFFS.remove(LOGO_OK_FILE);

    for (size_t i = 0; i < nChunks; i++) {
        char path[24];
        snprintf(path, sizeof(path), "/logo%03u.bin", (unsigned)i);
        size_t off  = i * LOGO_CHUNK_SIZE;
        size_t want = (total - off < LOGO_CHUNK_SIZE) ? (total - off) : LOGO_CHUNK_SIZE;

        File f = SPIFFS.open(path, FILE_WRITE);
        if (!f) {
            LOG_PORT.printf("[WALLPAPER] open %s FAILED\n", path);
            return false;
        }
        size_t w = f.write(data + off, want);
        f.close();
        // ⚠ 每一片都报一行：崩在哪一片，全靠这行日志。
        //   不报的话只知道"写盘时崩了"，不知道是第几片、写了多少。
        LOG_PORT.printf("[WALLPAPER] %s 写 %u/%u\n", path, (unsigned)w, (unsigned)want);
        if (w != want) {
            // ⚠ 写不进去 —— 如实报告实际写进去多少，别装作写成功了。
            LOG_PORT.printf("[WALLPAPER] write %s: %u/%u 字节（写到这写不下了）\n",
                            path, (unsigned)w, (unsigned)want);
            SPIFFS.remove(path);
            return false;
        }
        ct_alive();
    }

    // 全写完了才落"有效"标记
    File ok = SPIFFS.open(LOGO_OK_FILE, FILE_WRITE);
    if (!ok) { LOG_PORT.println("[WALLPAPER] open .ok FAILED"); return false; }
    char num[16];
    int n = snprintf(num, sizeof(num), "%u\n", (unsigned)nChunks);
    ok.write((const uint8_t*)num, n);
    ok.close();
    LOG_PORT.printf("[WALLPAPER] stored: %u 片 × %u 字节\n",
                    (unsigned)nChunks, (unsigned)LOGO_CHUNK_SIZE);
    return true;
}

// 读回整张图。没有 .ok 标记就当没有壁纸（宁可空着，不要半张图）。
static bool readWallpaperChunked(uint8_t* data, size_t total) {
    if (!SPIFFS.exists(LOGO_OK_FILE)) return false;

    File ok = SPIFFS.open(LOGO_OK_FILE, FILE_READ);
    if (!ok) return false;
    char num[16];
    size_t n = ok.readBytesUntil('\n', num, sizeof(num) - 1);
    ok.close();
    if (n == 0) return false;
    num[n] = '\0';
    unsigned nChunks = (unsigned)atoi(num);
    if (nChunks == 0 || nChunks > LOGO_CHUNK_MAX) return false;
    if ((size_t)nChunks * LOGO_CHUNK_SIZE < total) return false;   // 片数不够

    for (unsigned i = 0; i < nChunks; i++) {
        char path[24];
        snprintf(path, sizeof(path), "/logo%03u.bin", i);
        File f = SPIFFS.open(path, FILE_READ);
        if (!f) return false;
        size_t off  = (size_t)i * LOGO_CHUNK_SIZE;
        size_t want = (total - off < LOGO_CHUNK_SIZE) ? (total - off) : LOGO_CHUNK_SIZE;
        size_t rd = f.read(data + off, want);
        f.close();
        if (rd != want) return false;
        ct_alive();
    }
    return true;
}

// 开机自检：探一下 SPIFFS 的单文件上限到底是多少。
//
// 为什么要探：这个上限**不在代码里**，是 ESP-IDF 的编译期配置 +
// 文件系统格式决定的，写代码时很容易想当然地以为"115KB 也能写"。
// 探一次（写 8KB 读回来）就一清二楚，比猜强。
// 代价：开机多几十毫秒 + 一次 8KB 的 flash 擦写（flash 能擦十万次，可忽略）。
static void probeSpiffsLimit(void) {
    // ⚠ 只写 2048 字节 —— 和壁纸分片一样大，**确定在上限之内**。
    //   这个探测的价值是"确认 2048 字节能写进去"（分片方案的前提）。
    //   刻意不去写更大的量：万一"写超限"本身就会触发 SPIFFS 内部 assert，
    //   那这个探测就会把设备推进 boot loop —— 为了一个诊断把设备弄坏，
    //   不值。真正的上限由 writeWallpaperChunked 的逐片写入量报出来。
    static const size_t PROBE = 2048;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(PROBE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) buf = (uint8_t*)malloc(PROBE);
    if (buf == nullptr) { LOG_PORT.println("[SPIFFS] probe: 内存不足"); return; }
    memset(buf, 0xA5, PROBE);

    File f = SPIFFS.open("/probe.bin", FILE_WRITE);
    if (!f) { LOG_PORT.println("[SPIFFS] probe: open FAILED"); free(buf); return; }
    size_t w = f.write(buf, PROBE);
    f.close();

    size_t back = 0;
    File r = SPIFFS.open("/probe.bin", FILE_READ);
    if (r) { back = r.read(buf, PROBE); r.close(); }
    SPIFFS.remove("/probe.bin");
    free(buf);

    LOG_PORT.printf("[SPIFFS] 写 %u 字节 → 实际写入 %u，读回 %u\n",
                    (unsigned)PROBE, (unsigned)w, (unsigned)back);
}

// RAW 接收缓冲：固定 115200 字节（240*240*2），只分配一次，之后永不释放。
//
// 和 logoRxAlloc 的区别很关键：那块缓冲是按 **LOGO_RX_MAX(256KB)** 一次
// 分配、容量与"这次声明多长"彻底解耦 —— 因为 JPEG 的长度是主机临时说的，
// 万一先传过一张 2KB 的图就把容量钉死成 2KB，后来的大图会越界。
// RAW 没有这个问题：长度**恒定**是 WP_RAW_BYTES，不存在"这次多长"。
static uint16_t* logoRawAlloc(void) {
    if (logoRawBuf != nullptr) return logoRawBuf;   // 常驻
    logoRawBuf = (uint16_t*)heap_caps_malloc((size_t)WP_RAW_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (logoRawBuf == nullptr) logoRawBuf = (uint16_t*)malloc((size_t)WP_RAW_BYTES);
    if (logoRawBuf == nullptr) return nullptr;
    LOG_PORT.printf("[WALLPAPER] raw buffer %u bytes\n", (unsigned)WP_RAW_BYTES);
    return logoRawBuf;
}

// RAW 收齐 -> 拷进 wpPixels -> 落盘 -> 挂到 LVGL 图片源。
//
// 整条链路里**没有解码**：电脑端发来的就是成品像素，板子只搬字节。
// 落盘格式和 JPEG 那条路完全一样（/logo.bin = 240*240*2 的 RGB565），
// 所以开机读回那套逻辑（loadWallpaperFromDisk）一行都不用改。
static void finishLogoRaw(void) {
    ct_mark(CT_S_WP_DECODE);
    logoRxDone   = false;
    logoRxActive = false;
    if (!logoRxRaw) return;
    logoRxRaw = false;

    if (logoRxGot < (uint32_t)WP_RAW_BYTES) {
        abortLogoUpload("RAW 长度不匹配");
        return;
    }

    // 拷进一块**新的** wpPixels。不能在传输过程中就地改 wpPixels ——
    // 屏可能正在用它画壁纸，半张新半张旧的花屏比"没更新"更难看。
    uint16_t* out = (uint16_t*)heap_caps_malloc((size_t)WP_RAW_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == nullptr) { abortLogoUpload("out of PSRAM"); return; }

    // 分块拷贝 + 喂狗：115KB 一次性 memcpy 也就几毫秒，但分段更稳，
    // 也让"崩在拷贝中间"这种现场能落在确定的阶段号上。
    {
        const uint8_t* p = (const uint8_t*)logoRawBuf;
        uint8_t*       q = (uint8_t*)out;
        size_t left = (size_t)WP_RAW_BYTES;
        while (left) {
            size_t n = left > 16384 ? 16384 : left;
            memcpy(q, p, n);
            p += n; q += n; left -= n;
            ct_alive();
        }
    }
    LOG_PORT.printf("[WALLPAPER] raw %u bytes received\n", (unsigned)logoRxGot);

    logoRxTotal = 0;
    logoRxGot   = 0;

    // ⚠⚠⚠ 顺序：**先接图、先显示**，再落盘。
    //
    // 为什么反过来了：落盘要碰 SPIFFS，是整条链路上**唯一可能让固件崩掉**
    // 的地方（历史上就是崩在这儿）。图此刻已经完整地躺在 PSRAM 里了，
    // 先把它挂上去，用户就一定看得见 —— 哪怕下一秒因为落盘失败而重启。
    //
    // 反过来写的话，"落盘失败"就等于"这次上传白传了"，用户屏幕上什么都没有，
    // 白折腾一场。**看得见**比**存得下**重要得多。
    //
    // 顺序上唯一的硬约束：接新图必须在释放旧图**之前**
    // （wpImgDsc.data 才有合法地址），这个没变。
    uint16_t* oldPixels = wpPixels;
    wpPixels = out;
    bindWallpaperSource();
    if (oldPixels) heap_caps_free(oldPixels);

    if (wp_img) {
        lv_img_set_src(wp_img, &wpImgDsc);
        lv_obj_invalidate(wp_img);
    }
    // 屏保那张共用同一份 wpImgDsc，但它是另一个 lv_img 对象，得单独换 src。
    if (sv_img) {
        lv_img_set_src(sv_img, &wpImgDsc);
        lv_obj_invalidate(sv_img);
    }

    // 然后才落盘：**分片存**（SPIFFS 单文件上限 4096，装不下 115200）。
    if (!writeWallpaperChunked((const uint8_t*)out, (size_t)WP_RAW_BYTES)) {
        // 落盘失败 ≠ 图坏了。图在 RAM 里好好地显示着，只是重启后没了。
        // 如实回报"半成功"，但**绝不**把已经显示出来的图撤掉。
        LOG_PORT.println("[WALLPAPER] 落盘失败 —— 已显示，重启后会丢");
        triggerHudBig("壁纸上传", "已显示·未存盘", lv_color_hex(CLR_AMBER));
        btLinkReplyC("LOGOSTATUS", "WARN:落盘失败(已显示)");
        return;
    }

    triggerHud("壁纸更新", "上传完成", lv_color_hex(CLR_GREEN));
    btLinkReplyC("LOGOSTATUS", "OK:0/0");
}

// 解码收齐的 JPEG -> wpPixels -> 落盘。解码 + 写盘要几百毫秒，
// 所以放 loop 里跑，不占着 NimBLE 的主机任务。
static void finishLogoUpload(void) {
    ct_mark(CT_S_WP_DECODE);   // 现场：这一步要几百毫秒，崩在里面最常见
    logoRxDone  = false;
    logoRxActive = false;
    if (logoRxTotal == 0 || logoRxGot < logoRxTotal) {
        // 没收齐就别去解：解一半的 JPEG 出来是花屏，比明确失败更难查
        abortLogoUpload("长度不匹配，跳过解码");
        return;
    }
    LOG_PORT.printf("[WALLPAPER] got %lu bytes, PSRAM free %lu, decoding...\n",
                    (unsigned long)logoRxGot,
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ct_alive();

    uint16_t* out = (uint16_t*)heap_caps_malloc(WP_PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) { abortLogoUpload("out of PSRAM"); return; }

    logoOutBuf = out;
    memset(out, 0, WP_PIXELS * 2);   // 先铺黑：JPEG 没盖满 240x240 时不留上一张的残影
    ct_alive();

    // 关键：JPEGDEC 这个对象有多大？
    //   JPEGDEC 里只包了一个 JPEGIMAGE，光几个大数组就有
    //     usUnalignedPixels[2056]  4112 B
    //     usHuffAC[2048]          4096 B
    //     ucHuffDC[1024*2]        2048 B
    //     ucFileBuf[2048]         2048 B
    //     sQuantTable[32]           64 B
    //   合计约 12.6 KB。
    // 原来这里写的是 `JPEGDEC jpeg;` —— 局部变量，编译器直接给它从 loop 任务的
    // 栈上开 12.6KB。Arduino 的 loopTask 栈只有 8KB，一开解码立刻踩过栈顶，
    // GTask/window overflow 触发 panic → 芯片复位。
    // 症状正是"壁纸传到 98%（数据收齐、开始解码的那一瞬）键盘就重启"：
    // 传输阶段一切正常，崩在解码第一行。
    // 改成 static：进 .bss（内部 DRAM），不占栈。之所以不放 PSRAM，是 JPEGDEC
    // 内部对 usPixels / sMCUs 有 16 字节 SIMD 对齐的假设，PSRAM 上未对齐访问
    // 既慢又不可靠；12.6KB 静态内存换稳定，值。
    static JPEGDEC jpeg;
    bool ok = false;

    // ⚠⚠ JPEG 字节必须先拷进**内部 DRAM** 再喂给 JPEGDEC。
    //
    // logoRxBuf 是常驻在 PSRAM 的（logoRxAlloc 里 heap_caps_malloc(..., MALLOC_CAP_SPIRAM)），
    // 而 jpeg.openRAM(pBuf, len) 是让 JPEGDEC **直接在 pBuf 上解析**。
    // JPEG 是变长编码，解析过程要在缓冲区里大量跳转、逐位读哈夫曼表 ——
    // 这些访问跨 PSRAM cache line 时不但慢，还可能踩到未对齐访问。
    // JPEGDEC 自己就明确假设内部缓冲是 16 字节对齐的（下面 static JPEGDEC 那段
    // 注释里的原话："JPEGDEC 内部对 usPixels / sMCUs 有 16 字节 SIMD 对齐的假设，
    // PSRAM 上未对齐访问既慢又不可靠"）。
    //
    // 之前那次修复只做了一半：JPEGDEC **对象**挪进了 .bss（内部 DRAM），
    // 但**输入数据**还在 PSRAM 上。表现就是"传输一切正常、进度条到底、
    // 紧接着就 reason=4 异常崩溃" —— 崩在解码第一步，
    // 而崩溃现场（[BOOT] prev: stage=0 循环末尾）完全看不出是内存问题。
    //
    // 一张 240x240 的 JPEG 只有十几 KB，内部 DRAM 装得下（堆还有 80MB+），
    // 拿这点内存换解码稳定，值。
    static uint8_t* jpegIn = nullptr;
    size_t inLen = logoRxGot;
    jpegIn = (uint8_t*)malloc(inLen);
    if (jpegIn == nullptr) { abortLogoUpload("内部内存不足"); return; }
    memcpy(jpegIn, logoRxBuf, inLen);

    if (jpeg.openRAM(jpegIn, (int)inLen, logoJpegDraw)) {
        LOG_PORT.printf("[WALLPAPER] jpeg header %dx%d, decoding...\n",
                        jpeg.getWidth(), jpeg.getHeight());
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        ok = (jpeg.decode(0, 0, 0) != 0);
        jpeg.close();
    }
    // 解码完立刻还回内部 DRAM —— 这块不能像 PSRAM 常驻缓冲那样留着，
    // 内部 DRAM 紧张，每张图都要新申请/释放。
    free(jpegIn);
    jpegIn = nullptr;
    logoOutBuf = nullptr;
    ct_alive();
    LOG_PORT.printf("[WALLPAPER] decode %s\n", ok ? "ok" : "FAILED");

    // ⚠ 顺序：**先接图、先显示**，再落盘（和 RAW 那条路一个道理）。
    //   落盘是唯一可能崩的步骤；图在 RAM 里是完整的，先显示出来，
    //   哪怕落盘失败、重启了，用户也看到过这张图。
    //
    //   写盘那 200 多毫秒里旧的 wpPixels 必须保持有效 —— 屏还在用它画壁纸，
    //   这里提前 free 就是 use-after-free，wpImgDsc.data 还会指向已释放的地址。
    //   所以先把旧指针扣下，等新图彻底接上再释放。
    uint16_t* oldPixels = wpPixels;

    logoRxTotal = 0;
    logoRxGot   = 0;

    if (!ok) {
        heap_caps_free(out);
        triggerHudBig("壁纸传输", "解码失败", lv_color_hex(CLR_RED));
        return;
    }

    // 顺序不能反：先接新图（wpImgDsc.data 才有合法地址），再释放旧图
    wpPixels = out;
    bindWallpaperSource();
    if (oldPixels) heap_caps_free(oldPixels);

    if (wp_img) {
        lv_img_set_src(wp_img, &wpImgDsc);
        lv_obj_invalidate(wp_img);
    }
    if (sv_img) {
        lv_img_set_src(sv_img, &wpImgDsc);
        lv_obj_invalidate(sv_img);
    }

    // 再落盘一份，重启后直接从 SPIFFS 读回。**分片存**（单文件上限 4096）。
    if (!writeWallpaperChunked((const uint8_t*)out, (size_t)(WP_PIXELS * 2))) {
        LOG_PORT.println("[WALLPAPER] 落盘失败 —— 已显示，重启后会丢");
        triggerHudBig("壁纸上传", "已显示·未存盘", lv_color_hex(CLR_AMBER));
        btLinkReplyC("LOGOSTATUS", "WARN:落盘失败(已显示)");
        return;
    }

    triggerHud("壁纸更新", "上传完成", lv_color_hex(CLR_GREEN));

    // 主动回报：网页轮询 LOGO_STATUS 时能拿到确定答案。
    // 以前网页"字节发完"就 alert 上传成功，那是假成功 —— BLE 写成功只说明
    // 数据交给蓝牙了，不代表键盘解出来、落盘了。
    btLinkReplyC("LOGOSTATUS", "OK:0/0");
}

// 开机：SPIFFS 里有壁纸就读回 PSRAM，重启不用重传。
//
// 现在是**分片**存的（单文件上限 4096，装不下 115200），见 writeWallpaperChunked。
// 没有 /logo.ok 标记就当没有壁纸 —— 宁可空着，也不要挂一张半张的图。
static void loadWallpaperFromDisk(void) {
    // 旧格式（单文件 /logo.bin）已经废弃：它根本写不进去，留着只会占地方。
    // 删不掉也没关系 —— 读回路径已经不认它了。
    if (SPIFFS.exists("/logo.bin")) SPIFFS.remove("/logo.bin");

    if (!SPIFFS.exists(LOGO_OK_FILE)) return;

    uint16_t* buf = (uint16_t*)heap_caps_malloc((size_t)WP_PIXELS * 2,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) return;

    if (!readWallpaperChunked((uint8_t*)buf, (size_t)(WP_PIXELS * 2))) {
        LOG_PORT.println("[WALLPAPER] 读回失败 —— 按没有壁纸处理");
        heap_caps_free(buf);
        return;
    }

    if (wpPixels) heap_caps_free(wpPixels);
    wpPixels = buf;
    bindWallpaperSource();
    LOG_PORT.println("[WALLPAPER] 读回成功");
}

// ===========================
// 构建：壁纸模式
// ===========================
static void build_style_wallpaper(void) {
    if (wp_bg) { lv_obj_del(wp_bg); wp_bg = nullptr; }

    wp_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    // 壁纸铺满
    wp_img = lv_img_create(wp_bg);
    lv_obj_set_size(wp_img, 240, 240);
    lv_obj_set_pos(wp_img, 0, 0);
    lv_obj_set_style_bg_color(wp_img, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_width(wp_img, 0, LV_PART_MAIN);
    // 上传过壁纸就挂上去；没上传时给一块深色底，不至于全黑
    if (wpReady) {
        lv_img_set_src(wp_img, &wpImgDsc);
    } else {
        lv_img_set_src(wp_img, NULL);
    }

    // 压一层半透明黑，保证下面的字在任意壁纸上都读得出来。
    // 从 LV_OPA_50(=127) 降到 LV_OPA_20(=51)：字已经收进底部通栏条、
    // 而且条自己带了半透明黑底，全屏遮罩再压一半，整个画面就只剩"糊"——
    // 壁纸页的重点是图，不是字。
    // ⚠ LV_OPA_* 只有 0/10/20/30/40/50/60/70/80/90/100 这几档
    //   （lv_color.h，值分别是 0/25/51/76/102/127/153/178/204/229/255），
    //   没有 LV_OPA_25 这种东西，别照着百分点去猜。
    lv_obj_t* scrim = lv_obj_create(wp_bg);
    lv_obj_set_size(scrim, 240, 240);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(scrim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    // 时钟挪到**底部通栏**。
    //
    // 为什么不是右下角一块 112x40 的小卡片（原来的做法）：
    //   那块卡片带 1px 描边 + 圆角，浮在壁纸正中间偏右的位置，
    //   而壁纸的构图是**连续的圆弧**——112x40 的矩形正好把弧线挖断，
    //   一眼看过去就是"有张便利贴贴在图上"，这就是"时钟很突兀"的来源。
    //   （v1 壁纸弧线最低到 y=199，卡片在 y=190..230，直接切断。）
    //
    // 改成通栏之后：
    //   · 没有描边、没有圆角 → 它是"界面结构"，不是"贴上去的控件"
    //   · 正好落在壁纸底部预留的空场（见 wallpaper/gen_wallpaper_240_v2.py，
    //     那边刻意把 y=168..240 清空给这块条），弧线不再被切断
    //   · 用户上传**亮色**壁纸时也不怕：条是半透明黑底，文字照样读得出
    //     （原来那种"纯裸字压图"的做法在亮图上完全看不清，已放弃）
    lv_obj_t* bar = lv_obj_create(wp_bg);
    lv_obj_set_size(bar, 240, BAR_H);
    lv_obj_set_pos(bar, 0, BAR_Y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 时间：28px 靠左。28px 的 "22:48" 实测 75.3px 宽（lvgl_font_width.py），
    // 左边留 14px → 占 x=14..89，不会撞到右边的日期。
    wp_lbl_time = lv_label_create(bar);
    mkLabel(wp_lbl_time, &lv_font_montserrat_28, CLR_TEXT);
    lv_label_set_text(wp_lbl_time, "--:--");
    lv_obj_set_pos(wp_lbl_time, 14, 8);

    // 日期：16px 中文右对齐到 226。宽 120px → 占 x=106..226。
    // 和时间同一行，中间还有 17px 余量。
    wp_lbl_date = lv_label_create(bar);
    mkLabel(wp_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(wp_lbl_date, "1月1日 星期一");
    lv_obj_set_pos(wp_lbl_date, 106, 12);
}

// ===========================
// 构建：高对比度（纯黑 + 大字 + 大红键名）
// ===========================
// 版面（240x240，全屏坐标）：
//   0..6       顶部留 6px 喘息
//   6..32      顶部 3 颗大锁灯 + 右上系统图标（不再有底色条）
//   46..98     中间大时钟（montserrat_48，行高 52）
//   98..117    中文日期（simsun_16，行高 19）
//   124..176   中间大红色按键名（montserrat_48，行高 52，**无标题**）
//   178..235   底部状态条（w=220, x=10..230）：左 2/3 温湿度（上下堆叠 + 满宽进度条）
//              │                                            + 右 1/3 字数
//              ├ 180..198 温度小标（左）/ 温度值（右，行高18）
//              ├ 199..204 温度进度条（满宽 135px）
//              ├ 209..227 湿度小标（左）/ 湿度值（右）
//              └ 228..233 湿度进度条（满宽 135px）
//
// 设计要点：
//   · 不要 6 种风格共用的 dashTopBar() —— 那个是"小灯 + 文字图标 + 文字方案序号"，
//     主题色全是低对比度的蓝灰。这版要"高对比度"，所以锁灯做成**纯黑背景上的大色块**：
//     NUM / CAPS / SCR 文字直接嵌在色块里（亮起：色块满色 + 黑字；灭态：#1F1F1F + 灰字），
//     不用再额外摆一根竖条。
//   · 按键反馈按用户要求"那几个字就不要了"：没有"最近按键"标题，直接一个
//     红字键名 Space / Enter / A 杵在中间。montserrat_* 没有 CJK，
//     所以**显示的就是 ASCII 键名**，CJK 控件绕过它了。字号改成 montserrat_28
//     是为了让底部温 / 湿卡 + 字数都有空间。
//   · 底部状态条按用户要求改成「温湿度占 2/3、字数占 1/3」，而且温湿度是
//     **上下堆叠**的两根满宽进度条（小标 + 数值一行，下面一条 135px 的条）。
//     原来是三等分 + 每格一个 16x16 图标色块：并排时每根条只有 63px，
//     23°C 和 28°C 画出来只差 6px 根本分不出来；堆叠后条长 135px，长度差
//     一倍就读得出了。图标色块去掉了 —— 它们和顶部三颗锁灯的"大色块"语言打架。
// ===========================================================
// 底部状态条的两个小工具
// ===========================================================
// 进度条填充宽度：把 0..1 的比例换算成 0..槽宽像素。
//
// 为什么要手搓而不用 lv_bar：lv_bar 是 lv_obj + 内部指示器两层的封装，
// 而这个项目的性能约定是「所有每轮都会变的几何/样式写入都要先读回比对」
// （见 setBgColor / setSizePos 上面的注释）。lv_bar_set_value() 不走那套比对，
// 挂在每 100ms 跑一次的 updateDynamicElements() 上就是一次无脑 invalidate，
// 屏幕永远 dirty、SPI 一直推满屏。手搓两层 lv_obj 才能套 setSizePos()。
//
// 进度条的两根条（槽 + 填充）是**兄弟**对象、都直接挂在 hc_bg 上，用绝对坐标摆，
// 不是父子结构。三个原因：
//   1. iconRect() 建对象走的是 lv_obj_align()，而 LVGL 8.4 里它是
//        lv_obj_set_style_align(obj, align, 0) + lv_obj_set_pos(...)
//      —— 除了定位还会给对象挂一个 **align 样式**。填充每 100ms 改一次宽度，
//      那个 align 样式会跟着参与父级重排，这条路径没法离线确证。
//   2. 父子结构下 lv_obj_set_pos() 要补偿父对象的 pad 和滚动量，
//      子对象又被裁剪在父的内容区里；兄弟关系下这些耦合全部消失。
//   3. 律动页那 24 根柱子就是 lv_obj_create + lv_obj_set_pos() 的写法，
//      在这台机器上一直正常工作 —— 直接照抄已验证的写法，不赌。
#define HC_BAR_W   135
#define HC_BAR_H   6

// 建一根条。样式逐项写全，不依赖任何继承：bg_opa / border / pad / radius
// 缺一项都会让默认主题（浅色、pad 13px）漏进来，在纯黑底上表现成一条白杠。
static lv_obj_t* mkBar(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, HC_BAR_H);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, HC_BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void setBarFill(lv_obj_t* fill, lv_coord_t x, lv_coord_t y, float pct) {
    if (fill == nullptr) return;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    lv_coord_t w = (lv_coord_t)((float)HC_BAR_W * pct + 0.5f);
    // 读数非零却一根都不显示，看上去像传感器挂了 —— 至少留 1px
    if (w == 0 && pct > 0.0f) w = 1;
    setSizePos(fill, w, HC_BAR_H, x, y);
}

// 字数紧凑化：totalKeyCount 是 uint32_t，最大 4294967295（10 位），
// montserrat_18 光这串数字就有 ~100px，字数那格只有 62px 宽，放不下会被截成
// "12345..."。所以 5 位起就折成 "12.3k" / "1.2M"——计数器的通行做法，
// 既保证任何取值都不溢出，也让这格永远是同样宽的一小段。
//
// 分支上界特意写成 999_950 而不是 1_000_000：999999 四舍五入到 0.1k
// 正好是 1000.0k，宁可让它进 M 档，也不要出现四位数 k。
//
// 四舍五入用「先整除再看余数」，而不是 (v + 50) / 100：后者在 v 接近
// UINT32_MAX 时会溢出成很小的数 —— 4294967295 + 50000 回绕成 49999，
// 字数直接显示成 "0.0M"，而且平时根本看不出来。
static void formatCountCompact(char* out, size_t n, uint32_t v) {
    if (v < 10000UL) {
        snprintf(out, n, "%lu", (unsigned long)v);
        return;
    }
    uint32_t d1;
    if (v < 999950UL) {
        d1 = v / 100UL + ((v % 100UL >= 50UL) ? 1UL : 0UL);   // 以 0.1k 为单位
        snprintf(out, n, "%lu.%luk", (unsigned long)(d1 / 10), (unsigned long)(d1 % 10));
    } else {
        d1 = v / 100000UL + ((v % 100000UL >= 50000UL) ? 1UL : 0UL);   // 以 0.1M 为单位
        snprintf(out, n, "%lu.%luM", (unsigned long)(d1 / 10), (unsigned long)(d1 % 10));
    }
}

static void build_style_high_contrast(void) {
    if (hc_bg) { lv_obj_del(hc_bg); hc_bg = nullptr; }

    hc_bg = makeRootPanel(ensureMainScreen(), 0x000000);   // 纯黑

    // ===========================================================
    // 顶栏（y=0..40）
    //   左侧 3 列：20px LED 圆点（亮态实心+同色光晕，灭态空心描边）+ 下方 NUM/CAPS/SCR 数字
    //   右侧：32×32 方案图标 + simsun_16 方案名
    // ===========================================================
    const uint32_t onColors[3] = { lockLedColor[0], lockLedColor[1], lockLedColor[2] };
    static const char* lockNames[3] = { "NUM", "CAPS", "SCR" };
    for (int i = 0; i < 3; i++) {
        const lv_coord_t cx = 30 + i * 44;                 // x=30 / 74 / 118（避开右侧图标）
        // LED 圆点：20px 直径实心圆（亮 = 锁色 + 大光晕，灭 = 透明空圈）
        lv_obj_t* dot = lv_obj_create(hc_bg);
        lv_obj_set_size(dot, 20, 20);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, cx - 10, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(onColors[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(dot, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
        lv_obj_set_style_shadow_color(dot, lv_color_hex(onColors[i]), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(dot, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(dot, 4, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        hc_led_dot[i] = dot;
        hc_lockOn[i]  = onColors[i];
        // 下方数字（居中对齐圆点）
        lv_obj_t* t = lv_label_create(hc_bg);
        mkLabel(t, &lv_font_montserrat_10, CLR_TEXT_DIM);
        lv_label_set_text(t, lockNames[i]);
        lv_obj_set_width(t, 44);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, cx - 22, 26);
        hc_lbl_lock[i] = t;
    }
    // 右上角：大号方案图标（48×48，不带方案名文字）
    hc_img_profile = lv_img_create(hc_bg);
    lv_img_set_src(hc_img_profile, profile_icon_get(currentProfile, PROF_ICON_L));
    lv_obj_align(hc_img_profile, LV_ALIGN_TOP_RIGHT, -6, 0);

    // 方案名（仅内部记录，界面不显示）
    hc_lbl_profile = lv_label_create(hc_bg);
    mkLabel(hc_lbl_profile, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(hc_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_add_flag(hc_lbl_profile, LV_OBJ_FLAG_HIDDEN);

    // ===========================================================
    // 时间区（无卡片框, 文字直接居中显示在 hc_bg 上）
    //   时间 y=46..94 (字号 48), 日期 y=98..114 (字号 16)
    // ===========================================================
    hc_card_time_strip = nullptr;   // 已废弃：时间卡和装饰条都删了
    hc_lbl_time = lv_label_create(hc_bg);
    mkLabel(hc_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(hc_lbl_time, "--:--");
    lv_obj_align(hc_lbl_time, LV_ALIGN_TOP_MID, 0, 46);

    hc_lbl_date = lv_label_create(hc_bg);
    mkLabel(hc_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(hc_lbl_date, "--");
    lv_obj_set_width(hc_lbl_date, 240);
    lv_obj_set_style_text_align(hc_lbl_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hc_lbl_date, LV_ALIGN_TOP_MID, 0, 98);

    // ===========================================================
    // 按键区（无卡片框, 文字直接居中显示在 hc_bg 上）
    //   按键(48) y=118..166 字号 48, 按键(28) y=134 备用默认隐藏
    // ===========================================================
    hc_card_key_strip = nullptr;   // 已废弃：按键卡和装饰条都删了
    hc_lbl_lastkey_48 = lv_label_create(hc_bg);
    mkLabel(hc_lbl_lastkey_48, &lv_font_montserrat_48, CLR_RED);
    lv_label_set_text(hc_lbl_lastkey_48, "-");
    lv_obj_set_width(hc_lbl_lastkey_48, 200);
    lv_obj_set_style_text_align(hc_lbl_lastkey_48, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hc_lbl_lastkey_48, LV_LABEL_LONG_DOT);
    lv_obj_align(hc_lbl_lastkey_48, LV_ALIGN_TOP_MID, 0, 124);

    hc_lbl_lastkey_28 = lv_label_create(hc_bg);
    mkLabel(hc_lbl_lastkey_28, &lv_font_montserrat_28, CLR_RED);
    lv_label_set_text(hc_lbl_lastkey_28, "");
    lv_obj_set_width(hc_lbl_lastkey_28, 200);
    lv_obj_set_style_text_align(hc_lbl_lastkey_28, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hc_lbl_lastkey_28, LV_LABEL_LONG_DOT);
    lv_obj_align(hc_lbl_lastkey_28, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_add_flag(hc_lbl_lastkey_28, LV_OBJ_FLAG_HIDDEN);   // 默认隐藏（5..8 字符才显示）

    // ===========================================================
    // 数据带（KPM/TODAY/ACTIVE）已删除：腾出空间给按键放大 + 温湿度加框
    // ===========================================================

    // ===========================================================
    // 底部状态条（框 x=10..230, y=178..235, w=220, h=58）
    //
    //   ┌ 左侧 2/3 ────────────────────────┬ 右侧 1/3 ─┐
    //   │ 温度                      23.5°C  │           │  y=180 中文小标(行高19)
    //   │ ▬▬▬▬▬▓░░░░░░░░░░░░░░░░░░░░░░░░░  │   字数     │          + 数值(行高18)
    //   │ 湿度                          58%  │  128.4k   │  y=199 第一根条(高6)
    //   │ ▬▬▬▬▬▬▬▓▓░░░░░░░░░░░░░░░░░░░░░  │           │  y=228 第二根条(高6)
    //   └─────────────────────────────────┴───────────┘
    //
    // 为什么是「上下堆叠」而不是并排：温度 / 湿度是**进度条**，条形的长度
    // 就是读数。并排时两格各分一半宽度，每根条只有 63px，23°C 和 28°C
    // 画出来只差 6px，肉眼分不出来。上下堆叠后每根条独占整个 2/3 的
    // 135px，量程差一倍差距就明显得多 —— 条才是主角，数字退成副信息。
    //
    // 每格 = 「中文小标（左）+ 数值（右）」同一行，下面紧跟一根满宽条。
    // 小标、数值、条三者左右边缘都是 x=16 / x=150，对齐成两条竖直线。
    //
    // 行高（行盒高度取自字库的 .line_height，不是猜的）：
    //   simsun_16_cjk=19 / montserrat_16=18 / montserrat_48=52
    //   一行文字的高度取中文的 19。竖向账：
    //     19 + 6(条) + 4(格间距) + 19 + 6 = 54  ← 正好是框内可用高度
    //   按键名 y=124 + 52 = 176，框顶 178 只留 2px，不能再往上；
    //   框底 235 → 屏幕底 240 留 4px。
    {
        hc_bot_box = iconRect(hc_bg, 220, 58, LV_ALIGN_TOP_MID, 0, 178,
                              0x000000, 8);
        lv_obj_set_style_border_width(hc_bot_box, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(hc_bot_box, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
        // 两区之间的竖分隔线：上下各收 3px，比通长更透气
        hc_bot_div = iconRect(hc_bg, 1, 48, LV_ALIGN_TOP_LEFT, 156, 183, CLR_STROKE, 0);
    }

    // --- 温度格（上）：小标 + 数值同一行，下面一根满宽条 ---
    // 数值标签和小标标签**故意完全重合**（同 x 同宽同 y）：LVGL 的 label 没有
    // 底色，后建的数值只是叠在小标文字上，不会互相遮住。省得为两种字号
    // 去算两个不同的基线 y —— 行盒都是顶部对齐的，重合天然就对得齐。
    hc_lbl_strip_t = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_t, &lv_font_simsun_16_cjk, CLR_AMBER);
    lv_label_set_text(hc_lbl_strip_t, "温度");
    lv_obj_set_width(hc_lbl_strip_t, 135);
    lv_obj_set_style_text_align(hc_lbl_strip_t, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(hc_lbl_strip_t, LV_ALIGN_TOP_LEFT, 16, 180);

    hc_lbl_strip_t_val = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_t_val, &lv_font_montserrat_16, CLR_TEXT);
    lv_label_set_text(hc_lbl_strip_t_val, "--.-°C");
    lv_obj_set_width(hc_lbl_strip_t_val, 135);
    lv_obj_set_style_text_align(hc_lbl_strip_t_val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(hc_lbl_strip_t_val, LV_LABEL_LONG_DOT);
    lv_obj_align(hc_lbl_strip_t_val, LV_ALIGN_TOP_LEFT, 16, 180);

    // 进度条：槽和填充是 hc_bg 下的兄弟对象，绝对坐标（见 mkBar 上面那段）。
    // 初值给 0 —— 首次进主屏时 updateDynamicElements() 会按真实读数改写，
    // 不先画一截假进度条，免得闪一下又被改掉。
    hc_bar_t_track = mkBar(hc_bg, 16, 199, HC_BAR_W, CLR_SURFACE_2);
    hc_bar_t_fill  = mkBar(hc_bg, 16, 199, 0,          CLR_AMBER);

    // --- 湿度格（下）---
    hc_lbl_strip_h = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_h, &lv_font_simsun_16_cjk, CLR_GREEN);
    lv_label_set_text(hc_lbl_strip_h, "湿度");
    lv_obj_set_width(hc_lbl_strip_h, 135);
    lv_obj_set_style_text_align(hc_lbl_strip_h, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(hc_lbl_strip_h, LV_ALIGN_TOP_LEFT, 16, 209);

    hc_lbl_strip_h_val = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_h_val, &lv_font_montserrat_16, CLR_TEXT);
    lv_label_set_text(hc_lbl_strip_h_val, "--%");
    lv_obj_set_width(hc_lbl_strip_h_val, 135);
    lv_obj_set_style_text_align(hc_lbl_strip_h_val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(hc_lbl_strip_h_val, LV_LABEL_LONG_DOT);
    lv_obj_align(hc_lbl_strip_h_val, LV_ALIGN_TOP_LEFT, 16, 209);

    hc_bar_h_track = mkBar(hc_bg, 16, 228, HC_BAR_W, CLR_SURFACE_2);
    hc_bar_h_fill  = mkBar(hc_bg, 16, 228, 0,          CLR_GREEN);

    // --- 字数列（x=162..223，中心 192.5）：不画条，纯数字 ---
    // 小标和数值上下分开排，而不是像左边那样同一行左右分 —— 这一格只有
    // 62px 宽，塞不下"字数" + 数值同一行还不打架。
    hc_lbl_strip_chars = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_chars, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(hc_lbl_strip_chars, "字数");
    lv_obj_set_width(hc_lbl_strip_chars, 62);
    lv_obj_set_style_text_align(hc_lbl_strip_chars, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hc_lbl_strip_chars, LV_ALIGN_TOP_LEFT, 162, 184);

    hc_lbl_strip_chars_val = lv_label_create(hc_bg);
    mkLabel(hc_lbl_strip_chars_val, &lv_font_montserrat_16, CLR_TEXT);
    lv_label_set_text(hc_lbl_strip_chars_val, "0");
    lv_obj_set_width(hc_lbl_strip_chars_val, 62);
    lv_obj_set_style_text_align(hc_lbl_strip_chars_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hc_lbl_strip_chars_val, LV_LABEL_LONG_DOT);
    lv_obj_align(hc_lbl_strip_chars_val, LV_ALIGN_TOP_LEFT, 162, 208);

}

// ===========================
// 构建：方案 + 按键
// ===========================
// 这一页的版面（240x240，下面的坐标全是**屏幕坐标**）：
//
//   y=0..28     顶栏：左上角「YYQ键盘」，右上角三颗**长方形**锁状态灯
//   y=30..204   主体：左列 62px 放**当前方案那一个**图标，右侧 164px 放按键卡
//   y=208..233  底部：温度 / 湿度两条窄条
//
// 三个容易踩的点：
//
// 1. 这一页**不建** dashTopBar()。老顶栏是"三颗圆点 + 方案序号"，
//    用户要的是"左上角一行字 + 右边三个长方形指示灯"，两套顶栏不能同时存在。
//
// 2. 按键名的字号是**按实测字宽动态选**的（48 → 28 → 24），不是按字符个数。
//    同样 4~6 个字符，宽度能差 50px：montserrat 48px 下 "RIGHT"=154.0px、
//    "Space"=148.1px 放得下（卡片内宽 152px），但 "NumLk"=180.3px、
//    "DOWN"=173.0px、"NumEnt"=207.0px 就撑爆了 —— 一刀切按字符数必然歪一半。
//    这里用 LVGL 自己的 lv_font_get_glyph_width() 量（带 kerning，返回像素），
//    和离线工具 lvgl_font_width.py 读的是同一份 adv_w，两边结论一致。
//
// 3. 方案图标用**矢量栅格化**的 style_icons.h（白色咬口苹果 + 蓝色 Windows），
//    不用 profile_icons.h 那套 mmx 位图 —— 那套的苹果 48px 下是一坨认不出的
//    白团，Windows 还是青绿渐变，不是用户点名要的白 / 蓝。
//
// 4. 左列**只画当前方案那一个图标**（2026-10-22 用户定稿：
//    "左边图标还是展示一个吧，两个也有点分不清楚"）。原来是把两个方案
//    上下摆着、当前点亮另一个压灰 —— 52px 的苹果在 240px 屏上本来就看不清，
//    压灰那版更是只剩一团灰影，**分不出哪个是当前**。现在只剩一个，
//    永远是点亮态：白苹果 = macOS，蓝色 Windows = Windows。
//    下面也不用再挂「方案」两个字（用户："方案 那两个字可以去掉"）。
#define PK_TITLE_H     28      // 顶栏高
#define PK_LEFT_X      4
#define PK_LEFT_W      62
#define PK_ICON        52      // 方案图标边长
#define PK_ICON_CARD   61      // 图标后面那块高亮底卡的边长
#define PK_ICON_CARD_Y 82      // 底卡顶边（跟按键名 label 的顶边齐平）
#define PK_ICON_Y      86      // 图标本体的顶边（底卡 61 - 图标 52，居中 → +4）
#define PK_CARD_X      70
#define PK_CARD_W      164     // 右边缘 234（和灯组右边缘齐平）
#define PK_CARD_TOP    32
#define PK_CARD_BOT    200
#define PK_KEY_Y       82      // 按键名的顶边（屏幕坐标）
#define PK_TODAY_W     64      // 今日按键数那个定宽框（右对齐用）
#define PK_LIGHT_W     38      // 长方形锁灯
#define PK_LIGHT_H     14
#define PK_LIGHT_GAP   5
#define PK_LIGHT_RIGHT 6       // 灯组离右边多远
#define PK_BAR_Y1      208     // 温度条
#define PK_BAR_Y2      223     // 湿度条
#define PK_BAR_H       7
#define PK_BAR_X0      44      // 条的左端
#define PK_BAR_X1      176     // 条的右端
// 温湿度那两个字的中文标签，**故意不让它们跟条的行距一样**（15px）。
// 中文字体的墨迹是 15px 高（box_h=15, ofs_y=-2），LVGL 把墨迹放在
// 标签顶边 + (line_height - base_line) - box_h - ofs_y = +3 的位置：
//   温度标签在 203 → 墨迹 206..220
//   湿度标签在 218 → 墨迹 221..235
// 两行只差 15px，墨迹**正好相接、零间隙**，看着就是"温度湿度粘在一起"。
// 拉开到 17px 行距 → 中间留 2px，才读得出是两行。
#define PK_LBL_T_Y     201
#define PK_LBL_H_Y     218

// 取下一个 UTF-8 码点。
//
// 为什么要自己解：LVGL 的 letter / letter_next 是**码点**不是字节
// （见 lv_font.c 里 lv_font_get_glyph_dsc 的注释 "@param letter a UNICODE
// letter"）。直接拿 (uint8_t)*p 当码点在 ASCII 下碰巧对，但以后
// getKeyName() 一旦返回中文，量出来的宽度就是错的 → 选出来的字号偏大
// → 文字撑破卡片。宁可多写十行，也不能留这种雷。
static uint32_t pkNextCodepoint(const char* s, uint32_t* idx) {
    const uint8_t* p = (const uint8_t*)s;
    uint32_t i = *idx;
    uint8_t c = p[i];
    uint32_t cp;
    int extra;
    if (c < 0x80)                 { cp = c;        extra = 0; }
    else if ((c & 0xE0) == 0xC0)  { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0)  { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0)  { cp = c & 0x07; extra = 3; }
    else                          { cp = c;        extra = 0; }
    for (int k = 1; k <= extra; k++) {
        if ((p[i + k] & 0xC0) != 0x80) { extra = 0; break; }   // 截断的序列，当单字节处理
        cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    *idx = i + (uint32_t)extra + 1;
    return cp;
}

// 文本在给定字体下的排版宽度（像素）。
// 跟 LVGL 内部排版同一套算法：lv_font_get_glyph_width() 带 kerning，
// 等价于 lv_txt_get_width()，但那个在 misc/lv_txt.h 里、没有被 lvgl.h
// 公开导出，所以这里按同样逻辑自己累加。
static lv_coord_t pkMeasureWidth(const char* txt, const lv_font_t* font) {
    if (txt == nullptr || font == nullptr) return 0;
    lv_coord_t w = 0;
    uint32_t i = 0;
    while (txt[i] != '\0') {
        uint32_t cur = pkNextCodepoint(txt, &i);
        uint32_t nxt = 0;
        if (txt[i] != '\0') {
            uint32_t j = i;
            nxt = pkNextCodepoint(txt, &j);
        }
        lv_coord_t cw = (lv_coord_t)lv_font_get_glyph_width(font, cur, nxt);
        if (cw > 0) w += cw;
    }
    return w;
}

// 给按键名挑最大的、且放得下的字号。放不下就退一档，全放不下用 24 号兜底。
static void pkApplyKeyFont(const char* txt) {
    if (pk_lbl_key == nullptr) return;
    const lv_font_t* cands[3] = {
        &lv_font_montserrat_48, &lv_font_montserrat_28, &lv_font_montserrat_24
    };
    const lv_coord_t avail = PK_CARD_W - 12;    // 卡片内宽（左右各留 6px）
    const lv_font_t* pick = cands[2];            // 兜底
    for (int i = 0; i < 3; i++) {
        if (pkMeasureWidth(txt, cands[i]) <= avail) { pick = cands[i]; break; }
    }
    lv_obj_set_style_text_font(pk_lbl_key, pick, LV_PART_MAIN);
}

// 方案切换时刷新这一页的图标。
// 为什么要单独抽一个函数：updateDynamicElements() 开头有"系统时间没校准就
// return"的分支，只靠它会出现"换了方案图标不跟着变"。老顶栏的
// updateTopBarProfile() 就是为同一件事抽出来的，这里照抄那个套路。
// currentProfile：0 = Windows，1 = macOS（见 profileNamesCN）。
static void updatePlanKeyProfile(void) {
    if (pk_icon == nullptr) return;
    if (pkShownProfile == currentProfile) return;
    pkShownProfile = currentProfile;

    // 左列**只画当前方案那一个**图标，永远是点亮态：
    //   macOS → 白色咬口苹果，Windows → 蓝色四格旗。
    // 底卡 pk_iconCard 一直是 CLR_SURFACE_2 的实底（建的时候定死的），
    // 不用再按"当前/非当前"切透明度 —— 没有第二个图标了。
    bool mac = (currentProfile == 1);
    lv_img_set_src(pk_icon, mac ? &style_icon_apple_desc : &style_icon_win_desc);
}

// 底部两条窄条的填充长度。
static void pkSetBarFill(lv_obj_t* fill, lv_coord_t x, lv_coord_t y, float pct) {
    if (fill == nullptr) return;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    lv_coord_t trackW = PK_BAR_X1 - PK_BAR_X0;
    lv_coord_t w = (lv_coord_t)((float)trackW * pct + 0.5f);
    // 读数非零却一根都不显示，看上去像传感器挂了 —— 至少留 1px
    if (w == 0 && pct > 0.0f) w = 1;
    setSizePos(fill, w, PK_BAR_H, x, y);
}

// 「方案+按键」这一页每 100ms 刷一次的内容。
//
// ⚠️ 这个函数**必须**在 updateDynamicElements() 里那个
// "系统时间没校准就 return"的早退**之前**调用 —— 这一页显示的是
// "刚按下的键 / 今日次数 / 温湿度 / 锁灯"，**一样都不依赖系统时间**。
// 要是跟其它风格一样塞在 switch 里（switch 在早退之后），那在没对上 NTP
// 的时候这一页会一直停在初始状态：按什么键屏幕都不动 —— 正好把这一页
// 最核心的东西（按键反馈）废掉了。
static void updatePlanKeyElements(void) {
    if (pk_bg == nullptr) return;

    // 按键名：内容变了才换字号。字号是按实测字宽挑的
    // （见 pkApplyKeyFont 的注释），换字号要重排版，不能每 100ms 都做。
    if (strcmp(pkLastKey, lastKeyPressed) != 0) {
        snprintf(pkLastKey, sizeof(pkLastKey), "%s", lastKeyPressed);
        pkApplyKeyFont(pkLastKey);
    }
    setText(pk_lbl_key, lastKeyPressed);

    // 右下角：今日按键数。todayKeyCount 是"今天按了多少次"，
    // 本地时间跨日自动清零（按键分发路径里判 todayDateYmd，
    // 开机时再跟 NVS 的 today_y 比一次）。
    static char pkToday[16];
    formatCountCompact(pkToday, sizeof(pkToday), todayKeyCount);
    setText(pk_lbl_today, pkToday);

    // 数字的字号也按实测字宽挑：formatCountCompact 一天按到 10 万次就会
    // 输出 "999.9k"（20px 下 66.1px），10 万以上到 42.9 亿会输出 "4295.0M"
    // （85.7px）—— 那个 64px 的框**装不下**，而 CLIP + 右对齐是把超宽文本的
    // **左边**切掉，结果就是显示成一个少一位的错数（看着像 9.99 万，其实是
    // 99.99 万）。所以装不下就退到 14 号（最宽 60.0px，仍然放得下）。
    {
        const lv_font_t* tf[2] = { &lv_font_montserrat_20, &lv_font_montserrat_14 };
        const lv_font_t* pick = tf[0];
        if (pkMeasureWidth(pkToday, tf[0]) > PK_TODAY_W) pick = tf[1];
        lv_obj_set_style_text_font(pk_lbl_today, pick, LV_PART_MAIN);
    }

    // 方案图标：换方案的那一刻 switchProfile() 会调 updatePlanKeyProfile()，
    // 这里再兜一次 —— 万一这一页是在别的方案下建出来的。
    updatePlanKeyProfile();

    // 三颗长方形锁灯：亮 = 整颗填语义色，灭 = 暗底灰字。
    // 语义色和 C3 上 16/17/18 号物理指示灯同源（lockLedColor）。
    // 灭态文字用 CLR_TEXT_DIM（#8E9BB2）而不是 CLR_TEXT_MUTE（#5B6579）：
    // 三颗灯**默认都是灭的**，灭态才是常态，而 10px 的小字压在
    // CLR_SURFACE_2 上，CLR_TEXT_MUTE 的对比度只有 2.55:1 —— 屏上基本
    // 看不见"NUM/CAPS/SCR"三个字。换成 CLR_TEXT_DIM 是 5.31:1。
    const bool pkLocks[3] = { numLock, capsLock, scrollLock };
    for (int i = 0; i < 3; i++) {
        bool on = pkLocks[i];
        setBgColor(pk_lock[i], on ? lockLedColor[i] : CLR_SURFACE_2);
        setTextColor(pk_lockLbl[i], on ? CLR_BG : CLR_TEXT_DIM);
    }

    // 底部温度 / 湿度。shtAvailable 为 false（传感器没接上）时显示
    // "--.-°C" / "--%"，条保持空槽 —— 不能拿初值 0 画一根满格，
    // 骗用户以为读到的是 0% / 0°C（高对比度页也是这么处理的）。
    static char pkT[16], pkH[16];
    snprintf(pkT, sizeof(pkT), "%.1f°C", shtTemp);
    snprintf(pkH, sizeof(pkH), "%.0f%%", shtHumidity);
    setText(pk_lbl_tVal, shtAvailable ? pkT : "--.-°C");
    setText(pk_lbl_hVal, shtAvailable ? pkH : "--%");

    // 量程：温度 0..50°C（室内实际 15~35，23°C 正好在中段，一眼能分辨
    // 偏凉/舒服/偏热；用 0..100 的话室内永远只有 1/4 长，条就废了）。
    // 湿度 0..100%（传感器本来就是这个量程）。两边都夹紧，坏读数不能让条溢出。
    float pkTpct = 0.0f, pkHpct = 0.0f;
    if (shtAvailable) {
        pkTpct = shtTemp / 50.0f;
        pkHpct = shtHumidity / 100.0f;
    }
    pkSetBarFill(pk_bar_tFill, PK_BAR_X0, PK_BAR_Y1, pkTpct);
    pkSetBarFill(pk_bar_hFill, PK_BAR_X0, PK_BAR_Y2, pkHpct);
}

static void build_style_plankey(void) {
    if (pk_bg) {
        lv_obj_del(pk_bg);
        pk_bg = nullptr;
        // lv_obj_del(pk_bg) 会**连带**释放 pk_card 和它的子对象（按键名 /
        // TODAY / 今日数）、三颗锁灯、两条窄条的槽和填充、两个图标 …
        // 这里必须自己把子指针清掉。
        //
        // 以前只写 "lv_obj_del(pk_bg); pk_bg = nullptr;"，安全完全靠调用方
        // 恰好先调了 resetStylePointers()。哪天这个函数多一个调用点
        // （预览 / 快速切换），下一轮 updateDynamicElements() 就会往已释放
        // 的内存上写 —— 踩坏堆 → 整机重启。这正是本文件反复栽过的坑，
        // 函数不能依赖调用方的顺序。
        pk_lbl_title = nullptr; pk_card = nullptr;
        pk_lbl_key = nullptr;
        pk_lbl_todayCap = nullptr; pk_lbl_today = nullptr;
        pk_lbl_tCap = nullptr; pk_lbl_tVal = nullptr;
        pk_lbl_hCap = nullptr; pk_lbl_hVal = nullptr;
        pk_bar_tTrack = nullptr; pk_bar_tFill = nullptr;
        pk_bar_hTrack = nullptr; pk_bar_hFill = nullptr;
        for (int i = 0; i < 3; i++) { pk_lock[i] = nullptr; pk_lockLbl[i] = nullptr; }
        pk_icon = nullptr; pk_iconCard = nullptr;
        pkShownProfile = 0xFF;
        pkLastKey[0] = '\0';
    }

    pk_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

    // ---- 顶栏：左上标题 ----
    // 「YYQ键盘」用**一个** label 写完：拆成 "YYQ"(montserrat) + "键盘"(黑体)
    // 两个 label 的话两种字号行高不同，基线对不齐，中文会比英文高飘几像素。
    pk_lbl_title = lv_label_create(pk_bg);
    mkLabel(pk_lbl_title, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(pk_lbl_title, "YYQ键盘");
    lv_obj_align(pk_lbl_title, LV_ALIGN_TOP_LEFT, PK_LEFT_X, 5);

    // ---- 顶栏：右上三颗长方形锁灯 ----
    // 亮 = 整颗填语义色（NUM 绿 / CAPS 青 / SCR 琥珀），灭 = 暗底灰字。
    // 颜色跟 C3 上 16/17/18 号物理指示灯同源（lockLedColor）。
    const char* lockNames[3] = { "NUM", "CAPS", "SCR" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = lv_obj_create(pk_bg);
        lv_obj_set_size(b, PK_LIGHT_W, PK_LIGHT_H);
        // 从右往左摆：SCR 最右、CAPS 中间、NUM 最左（跟键盘上物理灯一致）
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT,
                     -(PK_LIGHT_RIGHT + (2 - i) * (PK_LIGHT_W + PK_LIGHT_GAP)),
                     (PK_TITLE_H - PK_LIGHT_H) / 2);
        mkChip(b, CLR_SURFACE_2, CLR_TEXT_MUTE, 4);
        pk_lock[i] = b;

        lv_obj_t* t = lv_label_create(b);
        mkLabel(t, &lv_font_montserrat_10, CLR_TEXT_MUTE);
        lv_label_set_text(t, lockNames[i]);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
        pk_lockLbl[i] = t;
    }

    // ---- 左列：当前方案的图标 ----
    // 只画**当前方案那一个**图标（用户 2026-10-22 定稿："左边图标还是展示一个吧，
    // 两个也有点分不清楚"）。52px 的苹果在 240px 屏上本来就小，两个一上一下摆着、
    // 一个点亮一个压灰，**压灰那版只剩一团灰影**，根本分不出哪个是当前 ——
    // 现在只剩一个，永远是点亮态：白苹果 = macOS，蓝 Windows = Windows。
    //
    // 底卡先建、图标后建，图标才画在底卡上面。底卡顶边 PK_ICON_CARD_Y = 82
    // 就是按键名 label 的顶边 —— 图标和那行大字**共用一条上边线**，
    // 读起来是"图标 → 按键"的一对，而不是两个互不相干的块。
    // 图标在 61px 底卡里居中：61-52 = 9 → 上 4 下 5，所以图标顶边 86。
    //
    // 2026-10-22：底卡原来用**抬升面 CLR_SURFACE_2**，按键卡用 **CLR_SURFACE**，
    // 两块一深一浅，一眼就分成了"两个东西"（用户原话："块不要区分的这么明显"）。
    // 现在两边**同一种材质**：同样的底色、同样的 1px 描边、同样的圆角，
    // 只靠"大小和位置"区分主次，不再靠颜色。图标那块小、按键那块大，
    // 主次本来就看得出来，不需要额外的亮度差来强调。
    pk_iconCard = lv_obj_create(pk_bg);
    lv_obj_set_size(pk_iconCard, PK_ICON_CARD, PK_ICON_CARD);
    lv_obj_align(pk_iconCard, LV_ALIGN_TOP_LEFT, PK_LEFT_X, PK_ICON_CARD_Y);
    mkCard(pk_iconCard, CLR_SURFACE, 12);

    pk_icon = lv_img_create(pk_bg);
    lv_img_set_src(pk_icon, (currentProfile == 1) ? &style_icon_apple_desc : &style_icon_win_desc);
    lv_obj_align(pk_icon, LV_ALIGN_TOP_LEFT,
                 PK_LEFT_X + (PK_LEFT_W - PK_ICON) / 2, PK_ICON_Y);
    pkShownProfile = 0xFF;      // 强制下一次 updatePlanKeyProfile() 真的刷一次
    updatePlanKeyProfile();     // 按当前方案选图标

    // ---- 右侧：按键卡 ----
    pk_card = lv_obj_create(pk_bg);
    lv_obj_set_size(pk_card, PK_CARD_W, PK_CARD_BOT - PK_CARD_TOP);
    lv_obj_align(pk_card, LV_ALIGN_TOP_LEFT, PK_CARD_X, PK_CARD_TOP);
    mkCard(pk_card, CLR_SURFACE, 12);

    // 按键名。**挂在卡里**而不是挂在 pk_bg 上：TOP_MID 是相对父对象居中的，
    // 挂 pk_bg 的话会居中到屏幕中线 x=120，而这张卡的中心在 x=152 ——
    // 差 32px，看着就是"字没居中"。挂卡里就自动跟着卡走。
    //
    // 颜色用**主强调青**（CLR_ACCENT），不是白 —— 2026-10-22 改。
    //
    // 为什么：这一页的主角就是这行按键名，但它原来是白字，而白字在这一页
    // **一共出现四处**（标题「YYQ键盘」/ 按键名 / 温度数值 / 湿度数值）。
    // 48px 的主角跟 14px 的小字在颜色上完全没分开，反倒是右下角那个紫色的
    // 「1284」因为有颜色、更抢眼 —— 主角被配角压住了。
    // 字号已经顶到 48px（能开的最大档）没法再加，**加色相**是唯一还能提
    // 层次的手段：OLED 上高色度的颜色比纯白更"跳"。
    //
    // 选青而不是别的：
    //   · 青是这套配色里的**主强调色**，语义上就是"最重要的那个"；
    //   · 和左边的 Windows 蓝旗（#0078D4）是同类冷色、和谐不打架，
    //     换成琥珀会跟下面的温度条同色、换成紫会跟「1284」同色；
    //   · 对比度 9.4:1（按键卡底 #161C2B 上），48px/28px 两种字号都够。
    pk_lbl_key = lv_label_create(pk_card);
    mkLabel(pk_lbl_key, &lv_font_montserrat_48, CLR_ACCENT);
    lv_label_set_text(pk_lbl_key, "-");
    // 字号是按实测字宽挑的，理论上一定放得下；但万一 getKeyName() 以后
    // 加了个超长键名，label 默认的长模式是 WRAP —— 会折成两行把版面顶乱。
    // CLIP 兜底：宁可右边切掉一点，也不许折行。
    lv_label_set_long_mode(pk_lbl_key, LV_LABEL_LONG_CLIP);
    // ★ 重建后**立刻**按当前键名定一次字号。
    //   updatePlanKeyElements() 里的 pkLastKey 缓存被 resetStylePointers()
    //   清掉了，下一次刷新会重挑；但那次刷新是 100ms 之后的下一轮 loop，
    //   这中间屏幕上会先摆一个 48 号的版本 —— "NumEnt" 就是 207px，
    //   撑破卡片、压住左列图标、跑出屏幕右边缘。这里先定好，别露那个丑态。
    pkApplyKeyFont(lastKeyPressed);
    lv_obj_align(pk_lbl_key, LV_ALIGN_TOP_MID, 0, PK_KEY_Y - PK_CARD_TOP);

    // 右下角：今日按键数。数字固定占 64px 的框、右对齐，
    // 这样不管今天按了多少次（位数在变），"TODAY" 这个词的右边缘都不会跑。
    pk_lbl_todayCap = lv_label_create(pk_card);
    mkLabel(pk_lbl_todayCap, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(pk_lbl_todayCap, "TODAY");
    lv_obj_align(pk_lbl_todayCap, LV_ALIGN_BOTTOM_RIGHT, -82, -14);

    pk_lbl_today = lv_label_create(pk_card);
    mkLabel(pk_lbl_today, &lv_font_montserrat_20, CLR_VIOLET);
    lv_label_set_text(pk_lbl_today, "0");
    lv_obj_set_width(pk_lbl_today, PK_TODAY_W);
    lv_obj_set_style_text_align(pk_lbl_today, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    // 同上：定宽 64px 的 label 默认会折行，今天的位数万一变多就折成两行。
    lv_label_set_long_mode(pk_lbl_today, LV_LABEL_LONG_CLIP);
    lv_obj_align(pk_lbl_today, LV_ALIGN_BOTTOM_RIGHT, -12, -8);

    // ---- 底部：温度 / 湿度两条窄条 ----
    const char* capNames[2] = { "温度", "湿度" };
    const int barY[2] = { PK_BAR_Y1, PK_BAR_Y2 };
    const int capY[2] = { PK_LBL_T_Y, PK_LBL_H_Y };
    for (int i = 0; i < 2; i++) {
        lv_obj_t* cap = lv_label_create(pk_bg);
        mkLabel(cap, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
        lv_label_set_text(cap, capNames[i]);
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, PK_LEFT_X, capY[i]);

        lv_obj_t* val = lv_label_create(pk_bg);
        mkLabel(val, &lv_font_montserrat_14, CLR_TEXT);
        lv_label_set_text(val, i == 0 ? "--.-°C" : "--%");
        lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -6, barY[i] - 5);

        lv_obj_t* track = lv_obj_create(pk_bg);
        lv_obj_set_size(track, PK_BAR_X1 - PK_BAR_X0, PK_BAR_H);
        lv_obj_align(track, LV_ALIGN_TOP_LEFT, PK_BAR_X0, barY[i]);
        mkChip(track, CLR_SURFACE_2, CLR_TEXT_MUTE, 3);

        lv_obj_t* fill = lv_obj_create(pk_bg);
        lv_obj_set_size(fill, 0, PK_BAR_H);
        lv_obj_align(fill, LV_ALIGN_TOP_LEFT, PK_BAR_X0, barY[i]);
        // 温度=琥珀、湿度=紫。湿度原来也是青（CLR_ACCENT），但青现在归
        // 按键名**独占**了 —— 同一屏三处青（按键名 + CAPS 灯 + 湿度条），
        // 主角的颜色就不主角了。紫色留给「1284」和湿度条当次级。
        mkChip(fill, i == 0 ? CLR_AMBER : CLR_VIOLET, CLR_TEXT_MUTE, 3);

        if (i == 0) {
            pk_lbl_tCap = cap; pk_lbl_tVal = val;
            pk_bar_tTrack = track; pk_bar_tFill = fill;
        } else {
            pk_lbl_hCap = cap; pk_lbl_hVal = val;
            pk_bar_hTrack = track; pk_bar_hFill = fill;
        }
    }
}

// ===========================
// 息屏 / 屏保
// ===========================
// 屏保是一块独立的屏幕对象，和主屏并存：
//   SAVER_BLACK 沿用老行为 —— 拆掉主屏，屏幕全黑
//   SAVER_WALL 壁纸铺满 + 底部通栏信息条（常驻，不再每 5 秒闪一次）
//   SAVER_INFO 只显示信息面板（居中卡片）
//   SAVER_OFF  根本不建屏保屏（这一档压根到不了这里，调用方会先拦掉）
// 唤醒统一走 gotoMainScreen()：先 showScreen(主屏) 再 destroyScreensaver()，
// 顺序反了就会删掉活动屏，LVGL 8.4 会把 disp->act_scr 置成 NULL。
static void destroyScreensaver(void) {
    sv_img = nullptr;
    sv_panel = nullptr;
    sv_lbl_time = nullptr; sv_lbl_date = nullptr;
    sv_lbl_temp = nullptr; sv_lbl_hum = nullptr;
    if (scr_saver != nullptr) {
        // 活动屏不能直接删：LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL，
        // 下一帧刷新就是野指针 panic（见 swapAwayIfActive 的注释）。
        // enterScreensaver() 会连续进两次（再按一次唤醒、马上又超时息屏），
        // 这条路径上 scr_saver 确实可能正显示着。
        swapAwayIfActive(scr_saver, nullptr);
        // ⚠ 原来的判据写在 scr_saver = nullptr **之后**：
        //     if (currentScreen == scr_saver) currentScreen = nullptr;
        // 拿已经置空的指针去比，永远为假，currentScreen 就留在已释放的屏上，
        // 后面谁拿它去 lv_scr_load 就是 use-after-free。必须在置空前比。
        if (currentScreen == scr_saver) currentScreen = nullptr;
        lv_obj_del(scr_saver);
        scr_saver = nullptr;
    }
}

// 屏保信息面板：时间 / 日期 / 温湿度全收在**底部通栏条**里。
//
// 旧版是一张 204x112 的**不透明**卡片，贴底居中：
//   · 它把壁纸下半张整个盖住 —— "YYQ"主标和律动条全没了，
//     屏保本来是给人看图的，图没了屏保就没意义了
//   · 卡片带 1px 描边 + 14px 圆角，浮在图上像张便利贴
//   · SAVER_WALL 模式下还每 5 秒"啪"地盖上又消失（见 updateScreensaver），
//     每次切换都是一次突兀的闪
//
// 改成通栏条之后：图完整露出，信息贴在底部一条暗带里，
// 描边和圆角全不要（它是界面结构，不是贴上去的控件）。
// SAVER_INFO（纯信息屏、没有壁纸）反而需要卡片 —— 那里没图可挡，
// 一张居中的卡片才是对的形态，所以这一档保留原样。
static void buildSaverPanel(void) {
    if (saverMode == SAVER_INFO) {
        // 纯信息模式：没有壁纸，一张居中的卡片才是对的形态
        sv_panel = lv_obj_create(scr_saver);
        lv_obj_set_size(sv_panel, 204, 112);
        lv_obj_align(sv_panel, LV_ALIGN_CENTER, 0, 0);
        mkCard(sv_panel, CLR_SURFACE, 14);

        sv_lbl_time = lv_label_create(sv_panel);
        mkLabel(sv_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(sv_lbl_time, "--:--");
        lv_obj_align(sv_lbl_time, LV_ALIGN_TOP_MID, 0, 6);

        sv_lbl_date = lv_label_create(sv_panel);
        mkLabel(sv_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(sv_lbl_date, "1月1日 星期一");
        lv_obj_align(sv_lbl_date, LV_ALIGN_TOP_MID, 0, 60);

        // 温湿度一行，中间一根细竖线隔开（比两段文字并排更容易一眼分开）
        iconRect(sv_panel, 1, 18, LV_ALIGN_BOTTOM_MID, 0, -8, CLR_STROKE, 0);

        sv_lbl_temp = lv_label_create(sv_panel);
        mkLabel(sv_lbl_temp, &lv_font_montserrat_16, CLR_AMBER);
        lv_label_set_text(sv_lbl_temp, "--.-C");
        lv_obj_align(sv_lbl_temp, LV_ALIGN_BOTTOM_MID, -52, -6);

        sv_lbl_hum = lv_label_create(sv_panel);
        mkLabel(sv_lbl_hum, &lv_font_montserrat_16, CLR_ACCENT);
        lv_label_set_text(sv_lbl_hum, "--%");
        lv_obj_align(sv_lbl_hum, LV_ALIGN_BOTTOM_MID, 52, -6);
        return;
    }

    // 壁纸模式：底部通栏条（和壁纸主屏风格 6 用的是同一套坐标，见 BAR_H/BAR_Y）
    sv_panel = lv_obj_create(scr_saver);
    lv_obj_set_size(sv_panel, 240, BAR_H);
    lv_obj_set_pos(sv_panel, 0, BAR_Y);
    lv_obj_set_style_bg_color(sv_panel, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sv_panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(sv_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sv_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sv_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sv_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 时间 28px 靠左（"22:48" = 75.3px）→ 占 x=14..89
    sv_lbl_time = lv_label_create(sv_panel);
    mkLabel(sv_lbl_time, &lv_font_montserrat_28, CLR_TEXT);
    lv_label_set_text(sv_lbl_time, "--:--");
    lv_obj_set_pos(sv_lbl_time, 14, 8);

    // 日期 16px 中文，右对齐到 226（宽 120px）→ 占 x=106..226
    sv_lbl_date = lv_label_create(sv_panel);
    mkLabel(sv_lbl_date, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(sv_lbl_date, "1月1日 星期一");
    lv_obj_set_pos(sv_lbl_date, 106, 12);

    // 温湿度：第二行右对齐。"24.3C"=44.2px + "48%"=34.5px，
    // 加 8px 间隔共 86.7px，从 225 往左排 → 温度 x=134、湿度 x=190，
    // 湿度右缘 190+34.5=224.5，留 15px 右边距（不贴边）。
    sv_lbl_temp = lv_label_create(sv_panel);
    mkLabel(sv_lbl_temp, &lv_font_montserrat_16, CLR_AMBER);
    lv_label_set_text(sv_lbl_temp, "--.-C");
    lv_obj_set_pos(sv_lbl_temp, 134, 38);

    sv_lbl_hum = lv_label_create(sv_panel);
    mkLabel(sv_lbl_hum, &lv_font_montserrat_16, CLR_ACCENT);
    lv_label_set_text(sv_lbl_hum, "--%");
    lv_obj_set_pos(sv_lbl_hum, 190, 38);
}

// 菜单里"屏保风格"那一项：黑屏 → 壁纸 → 信息面板 → 关闭 → 黑屏
// 设成指定的屏保模式（含落盘 + 正在屏保时立刻按新模式重建）。
// 原来这段逻辑长在 cycleScreensaverMode 里，只能"转一格"。配置 JSON 要能直接
// 设成任意一档（网页上勾哪项就是哪项），所以把主体抽出来，cycle 只负责 +1。
static void setScreensaverMode(uint8_t m) {
    if (m >= TOTAL_SAVER_MODES) return;
    saverMode = m;
    preferences.putUChar("saver_mode", saverMode);

    // 正在屏保状态下切换：立刻按新模式重建，用户不用等下一次超时
    if (currentSysMode == SYS_MODE_SLEEP) {
        destroyScreensaver();
        if (saverMode == SAVER_OFF) {
            // 刚从屏保切到"关闭"：主屏可能早就被 SAVER_BLACK 拆过，
            // 现在得把它建回来，否则屏幕就一直黑着。
            renderCurrentDisplayBase();
            showScreen(ensureMainScreen());
            currentSysMode = SYS_MODE_NORMAL;
        } else if (saverMode == SAVER_BLACK) {
            destroyMainScreen();
        } else {
            enterScreensaver();
        }
    } else if (saverMode == SAVER_BLACK && scr_main == nullptr) {
        // 从屏保退回来，但主屏早就被拆了 —— 现在就得补回来
        renderCurrentDisplayBase();
    }
    build_menu();
}

static void cycleScreensaverMode(void) {
    setScreensaverMode((uint8_t)((saverMode + 1) % TOTAL_SAVER_MODES));
}

static void enterScreensaver(void) {
    // "关闭"这一档不该走到这里（loop() 里的超时判定会先拦掉），
    // 但留一道保险：万一别处调进来，也不能把主屏拆了。
    if (saverMode == SAVER_OFF) return;
    if (saverMode == SAVER_BLACK) {
        destroyMainScreen();
        return;
    }
    if (scr_saver != nullptr) { destroyScreensaver(); }

    scr_saver = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_saver, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_saver, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr_saver, LV_OBJ_FLAG_SCROLLABLE);

    if (saverMode == SAVER_WALL) {
        // 壁纸铺满。原来是 lv_img_set_src(sv_img, NULL) —— 屏保里那张图
        // **从来就没挂上去过**，不管传没传过壁纸都只是一块深色底，
        // 这就是"睡眠的时候图片没有展示"。
        sv_img = lv_img_create(scr_saver);
        lv_obj_set_size(sv_img, 240, 240);
        lv_obj_set_pos(sv_img, 0, 0);
        lv_obj_set_style_bg_color(sv_img, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(sv_img, 0, LV_PART_MAIN);
        lv_img_set_src(sv_img, wpReady ? &wpImgDsc : NULL);
        if (!wpReady) setHidden(sv_img, true);   // 没图就别留一块空底色
    }

    buildSaverPanel();
    // 信息条常驻（不再按"有壁纸就藏起来"处理 —— 底部条只压住最下面一条，
    // 藏了反而让屏幕每隔几秒抖一下，见 updateScreensaver 的说明）。
    setHidden(sv_panel, false);

    updateScreensaver(true);
    showScreen(scr_saver);
}

// 屏保的慢变量刷新。息屏时 currentSysMode != SYS_MODE_NORMAL，
// updateDynamicElements() 压根不会被调用，所以这里自己按同样的节奏跑。
// force = true 时立刻刷一次（进屏保的那一瞬间不能等 100ms 才出数字）。
static void updateScreensaver(bool force) {
    if (scr_saver == nullptr) return;

    // 和 updateDynamicElements() 同样的 100ms 节奏（那边是 DYNAMIC_REFRESH_MS，
    // 那个宏定义在文件后段，这里用字面量避免前向依赖）
    static unsigned long lastRunMs = 0;
    unsigned long nowMs = millis();
    if (!force && nowMs - lastRunMs < 100UL) return;
    lastRunMs = nowMs;

    // 壁纸模式：**信息条常驻，不再每 5 秒闪一次**。
    //
    // 原来这里有个 5 秒的轮播（tick>=50 时把面板藏起来再放出来），
    // 起因是旧面板是 204x112 的不透明大卡片，贴着壁纸正中间，
    // 不藏起来根本看不见图 —— 藏/放之间那道突兀的闪是这么来的。
    //
    // 现在面板改成了底部 72px 的通栏条（见 buildSaverPanel），
    // 只压住图片最下面一条，主标和表盘全都露在外面，
    // 根本不需要"让位"，也就没有理由再闪。
    // 留着轮播只会让屏幕每隔 5 秒抖一下 —— 那是纯粹的突兀。
    setHidden(sv_panel, false);

    // 标签没建出来就不用刷（SAVER_BLACK 压根不会走到这）
    if (sv_lbl_time == nullptr) return;

    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || ti->tm_year < 124) return;

    static char tbuf[16], dbuf[32], cbuf[16], hbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M", ti);
    formatDateCN(dbuf, sizeof(dbuf), ti->tm_mon + 1, ti->tm_mday, ti->tm_wday);
    snprintf(cbuf, sizeof(cbuf), "%.1fC", shtTemp);
    snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);

    setText(sv_lbl_time, tbuf);
    setText(sv_lbl_date, dbuf);
    setText(sv_lbl_temp, shtAvailable ? cbuf : "--.-C");
    setText(sv_lbl_hum, shtAvailable ? hbuf : "--%");
}

// ===========================
// 菜单选择处理
// ===========================
// 切换主屏风格的统一入口
// LOGO 键（主屏短按）和菜单第 2 项都走这里，保证"HUD 文案 + 持久化 + 重建 + 切屏"
// 这一套永远是一份，不会出现两个入口行为不一致。
static void cycleDisplayStyle(void) {
    currentDispMode = (currentDispMode + 1) % TOTAL_DISP_MODES;
    preferences.putUChar("disp_mode", currentDispMode);
    renderCurrentDisplayBase();
    showScreen(ensureMainScreen());
}

// 二级里选中项要执行的动作。
// 一级和二级共用一个入口：一级按回车 = 进入那一组（不算执行任何动作），
// 二级按回车 = 真正跑这个动作。
static void runMenuAction(MenuAction act) {
    switch (act) {
        case MA_CYCLE_DISP:      // 循环切换主屏风格
            cycleDisplayStyle();
            break;
        case MA_CYCLE_SAVER:     // 循环切换屏保风格（黑屏→壁纸→信息→关闭）
            cycleScreensaverMode();
            break;
        case MA_OPEN_LIGHT:      // 背光开关 / 亮度 / 灯效 / 状态灯亮度 / 按键灯效
            build_settings_light();
            break;
        case MA_OPEN_TIME:
            build_settings_time();
            break;
        case MA_OPEN_ALARM:
            build_settings_alarm();
            break;
        case MA_OPEN_TIMER:
            build_settings_timer();
            break;
        case MA_CYCLE_PROFILE:   // 循环切换配置方案
            switchProfile((currentProfile + 1) % TOTAL_PROFILES);
            break;
        case MA_REFRESH_SHT:
            if (shtAvailable) {
                sht31_update();
            } else {
                triggerHudBig("温湿度", "未找到", lv_color_hex(CLR_RED));
            }
            break;
        case MA_OPEN_CALTEMP:
            build_settings_caltemp();
            break;
        case MA_CLEAR_COUNTERS:
            totalKeyCount = 0;
            todayKeyCount = 0;
            preferences.putUInt("keyCount", 0);
            preferences.putUInt("today_key", 0);
            break;
        case MA_OPEN_ELOG:       // 错误日志：重启后在这里翻出错内容
            build_elog();
            break;
        case MA_OPEN_HELP:       // 按键速查：一张随时能翻的按键功能表
            build_help();
            break;
    }
}

static void handleMenuSelect(void) {
    // 一级：回车 = 进这一组
    if (!menuInSub) {
        menuInSub = true;
        menuItemSel = 0;
        menuScrollOffset = 0;
        build_menu();
        return;
    }
    // 二级：回车 = 执行
    uint8_t g = menuSel;
    if (g >= MENU_GROUP_COUNT) return;
    const MenuGroup* grp = &menuGroups[g];
    if (menuItemSel >= grp->count) return;
    runMenuAction(grp->items[menuItemSel].act);
}

// ===========================
// 构建：菜单（两级）
// ===========================
// 一级列 6 个大类，回车进去列该组的具体项。列表本身还是真滚动：
// 之前 yPos 恒等于 i*32、menuScrollOffset 只用来改透明度，滚到第二页时
// 可见窗口是第 1~6 项、y 落在 32~192 而容器只有 192 高，第 4/5/6 项被裁掉 ——
// 看起来就是"一翻页全黑"。现在 yPos 跟着 menuScrollOffset 走。
//
// 一级：分组列表。二级：该分组下的具体项。
static const MenuGroup* curMenuGroup(void) {
    if (menuSel >= MENU_GROUP_COUNT) return &menuGroups[0];
    return &menuGroups[menuSel];
}

// 当前层级一共几行
static uint8_t menuRowCount(void) {
    if (menuInSub) return curMenuGroup()->count;
    return MENU_GROUP_COUNT;
}

static uint8_t menuCurRow(void) {
    return menuInSub ? menuItemSel : menuSel;
}

// 菜单滚动相关
// 一级现在 7 行、二级最多 4 行，都比 MENU_VISIBLE_ITEMS(6) 大一点点，
// 所以滚动是真的会用上了：第 7 个大类靠 menuScrollOffset 滚进窗口。
static void build_menu(void) {
    uint8_t rows  = menuRowCount();
    uint8_t cur   = menuCurRow();

    // 计算滚动偏移（保证选中项始终在可见窗口内，且不越界）
    if ((int)cur < menuScrollOffset) {
        menuScrollOffset = cur;
    } else if ((int)cur >= menuScrollOffset + MENU_VISIBLE_ITEMS) {
        menuScrollOffset = cur - MENU_VISIBLE_ITEMS + 1;
    }
    int maxOffset = (int)rows - MENU_VISIBLE_ITEMS;
    if (maxOffset < 0) maxOffset = 0;
    if (menuScrollOffset > maxOffset) menuScrollOffset = maxOffset;
    if (menuScrollOffset < 0) menuScrollOffset = 0;

    const MenuGroup* grp = curMenuGroup();

    if (scr_menu == nullptr) {
        init_styles();
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr_menu, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr_menu, 0, LV_PART_MAIN);

        // ---- 顶部标题栏 ----
        menu_title = lv_label_create(scr_menu);
        mkLabel(menu_title, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(menu_title, "系统菜单");
        lv_obj_align(menu_title, LV_ALIGN_TOP_LEFT, 16, 12);

        // 标题左侧的强调色竖条
        lv_obj_t* tbar = lv_obj_create(scr_menu);
        lv_obj_set_size(tbar, 3, 14);
        lv_obj_align(tbar, LV_ALIGN_TOP_LEFT, 8, 13);
        lv_obj_set_style_bg_color(tbar, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(tbar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(tbar, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

        // 右上角页码胶囊
        lv_obj_t* pos_chip = lv_obj_create(scr_menu);
        lv_obj_set_size(pos_chip, 44, 20);
        lv_obj_align(pos_chip, LV_ALIGN_TOP_RIGHT, -16, 10);
        mkChip(pos_chip, CLR_SURFACE_2, CLR_TEXT_DIM, 10);

        menu_position = lv_label_create(pos_chip);
        mkLabel(menu_position, &lv_font_montserrat_12, CLR_TEXT_DIM);
        lv_label_set_text(menu_position, "1/6");
        lv_obj_center(menu_position);

        // ---- 列表容器（只做裁剪，不画底）----
        menu_cont = lv_obj_create(scr_menu);
        lv_obj_set_size(menu_cont, 216, MENU_VISIBLE_ITEMS * MENU_PITCH);
        lv_obj_align(menu_cont, LV_ALIGN_TOP_MID, 0, MENU_LIST_TOP);
        lv_obj_set_style_bg_opa(menu_cont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(menu_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(menu_cont, 0, LV_PART_MAIN);

        // ---- MENU_ROW_MAX 行（只建一次，之后只改位置、文字和配色）----
        // 建满上限、切换层级时只改可见性：既避免每次进出都整屏删建
        // （LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL），也省掉一层分支。
        for (int i = 0; i < MENU_ROW_MAX; i++) {
            lv_obj_t* btn = lv_obj_create(menu_cont);
            lv_obj_set_size(btn, 212, MENU_ITEM_H);
            mkCard(btn, CLR_SURFACE, 7);

            // 选中态左侧强调条。上下各留 3px（MENU_ITEM_H - 6），
            // 行高收到 24 之后不能再用 8 的留白，那会剩下 0px 上下边、顶到卡片边上。
            lv_obj_t* sel = lv_obj_create(btn);
            lv_obj_set_size(sel, 3, MENU_ITEM_H - 6);
            lv_obj_set_pos(sel, 0, 3);
            lv_obj_set_style_bg_color(sel, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sel, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(sel, 2, LV_PART_MAIN);
            lv_obj_set_style_border_width(sel, 0, LV_PART_MAIN);
            lv_obj_clear_flag(sel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* lbl = lv_label_create(btn);
            mkLabel(lbl, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(lbl, "");
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);

            // 右侧序号
            lv_obj_t* num = lv_label_create(btn);
            mkLabel(num, &lv_font_montserrat_12, CLR_TEXT_MUTE);
            lv_label_set_text(num, "");
            lv_obj_align(num, LV_ALIGN_RIGHT_MID, -12, 0);

            // 右侧当前值角标：一级放"该组有幾项"，二级放屏保模式 / 错误条数
            lv_obj_t* val = lv_label_create(btn);
            mkLabel(val, &lv_font_simsun_16_cjk, CLR_ACCENT);
            lv_label_set_text(val, "");
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, -34, 0);
            menu_items_val[i] = val;

            menu_items[i] = btn;
        }

        // ---- 底部两行按键提示（建一次，之后只改文案）----
        // 一级：回车进组、ESC 退出菜单
        // 二级：回车执行、ESC 回上一级
        menu_hint1 = mkHintLine(scr_menu, HINT_LINE1_Y, "↑↓ 选择 · 回车进入");
        menu_hint2 = mkHintLine(scr_menu, HINT_LINE2_Y, "ESC 返回主屏");
    }

    // ---- 标题：一级是"系统菜单"，二级带上组名，让人知道自己在哪 ----
    if (menu_title) {
        static char titleBuf[40];
        if (menuInSub) snprintf(titleBuf, sizeof(titleBuf), "%s", grp->cn);
        else           snprintf(titleBuf, sizeof(titleBuf), "系统菜单");
        setText(menu_title, titleBuf);
    }

    // ---- 每行的文字 ----
    for (int i = 0; i < MENU_ROW_MAX; i++) {
        lv_obj_t* btn = menu_items[i];
        if (btn == nullptr) continue;
        lv_obj_t* lbl = lv_obj_get_child(btn, 1);
        lv_obj_t* num = lv_obj_get_child(btn, 2);
        lv_obj_t* val = menu_items_val[i];

        if ((uint8_t)i >= rows) {
            // 这一行当前层级用不到
            setHidden(btn, true);
            if (val) setHidden(val, true);
            continue;
        }
        setHidden(btn, false);

        if (!menuInSub) {
            // 一级：组名 + 右侧"有幾项"
            setText(lbl, menuGroups[i].cn);
            static char cntBuf[8];
            snprintf(cntBuf, sizeof(cntBuf), "%d 项", menuGroups[i].count);
            setText(val, cntBuf);
            setHidden(val, false);
            lv_obj_set_style_text_color(val, lv_color_hex(CLR_TEXT_MUTE), LV_PART_MAIN);
        } else {
            MenuAction act = grp->items[i].act;
            setText(lbl, grp->items[i].cn);
            setText(num, "");
            // 只有屏保风格 / 错误日志这两项需要角标，其余留空
            setHidden(val, act != MA_CYCLE_SAVER && act != MA_OPEN_ELOG);
            if (act == MA_CYCLE_SAVER) {
                setText(val, saverMode < TOTAL_SAVER_MODES ? saverModeNames[saverMode] : "");
                lv_obj_set_style_text_color(val, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
            } else if (act == MA_OPEN_ELOG) {
                // 错误条数直接顶在右边：不用进日志页也知道有没有东西出过问题
                int errs = elog_error_count();
                static char ebuf[16];
                if (errs > 0) {
                    snprintf(ebuf, sizeof(ebuf), "%d 条", errs);
                    lv_obj_set_style_text_color(val, lv_color_hex(CLR_RED), LV_PART_MAIN);
                } else {
                    snprintf(ebuf, sizeof(ebuf), "无");
                    lv_obj_set_style_text_color(val, lv_color_hex(CLR_GREEN), LV_PART_MAIN);
                }
                setText(val, ebuf);
            }
        }
    }

    // ---- 底部提示跟着层级换 ----
    if (!menuInSub) {
        setHintText(menu_hint1, "↑↓ 选择 · 回车进入");
        setHintText(menu_hint2, "ESC 返回主屏");
    } else {
        setHintText(menu_hint1, "↑↓ 选择 · 回车执行");
        setHintText(menu_hint2, "ESC 返回上一级");
    }

    // ---- 更新页码 ----
    if (menu_position) {
        static char posBuf[16];
        snprintf(posBuf, sizeof(posBuf), "%d/%d", (int)cur + 1, (int)rows);
        setText(menu_position, posBuf);
    }

    // ---- 更新每项的位置（真滚动）与配色 ----
    for (int i = 0; i < MENU_ROW_MAX; i++) {
        lv_obj_t* btn = menu_items[i];
        if (btn == nullptr) continue;

        int slot = i - menuScrollOffset;                 // 在窗口里的第几行
        bool isVisible = (slot >= 0 && slot < MENU_VISIBLE_ITEMS) && ((uint8_t)i < rows);
        bool isSelected = (i == (int)cur);

        if (isVisible) lv_obj_set_pos(btn, 0, slot * MENU_PITCH);
        // 窗口外的行移出容器并隐藏（不删，重建成本高）
        lv_obj_set_style_opa(btn, isVisible ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);

        lv_obj_set_style_bg_color(btn,
            lv_color_hex(isSelected ? CLR_SURFACE_2 : CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn,
            lv_color_hex(isSelected ? CLR_ACCENT_D : CLR_STROKE), LV_PART_MAIN);

        // 左侧强调条：只有选中项点亮
        lv_obj_t* sel = lv_obj_get_child(btn, 0);
        if (sel) {
            lv_obj_set_style_bg_opa(sel,
                isSelected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        }
        // 文本 / 序号
        lv_obj_t* lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(isSelected ? CLR_TEXT : CLR_TEXT_DIM), LV_PART_MAIN);
        }
        lv_obj_t* num = lv_obj_get_child(btn, 2);
        if (num) {
            // 一级不显示序号（组序号没有意义），二级才显示组内序号
            if (!menuInSub) setText(num, "");
            else {
                static char numBuf[MENU_ROW_MAX][4];
                snprintf(numBuf[i], sizeof(numBuf[i]), "%02d", i + 1);
                setText(num, numBuf[i]);
            }
            lv_obj_set_style_text_color(num,
                lv_color_hex(isSelected ? CLR_ACCENT : CLR_TEXT_MUTE), LV_PART_MAIN);
        }
    }

    showScreen(scr_menu);
}

// ===========================
// HUD 浮层（操作反馈提示条）
// ===========================
//
// 分两档，这是"提示不醒目"和"提示太多"两个问题的解法。
//
//   大提示 triggerHudBig()   232×112  实心语义色块 + 黑字   2.4 秒
//       给**必须看见**的：出错、警告，以及键盘被锁 / 要重启这类状态翻转。
//       为什么做成实心色块：这块屏是纯黑底的，黑底卡片配 2px 细描边在
//       240×240 上只是"那儿有东西"，余光一扫就滑过去了；反过来，一块
//       实心色 + 黑字是这块屏上对比最强的组合，隔半米也能读出来。
//
//   小提示 triggerHud()      216×58   抬升面卡片 + 次级标题 / 强调数值   1.1 秒
//       给例行确认："已保存""已开启""静音切换"这类。
//       屏上本来就有反馈的（改了风格、拧了灯效、存了设置）**根本不该弹提示**
//       —— 那些调用点已经删掉了。
//
// 两条硬规矩：
//   1. 同一条提示重复触发时**只改文字、不重建卡片**。重建的话卡片在旋钮
//      连拧的时候会一闪一闪地抖（每转一格就 lv_obj_del + 建一遍）；
//      "在原地刷新"看起来就是一条稳定的提示。
//   2. 提示不叠多条。一条提示在屏幕上时，新的提示直接接管它 —— 叠卡片在
//      240×240 上只会互相压住，谁也看不清。
//
// ⚠ 字号的现实：中文只有 16px 一种字号（lv_conf.h 里只开了
//   lv_font_simsun_16_cjk），再大就得重生成一份子集字库，代价和收益不匹配。
//   所以"醒目"靠的是**色块、对比和尺寸**，不是字号。
//
// ⚠ 16px 中文字体的行高是 **19px**（= HINT_LINE_H，见 simsun_16_cjk 的
//   line_height）。两行文字要塞进一张卡，行距必须按 19 算，不能按 16 算 ——
//   按 16 算出来的排版**两行会重叠 1px**，看着就是"两行字粘在一起"。
//   这就是原来小提示（52px 高）难看的直接原因。
#define HUD_BIG_W     232
#define HUD_BIG_H     112
#define HUD_SMALL_W   216
#define HUD_SMALL_H    58
// 小提示卡内两行的顶边（相对卡片内容区）：
//   标题 9 → 墨迹 12~26
//   数值 32 → 墨迹 35~49
//   两行墨迹之间留 9px，卡底还剩 9px，上下对称。
#define HUD_SMALL_TITLE_Y  9
#define HUD_SMALL_VALUE_Y 32
#define HUD_BIG_MS    2400
#define HUD_SMALL_MS  1100

// HUD 的语义色要同时喂给两种接口：lv_obj_set_style_*_color() 收 lv_color_t，
// 而 mkLabel() 收的是 0xRRGGBB 的 uint32_t（它内部再套一层 lv_color_hex）。
// lv_color_to32() 返回的是 0xFFRRGGBB（带 alpha），直接当 0xRRGGBB 用会把
// 高 8 位的 0xFF 串到红通道上 —— 青色会变成玫红。必须把 alpha 摘掉。
static inline uint32_t hudRgb(lv_color_t c) {
    return lv_color_to32(c) & 0xFFFFFFUL;
}

// HUD 的子对象指针：为了"同一条提示只改文字、不重建卡片"。
static lv_obj_t* hudTitleLbl  = nullptr;
static lv_obj_t* hudValueLbl  = nullptr;
static lv_obj_t* hudAccentObj = nullptr;   // 大提示的实心数值块（小提示不再用它）

static void build_hud(void) {
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
    hudTitleLbl = nullptr; hudValueLbl = nullptr;
    hudAccentObj = nullptr;

    // ⚠⚠ 每一个 lv_obj_create / lv_label_create 都可能返回 NULL，必须逐个判。
    //
    // LVGL 的分配器是 lv_mem_port.h 里那个：优先 PSRAM，失败**回落 malloc**（内部
    // DRAM）。而内部 DRAM 正是最紧张的那块 —— BLE 协议栈、ArduinoJson、帧缓冲、
    // 任务栈全挤在这 327KB 里。所以"分配失败"不是理论风险，是这个项目历史上
    // 最主要的重启原因之一（见 lv_mem_port.h 顶部那段）。
    //
    // 一旦 scr_hud 是 NULL，下面那句 lv_obj_set_size(scr_hud, ...) 就会解引用空
    // 指针 → panic。**少一个 HUD 只是少一次提示，崩了是整机重启** ——
    // 这个代价对比决定了这里必须判。返回即可，hud.active 已经在
    // triggerHudTier() 里置过，loop 里照常会把它超时收掉。
    scr_hud = lv_obj_create(lv_layer_top());
    if (scr_hud == nullptr) return;

    if (hud.big) {
        // ---------------- 大提示 ----------------
        //
        // 2026-10-22 改版：卡片底从**纯黑**改成抬升面 CLR_SURFACE_2。
        // 纯黑（0x000000）压在页面底 CLR_BG（0x0B0F17）上，比背景还暗，
        // 看着像"在屏幕上挖了个洞"，再套一圈 4px 彩色描边就成了一个
        // 悬空的贴纸。抬升面比背景亮一圈，层次才对。
        //
        // 同时**去掉了顶部那条实心横条**：原来一个语义色在卡上出现四次
        // （4px 描边 / 顶部横条 / 标题文字 / 数值块），红红红红一片，
        // 反而没有重点。现在只留两处 —— 彩色描边（框）+ 实心数值块（内容），
        // 标题改成白字：白字对深底对比最高，"键盘已锁定"这种必须一眼看清的
        // 字就该是最白的那几个字。
        lv_obj_set_size(scr_hud, HUD_BIG_W, HUD_BIG_H);
        lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, -6);
        mkCard(scr_hud, CLR_SURFACE_2, 10);
        lv_obj_set_style_border_width(scr_hud, 4, LV_PART_MAIN);
        lv_obj_set_style_border_color(scr_hud, hud.color, LV_PART_MAIN);
        lv_obj_move_foreground(scr_hud);

        // 标题：白字。y=16 是相对卡片内容区（4px 描边之内）的顶边。
        hudTitleLbl = lv_label_create(scr_hud);
        if (hudTitleLbl) {
            mkLabel(hudTitleLbl, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(hudTitleLbl, hud.title);
            lv_obj_set_width(hudTitleLbl, HUD_BIG_W - 24);
            lv_label_set_long_mode(hudTitleLbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(hudTitleLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(hudTitleLbl, LV_ALIGN_TOP_MID, 0, 16);
        }

        // 数值：实心语义色块 + 黑字 —— 这块屏上对比最强的组合。
        // 块高 40（原来 34）：16px 中文字体行高 19，上下各留 10px 才不挤。
        hudAccentObj = lv_obj_create(scr_hud);
        if (hudAccentObj) {
            lv_obj_set_size(hudAccentObj, HUD_BIG_W - 32, 40);
            lv_obj_align(hudAccentObj, LV_ALIGN_BOTTOM_MID, 0, -16);
            lv_obj_set_style_bg_color(hudAccentObj, hud.color, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(hudAccentObj, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(hudAccentObj, 6, LV_PART_MAIN);
            lv_obj_set_style_border_width(hudAccentObj, 0, LV_PART_MAIN);
            lv_obj_clear_flag(hudAccentObj, LV_OBJ_FLAG_SCROLLABLE);

            hudValueLbl = lv_label_create(hudAccentObj);
            if (hudValueLbl) {
                mkLabel(hudValueLbl, &lv_font_simsun_16_cjk, 0x000000);
                lv_label_set_text(hudValueLbl, hud.value);
                lv_obj_set_width(hudValueLbl, HUD_BIG_W - 44);
                lv_label_set_long_mode(hudValueLbl, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(hudValueLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_center(hudValueLbl);
            }
        }
    } else {
        // ---------------- 小提示 ----------------
        //
        // 2026-10-22 改版（用户："这个提示还是有点丑，比如静音控制这种的弹框"）。
        // 原来那版的三个毛病：
        //
        //   1. 卡片是**纯黑**（0x000000）压在 CLR_BG（0x0B0F17）上 —— 比页面底
        //      还暗，看着不像卡片，像挖了个洞；再套 2px 高饱和描边，整张卡
        //      就成了一个悬空的**线框框**，而不是一块"浮起来的提示"。
        //      → 改成抬升面 CLR_SURFACE_2 + 1px 中性描边（CLR_STROKE）：
        //        卡片比页面亮一圈，一眼看出是浮起来的一块。
        //
        //   2. **两行文字重叠了 1px**。卡高 52，标题在 y=6、数值在 y=-5，
        //      而 16px 中文字体行高是 19（不是 16）—— 标题占 6~25、数值占
        //      24~43，正好压掉 1px，看着就是"两行字粘在一起"。
        //      → 卡高改 58，两行顶边定死 9 / 32（行盒之间留 4px，
        //        墨迹之间留 9px），卡底还剩 9px，上下对称。
        //
        //   3. 强调色用了三次（2px 描边 + 左侧竖条 + 标题文字），花。
        //      而且文字是**居中**的，左边那条竖条孤零零杵在左边，离文字
        //      十万八千里，看着像个没对齐的零件。
        //      → 竖条整个去掉，强调色只留给**数值文字**（那才是要读的半句），
        //        标题退成次级色 CLR_TEXT_DIM。
        //
        // 值和标题必须分两行：中文 16px，并一行的话"已收回屏幕，仍在计时"
        // （实测 160px）加标题就超了 216px 的卡宽 —— 截掉的恰恰是最该看的那半句。
        lv_obj_set_size(scr_hud, HUD_SMALL_W, HUD_SMALL_H);
        lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, 40);
        mkCard(scr_hud, CLR_SURFACE_2, 12);
        lv_obj_move_foreground(scr_hud);

        // 标题：次级色。它是"这条提示属于哪一类"（静音控制 / 灯效切换），
        // 不是用户要读的那半句，退到次级色才不跟数值抢。
        hudTitleLbl = lv_label_create(scr_hud);
        if (hudTitleLbl) {
            mkLabel(hudTitleLbl, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
            lv_label_set_text(hudTitleLbl, hud.title);
            lv_obj_set_width(hudTitleLbl, HUD_SMALL_W - 28);
            lv_label_set_long_mode(hudTitleLbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(hudTitleLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(hudTitleLbl, LV_ALIGN_TOP_MID, 0, HUD_SMALL_TITLE_Y);
        }

        // 数值：语义色。绿=成功 / 红=出错 / 琥珀=警告，颜色跟着语义走，
        // 而且**只有这一处**用色，所以颜色变了就是内容变了，一眼能分辨。
        hudValueLbl = lv_label_create(scr_hud);
        if (hudValueLbl) {
            mkLabel(hudValueLbl, &lv_font_simsun_16_cjk, hudRgb(hud.color));
            lv_label_set_text(hudValueLbl, hud.value);
            lv_obj_set_width(hudValueLbl, HUD_SMALL_W - 28);
            lv_label_set_long_mode(hudValueLbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(hudValueLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(hudValueLbl, LV_ALIGN_TOP_MID, 0, HUD_SMALL_VALUE_Y);
        }
    }
}

// 收掉当前提示（连卡片一起删）
static void hudClear(void) {
    hud.active = false;
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
    hudTitleLbl = nullptr; hudValueLbl = nullptr;
    hudAccentObj = nullptr;
}

// 把新的标题/值刷到已经建好的卡片上。
// 走这条路而不是重建，是为了让"连着按同一个键""连着拧旋钮"看起来是**一条**
// 稳定的提示，而不是卡片反复闪。
static void hudRefreshText(void) {
    if (scr_hud == nullptr) return;
    if (hudTitleLbl) lv_label_set_text(hudTitleLbl, hud.title);
    if (hudValueLbl) lv_label_set_text(hudValueLbl, hud.value);
    // 语义色也跟着换：绿色的"已保存"被红色的"保存失败"顶掉时，
    // 颜色得跟着变，否则颜色和内容对不上。
    //
    // ⚠ 两档的"颜色放在哪"不一样，**不能共用一套刷新逻辑**：
    //   大提示：语义色在**描边**和**实心数值块**上（标题是白字）。
    //   小提示：语义色只在**数值文字**上，卡片描边保持中性 CLR_STROKE。
    //     小提示以前也有彩色描边，但那时候它还配着左侧竖条和彩色标题，
    //     一次改动三处全变，看着像"闪了一下"；现在只剩文字变色，
    //     卡片骨架是稳的，换色只影响读的那半句话。
    if (hud.big) {
        lv_obj_set_style_border_color(scr_hud, hud.color, LV_PART_MAIN);
        if (hudAccentObj) lv_obj_set_style_bg_color(hudAccentObj, hud.color, LV_PART_MAIN);
    } else {
        if (hudValueLbl) {
            lv_obj_set_style_text_color(hudValueLbl, lv_color_hex(hudRgb(hud.color)), LV_PART_MAIN);
        }
    }
}

// 语义色里，红色=出错、琥珀=警告 —— 这两类必须一眼看见；
// 绿/青/紫/灰是"成功或例行"，小提示就够。
//
// ⚠ 比的是 **RGB565 里的原始值**（.full），不是 RGB888。
//   LV_COLOR_DEPTH=16，0xF87171 存进 RGB565 再取出来已经不是原来那个数了
//   （会差 1~2），直接比 RGB888 会永远不相等 —— 那样所有错误提示都会被
//   当成例行消息降级成小提示，正好把最该看见的那类藏起来。
static bool hudUrgent(lv_color_t c) {
    return c.full == lv_color_hex(CLR_RED).full
        || c.full == lv_color_hex(CLR_AMBER).full;
}

// 提示的统一入口。big=true 走大提示，false 走小提示。
static void triggerHudTier(bool big, const char* title, const char* value, lv_color_t color) {
    // 通知面板在场时让路：通知是 228×196 的大卡片，HUD 压在它下面两边都看不清。
    if (notifCount > 0) return;

    // 同一档位的提示还在屏幕上 → 只改文字 + 续命，不重建卡片。
    if (hud.active && hud.big == big && scr_hud != nullptr) {
        strncpy(hud.title, title, sizeof(hud.title) - 1);
        hud.title[sizeof(hud.title) - 1] = '\0';
        strncpy(hud.value, value, sizeof(hud.value) - 1);
        hud.value[sizeof(hud.value) - 1] = '\0';
        hud.color = color;
        hud.startMs = millis();
        hudRefreshText();
        return;
    }

    hud.active = true;
    hud.big = big;
    strncpy(hud.title, title, sizeof(hud.title) - 1);
    hud.title[sizeof(hud.title) - 1] = '\0';
    strncpy(hud.value, value, sizeof(hud.value) - 1);
    hud.value[sizeof(hud.value) - 1] = '\0';
    hud.color = color;          // 原样存 lv_color_t，别再 lv_color_to32 来回转
    hud.startMs = millis();
    hud.showMs = big ? HUD_BIG_MS : HUD_SMALL_MS;

    build_hud();
}

// 小提示：例行确认（"已保存""已开启"这类），1.1 秒。
static void triggerHud(const char* title, const char* value, lv_color_t color) {
    triggerHudTier(false, title, value, color);
}

// 大提示：必须看见的（出错、警告、键盘被锁、正在重启这类），2.4 秒。
static void triggerHudBig(const char* title, const char* value, lv_color_t color) {
    triggerHudTier(true, title, value, color);
}

// ===========================
// 通知系统实现
// ===========================
//
// 正文兜底：主机只发颜色、不带文字（就是 `ALERT:RED`）时，notifTexts 里存空串，
// 由 drawNotifPanel() 现场合成「你有 N 条消息」—— 条数会随确认一条条变，
// 必须绘制时才算，不能像固定文案那样入队时就写死。
// 之前这里是回一个英文 "Alert"，屏上就俩英文字母，等于什么都没告诉人。
//
// 主机带了正文（`ALERT:RED:磁盘不足`）就以主机说的为准，这里完全不介入。
// 想挑一句更具体的话，让 PC 端用 kbctl_hid.py alert <颜色> <描述> 明确下发。
// 常用告警文案库。主机只发颜色、不带正文（`ALERT:RED`）时，从这里轮换取一句，
// 免得屏上只有一个"你有 N 条消息"这种废话。
// 这张表和 PC 端 kbctl_hid.py 里的 ALERT_PRESETS 是一一对应的，改一处记得改另一处。
//
// 之前这里只有一句注释说"从常用文案库里挑一句"，实际实现里根本没这张表：
// pushNotification() 存空串、drawNotifPanel() 兜底成「你有 N 条消息」。
// 所以网页上那三个 🚨/🟢/⚠️ 按钮（只发颜色）按下去，展示框里永远只有那一句，
// 主机自己带的说明文字（`ALERT:RED:磁盘空间不足`）才显示得出来。
static const char* const ALERT_PRESET_RED[] = {
    "构建失败", "磁盘空间不足", "服务已宕机", "内存溢出",
    "网络连接中断", "CI 流水线失败", "磁盘写入错误", "进程异常退出",
};
static const char* const ALERT_PRESET_YELLOW[] = {
    "服务器无响应", "CPU 温度偏高", "电量不足", "磁盘即将写满",
    "测试未通过", "证书即将过期", "队列积压", "同步冲突待处理",
};
static const char* const ALERT_PRESET_GREEN[] = {
    "部署完成", "构建通过", "备份已完成", "任务已结束",
    "依赖已更新", "测试全部通过", "文件已同步", "服务已恢复",
};

// 按颜色轮换着取，同一颜色连按两次给两句不同的，不会一直重复。
static const char* nextAlertPreset(AlertType type) {
    static uint8_t idxRed = 0, idxYellow = 0, idxGreen = 0;
    switch (type) {
        case ALERT_RED:    return ALERT_PRESET_RED[(idxRed++)    % (sizeof(ALERT_PRESET_RED)    / sizeof(char*))];
        case ALERT_YELLOW: return ALERT_PRESET_YELLOW[(idxYellow++) % (sizeof(ALERT_PRESET_YELLOW) / sizeof(char*))];
        case ALERT_GREEN:  return ALERT_PRESET_GREEN[(idxGreen++)  % (sizeof(ALERT_PRESET_GREEN)  / sizeof(char*))];
        default:           return nullptr;
    }
}

static void pushNotification(AlertType type, const String& text) {
    if (type == ALERT_NONE) return;
    if (notifCount >= MAX_NOTIFS) {
        // 移除最老的通知
        for (int i = 0; i < notifCount - 1; i++) {
            notifQueue[i] = notifQueue[i + 1];
            strncpy(notifTexts[i], notifTexts[i + 1], 127);
        }
        notifCount--;
    }

    notifQueue[notifCount] = type;
    if (text.length() > 0) {
        strncpy(notifTexts[notifCount], text.c_str(), 127);   // 主机带了正文，以主机说的为准
    } else {
        const char* preset = nextAlertPreset(type);
        if (preset) strncpy(notifTexts[notifCount], preset, 127);
        else        notifTexts[notifCount][0] = '\0';
    }
    notifTexts[notifCount][127] = '\0';
    notifCount++;

    drawNotifPanel();
}

// 通知面板：居中大卡片。
// 交互模型（用户定的）：来一条就显示最新那条 + 灯按它的颜色闪；用户没处理又来一条，
// 屏幕切到新消息、灯换色，但"还有几条没处理"要一直摆着；按一次静音/灯光键 = 掉最新一条，
// 队列里前一条自动顶上继续显示、继续闪；全清完灯自然灭。所以这里只画"队尾 + 剩余条数"。
//
// 之前是贴在顶部的 58px 小条：存在感太弱，人扫一眼就过去了 —— 但这东西的价值
// 全在"别漏掉"，所以改成压在屏幕正中，数字和正文都放大。
// 正文用字体本身的 16px 排满卡片内宽（最多 5 行）；**不要**再用 transform_zoom
// 放大，原因见下面创建 notif_label 处的注释。
static void drawNotifPanel(void) {
    if (notifCount == 0) {
        if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }
        return;
    }

    // 删除旧的通知面板
    if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }

    // 通知在场时把 HUD 收掉：通知是 228×196 的大卡片，HUD 压在它下面
    // 两边都看不清，两张卡叠着比只有一张更糟。通知比提示重要。
    hudClear();

    // 语义色：错误=红 / 正常=绿 / 警告=琥珀 / 普通=青
    // 只用颜色表示，**不写"错误/正常/警告"这些字**：一条告警到底算不算错误
    // 取决于主机那边在干什么，键盘擅自下判断经常是错的（CI 变绿、构建变红都很常见）。
    // 文字留给消息正文自己说。
    AlertType latestType = notifQueue[notifCount - 1];
    uint32_t accent;
    switch (latestType) {
        case ALERT_RED:    accent = CLR_RED;   break;
        case ALERT_GREEN:  accent = CLR_GREEN; break;
        case ALERT_YELLOW: accent = CLR_AMBER; break;
        default:           accent = CLR_ACCENT; break;
    }

    // 纯黑底 + 2px 语义描边，居中 228x196（屏是 240x240）
    // 之前左边还挂了一条 5px 竖条，现在去掉了：描边本身已经带语义色，
    // 竖条只是把正文挤窄一截，告警文字才是这块屏上真正要看的东西。
    scr_notif = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_notif, 228, 196);
    lv_obj_center(scr_notif);
    mkCard(scr_notif, 0x000000, 14);
    lv_obj_set_style_border_width(scr_notif, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_notif, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_move_foreground(scr_notif);

    // 左上角：同色圆点。只表示"这条是红/绿/黄"，不写"错误/正常/警告"
    // —— 一条通知算不算错误取决于主机上下文，键盘擅自下判断经常是错的。
    // 文字留给消息正文自己说。
    lv_obj_t* kindDot = lv_obj_create(scr_notif);
    lv_obj_set_size(kindDot, 16, 16);
    lv_obj_set_style_radius(kindDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kindDot, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kindDot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kindDot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(kindDot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(kindDot, LV_ALIGN_TOP_LEFT, 18, 15);

    // 右上角：还剩几条待处理。之前这条信息占了整个顶部一行 + 一个 28 号大数字，
    // 逼得正文只剩 138 宽；现在收成一行，数字用 20 号大字体单独拎出来，
    // "1 条" 也一眼看得见（单条的时候最容易被当成没有条数）。
    //
    // 顺序：数字在前（读"1"先入眼），后接"条待处理"。文字锚在最右，数字往左贴，
    // 整体显示成「1 条待处理」。
    char nbuf[8];
    snprintf(nbuf, sizeof(nbuf), "%d", notifCount);

    lv_obj_t* cntLbl = lv_label_create(scr_notif);
    mkLabel(cntLbl, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(cntLbl, "条待处理");
    lv_obj_align(cntLbl, LV_ALIGN_TOP_RIGHT, -18, 12);

    lv_obj_t* cntNum = lv_label_create(scr_notif);
    mkLabel(cntNum, &lv_font_montserrat_20, accent);
    lv_label_set_text(cntNum, nbuf);
    // 数字贴到"条待处理"左侧,垂直略下移,让数字基线与文字下半段对齐
    lv_obj_align_to(cntNum, cntLbl, LV_ALIGN_OUT_LEFT_TOP, -3, 3);

    // ---- 分隔线 ----
    lv_obj_t* sep = lv_obj_create(scr_notif);
    lv_obj_set_size(sep, 196, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(sep, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 主体：最新一条的告警正文 ----
    // 主机没给正文时（`ALERT:RED`）不留空白，合成一句「你有 N 条消息」——
    // 条数每次确认都会变，所以在这里现算，不能入队时写死。
    char fallback[32];
    const char* body = notifTexts[notifCount - 1];
    if (body[0] == '\0') {
        snprintf(fallback, sizeof(fallback), "你有 %d 条消息", (int)notifCount);
        body = fallback;
    }

    // **这里绝对不要再用 transform_zoom 放大字号。**
    //
    // 以前正文是"16px 拉到 1.5 倍"（transform_zoom = 384），思路是用缩放冒充大字，
    // 但那会让这个 label 走 LVGL 的**中间图层**渲染路径，而 LVGL 8.4 的软件渲染器
    // 在这条路上有一道闸门（lvgl/src/draw/sw/lv_draw_sw_layer.c 第 45 行）：
    //     if(LV_COLOR_SCREEN_TRANSP == 0 && (flags & LV_DRAW_LAYER_FLAG_HAS_ALPHA))
    //         return NULL;
    // 本项目 lv_conf.h 没开 LV_COLOR_SCREEN_TRANSP（默认 0），于是图层建不出来，
    // refr_obj() 直接 return —— **这个 label 一个像素都不画**。
    // 表现就是通知面板弹出来了（圆点、条数、"按灯光键处理"都在，它们都不走图层），
    // 唯独正文是空的，主机发什么文字都一样。
    //
    // 现在改回原生 16px 排版：宽度给满卡片内宽，行距收紧，能放 5 行。
    // 字库那边是 3500 常用汉字 + 源码补字的全量字库，任意汉字正文都出得来，
    // 所以"直接把字排大"这条路走不通时（240x240 放不下 24px 全量字库），
    // 靠"排满一屏"来保证可读性才是稳的。
    notif_label = lv_label_create(scr_notif);
    mkLabel(notif_label, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(notif_label, body);
    lv_obj_set_width(notif_label, 200);        // 卡片内宽 224（228 减两侧 2px 描边），各留 12
    lv_label_set_long_mode(notif_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(notif_label, 4, LV_PART_MAIN);
    lv_obj_align(notif_label, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_height(notif_label, 112);       // 5 行 × (16 字高 + 4 行距) = 100

    // ---- 底部：操作提示（只提灯光键，这是实际会按的那个） ----
    lv_obj_t* hint = lv_label_create(scr_notif);
    mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(hint, "按灯光键处理");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    // notifStartMs 记录"最新一条通知是什么时候到的"，兜底超时用（见 NOTIF_SAFETY_MS）
    notifStartMs = millis();
    notifShowMs = 3000;
}

static void clearNotifications(void) {
    notifCount = 0;
    if (scr_notif) { lv_obj_del(scr_notif); scr_notif = nullptr; }
}

// 通知不再"弹 3 秒自己消失"了。红绿灯告警的价值就在于灯一直闪到人处理为止，
// 3 秒后自动消失等于什么都没发生 —— 这也是 s3.ino 原版的做法：通知常驻，
// 按静音键逐条确认。现在只有一道兜底：120 秒没人碰就整队清掉，
// 免得键盘放一晚上红灯闪到天亮。
#define NOTIF_SAFETY_MS 120000UL

// 当前生效的告警永远取队尾（最新）那条。屏幕面板和灯光共用这一个来源，
// 不会出现"屏上显示黄的、灯在闪红的"这种各说各话。
static AlertType currentAlertType(void) {
    return (notifCount > 0) ? notifQueue[notifCount - 1] : ALERT_NONE;
}

// 用户按静音键 = "我已知晓"。只掉最新的一条，前一条自动顶上继续显示，
// 灯也跟着换成前一条的颜色；全清完之后灯自然就不闪了。
// 返回 false 表示当前没有待处理通知，调用方据此决定要不要给别的反馈。
static bool acknowledgeAlert(void) {
    if (notifCount == 0) return false;

    notifCount--;
    drawNotifPanel();

    // 不再弹"已确认"：通知卡片自己消失就是反馈，屏上多压一条提示
    // 只会挡住下一条通知。
    return true;
}

// ===========================
// 响铃：闹钟和倒计时到点共用
// ===========================
// 卡片是**常驻**的，不自动消失 —— 闹钟的价值全在"别漏掉"，弹 3 秒就没了
// 等于什么都没发生。停铃只有两个键：灯光键和静音键（用户定的），
// 别的键照常发给主机，不吃。
//
// 60 秒没人理会会自动停铃，但会留一条通知在队列里（"闹钟未确认"），
// 不这么兜底的话一排黄灯能闪一整晚。
#define RING_SAFETY_MS 60000UL

// 颜色按通道各自压暗（用于响铃卡片描边的"暗"那一半）。
//
// ⚠ **不能写成 `hex >> 2`**。0xRRGGBB 是一个打包整数，整体右移会让位从
// 通道边界穿过去：琥珀 0xFBBF24 右移 2 位得到 0x3EEFC9 —— 绿 191 变 239、
// 蓝 36 变 201，颜色从琥珀变成青，闪的时候一眼就看得出不对。
// 必须先拆出三个 8 位通道、各自移位、再重新打包。
static uint32_t dimRGB(uint32_t hex, uint8_t shift) {
    uint8_t r = (uint8_t)((hex >> 16) & 0xFF);
    uint8_t g = (uint8_t)((hex >> 8) & 0xFF);
    uint8_t b = (uint8_t)(hex & 0xFF);
    return ((uint32_t)(r >> shift) << 16) | ((uint32_t)(g >> shift) << 8) | (b >> shift);
}

static void startRinging(RingKind kind) {
    if (kind == RING_NONE) return;
    // 已经在响了就别打断，也别把兜底计时重置掉
    if (ringingKind != RING_NONE) return;

    ringingKind = kind;
    ringStartMs = millis();
    ringBlinkOn = true;

    // 响铃比 HUD 重要：先把浮层让出来，别让一条提示压在响铃上面
    hudClear();

    // 屏幕可能正息着。只闪灯不看屏等于没提醒，所以把屏幕叫醒。
    lastActivityTime = millis();
    if (currentSysMode == SYS_MODE_SLEEP) {
        currentSysMode = SYS_MODE_NORMAL;
        gotoMainScreen();
    }

    // 倒计时全屏让位给响铃：倒计时已经跑完了，这块屏本来就要收掉
    countdownShown = false;
    destroyCountdownScreen();
}

static void stopRinging(void) {
    if (ringingKind == RING_NONE) return;
    ringingKind = RING_NONE;
    if (ring_win) { lv_obj_del(ring_win); ring_win = nullptr; }
    ring_lbl_title = nullptr;
    ring_lbl_big = nullptr;
}

// 响铃卡片。描边每 500ms 亮/暗翻一次 —— 屏幕在闪 + 整排灯在闪，
// 两个一起才够"闹钟"的份量；只有一个的话很容易当成普通通知划过去。
static void drawRingOverlay(void) {
    if (ringingKind == RING_NONE) {
        if (ring_win) { lv_obj_del(ring_win); ring_win = nullptr; }
        return;
    }

    // 兜底超时：到点没人理就停铃，但留一条通知，不算静默丢失
    if (millis() - ringStartMs > RING_SAFETY_MS) {
        const char* what = (ringingKind == RING_ALARM) ? "闹钟未确认" : "倒计时未确认";
        stopRinging();
        pushNotification(ALERT_YELLOW, what);
        return;
    }

    // 描边翻转只需要改颜色，但整张卡重建也只有 5 个控件，量很小；
    // 换来的是只有一条绘制路径，不会出现"某次重建漏了某个指针"的野指针问题。
    static unsigned long lastBlinkMs = 0;
    static bool dirty = true;
    unsigned long nowMs = millis();
    if (nowMs - lastBlinkMs >= 500) {
        lastBlinkMs = nowMs;
        ringBlinkOn = !ringBlinkOn;
        dirty = true;
    }
    if (!dirty && ring_win) return;
    dirty = false;

    if (ring_win) { lv_obj_del(ring_win); ring_win = nullptr; }

    const bool isAlarm = (ringingKind == RING_ALARM);
    const uint32_t accent = CLR_AMBER;

    // 纯黑底 + 3px 琥珀描边，228x196 居中（屏 240x240）。
    // 描边亮时满色、暗时压到 1/4，肉眼就是"一闪一闪"。
    ring_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ring_win, 228, 196);
    lv_obj_center(ring_win);
    mkCard(ring_win, 0x000000, 14);
    lv_obj_set_style_border_width(ring_win, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring_win,
        lv_color_hex(ringBlinkOn ? accent : dimRGB(accent, 2)), LV_PART_MAIN);
    lv_obj_move_foreground(ring_win);

    // 标题：就是用户要的那几个字
    ring_lbl_title = lv_label_create(ring_win);
    mkLabel(ring_lbl_title, &lv_font_simsun_16_cjk, accent);
    lv_label_set_text(ring_lbl_title, isAlarm ? "闹钟响了" : "倒计时结束");
    lv_obj_align(ring_lbl_title, LV_ALIGN_TOP_MID, 0, 26);

    // 中间大字。中文只有 16px 子集字库（见 README「改完界面文案后必须重生成字体」），
    // 放不大，所以"大"这件事交给 48 号数字：闹钟报本该几点响，倒计时报刚才设了多久。
    char big[24];
    if (isAlarm) {
        snprintf(big, sizeof(big), "%02u:%02u", (unsigned)alarmHour, (unsigned)alarmMinute);
    } else if (timerTotalSec >= 3600) {
        snprintf(big, sizeof(big), "%02u:%02u:%02u",
                 (unsigned)(timerTotalSec / 3600),
                 (unsigned)((timerTotalSec / 60) % 60),
                 (unsigned)(timerTotalSec % 60));
    } else {
        snprintf(big, sizeof(big), "%02u:%02u",
                 (unsigned)(timerTotalSec / 60), (unsigned)(timerTotalSec % 60));
    }
    ring_lbl_big = lv_label_create(ring_win);
    mkLabel(ring_lbl_big, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(ring_lbl_big, big);
    lv_obj_align(ring_lbl_big, LV_ALIGN_CENTER, 0, 6);

    // 底部：只提真正能停铃的那两个键
    lv_obj_t* hint = lv_label_create(ring_win);
    mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(hint, "按灯光键或静音键停止");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

// 删一块屏之前，先确认它不是**当前正在显示**的那块。
//
// LVGL 8.4 的 lv_obj_del() 删掉的正好是活动屏时，会把 disp->act_scr 直接置成
// NULL（lv_obj_tree.c 里那句 `if(act_scr_del) disp->act_scr = NULL;`），
// 之后 lv_scr_act() 一直返回 NULL，任何解引用它的操作都是野指针 panic → 重启。
// 本项目已经为这个坑栽过两次（swapScreen / destroyMainScreen），这是第三处。
//
// 判据必须用 lv_scr_act()，**不能用 currentScreen**：
// destroyMainScreen() 结尾会把 currentScreen 无脑置成 nullptr，可它临时造的
// blank 屏还活着并且正在显示。那时候 currentScreen 是 nullptr，
// "currentScreen == 我要删的屏" 这个判断恒为假，可实际在显示的偏偏就是它
// —— 于是本该先换屏的动作被跳过，直接把活动屏删了。
// 问 LVGL「当前活动的是谁」才是准的。
//
// 返回值：换屏用的替身屏（调用方负责记进 currentScreen）。
static lv_obj_t* swapAwayIfActive(lv_obj_t* doomed, lv_obj_t* replacement) {
    if (doomed == nullptr) return nullptr;
    if (lv_scr_act() != doomed) return nullptr;
    if (replacement == nullptr || replacement == doomed) {
        replacement = ensureMainScreen();
    }
    if (replacement == doomed) return nullptr;   // 无处可退，交给调用方处理
    currentScreen = replacement;
    lv_scr_load(replacement);
    return replacement;
}

// ===========================
// 倒计时全屏
// ===========================
// 用户的要求很直白：「全屏幕就是展示这一个倒计时」，所以这块屏上
// **只有那一个数字** —— 连进度条、"按灯光键退出"都不加。要看提示去 README，
// 加了提示它就变成另一块主屏，那就不是用户要的东西了。
static void destroyCountdownScreen(void) {
    if (scr_countdown) {
        swapAwayIfActive(scr_countdown, nullptr);
        if (currentScreen == scr_countdown) currentScreen = nullptr;
        lv_obj_del(scr_countdown);
        scr_countdown = nullptr;
    }
    cd_lbl_main = nullptr;
}

static void updateCountdownScreenText(void) {
    if (!scr_countdown || !cd_lbl_main) return;
    char buf[24];
    if (timerRemainSec >= 3600) {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                 (unsigned)(timerRemainSec / 3600),
                 (unsigned)((timerRemainSec % 3600) / 60),
                 (unsigned)(timerRemainSec % 60));
    } else {
        snprintf(buf, sizeof(buf), "%02u:%02u",
                 (unsigned)(timerRemainSec / 60), (unsigned)(timerRemainSec % 60));
    }
    setText(cd_lbl_main, buf);
}

static void buildCountdownScreen(void) {
    if (scr_countdown) {
        // 同样先换屏再删，别裸着 lv_obj_del 一块可能正在显示的屏
        swapAwayIfActive(scr_countdown, nullptr);
        if (currentScreen == scr_countdown) currentScreen = nullptr;
        lv_obj_del(scr_countdown);
        scr_countdown = nullptr;
    }

    scr_countdown = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_countdown, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_countdown, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr_countdown, LV_OBJ_FLAG_SCROLLABLE);

    // 48 号 Montserrat 里数字宽 28~32px、冒号约 11px。
    // 最坏情况 "88:88:88" = 6 数字 + 2 冒号 ≈ 203px，240px 的屏放得下；
    // 不满 1 小时只排 "MM:SS"（约 142px），字大到隔着半张桌子也看得清。
    cd_lbl_main = lv_label_create(scr_countdown);
    mkLabel(cd_lbl_main, &lv_font_montserrat_48, CLR_TEXT);
    lv_label_set_text(cd_lbl_main, "--:--");
    lv_obj_center(cd_lbl_main);

    showScreen(scr_countdown);
    // 起跑那一瞬间就要出数字，不能等下一秒的节拍
    updateCountdownScreenText();
}

// 这块屏什么时候该在、什么时候该让位，一次说清：
//   允许接管（countdownShown）且倒计时在跑 → 盖在主屏上
//   进了菜单/任一设置页/录制/日志           → 让位，别把人困在倒计时里出不来
//   响铃中                                 → 让位（响铃卡片是浮层，优先级更高）
//   息屏                                   → 让位给屏保
static void updateCountdownVisibility(void) {
    // ⚠ 锁定期间**不许**接管显示。
    //
    // 少了这一条会踩一个很难察觉的坑：倒计时在跑、currentSysMode 又是 NORMAL 时，
    // wantIt 恒为真，而 currentScreen 是锁屏 ≠ scr_countdown，于是
    // buildCountdownScreen() 会**每轮 loop 都重建一次**倒计时屏
    // （lv_obj_del + lv_obj_create），而 showScreen() 又被锁定拦掉、屏上根本看不到。
    // 结果就是：锁着的时候白烧 CPU，PSRAM 被反复申请/释放，屏还是锁屏。
    //
    // 倒计时本身**照跑**（timerRunning 不受影响），解锁之后 wantIt 重新为真，
    // 倒计时屏会自己回来 —— 锁只锁"打字"，不锁后台任务。
    const bool wantIt = countdownShown && timerRunning
                        && (ringingKind == RING_NONE)
                        && (currentSysMode == SYS_MODE_NORMAL)
                        && !kbLocked;
    if (!wantIt) {
        if (scr_countdown) destroyCountdownScreen();
        return;
    }
    if (currentScreen != scr_countdown) buildCountdownScreen();
    else                               updateCountdownScreenText();
}

// ===========================
// 键盘锁定（双击静音键 = 锁定，Fn + LOGO = 解锁）
// ===========================
//
// 用户要的行为：
//   · 双击 C3 上的静音键 → 锁定键盘
//   · 锁住时整屏一把大锁，下面一行 "YYQ 已锁定"（"已锁定"是红色）
//   · 锁住时按任何键都**不生效**（一律不发主机），但屏幕要像高警一样：
//     红色边框闪一下，告诉用户"键我收到了，只是被锁着"
//   · 再按 Fn + LOGO 解锁
//
// ⚠ 锁屏上**不写**"怎么解锁"：会解锁的人本来就知道，写在屏上等于给旁边
//   的人指路。解锁方式只写在菜单的「按键速查」里 —— 那是给机主看的，
//   而锁屏是给"机主之外的人"看的。
//
// 三个容易踩的点（各自在下面有详细注释）：
//   1. 锁定状态**不进** currentSysMode（见上面 kbLocked 的注释）
//   2. 锁定期间**只有锁屏能上屏**（统一挡在 showScreen() 里）
//   3. 锁定判定必须放在**同键连发探测之前**（见按键分发那段注释）
//
// 锁定状态**落 NVS**：重启之后仍然是锁着的。
//   锁的意义就是"不让别人打字"，要是重启就自动解锁，拔电重插就能绕过，
//   这锁形同虚设。解锁入口有两个：Fn + LOGO、网页/BLE 的 LOCK: 指令。

#define NVS_KEY_LOCKED      "kb_locked"
#define MUTE_DOUBLE_TAP_MS  350      // 双击判定窗口（和 CPG 双击同量级）
#define LOCK_FLASH_MS       700      // 红框一共闪多久
#define LOCK_FLASH_HALF_MS  90       // 红框一亮一灭各多久

// 锁屏排版。坐标和 mock_lock.py 里的完全一致 —— 改了这里也要改那个脚本，
// 不然电脑上画出来的和屏上不一样，排查起来纯属浪费。
//
// 整块内容是**垂直居中**的：大锁 176px + 8px 缝 + 文字 19px = 203px，
// 上下各留 (240-203)/2 ≈ 18px。
// 之前底部还挂着一行"Fn + LOGO 解锁"提示，去掉那一行之后如果不动坐标，
// 底下会空出一大块，锁和字全挤在上半屏、看着像没排完。
#define LOCK_IMG_Y          18       // 大锁
#define LOCK_TXT_Y          202      // "YYQ 已锁定"

static lv_img_dsc_t lockImgDsc;              // 喂给 lv_img_set_src 的描述符
static lv_obj_t*    lockImgObj    = nullptr; // 大锁
static lv_obj_t*    lockBorderObj = nullptr; // 红色边框（闪烁告警）
static lv_obj_t*    lockLblLocked = nullptr; // "已锁定"（红）
static lv_obj_t*    lockLblYyq    = nullptr; // "YYQ"（灰）

static unsigned long lockFlashStartMs = 0;
static bool          lockFlashOn      = false;

// 双击静音键的配对状态（和 CPG 双击那套一模一样）
static unsigned long g_muteTapMs    = 0;      // 上一条 BTN:MUTE 的时刻
static bool          g_muteTapArmed = false;  // 是否还挂着一次"待配对"的单击

// 拆锁屏（放在建屏前面：建屏失败要靠它收拾残局）
static void lockScreenDestroy(void) {
    if (scr_lock != nullptr) {
        // 同样先换屏再删，别裸着 lv_obj_del 一块可能正在显示的屏
        swapAwayIfActive(scr_lock, nullptr);
        if (currentScreen == scr_lock) currentScreen = nullptr;
        lv_obj_del(scr_lock);
        scr_lock = nullptr;
    }
    lockImgObj    = nullptr;
    lockBorderObj = nullptr;
    lockLblLocked = nullptr;
    lockLblYyq    = nullptr;
    lockFlashOn   = false;
}

// 建锁屏
static void lockScreenBuild(void) {
    if (scr_lock != nullptr) return;

    // ⚠⚠ 每一个 lv_obj_create 都可能返回 NULL，必须逐个判。
    //
    // LVGL 的分配器优先 PSRAM、失败回落内部 DRAM，而内部 DRAM 正是这个项目
    // 历史上最主要的重启原因（见 lv_mem_port.h 顶部那段）。分配失败时
    // 少一把锁只是"不显示锁定画面"，崩了却是整机重启 —— 这个代价对比
    // 决定了这里必须判（和 build_hud 同一套理由）。
    scr_lock = lv_obj_create(NULL);
    if (scr_lock == nullptr) return;
    // 底色用**纯黑**：锁图的背景像素本来就是纯黑，底色换成 CLR_BG(0x0B0F17)
    // 的话，屏上会多出一个"比周围亮一点的方块"，非常难看。
    lv_obj_set_style_bg_color(scr_lock, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_lock, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_lock, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(scr_lock, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr_lock, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr_lock, LV_OBJ_FLAG_SCROLLABLE);

    // 大锁。lockImgDsc.data 直接指向 flash 里的 RGB565 数组。
    lockImgDsc.header.always_zero = 0;
    lockImgDsc.header.w          = LOCK_IMG_W;
    lockImgDsc.header.h          = LOCK_IMG_H;
    lockImgDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    lockImgDsc.data_size         = LOCK_IMG_W * LOCK_IMG_H * 2;
    lockImgDsc.data              = (const uint8_t*)lock_img_pixels;

    lockImgObj = lv_img_create(scr_lock);
    if (lockImgObj == nullptr) { lockScreenDestroy(); return; }
    lv_obj_set_size(lockImgObj, LOCK_IMG_W, LOCK_IMG_H);
    lv_obj_set_pos(lockImgObj, (240 - LOCK_IMG_W) / 2, LOCK_IMG_Y);
    lv_img_set_src(lockImgObj, &lockImgDsc);

    // "已锁定"（红）先建、先居中，"YYQ" 再用 align_to 挂到它左边 ——
    // 这样整行是**作为一个整体**居中的，而不是两段各自居中然后错开。
    //
    // 为什么不用一个 label 写 "YYQ 已锁定"：LV_LABEL_RECOLOR 没开，
    // 一段文字只能一个颜色，"已锁定"要红色就必须拆成两个 label。
    lockLblLocked = lv_label_create(scr_lock);
    if (lockLblLocked == nullptr) { lockScreenDestroy(); return; }
    mkLabel(lockLblLocked, &lv_font_simsun_16_cjk, CLR_RED);
    lv_label_set_text(lockLblLocked, "已锁定");
    lv_obj_align(lockLblLocked, LV_ALIGN_TOP_MID, 0, LOCK_TXT_Y);

    lockLblYyq = lv_label_create(scr_lock);
    if (lockLblYyq == nullptr) { lockScreenDestroy(); return; }
    mkLabel(lockLblYyq, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(lockLblYyq, "YYQ");
    // align_to 是按 base 的**当前坐标**算的，base 刚 align 完坐标还没落地，
    // 所以先把整屏的布局算一遍再挂。
    lv_obj_update_layout(scr_lock);
    lv_obj_align_to(lockLblYyq, lockLblLocked, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    // 红色边框：默认隐藏。按键被吞的时候闪一下。
    // 最后建 = 画在最上层（要盖住锁图和文字的边缘）。
    lockBorderObj = lv_obj_create(scr_lock);
    if (lockBorderObj == nullptr) { lockScreenDestroy(); return; }
    lv_obj_set_size(lockBorderObj, 240, 240);
    lv_obj_set_pos(lockBorderObj, 0, 0);
    lv_obj_set_style_bg_opa(lockBorderObj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lockBorderObj, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(lockBorderObj, lv_color_hex(CLR_RED), LV_PART_MAIN);
    lv_obj_set_style_radius(lockBorderObj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lockBorderObj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lockBorderObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lockBorderObj, LV_OBJ_FLAG_HIDDEN);
}

// 红框闪一下：按键被吞掉时的高警反馈
static void lockScreenFlash(void) {
    if (lockBorderObj == nullptr) return;
    lockFlashStartMs = millis();
    lockFlashOn = true;
    lv_obj_clear_flag(lockBorderObj, LV_OBJ_FLAG_HIDDEN);
}

// 每轮 loop 调一次：按 90ms 一拍做亮/灭交替，超时收起
static void lockScreenTick(void) {
    if (lockBorderObj == nullptr || !lockFlashOn) return;
    unsigned long now = millis();
    if (now - lockFlashStartMs >= LOCK_FLASH_MS) {
        lockFlashOn = false;
        lv_obj_add_flag(lockBorderObj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    bool wantOn = (((now - lockFlashStartMs) / LOCK_FLASH_HALF_MS) % 2) == 0;
    if (wantOn) lv_obj_clear_flag(lockBorderObj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(lockBorderObj, LV_OBJ_FLAG_HIDDEN);
}

// 改锁定状态 + 切界面（**不落盘**）。
//
// 单独拆出来是因为开机恢复那条路用得上：NVS 里本来就存着"锁着"，
// 再写一遍是**无意义的 NVS 擦写**（NVS 有磨损，而且写的时候会阻塞几十毫秒）。
static void lockApplyState(bool locked) {
    kbLocked = locked;

    // 锁定状态刚变过，之前那次静音单击不能再当成双击的第一击 ——
    // 否则"先按一下静音键 → 用网页远程锁定 → 350ms 内再按一下静音键"
    // 会被判成双击，锁刚上就又解开了。
    g_muteTapArmed = false;

    if (locked) {
        // 锁上之前 LOGO 可能正被按着 —— 把长按判定的状态清掉，
        // 不然锁屏上会莫名其妙弹出一个宠物页。
        logoIsDown = false;
        logoHoldFired = false;
        lockScreenBuild();
        showScreen(scr_lock);
        ELINFO("LOCK", "键盘已锁定");
    } else {
        lockScreenDestroy();
        // 解锁之后重新计一次活动时间：锁着的时候按键被吞掉了，
        // lastActivityTime 一直停在锁定那一刻（而且锁定期间不许息屏，
        // 时间差被完整累计）。不重置的话"锁一小时再解锁"会当场黑屏。
        lastActivityTime = millis();
        gotoMainScreen();
        triggerHudBig("键盘锁定", "已解锁", lv_color_hex(CLR_GREEN));
    }
}

// 锁定 / 解锁的唯一对外入口（落盘 + 改状态）
static void lockSetLocked(bool locked) {
    if (kbLocked == locked) return;
    preferences.putBool(NVS_KEY_LOCKED, locked);
    lockApplyState(locked);
}

// 启停倒计时的唯一入口：设时长、起表、把全屏叫起来。
// 网页的 TIMERSET: 和键盘设置页的回车键都走这里，避免两处各写一份
// "谁负责把 countdownShown 置上"的初始化。
static void startCountdown(uint32_t totalSec) {
    if (totalSec == 0) {
        triggerHudBig("倒计时", "时长不合法", lv_color_hex(CLR_RED));
        return;
    }
    if (totalSec > 86400UL) totalSec = 86400UL;   // 24 小时封顶

    timerTotalSec = totalSec;
    timerRemainSec = totalSec;
    timerStartMs = millis();
    timerRunning = true;
    timerEditH = (int)(totalSec / 3600);
    timerEditM = (int)((totalSec / 60) % 60);
    timerEditS = (int)(totalSec % 60);
    countdownShown = true;
}

static void stopCountdown(bool silent) {
    timerRunning = false;
    timerRemainSec = 0;
    countdownShown = false;
    destroyCountdownScreen();
    if (!silent) triggerHud("倒计时", "已停止", lv_color_hex(CLR_AMBER));
}

// 键盘设置页倒计时那一屏的回车键 = 页面那颗"开始/停止"按钮。
//
// 原来这里是个**摆设**：按钮画出来了、文案会跟着 timerRunning 在"开始/停止"
// 之间变，但全工程没有任何一行代码处理它；回车键走的是通用保存分支，
// 只把时长记进 timerTotalSec 就退出界面，一次都没起过表。所以"在键盘上
// 设倒计时"和"在网页上设倒计时"一样，从来就没跑起来过。
// 现在让按钮说的话成真：没在跑就起跑（并把全屏让给倒计时），在跑就停。
static void toggleCountdownFromKeyboard(void) {
    if (timerRunning) {
        stopCountdown(false);
    } else {
        uint32_t total = (uint32_t)(timerEditH * 3600 + timerEditM * 60 + timerEditS);
        if (total == 0) {
            triggerHudBig("倒计时", "时长不合法", lv_color_hex(CLR_RED));
            return;   // 停在设置页，让人改时长，别把界面弹走
        }
        startCountdown(total);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timerEditH, timerEditM, timerEditS);
        triggerHud("倒计时", buf, lv_color_hex(CLR_ACCENT));
    }
    gotoMainScreen();   // 交回主屏，倒计时全屏下一轮 loop 自己接管
}

// ===========================
// 每秒跑一遍：闹钟到点判定 + 倒计时推进
// ===========================
// 放主循环里，不占任何中断。
static void updateTimers(void) {
    static unsigned long lastSec = 0;
    unsigned long nowSec = millis() / 1000UL;
    if (nowSec == lastSec) return;
    lastSec = nowSec;

    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);

    // ---- 闹钟 ----
    // 时间没校准（tm_year < 124，1970 前后）时不判定，否则开机瞬间正好撞上
    // 00:00 的闹钟会莫名其妙响一次。
    // 一天只响一次靠 tm_yday 去重：跨过整点后这一分钟内会连续命中很多次 loop，
    // 没有 yday 就会连着响一整分钟。
    if (lt && lt->tm_year > 124 && alarmEnabled && ringingKind == RING_NONE) {
        int nowMin = lt->tm_hour * 60 + lt->tm_min;
        if (nowMin == alarmHour * 60 + alarmMinute && lt->tm_yday != alarmLastFiredYday) {
            alarmLastFiredYday = lt->tm_yday;   // 先记再去重，否则起铃失败会一直重试
            startRinging(RING_ALARM);
        }
    }

    // ---- 倒计时 ----
    if (!timerRunning) return;

    // 剩余量从"开始时刻"反算，而不是每秒自减：中间被宏的 delay() 或壁纸解码
    // 阻塞过几百毫秒也不会越欠越多（自减会把这段时间白吃掉）。
    // 无符号减法让 millis() 每 49.7 天回绕一次也天然正确。
    unsigned long elapsedSec = (millis() - timerStartMs) / 1000UL;
    if (elapsedSec >= timerTotalSec) {
        timerRemainSec = 0;
        timerRunning = false;
        countdownShown = false;
        destroyCountdownScreen();
        startRinging(RING_TIMER);
        return;
    }
    timerRemainSec = timerTotalSec - (uint32_t)elapsedSec;
}

// ===========================
// 设置界面显示更新函数
// ===========================
static void update_setting_time_display(void) {
    static char dbuf[32], tbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", timeEditY, timeEditMo, timeEditD);
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", timeEditH, timeEditMi);
    setText(set_time_lbl_date, dbuf);
    setText(set_time_lbl_time, tbuf);
    for (int i = 0; i < 5; i++) markField(set_time_field_labels[i], i == timeFieldIdx);
}

static void update_setting_alarm_display(void) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", alarmEditH, alarmEditM);
    setText(set_alarm_lbl_time, buf);
    // 开关和状态文字都跟**编辑态**走，不是跟已保存的值走：
    // 用户在页面上翻到"开"还没按回车时，屏上就该已经是"已开启"，
    // 否则会出现"我明明选了开，它还显示已关闭"这种看起来没生效的情况。
    if (set_alarm_sw) {
        if (alarmEditOn) lv_obj_add_state(set_alarm_sw, LV_STATE_CHECKED);
        else              lv_obj_clear_state(set_alarm_sw, LV_STATE_CHECKED);
    }
    setText(set_alarm_state_lbl, alarmEditOn ? "已开启" : "已关闭");
    if (set_alarm_state_lbl) {
        lv_obj_set_style_text_color(set_alarm_state_lbl,
            lv_color_hex(alarmEditOn ? CLR_GREEN : CLR_TEXT_MUTE), LV_PART_MAIN);
    }
    for (int i = 0; i < 3; i++) markField(set_alarm_field_labels[i], i == alarmFieldIdx);
}

static void update_setting_timer_display(void) {
    static char buf[32];
    uint32_t h = timerRemainSec / 3600;
    uint32_t m = (timerRemainSec % 3600) / 60;
    uint32_t s = timerRemainSec % 60;
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
    setText(set_timer_lbl_time, buf);
    if (set_timer_btn) {
        lv_obj_set_style_bg_color(set_timer_btn,
            lv_color_hex(timerRunning ? CLR_RED : CLR_ACCENT), LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(set_timer_btn, 0);
        setText(lbl, timerRunning ? "停止" : "开始");
    }
    for (int i = 0; i < 3; i++) markField(set_timer_field_labels[i], i == timerFieldIdx);
}

static void update_setting_caltemp_display(void) {
    static char tbuf[32], obuf[32];
    snprintf(tbuf, sizeof(tbuf), "%.1f °C", shtTemp);
    snprintf(obuf, sizeof(obuf), "偏移 %+.1f", shtTempOffset);
    setText(set_cal_lbl_temp, tbuf);
    setText(set_cal_lbl_offset, obuf);
}

// ===========================
// 灯光设置：显示函数
// ===========================
// 四个字段共用一块"大字预览卡"：上面一行是当前字段的中文名，
// 中间是超大号数值，下面一行英文小字。纯数字的字段（背光亮度）用
// Montserrat 28 显得更锐利，中文/英文混排的用中文字体。
static void update_setting_light_display(void) {
    if (set_light_lbl_value == nullptr) return;

    static char nbuf[24];
    const char* value;
    bool numeric = false;

    switch (lightFieldIdx) {
        case 0:
            value = lightOn ? lightSwitchCN[0] : lightSwitchCN[1];
            break;
        case 1:
            snprintf(nbuf, sizeof(nbuf), "%d%%", (int)brightness * 100 / 255);
            value = nbuf;
            numeric = true;
            break;
        case 2:
            value = effectNames[currentEffect];
            break;
        case 3:
            value = indLevelNames[indLevel];
            break;
        default:
            value = keyFxNames[keyFxStyle];
            break;
    }

    setText(set_light_lbl_cap, lightFieldCN[lightFieldIdx]);
    setText(set_light_lbl_en, lightFieldEN[lightFieldIdx]);
    setText(set_light_lbl_value, value);
    lv_obj_set_style_text_font(set_light_lbl_value,
        numeric ? &lv_font_montserrat_28 : &lv_font_simsun_16_cjk, LV_PART_MAIN);

    for (int i = 0; i < LIGHT_FIELD_COUNT; i++) {
        markField(set_light_field_labels[i], i == lightFieldIdx);
    }
}

// ===========================
// 设置界面构建函数
// ===========================
// 全部改成「建一次，之后只刷标签」。
// 原因：原来每次进来都 lv_obj_del(旧屏) + lv_obj_create(NULL)，而旧屏往往正是
// 当前活动屏 —— LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL，紧接着的刷新就
// 解引用空指针 → panic → 重启（MR 进录制必崩就是这个）。顺带整屏删建也太重。
// 设置页外壳。返回的是"第二行提示"那个 label，方便调用方整块改文案
// （见 build_elog / build_recording）；第一行是第 3 个子对象。
// 子对象顺序是有讲究的，别乱插：0=竖条 1=标题 2=英文副标题 3=提示第一行 4=提示第二行。
// hintCN 传 nullptr 就用默认那套（第一行切字段/调值，第二行保存/取消）。
static lv_obj_t* settingShell(lv_obj_t* scr, const char* titleCN, const char* titleEn,
                              const char* hintCN1 = "←→ 切换 · ↑↓ 调整",
                              const char* hintCN2 = "回车保存 · ESC 取消") {
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 3, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 14, 13);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(scr);
    mkLabel(t, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(t, titleCN);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 12);

    lv_obj_t* sub = lv_label_create(scr);
    mkLabel(sub, &lv_font_montserrat_10, CLR_TEXT_MUTE);
    lv_label_set_text(sub, titleEn);
    lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -16, 16);

    // 底部两行按键提示。宽度上限 HINT_W + LONG_DOT 是硬兜底，
    // 文案再长也只会变省略号，不会画到屏幕外面去。
    lv_obj_t* hint1 = mkHintLine(scr, HINT_LINE1_Y, hintCN1);
    lv_obj_t* hint2 = mkHintLine(scr, HINT_LINE2_Y, hintCN2);
    // 让引用"两块提示"的调用方好拿：把第二行挂成第一行的子对象不行（会跟着移动），
    // 这里只保证第一行固定是第 3 个子对象，跟旧代码的 lv_obj_get_child(scr, 3) 对得上。
    (void)hint1;
    return hint2;
}

// 字段标签（当前字段点亮成强调色）。返回里面的 label，供 update_* 改配色。
static lv_obj_t* fieldChip(lv_obj_t* parent, int x, int y, int w, const char* text, bool active) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, 22);
    lv_obj_align(c, LV_ALIGN_CENTER, x, y);
    mkChip(c, active ? CLR_SURFACE_2 : CLR_SURFACE,
              active ? CLR_ACCENT : CLR_TEXT_MUTE, 8);
    lv_obj_t* l = lv_label_create(c);
    mkLabel(l, &lv_font_simsun_16_cjk, active ? CLR_ACCENT : CLR_TEXT_MUTE);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return l;
}

// 高亮某个字段标签：文字变强调色，底下的胶囊也一起点亮
static void markField(lv_obj_t* lbl, bool active) {
    if (lbl == nullptr) return;
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(active ? CLR_ACCENT : CLR_TEXT_MUTE), LV_PART_MAIN);
    lv_obj_t* chip = lv_obj_get_parent(lbl);
    if (chip) {
        lv_obj_set_style_bg_color(chip,
            lv_color_hex(active ? CLR_SURFACE_2 : CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, active ? 1 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(chip, lv_color_hex(CLR_ACCENT_D), LV_PART_MAIN);
    }
}

static void build_settings_time(void) {
    // 编辑值从当前 RTC 取初值
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (ti && ti->tm_year >= 124) {
        timeEditY = ti->tm_year + 1900;
        timeEditMo = ti->tm_mon + 1;
        timeEditD = ti->tm_mday;
        timeEditH = ti->tm_hour;
        timeEditMi = ti->tm_min;
    }
    timeFieldIdx = 0;

    if (scr_settings_time == nullptr) {
        scr_settings_time = lv_obj_create(NULL);
        settingShell(scr_settings_time, "设置时间", "SET TIME");

        // 主卡片：日期 + 时间
        lv_obj_t* card = lv_obj_create(scr_settings_time);
        lv_obj_set_size(card, 200, 120);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -14);
        mkCard(card, CLR_SURFACE, 12);

        set_time_lbl_date = lv_label_create(card);
        mkLabel(set_time_lbl_date, &lv_font_montserrat_20, CLR_TEXT_DIM);
        lv_label_set_text(set_time_lbl_date, "2026-01-01");
        lv_obj_align(set_time_lbl_date, LV_ALIGN_TOP_MID, 0, 14);

        set_time_lbl_time = lv_label_create(card);
        mkLabel(set_time_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_time_lbl_time, "00:00");
        lv_obj_align(set_time_lbl_time, LV_ALIGN_CENTER, 0, 16);

        const char* fieldNames[5] = { "年", "月", "日", "时", "分" };
        int fieldX[5] = { -92, -46, 0, 46, 92 };
        for (int i = 0; i < 5; i++) {
            set_time_field_labels[i] = fieldChip(scr_settings_time, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_time_display();
    showScreen(scr_settings_time);
    currentSysMode = SYS_MODE_SET_TIME;
}

static void build_settings_alarm(void) {
    alarmEditH = alarmHour;
    alarmEditM = alarmMinute;
    alarmFieldIdx = 0;
    alarmEditOn = alarmEnabled;   // 进页面先把当前状态搬进编辑态

    if (scr_settings_alarm == nullptr) {
        scr_settings_alarm = lv_obj_create(NULL);
        settingShell(scr_settings_alarm, "闹钟设置", "ALARM");

        lv_obj_t* card = lv_obj_create(scr_settings_alarm);
        lv_obj_set_size(card, 200, 118);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -14);
        mkCard(card, CLR_SURFACE, 12);

        set_alarm_lbl_time = lv_label_create(card);
        mkLabel(set_alarm_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_alarm_lbl_time, "07:00");
        lv_obj_align(set_alarm_lbl_time, LV_ALIGN_TOP_MID, 0, 18);

        // 开关 + 文字状态。
        //
        // 这个 lv_switch **只当显示器用**，不指望谁来点它：整机没有触屏，
        // 全工程也没有一处 add_event_cb，它天生点不动。以前它是唯一的"开关"，
        // 状态只从 alarmEnabled 单向镜像过去、没有任何输入路径能改 ——
        // 看着像个开关，其实只能看。现在真正能改它的是下面第三个字段"开关"，
        // ←/→ 选到它、↑/↓ 翻 alarmEditOn，这里跟着刷新。
        set_alarm_sw = lv_switch_create(card);
        lv_obj_set_size(set_alarm_sw, 52, 28);
        lv_obj_align(set_alarm_sw, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_TEXT), LV_PART_KNOB);
        lv_obj_clear_flag(set_alarm_sw, LV_OBJ_FLAG_CLICKABLE);

        set_alarm_state_lbl = lv_label_create(card);
        mkLabel(set_alarm_state_lbl, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(set_alarm_state_lbl, "已关闭");
        lv_obj_align(set_alarm_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

        // 时 / 分 / 开关。和灯光设置页的"背光开关"是同一套做法：
        // 布尔量不做成不可点的控件，而是排进可导航的字段循环里，↑/↓ 直接翻。
        const char* fieldNames[3] = { "时", "分", "开关" };
        int fieldX[3] = { -70, 0, 70 };
        for (int i = 0; i < 3; i++) {
            set_alarm_field_labels[i] = fieldChip(scr_settings_alarm, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_alarm_display();
    showScreen(scr_settings_alarm);
    currentSysMode = SYS_MODE_SET_ALARM;
}

static void build_settings_timer(void) {
    timerFieldIdx = 0;
    if (timerTotalSec > 0) {
        timerEditH = timerTotalSec / 3600;
        timerEditM = (timerTotalSec % 3600) / 60;
        timerEditS = timerTotalSec % 60;
    }
    if (!timerRunning) timerRemainSec = timerTotalSec;

    if (scr_settings_timer == nullptr) {
        scr_settings_timer = lv_obj_create(NULL);
        // 这一屏的回车是"开始/停止"，不是"保存"（见 toggleCountdownFromKeyboard），
        // 所以第二行提示不能沿用通用的"回车保存"，否则会教用户按一个不起作用的键。
        settingShell(scr_settings_timer, "倒计时", "TIMER",
                     "←→ 切换 · ↑↓ 调整",
                     "回车启停 · ESC 取消");

        lv_obj_t* card = lv_obj_create(scr_settings_timer);
        lv_obj_set_size(card, 200, 108);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -16);
        mkCard(card, CLR_SURFACE, 12);

        set_timer_lbl_time = lv_label_create(card);
        mkLabel(set_timer_lbl_time, &lv_font_montserrat_48, CLR_TEXT);
        lv_label_set_text(set_timer_lbl_time, "00:05:00");
        lv_obj_align(set_timer_lbl_time, LV_ALIGN_TOP_MID, 0, 16);

        set_timer_btn = lv_obj_create(card);
        lv_obj_set_size(set_timer_btn, 108, 30);
        lv_obj_align(set_timer_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
        mkChip(set_timer_btn, CLR_ACCENT, CLR_BG, 15);
        lv_obj_t* btn_lbl = lv_label_create(set_timer_btn);
        mkLabel(btn_lbl, &lv_font_simsun_16_cjk, CLR_BG);
        lv_label_set_text(btn_lbl, "开始");
        lv_obj_center(btn_lbl);

        const char* fieldNames[3] = { "时", "分", "秒" };
        int fieldX[3] = { -52, 0, 52 };
        for (int i = 0; i < 3; i++) {
            set_timer_field_labels[i] = fieldChip(scr_settings_timer, fieldX[i], 62, 44, fieldNames[i], false);
        }
    }
    update_setting_timer_display();
    showScreen(scr_settings_timer);
    currentSysMode = SYS_MODE_SET_TIMER;
}

static void build_settings_caltemp(void) {
    calTempOriginal = shtTempOffset;

    if (scr_settings_caltemp == nullptr) {
        scr_settings_caltemp = lv_obj_create(NULL);
        settingShell(scr_settings_caltemp, "温度校准", "CALIBRATE",
                     // 这一屏只有一个字段，"←→ 切换"没有意义（切不到别处去），
                     // 第一行只留"↑↓ 调偏移量"，比通用的那行更准。
                     "↑↓ 调整偏移量",
                     "回车保存 · ESC 取消");

        lv_obj_t* card = lv_obj_create(scr_settings_caltemp);
        lv_obj_set_size(card, 200, 110);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -18);
        mkCard(card, CLR_SURFACE, 12);

        lv_obj_t* cap = lv_label_create(card);
        mkLabel(cap, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(cap, "当前温度");
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 14);

        set_cal_lbl_temp = lv_label_create(card);
        mkLabel(set_cal_lbl_temp, &lv_font_montserrat_48, CLR_AMBER);
        lv_label_set_text(set_cal_lbl_temp, "--.- C");
        lv_obj_align(set_cal_lbl_temp, LV_ALIGN_CENTER, 0, 6);

        set_cal_lbl_offset = lv_label_create(card);
        mkLabel(set_cal_lbl_offset, &lv_font_simsun_16_cjk, CLR_ACCENT);
        lv_label_set_text(set_cal_lbl_offset, "偏移 +0.0");
        lv_obj_align(set_cal_lbl_offset, LV_ALIGN_BOTTOM_MID, 0, -12);

        // 原来这里还有一条 "↑↓ 调整偏移量" 的副提示，挂在 y=212，
        // 正好压在两行按键提示（199~237）上面。现在底部提示已经写了同一句话，
        // 这条重复的删掉，卡片也顺势上移一点。
    }
    update_setting_caltemp_display();
    showScreen(scr_settings_caltemp);
    currentSysMode = SYS_MODE_CAL_TEMP;
}

// ===========================
// 构建：灯光设置
// ===========================
// 背光开关 / 背光亮度 / 灯效 / 状态灯亮度 四项集中在这里。
// 这几项以前散在菜单里"按一下循环一档"，问题是按错一次既不知道现在在哪一档、
// 也退不回去；做成设置页后每项都能看到当前值，←→ 选、↑↓ 调、回车才生效。
static void build_settings_light(void) {
    // 记一份原值，ESC 取消时原样还回去（灯效这类循环量不记就回不去了）
    lightOnOriginal = lightOn;
    lightBrightOriginal = brightness;
    lightEffectOriginal = currentEffect;
    lightIndLevelOriginal = indLevel;
    lightKeyFxOriginal = keyFxStyle;
    lightFieldIdx = 0;

    if (scr_settings_light == nullptr) {
        scr_settings_light = lv_obj_create(NULL);
        settingShell(scr_settings_light, "灯光设置", "LIGHTING");

        // 大字预览卡。
        // 高度和 y 都往下（往上）挪过一轮：底部两行按键提示占 y=199~237，
        // 原来这页第三排胶囊落在 189~211，正好压在上面。现在整块内容收在
        // 40~197 之间，和提示之间留 9px。
        lv_obj_t* card = lv_obj_create(scr_settings_light);
        lv_obj_set_size(card, 204, 76);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -40);
        mkCard(card, CLR_SURFACE, 12);

        set_light_lbl_cap = lv_label_create(card);
        mkLabel(set_light_lbl_cap, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
        lv_label_set_text(set_light_lbl_cap, lightFieldCN[0]);
        lv_obj_align(set_light_lbl_cap, LV_ALIGN_TOP_MID, 0, 8);

        set_light_lbl_value = lv_label_create(card);
        mkLabel(set_light_lbl_value, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(set_light_lbl_value, "开");
        lv_obj_set_width(set_light_lbl_value, 184);
        lv_label_set_long_mode(set_light_lbl_value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(set_light_lbl_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(set_light_lbl_value, LV_ALIGN_CENTER, 0, 4);

        set_light_lbl_en = lv_label_create(card);
        mkLabel(set_light_lbl_en, &lv_font_montserrat_10, CLR_TEXT_MUTE);
        lv_label_set_text(set_light_lbl_en, lightFieldEN[0]);
        lv_obj_align(set_light_lbl_en, LV_ALIGN_BOTTOM_MID, 0, -8);

        // 五个字段胶囊：前四个 2x2，第五个（按键灯效）单独占一整行。
        // 第五个做成通栏是因为它是个"开/关型"的新开关，和上面四个调量分成两排，
        // 视觉上不容易和"灯效"那一格看混（两者名字太像了）。
        // chipY 三排分别是 125~147 / 150~172 / 175~197，最底下一排刚好收在
        // 两行按键提示（199 起）之上。
        const int chipX[2] = { -52, 52 };
        const int chipY[3] = { 16, 41, 66 };
        for (int i = 0; i < LIGHT_FIELD_COUNT; i++) {
            if (i == 4) {
                set_light_field_labels[i] = fieldChip(scr_settings_light,
                    0, chipY[2], 204, lightFieldCN[i], false);
            } else {
                set_light_field_labels[i] = fieldChip(scr_settings_light,
                    chipX[i % 2], chipY[i / 2], 96, lightFieldCN[i], false);
            }
        }
    }
    update_setting_light_display();
    showScreen(scr_settings_light);
    currentSysMode = SYS_MODE_SET_LIGHT;
}

// ===========================
// 宏录制界面（MR 三段式）
// ===========================
// MR 第一次按 = 连续输入(SEQ)，第二次按 = 组合键(CMB)，第三次按 = 取消回主屏。
// 三段状态机只有 recStage 一个变量，录制页也就只建一次、之后只刷 label ——
// 原来每录一颗键就 lv_obj_del(scr_recording) 整屏重建，而它正是活动屏，
// LVGL 8.4 删活动屏会把 disp->act_scr 置 NULL，下一帧刷新直接空指针 → panic → 重启。
static const char* recStageTitleCN[2] = { "录制 · 连续输入", "录制 · 组合键" };
static const char* recStageTag[2]     = { "SEQ", "CMB" };

static void update_recording_display(void) {
    if (rec_lbl_count == nullptr) return;

    static char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d / %d", recKeyCount, MAX_REC_KEYS);
    setText(rec_lbl_count, cbuf);

    // 键流用按键名而不是十六进制码，和主屏"最近按键"保持一致
    static char kbuf[256];
    size_t used = 0;
    kbuf[0] = '\0';
    int shown = 0;
    for (int i = 0; i < recKeyCount && shown < 8; i++) {
        const char* nm = getKeyName(recKeyBuffer[i]);
        int n = snprintf(kbuf + used, sizeof(kbuf) - used, "%s%s",
                         shown ? " · " : "", nm);
        if (n < 0 || (size_t)n >= sizeof(kbuf) - used) break;
        used += n;
        shown++;
    }
    if (recKeyCount == 0) {
        snprintf(kbuf, sizeof(kbuf), "%s",
                 (recStage == REC_STAGE_SEQ) ? "依次按下要录制的按键…"
                                             : "按下要同时按住的组合键…");
    }
    setText(rec_lbl_keys, kbuf);
}

static void build_recording(void) {
    const bool seq = (recStage == REC_STAGE_SEQ);
    uint32_t accent = seq ? CLR_AMBER : CLR_VIOLET;

    if (scr_recording == nullptr) {
        scr_recording = lv_obj_create(NULL);
        settingShell(scr_recording, recStageTitleCN[0], "MACRO REC");

        // 标题竖条颜色随模式变（SEQ 琥珀 / CMB 紫）
        lv_obj_t* shell_bar = lv_obj_get_child(scr_recording, 0);
        if (shell_bar) {
            lv_obj_set_style_bg_color(shell_bar, lv_color_hex(accent), LV_PART_MAIN);
        }
        // 标题文字
        lv_obj_t* shell_title = lv_obj_get_child(scr_recording, 1);
        if (shell_title) setText(shell_title, recStageTitleCN[0]);

        // 计数
        rec_lbl_count = lv_label_create(scr_recording);
        mkLabel(rec_lbl_count, &lv_font_montserrat_48, accent);
        lv_label_set_text(rec_lbl_count, "0 / 64");
        lv_obj_align(rec_lbl_count, LV_ALIGN_TOP_MID, 0, 52);

        // 已录按键流
        lv_obj_t* keys_bg = lv_obj_create(scr_recording);
        lv_obj_set_size(keys_bg, 204, 76);
        lv_obj_align(keys_bg, LV_ALIGN_CENTER, 0, 6);
        mkCard(keys_bg, CLR_SURFACE, 10);

        rec_lbl_keys = lv_label_create(keys_bg);
        mkLabel(rec_lbl_keys, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(rec_lbl_keys, "");
        lv_obj_set_width(rec_lbl_keys, 184);
        lv_label_set_long_mode(rec_lbl_keys, LV_LABEL_LONG_WRAP);
        lv_obj_align(rec_lbl_keys, LV_ALIGN_TOP_MID, 0, 10);

        // 模式标签（SEQ / CMB）
        rec_lbl_mode = lv_label_create(scr_recording);
        mkLabel(rec_lbl_mode, &lv_font_montserrat_12, accent);
        lv_label_set_text(rec_lbl_mode, recStageTag[0]);
        lv_obj_align(rec_lbl_mode, LV_ALIGN_TOP_RIGHT, -16, 16);

        // 底部按键提示：settingShell 默认写的是设置页那套，这里换成录制页的
        // 子对象顺序：0=竖条 1=标题 2=英文副标题 3=提示第一行 4=提示第二行
        setHintText(lv_obj_get_child(scr_recording, 3), "M1-M12 保存 · MR 下一步");
        setHintText(lv_obj_get_child(scr_recording, 4), "ESC 取消");
    } else {
        // 已存在：只切标题 / 配色
        lv_obj_t* shell_bar = lv_obj_get_child(scr_recording, 0);
        if (shell_bar) lv_obj_set_style_bg_color(shell_bar, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_t* shell_title = lv_obj_get_child(scr_recording, 1);
        if (shell_title) setText(shell_title, recStageTitleCN[seq ? 0 : 1]);
        setText(rec_lbl_mode, recStageTag[seq ? 0 : 1]);
        lv_obj_set_style_text_color(rec_lbl_mode, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_set_style_text_color(rec_lbl_count, lv_color_hex(accent), LV_PART_MAIN);
    }

    update_recording_display();
    showScreen(scr_recording);
    currentSysMode = seq ? SYS_MODE_REC_SEQ : SYS_MODE_REC_CMB;
}

// 从主屏进录制：永远从"连续输入"这一档开始
static void enterRecording(void) {
    recStage = REC_STAGE_SEQ;
    recKeyCount = 0;
    triggerHud("宏录制", "连续输入", lv_color_hex(CLR_AMBER));
    build_recording();
}

// 录制态里再按一次 MR：SEQ → CMB → 取消，三段走完
static void advanceRecordingStage(void) {
    switch (recStage) {
        case REC_STAGE_SEQ:
            recStage = REC_STAGE_CMB;
            recKeyCount = 0;      // 换模式就清空键流，两种模式的键流语义不同
            triggerHud("宏录制", "组合键", lv_color_hex(CLR_VIOLET));
            build_recording();
            break;

        case REC_STAGE_CMB:
        default:
            recStage = REC_STAGE_EXIT;
            recKeyCount = 0;
            triggerHud("宏录制", "已取消", lv_color_hex(CLR_RED));
            gotoMainScreen();
            break;
    }
}

static void cancelRecording(void) {
    recStage = REC_STAGE_EXIT;
    recKeyCount = 0;
    triggerHud("宏录制", "已取消", lv_color_hex(CLR_RED));
    gotoMainScreen();
}

// ===========================
// 错误日志查看页
// ===========================
//
// 这块屏存在的理由：整块板只有一个 USB 口，做键盘时被 HID 占死，
// 串口日志和 BLE 网页日志在现场都指望不上。出错信息只能留在芯片自己的
// flash 里，靠这一页在重启后翻出来。详见 elog.h 顶部。
//
// 一条日志占两行（第一行 时间/级别/次数，第二行 正文），正文允许再折一行，
// 所以行高是按"最坏情况 = 正文两行"算的：
//   3(上留白) + 12(数字行) + 1(行间隙) + 19×2(中文正文两行) + 2(下留白) = 56
// 这里的 19 是 lv_font_simsun_16_cjk 的 line_height（字库 .line_height 字段实测值）。
//
// 纵向预算（屏 240 高）：
//   12~31    标题「错误日志」+ 右上「共 N 条」
//   36~168   列表：3 行 × 44
//   199~237  底部按键提示（两行）
//
// 行高从 58 压到 44：底部提示改成两行之后要占 38px（原来一行只占 19px），
// 而列表 3 行 × 58 排到 210，正好压在第一行提示上。44px 一行放得下
// 正文那两行（19 + 19 = 38，余 6px 当行距），行数还是 3 行没少。
//
// 为什么只有 3 行：正文要留两行的余量（一条崩溃记录能到 20 多个字，
// 206px 宽一行只放得下 12 个汉字）。翻页用 PgUp/PgDn 一次跳 3 行。
//
// 为什么没有"错误/警告/信息 各多少条"的汇总行：它和每行自带的级别字样
// + 左侧色条 + 菜单里那条红色角标信息重复。错误条数在「系统 → 错误日志」
// 那一项的角标上，进这一页之前就能看到。
#define ELOG_ROW_H      44
#define ELOG_ROWS       3
#define ELOG_LIST_TOP   36
#define ELOG_SEL_MAX    (ELOG_CAP - 1)

static lv_obj_t* scr_elog = nullptr;
static lv_obj_t* elog_cont = nullptr;                 // 裁剪窗口
static lv_obj_t* elog_lbl_count = nullptr;            // 右上角「共 N 条」
static lv_obj_t* elog_lbl_empty = nullptr;            // 没有记录时的提示
static lv_obj_t* elog_row[ELOG_ROWS] = { nullptr };   // 可见的行容器（ELOG_ROWS 个）
static int elogScroll = 0;                            // 窗口第一条的逻辑下标
static int elogSel = 0;                               // 当前选中条（0 = 最新）
static bool elogArmClear = false;                     // DEL 连按两次才真清空

static uint32_t elogLevelColor(uint8_t lv) {
    switch (lv) {
        case EL_LVL_ERR:  return CLR_RED;
        case EL_LVL_WARN: return CLR_AMBER;
        default:          return CLR_ACCENT;
    }
}
static const char* elogLevelCN(uint8_t lv) {
    switch (lv) {
        case EL_LVL_ERR:  return "错误";
        case EL_LVL_WARN: return "警告";
        default:          return "信息";
    }
}

// 一条日志的"第一行"：时间 + 级别 + 标签 + 次数。
// 时间优先用墙钟；系统时间没校过就退成"运行 N 分"，免得显示一排 1970 年。
static void elogFormatTime(const ELogRec* r, char* out, size_t n) {
    if (r->epoch >= 1577836800UL) {
        time_t t = (time_t)r->epoch;
        struct tm tmv;
        if (localtime_r(&t, &tmv)) {
            snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            return;
        }
    }
    snprintf(out, n, "%d分", (int)(r->stampMs / 60000UL));
}

// 刷新列表。elogSel 是"从最新往回数第几条"（0 = 最新），
// 转成逻辑下标要用 total-1-elogSel —— 日志是**新的在上**，符合看日志的直觉。
static void elogRefresh(void) {
    int total = elog_count();
    if (total == 0) {
        setText(elog_lbl_count, "共 0 条");
        for (int i = 0; i < ELOG_ROWS; i++) {
            if (elog_row[i]) setHidden(elog_row[i], true);
        }
        if (elog_lbl_empty) {
            // 没记录时判"时间未校准"只能看**当前**系统时间：elog_wallValid()
            // 是去翻已有记录的，这里一条都没有，它必然返回 false，用不了。
            bool clockOk = ((uint32_t)time(nullptr) >= 1577836800UL);
            setText(elog_lbl_empty, clockOk ? "没有错误 一切正常" : "没有错误 时间未校准");
            setHidden(elog_lbl_empty, false);
        }
        return;
    }
    if (elog_lbl_empty) setHidden(elog_lbl_empty, true);

    if (elogSel >= total) elogSel = total - 1;
    if (elogSel < 0) elogSel = 0;

    // 让选中项始终落在可见窗口里
    int selLogical = total - 1 - elogSel;
    if (selLogical < elogScroll) elogScroll = selLogical;
    if (selLogical >= elogScroll + ELOG_ROWS) elogScroll = selLogical - ELOG_ROWS + 1;
    if (elogScroll > total - ELOG_ROWS) elogScroll = total - ELOG_ROWS;
    if (elogScroll < 0) elogScroll = 0;

    char cnt[16];
    snprintf(cnt, sizeof(cnt), "共 %d 条", total);
    setText(elog_lbl_count, cnt);

    // 画 ELOG_ROWS 个可见行
    for (int i = 0; i < ELOG_ROWS; i++) {
        lv_obj_t* row = elog_row[i];
        if (row == nullptr) continue;
        int li = elogScroll + i;
        if (li >= total) { setHidden(row, true); continue; }
        setHidden(row, false);

        const ELogRec* r = elog_get(li);
        if (r == nullptr) { setHidden(row, true); continue; }
        bool isSel = (li == selLogical);
        uint32_t lc = elogLevelColor(r->level);

        // 底色：选中项抬升一层，一眼能看出光标在哪
        lv_obj_set_style_bg_color(row, lv_color_hex(isSel ? CLR_SURFACE_2 : CLR_SURFACE),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(isSel ? lc : CLR_STROKE),
                                      LV_PART_MAIN);

        // 子对象下标必须和建行时的创建顺序对上：bar=0, l1=1(ASCII 首行), l2=2(中文正文)
        lv_obj_t* bar = lv_obj_get_child(row, 0);   // 左侧级别色条
        lv_obj_t* l1   = lv_obj_get_child(row, 1);   // 时间 · 模块 · 次数
        lv_obj_t* l2   = lv_obj_get_child(row, 2);   // 级别 + 正文

        // 首行纯 ASCII：时间 + 模块标签 + 重复次数
        char tbuf[16];
        elogFormatTime(r, tbuf, sizeof(tbuf));
        char head[64];
        if (r->count > 1) {
            snprintf(head, sizeof(head), "%s  %s  x%u", tbuf, r->tag, (unsigned)r->count);
        } else {
            snprintf(head, sizeof(head), "%s  %s", tbuf, r->tag);
        }
        setText(l1, head);
        lv_obj_set_style_text_color(l1, lv_color_hex(isSel ? CLR_TEXT_DIM : CLR_TEXT_MUTE),
                                    LV_PART_MAIN);

        // 第二行：级别字样 + 正文。级别用字而不是只靠颜色，色觉障碍 /
        // 小屏反光下也读得出来 —— 和通知面板"不靠颜色下判断"是同一个原则。
        char body[ELOG_MSG_MAX + 16];
        snprintf(body, sizeof(body), "%s %s", elogLevelCN(r->level), r->msg);
        setText(l2, body);
        lv_obj_set_style_text_color(l2, lv_color_hex(isSel ? CLR_TEXT : CLR_TEXT_DIM),
                                    LV_PART_MAIN);

        if (bar) {
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(lc), LV_PART_MAIN);
        }
    }
}

static void build_elog(void) {
    if (scr_elog == nullptr) {
        scr_elog = lv_obj_create(NULL);
        settingShell(scr_elog, "错误日志", "ERROR LOG");

        // settingShell 建的两行提示在这里换成日志页自己的。
        // 子对象顺序：0=竖条 1=标题 2=英文副标题 3=提示第一行 4=提示第二行
        setHintText(lv_obj_get_child(scr_elog, 3), "↑↓ 翻看 · DEL 清空");
        setHintText(lv_obj_get_child(scr_elog, 4), "ESC 返回菜单");
        // 标题竖条改成红色：这一页专门看故障，跟其它设置页一个颜色容易滑过去
        lv_obj_t* shell_bar = lv_obj_get_child(scr_elog, 0);
        if (shell_bar) {
            lv_obj_set_style_bg_color(shell_bar, lv_color_hex(CLR_RED), LV_PART_MAIN);
        }

        // settingShell 的右上角本来放英文副标题（child 2）。这一页那个位置
        // 要放「共 N 条」—— **复用同一个 label**，不要再新建一个：
        // 两个 label 都在 TOP_RIGHT 会叠在一起，"共 N 条"被英文标题压住。
        elog_lbl_count = lv_obj_get_child(scr_elog, 2);
        if (elog_lbl_count) {
            mkLabel(elog_lbl_count, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
            lv_obj_align(elog_lbl_count, LV_ALIGN_TOP_RIGHT, -14, 12);
            lv_label_set_text(elog_lbl_count, "共 0 条");
        }

        // 列表容器：只裁剪，不画底。
        // **必须清掉 SCROLLABLE**：默认 lv_obj_create 是可滚动的，内容超出
        // 会被 LVGL 当成可滚动区域处理；我们要的是"超出就裁掉"。
        elog_cont = lv_obj_create(scr_elog);
        lv_obj_set_size(elog_cont, 232, ELOG_ROWS * ELOG_ROW_H);
        lv_obj_align(elog_cont, LV_ALIGN_TOP_MID, 0, ELOG_LIST_TOP);
        lv_obj_set_style_bg_opa(elog_cont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(elog_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(elog_cont, 0, LV_PART_MAIN);
        lv_obj_clear_flag(elog_cont, LV_OBJ_FLAG_SCROLLABLE);

        // 空状态提示：一条记录都没有时显示，正常状态隐藏
        elog_lbl_empty = lv_label_create(elog_cont);
        mkLabel(elog_lbl_empty, &lv_font_simsun_16_cjk, CLR_GREEN);
        lv_label_set_text(elog_lbl_empty, "没有错误 一切正常");
        lv_obj_set_width(elog_lbl_empty, 224);
        lv_label_set_long_mode(elog_lbl_empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(elog_lbl_empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(elog_lbl_empty, LV_ALIGN_CENTER, 0, -20);
        setHidden(elog_lbl_empty, true);

        // ELOG_ROWS 个行容器，建一次，之后只改文字和配色。
        // 行内两行都用 TOP 锚（不用 BOTTOM 锚）：字库 line_height 是 19，
        // 贴着底锚会把第一行顶出容器，两行文字直接压在一起。
        for (int i = 0; i < ELOG_ROWS; i++) {
            lv_obj_t* row = lv_obj_create(elog_cont);
            lv_obj_set_size(row, 228, ELOG_ROW_H - 2);   // 行间距 2px
            lv_obj_set_pos(row, 0, i * ELOG_ROW_H);
            mkCard(row, CLR_SURFACE, 6);

            // 左侧级别色条
            lv_obj_t* bar = lv_obj_create(row);
            lv_obj_set_size(bar, 3, ELOG_ROW_H - 16);
            lv_obj_set_pos(bar, 1, 8);
            lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
            lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

            // 第一行：时间 · 模块标签 · 次数。
            //
            // ⚠ 这里**必须用 montserrat_12**（纯 ASCII 字体），因为内容全是
            // ASCII：时间数字走不了中文字库。**反过来不行** —— montserrat
            // 没有汉字字形，把"错误/警告/信息"这种中文塞进 montserrat 的 label
            // 会一个字都画不出来（字体回退只从主字体指向它配的 fallback，
            // 不会反向生效）。所以级别字样统一放到下面那行中文里。
            lv_obj_t* l1 = lv_label_create(row);
            mkLabel(l1, &lv_font_montserrat_12, CLR_TEXT_MUTE);
            lv_label_set_text(l1, "");
            lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 10, 3);

            // 第二行：**级别字样 + 正文**（16px 中文）。
            //
            // ⚠ 这里**只能放一行**。原来写的是 LONG_WRAP、注释说"可折两行"，那是错的：
            //   行容器只有 ELOG_ROW_H-2 = 42px 高，第一行(12px 字体)在 y=3 占到 y~17，
            //   正文从 y=17 开始，16px 中文行高 19 —— 两行要占到 y=55，直接超出容器，
            //   第二行被裁掉一半，屏幕上看到的就是"第一行正常、第二行残缺"。
            //   （之前把 ELOG_ROW_H 从 58 压到 44 就是为了塞进 3 行，压的时候没同步
            //     收掉"折两行"的余量。）
            //
            // 宽度也只有 166px 可用：206px 减去级别字样"信息"(2 个汉字 32px) 加空格。
            // 所以正文超长时**用省略号截断**（显式给高度才会触发 DOT），而不是让它
            // 折行 —— 折行截断出来的半截字比"..."更难认。
            //
            // ⚠ 因此**日志正文一律要写短**：一行 ≈ 10 个汉字 / 20 个 ASCII 字符。
            lv_obj_t* l2 = lv_label_create(row);
            mkLabel(l2, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(l2, "");
            lv_obj_set_width(l2, 206);
            lv_obj_set_height(l2, 19);          // 给死高度，DOT 模式才会截断而不是撑高
            lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
            lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 10, 17);

            elog_row[i] = row;
        }
    }

    elogScroll = 0;
    elogSel = 0;
    elogArmClear = false;
    elogRefresh();
    showScreen(scr_elog);
    currentSysMode = SYS_MODE_ELOG;
}

// 日志页按键：↑↓ 翻、DEL 清空（两次确认）、ESC/MC 回菜单
static void elogKey(uint16_t baseKey) {
    if (baseKey == KEY_UP_ARROW) {
        if (elogSel < ELOG_SEL_MAX) elogSel++;
        elogArmClear = false;
    } else if (baseKey == KEY_DOWN_ARROW) {
        if (elogSel > 0) elogSel--;
        elogArmClear = false;
    } else if (baseKey == KEY_PAGE_UP) {
        elogSel = (elogSel + ELOG_ROWS < ELOG_SEL_MAX) ? elogSel + ELOG_ROWS : ELOG_SEL_MAX;
        elogArmClear = false;
    } else if (baseKey == KEY_PAGE_DOWN) {
        elogSel = (elogSel >= ELOG_ROWS) ? elogSel - ELOG_ROWS : 0;
        elogArmClear = false;
    } else if (baseKey == KEY_DELETE) {
        // 清空是不可撤销的，所以要按两次：第一次只提示，第二次才真删。
        if (!elogArmClear) {
            elogArmClear = true;
            triggerHudBig("清空日志", "再按一次删除键", lv_color_hex(CLR_AMBER));
            return;
        }
        elog_clear();
        elogScroll = 0;
        elogSel = 0;
        elogArmClear = false;
        elogRefresh();
        triggerHud("错误日志", "已清空", lv_color_hex(CLR_GREEN));
    } else if (baseKey == KEY_ESC || baseKey == K_MC) {
        elogArmClear = false;
        // 回**菜单**而不是主屏：这一页是从菜单进来的，回菜单顺手还能点别的项
        currentSysMode = SYS_MODE_MENU;
        build_menu();
    } else {
        elogArmClear = false;   // 其它键顺手解除待确认状态
    }
    elogRefresh();
}

// ===========================
// 按键速查（菜单里的对话框）
// ===========================
//
// 动机：这块板子上的功能已经多到记不住了 ——
//   灯光键  短按切控制目标 / 长按切背光 / 响铃时停铃
//   静音键  短按静音 / 双击锁键盘 / 长按整机重启
//   CPG 键 短按切灯效 / 双击重启扩展芯片
//   旋钮    旋转调当前目标 / 按下确认
//   LOGO    短按切主屏风格 / 长按进宠物页
//   Fn+LOGO 解锁键盘
// 全靠记忆迟早要出事，所以做成一张表放在菜单里，随时翻。
//
// 形态：**独立屏幕**，但画成"浮在暗底上的对话框"（深底 + 居中卡片 + 强调色描边）。
//   为什么不做成菜单屏的子对象（那样菜单能透出来，更像真模态）：
//   菜单屏会被 build_menu() 重排、被换屏换掉，浮层挂在它身上等于把生命周期
//   绑在别人身上；更要命的是从菜单退回主屏 / 进屏保之后，浮层会"跟着复活"——
//   表现是"关掉速查表之后，下次打开菜单它又自己冒出来了"。
//   独立屏幕和错误日志页一个套路：showScreen() 切进来，关掉时它还活着，下次直接复用。
//
// 滚动是**手动窗口**（elogScroll 那套）：LVGL 自带的滚动条要靠手指/滚轮驱动，
// 这块板子上只有旋钮，拧不动它。

#define HELP_HEAD   0
#define HELP_ENTRY  1

struct HelpItem {
    uint8_t     kind;   // HELP_HEAD = 分组标题，HELP_ENTRY = 一条速查
    const char* key;    // 左列：按什么
    const char* act;    // 右列：会发生什么
};

// 内容表。**改这里就行**，排版、滚动、位置指示都是自动的。
//
// ⚠ 宽度是**量出来**的，不是估的（text_width.py 读的是真实字库的 adv_w）：
//   左列最宽 "静音键 短按"  = 88px（列宽 92）
//   右列最宽 "锁定/解锁键盘" = 104px（列宽 108）
// 写长了会被省略号截断（不会画到屏幕外，但后半句会被砍掉 —— 所以文案要短）。
// 改文案之后先跑一遍 `text_width.py --margin 92 "..."` 核一下。
static const HelpItem helpItems[] = {
    { HELP_HEAD,  "C3 实体键", "" },
    { HELP_ENTRY, "静音键 短按", "静音切换" },
    { HELP_ENTRY, "静音键 双击", "锁定/解锁键盘" },
    { HELP_ENTRY, "静音键 长按", "整机重启" },
    { HELP_ENTRY, "灯光键 短按", "切控制目标" },
    { HELP_ENTRY, "灯光键 长按", "背光总开关" },
    { HELP_ENTRY, "CPG键 短按", "切换灯效" },
    { HELP_ENTRY, "CPG键 双击", "重启扩展芯片" },
    { HELP_HEAD,  "旋钮", "" },
    { HELP_ENTRY, "旋转", "调下面的目标" },
    { HELP_ENTRY, "按下", "确认/保存" },
    { HELP_ENTRY, "键盘背光", "本机亮度" },
    { HELP_ENTRY, "屏幕亮度", "主机亮度" },
    { HELP_ENTRY, "系统音量", "主机音量" },
    { HELP_ENTRY, "灯效", "灯光效果" },
    { HELP_HEAD,  "矩阵键", "" },
    { HELP_ENTRY, "LOGO 短按", "切主屏风格" },
    { HELP_ENTRY, "LOGO 长按", "宠物全屏页" },
    { HELP_ENTRY, "Fn+LOGO", "解锁键盘" },
    { HELP_ENTRY, "MC", "系统菜单" },
    { HELP_ENTRY, "MR", "宏录制" },
    { HELP_ENTRY, "MA/MB", "全局键" },
    { HELP_ENTRY, "Play", "播放/暂停" },
};
#define HELP_ITEM_COUNT  ((int)(sizeof(helpItems) / sizeof(HelpItem)))

// 版面。卡片 232x216 居中在 240x240 上（上下各留 12px）。
#define HELP_CARD_W      232
#define HELP_CARD_H      216
#define HELP_VISIBLE     7       // 一屏显示几行
#define HELP_ROW_H       22      // 行距（16px 中文字库 line_height = 19，留 3px 缝）
#define HELP_KEY_W       92      // 左列宽（最宽的一条"静音键 短按"实测 88px）
#define HELP_ACT_W       108     // 右列宽（最宽的一条"锁定/解锁键盘"实测 104px）

static lv_obj_t* scr_help     = nullptr;
static lv_obj_t* helpCont     = nullptr;
static lv_obj_t* helpPosLbl   = nullptr;
static lv_obj_t* helpRows[HELP_VISIBLE];
static int       helpScroll   = 0;    // 窗口第一条的下标

// 只在宽度真的变了才写。lv_obj_set_width() 会 invalidate + 重排版，
// 翻一页要写 14 次（7 行 × 2 列），白白把屏幕刷脏。
static void setLabelWidth(lv_obj_t* l, lv_coord_t w) {
    if (l == nullptr) return;
    if (lv_obj_get_width(l) == w) return;
    lv_obj_set_width(l, w);
}

// 刷新可见窗口里的每一行
static void helpRefresh(void) {
    if (scr_help == nullptr) return;

    int total = HELP_ITEM_COUNT;
    int maxScroll = total - HELP_VISIBLE;
    if (maxScroll < 0) maxScroll = 0;
    if (helpScroll > maxScroll) helpScroll = maxScroll;
    if (helpScroll < 0) helpScroll = 0;

    for (int i = 0; i < HELP_VISIBLE; i++) {
        lv_obj_t* row = helpRows[i];
        if (row == nullptr) continue;
        int idx = helpScroll + i;
        if (idx >= total) { setHidden(row, true); continue; }
        setHidden(row, false);

        const HelpItem* it = &helpItems[idx];
        lv_obj_t* bar  = lv_obj_get_child(row, 0);
        lv_obj_t* klbl = lv_obj_get_child(row, 1);
        lv_obj_t* albl = lv_obj_get_child(row, 2);

        if (it->kind == HELP_HEAD) {
            // 分组标题：左边一道琥珀色竖条 + 琥珀色标题，占满整行
            if (bar) {
                lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_AMBER), LV_PART_MAIN);
            }
            setText(klbl, it->key);
            setLabelWidth(klbl, 200);
            setTextColor(klbl, CLR_AMBER);
            setHidden(albl, true);
        } else {
            if (bar) lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            setText(klbl, it->key);
            setLabelWidth(klbl, HELP_KEY_W);
            setTextColor(klbl, CLR_ACCENT);
            setText(albl, it->act);
            setHidden(albl, false);
        }
    }

    // 右上角位置指示：现在显示的是第几条到第几条 / 一共几条
    if (helpPosLbl) {
        int first = helpScroll + 1;
        int last  = helpScroll + HELP_VISIBLE;
        if (last > total) last = total;
        static char pb[16];
        snprintf(pb, sizeof(pb), "%d-%d/%d", first, last, total);
        setText(helpPosLbl, pb);
        // 文本变了宽度也会变（"1-7/23" → "17-23/23"），重新居中一次。
        // align 是样式属性、LVGL 自己在重排时也会重算，但这里显式来一次
        // 最省心：胶囊就一个元素，重算一次的开销可以忽略。
        lv_obj_center(helpPosLbl);
    }
}

// 翻页。d 为正往下翻，为负往上翻。
static void helpScrollBy(int d) {
    int total = HELP_ITEM_COUNT;
    int maxScroll = total - HELP_VISIBLE;
    if (maxScroll < 0) maxScroll = 0;
    int s = helpScroll + d;
    if (s < 0) s = 0;
    if (s > maxScroll) s = maxScroll;
    if (s == helpScroll) return;
    helpScroll = s;
    helpRefresh();
}

// 关掉对话框回到菜单
static void helpClose(void) {
    currentSysMode = SYS_MODE_MENU;
    // 菜单屏一直活着（从来没被删过），切回去就行，不用重建。
    showScreen(scr_menu);
}

// 建速查对话框
static void build_help(void) {
    if (scr_help == nullptr) {
        scr_help = lv_obj_create(NULL);
        // 对话框底：比页面底色更深，让卡片"浮"起来
        lv_obj_set_style_bg_color(scr_help, lv_color_hex(0x05070C), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr_help, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr_help, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr_help, 0, LV_PART_MAIN);
        lv_obj_clear_flag(scr_help, LV_OBJ_FLAG_SCROLLABLE);

        // 卡片
        lv_obj_t* card = lv_obj_create(scr_help);
        lv_obj_set_size(card, HELP_CARD_W, HELP_CARD_H);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
        mkCard(card, CLR_SURFACE, 12);
        // 描边换成强调色暗部：这是一张"浮出来的速查表"，跟普通设置页要能一眼分开
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_ACCENT_D), LV_PART_MAIN);

        // 标题
        lv_obj_t* title = lv_label_create(card);
        mkLabel(title, &lv_font_simsun_16_cjk, CLR_TEXT);
        lv_label_set_text(title, "按键速查");
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

        // 右上角位置胶囊。60px 宽是照最宽的一串"17-23/23"（montserrat_12
        // 约 51px）定的，再宽就不必要了。
        lv_obj_t* chip = lv_obj_create(card);
        lv_obj_set_size(chip, 60, 18);
        lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -10, 8);
        mkChip(chip, CLR_SURFACE_2, CLR_TEXT_DIM, 9);
        helpPosLbl = lv_label_create(chip);
        mkLabel(helpPosLbl, &lv_font_montserrat_12, CLR_TEXT_DIM);
        // 先塞一个**同样 6 个字符**的占位（"0-0/00" vs 真的 "1-7/23"）：
        // montserrat 是等宽数字，宽高完全一致，label 的尺寸从建出来那一刻
        // 就是对的，lv_obj_center() 算出来的居中位置不会被后来的文本改动带歪。
        // （真正的值由 helpRefresh() 在 showScreen() 之前写好，屏上不会出现占位。）
        lv_label_set_text(helpPosLbl, "0-0/00");
        lv_obj_center(helpPosLbl);

        // 列表容器：只裁剪，不画底
        helpCont = lv_obj_create(card);
        lv_obj_set_size(helpCont, 216, HELP_VISIBLE * HELP_ROW_H);
        lv_obj_align(helpCont, LV_ALIGN_TOP_LEFT, 8, 32);
        lv_obj_set_style_bg_opa(helpCont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(helpCont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(helpCont, 0, LV_PART_MAIN);
        lv_obj_clear_flag(helpCont, LV_OBJ_FLAG_SCROLLABLE);

        // HELP_VISIBLE 行，建一次，之后只改文字和配色
        for (int i = 0; i < HELP_VISIBLE; i++) {
            lv_obj_t* row = lv_obj_create(helpCont);
            lv_obj_set_size(row, 216, HELP_ROW_H - 2);
            lv_obj_set_pos(row, 0, i * HELP_ROW_H);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            // 分组标题左边那道竖条（普通条目里是隐藏的）
            lv_obj_t* bar = lv_obj_create(row);
            lv_obj_set_size(bar, 3, 12);
            lv_obj_set_pos(bar, 2, 3);
            lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_AMBER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
            lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

            // 左列：按什么
            lv_obj_t* klbl = lv_label_create(row);
            mkLabel(klbl, &lv_font_simsun_16_cjk, CLR_ACCENT);
            lv_label_set_text(klbl, "");
            lv_obj_set_width(klbl, HELP_KEY_W);
            lv_label_set_long_mode(klbl, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(klbl, 10, 0);

            // 右列：会发生什么
            lv_obj_t* albl = lv_label_create(row);
            mkLabel(albl, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(albl, "");
            lv_obj_set_width(albl, HELP_ACT_W);
            lv_label_set_long_mode(albl, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(albl, 10 + HELP_KEY_W + 4, 0);

            helpRows[i] = row;
        }

        // 底部提示
        lv_obj_t* hint = lv_label_create(card);
        mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(hint, "旋钮↑↓ 翻看 · ESC 关闭");
        lv_obj_set_width(hint, 216);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    helpScroll = 0;
    helpRefresh();
    showScreen(scr_help);
    currentSysMode = SYS_MODE_HELP;
}

// 速查页按键：↑↓/翻页键翻页，ESC·MC 关闭
static void helpKey(uint16_t baseKey) {
    if (baseKey == KEY_UP_ARROW) {
        helpScrollBy(-1);
    } else if (baseKey == KEY_DOWN_ARROW) {
        helpScrollBy(1);
    } else if (baseKey == KEY_PAGE_UP) {
        helpScrollBy(-HELP_VISIBLE);
    } else if (baseKey == KEY_PAGE_DOWN) {
        helpScrollBy(HELP_VISIBLE);
    } else if (baseKey == KEY_ESC || baseKey == K_MC) {
        helpClose();
    }
}

static void finishMacroRecording(const String& targetKey) {
    if (recKeyCount == 0) {
        triggerHud("宏录制", "没有录到按键", lv_color_hex(CLR_RED));
        gotoMainScreen();
        return;
    }

    // 构建宏数据字符串。格式必须和 executeSequenceAction / executeComboAction
    // 的解析方式对上（也和网页端下发的那套保持一致）：
    //   SEQ → 可打印字符原样拼，回车/Tab/Esc/退格拼成 [ENTER] [TAB] [ESC] [BACKSPACE]
    //         （直接把 KEY_RETURN=0xB0 那种原始码写进去，播放时会被当成
    //          不可打印字符丢掉 —— 录了半天回车，回放时回车没了）
    //   CMB → 十进制码点逗号分隔，例 "128,129,65"。以前这里写的是 %02X 十六进制
    //         带尾逗号（"C0,1F,"），而播放端按十进制读，"C0" 解析成 0 → 一个键都不按。
    String macroData;
    if (recStage == REC_STAGE_SEQ) {
        macroData = "SEQ:";
        for (int i = 0; i < recKeyCount; i++) {
            uint16_t c = recKeyBuffer[i];
            if (c == KEY_RETURN)         macroData += "[ENTER]";
            else if (c == KEY_TAB)       macroData += "[TAB]";
            else if (c == KEY_ESC)       macroData += "[ESC]";
            else if (c == KEY_BACKSPACE) macroData += "[BACKSPACE]";
            else if (c >= 32 && c <= 126) macroData += (char)c;
            // 其余（修饰键/方向键/F 键）SEQ 模式下表达不了，跳过
        }
    } else {
        macroData = "CMB:";
        for (int i = 0; i < recKeyCount; i++) {
            macroData += String(recKeyBuffer[i]);
            if (i < recKeyCount - 1) macroData += ",";
        }
    }

    // 保存宏
    char pKey[32];
    snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, targetKey.c_str());
    preferences.putString(pKey, macroData);

    triggerHud("宏已保存", targetKey.c_str(), lv_color_hex(CLR_GREEN));

    // 清理录制状态
    recKeyCount = 0;
    gotoMainScreen();
}

// ===========================
// 设置界面控制函数
// ===========================
static void moveSettingField(int dir) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME:
            timeFieldIdx = (timeFieldIdx + dir + 5) % 5;
            update_setting_time_display();
            break;
        case SYS_MODE_SET_ALARM:
            // 三个字段了（时/分/开关），模数跟着从 2 改成 3
            alarmFieldIdx = (alarmFieldIdx + dir + 3) % 3;
            update_setting_alarm_display();
            break;
        case SYS_MODE_SET_TIMER:
            if (!timerRunning) {
                timerFieldIdx = (timerFieldIdx + dir + 3) % 3;
                update_setting_timer_display();
            }
            break;
        case SYS_MODE_SET_LIGHT:
            lightFieldIdx = (uint8_t)((lightFieldIdx + dir + LIGHT_FIELD_COUNT) % LIGHT_FIELD_COUNT);
            update_setting_light_display();
            break;
    }
}

static void adjustSettingField(int delta) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME: {
            switch (timeFieldIdx) {
                case 0: timeEditY = constrain(timeEditY + delta, 2020, 2099); break;
                case 1: timeEditMo = constrain(timeEditMo + delta, 1, 12); break;
                case 2: timeEditD = constrain(timeEditD + delta, 1, 31); break;
                case 3: timeEditH = constrain(timeEditH + delta, 0, 23); break;
                case 4: timeEditMi = constrain(timeEditMi + delta, 0, 59); break;
            }
            update_setting_time_display();
            break;
        }
        case SYS_MODE_SET_ALARM: {
            switch (alarmFieldIdx) {
                case 0: alarmEditH = constrain(alarmEditH + delta, 0, 23); break;
                case 1: alarmEditM = constrain(alarmEditM + delta, 0, 59); break;
                // ↑/↓ 翻开关。delta 是 ±1，这里只用它的正负，翻一次就够 ——
                // 按住不放连翻也只会"开→关→开"，正好是想要的手感。
                case 2: alarmEditOn = !alarmEditOn; break;
            }
            update_setting_alarm_display();
            break;
        }
        case SYS_MODE_SET_TIMER: {
            if (!timerRunning) {
                switch (timerFieldIdx) {
                    case 0: timerEditH = constrain(timerEditH + delta, 0, 23); break;
                    case 1: timerEditM = constrain(timerEditM + delta, 0, 59); break;
                    case 2: timerEditS = constrain(timerEditS + delta, 0, 59); break;
                }
                timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
                timerRemainSec = timerTotalSec;
                update_setting_timer_display();
            }
            break;
        }
        case SYS_MODE_CAL_TEMP: {
            shtTempOffset = constrain(shtTempOffset + delta * 0.5f,
                                      SHT_TEMP_OFFSET_MIN, SHT_TEMP_OFFSET_MAX);
            update_setting_caltemp_display();
            break;
        }
        case SYS_MODE_SET_LIGHT: {
            switch (lightFieldIdx) {
                case 0:
                    lightOn = !lightOn;
                    break;
                case 1:
                    // 20/255 一档，和原版 ↑↓ 调背光的手感一致
                    brightness = (uint8_t)constrain((int)brightness + delta * 20, 0, 255);
                    break;
                case 2:
                    currentEffect = (uint8_t)((currentEffect + (delta > 0 ? 1 : MAX_EFFECTS - 1))
                                              % MAX_EFFECTS);
                    break;
                case 3:
                    indLevel = (uint8_t)constrain((int)indLevel + delta, 0, IND_LEVEL_COUNT - 1);
                    indBrightness = indLevelValues[indLevel];
                    break;
                default:
                    // 按键灯效：循环量，关掉时顺手把正在跑的动画停掉，
                    // 否则下次开回来会接着上一条没收完的波纹继续跑
                    keyFxStyle = (uint8_t)((keyFxStyle + (delta > 0 ? 1 : KEYFX_COUNT - 1))
                                           % KEYFX_COUNT);
                    if (keyFxStyle == KEYFX_OFF) keyFxActive = false;
                    break;
            }
            update_setting_light_display();
            break;
        }
    }
}

static void saveSettingScreen(void) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME: {
            struct tm t = {0};
            t.tm_year = timeEditY - 1900;
            t.tm_mon = timeEditMo - 1;
            t.tm_mday = timeEditD;
            t.tm_hour = timeEditH;
            t.tm_min = timeEditMi;
            t.tm_sec = 0;
            time_t epoch = mktime(&t);
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            preferences.putUInt("set_epoch", epoch);
            break;
        }
        case SYS_MODE_SET_ALARM: {
            alarmHour = alarmEditH;
            alarmMinute = alarmEditM;
            // 取编辑态，不再从那个点不动的 lv_switch 上读。
            // 以前读开关：整机没触屏、全工程没有 add_event_cb，那开关的状态
            // 只会是进页面时镜像进来的 alarmEnabled —— 也就是说"在闹钟页里
            // 改开关"这件事改了个寂寞，保存时又把它盖回原值。
            alarmEnabled = alarmEditOn;
            preferences.putUChar("alarm_h", alarmHour);
            preferences.putUChar("alarm_m", alarmMinute);
            preferences.putBool("alarm_on", alarmEnabled);
            break;
        }
        case SYS_MODE_SET_TIMER: {
            timerRunning = false;
            timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
            timerRemainSec = timerTotalSec;
            break;
        }
        case SYS_MODE_CAL_TEMP: {
            preferences.putFloat("sht_offset", shtTempOffset);
            break;
        }
        case SYS_MODE_SET_LIGHT: {
            preferences.putBool("light_on", lightOn);
            preferences.putUChar("brightness", brightness);
            preferences.putUChar("effect", currentEffect);
            preferences.putUChar("ind_level", indLevel);
            preferences.putUChar("key_fx", keyFxStyle);
            break;
        }
    }
    gotoMainScreen();
}

static void cancelSettingScreen(void) {
    // 不弹"已取消"：设置页关掉、值回到原样，这就是反馈。
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME:
            break;
        case SYS_MODE_SET_ALARM:
            break;
        case SYS_MODE_SET_TIMER:
            timerRunning = false;
            triggerHud("倒计时", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_CAL_TEMP:
            shtTempOffset = calTempOriginal;
            break;
        case SYS_MODE_SET_LIGHT:
            // 五项全部原样还回去：灯效/亮度这类循环量不记原值就退不回去了
            lightOn = lightOnOriginal;
            brightness = lightBrightOriginal;
            currentEffect = lightEffectOriginal;
            indLevel = lightIndLevelOriginal;
            indBrightness = indLevelValues[indLevel];
            keyFxStyle = lightKeyFxOriginal;
            if (keyFxStyle == KEYFX_OFF) keyFxActive = false;
            break;
    }
    gotoMainScreen();
}

// ===========================
// Profile 管理
// ===========================
// 定义在下面的"键盘工具函数"一节。这里先声明：loadRemapsFromStorage() 读 NVS
// 时就要它把老配置里 224~231 那套 HID 编码折算成 Arduino 风格。
static inline uint16_t normalizeRemapKey(uint16_t code);

void loadRemapsFromStorage() {
    int total = 0;
    for (int p = 0; p < TOTAL_PROFILES; p++) {
        char key[16];
        snprintf(key, sizeof(key), "rmp_cnt_%d", p);
        remapCounts[p] = preferences.getInt(key, 0);
        if (remapCounts[p] > MAX_REMAP_RULES) remapCounts[p] = MAX_REMAP_RULES;
        for (int i = 0; i < remapCounts[p]; i++) {
            char itemKey[20];
            snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", p, i);
            uint32_t val = preferences.getUInt(itemKey, 0);
            // 读出来也过一遍 normalize：以前存进去的规则里 toKey 可能是 224~231
            // 那套 HID 编码，不折算的话老配置改完固件照样发不出修饰键。
            profileRemaps[p][i].fromKey = normalizeRemapKey((uint16_t)(val >> 16));
            profileRemaps[p][i].toKey = normalizeRemapKey((uint16_t)(val & 0xFFFF));
        }
        total += remapCounts[p];
    }
    // 开机把"NVS 里到底还剩几条"喊出来。断电后配置消失时，这一行就是判据：
    // 这里报 0 而关机前屏幕显示过"已保存"，说明写盘那步没成（分区满了 /
    // 写失败被忽略），而不是重启逻辑有问题。
    if (total > 0) ELINFO("REMAP", "开机从NVS恢复 %d 条", total);
    else          ELINFO("REMAP", "开机NVS里没有任何映射");
}

void switchProfile(uint8_t profIdx) {
    if (profIdx >= TOTAL_PROFILES) return;
    currentProfile = profIdx;
    preferences.putUChar("curr_prof", profIdx);
    // 顶部条的序号 + 系统图标立刻跟上。等 updateDynamicElements() 那 100ms 也行，
    // 但它开头有"系统时间未校准就 return"的分支，靠它会出现图标不跟着换。
    updateTopBarProfile();
    // 「方案+按键」页的图标不在顶栏（它那一页是自带的顶栏），得单独刷。
    updatePlanKeyProfile();
}

// ===========================
// 键盘工具函数
// ===========================
// 修饰键有**两套**编码——注意这里只能认 Arduino 那一套：
//   0x80~0x87 = Arduino 风格（KEY_LEFT_CTRL=128…KEY_RIGHT_GUI=135），
//               按键矩阵、网页"组合键"复选框用的都是这套，需要 +0x60 转成 HID usage。
//   0xE0~0xE7 = 网页"按键映射"下拉里那几个 "🔹 Left Ctrl"（见 s3-setting.html 的
//               KEY_OPTIONS: 224~231）。**这一套绝不能在这里认**：矩阵里的 0xE0~0xE7
//               走的是 Arduino 的 "0x88 + usage" 约定，是**小键盘**键位 ——
//                  0xE0=小键盘回车 0xE1~0xE7=小键盘 1~7
//               （Row0[1..3]=0xE9/0xE8/0xE7 是小键盘 9/8/7，Row1[10..15] 是小键盘 3~6…）
//               一旦在这里把 0xE0~0xE7 当修饰键，小键盘 7 就会变成 Win 键（0xE7→右 GUI）、
//               1~6 变成 Shift/Alt/Ctrl —— 整片小键盘全错。
//               那套值的转换放在 remap 表读入处做（见 normalizeRemapKey）。
//
// 0x53~0x63 这段原本被当成 HID 小键盘 usage 走 pressRaw(因为 _asciimap[0x54]='T' 等),
// 但 ASCII 'a'/'b'/'c' = 0x61/0x62/0x63 跟 HID Num9/Num0/Num. 是**同一个数字**,
// baseMatrix 里这些 ASCII 字母走这条分支会被发成小键盘数字 ——
// 这是基础键盘按 A 出 9、按 B 出 0、按 C 出 . 的根因。
//
// 第一版修法是"把 CMB 小键盘的编码从 HID Usage(0x53-0x63)统一改成 ASCII"
// —— 数值撞车确实没了,但**矫枉过正**:ASCII 走的是 _asciimap,发出来的是
// **主键盘**上打出同一个字符的键位组合,不是小键盘那颗键:
//   Num * = '*' = 0x2A → _asciimap[0x2A] = 0x25|SHIFT → 主键盘 Shift+8  ✗
//   Num + = '+' = 0x2B → _asciimap[0x2B] = 0x2E|SHIFT → 主键盘 Shift+=  ✗
//   Num / = '/' = 0x2F → _asciimap[0x2F] = 0x38         → 主键盘 /       ✗
// 用户要的是**小键盘**那颗 *（HID usage 0x55），只有 pressRaw 才发得出来。
//
// 所以小键盘符号键改用 0xDC~0xDF —— 这是"HID 小键盘 usage + 0x88"，也就是
// **键盘矩阵里小键盘那一片本来的编码**：
//   Num / = 0x88+0x54 = 0xDC    Num * = 0x88+0x55 = 0xDD
//   Num - = 0x88+0x56 = 0xDE    Num + = 0x88+0x57 = 0xDF
// 走 kbPress 的 >=0x88 分支被减回 0x54~0x57 交给 pressRaw，发出来就是真正的小键盘键；
// 同时这段和 ASCII 'a'/'b'/'c' 不撞车，A→9 那个 bug 也不会回来。
// 效果上等价于"宏里选 Num *" == "手按物理小键盘的 * 键"，两边发同一个码点。
//
// 小键盘数字(1-9/0/.)保持 ASCII：_asciimap 翻成主键盘数字，字符是对的（'1' 还是 '1'）；
// 而 0xE0~0xE7 那段和网页"🔹 修饰键"224~231 撞车，走 normalizeRemapKey 会被
// 折算成 Ctrl/Shift，反而更糟。Num Enter 用 '\n' 同样能得到回车，一并留在 ASCII 侧。
//
// 网页那边对应的定义在 s3-setting.html 的 KEY_OPTIONS（"Num /":220 ~ "Num +":223），改一处要改两处。
// 把 mappedKey 发给主机。
//   修饰键 (Arduino 0x80~0x87) 不能走 press(),因为 _asciimap[0x80~0x87]=0 会被吞;
//   改用 pressRaw 直接发 HID Usage (0xE0~0xE7)。其它键 (ASCII / HID Usage>=0x88)
//   Keyboard.press() 自己处理。
//
// "交换两个键"的实现: 例如设置 ctrl->win; win->ctrl 两条规则,
// getMappedKey 单步查表 ——
//   按 ctrl  → mappedKey=0x83(KEY_LEFT_GUI)  → pressRaw(0xE3) → 发 Win ✓
//   按 win   → mappedKey=0x80(KEY_LEFT_CTRL) → pressRaw(0xE0) → 发 Ctrl ✓
// 两条规则并存、各自独立,不互相"抵消"。
static inline void kbPress(uint8_t code) {
    if (code == 0) {
        LOG_PORT.printf("[KB] press code=0x00 path=skip(null)\n");
        return;
    }
    if (code >= 0x80 && code < 0x88) {
        // 修饰键 (Arduino 0x80~0x87) → HID Usage (0xE0~0xE7)
        Keyboard.pressRaw((uint8_t)(code + 0x60));
        LOG_PORT.printf("[KB] press code=0x%02X path=pressRaw(mod+0x60)\n", code);
    } else {
        Keyboard.press(code);
        LOG_PORT.printf("[KB] press code=0x%02X path=press(asciimap)\n", code);
    }
}

static inline void kbRelease(uint8_t code) {
    if (code == 0) return;
    if (code >= 0x80 && code < 0x88) Keyboard.releaseRaw((uint8_t)(code + 0x60));
    else Keyboard.release(code);
}

// 网页"按键映射"下拉里的修饰键给的是 HID 风格 224~231，而 Keyboard.press() 按
// ">= 0x88 就是非打印键" 处理，会算成 (224-0x88)=0x88 这个不存在的 usage ——
// 主机收到的既不是 Ctrl 也不是别的键。在写进 remap 表之前统一折算成 Arduino 风格，
// 这样下游 kbPress/getMappedKey 都不用再管这套编码。
static inline uint16_t normalizeRemapKey(uint16_t code) {
    return (code >= 0xE0 && code <= 0xE7) ? (uint16_t)(code - 0x60) : code;
}

// ===========================
// 按键名（给"最近按键"和录制列表用）
// ===========================
// 之前这里是直接 snprintf("%02X") 十六进制码，界面上就是一串数字 —— 空格显示 20、
// 字母显示 6B，纯粹没法看。照搬 s3.ino 原版的 getKeyName()。
static const char* getKeyName(uint16_t code) {
    // 可打印 ASCII 原样成字符。字母/数字/符号键在矩阵里存的就是 ASCII
    // （'/'=47、';'=59、'.'=46 …），所以一条就够了。
    // ⚠ 必须排在下面的 switch 之前：那里面 `case 0x2F: return "["` 一类用的是
    // **HID usage 码**，和 ASCII 撞车（0x2E 既是 HID 的 '='、又是 ASCII 的 '.'），
    // 撞上就会把 '.' 显示成 '='。矩阵里存的是 ASCII，以 ASCII 为准。
    if (code == ' ') return "Space";
    if (code >= 32 && code <= 126) {
        static char b[2];
        b[0] = (char)code;
        b[1] = 0;
        return b;
    }
    switch (code) {
        case KEY_LEFT_CTRL: case KEY_RIGHT_CTRL:   return "Ctrl";
        case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return "Shift";
        case KEY_LEFT_ALT: case KEY_RIGHT_ALT:     return "Alt";
        case KEY_LEFT_GUI: case KEY_RIGHT_GUI:      return "Win";
        case KEY_RETURN:      return "Enter";
        case KEY_ESC:         return "Esc";
        case KEY_BACKSPACE:   return "Back";
        case KEY_TAB:         return "Tab";
        case ' ':             return "Space";
        case KEY_UP_ARROW:    return "UP";
        case KEY_DOWN_ARROW:  return "DOWN";
        case KEY_LEFT_ARROW:  return "LEFT";
        case KEY_RIGHT_ARROW: return "RIGHT";
        case KEY_INSERT:      return "Ins";
        case KEY_DELETE:      return "Del";
        case KEY_HOME:        return "Home";
        case KEY_END:         return "End";
        case KEY_PAGE_UP:     return "PgUp";
        case KEY_PAGE_DOWN:   return "PgDn";
        case KEY_CAPS_LOCK:   return "Caps";
        case 0x65:            return "Menu";      // 菜单键，USBHIDKeyboard.h 里没有对应宏
        case KEY_PRINT_SCREEN:return "PrtSc";
        case KEY_SCROLL_LOCK: return "Scrlk";
        case KEY_PAUSE:       return "Pause";
        case KEY_NUM_LOCK:    return "NumLk";
        case KEY_KP_SLASH:    return "Num /";
        case KEY_KP_ASTERISK: return "Num *";
        case KEY_KP_MINUS:    return "Num -";
        case KEY_KP_PLUS:     return "Num +";
        case KEY_KP_ENTER:    return "NumEnt";
        case KEY_KP_0: case KEY_KP_1: case KEY_KP_2: case KEY_KP_3: case KEY_KP_4:
        case KEY_KP_5: case KEY_KP_6: case KEY_KP_7: case KEY_KP_8: case KEY_KP_9: {
            static char b[6];
            snprintf(b, sizeof(b), "Num %c", (code == KEY_KP_0) ? '0' : (char)('1' + (code - KEY_KP_1)));
            return b;
        }
        case KEY_KP_DOT:      return "Num .";
        // 上面这组 KEY_KP_*(0x53~0x63)、KEY_NUM_LOCK 是 **HID usage 码**，
        // 网页的按键下拉和 s3.ino 原版都按这套写的，保留给它们用。
        // 但**按键矩阵里存的不是这套**：矩阵用的是 Arduino 的 "0x88 + usage"，
        // 小键盘实际落在 0xDB~0xEB 这一段（0x88+0x53=0xDB = NumLock，
        // 0x88+0x58=0xE0 = 小键盘回车，0x88+0x5F=0xE7 = 小键盘 7 …）。
        // 不补下面这几行的话，"最近按键"和录制列表里小键盘会显示成 Ctl1B / E7 这种鬼东西。
        case 0xCE: return "PrtSc";    // 0x88+0x46
        case 0xCF: return "Scrlk";    // 0x88+0x47
        case 0xD0: return "Pause";    // 0x88+0x48
        case 0xDB: return "NumLk";    // 0x88+0x53
        case 0xDC: return "Num /";    // 0x88+0x54
        case 0xDD: return "Num *";    // 0x88+0x55
        case 0xDE: return "Num -";    // 0x88+0x56
        case 0xDF: return "Num +";    // 0x88+0x57
        case 0xE0: return "NumEnt";   // 0x88+0x58
        case 0xEA: return "Num 0";    // 0x88+0x62
        case 0xEB: return "Num .";    // 0x88+0x63
        case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        case 0xE6: case 0xE7: case 0xE8: case 0xE9: {   // 0x88+0x59~0x61 = 小键盘 1~9
            static char b[7];
            snprintf(b, sizeof(b), "Num %u", (unsigned)(code - 0xE0));
            return b;
        }
        case 0xED: return "Menu";     // 0x88+0x65 应用/菜单键
        // 下面这组符号键 USBHIDKeyboard.h 里没有宏，直接用 HID Usage ID
        case 0x2F: return "[";       // [
        case 0x30: return "]";       // ]
        case 0x31: return "\\";      // \
        case 0x33: return ";";       // ;
        case 0x34: return "'";       // '
        case 0x35: return "`";       // `
        case 0x36: return ",";       // ,
        case 0x37: return ".";       // .
        case 0x38: return "/";       // /
        case 0x2D: return "-";
        case 0x2E: return "=";
        default: break;
    }
    if (code >= KEY_F1 && code <= KEY_F12) {
        static char b[4];
        snprintf(b, sizeof(b), "F%u", (unsigned)(code - KEY_F1 + 1));
        return b;
    }
    // 宏键 / 功能键
    if (code == K_LOGO) return "LOGO";
    if (code == K_PLAY) return "Play";
    if (code == K_NEXT) return "Next";
    if (code == K_PREV) return "Prev";
    if (code == K_FN)   return "Fn";
    if (code == K_MR)   return "MR";
    if (code == K_MC)   return "MC";
    if (code == K_ME)   return "ME";
    if (code == K_MA)   return "MA";
    if (code == K_MB)   return "MB";
    if (code >= K_M1 && code <= K_M12) {
        static char b[4];
        snprintf(b, sizeof(b), "M%u", (unsigned)(code - K_M1 + 1));
        return b;
    }
    // 0xC0~0xDE 是消费级控制键，落在 matrix 里的那些
    if (code >= 0xC0 && code <= 0xDF) {
        static char b[10];
        snprintf(b, sizeof(b), "Ctl%02X", (unsigned)(code - 0xC0));
        return b;
    }
    static char b[8];
    snprintf(b, sizeof(b), "%02X", (unsigned)code);
    return b;
}

static String getMacroNameByCode(uint16_t code) {
    if (code >= K_M1 && code <= K_M12) return "M" + String(code - K_M1 + 1);
    if (code == K_MA) return "MA";
    if (code == K_MB) return "MB";
    if (code == K_MC) return "MC";    if (code == K_MR) return "MR";
    if (code == K_ME) return "ME";
    if (code == K_LOGO) return "LOGO";
    return "";
}

// ---- 最近一次普通按键的查表现场（只读诊断，回拉配置时带给网页）----
//
//   "配置明明对、按键行为却不对"这类问题，光看两边数据永远不够 —— 必须知道
//   **这一次按键到底查的是哪张表、查到没有**。这里把最近一次 getMappedKey 的
//   输入输出和当时用的方案记下来，随 cfgBuildJson 一起发出去，网页的「方案诊断」
//   直接显示。方案下标标成 0xFFFFFFFF 表示"还没按过任何普通键"。
static uint32_t lmProf   = 0xFFFFFFFFu;
static uint32_t lmCount  = 0;
static uint16_t lmFrom   = 0;
static uint16_t lmTo     = 0;
static uint32_t lmHits   = 0;

static uint16_t getMappedKey(uint16_t originalKey) {
    lmProf  = currentProfile;
    lmCount = (uint32_t)remapCounts[currentProfile];
    lmFrom  = originalKey;
    lmTo    = originalKey;
    // ⚠ 只在"确实发出去了"的那一次调用里记（松键走 hostKeySent，不再调本函数），
    //   所以这里记下的就是主机真正收到的那颗键。
    for (int i = 0; i < remapCounts[currentProfile]; i++) {
        if (profileRemaps[currentProfile][i].fromKey == originalKey) {
            lmTo = profileRemaps[currentProfile][i].toKey;
            lmHits++;
            return lmTo;
        }
    }
    return originalKey;
}

// ===========================
// 宏执行：SEQ(击键流) / CMB(组合键) + 全局动作
// ===========================
// 这一整块是从原版 s3/s3.ino 抄回来的 —— LVGL 移植时把它砍掉了一半，
// 结果"网页里配好的宏在键盘上按下去什么也不发生"：
//   · executeGlobalKey() 只解析 GSET payload 里的 "SW:"，后面的 "+SEQ:…"/"+CMB:…"
//     整段被丢掉（网页的 payload 就是 `SW:1+SEQ:你好` / `SW:2+CMB:128,129,65`）
//   · executeMacro() 只认 "SEQ:"，"CMB:" 一条都没实现，组合键全废
//   · executeComboAction() 这个函数在移植版里直接消失了
// 下面按原版语义补齐，行为以原版为准。

// delay() 期间 loopTask 是被挂起的，整块主循环都停在这儿。凡是宏里要等的
// 地方都走这个函数，边等边报活（让开机那张"上次现场"能看出宏跑到哪一步）。
static void kbSafeDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        ct_alive();
        delay(1);
    }
}

// SEQ: 连续击键流。支持三类写法：
//   SLEEP(ms) / [SLEEP:ms]  等一会儿再打下一个键
//   [ENTER] [TAB] [ESC] [BACKSPACE]  特殊键（键盘录制的宏就是用这几个 tag 存的）
//   其余可打印 ASCII 字符，逐字打出去，每个之间留 5ms 让主机跟得上
static void executeSequenceAction(String seq) {
    int i = 0;
    while (i < (int)seq.length()) {
        if (seq.substring(i).startsWith("SLEEP(")) {
            int endP = seq.indexOf(')', i + 6);
            if (endP != -1) {
                int sleepMs = seq.substring(i + 6, endP).toInt();
                if (sleepMs > 0) kbSafeDelay(sleepMs);
                i = endP + 1;
                continue;
            }
        } else if (seq.substring(i).startsWith("[SLEEP:")) {
            int endB = seq.indexOf(']', i + 7);
            if (endB != -1) {
                int sleepMs = seq.substring(i + 7, endB).toInt();
                if (sleepMs > 0) kbSafeDelay(sleepMs);
                i = endB + 1;
                continue;
            }
        } else if (seq[i] == '[') {
            int endB = seq.indexOf(']', i);
            if (endB != -1) {
                String tag = seq.substring(i + 1, endB);
                bool matched = true;
                if (tag == "ENTER")           Keyboard.write(KEY_RETURN);
                else if (tag == "TAB")        Keyboard.write(KEY_TAB);
                else if (tag == "ESC")        Keyboard.write(KEY_ESC);
                else if (tag == "BACKSPACE")  Keyboard.write(KEY_BACKSPACE);
                else matched = false;
                if (matched) { i = endB + 1; continue; }
            }
        }
        // 只发 ASCII：中文/全角字符的 UTF-8 字节 >= 0x80，喂给 Keyboard.write()
        // 会被当成"修饰键码"(0x80~0x87) 或非打印键，主机收到的是乱七八糟的
        // Ctrl/Alt 按住事件。中文只能走 ME 那条 hex 通道。
        uint8_t c = (uint8_t)seq[i++];
        if (c >= 32 && c <= 126) Keyboard.write(c);
        kbSafeDelay(5);
    }
}

// CMB: 组合键，编码是"HID 码点用逗号分隔"，例如 Ctrl+Shift+A = "128,129,65"
// （码值定义和网页 KEY_OPTIONS 一致：Ctrl=128 Shift=129 Alt=130 Win=131，
//  普通键用 Arduino 的码值，字母就是 ASCII）。
// 关键是**先全部按住、再一起松开**：挨个 press+release 出来的是三个单键，
// 不是组合键。
static void executeComboAction(String cmb) {
    while (cmb.length() > 0) {
        int comma = cmb.indexOf(',');
        String tok = (comma == -1) ? cmb : cmb.substring(0, comma);
        tok.trim();
        int code = tok.toInt();
        if (code > 0) kbPress((uint8_t)code);
        if (comma == -1) break;
        cmb = cmb.substring(comma + 1);
    }
    kbSafeDelay(50);
    Keyboard.releaseAll();
}

// GSET payload 的后半段：`SEQ:…` 或 `CMB:…`。
// SW: 那一段已经在 executeGlobalKey() 里处理掉了。
static void executeActionPayload(String extra) {
    if (extra.startsWith("SEQ:"))      executeSequenceAction(extra.substring(4));
    else if (extra.startsWith("CMB:")) executeComboAction(extra.substring(4));
}

// ===========================
// ME 键输出：分帧执行（状态机）
// ===========================
//
// 原来的写法是一把梭：while (f.available()) { Keyboard.print(...) }。
//
// ⚠⚠ 为什么必须改 —— 这就是"按 ME 键就崩"的真身：
//
//   ME 键要把整段文本通过 USB HID 打到主机，**每个字符要发 2 个 HID report**
//   （按下 + 松开）。而 Arduino 的 USBHID::SendReport() 是**阻塞**的：
//   它取信号量、把报告交给 TinyUSB，然后**等主机把报告取走**
//   （tud_hid_report_complete_cb 才给信号量），默认超时 100ms。
//   主机正常轮询时一个字符约 2ms；5KB 中文 = 10000 个字符 = 20000 个 report
//   ≈ **40 秒**。主机不轮询时（屏保、别的 USB 设备占着总线）更惨：
//   每个字符都能吃满 100ms 超时 —— 那就是几十分钟。
//
//   一把梭的后果：这 40 秒里 loop() 完全停住 ——
//     · 屏幕不刷新、按键不响应 → 用户看着像"键盘死了"
//     · **btLinkPoll() 不再跑 → 蓝牙一条都不回** → 网页那边全是超时，
//       主机也会因为收不到任何 GATT 应答而判定设备"失联"
//   所以它不是"复位"，是**主循环被按住**。用户说的"崩"多半就是这个。
//
// 改法：拆成状态机，每轮 loop 只发 ME_FRAME_CHARS 个字符就还回 CPU。
//   于是这 40 秒里键盘仍然是活的：屏幕有进度、按键能中断、蓝牙照常回话。
//
// 注意：文本本身仍然是 **hex 字符串**（网页下发时就转好了），
// 所以发出去的仍然是 ASCII 的 hex，主机按 [HEXS]/[HEXE] 协议自己解码。
// 这里**不改协议**，只改"什么时候发"。

#define ME_FRAME_CHARS  4      // 每轮 loop 发几个字符（4 个 ≈ 8ms，留得住主循环）
#define ME_TICK_GAP_MS  1      // 帧与帧之间的最小间隔

static File       meOutFile;
static bool       meOutActive = false;
static size_t     meOutTotal  = 0;
static size_t     meOutSent   = 0;
static size_t     meOutSkip   = 0;
static uint8_t    meOutStage  = 0;   // 0=发头 1=发正文 2=发尾 3=收尾
static unsigned long meOutLastTick   = 0;
static unsigned long meOutLastReport = 0;

// BOOTDL：收到指令后置位，真正的复位在 loop() 末尾做（见 handleCommand 里那段）。
static bool g_bootDlPending;      // 前置声明在 UART 指令通道那边
// BOOTAPP：同上，但方向相反（拉高 BOOT 脚后复位，用来从下载模式回应用）。
static bool g_bootAppPending;     // 前置声明在 UART 指令通道那边

// ===========================
// UART0 维护指令通道（USB-TTL / COM 口）
// ===========================
//
// 为什么要有：LOG_PORT 走的是 UART0，但 UART0 的**接收方向一直没人用**。
//   于是"只有一根 USB-TTL 线"的时候，除了看日志什么也做不了 ——
//   比如蓝牙配对记录过期（主机侧报 Code=14 "Peer removed pairing
//   information"），明明固件里有 BOND:clr 指令，却因为**连不上蓝牙**
//   而根本发不出去。加了这条通道，维护动作走串口就能做，不依赖蓝牙链路。
//
// 一行一条，'\n' 结束：
//   PING          → 回 PONG（顺便验证串口双向通不通）
//   BOND:LIST     → 列出蓝牙配对记录
//   BOND:CLR      → 清掉全部配对记录（专治 Code=14）
//   ADV:STATUS    → 打印当前广播状态
//   REBOOT        → 软复位
//   BOOTDL/BOOTAPP→ 进下载模式 / 回应用模式
//
// ⚠ 串口上**只有**维护指令，没有业务指令 —— 键盘/网页那条路才是业务的。
static void btBondList(char* buf, size_t n);
static int  btBondClearAll(void);

// 列出蓝牙配对记录（Arduino 的 BLE 封装没暴露这两个接口，直接用 IDF 的）。
// Arduino BLE 封装没有 bond 管理接口，而 IDF 有：
//   esp_ble_get_bond_device_num / esp_ble_get_bond_device_list
//   esp_ble_remove_bond_device
static void btBondList(char* buf, size_t n) {
    if (buf == nullptr || n == 0) return;
    int cnt = esp_ble_get_bond_device_num();
    int off = snprintf(buf, n, "COUNT:%d", cnt);
    if (cnt > 0) {
        esp_ble_bond_dev_t devs[8];
        int want = (cnt > 8) ? 8 : cnt;
        if (esp_ble_get_bond_device_list(&want, devs) == ESP_OK) {
            for (int i = 0; i < want && off < (int)n - 20; i++) {
                uint8_t* a = devs[i].bd_addr;
                off += snprintf(buf + off, n - off,
                                " %02X:%02X:%02X:%02X:%02X:%02X/m%X",
                                a[5], a[4], a[3], a[2], a[1], a[0],
                                (unsigned)devs[i].bond_key.key_mask);
            }
        }
    }
}

static int btBondClearAll(void) {
    int cnt = esp_ble_get_bond_device_num();
    int removed = 0;
    if (cnt > 0) {
        esp_ble_bond_dev_t devs[8];
        int want = (cnt > 8) ? 8 : cnt;
        if (esp_ble_get_bond_device_list(&want, devs) == ESP_OK) {
            for (int i = 0; i < want; i++) {
                if (esp_ble_remove_bond_device(devs[i].bd_addr) == ESP_OK) {
                    removed++;
                }
            }
        }
    }
    return removed;
}

static void handleUartCommand(const String& cmd) {
    if (cmd == "PING") {
        LOG_PORT.println("UART> PONG");
    }
    else if (cmd == "BOND:LIST") {
        char buf[160];
        btBondList(buf, sizeof(buf));
        LOG_PORT.printf("UART> BOND %s\n", buf);
    }
    else if (cmd == "BOND:CLR") {
        int removed = btBondClearAll();
        LOG_PORT.printf("UART> BOND removed=%d left=%d\n",
                        removed, esp_ble_get_bond_device_num());
    }
    else if (cmd == "ADV:STATUS") {
        LOG_PORT.printf("UART> ADV state=%d streak=%u connected=%d\n",
                        (int)g_advState, (unsigned)advFailStreak,
                        btLinkConnected() ? 1 : 0);
    }
    else if (cmd == "REBOOT") {
        LOG_PORT.println("UART> reboot");
        delay(50);
        esp_restart();
    }
    else if (cmd == "BOOTDL")  { g_bootDlPending = true;  LOG_PORT.println("UART> BOOTDL 排队"); }
    else if (cmd == "BOOTAPP") { g_bootAppPending = true; LOG_PORT.println("UART> BOOTAPP 排队"); }
    else {
        LOG_PORT.printf("UART> 未知指令: %s\n", cmd.c_str());
    }
}

static void handleUartEvents(void) {
    static String buf = "";
    while (Serial0.available() > 0) {
        char c = (char)Serial0.read();
        if (c == '\n') {
            buf.trim();
            if (buf.length() > 0) {
                handleUartCommand(buf);
                buf = "";
            }
        } else if (c != '\r') {
            if (buf.length() < 96) buf += c;   // 掐断超长行
        }
    }
}


static void meOutClose() {
    if (meOutFile) { meOutFile.close(); meOutFile = File(); }
}

// 中断正在进行的输出（用户按了别的键）。
static void meOutCancel() {
    if (!meOutActive) return;
    meOutActive = false;
    meOutClose();
    LOG_PORT.printf("[ME] cancelled at %u/%u\n",
                    (unsigned)meOutSent, (unsigned)meOutTotal);
    triggerHud("ME 文本", "已中断", lv_color_hex(CLR_AMBER));
}

// 开始把 ME 文本往主机打。原来这段是一把梭，现在只是"起个头"，
// 真正的发送在 loop() 的 meOutPump() 里分帧做。
static void meOutStart() {
    if (meOutActive) { meOutCancel(); }   // 已经在发了 → 先中断旧的
    if (!SPIFFS.exists("/me_hex.txt")) {
        LOG_PORT.println("[ME] /me_hex.txt not found");
        triggerHud("ME 文本", "尚未设置", lv_color_hex(CLR_TEXT_DIM));
        return;
    }
    File f = SPIFFS.open("/me_hex.txt", FILE_READ);
    if (!f) {
        LOG_PORT.println("[ME] open /me_hex.txt FAILED");
        triggerHudBig("ME 文本", "打开失败", lv_color_hex(CLR_RED));
        return;
    }
    meOutFile   = f;
    meOutTotal  = meOutFile.size();
    meOutSent   = 0;
    meOutSkip   = 0;
    meOutStage  = 0;
    meOutActive = true;
    meOutLastTick   = millis();
    meOutLastReport = millis();
    LOG_PORT.printf("[ME] start, %u bytes, freeHeap=%u\n",
                    (unsigned)meOutTotal, (unsigned)ESP.getFreeHeap());
    triggerHud("ME 文本", "正在发送…", lv_color_hex(CLR_ACCENT));
}

// 每轮 loop 调一次。返回 true = 还在发。
static bool meOutPump() {
    if (!meOutActive) return false;
    if ((long)(millis() - meOutLastTick) < ME_TICK_GAP_MS) return true;
    meOutLastTick = millis();

    if (meOutStage == 0) {
        Keyboard.print("[HEXS]");
        meOutStage = 1;
        return true;
    }
    if (meOutStage == 2) {
        Keyboard.print("[HEXE]");
        meOutStage = 3;
        return true;
    }
    if (meOutStage == 3) {
        meOutActive = false;
        meOutClose();
        LOG_PORT.printf("[ME] done %u/%u (skip=%u) freeHeap=%u\n",
                        (unsigned)meOutSent, (unsigned)meOutTotal,
                        (unsigned)meOutSkip, (unsigned)ESP.getFreeHeap());
        if (meOutSkip > 0) {
            char buf[40];
            snprintf(buf, sizeof(buf), "已发 %u 跳过 %u",
                     (unsigned)(meOutSent - meOutSkip), (unsigned)meOutSkip);
            triggerHud("ME 文本(部分)", buf, lv_color_hex(CLR_AMBER));
        } else {
            triggerHud("ME 文本", "已发送", lv_color_hex(CLR_GREEN));
        }
        return false;
    }

    // ---- stage 1：正文 ----
    int n = 0;
    while (n < ME_FRAME_CHARS && meOutFile.available()) {
        int c = meOutFile.read();
        if (c < 0) break;
        // ★ 关键安全修：跳过所有 >= 0x80 的字节。
        //   Keyboard.print((char)c) 走的是 USBHIDKeyboard::press()，
        //   对 0x80~0xFF 会进入 `modifiers |= (1 << (k-0x80))` 分支。
        //   当 k-0x80 >= 8（即 k >= 0x88）时，左移数超过 modifier byte 位宽
        //   是 C/C++ 未定义行为 —— Xtensa 上 5K+ 中文 UTF-8 字节（0xE4/0xB8/0xAD
        //   这种）反复触发，踩坏堆就会重启。
        if (c >= 0x80) { meOutSkip++; meOutSent++; n++; continue; }
        Keyboard.print((char)c);
        meOutSent++;
        n++;
    }
    ct_alive();
    if (!meOutFile.available()) meOutStage = 2;

    // 每 ~500ms 报一次进度：既让人知道还在动，也让"真卡住了"有据可查
    if (millis() - meOutLastReport > 500) {
        meOutLastReport = millis();
        LOG_PORT.printf("[ME] %u/%u (skip=%u) freeHeap=%u\n",
                        (unsigned)meOutSent, (unsigned)meOutTotal,
                        (unsigned)meOutSkip, (unsigned)ESP.getFreeHeap());
        char buf[40];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)(meOutTotal ? meOutSent * 100 / meOutTotal : 0));
        triggerHud("ME 文本", buf, lv_color_hex(CLR_ACCENT));
    }
    return true;
}

static void executeMacro(String keyName) {
    // ME 键是特殊的：它带的内容不是"按什么键"，而是一整段要贴给主机的文本。
    // 网页端蓝牙过来时只写 /me_hex.txt（HEX 字符串），按下 ME 键才真的打字。
    // 外面套 [HEXS] / [HEXE] 是主机端识别用的标记 —— 纯 ASCII 打字发中文根本
    // 做不到，hex 走一遍协议才能把 UTF-8 字节原样送到主机。
    //
    // ⚠ 这里只"起个头"，真正的发送由 loop() 里的 meOutPump() 分帧做 ——
    //   一把梭会把主循环按住 40 秒（见上面那段说明）。
    if (keyName == "ME") {
        LOG_PORT.println("[ME] key pressed");
        meOutStart();
        return;
    }

    char pKey[32];
    snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, keyName.c_str());
    String macroData = preferences.getString(pKey, "");
    if (macroData.length() == 0) return;
    if (macroData.startsWith("SEQ:"))      executeSequenceAction(macroData.substring(4));
    else if (macroData.startsWith("CMB:")) executeComboAction(macroData.substring(4));
}

static void executeGlobalKey(String gKey) {
    String val = preferences.getString(("g_" + gKey).c_str(), "");
    if (val.length() == 0) {
        if (gKey == "MA") switchProfile(0);
        else if (gKey == "MB") switchProfile(1);
        return;
    }
    if (val == "NONE") return;      // 网页"清除全局配置"写进来的哨兵值

    // 网页的 payload 语法（见 s3-setting.html buildGlobalkPayload）：
    //   NONE | SW:<方案> | SW:<方案>+SEQ:<文本> | SW:<方案>+CMB:<码点,码点>
    //   （也可能只有后半段：SEQ:<文本> / CMB:<码点,码点>）
    // <方案> 是 0~3，或 NEXT = 循环切到下一个方案。
    String rest = val;
    if (rest.startsWith("SW:")) {
        rest = rest.substring(3);
        int plusIdx = rest.indexOf('+');
        String swPart = (plusIdx != -1) ? rest.substring(0, plusIdx) : rest;
        if (swPart == "NEXT") switchProfile((currentProfile + 1) % TOTAL_PROFILES);
        else if (swPart != "NONE") switchProfile(swPart.toInt());

        if (plusIdx == -1) return;
        rest = rest.substring(plusIdx + 1);
    }
    executeActionPayload(rest);
}

// ===========================
// MA / MB 双状态切换执行
// ===========================
// 设计:每个按键(MA / MB)有状态 A 和状态 B 两套独立 payload,
// 按一次翻一下: 0 → 跑 A → 1 → 跑 B → 0 → 跑 A ...
//
// 存储:
//   g_<KEY>a / g_<KEY>b    状态 A / B 的 payload(同 g_<KEY> 的语法)
//   g_<KEY>_ph            当前相位(0=下次跑 A, 1=下次跑 B),持久化
//
// 兼容老固件(只设了 g_MA / g_MB 单套):状态 A 和 B 都为空时,
// 走老的 executeGlobalKey() 兜底,行为和 v1 一致 —— 不破坏存量用户。
//
// HUD 反馈:
//   · 跑了 A / B → "MA 状态 A 已触发"
//   · 选中状态为空 + 另一状态非空 → "MA 状态 B 未配置"(相位仍翻,提醒去网页配)
//   · 两状态都为空 → 走老路径,不显示这条 HUD
//
// 这里把 SW: 切方案的解析搬到这里执行,而不是复用 executeActionPayload(),
// 是因为 executeActionPayload() 只认 SEQ/CMB 后半段,SW 那段得在外层消化。
// 为避免再写一遍同样 if 链,直接内联实现 —— 状态量极小(<10 行),不抽函数。
static void executeToggleKey(const char* keyName) {
    // 只认 MA / MB,其它键理论上不会进来(键盘分发处已 hardcode)
    if (strcmp(keyName, "MA") != 0 && strcmp(keyName, "MB") != 0) {
        executeGlobalKey(keyName);   // 兜底:理论不会触发,防止误用
        return;
    }

    char phKey[16];
    snprintf(phKey, sizeof(phKey), "g_%s_ph", keyName);
    uint8_t phase = preferences.getUChar(phKey, 0);
    if (phase > 1) phase = 0;     // 容错:被人手动写过 NVS 时强行纠正

    char sKey[16];
    snprintf(sKey, sizeof(sKey), "g_%s%c", keyName, (phase == 0) ? 'a' : 'b');
    String val = preferences.getString(sKey, "");
    char oKey[16];
    snprintf(oKey, sizeof(oKey), "g_%s%c", keyName, (phase == 0) ? 'b' : 'a');
    String otherVal = preferences.getString(oKey, "");

    // 两套状态都未配 → 老路径(单套 g_<KEY> 或缺省切方案),相位不动
    if (val.length() == 0 && otherVal.length() == 0) {
        executeGlobalKey(keyName);
        return;
    }

    // 选中状态为空 + 另一状态非空 → HUD 提示 + 相位翻(下次会用另一套)
    if (val.length() == 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "状态 %c 未配置", (phase == 0) ? 'A' : 'B');
        triggerHud(keyName, buf, lv_color_hex(CLR_AMBER));
        preferences.putUChar(phKey, phase ^ 1);
        return;
    }

    // 正常路径:解析 SW/SEQ/CMB 并执行
    String rest = val;
    if (rest.startsWith("SW:")) {
        rest = rest.substring(3);
        int plusIdx = rest.indexOf('+');
        String swPart = (plusIdx != -1) ? rest.substring(0, plusIdx) : rest;
        if (swPart == "NEXT") switchProfile((currentProfile + 1) % TOTAL_PROFILES);
        else if (swPart != "NONE") switchProfile(swPart.toInt());
        if (plusIdx == -1) {
            // 只有 SW: 没有后半段,执行完就翻相位
            preferences.putUChar(phKey, phase ^ 1);
            char buf[24];
            snprintf(buf, sizeof(buf), "状态 %c 已触发", (phase == 0) ? 'A' : 'B');
            triggerHud(keyName, buf, lv_color_hex(CLR_GREEN));
            return;
        }
        rest = rest.substring(plusIdx + 1);
    }
    executeActionPayload(rest);

    // 相位翻并写盘(每次按下都写 NVS —— NVS 寿命 10w+ 次,不会触达)
    preferences.putUChar(phKey, phase ^ 1);
    char buf[24];
    snprintf(buf, sizeof(buf), "状态 %c 已触发", (phase == 0) ? 'A' : 'B');
    triggerHud(keyName, buf, lv_color_hex(CLR_GREEN));
}

// ===========================
// USB HID 事件
// ===========================
static void usbHidKeyboardEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
        auto* led_data = (arduino_usb_hid_keyboard_event_data_t*)data;
        if (led_data == nullptr) return;
        // 只在状态真的翻转时置脏位。主机每帧 LED 报告都广播一遍当前值，
        // 不做差分的话 lockStateDirty 一直为真，每次 loop 都白跑一次推帧。
        bool newNum = led_data->numlock;
        bool newCaps = led_data->capslock;
        bool newScr  = led_data->scrolllock;
        if (newNum != numLock || newCaps != capsLock || newScr != scrollLock) {
            numLock    = newNum;
            capsLock   = newCaps;
            scrollLock = newScr;
            lockStateDirty = true;   // 让 loop() 立刻推一帧 LED + 跑一次屏幕刷新
        }
    }
}

// ===========================
// 键盘矩阵扫描
// ===========================
static void initBaseMatrix(void) {
    memset(baseMatrix, 0, sizeof(baseMatrix));

    // Row 0
    baseMatrix[0][0] = KEY_LEFT_ALT;
    baseMatrix[0][1] = 0xE9; baseMatrix[0][2] = 0xE8; baseMatrix[0][3] = 0xE7;
    baseMatrix[0][4] = 0xDE; baseMatrix[0][5] = 0xDD; baseMatrix[0][6] = 0xDC;
    baseMatrix[0][7] = 0xDB; baseMatrix[0][8] = KEY_RIGHT_ARROW; baseMatrix[0][9] = KEY_DOWN_ARROW;
    baseMatrix[0][10] = KEY_LEFT_ARROW; baseMatrix[0][11] = KEY_RIGHT_CTRL;
    baseMatrix[0][12] = 0xED; baseMatrix[0][13] = K_FN; baseMatrix[0][14] = KEY_RIGHT_ALT; baseMatrix[0][15] = ' ';

    // Row 1
    baseMatrix[1][0] = 0xDF; baseMatrix[1][1] = K_MB; baseMatrix[1][2] = K_MA;
    baseMatrix[1][3] = K_NEXT; baseMatrix[1][4] = K_PLAY; baseMatrix[1][5] = K_PREV; baseMatrix[1][6] = K_LOGO;
    baseMatrix[1][7] = 0xE0; baseMatrix[1][8] = 0xEB; baseMatrix[1][9] = 0xEA;
    baseMatrix[1][10] = 0xE3; baseMatrix[1][11] = 0xE2; baseMatrix[1][12] = 0xE1;
    baseMatrix[1][13] = 0xE6; baseMatrix[1][14] = 0xE5; baseMatrix[1][15] = 0xE4;

    // Row 2
    baseMatrix[2][0] = KEY_LEFT_SHIFT; baseMatrix[2][1] = KEY_LEFT_GUI; baseMatrix[2][2] = KEY_LEFT_CTRL;
    baseMatrix[2][3] = KEY_UP_ARROW; baseMatrix[2][4] = KEY_RIGHT_SHIFT;
    baseMatrix[2][5] = '/'; baseMatrix[2][6] = '.'; baseMatrix[2][7] = ',';
    baseMatrix[2][8] = 'm'; baseMatrix[2][9] = 'n'; baseMatrix[2][10] = 'b';
    baseMatrix[2][11] = 'v'; baseMatrix[2][12] = 'c'; baseMatrix[2][13] = 'x'; baseMatrix[2][14] = 'z';

    // Row 3: 物理空行，保持为 0

    // Row 4
    baseMatrix[4][0] = KEY_END; baseMatrix[4][1] = KEY_RETURN; baseMatrix[4][2] = KEY_INSERT;
    baseMatrix[4][3] = '\''; baseMatrix[4][4] = ';'; baseMatrix[4][5] = 'l'; baseMatrix[4][6] = 'k'; baseMatrix[4][7] = 'j';
    baseMatrix[4][8] = 'h'; baseMatrix[4][9] = 'g'; baseMatrix[4][10] = 'f'; baseMatrix[4][11] = 'd';
    baseMatrix[4][12] = 's'; baseMatrix[4][13] = 'a'; baseMatrix[4][14] = KEY_CAPS_LOCK; baseMatrix[4][15] = 0xD6;

    // Row 5
    baseMatrix[5][0] = KEY_PAGE_UP; baseMatrix[5][1] = KEY_DELETE; baseMatrix[5][2] = '\\';
    baseMatrix[5][3] = ']'; baseMatrix[5][4] = '['; baseMatrix[5][5] = 'p'; baseMatrix[5][6] = 'o';
    baseMatrix[5][7] = 'i'; baseMatrix[5][8] = 'u'; baseMatrix[5][9] = 'y'; baseMatrix[5][10] = 't';
    baseMatrix[5][11] = 'r'; baseMatrix[5][12] = 'e'; baseMatrix[5][13] = 'w'; baseMatrix[5][14] = 'q';
    baseMatrix[5][15] = KEY_TAB;

    // Row 6
    baseMatrix[6][0] = '`'; baseMatrix[6][1] = KEY_HOME; baseMatrix[6][2] = KEY_INSERT;
    baseMatrix[6][3] = KEY_BACKSPACE; baseMatrix[6][4] = '='; baseMatrix[6][5] = '-';
    baseMatrix[6][6] = '0'; baseMatrix[6][7] = '9'; baseMatrix[6][8] = '8'; baseMatrix[6][9] = '7';
    baseMatrix[6][10] = '6'; baseMatrix[6][11] = '5'; baseMatrix[6][12] = '4'; baseMatrix[6][13] = '3';
    baseMatrix[6][14] = '2'; baseMatrix[6][15] = '1';

    // Row 7
    baseMatrix[7][0] = KEY_ESC;
    baseMatrix[7][1] = 0xD0; baseMatrix[7][2] = 0xCF; baseMatrix[7][3] = 0xCE;
    baseMatrix[7][4] = KEY_F12; baseMatrix[7][5] = KEY_F11; baseMatrix[7][6] = KEY_F10; baseMatrix[7][7] = KEY_F9;
    baseMatrix[7][8] = KEY_F8; baseMatrix[7][9] = KEY_F7; baseMatrix[7][10] = KEY_F6; baseMatrix[7][11] = KEY_F5;
    baseMatrix[7][12] = KEY_F4; baseMatrix[7][13] = KEY_F3; baseMatrix[7][14] = KEY_F2; baseMatrix[7][15] = KEY_F1;

    // Row 8
    baseMatrix[8][0] = K_MC; baseMatrix[8][1] = K_ME; baseMatrix[8][2] = K_ME; baseMatrix[8][3] = K_M12;
    baseMatrix[8][4] = K_M11; baseMatrix[8][5] = K_M10; baseMatrix[8][6] = K_M9; baseMatrix[8][7] = K_M8;
    baseMatrix[8][8] = K_M7; baseMatrix[8][9] = K_M6; baseMatrix[8][10] = K_M5; baseMatrix[8][11] = K_M4;
    baseMatrix[8][12] = K_M3; baseMatrix[8][13] = K_M2; baseMatrix[8][14] = K_M1; baseMatrix[8][15] = K_MR;

    // Row 9: 全 0（与原版一致）
}

static void scanKeyboardMatrix(void) {
    // I2C 健康检查：原版 s3.ino:3484-3491 每 500ms 检查一次 MCP23017，
    // 失败就 recoverI2CBus() 重新初始化整条总线。LVGL 版把这一段整个丢了，
    // 后果是 I2C 一旦被干扰（干扰按键灯/插拔 USB/BLE 突发），键盘就永久失灵，
    // 只能断电重启才恢复。补回来。
    //
    // ⚠ 检查项后来换过一次：原来探的是"I2C 地址有没有应答"，现在探的是
    // "列选信号发不发得出去"。区别和原因见下面那段注释。
    static unsigned long lastI2CCheck = 0;
    if (millis() - lastI2CCheck > 500) {
        lastI2CCheck = millis();
        ct_mark(CT_S_SCAN_I2C);
        //
        // ⚠ 这里检的是"列选信号发不发得出去"，**不是**"地址有没有应答"。
        //
        // 原来只探地址（Wire.endTransmission()），那是最弱的一种存活信号：
        // MCP23017 就算 GPIO 全跑回输入态、16 个列脚一个都驱动不了，I2C 侧
        // 也照样完全健康、地址照答不误。那种情况下恢复**一次都不会被触发**，
        // 于是键盘全哑、屏幕和 C3 键却都好好的（列脚不拉低 → 行脚只读得到
        // 上拉高电平 → 一个键都扫不出来），只能断电重来。
        //
        // 现在改成真写一列再读回来核对：写进去的图案读不出来，就是芯片没法用了。
        // 每 500ms 一次，只多 3 个 I2C 事务，代价可以忽略。
        //
        // 这一路只负责**判死**，不再自己动手恢复 —— 判死之后一律交给下面那个
        // 统一入口，免得三路探测各写一套重试节奏，最后谁在敲总线都说不清。
        if (!mcpColumnStrobeOk()) {
            ELWARN("I2C", "列选自检失败 判死");
            mcpTripFault();
        }
    }

    // ---- 统一自愈入口：任何一路判死都在这里恢复 ----
    //
    // 判死之后**不再盲扫**（理由和"连发"现象写在 mcpHealthy 的声明处）：
    // 这时候一个键也扫不出来，盲扫唯一的作用是产生幽灵按键。
    //
    // 恢复本身是阻塞的（阶梯最深一级要几十毫秒），所以按间隔退避，不要每一拍
    // 都去敲 —— 总线真废掉时每次 begin_I2C 都要吃满 25ms 超时。
    if (!mcpHealthy) {
        unsigned long iv = (g_mcpRefault >= MCP_STRIKE_SLOW) ? MCP_RETRY_MS_SLOW
                                                              : MCP_RETRY_MS;
        if (millis() - mcpRetryMs >= iv) {
            mcpRetryMs = millis();
            ct_mark(CT_S_SCAN_RECOVER);
            // 点名是哪几行被按死、以及同键连发计数，排障时能直接缩小到某一列
            ELWARN("I2C", "开始恢复 总线=%s 空闲低行=%03X",
                   g_mcpIdleStrike ? "空" : "列选",
                   (unsigned)g_mcpIdleLowRows);

            if (recoverI2CBus()) {
                mcpHealthy = true;
                g_mcpIdleStrike  = 0;                       // 重新开始计连续判死
                memset(ghostPresses, 0, sizeof(ghostPresses));
                g_mcpRefault = 0;                            // 本次是真救回来了
                mcpStableMs = millis();                      // 稳定性计时起点
                g_mcpRecoverN = mcpAutoRecoverCount() + 1;
                preferences.putUInt("mcp_recover_n", g_mcpRecoverN);  // 跨重启保留
                ELINFO("I2C", "已复位自动恢复 累计第 %lu 次", (unsigned long)g_mcpRecoverN);
            } else {
                ELWARN("I2C", "恢复未成功 稍后重试");
            }
        }
        return;   // 没恢复就一直不扫
    }

    for (int c = 0; c < NUM_COLS; c++) {
        // 选列只用一次寄存器写。
        //
        // 原来的 mcp.digitalWrite(c, LOW) 在 Adafruit 库里是**读-改-写**：
        // Adafruit_MCP23XXX::digitalWrite 走 RegisterBits::write()，先读回整个
        // 端口字节、改一位、再写回 —— 一次 digitalWrite = 2 次 I2C 事务。
        // 16 列 × (LOW + HIGH) = 32 次 digitalWrite = **64 次 I2C 事务**，
        // 400kHz 下单次事务 ~112µs，光 I2C 就要 ~7ms，再加上 16×20µs 的列稳定
        // 等待，一次完整扫描 ~7.3ms。而 loop() 里 SCAN_INTERVAL 现在是 8ms
        // （这行原来写的是 2ms，是改成寄存器直写之前的初版值；SCAN_INTERVAL
        //  后来随 LVGL 帧预算一起放宽到 8ms，见文件底部那个常量）。
        // 也就是说：改成单次寄存器写之前，**需求是硬件能力的 3.5 倍**。
        // 扫描是阻塞 I2C，又排在 lvgl_driver_loop()
        // 前面，loop() 永远补不回来 → 界面停更（看起来卡死）+ 扫描本身也跑不满
        // （按键失灵）。这就是"切到要素最多的信息面板就整机卡死"的主因。
        //
        // MCP23017 引脚 0~7 = GPIOA、8~15 = GPIOB（Adafruit 库里 MCP_PORT(pin) = pin>>3）。
        // 每轮开始时两个端口都是 0xFF（全部 HIGH 空闲），所以选一列只需要改
        // 它所在的那一个端口：每列 1 次事务，一次扫描降到 ~1.8ms。
        if (c < 8) mcp.writeGPIOA((uint8_t)(0xFF & ~(1 << c)));
        else       mcp.writeGPIOB((uint8_t)(0xFF & ~(1 << (c - 8))));
        delayMicroseconds(20);

        for (int r = 0; r < NUM_ROWS; r++) {
            bool currentState = (digitalRead(rowPins[r]) == LOW);

            // ---- 三态消抖：raw → cand（带起始时刻）→ 稳定态 keebMatrix ----
            //
            // 关键点是"**连续保持**"而不是"距上次认账够久"：
            //  · 读到和稳定态不同的状态 → 记成候选并盖上时刻（不管它是什么）
            //  · 候选和稳定态一致 → 用的是上次那个时刻，不会被刷新
            //    （每拍重新盖时间戳的话就是"每拍都重新等 20ms"，永远认不了账）
            //  · 候选连续保持满 DEBOUNCE_DELAY → 才提升为稳定态，下面分发只跑一次
            //
            // 只有稳定态**真的翻转**时才进分发，所以一次物理按下最多产生一次 press，
            // 抖动无论多长都进不来（它总是把候选改回去，重置计时）。
            if (currentState != debounceCand[r][c]) {
                debounceCand[r][c]   = currentState;
                debounceCandMs[r][c] = millis();
            }
            else if (currentState != keebMatrix[r][c]
                     && millis() - debounceCandMs[r][c] >= DEBOUNCE_DELAY) {
                keebMatrix[r][c] = currentState;

                uint16_t baseKey = baseMatrix[r][c];
                if (baseKey == 0) continue;

                // 现场记录：记下这一刻的阶段和键码。
                // 「按某个键就崩」这类问题靠 panic 回溯很难对上号，
                // 但只要开机 HUD 能报出"上次死在 按键分发，键 XXX"，
                // 就能直接拿这个键去复现。
                ct_mark(CT_S_SCAN_KEY);
                ct_key(baseKey);
                ct_set_ctx(currentSysMode, currentDispMode);

                if (currentState) {
                    // ---- ME 文本正在往主机打：任何键都用来**中断**它 ----
                    //
                    // 一次 5KB 的 ME 输出要打 40 秒。这 40 秒里不给用户一个
                    // 反悔的机会太霸道了 —— 而且按下去也没别的用（键都去打给
                    // 主机了），所以拿它当"停"最自然。
                    // Fn 是修饰键，不算"想按点什么"，不拿它中断。
                    if (meOutActive && baseKey != K_FN) {
                        meOutCancel();
                        continue;
                    }
                    // ---- 键盘锁定：按键在这里被拦下 ----
                    //
                    // ⚠⚠ 位置必须在**同键连发探测之前**，这不是讲究，是必须：
                    //   那个探测统计"同一格连续按下的次数"，超阈值就判 MCP23017
                    //   故障并触发恢复。锁着的时候按键是**被吞掉**的（没发主机），
                    //   但物理上照样按得出来 —— 连按几下就把连发计数攒到阈值，
                    //   于是"锁着的时候猛按键盘"会把键盘扩展芯片判死，
                    //   表现成"锁一次键盘就坏了"。锁定态的按键不进那个计数器。
                    //
                    //   ⚠ 代价（明写在这，别以后当 bug 查）：这一路探测在锁定期间
                    //   是关着的 —— MCP23017 在锁定期间真坏了不会自愈，
                    //   要等解锁后才会被发现。另外两路自检（列选自检、空闲态
                    //   行脚）都在 per-cell 分发之外，锁定期间照常跑。
                    //
                    // 拦下的键：不发主机、不计 KPM、不唤醒屏保、不动灯效，
                    // 只在屏上闪一下红边框，告诉用户"键我收到了，只是被锁着"。
                    if (kbLocked) {
                        // Fn 放行：它是修饰键，本身不发主机，但 fnPressed 必须置起来，
                        // 否则 Fn+LOGO 根本凑不出来 —— 解锁就无从下手。
                        if (baseKey == K_FN) {
                            fnPressed = true;
                            continue;
                        }
                        // Fn + LOGO = 解锁。
                        if (baseKey == K_LOGO && fnPressed) {
                            // ⚠ logoHoldFired = true：告诉松手那一拍"这一下已经
                            //   处理过了，别再当短按切显示风格"。不加这一句的话，
                            //   解锁的瞬间 LOGO 的松手会被当成短按，
                            //   主屏风格当场换一档 —— 用户看到的是"我明明在解锁，
                            //   怎么界面样式也变了"。
                            logoHoldFired = true;
                            lockSetLocked(false);
                            continue;
                        }
                        // 其余一律吞掉。
                        // hostKeyHeld 保持 false：按下时没发过 press，
                        // 松手时自然也不会补一个无头的 release 给主机。
                        lockScreenFlash();
                        continue;
                    }

                    // ---- 同键连发探测：卡在任何分发**之前** ----
                    //
                    // 放在这里而不是分发之后，是因为用户真正受害的就是"幽灵按键
                    // 真的发到了主机"这一步 —— 6~7 次连发已经打进正在编辑的文档
                    // 里了，判死再快也追不回来。这里直接不认这一账。
                    //
                    // 注意 keebMatrix 在上面已经翻成 true 了，这没关系：
                    // hostKeyHeld 仍是 false，于是松键时不会发一个无头的 release，
                    // 主机那边从头到尾没收到过这颗键，干净。
                    //
                    // ⚠ 这里用 continue（只跳过这一格）而**不**中断整轮扫描，是
                    // 有依据的：DEBOUNCE_DELAY 是 20ms，而一拍（SCAN_INTERVAL）
                    // 只有 8ms —— 一轮之内任何单元格都不可能攒够 20ms 的连续
                    // 稳定，也就**不可能**再升成稳定态、产生第二个幽灵按键。
                    // 所以整轮扫完是安全的，不加 break 反而少一处状态机。
                    if (mcpGhostBurstAt(r, c)) {
                        ELWARN("I2C", "同键连发 %u 次 判死 行=%d 列=%d",
                               (unsigned)ghostPresses[r][c], r, c);
                        mcpTripFault();
                        continue;   // 这一格本次不认账
                    }

                    // 按键按下
                    totalKeyCount++;
                    lastActivityTime = millis();

                    // ---- B-1 仪表盘：累计 KPM（写入环形缓冲当前秒）和 TODAY ----
                    // KPM：当前秒的落键数 +1。每次落键时都加，最后由 updateKpmRing()
                    // 推进到下一格（按 wall-clock 自动）。
                    kpmRing[kpmRingHead]++;

                    // TODAY：按本地时区的"日"判断。首次开机 lastTodayDate=-1 → 直接
                    // 把当前日数记下来；后续跨日即清零。NTP 校时（设置/启动）会触
                    // 同样的检查路径。
                    {
                        time_t nowT = time(nullptr);
                        struct tm tmNow;
                        if (localtime_r(&nowT, &tmNow)) {
                            int ymd = (tmNow.tm_year + 1900) * 10000
                                    + (tmNow.tm_mon + 1) * 100
                                    + tmNow.tm_mday;
                            if (todayDateYmd < 0) {
                                todayDateYmd = ymd;
                            } else if (ymd != todayDateYmd) {
                                todayKeyCount = 0;
                                todayDateYmd  = ymd;
                                preferences.putUInt("today_key", 0);
                                preferences.putUInt("today_y", (uint32_t)ymd);
                            }
                            todayKeyCount++;
                        }
                    }

                    // 按键灯效：任何键都算，包括界面态里的菜单导航和宏键 ——
                    // 界面里按方向键也该有反馈，不然会以为没按上。
                    // 这里只置状态，真正的绘制在 20ms 的灯效节拍里做。
                    triggerKeyReaction();

                    // 唤醒
                    if (currentSysMode == SYS_MODE_SLEEP) {
                        gotoMainScreen();
                    }

                    // ================= 按键分发 =================
                    // 顺序必须和 s3.ino 原版一致：界面态（菜单 → 设置 → 录制）
                    // 先接管，K_MC 放最后。原来 K_MC 排在整条链的最前面，
                    // 于是"在菜单里再按一次 MC"会命中它 → menuSel 归零 + 重新
                    // build_menu()，视觉上等于没反应，永远出不去。
                    // 原版里 K_MC 在界面态的语义是"退出"，只有回到主界面才是"进入菜单"。
                    const bool inRecMode = (currentSysMode == SYS_MODE_REC_SEQ
                                            || currentSysMode == SYS_MODE_REC_CMB);
                    const bool inElogMode = (currentSysMode == SYS_MODE_ELOG);
                    // 速查表是纯查阅界面：在里面打字没有任何意义，矩阵键照常
                    // 发主机只会把主机屏幕搞得莫名其妙。所以它要进 inUiMode。
                    const bool inHelpMode = (currentSysMode == SYS_MODE_HELP);
                    const bool inUiMode = (currentSysMode == SYS_MODE_MENU)
                                          || IS_SETTING_MODE(currentSysMode)
                                          || inRecMode
                                          || inElogMode
                                          || inHelpMode;

                    // 宠物页**故意不在** inUiMode 里：那个分支会把按键吃干净、
                    // 一律不发主机，进去就一个字都打不出来。宠物页要的是
                    if (inUiMode) {
                        // ---- 界面态：这一整块把按键吃干净，一律不发到主机 ----

                        // 错误日志页：上下翻 / DEL 清空 / ESC 回菜单。
                        // 必须排在菜单分支**之前**判断吗？不必 —— SYS_MODE_ELOG
                        // 和 SYS_MODE_MENU 是互斥的 currentSysMode，顺序无所谓，
                        // 但放在最前面读起来更清楚。
                        if (inElogMode) {
                            elogKey(baseKey);
                        }
                        // 按键速查：↑↓ 翻页 / ESC·MC 关闭。
                        // 排在菜单分支前面 —— 两者是互斥的 currentSysMode，顺序无所谓，
                        // 但速查表是从菜单里点进来的，放最前面读起来更清楚。
                        else if (inHelpMode) {
                            helpKey(baseKey);
                        }
                        // 菜单（两级）：方向键移动 / 回车进组或执行 / ESC·MC 逐级退回
                        // 二级里 ESC 是"回一级"而不是直接退出 —— 两级菜单必须能往回走，
                        // 否则进了一个组就只能一路 ESC 到底才知道自己在哪。
                        else if (currentSysMode == SYS_MODE_MENU) {
                            uint8_t rows = menuInSub ? curMenuGroup()->count : MENU_GROUP_COUNT;
                            uint8_t& sel = menuInSub ? menuItemSel : menuSel;
                            if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                                sel = (uint8_t)((sel + 1) % rows);
                                build_menu();
                            } else if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
                                sel = (uint8_t)((sel + rows - 1) % rows);
                                build_menu();
                            } else if (baseKey == KEY_RETURN) {
                                handleMenuSelect();
                            } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                                if (menuInSub) {
                                    menuInSub = false;   // 二级 → 回一级
                                    menuScrollOffset = 0;
                                    build_menu();
                                } else {
                                    gotoMainScreen();
                                }
                            }
                        }
                        // 设置子界面：左右切字段 / 上下调值 / 回车保存 / ESC·MC 取消
                        else if (IS_SETTING_MODE(currentSysMode)) {
                            if (baseKey == KEY_LEFT_ARROW) moveSettingField(-1);
                            else if (baseKey == KEY_RIGHT_ARROW) moveSettingField(1);
                            else if (baseKey == KEY_UP_ARROW) adjustSettingField(1);
                            else if (baseKey == KEY_DOWN_ARROW) adjustSettingField(-1);
                            else if (baseKey == KEY_RETURN) {
                                // 倒计时那一屏的回车是"启停"，不是"保存" ——
                                // 页面上那颗开始/停止按钮就是这么标的（见
                                // toggleCountdownFromKeyboard 的注释）
                                if (currentSysMode == SYS_MODE_SET_TIMER) toggleCountdownFromKeyboard();
                                else saveSettingScreen();
                            }
                            else if (baseKey == KEY_ESC || baseKey == K_MC) cancelSettingScreen();
                        }
                        // 宏录制：M1-M12 保存 / MR 走三段 / ESC·MC 取消 / 其余记进键流
                        else {
                            if (baseKey == K_MC || baseKey == KEY_ESC) {
                                cancelRecording();
                            } else if (baseKey >= K_M1 && baseKey <= K_M12) {
                                finishMacroRecording(getMacroNameByCode(baseKey));
                            } else if (baseKey == K_MR) {
                                // 三段：连续输入 → 组合键 → 取消
                                advanceRecordingStage();
                            } else if (baseKey == K_FN) {
                                fnPressed = true;   // 录制中也得能识别 Fn 修饰
                            } else if (baseKey < MACRO_BASE
                                       && baseKey != K_MA && baseKey != K_MB
                                       && baseKey != K_ME) {
                                if (recKeyCount < MAX_REC_KEYS) {
                                    recKeyBuffer[recKeyCount++] = baseKey;
                                    update_recording_display();
                                }
                            }
                        }
                    }
                    // ---- 主界面：普通键盘 + 功能键 ----
                    else {
                        // Fn 键
                        if (baseKey == K_FN) {
                            fnPressed = true;
                        }
                        // MC 进入菜单
                        else if (baseKey == K_MC) {
                            currentSysMode = SYS_MODE_MENU;
                            // 每次进来都从一级大类列表开始。上一轮如果停在二级
                            // （menuInSub=true），不重置的话 build_menu() 会照着
                            // 那个层级渲染，用户按 MC 想"回到主菜单再进来"，
                            // 结果直接落在某个具体项上，看着像按了没反应。
                            menuSel = 0;
                            menuItemSel = 0;
                            menuInSub = false;
                            menuScrollOffset = 0;
                            menuNeedsRebuild = true;
                            // 不再弹"系统菜单/请选择功能"HUD：菜单本身就是提示，
                            // 每次进菜单都闪一次只是噪音，还压住菜单第一屏。

                        }
                        // MA / MB 全局键(双状态切换版本):
                        // 每次按下在「状态 A ↔ 状态 B」之间翻,执行对应那套。
                        // 配过任意一套就走新路径;两套都没配才走老的单套兜底(见 executeToggleKey)。
                        else if (baseKey == K_MA) {
                            executeToggleKey("MA");
                        }
                        else if (baseKey == K_MB) {
                            executeToggleKey("MB");
                        }
                        // MR 键：第一次按 = 进入"连续输入"录制（必须排在 >= MACRO_BASE 之前）
                        else if (baseKey == K_MR) {
                            enterRecording();
                        }
                        // 宏按键
                        // 小键盘整片（0xDB~0xEB，含 0xE2=小键盘 2）都不在这里拦，
                        // 全部落到最后的"普通键盘"分支走 kbPress —— 这是对的。
                        // 静音键在 C3 上（BTN:MUTE），别在这儿加分支。
                        else if (baseKey >= MACRO_BASE) {
                            if (baseKey == K_LOGO) {
                                // LOGO 的动作**从按下挪到松手**：按下只记"按住了"，
                                // 松手时再决定这算不算一次短按。
                                //
                                // ⚠️ Fn + LOGO 现在**只**用来解锁 —— 锁定时在按键分发
                                //    的锁定分支里就处理掉了（那是唯一会走到这里的
                                //    情况之外）。整机重启这个动作已经从 Fn+LOGO 上
                                //    拿掉，剩下的整机重启入口是"静音键长按"
                                //    （见 handleC3Command 的 BTN:MUTE_HOLD）。
                                //
                                //    未锁定时按 Fn+LOGO 什么都不做，所以这里**不能**
                                //    顺手置 logoIsDown：那会让松手时去切显示风格，
                                //    而 Fn+LOGO 的语义是"解锁"，不是"切风格"。
                                if (!fnPressed) {
                                    logoIsDown = true;
                                    logoHoldFired = false;
                                }
                            } else if (baseKey == K_PLAY) {
                                if (fnPressed) {
                                    SystemControl.press(SYSTEM_CONTROL_STANDBY);
                                    SystemControl.release();
                                } else {
                                    ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                                    mediaPlaying = !mediaPlaying;
                                }
                            } else if (baseKey == K_NEXT) {
                                if (!fnPressed) {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
                                    mediaPlaying = true;
                                }
                            } else if (baseKey == K_PREV) {
                                if (!fnPressed) {
                                    ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
                                    mediaPlaying = true;
                                }
                            } else {
                                String macroName = getMacroNameByCode(baseKey);
                                if (baseKey >= K_M1 && baseKey <= K_M12) {
                                    executeGlobalKey(macroName);
                                }
                                executeMacro(macroName);
                            }
                        }
                        else {
                            uint16_t mappedKey = getMappedKey(baseKey);
                            // 最近按键显示：走 getKeyName，空格显示 "Space" 而不是 "20"
                            snprintf(lastKeyPressed, sizeof(lastKeyPressed), "%s", getKeyName(baseKey));
                            // 触发律动效果：峰值柱 + 两侧余晖（pulseRhythm 里定义波形的形状）
                            pulseRhythm();
                            hostKeyHeld[r][c] = true;
                            // Caps Lock 本地兜底：USB HID LED 事件（ARDUINO_USB_HID_KEYBOARD_LED_EVENT）
                            // 在某些主机 / 复合 HID 接口下根本不来，靠它就完全无反应。
                            // 按下 KEY_CAPS_LOCK 这一刻我们已经知道"用户想翻转大写"，
                            // 本地先翻一次让屏/灯立刻跟上，host 那边返回的 LED 报告就算延迟、
                            // 不来、或者延后很多帧，最终也是同一个值，不会冲突。
                            // Num/Scroll 没有物理按键，仍只能等 LED 报告。
                            if (baseKey == KEY_CAPS_LOCK) {
                                capsLock = !capsLock;
                                lockStateDirty = true;
                            }
                            // [DEBUG a→9] 临时诊断:确认 baseKey 和 remap 后实际送 kbPress 的码点
                        LOG_PORT.printf("[KB] base=0x%04X mapped=0x%04X cnt=%d\n",
                            baseKey, mappedKey, remapCounts[currentProfile]);
                        // 强制重置节流,确保 kbPress 里那条 [KB] press code=... 不会被吞掉。
                        // 按键两条日志挨着(<1ms),正常 100ms 节流会把第二条丢了,
                        // 那样就看不到 kbPress 走哪个分支,定位就缺一半证据。
                        btLinkStreamResetThrottle();
                        // ⚠ 记下"这一次真正发出去的码点"，松键时原样松掉它 ——
                        //   松键不能重查映射表，理由见 hostKeySent 那段注释。
                        hostKeySent[r][c] = (uint8_t)mappedKey;
                        kbPress((uint8_t)mappedKey);
                        }
                    }
                } else {
                    // 按键释放
                    if (baseKey == K_FN) {
                        fnPressed = false;
                    }
                    else if (baseKey >= MACRO_BASE) {
                        // 消费级键松开才发 release。
                        // LOGO 的短按动作在"松手"时补做（因为要区分长按）；
                        // 长按已经由 scanKeyboardMatrix 开头的判定处理过了。
                        if (baseKey == K_LOGO) {
                            // wasDown = 这一下**确实按下过**。
                            // 少了它，未锁定时按 Fn+LOGO 会顺手切一次显示风格 ——
                            // Fn+LOGO 的语义是"解锁"，没锁的时候按它应该什么都不做。
                            // 正常短按 LOGO 时 logoIsDown 是 true，不受影响。
                            bool wasDown = logoIsDown;
                            logoIsDown = false;
                            // ⚠ 锁定期间**跳过**短按动作：按下的时候就被吞了，
                            //   没发过 press，这里再补动作等于凭空切一次显示风格 ——
                            //   而 cycleDisplayStyle() 会重建主屏，锁屏就被顶掉了，
                            //   表现成"锁着锁着屏幕自己变了"。
                            if (!kbLocked && !logoHoldFired && wasDown) {
                                cycleDisplayStyle();
                            }
                            logoHoldFired = false;
                        }
                        // PLAY/NEXT/PREV 的 Consumer 松键照旧。
                        //
                        // ⚠ 锁定期间**不发**：ConsumerControl.release() 不是空操作，
                        //   它真往主机发一帧 HID 报告（usage 0，见 Arduino 的
                        //   USBHIDConsumerControl::release() → send(0)）。
                        //   锁的意义就是"键盘不控制主机"，这一帧也不能有。
                        if (!kbLocked
                            && (baseKey == K_PLAY || baseKey == K_NEXT || baseKey == K_PREV)) {
                            ConsumerControl.release();
                        }
                    }
                    else if (hostKeyHeld[r][c]) {
                        // 只有"按下时确实发给过主机"的键才需要补一个松键。
                        // 界面态里被吞掉的键（菜单导航、ESC、设置字段…）按下时没发过
                        // press，这里再发 release 就会给主机一个无头的松键事件。
                        hostKeyHeld[r][c] = false;
                        // ⚠⚠ 松的是**按下时发出去的那个码点**，不是重新查一遍表。
                        //   重查会拿到"松手这一刻"的方案映射，和按下时发出的可能
                        //   不是同一颗键 —— 修饰键一旦对不上就再也松不掉，主机那边
                        //   从此所有组合键都像按着 Cmd/Win。切方案的动作（MA/MB、
                        //   配了 SW: 的 M1~M12）随时可能发生在一次按住的期间，
                        //   所以这里必须用 hostKeySent，绝不重查。
                        kbRelease(hostKeySent[r][c]);
                        }
                    }
            }
        }
        // 全部列恢复空闲高电平（同时保住"下一轮开始时两个端口都是 0xFF"这个前提）
        mcp.writeGPIOA(0xFF);
        mcp.writeGPIOB(0xFF);
    }

    // ---- 空闲态自检：一整轮跑完，列脚刚刚全部放开，此刻行脚必须全高 ----
    //
    // 这是三路探测里**最快**的一路：每 8ms 一拍，连续 2 拍不对就判死，
    // 最坏 16ms。原来 500ms 一次的自检在"按住键连发"的场景下根本来不及
    // （连发本身要 240~280ms），只能靠运气。
    //
    // 不变量和为什么不会误报见 mcpIdleRowsClean() 的注释。
    if (mcpIdleRowsClean()) {
        g_mcpIdleStrike = 0;
    } else if (++g_mcpIdleStrike >= MCP_IDLE_STRIKE_N) {
        ELWARN("I2C", "空闲态仍有行脚被拉低=%03X 判死",
               (unsigned)g_mcpIdleLowRows);
        mcpTripFault();
    }
}

// ===========================
// I2C 恢复：逐级加码
// ===========================
// 返回 true = 总线和 MCP23017 都救回来了。
//
// 这条路径以前是静默的：begin_I2C 失败也照样返回，调用方无从判断，
// 于是"键盘突然失灵"这件事在日志里一个字都没有 —— 而它恰恰是
// 最需要留证的一类故障（总线受干扰 / 接触不良 / 上电时序没满足）。
// 现在失败会直接进错误日志，菜单里能翻到"总线恢复失败 x N 次"。
//
// ⚠⚠ 为什么原来那一版救不回来
//
// 原来是 `Wire.end()` + `Wire.begin()` 就完事。这一步**治不了总线卡死**：
// 它只把主控自己的引脚浮空，管不到**从机正在驱动的那条线**。当 MCP23017
// 卡在半个字节中间、拉着 SDA 不放的时候，这个操作对它毫无作用，下一句
// mcp.begin_I2C() 必然失败。于是每 500ms 重试一次、每次都失败，键盘一直
// 哑到断电为止 —— **S3 软重启也救不了**，因为重启并不会让 3.3V 掉下去，
// 芯片还是那个卡住的芯片，只有彻底断电才能让它松开 SDA。
//
// 敲 SCL 只对「芯片还在正常走协议、只是总线停在半个字节中间」有效：敲够 9 拍
// 它就会把手里那个字节走完再放线，这是 i2c 总线救援的标准动作。
//
// 但如果芯片内部状态机是**整个死透**的，它已经不是从机状态机了，只剩下一个把
// SDA 按住的输出级 —— 敲多少拍都松不开，S3 软重启也没用（3.3V 没掉下去，
// 还是那个卡住的芯片）。这种只有**硬件复位**能救：数据手册 TABLE 1-2 明确
// RESET 拉低后 1µs 内输出脚全部进入高阻。这是四级阶梯里唯一不靠总线
// 通信就能生效的一级，也是唯一能真正免断电的一级。
//
// 所以现在分四级，从最便宜的试起：
//   0 级  总线还通，只是芯片丢了寄存器配置 → 重刷方向和电平就行
//   1 级  硬复位 MCP23017（只有真接了 RESET 脚的那台机器才有这级）
//   2 级  12 拍救援 + 重建 I2C 外设
//   3 级  40 拍、慢 12 倍，给半死不活的从机最后机会

// 把 SCL 敲 pulses 下，最后补一个 STOP。
//
// ⚠ 只敲 SCL 不够：敲完总线停在 START 之后的状态，从机还是不认。必须再补
// 一次"SCL 为高时 SDA 由低走高"才是 STOP，总线才真正回到空闲态。
// 拍数取 12 而不是刚好 9：9 是走完一个字节的下限，做多几个留余量。
static void i2cStrobe(int pulses, uint16_t halfUs) {
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);
    delayMicroseconds(10);
    for (int i = 0; i < pulses; i++) {
        pinMode(I2C_SCL, OUTPUT);
        digitalWrite(I2C_SCL, LOW);
        delayMicroseconds(halfUs);
        pinMode(I2C_SCL, INPUT_PULLUP);   // 交回上拉 = 时钟走高的半拍
        delayMicroseconds(halfUs);
    }
    // STOP：SDA 先拉低 → SCL 拉高 → 放开 SDA（SDA 低->高）
    pinMode(I2C_SDA, OUTPUT);
    digitalWrite(I2C_SDA, LOW);
    delayMicroseconds(halfUs);
    pinMode(I2C_SCL, OUTPUT);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(halfUs);
    pinMode(I2C_SDA, INPUT_PULLUP);
    delayMicroseconds(halfUs);
}

// 列脚的方向 + 空闲电平。
//
// ⚠ 顺序是反直觉的：**先电平、后方向**。
//   先 pinMode 的话，从"全是输入"切到"全是输出"那一瞬间，输出锁存器还是
//   上电默认的 0，等于 16 个列脚同时被拉低 —— 行脚会读到"10 个键全按着"。
//   先写电平的话那时引脚还是输入，OLAT 只是写进去了没人驱动，随后的
//   pinMode 一上手就是高电平，没有那一瞬。
static void mcpApplyColumnConfig(void) {
    mcp.writeGPIOA(0xFF);
    mcp.writeGPIOB(0xFF);
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.pinMode(c, OUTPUT);
    }
}

// 列选通路自检：写一个"只有第 0 列为低"的图案，再把 IN 寄存器读回来核对。
//
// 为什么探地址不够：MCP23017 就算掉回全输入态，I2C 侧也是完全健康的，
// 地址照答不误。地址能应答只证明"这颗芯片挂在总线上"，一点都证明不了
// "它还能不能驱动列脚"—— 而键盘能不能用，恰恰只看后者。
//
// 为什么读回值能判：输出脚在 MCP23017 上 IN 位反映的就是引脚实际电平。
// 芯片正常 → 第 0 位是 0；掉回全输入 → 只有内部 100k 弱上拉 → 读回 0xFF。
//
// ⚠ A、B 两个端口**都要测**。列 0~7 在 GPIOA、列 8~15 在 GPIOB，
//   只测 A 的话，B 侧写坏了自检照样返回"正常"，8~15 列全哑却永远没人
//   去恢复它 —— 半把键盘失灵会比全哑更难被发现。
//
// 每 500ms 调一次，会有一瞬（约 250µs）把两边的第 0 列拉低。三态消抖天然
// 吃掉它：候选位翻过去不到 1ms 就翻回来，凑不满 DEBOUNCE_DELAY(20ms)，
// 升不成稳定态，不会产生任何假按键。
static bool mcpColumnStrobeOk(void) {
    mcp.writeGPIOA(0xFE);          // A 侧只有第 0 列为低
    uint8_t a = mcp.readGPIOA();
    mcp.writeGPIOA(0xFF);
    mcp.writeGPIOB(0xFE);          // B 侧同样挑第 0 列（即全局第 8 列）
    uint8_t b = mcp.readGPIOB();
    mcp.writeGPIOB(0xFF);          // 同时回到"每轮扫描开始时两个端口都是 0xFF"
    return (a == 0xFE) && (b == 0xFE);
}

// 空闲态行脚自检：所有列都放开时，10 个行脚必须**全部**是高。
//
// 为什么这个不变量成立：一轮扫描结束时 16 列已经全部写回 0xFF，也就是没有
// 任何一根列脚被选中。此刻**无论用户按着几颗键**，行脚都必然读回高 ——
// "按下"在电气上只是把行脚和列脚接通，而列脚此刻是高电位，中间不存在低电位
// 源，行脚自己的内部上拉（INPUT_PULLUP，见 setup）把它拉高。所以：
//
//     行脚为低  ⟺  芯片正在驱动一根本该放开的列
//
// 这正是"按住一个键连发六七次"的直接根源 —— 那一列被芯片按死不放，行脚就
// 一直读到"按下"，而总线又时通时不通，让这个读数反复翻转；每一次翻转只要
// 稳定满 20ms，三态消抖就认一次账，于是物理上按了一次、主机收到 6~7 次。
//
// 为什么这一条比 mcpColumnStrobeOk 更适合抓运行期故障：
//   · mcpColumnStrobeOk 是**写完立刻读寄存器**，读到的是芯片"认为自己"驱动成
//     什么；寄存器对得上、引脚实际没松开的情况它照样判通过。
//   · 这里读的是**引脚上的真实电平**。输出锁存器卡死、寄存器丢失、输出级损坏
//     —— 三种病根都会在这里现形，而它们对 I2C 侧全都"表现正常"。
//
// 代价：10 次 digitalRead + 一次 50µs 沉降，每 8ms 一拍，约占一拍预算的 1%，
// 而且**不产生任何 I2C 事务**。
//
// ⚠ 调用位置有讲究：必须在**一整轮列循环结束**之后调用。此时列脚刚被写回
//   0xFF，正好是"没有任何列被选中"这个前提成立的那一刻。
static bool mcpIdleRowsClean(void) {
    // 等引脚和芯片输出级都落定再读。MCP23017 的输出建立时间是几百 ns 量级，
    // 50µs 足够；相对一拍的 8ms 也完全可以忽略。
    delayMicroseconds(50);

    uint16_t low = 0;
    for (int r = 0; r < NUM_ROWS; r++) {
        if (digitalRead(rowPins[r]) == LOW) low |= (uint16_t)(1u << r);
    }
    g_mcpIdleLowRows = low;
    return low == 0;
}

// 同键连发检测：在消抖**认下按下**的那一刻调用，返回 true = 这一格不可信。
//
// 判据：同一颗键在 GHOST_WINDOW_MS(400ms) 内被认下 GHOST_BURST_N(4) 次按下。
// 人手做不到 —— 就算世界纪录手速，同一颗键也得 ~100ms 一次，400ms 最多 4 次
// 的边界；正常打字连按同一个字母的间隔远大于此。而半死的总线正是把这个形状
// 批量生产出来的。
//
// 存在的意义是补 ① 的盲区：① 要求故障**持续整整一拍**（8ms）才看得见，
// 而真正的故障往往是时好时坏的 —— 坏 5ms 好 30ms，① 每次读都干净，
// 但消抖照样会把中间那几毫秒认成"按了一下"。这种短命故障只有按**认账事件**
// 去数才抓得到。
//
// 越早判死越好：调用点在任何分发**之前**，幽灵按键不会走到主机去。
static bool mcpGhostBurstAt(int r, int c) {
    unsigned long nowMs = millis();

    // 整个窗口共用一个起点，过期就整块清零（160 字节 memset，可忽略）
    if (nowMs - ghostWinMs >= GHOST_WINDOW_MS) {
        memset(ghostPresses, 0, sizeof(ghostPresses));
        ghostWinMs = nowMs;
    }
    if (ghostPresses[r][c] < 255) ghostPresses[r][c]++;
    return ghostPresses[r][c] >= GHOST_BURST_N;
}

// 自动复位成功次数，懒加载自 NVS。
// 之所以要懒加载：这一段代码在 setup() 里读 preferences 之前也可能在
// 运行期被调到，而 preferences.begin() 的时机由 setup() 控制。
static uint32_t mcpAutoRecoverCount(void) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        g_mcpRecoverN = preferences.getUInt("mcp_recover_n", 0);
    }
    return g_mcpRecoverN;
}

// 统一的判死入口。三路探测（500ms 列选自检 / 空闲态行脚 / 同键连发）
// 都必须走这里，不要就地写 mcpHealthy = false。
//
// 它额外干一件就地赋值干不了的事：**统计"刚恢复就又坏"**。
// 恢复成功之后短时间内再次判死，说明这不是间歇干扰而是救不回来的那种坏
// （焊点虚焊 / 短路 / 芯片本身坏了）—— 每 300ms 敲一次 I2C 既救不了，
// 又会把主循环连同界面一起拖死，所以连续几次之后把重试间隔退到 2s，
// 并在日志里明确喊出来，让用户知道该动烙铁而不是继续折腾软件。
//
// mcpStableMs == 0 表示"本次上电以来还没恢复过"，此时第一次判死不算反复。
static void mcpTripFault(void) {
    if (!mcpHealthy) return;              // 已经判死了，不用重复计数
    if (mcpStableMs != 0 && millis() - mcpStableMs < MCP_RETRY_MS) {
        g_mcpRefault++;
        if (g_mcpRefault == MCP_STRIKE_SLOW) {
            ELERR("I2C", "恢复后立刻又卡死（%u 次） 重试已放慢 请查 RESET 焊点与芯片",
                  (unsigned)g_mcpRefault);
        }
    }
    mcpHealthy = false;
}

// 直读方向寄存器 IODIRA(0x00)。Adafruit 库没暴露读方向的接口，走原始 Wire。
//
// 这是故障现场最关键的一个数，因为它的两个取值直接指向两种不同的病根：
//   0xFF = 芯片被复位过 / 配置丢了（复位默认值就是全输入）
//   0x00 = 配置还在 → 那问题在总线或写入上，跟复位无关
// 总线本身不通时返回 0xFF 当哨兵 —— 这时"读不到"和"全输入"从外面看一样，
// 但两者在日志里配合错误文案能分得开。
static uint8_t mcpReadIODIR(void) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return 0xFF;
    if (Wire.requestFrom(MCP23017_ADDR, 1) != 1) return 0xFF;
    return (uint8_t)Wire.read();
}

// 恢复成功后清干净消抖和"按下"状态。
//
// 总线拖死的时候很可能正卡在一次按下的中间，keebMatrix 里会留下一个永远
// 松不开的"按下"。不清的话：一来幽灵按键会继续触发界面动作；二来
// hostKeyHeld 里的键再也不会走正常松键分支，主机那边就永远按着它 ——
// 症状是"所有组合键都像按着 Cmd"，比键盘全哑更难查。
static void mcpResetMatrixState(void) {
    for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < NUM_COLS; c++) {
            if (hostKeyHeld[r][c]) {
                kbRelease(hostKeySent[r][c]);   // 原样松掉发出去的那个码点，绝不重查
            }
            hostKeyHeld[r][c] = false;
        }
    }
    memset(keebMatrix,     0, sizeof(keebMatrix));
    memset(debounceCand,   0, sizeof(debounceCand));
    memset(debounceCandMs, 0, sizeof(debounceCandMs));
}

// 上电时把 MCP23017 一起复位一遍，再去初始化它。
//
// 为什么必须在开机做：MCP23017 的寄存器是**掉电易失**的，IODIRA/OLAT 这些
// 上电默认回到 0xFF（全输入）。于是芯片每次上电的寄存器状态，取决于它自己
// 在上电瞬间经历了什么 —— 电源爬坡沿抖动、VDD 比主控慢到位、或者干脆是一次
// 掉电重启，都会让它带着一份**不确定的寄存器内容**进入工作状态。
//
// 而"列脚是输出还是输入"这件事决定了键盘能不能用。只靠 I2C 写寄存器去纠正，
// 是**假设**寄存器已经被写成功过一次；芯片要是卡在某个写不进去的状态，
// 这个假设从第一步就不成立。数据手册给的唯一确定手段就是 RESET 脚。
//
// 时序比运行期那一次宽裕：这一刻 VDD 可能还在爬坡，而且反正只花 20ms。
//   TRSTL（复位低电平宽度）手册要求 ≥1µs，这里给 10ms
//   放开后等 10ms，让内部状态机和总线逻辑都稳下来再发第一条 I2C
//
// ⚠ 前提是这个脚真的接上了 GPIO11 且上拉接的是 **3.3V**。上拉接到 5V 的
//   模块 VCC 时，GPIO11 写高只有 3.3V，够不到 0.8VDD 的 VIH —— 芯片压根
//   出不了复位态，这一段会静默失效，症状是"复位好像没起作用"。
static void mcpPowerOnReset(void) {
#if MCP_RESET_PIN >= 0
    pinMode(MCP_RESET_PIN, OUTPUT);
    digitalWrite(MCP_RESET_PIN, LOW);
    delay(10);
    digitalWrite(MCP_RESET_PIN, HIGH);
    delay(10);
#endif
}

// 硬复位 MCP23017：把 RESET 脚拉低几毫秒再放开。
//
// 这是整个阶梯里唯一能对付「芯片状态机死透、自己拉着 SDA 不放」的动作，也是
// 唯一真正免断电的一级 —— 它走的是复位脚，**不经过 I2C 总线**，所以哪怕总线
// 已经彻底废了照样生效。
//
// 时序取得很宽：数据手册 TABLE 1-2 只要求 TRSTL（复位低电平宽度）≥ 1µs、
// THLD（放开后到工作）= 0ns，这里给 5ms 低电平 + 2ms 恢复余量，对任何一批芯片
// 都绰绰有余，总代价不到 10ms。
//
// ⚠ MCP_RESET_PIN = -1（没接这根线）时整个函数编译成空的，阶梯自动跳过这一级，
//   行为和改动前一模一样。也就是说光刷固件不会改变还没改硬件的那台机器。
static void mcpHardwareReset(void) {
#if MCP_RESET_PIN >= 0
    pinMode(MCP_RESET_PIN, OUTPUT);
    digitalWrite(MCP_RESET_PIN, LOW);
    delay(5);
    digitalWrite(MCP_RESET_PIN, HIGH);
    delay(2);
#endif
}

// ===========================
// 手动重启 MCP23017（CPG 键长按触发）
// ===========================
//
// 为什么单列一条路，不直接复用 recoverI2CBus()：
// recoverI2CBus() 是"总线已经判死才走的自愈阶梯"，它的前提是**已经坏了**。
// 长按 CPG 是用户**主动要求**把扩展芯片从确定状态重启一遍 —— 哪怕此刻键盘
// 还完全正常也要做（"我怀疑它抽风了，先复位再说"）。所以这里的顺序是
// "先复位 → 再重建 → 再自检"，判据也比阶梯里更严：列选必须真的通，
// 探到地址不算数（芯片掉回全输入态时地址照答不误）。
//
// 五件事，顺序不能换：
//   1. 拉复位脚 —— RESET 接了 GPIO11 的机器才有效，没接的机器这步是空函数。
//      数据手册：RESET 拉低后 1µs 内所有输出脚进高阻，芯片会主动松开 SDA。
//   2. 重建 I2C 外设 —— 上一步已经把总线放开了，但主控自己的 I2C 外设
//      可能还停在半个字节中间，必须 end/begin 重新武装。
//   3. 重新初始化 + 重刷列配置 —— 复位后寄存器回到 0xFF（全输入），
//      不重刷的话 16 个列脚一个都不驱动，键盘直接全哑。
//   4. 列选自检 —— 唯一能证明"列脚真的能驱动低"的判据。
//   5. 清消抖 + 清"按住"状态 —— 复位瞬间留在 keebMatrix 里的按下必须抹掉，
//      否则会冒幽灵键，主机那边还会永远按着一个组合键不放。
//
// ⚠ 失败时把 mcpHealthy 打成 false：让扫描主循环里的完整阶梯接手
//   （那边还有 12/40 拍时钟救援，manual 这条只做了复位+重建）。
//   不这么做的话，手动重启失败之后键盘会一直保持"半死"状态 ——
//   自愈阶梯只对"被判定为死"的芯片动手。
static bool mcpRestartChip(void) {
    ELINFO("I2C", "手动重启 MCP23017 开始");

    // 1) 硬复位复位脚（没接线的机器上这一步编译成空）
    mcpHardwareReset();

    // 2) 重建 I2C 外设
    Wire.end();
    delay(5);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);

    // 3) 重新初始化芯片 + 重刷列配置
    if (mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        mcpApplyColumnConfig();

        // 4) 列选自检
        if (mcpColumnStrobeOk()) {
            // 5) 清干净状态
            mcpResetMatrixState();
            memset(ghostPresses, 0, sizeof(ghostPresses));
            g_mcpIdleStrike = 0;
            g_mcpRefault    = 0;
            mcpStableMs     = millis();
            mcpHealthy      = true;
            ELINFO("I2C", "手动重启 MCP23017 成功");
            return true;
        }
    }

    ELERR("I2C", "手动重启失败 IODIRA=%02X 交自愈阶梯", (unsigned)mcpReadIODIR());
    mcpHealthy = false;      // 交给完整阶梯继续救（含 12/40 拍时钟救援）
    return false;
}

// ===========================
// RESET 连线自检
// ===========================
// 唯一能证明「RESET 这根线真的落在芯片脚上」的动作 —— 必须主动去戳它一脚。
//
// 为什么非做不可：固件把 MCP_RESET_PIN 改成实际接线的那一脚之后，如果这根线
// 其实**没焊到芯片的 RESET 上**（焊错脚 / 虚焊 / PCB 上断线），那拉高拉低对
// 芯片毫无影响。而「拉高没用」和「芯片本身坏了」在键盘上的表现**完全一样**：
// 都是全哑，I2C 侧都照样答地址。只看 mcpColumnStrobeOk() 失败，分不出这两种 ——
// 于是每次都只能盯着同一批日志反复猜到底该动软件还是动烙铁。
//
// 判据（RESET 低有效，拉低后芯片一定进复位态，16 个列脚全变高阻）：
//   strobeHigh=真 + strobeLow=假  → 线通了，芯片也能驱动列，硬件合格
//   strobeHigh=真 + strobeLow=真  → **复位线没接到这颗芯片上**（最常见的一类）
//   strobeHigh=假                → 没供电 / I2C 不通 / 列配置写不进去
//
// 收尾必须把 RESET 拉回高并重刷列配置。不这么做的话测完这一轮键盘就真的哑了，
// 而用户看到的是「本来就坏的键盘」，根本分不清是自己测坏的。
struct McpWireCheck {
    bool    strobeHigh;   // RESET 拉高时列选通过 —— 期望真
    bool    strobeLow;    // RESET 拉低时列选通过 —— 期望**假**（真 = 线没接上）
    bool    strobeBack;   // 拉回高之后列选通过 —— 期望真
    uint8_t iodira;       // 直读方向寄存器：FF=被复位过/配置丢了，00=配置还在
};
static McpWireCheck g_mcpWire = { false, false, false, 0xFF };

static const char* mcpWireVerdict(void) {
    if (g_mcpWire.strobeHigh && !g_mcpWire.strobeLow) return "线通 芯片正常";
    if (g_mcpWire.strobeHigh &&  g_mcpWire.strobeLow) return "复位线未接";
    if (g_mcpWire.strobeBack)                            return "上电态失败";
    return "芯片无响应";
}

static void mcpResetWireSelfTest(void) {
#if MCP_RESET_PIN >= 0
    // 必须从高电平开始：芯片得先在正常工作态，后面「拉低应当失败」才有意义
    pinMode(MCP_RESET_PIN, OUTPUT);
    digitalWrite(MCP_RESET_PIN, HIGH);
    delay(5);
    g_mcpWire.strobeHigh = mcpColumnStrobeOk();

    // 拉低 → 进复位态 → 列脚全高阻 → 列选必须失败
    digitalWrite(MCP_RESET_PIN, LOW);
    delay(5);
    g_mcpWire.strobeLow = mcpColumnStrobeOk();

    // 拉回来并重刷列配置，把芯片放回正常态再交还主循环
    digitalWrite(MCP_RESET_PIN, HIGH);
    delay(5);
    mcpApplyColumnConfig();
    g_mcpWire.strobeBack = mcpColumnStrobeOk();
#endif
    g_mcpWire.iodira = mcpReadIODIR();
}

static bool recoverI2CBus(void) {
    // ---- 第 0 级：总线还通，只是芯片丢了寄存器配置 ----
    // 最常见也最便宜，一次 I2C 外设都不用重建。覆盖 IODIRA/IODIRB 被复位
    // 回 0xFF（全输入）那种情况 —— 那时地址探得好好的，只有真去写一列再读
    // 回来才看得出来。
    Wire.beginTransmission(MCP23017_ADDR);
    if (Wire.endTransmission() == 0) {
        mcpApplyColumnConfig();
        if (mcpColumnStrobeOk()) {
            mcpResetMatrixState();
            return true;
        }
    }

    // ---- 第 1/2/3 级：芯片或总线本身死了 ----
    for (int stage = 1; stage <= 3; stage++) {
        Wire.end();
        delay(5);
        // 第 1 级硬复位，后两级敲时钟。
        // 顺序不是随便排的：芯片死透的时候 SDA 被它自己按着，这时候敲 12 拍
        // 时钟它根本不理（它已经不在走协议了）。必须**先**把它复位出来、总线
        // 回到空闲态，后面的时钟敲击才有意义。反过来先敲再复位也行，但那是把
        // 确定有效的动作排在确定无效的动作后面，白白多花几十毫秒。
        if (stage == 1) mcpHardwareReset();                     // 没接线时是空函数
        else            i2cStrobe(stage == 2 ? 12 : 40, stage == 2 ? 5 : 60);
        delay(10);
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(400000);
        Wire.setTimeOut(25);
        // ⚠⚠ begin_I2C() **不会**复位芯片。它只是 new 一个 Adafruit_I2CDevice
        // 再探一下地址应答（Adafruit_MCP23XXX.cpp:33-36 就这三行），没有任何
        // 寄存器写入。这里以前的注释写着「内部会写 IOCLR=0x05 做软件复位」——
        // **是错的**，那版注释把人带偏了：以为 1/2 级顺手清了寄存器，其实
        // 1/2 级和 0 级做的是同一件事（探地址 + 重刷列配置），只多敲了时钟。
        // 这大概就是上到板子之后键盘还是会失灵、只能断电的原因。
        //
        // 真要清寄存器可以写 IOCON(0x05) 的 bit2（SOFT RESET），但那还是走
        // 总线 —— 总线已经卡死时根本送不到。这一点也正是要用复位脚的原因。
        if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) continue;   // 失败就升一级
        mcpApplyColumnConfig();
        if (mcpColumnStrobeOk()) {
            mcpResetMatrixState();
            return true;
        }
    }

    // 去重会把反复的同类失败合并成一条并累加次数，所以这里可以放心每次都记。
    //
    // 把 IODIRA 一起带上：0xFF 说明芯片是被复位过 / 配置丢了，0x00 说明配置
    // 还在、问题在总线或写入上 —— 两者修法完全不同，光看"恢复失败"四个字
    // 分不出来，下次再犯还是只能猜。
    //
    // 再带上"硬复位试没试过"，因为它一刀切开了两类病根：
    //   硬复位已试仍然失败 → 芯片放开了 SDA 还是不通 → 病根在**外部**（焊点、
    //     短路、上拉电阻坏了），软件再怎么写都白搭，该动烙铁了
    //   没接线             → 还不知道芯片能不能被救回来，需要先飞线
    ELERR("I2C", "总线恢复失败 IODIRA=%02X %s", mcpReadIODIR(),
          (MCP_RESET_PIN >= 0) ? "硬复位已试" : "无复位脚");
    return false;
}

// ===========================
// 配置 JSON：一张表驱动"生成"和"应用"
//
// ⚠ 为什么必须是**一张**表：这两个方向一旦各写各的，字段名/范围迟早会漂 ——
//   网页按 A 的名字塞进来、固件按 B 的名字找，找不到就静默跳过，用户看到的是
//   "保存成功"但设置根本没变。这里让同一个 CFG_FIELDS[] 同时提供
//   · 生成：字段名 + 当前运行时变量的值
//   · 应用：字段名 + 取值范围 + 落哪个 NVS 键 + 要不要跑副作用
//   加字段只有改这一处，不可能只改一边。
//
// NVS 键沿用原来那套（一个都没新增、没改名）。运行时变量用 &var 取地址，
// 所以表里存的是**指针**——不要写成值。
enum CfgType : uint8_t { CFG_U8, CFG_BOOL, CFG_F32, CFG_U32 };

// 应用完某个字段之后除了"赋值 + 写 NVS"还要做的事。
enum CfgPost : uint8_t {
    CFG_POST_NONE = 0,
    CFG_POST_DISP,     // 显示风格：主屏要整屏重建
    CFG_POST_SAVER,    // 屏保风格：正在屏保时要拆/建屏
    CFG_POST_CLOCK,    // 时间：立刻 settimeofday
    // ⚠ 原来这里有 CFG_POST_PROF（"当前方案：顶部条要跟上"）。curr_prof 摘出
    //   CFG_FIELDS 之后**没有任何字段引用它**了，留着就是一条走不到的死码 ——
    //   而更糟的是它暗示"配置层能改运行态"，正是这次 bug 的病根。
    //   方案切换后的 UI 刷新由 switchProfile() 自己负责（它末尾就调
    //   updateTopBarProfile()），不依赖配置通道。
};

struct CfgField {
    const char* key;        // JSON 字段名（网页就靠它，两边必须一致）
    uint8_t     type;
    void*       var;        // 指向运行时变量
    float       lo, hi;     // 合法区间，闭区间；超了直接拒绝（不静默夹取）
    const char* nvsKey;     // 落盘用哪个 NVS 键
    uint8_t     post;
};

// 刻意**不含**这些，理由见下面 cfgApplyDoc 的注释：
//   today_key / today_y —— 每日按键计数，跟着用键涨，属于运行状态不是设置
//   boot_cnt / elog_fs / nvs_probe —— 内部记账
//   set_epoch            —— 含了，但它是"时间"这个**设置**，算
//
// ⚠⚠ curr_prof 也**不在**这张表里了（原来在，2026-10 摘掉）。它曾经和
//   disp_mode / brightness 摆在一起，于是网页 cfgDiffPatch() 把它当普通
//   配置项一起求差 —— 页面上任何一次保存（改亮度、存映射、存宏）只要
//   发现它和网页本地值不一致，就顺手把它下发，固件这边 switchProfile()
//   立刻切方案。现场表现是"配了 macOS 映射，按键却一直走 Windows 规则，
//   而且怎么切都切不过去"，而页面上看不出任何异常。
//
//   摘掉之后，配置层和运行态彻底分家：
//     · 配置层 = windows/mac 两块 remap + gkeys（MA/MB/M1-M12 的动作定义）
//       ——纯数据，改它不影响当前跑哪个方案
//     · 运行态 = currentProfile，只由 switchProfile() 改，而 switchProfile()
//       只有两个调用方：宏动作（SW: 段，见 executeGlobalKey/executeToggleKey）
//       和菜单里的「切换配置方案」
//   两者的唯一交集是 getMappedKey() 读 profileRemaps[currentProfile] ——
//   那是一次**读**，不是一个写。
//
//   运行态仍然要跨重启保留（用户要求），所以它照旧写 NVS 的 curr_prof、
//   开机照旧从 NVS 恢复（见 setup 里那段钳位）。只是**不再经过配置通道**：
//   网页整份 JSON 里就算手写了 curr_prof 也不再被应用（见 cfgApplyDoc
//   对"未知键"那条分支的处理），要切方案请走 MA/MB 或全局功能键。
static const CfgField CFG_FIELDS[] = {
    { "disp_mode",   CFG_U8,   &currentDispMode,  0, TOTAL_DISP_MODES - 1, "disp_mode",   CFG_POST_DISP  },
    { "saver_mode",  CFG_U8,   &saverMode,        0, TOTAL_SAVER_MODES - 1,"saver_mode",  CFG_POST_SAVER },
    { "light_on",    CFG_BOOL, &lightOn,          0, 1,                    "light_on",    CFG_POST_NONE  },
    { "brightness",  CFG_U8,   &brightness,       0, 255,                  "brightness",  CFG_POST_NONE  },
    { "effect",      CFG_U8,   &currentEffect,    0, MAX_EFFECTS - 1,      "effect",      CFG_POST_NONE  },
    { "ind_level",   CFG_U8,   &indLevel,         0, 3,                    "ind_level",   CFG_POST_NONE  },
    { "key_fx",      CFG_U8,   &keyFxStyle,       0, KEYFX_COUNT - 1,      "key_fx",      CFG_POST_NONE  },
    { "alarm_on",    CFG_BOOL, &alarmEnabled,     0, 1,                    "alarm_on",    CFG_POST_NONE  },
    { "alarm_h",     CFG_U8,   &alarmHour,        0, 23,                   "alarm_h",     CFG_POST_NONE  },
    { "alarm_m",     CFG_U8,   &alarmMinute,      0, 59,                   "alarm_m",     CFG_POST_NONE  },
    { "sht_offset",  CFG_F32,  &shtTempOffset, -99.9f, 99.9f,              "sht_offset",  CFG_POST_NONE  },
    { "set_epoch",   CFG_U32,  &epochCache,       0, 4294967295.0f,        "set_epoch",   CFG_POST_CLOCK },
};
static const size_t CFG_FIELD_COUNT = sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]);

// ---- 方案三件套的键名表（gkeys 用） ----
//
// GKEY_SLOT[i] 是 JSON 里的键名，GKEY_NVS[i] 是对应的 NVS 键。
// 两张表**必须同序同长** —— 一旦错位，写下去的就是"把 A 的值存进了 B 的键"，
// 而现场毫无异常（只有下次读回来才发现串了）。
//
// 槽位清单（18 个）：
//   MA / MB    老单套全局动作（保留兼容，网页新 UI 不显示但老配置还在）
//   MAa / MAb  MA 的状态 A / 状态 B
//   MBa / MBb  MB 的状态 A / 状态 B
//   M1~M12     M1~M12 各自额外挂的"全局动作"（先跑它，再播按方案宏）
// 相位 g_MA_ph / g_MB_ph 不在这张表里 —— 它是数字不是字符串，单开一处处理。
//
// ⚠⚠ M1~M12 以前**不在**这张表里，而网页的「全局独立功能键」一直在往
//   `gkeys.M1` 写。于是 cfgGkeySlotOf("M1") 返回 -1，落进"未知键忽略"那条
//   分支 —— 固件既不落盘也不报错，网页照常回"已保存并生效"。端到端表现是：
//   · 按 M1 什么都不发生
//   · 看网页本地模型，gkeys.M1 明明在（那是网页自己写的）
//   · 一重新拉取就没了（键盘 NVS 里从来没存进去过）
//   根因是"网页改了通道（GSET: → JSON），固件的槽表没跟着扩"。
//   NVS 键名沿用 g_M1~g_M12 —— executeGlobalKey() 一直读的就是这几个键，
//   所以老版本用 GSET:M1:… 存进去的存量配置**不需要迁移**。
// ⚠ NVS 键名**没有第二张表**，而是从槽名派生的：`g_<槽>`。
//   原来 GKEY_SLOT[] 和 GKEY_NVS[] 是两张并排的字符串数组，注释里专门写着
//   "必须同序同长" —— 而这恰恰是最容易出错、又最不可能被测出来的地方：
//   长度不匹配是静默的越界读，两张表错位则"把 A 的值存进 B 的键"，而当场
//   一切正常。派生之后这个约定从"靠人记得"变成"结构上不可能违反"。
//   （executeGlobalKey() 读的一直就是 g_<键名>，所以存量配置无感知。）
static const char* const GKEY_SLOT[] = {
    "MA", "MB", "MAa", "MAb", "MBa", "MBb",
    "M1", "M2", "M3", "M4", "M5", "M6",
    "M7", "M8", "M9", "M10", "M11", "M12"
};
static const int          GKEY_SLOT_COUNT = (int)(sizeof(GKEY_SLOT) / sizeof(GKEY_SLOT[0]));

// 第 i 个槽对应的 NVS 键，形如 "g_MAa"。
// 按值返回（不是引用）：Preferences::getString/putString/remove 只收 const char*，
// 一旦传 String 引用进去是编译不过的；而"返回局部静态的引用"这种写法，
// 调用方随手存一下就悬空了，不如老老实实按值返回 —— 一次 CFGGET 才 18 个
// 十来个字符的串，拷贝成本可以忽略。
static String gkeyNvsKey(int slot) {
    return String("g_") + GKEY_SLOT[slot];
}


// 每个方案的 M 键个数。宏的 NVS 键是 p<方案>_M<序号>，MACROS_RESET 里
// 硬写的 12、网页的 4×12 批量预加载都是这个数 —— 改一处要改三处。
#define MACRO_SLOTS 12

// 宏的 NVS 键名校验：必须严格是 p<方案>_M<序号>，方案 0~(TOTAL_PROFILES-1)、
// 序号 1~MACRO_SLOTS。
//
// ⚠ 序号要**按十进制解析**，不能只看第一个字符。第一版图省事写成
//   `k[4] ∈ ['1', '0' + MACRO_SLOTS/10]` 且 `k[5]=='\0'`，于是 M10~M12
//   全部被当成非法键名拒掉 —— 而 M1~M9 能过。表现出来就是"保存返回
//   宏键名 p0_M10 不合法"，用户完全不知道是自己名字打错了还是固件有毛病。
//
// ⚠ 为什么要卡得这么死：键名是直接拼进 preferences.putString 的。如果放行
//   任意字符串，一个手滑的 "p99_M1" 或 "hello" 就会在 NVS 里留下一个**永远
//   没人读、也永远删不掉**的键 —— 而 NVS 分区只有 20KB 且只增不减，这种键
//   攒几个就把分区写满了，表现是"别的配置突然存不进去了"。宁可当场报错。
// 把 JSON 里的宏键名翻译成 NVS 键名。
//
// v3 之后 JSON 按操作系统分块（"windows" / "mac"），所以块名已经说明了方案，
// 宏键名里的方案前缀就是冗余的 —— 写成 "M7" 即可，不再是 "p0_M7"。
// 但**旧的 p<n>_M<slot> 形式仍然接受**：v2 的 JSON（含用户手改过、存进浏览器
// 历史的）不能因为升个版本就全部失效。
//
// 三条规则，都是为了不制造"看着存进去了、其实落到别处"的静默错位：
//   "M7"    在 prof 块里 → p<prof>_M7            （新写法，块即方案）
//   "p0_M7" 在 prof 块里 → 方案号必须等于 prof，否则**拒**（而不是"按块的来"）
//   "p0_M01"             → 拒（前导零，理由见下面）
//
// 为什么"方案号与所在块不符"要拒而不是听块的：用户把一段 v2 的旧 JSON 直接
// 粘进来时，"p0_M7" 出现在 "mac" 块里几乎一定是"从 windows 复制过来忘了改"。
// 默默存成 mac 的 M7 等于"按了 M7 结果 Windows 的宏跑在了 mac 上"，
// 而页面上看不出任何异常 —— 这比直接报错难查一百倍。
static bool cfgMacroNvsKey(int prof, const char* k, char* out, size_t cap,
                           char* err, size_t errCap) {
    if (k == nullptr || prof < 0 || prof >= TOTAL_PROFILES) return false;
    const char* slotPart = nullptr;
    if (k[0] == 'M' || k[0] == 'm') {
        slotPart = k + 1;                       // 新写法：M7
    } else if (k[0] == 'p' && k[2] == '_' && (k[3] == 'M' || k[3] == 'm')) {
        // 旧写法：p<n>_M<slot>，必须确认 n 就是当前块
        if (k[1] < '0' || k[1] > '9') return false;
        int n = k[1] - '0';
        if (n != prof) {
            snprintf(err, errCap,
                     "宏键名 \"%s\" 写的是方案%d,但放在了 \"%s\" 块里(方案%d);"
                     "去掉 p%d_ 前缀直接写 M%s 即可",
                     k, n, CFG_PROF_NAMES[prof], prof, n, k + 4);
            return false;
        }
        slotPart = k + 4;
    } else {
        snprintf(err, errCap,
                 "宏键名 \"%s\" 不合法(块内直接写 M1~M%d,例:M7)", k, (int)MACRO_SLOTS);
        return false;
    }
    // 挡掉**前导零**（"M01"）：它按十进制解析等于槽 1，能过下面的范围检查，
    // 但 NVS 里存下的键是 `p0_M01`，而 executeMacro 读的是 `p0_M1` —— 两者永远
    // 对不上。表现出来就是"保存返回成功、这个宏按 M1 毫无反应"，而且那把键
    // 还留在 NVS 里谁都删不掉。
    if (slotPart[0] == '0') {
        snprintf(err, errCap, "宏键名 \"%s\" 不能有前导零(写 M7,不是 M07)", k);
        return false;
    }
    int slot = 0, digits = 0;
    for (const char* q = slotPart; *q != '\0'; q++) {
        if (*q < '0' || *q > '9') {
            snprintf(err, errCap, "宏键名 \"%s\" 里有非数字字符", k);
            return false;
        }
        slot = slot * 10 + (*q - '0');
        if (++digits > 3) {
            snprintf(err, errCap, "宏键名 \"%s\" 的槽位号太长", k);
            return false;                       // 防 "M99999999" 溢出
        }
    }
    if (digits == 0) {
        snprintf(err, errCap, "宏键名 \"%s\" 缺槽位号(要写成 M1~M%d)", k, (int)MACRO_SLOTS);
        return false;
    }
    if (slot < 1 || slot > MACRO_SLOTS) {
        snprintf(err, errCap, "宏槽位 %d 超出 1~%d(键名 \"%s\")", slot, (int)MACRO_SLOTS, k);
        return false;
    }
    snprintf(out, cap, "p%d_M%d", prof, slot);
    return true;
}

// 键名 → GKEY_SLOT 下标；不是那 6 个槽之一就返回 -1。
static int cfgGkeySlotOf(const char* k) {
    if (k == nullptr) return -1;
    for (int i = 0; i < GKEY_SLOT_COUNT; i++) {
        if (strcmp(k, GKEY_SLOT[i]) == 0) return i;
    }
    return -1;
}

// 某个方案的 remap 整份替换：先删干净旧的，再逐条写新的，最后**回读校验**。
// 返回实际存住（回读逐条比对通过）的条数。nRules<=0 表示"只清空"。
//
// ⚠ 为什么必须回读校验：RAM 里有这份表 ≠ flash 里有这份表。Preferences::put*
//   写不下新键时返回 0（NVS 分区只有 20KB，是只增不减的日志结构存储），
//   而当次运行按键照样按 RAM 这份新表工作 —— 表现是"配好了当场能用，
//   断一次电全没了，再配一次还是不行"。而单条规则通常写得进去（新键少、
//   旧条目还能就地覆盖），"只配一条正常、配两条就丢"这个不对称正指向这里。
//   所以以**回读结果**为准，并把存不住这件事摆到屏幕上（别让用户以为成功了）。
//
// 这条路径被两处共用：配置 JSON 的 remap 块、老协议 REMAP:<p>:clear:<rules>。
// 共用是故意的 —— 两条路必须落出**完全一样**的 NVS 布局，否则用户在 JSON 里
// 改一遍、老协议再存一遍，两边会互相覆盖出不同的结果。
static int cfgStoreRemap(int prof, const RemapRule* rules, int nRules) {
    ct_mark(CT_S_CFG_APPLY);       // remap 是目前唯一"一改就崩"的路径,单独也标一次
    if (prof < 0 || prof >= TOTAL_PROFILES) return 0;
    if (nRules > MAX_REMAP_RULES) nRules = MAX_REMAP_RULES;

    // 0. **整份没变就别碰 flash。**
    //
    // NVS 是只增不减的日志结构：写一次就是往分区尾巴上追加一条，旧的留着等 GC。
    // 所以"把 32 个键全删一遍再写一遍"这种做法，哪怕值一模一样，也在每次保存时
    // 制造几十条垃圾条目 —— 分区就这么被自己写满的（满了之后 put* 静默失败，
    // 于是"清空方案"看着成功、开机又还是老那两条）。
    //
    // 网页每次保存都会把**整份**配置下发过来，remap 这一块多半根本没动。
    // 先比一遍，没变直接返回，能省掉一次几十条的 NVS 写入和后面那圈回读。
    if (nRules == remapCounts[prof]) {
        bool same = true;
        for (int i = 0; i < nRules; i++) {
            if (profileRemaps[prof][i].fromKey != rules[i].fromKey ||
                profileRemaps[prof][i].toKey   != rules[i].toKey) { same = false; break; }
        }
        if (same) {
            ELINFO("REMAP", "方案%d 仍是 %d 条，没变，跳过写入", prof + 1, nRules);
            return nRules;
        }
    }

    // 1. 老的全删。⚠ 不能只把条数置 0：那些 rmp_<p>_<i> 键还留在 NVS 里，
    //    换固件/读档时会诈尸，而且再也清不掉。
    //    （键本来就不存在时 nvs_erase_key 直接返回 NOT_FOUND、不产生写入，
    //     所以这里按满 32 个删并不会平白写花分区，保留全删是为了能捞干净历史残留。）
    for (int i = 0; i < MAX_REMAP_RULES; i++) {
        char itemKey[20];
        snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
        preferences.remove(itemKey);
        if ((i & 15) == 0) btLinkKeepAlive();
    }
    memset(profileRemaps[prof], 0, sizeof(profileRemaps[prof]));
    remapCounts[prof] = 0;
    char cntKey[16];
    snprintf(cntKey, sizeof(cntKey), "rmp_cnt_%d", prof);
    preferences.putInt(cntKey, 0);

    // 2. 新的逐条写
    int nvsFail = 0;
    for (int i = 0; i < nRules; i++) {
        profileRemaps[prof][i] = rules[i];
        // ⚠ 必须看返回值，理由见上面的注释
        char itemKey[20];
        snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
        uint32_t val = ((uint32_t)rules[i].fromKey << 16) | (uint32_t)rules[i].toKey;
        if (preferences.putUInt(itemKey, val) == 0) nvsFail++;
        remapCounts[prof] = i + 1;
        btLinkKeepAlive();      // 每写一条喂一次狗，见 cfgApplyDoc 第二遍那段
    }
    preferences.putInt(cntKey, remapCounts[prof]);

    // 3. 回读校验：条数 + 每一条的两个键，都重新比一遍
    int stored = preferences.getInt(cntKey, -1);
    if (stored != remapCounts[prof]) nvsFail++;
    int verified = 0;
    for (int i = 0; i < remapCounts[prof]; i++) {
        char itemKey[20];
        snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
        uint32_t back = preferences.getUInt(itemKey, 0xFFFFFFFFUL);
        uint16_t rf = normalizeRemapKey((uint16_t)(back >> 16));
        uint16_t rt = normalizeRemapKey((uint16_t)(back & 0xFFFF));
        if (rf == profileRemaps[prof][i].fromKey && rt == profileRemaps[prof][i].toKey) verified++;
    }
    // flash 里的东西不可信 → 把 RAM 也对齐成"实际存住的那部分"，
    // 否则当次运行看起来正常、下次开机又变样，更没法查。
    if (nvsFail > 0) remapCounts[prof] = verified;

    ELINFO("REMAP", "方案%d 保存 %d 条 回读通过 %d 条 失败 %d",
           prof + 1, remapCounts[prof], verified, nvsFail);
    if (nvsFail > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "只存住 %d/%d 条", verified, remapCounts[prof]);
        ELERR("REMAP", "%s NVS分区可能已满", buf);
        cfgQueueRemapHud(buf);     // ⚠ 只置位,HUD 交给 cfgPostPump,见上面那段注释
    }
    return verified;
}

// 落盘：按类型挑 put* 函数。新加一个类型记得在这加一个 case ——
// 漏了的话字段会被"应用成功但重启后丢失"，而且现场毫无异常。
//
// ⚠⚠ 参数**必须**是下标，不能写成 `const CfgField& f`。原因是 PlatformIO 处理
//   .ino 的方式：它用正则把所有函数原型**提到文件顶部**（插在第一个原型所在的位置，
//   本文件是第 56 行 pushLogLine 那一带），一共 73 个。而 struct CfgField 声明在
//   5000 行之后 —— 原型一提上去就成了 "'CfgField' does not name a type"，
//   编译直接失败，而且报错位置指向几百行外的无关代码，极难看出真因。
//   （本文件里 handleCommand(const String&) 没事，是因为 String 是 Arduino.h
//   已知的类型；**只有自定义类型会中招**。）
//   所以这里一律传下标，内部自己去表里取。
static void cfgPutNvs(size_t idx) {
    const CfgField& f = CFG_FIELDS[idx];
    switch (f.type) {
        case CFG_U8:   preferences.putUChar(f.nvsKey, *(uint8_t*)f.var);   break;
        case CFG_BOOL: preferences.putBool (f.nvsKey, *(bool*)f.var);      break;
        case CFG_F32:  preferences.putFloat(f.nvsKey, *(float*)f.var);     break;
        case CFG_U32:  preferences.putUInt (f.nvsKey, *(uint32_t*)f.var);   break;
    }
}

// ============================ 缓冲分配 ============================
//
// 函数体放在这里而不是文件顶部那一段，是因为 ELERR 来自 elog.h，而 elog.h 是
// 本文件后段才 include 的（顶部只有 ArduinoJson / bt_link.h）。
//
// 放在 PSRAM：编译带了 -DBOARD_HAS_PSRAM / -DBOARD_HAS_PSRAM_NOW，ps_malloc 会
// 优先从 8MB PSRAM 里切，内部 DRAM 一字节不占。分配失败必须明确报错，
// **不能退回固定小缓冲** —— 那等于把 v2 的配置静默截断成半份。
//
// ⚠ 两份都多要 1 字节：收侧最后要在 cfgRxBuf[cfgRxLen] 写一个 '\0'，而
//   cfgRxLen 最大能等于 CFG_BUF_SIZE（声明总长恰好等于上限时）。按 CFG_BUF_SIZE
//   正好分配的话，这一写就越界一个字节。
static bool cfgBufEnsure(void) {
    if (cfgTxBuf == nullptr) {
        cfgTxBuf = (char*)ps_malloc(CFG_BUF_SIZE + 1);
        if (cfgTxBuf == nullptr) {
            ELERR("CFG", "ps_malloc %d 字节（发送）失败", (int)(CFG_BUF_SIZE + 1));
            return false;
        }
    }
    if (cfgRxBuf == nullptr) {
        cfgRxBuf = (char*)ps_malloc(CFG_BUF_SIZE + 1);
        if (cfgRxBuf == nullptr) {
            ELERR("CFG", "ps_malloc %d 字节（接收）失败", (int)(CFG_BUF_SIZE + 1));
            return false;
        }
    }
    return true;
}

// ============================ 下载：固件 → 网页 ============================
//
// 生成压缩成一行的 JSON（不能有 \n / \r，理由见 CFG_RX_CHUNK_MAX 那段）。
// 返回写出的字节数（不含结尾的 '\0'）。放不下就返回 0，调用方负责报错 ——
// 宁可让网页看到"缓冲不足"，也不要静默截断出一份语法不完整、看着还挺像样的
// JSON：那种错误会在网页那边报成"某一行 unexpected end of input"，极难查。
//
// v2 在十三个标量之外多三块**方案**数据。三块的取舍不一样，先说清楚：
//
// · remap  —— 发**定长**的 4 个数组（一个方案一个，没规则就是空数组 []）。
//            必须发空数组：数组一旦缺项，"这个方案没规则"和"这个方案没提"
//            就分不开了，而后者按本模块的规矩是"不动"，会让"清空某方案"
//            根本清不掉。
// · macros —— 发**稀疏**对象，只带真正设过的键（"p0_M1": "SEQ:ab"）。
//            48 个键全发的话光键名就 400 多字节，而绝大多数人只设了三五个。
//            没设过的键**压根不出现**（不是 null）："清空某个宏"在网页上
//            就是把那一行删掉，少一个键 = 删一个宏，语义正好。
// · gkeys  —— 同上稀疏，但两个相位（MA_ph / MB_ph）**永远发**，因为 0
//            是合法值，"没发"和"是 0"必须能区分开。
static size_t cfgBuildJson(char* out, size_t cap) {
    ELINFO("MEM", "组串 栈%uB", (unsigned)(uxTaskGetStackHighWaterMark(NULL)));
    JsonDocument doc;
    doc["version"] = CFG_SCHEMA_VERSION;
    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        const CfgField& f = CFG_FIELDS[i];
        switch (f.type) {
            case CFG_U8:   doc[f.key] = *(uint8_t*)f.var;   break;
            case CFG_BOOL: doc[f.key] = *(bool*)f.var;      break;
            case CFG_F32:  doc[f.key] = *(float*)f.var;     break;
            case CFG_U32:  doc[f.key] = *(uint32_t*)f.var;   break;
        }
    }

    // ---- curr_prof：**只读上报**，不是可写配置 ----
    //
    // 它已经从 CFG_FIELDS 里摘掉了（见那张表的注释），但网页仍然需要知道
    // "键盘此刻在跑哪个方案" —— 否则用户配完 macOS 的映射，看不出还没切
    // 过去，只会怀疑自己配错了。所以这里单独补一个**只读**字段。
    //
    // ⚠ 只出不收：cfgApplyDoc 那边遍历的是 CFG_FIELDS，curr_prof 不在里面，
    //   所以 JSON 里就算手写 curr_prof 也不会被应用（落进"未知键忽略"那条
    //   分支）。方向是单向的：固件 → 网页 有，网页 → 固件 无。
    //   切方案只能靠 MA/MB 或全局功能键的动作（SW: 段）。
    doc["curr_prof"] = currentProfile;

    // ---- last_map：最近一次普通按键的查表现场（只读）----
    //
    // 它回答的是"配置看着没问题，为什么按键行为不对"里唯一缺的那块证据：
    // 这一颗键**到底查了哪张表**、那张表当时有几条规则、命中没有。
    // 六个字段全是只读，cfgApplyDoc 那边不认这个键（落进"未知键忽略"分支）。
    {
        JsonObject lm = doc["last_map"].to<JsonObject>();
        if (lmProf == 0xFFFFFFFFu) {
            lm["prof"] = -1;   // 还没按过任何普通键
            lm["cnt"]  = 0;
            lm["from"] = 0;
            lm["to"]   = 0;
            lm["hit"]  = 0;
        } else {
            lm["prof"] = (int)lmProf;
            lm["cnt"]  = (int)lmCount;
            lm["from"] = lmFrom;
            lm["to"]   = lmTo;
            lm["hit"]  = (lmTo != lmFrom) ? 1 : 0;
        }
        lm["hits"] = (int)lmHits;   // 开机以来累计命中次数
    }

    // ---- 每个方案一个对象：CFG_PROF_NAMES[p] = { "remap": [...], "macros": {...} } ----
    //
    // 走 RAM 里的 profileRemaps / remapCounts，不现读 NVS：这两个数组是
    // setup() 里已经把 NVS 全 load 进来过的运行时副本，读它和读 NVS 等价，
    // 但省掉一整轮 NVS 查表（每次都要走一遍分区查找）。
    //
    // 两种取值的取舍（和 v2 一样，但换成了按块组织）：
    // · remap  —— **定长**数组，一个都没配就是 []。必须发空数组：数组一旦缺项，
    //            "这个方案没规则"和"这个方案没提"就分不开了，而后者按本模块的
    //            规矩是"不动"，会让"清空某个方案"根本清不掉。
    // · macros —— **稀疏**对象，只带真正设过的键（"M7": "SEQ:ab"）。24 个键
    //            全发的话光键名就 200 多字节，而绝大多数人只设了三五个。
    //            没设过的键**压根不出现**（不是 null）："清空某个宏"在网页上
    //            就是把那一行删掉，少一个键 = 删一个宏，语义正好。
    //            键名里不再带 p 前缀 —— 块名已经说明了是哪个系统，见 cfgMacroNvsKey。
    for (int p = 0; p < TOTAL_PROFILES; p++) {
        JsonObject prof = doc[CFG_PROF_NAMES[p]].to<JsonObject>();
        JsonArray remap = prof["remap"].to<JsonArray>();
        int cnt = remapCounts[p];
        if (cnt > MAX_REMAP_RULES) cnt = MAX_REMAP_RULES;   // 被人手改过 NVS 时的兜底
        for (int i = 0; i < cnt; i++) {
            JsonObject r = remap.add<JsonObject>();
            r["from"] = (uint16_t)profileRemaps[p][i].fromKey;
            r["to"]   = (uint16_t)profileRemaps[p][i].toKey;
        }
        JsonObject macros = prof["macros"].to<JsonObject>();
        for (int m = 1; m <= MACRO_SLOTS; m++) {
            char nk[16], mk[16];
            snprintf(mk, sizeof(mk), "p%d_M%d", p, m);
            String v = preferences.getString(mk, "");
            if (v.length() > 0) {
                // 超过上限的（理论上不可能，见 CFG_MACRO_MAX）截断并留痕 ——
                // 宁可让用户看到"有个宏只回了一半"，也不能让整份 JSON 生成失败。
                if (v.length() > CFG_MACRO_MAX) {
                    v = v.substring(0, CFG_MACRO_MAX);
                    ELWARN("CFG", "宏 %s 超过 %d 字节,回拉时截断", mk, (int)CFG_MACRO_MAX);
                }
                snprintf(nk, sizeof(nk), "M%d", m);
                macros[nk] = v;
            }
            if ((m & 3) == 0) btLinkKeepAlive();
        }
    }

    // ---- gkeys：MA/MB 的双状态 + M1~M12 的全局动作 + 相位（全局，不分系统）----
    //
    // 留在顶层而不是塞进 windows/mac：这些是"按这颗键额外做什么"，和当前跑在
    // 哪个操作系统没关系，拆成两套只会让 NVS 多占一倍、页面多出一倍卡片。
    JsonObject gk = doc["gkeys"].to<JsonObject>();
    for (int i = 0; i < GKEY_SLOT_COUNT; i++) {
        const String nk = gkeyNvsKey(i);
        String v = preferences.getString(nk.c_str(), "");
        if (v.length() > 0) {
            if (v.length() > CFG_GKEY_MAX) {
                v = v.substring(0, CFG_GKEY_MAX);
                ELWARN("CFG", "全局键 %s 超过 %d 字节，回拉时截断", nk.c_str(), (int)CFG_GKEY_MAX);
            }
            gk[GKEY_SLOT[i]] = v;
        }
        btLinkKeepAlive();
    }
    // 相位永远发：0 是合法值，"没这个键"和"相位是 0"必须分得开
    gk["MA_ph"] = preferences.getUChar("g_MA_ph", 0);
    gk["MB_ph"] = preferences.getUChar("g_MB_ph", 0);

    // ---- timer：倒计时（只读，不落盘）----
    //
    // ⚠ 这块是**只读**的：cfgApplyDoc 收到它会原样忽略，不写 NVS、不改运行状态。
    //   倒计时按设计就是运行内存里的东西（TIMERSET: 设、跑完归零、断电归零），
    //   落盘会带来两个坏处：
    //     · 断电重启后倒计时"复活"了，而用户以为它已经随断电结束
    //     · 每次打开网页点任意一个"保存"都会把这个快照写一遍，把一个
    //       纯运行时的量伪装成配置 —— 比没有这个字段更容易误导
    //   所以它进 JSON 只有一个目的：**让页面能看到键盘此刻正在跑什么**。
    //   三个字段都发：run 用来显示"跑着/停了"，remain 是此刻真剩多少秒
    //   （不是快照，是每次拉的时候现算的），total 是当初设的整段时长。
    {
        JsonObject t = doc["timer"].to<JsonObject>();
        t["run"] = (bool)timerRunning;
        t["remain"] = (long)timerRemainSec;
        t["total"] = (long)timerTotalSec;
    }

    // ⚠ overflowed() 必须在序列化**之前**判，而且它是这一整段里最重要的一行检查。
    //
    //   doc 装不下时，ArduinoJson **不报错**：装不下的那些键被整个静默丢弃，
    //   而 serializeJson 照样返回一个"看起来很正常"的长度（它只算自己实际
    //   能写出的字节数）。于是网页收到一份**语法合法、版本号也对、就是缺了
    //   一半键**的配置，页面照着它把表单填好、用户再点一次保存就把缺掉的
    //   部分**清零**了 —— 整条链路上没有任何一处会报错。这就是和"丢片"同一
    //   类的静默数据损坏，只不过发生在固件这一侧。
    //
    //   实测不会经常触发：JsonDocument 在 ESP32 上走 malloc，池子按需涨。
    //   但"配置项越加越多，某天悄悄越过"正是它唯一的失效方式，所以必须挡。
    if (doc.overflowed()) {
        snprintf(cfgBuildErr, sizeof(cfgBuildErr), "JSON 装不下（宏/全局键太多）");
        ELWARN("CFG", "%s", cfgBuildErr);
        return 0;
    }
    size_t n = serializeJson(doc, out, cap);
    if (n == 0 || n >= cap) {
        snprintf(cfgBuildErr, sizeof(cfgBuildErr), "缓冲不够（要 %d 字节，上限 %d）",
                 (int)n + 1, (int)cap);
        return 0;
    }
    // 兜底：正文里混进 \n 或 \r 就没法按行攒了，宁可当场失败。
    if (memchr(out, '\n', n) != nullptr || memchr(out, '\r', n) != nullptr) {
        snprintf(cfgBuildErr, sizeof(cfgBuildErr), "配置内容含换行");
        return 0;
    }
    return n;
}

// ============================ 上传：网页 → 固件 ============================
//
// 返回真正应用的字段数；<0 表示整体被拒（err 里带原因）。
//
// 三条设计决定，都是踩过的坑：
//
// 1. **缺字段 = 不动，不是清零。** 网页只发了它认识的那几项，没发的保持原样。
//    之前 REMAP 那条命令就是"只要带了 rules 就先清空"（见 handleCommand 里
//    REMAP: 的注释），语义上是"整份替换"；配置 JSON 不这样 —— 缺项清零会让
//    "只改亮度"顺手把闹钟、方案全抹掉，而且界面上看不出任何异常。
//
// 2. **超范围 = 拒整个请求，不夹取。** 亮度写成 9999 夹成 255 看着像"生效了"，
//    其实是把一个 bug 藏起来。这里让整个请求失败并在 CFGSAVE:ERR 里点名是哪个
//    字段，网页能原样显示给用户。
//
// 3. **未知字段 = 忽略，不报错。** 网页比固件新时会多发一些字段（比如网页已经
//    支持宏、这版固件还没接），忽略掉才能让两端独立升级；但已知字段一个都不许错。
// 整份（isPatch=false）和增量（isPatch=true）走的是**同一个函数**。
//
// 这不是图省事，是这份文档的语义本来就是增量的：所有字段的规则都是
// "出现 = 动，没出现 = 不动"（标量如此，<块>.remap 整份替换、macros /
// gkeys 逐键，详见下面方案块那段注释）。所以一份只带 disp_mode 的补丁，
// 和一份带着 disp_mode 的整份配置，在这套语义下作用完全一样 —— 区别只在
// 有没有 version 那道闸门。
static int cfgApplyDoc(const char* json, size_t len, bool isPatch, char* err, size_t errCap) {
    ct_mark(CT_S_CFG_APPLY);       // 崩在这一段里时,RTC 慢存能指出是"配置落盘"
    // 动手前把栈余量记一笔。崩了之后这一行就是"出事瞬间"的余量，
    // 比开机那次有意义得多（开机时一切正常，余量看着很宽裕）。
    // 正文要短：错误日志一页只有一行正文的位置（见 elog 行那段）。
    // ⚠️ uxTaskGetStackHighWaterMark 在 ESP-IDF 里返回的是**字节**，不是原版
    //   FreeRTOS 的"字数"（IDF 改过这个 API：in bytes not words, unlike
    //   vanilla FreeRTOS）。之前这里多乘了 4，报出 21856B —— 比 loopTask
    //   整个栈（8192B）还大，一眼就知道换算错了。真实值是 5464B。
    ELINFO("MEM", "落盘 栈%uB", (unsigned)(uxTaskGetStackHighWaterMark(NULL)));
    nNvsFail = 0;                 // 每次请求从零开始，失败名单不跨请求累积
    cfgFailNames[0] = '\0';
    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, json, len);
    if (de) {
        // ⚠ ArduinoJson **7.x** 的 DeserializationError 只有 code() / c_str()，
        //   **没有 offset()**（v6 有，v7 移除了）。所以别写 de.offset，会编译不过。
        //   好在这几个码本身就够定位问题了，翻成中文直接给用户看：
        //   IncompleteInput 是这里最常见的一个 —— 几乎总是"少收了几片"，
        //   而不是 JSON 本身写错了。
        switch (de.code()) {
            case DeserializationError::IncompleteInput:
                snprintf(err, errCap, "JSON 不完整(多半是传输丢片)");
                break;
            case DeserializationError::EmptyInput:
                snprintf(err, errCap, "JSON 是空的");
                break;
            case DeserializationError::InvalidInput:
                snprintf(err, errCap, "JSON 语法错(多余的逗号/括号不配对)");
                break;
            case DeserializationError::NoMemory:
                snprintf(err, errCap, "内存不够,解析不了");
                break;
            case DeserializationError::TooDeep:
                snprintf(err, errCap, "JSON 嵌套太深");
                break;
            default:
                snprintf(err, errCap, "JSON 解析失败: %s", de.c_str());
                break;
        }
        return -1;
    }
    if (doc.is<JsonObject>() == false) {
        snprintf(err, errCap, "顶层不是 JSON 对象");
        return -1;
    }

    // ---- version 当成"这份 JSON 是不是给我看的"来校验 ----
    //
    // ⚠ 第一版是**静默跳过** version 的（它不在字段表里，循环里直接 continue），
    //   结果网页上把 version 改成 2 点保存，固件回 OK:13，重新拉回来还是 1，
    //   页面显示"已保存"而用户看什么都没变 —— 正是这个功能本来要消灭的那类
    //   "界面说成功、其实没生效"。所以现在把它变成一道**闸门**：
    //   不等于固件认识的版本就整份拒绝，并说清差在哪。
    //
    // 往后字段会一个个加进来，CFG_SCHEMA_VERSION 往上加；网页那边带的
    // version 更老（浏览器缓存了旧页面）或更新（网页先发版、固件还没跟上）
    // 都会在这里被明确挡住，而不是悄悄丢字段。
    //
    // ⚠ 增量（isPatch）**跳过**这道闸门：补丁不带 version，而且真要对齐版本
    //   也没必要在这里做 —— 补丁的语义是"只动我点名的那些"，网页拿老版本的
    //   补丁打过来，最坏结果是少改几项（它没提到的键本来就不动），而不是
    //   写坏数据。整份那条路（isPatch=false）才继续硬拦。
    if (!isPatch) {
        if (doc["version"].isNull()) {
            snprintf(err, errCap, "缺少 version 字段");
            return -1;
        }
        if (!doc["version"].is<int>()) {
            snprintf(err, errCap, "version 必须是数字");
            return -1;
        }
        int ver = doc["version"].as<int>();
        if (ver != CFG_SCHEMA_VERSION) {
            snprintf(err, errCap, "版本对不上:这份 JSON 是 v%d,本固件只认 v%d"
                                 "(请刷新网页重新拉取一份)",
                     ver, CFG_SCHEMA_VERSION);
            return -1;
        }
    }

    // ---- 第一遍：先校验，一个都不落地 ----
    // 为什么不先写完再报错：网页发来的 JSON 少一项、多一个逗号都可能，
    // 半途落盘会留下"一半新一半旧"的配置，而用户完全看不出是哪一半。
    // changed 由第二遍赋值时顺手记下来，后面挂副作用时**只读它**。
    // 曾经 pendMask 那一段想"再比一次看看变没变"，可第二遍早就把新值写进
    // 变量了 —— 再比必然相等，于是 pendMask 恒为 0，副作用一个都挂不上。
    struct Pending { const CfgField* f; double num; bool boolean; bool changed; };
    static Pending pend[CFG_FIELD_COUNT];
    int nPend = 0;

    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        const CfgField& f = CFG_FIELDS[i];
        JsonVariantConst v = doc[f.key];
        if (v.isNull()) continue;               // 没这个键 → 这项不动（决定 1）
        // version 已在上一步当闸门校验过，不是一个可写字段，跳过

        if (f.type == CFG_BOOL) {
            if (!v.is<bool>()) {
                snprintf(err, errCap, "%s 应该是 true/false", f.key);
                return -1;
            }
            pend[nPend].f = &f;
            pend[nPend].boolean = v.as<bool>();
            pend[nPend].num = 0;
            pend[nPend].changed = false;
            nPend++;
            continue;
        }
        if (!v.is<int>() && !v.is<float>() && !v.is<double>()) {
            snprintf(err, errCap, "%s 应该是数字", f.key);
            return -1;
        }
        double num = v.as<double>();
        if (num < f.lo || num > f.hi) {
            snprintf(err, errCap, "%s 超出范围 (%g~%g),收到 %g",
                     f.key, (double)f.lo, (double)f.hi, num);
            return -1;
        }
        pend[nPend].f = &f;
        pend[nPend].num = num;
        pend[nPend].boolean = false;
        pend[nPend].changed = false;
        nPend++;
    }

    // ---- 方案块：结构校验（和标量一样，"先校验，一个都不落地"）----
    //
    // v3 起方案块是按操作系统分的两个对象：`"windows"` 和 `"mac"`（名字见
    // CFG_PROF_NAMES）。每块下面挂它自己的 remap + macros，gkeys 留在顶层。
    //
    // 语义各不相同，先钉死：
    //   <块>.remap  出现 = **整份替换**该方案（空数组 = 清空）。
    //                块缺位 = 不动。
    //   <块>.macros 出现 = 写这一个键；值为 null / "" = 删这一个键。
    //                没出现的键 = 不动。键名不带方案前缀（块名已经说明了）。
    //   gkeys       同 macros；另外两个相位是数字。
    //   timer        **只读**：校验形状但绝不落盘，也不改运行状态。
    //
    // 为什么 remap 是"整份替换"而 macros 是"逐键"：remap 在网页上是一个
    // **列表编辑器**（用户勾了一堆规则点保存，页面只知道保存后的完整列表），
    // 逐键语义下用户删掉一条规则就永远删不掉。macros / gkeys 是一个键一个
    // 表单项，各自独立存，网页改 M3 不该动 M1~M2。
    // ⚠ 这些必须声明成 **JsonVariant（可变）**而不是 JsonVariantConst：
    //   doc 本身是可变的 JsonDocument，而 ArduinoJson 7 不允许
    //   JsonVariantConst → JsonArray 这种"加可变性"的转换（只有反向成立），
    //   写成 const 会报 InvalidConversion。
    for (int p = 0; p < TOTAL_PROFILES; p++) {
        JsonVariant jProf = doc[CFG_PROF_NAMES[p]];
        if (jProf.isNull()) continue;             // 整个块没提 = 这方案不动
        // ⚠⚠ 必须是 is<JsonObjectConst>()，**不能**写 is<JsonObject>()。
        //
        //   这个坑很阴：写错了**编译得过、运行时恒为 false**、而且不报任何错。
        //   原因是 ArduinoJson 7 把 is<T>() 拆成了两个重载：
        //     · Converter<T>::fromJson 的首参**恰好**是 JsonVariantConst 才走真判定
        //     · 否则命中另一个重载，函数体就一句 `return false;`
        //   而 Converter<JsonObject>::fromJson 收的是**非 const** 的 JsonVariant
        //   （Converter<JsonObjectConst>::fromJson 才收 JsonVariantConst），
        //   所以在 JsonVariantConst 上写 is<JsonObject>() —— 对**任何**输入都返回
        //   false，包括完全合法的对象。
        //
        //   现场表现：只要 JSON 里带这个块，保存必被拒，且报的还是
        //   "xxx 应该是对象" —— 一句**完全误导**的话，用户会以为 JSON 写错了，
        //   去检查括号、检查引号，检查一百遍也查不出来。
        if (!jProf.is<JsonObjectConst>()) {
            snprintf(err, errCap, "\"%s\" 应该是对象 (含 remap / macros 两个子项)",
                     CFG_PROF_NAMES[p]);
            return -1;
        }
        JsonObjectConst profObj = jProf.as<JsonObjectConst>();

        // ---- remap ----
        JsonVariantConst jRemap = profObj["remap"];
        if (!jRemap.isNull()) {
            if (!jRemap.is<JsonArrayConst>()) {
                snprintf(err, errCap, "\"%s\".remap 应该是规则数组"
                         "(形如 [{\"from\":128,\"to\":131}])", CFG_PROF_NAMES[p]);
                return -1;
            }
            JsonArrayConst ra = jRemap.as<JsonArrayConst>();
            if (ra.size() > (size_t)MAX_REMAP_RULES) {
                snprintf(err, errCap, "\"%s\".remap 有 %u 条,上限 %d",
                         CFG_PROF_NAMES[p], (unsigned)ra.size(), MAX_REMAP_RULES);
                return -1;
            }
            for (size_t i = 0; i < ra.size(); i++) {
                JsonVariantConst r = ra[i];
                if (!r.is<JsonObjectConst>()) {
                    snprintf(err, errCap, "\"%s\".remap[%u] 应该是 {\"from\":...,\"to\":...}",
                             CFG_PROF_NAMES[p], (unsigned)i);
                    return -1;
                }
                JsonVariantConst fv = r["from"];
                JsonVariantConst tv = r["to"];
                if (!fv.is<int>() || !tv.is<int>()) {
                    snprintf(err, errCap, "\"%s\".remap[%u] 的 from/to 应该是数字",
                             CFG_PROF_NAMES[p], (unsigned)i);
                    return -1;
                }
                long fk = fv.as<long>(), tk = tv.as<long>();
                // 键码是 uint16。越界的绝大多数是手抄错了（比如把 224 写成 2224），
                // 夹到 65535 只会让那颗键彻底没反应，必须点名拒掉。
                if (fk < 0 || fk > 65535 || tk < 0 || tk > 65535) {
                    snprintf(err, errCap, "\"%s\".remap[%u] 键码超出 0~65535(收到 %ld->%ld)",
                             CFG_PROF_NAMES[p], (unsigned)i, fk, tk);
                    return -1;
                }
            }
        }

        // ---- macros ----
        JsonVariantConst jMacros = profObj["macros"];
        if (!jMacros.isNull()) {
            if (!jMacros.is<JsonObjectConst>()) {
                snprintf(err, errCap, "\"%s\".macros 应该是对象 (形如 {\"M7\": \"SEQ:...\"})",
                         CFG_PROF_NAMES[p]);
                return -1;
            }
            for (JsonPairConst kv : jMacros.as<JsonObjectConst>()) {
                const char* k = kv.key().c_str();
                char nk[16];
                // 键名在这里就地翻译成 NVS 键名，顺带把"方案号与所在块不符"、
                // 前导零、槽位越界这些一次性挑出来，并且都带上具体原因。
                if (!cfgMacroNvsKey(p, k, nk, sizeof(nk), err, errCap)) return -1;
                JsonVariantConst v = kv.value();
                if (v.isNull()) continue;              // null = 删掉这个宏，合法
                if (!v.is<const char*>()) {
                    snprintf(err, errCap, "宏 %s 的值应该是字符串", k);
                    return -1;
                }
                if (strlen(v.as<const char*>()) > CFG_MACRO_MAX) {
                    snprintf(err, errCap, "宏 %s 超过 %d 字节", k, (int)CFG_MACRO_MAX);
                    return -1;
                }
            }
        }
    }

    JsonVariantConst jGkeys = doc["gkeys"];
    if (!jGkeys.isNull()) {
        // 同上：JsonVariantConst 上必须用 Const 版，见 macros 那段的详细说明
        if (!jGkeys.is<JsonObjectConst>()) {
            snprintf(err, errCap, "gkeys 应该是对象");
            return -1;
        }
        for (JsonPairConst kv : jGkeys.as<JsonObjectConst>()) {
            const char* k = kv.key().c_str();
            int slot = cfgGkeySlotOf(k);
            if (slot >= 0) {
                JsonVariantConst v = kv.value();
                if (v.isNull()) continue;          // null = 删掉这一项，合法
                if (!v.is<const char*>()) {
                    snprintf(err, errCap, "全局键 %s 的值应该是字符串", k);
                    return -1;
                }
                if (strlen(v.as<const char*>()) > CFG_GKEY_MAX) {
                    snprintf(err, errCap, "全局键 %s 超过 %d 字节", k, (int)CFG_GKEY_MAX);
                    return -1;
                }
                continue;
            }
            // 两个相位是数字，范围 0~1
            if (strcmp(k, "MA_ph") == 0 || strcmp(k, "MB_ph") == 0) {
                JsonVariantConst v = kv.value();
                if (!v.is<int>()) {
                    snprintf(err, errCap, "%s 应该是 0 或 1", k);
                    return -1;
                }
                int ph = v.as<int>();
                if (ph < 0 || ph > 1) {
                    snprintf(err, errCap, "%s 只能是 0 或 1,收到 %d", k, ph);
                    return -1;
                }
                continue;
            }
            // 未知键按本模块的老规矩忽略（网页比固件新时才会出现）
        }
    }

    // "什么都没给"要拒：否则网页把整份配置发空了，固件回 OK:0，页面显示
    // "已保存 0 项"，用户以为存上了，其实什么都没发生。
    // timer 单独算：它是只读的，一份只带 timer 的 JSON 等于什么都没要求。
    bool anyScheme = false;
    for (int p = 0; p < TOTAL_PROFILES; p++) {
        JsonVariantConst jProf = doc[CFG_PROF_NAMES[p]];
        if (!jProf.isNull() && jProf.is<JsonObjectConst>()) { anyScheme = true; break; }
    }
    if (nPend == 0 && !anyScheme && doc["gkeys"].isNull()) {
        snprintf(err, errCap, "一个认识的字段都没有(timer 是只读的,不算)");
        return -1;
    }

// ---- 第二遍：校验全过了才真的赋值 + 落盘 ----
//
// ⚠ 每写一个字段喂一次狗。NVS 没有后台提交线程：每写一个新键就要把对应的
//   4KB 页读进来改、再写回去（flash 擦除一扇区几百毫秒），13 个字段连着写
//   轻松就是好几秒。中间不喂狗，整条指令就会顶到 10 秒看门狗上 panic。
//
// ⚠⚠ 第二个 ⚠ 是这一段最要紧的语义：**只在值真的变了的时候才落盘、才做副作用**。
//
//   原来这里是"JSON 里带了 disp_mode 就赋值 + 挂重建"，**完全不看值变没变**。
//   而网页每次都是全量下发（cfgModel 整份序列化），于是只改个亮度，JSON 里
//   也带着 disp_mode，键盘就跟着 applyDispMode() 把主屏整个重建一遍：
//   屏幕闪一下、全部控件指针重来，用户看到的就是"改个亮度，键盘自己跳到
//   设置页面去了"。反过来想也成立：改显示风格本身是真的需要重建的，
//   但那一件事不该由"有没有顺便带上 brightness"来决定。
//
//   顺带还有个 NVS 层面的好处：值没变还写一遍，是**白白消耗分区空间**。
//   nvs 只有 20KB 且是只增不减的日志结构（见 setup() 里的可写性自检），
//   每次全量保存都把 13 个键重写一遍，是在主动缩短这个分区的寿命。
int nChanged = 0;          // 真的变了的标量项数，用来决定挂哪些副作用
for (int i = 0; i < nPend; i++) {
    const CfgField* f = pend[i].f;
    bool changed = false;
    switch (f->type) {
        case CFG_U8:
            if (*(uint8_t*)f->var != (uint8_t)pend[i].num) {
                *(uint8_t*)f->var = (uint8_t)pend[i].num; changed = true;
            }
            break;
        case CFG_BOOL:
            if (*(bool*)f->var != pend[i].boolean) {
                *(bool*)f->var = pend[i].boolean; changed = true;
            }
            break;
        case CFG_F32:
            // ⚠ 浮点必须比阈值不能比等值：JSON 里的 -5.3 解析成 float 是
            //   -5.2999997，和 NVS 里读出来的那个不一定逐位相同。用等值比
            //   的话，"什么都没改"会被判成"变了"，反而又去重建一次。
            if (fabsf(*(float*)f->var - (float)pend[i].num) > 0.0005f) {
                *(float*)f->var = (float)pend[i].num; changed = true;
            }
            break;
        case CFG_U32:
            if (*(uint32_t*)f->var != (uint32_t)pend[i].num) {
                *(uint32_t*)f->var = (uint32_t)pend[i].num; changed = true;
            }
            break;
    }
    if (!changed) continue;      // 没变：不写 NVS、不做副作用、不喂狗
    pend[i].changed = true;      // 记下来给下面的 pendMask 用（见 Pending::changed）
    nChanged++;
    unsigned long tField = millis();
    cfgPutNvs((size_t)(f - CFG_FIELDS));   // 传下标，见 cfgPutNvs 上面的坑
    unsigned long cost = millis() - tField;
    // 只记"慢到值得看一眼"的：全打 13 行会把关键的总耗时行淹掉，
    // 而正常情况下写一个键就是零点几毫秒，没什么可看的。
    if (cost > 50) ELWARN("CFG", "%s 落盘慢了，%lums", f->key, cost);
    btLinkKeepAlive();
}

// ---- 方案块：落盘 ----
//
// 标量写完了才动这些块。顺序无所谓（互不相干），但**都在显式提交之前**，
// 这样回 OK 的时候它们已经一起进了 flash。
int nScheme = 0;   // 方案类改动条数，只用来给 HUD/日志报个数


// timer：**只读，刻意不落盘**。原因见 cfgBuildJson 里 timer 那段。
// 这里显式记一笔，免得以后有人看到"JSON 里有 timer 却没人写它"以为是漏了。
{
    JsonVariantConst jTimer = doc["timer"];
    if (!jTimer.isNull())
        ELINFO("CFG", "timer 是只读的,已忽略(倒计时是运行时状态,断电归零)");
}

// 每个方案块：remap 整份替换 + macros 逐键写/删
for (int p = 0; p < TOTAL_PROFILES; p++) {
    JsonVariantConst jProf = doc[CFG_PROF_NAMES[p]];
    if (jProf.isNull() || !jProf.is<JsonObjectConst>()) continue;
    JsonObjectConst profObj = jProf.as<JsonObjectConst>();

    // remap：出现的方案整份替换（空数组 = 清空）
    JsonVariantConst jRemap = profObj["remap"];
    if (!jRemap.isNull()) {
        JsonArrayConst ra = jRemap.as<JsonArrayConst>();
        RemapRule tmp[MAX_REMAP_RULES];
        int n = 0;
        for (size_t i = 0; i < ra.size() && n < MAX_REMAP_RULES; i++) {
            tmp[n].fromKey = normalizeRemapKey((uint16_t)ra[i]["from"].as<long>());
            tmp[n].toKey   = normalizeRemapKey((uint16_t)ra[i]["to"].as<long>());
            n++;
        }
        // 整份替换，所以 n==0 就是"清空这个方案"
        cfgStoreRemap(p, tmp, n);
        nScheme += n;
    }

        // 逐键写 / 删（值为 null 或 "" = 删）
        // 键名在这里第二次翻译成 NVS 键名 —— 校验段已经保证翻译一定成功。
        //
        // ⚠⚠ 写入后**必须回读校验**，理由和 cfgStoreRemap 那段一样、后果一样：
        //   Preferences::put* 写不下新键时静默失败（返回 0），而旧代码**根本
        //   不看返回值**，直接 ELINFO("已写")。于是网页显示"已保存 N 项"、
        //   键盘当场也按新配置工作（RAM 里有），**断一次电全没了**，而两边
        //   的日志都写着成功。宏一多就特别容易撞上：光 24 个满宏（256 字节
        //   一个）就 6KB，加上映射和全局键，20KB 的 nvs 分区很快就写不下
        //   新键了（它是只增不减的日志结构，旧条目要靠 GC 慢慢回收）。
        //   现在以**回读结果**为准，写不进去就如实报给网页 —— 让"保存成功"
        //   真的等于"存住了"。
        JsonVariantConst jMacros = profObj["macros"];
        if (!jMacros.isNull()) {
            for (JsonPairConst kv : jMacros.as<JsonObjectConst>()) {
                const char* k = kv.key().c_str();
                char nk[16];
                if (!cfgMacroNvsKey(p, k, nk, sizeof(nk), err, errCap)) return -1;
                JsonVariantConst v = kv.value();
                const char* val = v.isNull() ? "" : v.as<const char*>();
                // ⚠⚠ 先读出来比一遍，**没变就一个字节都不写**。
                //
                // 这一条不是省时间，是保命：NVS 只增不减，写一次追加一条日志条目。
                // 网页每次保存都把整份配置下发过来，24 个宏里通常一个都没动 ——
                // 老代码却每个都 putString() 一遍，单次保存凭空制造 6KB 垃圾，
                // 几轮就把分区写满，然后 put* 开始静默失败（"保存成功"但重启即丢）。
                String back = preferences.getString(nk, "");
                if (val[0] == '\0') {
                    if (back.length() > 0) {
                        preferences.remove(nk);
                        ELINFO("CFG", "宏 %s(%s) 已删", nk, k);
                    }
                } else if (back != val) {
                    preferences.putString(nk, val);
                    back = preferences.getString(nk, "");
                    if (back != val) {
                        // 记失败项（宏，.ino 里放不了新函数，见 CFG_FAIL_ADD）
                        CFG_FAIL_ADD(nk);
                        ELWARN("CFG", "宏 %s 没存住(NVS 写不下,分区可能满了)", nk);
                    } else {
                        ELINFO("CFG", "宏 %s(%s) 已写 %u 字节", nk, k, (unsigned)strlen(val));
                    }
                } else {
                    ELINFO("CFG", "宏 %s(%s) 没变,跳过", nk, k);
                }
                nScheme++;
                btLinkKeepAlive();
            }
        }
    }

// gkeys：逐槽写 / 删 + 两个相位
//
// ⚠ 同样要回读校验，理由见上面 macros 那段（写不下新键时 putString 静默失败）。
//   全局键这条尤其容易踩：一条 SW:1+CMB:128,4 之类的组合有一两百字节，
//   而 gkeys 现在有 18 个槽（MA/MB 六套 + M1~M12），加上宏和映射，
//   nvs 那 20KB 很快就满了。
if (!jGkeys.isNull()) {
    for (JsonPairConst kv : jGkeys.as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        int slot = cfgGkeySlotOf(k);
        if (slot >= 0) {
            JsonVariantConst v = kv.value();
            const char* val = v.isNull() ? "" : v.as<const char*>();
            const String nk = gkeyNvsKey(slot);
            // 和宏那段同一个道理：没变就别写（见 cfgStoreRemap 上面那条 ⚠⚠）。
            String back = preferences.getString(nk.c_str(), "");
            if (val[0] == '\0') {
                if (back.length() > 0) {
                    preferences.remove(nk.c_str());
                    ELINFO("CFG", "全局键 %s 已删", k);
                }
            } else if (back != val) {
                preferences.putString(nk.c_str(), val);
                back = preferences.getString(nk.c_str(), "");
                if (back != val) {
                    // 记失败项（同上，宏）
                    CFG_FAIL_ADD(k);
                    ELWARN("CFG", "全局键 %s 没存住(NVS 写不下,分区可能满了)", k);
                } else {
                    ELINFO("CFG", "全局键 %s 已写 %u 字节", k, (unsigned)strlen(val));
                }
            } else {
                ELINFO("CFG", "全局键 %s 没变,跳过", k);
            }
            nScheme++;
        } else if (strcmp(k, "MA_ph") == 0 || strcmp(k, "MB_ph") == 0) {
            // 相位顺带重置了对应的旧单套键吗？—— 不重置。相位是相位，
            // 老单套 g_MA / g_MB 是独立的一条配置，清它是另一件事。
            char nk[16];
            snprintf(nk, sizeof(nk), "g_%s", k);      // "g_MA_ph"
            preferences.putUChar(nk, (uint8_t)kv.value().as<int>());
            nScheme++;
        }
        btLinkKeepAlive();
    }
}

// 显式提交：告诉网页"保存成功"之前，得保证这些值真的进了 flash。
//
// 为什么必须在这儿提交 —— Preferences 是 setup() 里 begin("keyboard", false)
// 开的，全工程**从来没有** end() 过。nvs_set_* 只写进 RAM 里的页缓存，要等到
// 缓存填满或显式提交才落 flash。也就是说：不提交就回"OK"，用户断电一拔
// 配置全没，而界面上明明写着"已保存"。这正是这个功能要解决的那类问题，
// 自己不能犯。
//
// end() 会关掉句柄，必须立刻重新 begin()，否则后面所有 preferences.get*
// 会读到默认值。整段是同步的，中间不会有别的代码来读。
ct_mark(CT_S_CFG_POST);
unsigned long tCommit = millis();
preferences.end();
btLinkKeepAlive();
preferences.begin("keyboard", false);
ELINFO("CFG", "已应用 %d 个设置项（其中方案类 %d 条），NVS 提交用了 %lums",
       nPend, nScheme, (unsigned long)(millis() - tCommit));

// ---- 界面副作用：**只置位**，留给 cfgPostPump 在后面的 loop 里一件一件做 ----
//
// ⚠ 只按**真的变了**的字段来挂 —— 判据是第二遍记下来的 pend[i].changed。
//   原来这里是"JSON 里带了哪个字段就挂哪个"，配合网页的全量下发，等于每改
//   任何一项都会重建主屏 —— 那正是"改个亮度键盘自己跳到设置页面"的成因。
//
// ⚠⚠ 后来改成了"再比一遍 `*f->var == pend[i].num` 确认变没变"，那是更糟的一版：
//   第二遍早就把新值写进那个变量了，所以这个比较**恒为相等**、恒判成"没变"、
//   恒 continue —— pendMask 永远是 0，**一个副作用都挂不上**。
//   现场表现：网页回"已保存 1 项配置"、NVS 里也真写进去了，但屏幕纹丝不动。
//   同步电脑时间也一样：set_epoch 进了 NVS、epochCache 也更新了，唯独
//   settimeofday() 没人调 —— 键盘上的钟还是旧时间。
//   "改个亮度键盘自己跳设置页"当时看着是修好了，其实是这个 bug 顺手把
//   **所有**副作用都掐了；副作用一恢复，这个判据就只能读第二遍的结果。
//
// 顺序有讲究：
//   CLOCK  改系统时间，无 UI 依赖，最先
//   DISP   重建主屏（resetStylePointers 会把顶部条指针清成 nullptr 再由
//          dashTopBar 重建），所以必须在 PROF 之前
//   PROF   跟着重建完的顶部条把方案号/图标对上
//   SAVER  最后：屏保模式可能要 destroyMainScreen()，放最后才不会白重建
uint8_t pendMask = 0;
for (int i = 0; i < nPend; i++) {
    const CfgField* f = pend[i].f;
    if (!pend[i].changed) continue;   // 没变 → 不挂副作用（值本来就是它）
    switch (f->post) {
        case CFG_POST_CLOCK: pendMask |= CFG_PEND_CLOCK; break;
        case CFG_POST_DISP:  pendMask |= CFG_PEND_DISP;  break;
        case CFG_POST_SAVER: pendMask |= CFG_PEND_SAVER; break;
        case CFG_POST_NONE:
        default: break;
    }
}
cfgPend |= pendMask;   // 或运算：上一次还没做完的不要被覆盖掉
if (pendMask == 0 && nChanged == 0)
    ELINFO("CFG", "本次没有任何值发生变化,未落盘也未重建界面");

return nChanged + nScheme;
}

// loop() 里跑：cfgApplyDoc 置起来的界面副作用，**一轮只做一件**。
//
// 这才是"保存配置不再重启"的真正原因 —— 一次全屏重建独占一轮 loop，
// 前后都有键盘扫描和 LVGL 刷新，主循环始终在动。
// 一份配置最多摊 4 轮，肉眼看不出延迟。
// 映射落盘失败 → 把 HUD 正文收好、挂一个位。真正的 HUD 由 cfgPostPump 下一轮 loop
// 来弹（定义必须放在这里，见前面那个前置声明处的说明）。
static void cfgQueueRemapHud(const char* msg) {
    if (msg == nullptr) return;
    strncpy(cfgRemapHudMsg, msg, sizeof(cfgRemapHudMsg) - 1);
    cfgRemapHudMsg[sizeof(cfgRemapHudMsg) - 1] = '\0';
    cfgPend |= CFG_PEND_RMHUD;
}

static void cfgPostPump(void) {
    if (cfgPend == 0) return;
    btLinkKeepAlive();          // 动手之前先喂一次
    ct_mark(CT_S_CFG_POST);      // 万一还是崩了，RTC 慢存会记下这个阶段

    if (cfgPend & CFG_PEND_CLOCK) {
        cfgPend &= ~CFG_PEND_CLOCK;
        time_t t = (time_t)epochCache;
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ELINFO("CFG", "时间已设为 %lu", (unsigned long)epochCache);
    }
    else if (cfgPend & CFG_PEND_DISP) {
        cfgPend &= ~CFG_PEND_DISP;
        unsigned long t0 = millis();
        applyDispMode(currentDispMode);
        ELINFO("CFG", "显示风格重建用了 %lums", (unsigned long)(millis() - t0));
    }
    else if (cfgPend & CFG_PEND_SAVER) {
        cfgPend &= ~CFG_PEND_SAVER;
        unsigned long t0 = millis();
        setScreensaverMode(saverMode);
        ELINFO("CFG", "屏保模式重建用了 %lums", (unsigned long)(millis() - t0));
    }
    else if (cfgPend & CFG_PEND_RMHUD) {
        cfgPend &= ~CFG_PEND_RMHUD;
        // 放在最后：前三个是"用户改了什么"的反馈，这条是"有东西没存住"的告警，
        // 优先级最低，但不能丢 —— 它是用户唯一能知道"这次保存其实没生效"的途径。
        triggerHudBig("按键重映射", cfgRemapHudMsg, lv_color_hex(CLR_RED));
    }
    btLinkKeepAlive();          // 做完再喂一次
}

// 每轮 loop 把 cfgTxBuf 里还没喂完的一段推进 bt_link。和 logDumpPump 一个套路：
// 缓冲一满就立刻把 CPU 还回去，于是整段 JSON 自动摊到几轮 loop 上，
// 不存在"一条大 JSON 把主循环按住"的风险。
static void cfgJsonPump(void) {
    if (!cfgTxActive) return;
    btLinkKeepAlive();

    // 链断了 / 对端没了：收摊，别一直挂着占住流式单槽
    if (!btLinkStreamBusy()) { cfgTxActive = false; return; }
    if (cfgTxPos >= cfgTxLen) {
        btLinkStreamEnd();          // 剩下的由 bt_link 发完并补收尾标记
        cfgTxActive = false;
        return;
    }
    size_t room = btLinkStreamRoom();
    if (room == 0) return;          // 单槽满了，等下一轮
    size_t n = cfgTxLen - cfgTxPos;
    if (n > room) n = room;         // ⚠ 必须封顶，且只推进真正写进去的 n
    btLinkStreamWrite(cfgTxBuf + cfgTxPos, n);
    cfgTxPos += n;
}

// 设成指定的显示风格（落盘 + 整屏重建）。和 setScreensaverMode 一样是从
// "循环切换"里抽出来的，好让配置 JSON 能直接设成任意一档。
static void applyDispMode(uint8_t mode) {
    if (mode >= TOTAL_DISP_MODES) return;
    currentDispMode = mode;
    preferences.putUChar("disp_mode", currentDispMode);
    renderCurrentDisplayBase();
    // 主机切风格时用户可能正停在菜单/设置界面，别把他踢出当前界面
    if (currentSysMode == SYS_MODE_NORMAL) showScreen(ensureMainScreen());
}

// 开一段配置接收。isPatch 区分"整份"和"增量"两种后续。
//
// 这两个入口（CFGBEGIN / CFGPATCH）的收包流程**完全一样** —— 同一个缓冲、
// 同一套长度校验、同一片 5 秒超时，唯一区别是收尾时走 cfgApplyDoc 的哪个
// 模式。所以合并成一个函数，免得以后修校验漏掉其中一条路。
static bool cfgRxBegin(const String& s, bool isPatch) {
    uint32_t total = (uint32_t)s.toInt();
    if (!cfgBufEnsure()) {
        btLinkReplyC("CFGSAVE", "ERR:固件内存不够");
        cfgRxActive = false;
        return false;
    }
    if (total == 0 || total > CFG_BUF_SIZE) {
        ELWARN("CFG", "拒绝一段 %u 字节的配置（上限 %d）",
               (unsigned)total, (int)CFG_BUF_SIZE);
        btLinkReplyC("CFGSAVE", "ERR:长度不合法");
        cfgRxActive = false;
        return false;
    }
    cfgRxPatch  = isPatch;
    cfgRxTotal  = total;
    cfgRxLen    = 0;
    cfgRxActive = true;
    cfgRxLastMs = millis();
    return true;
}

static void handleCommand(const String& cmd) {
    // DISP_MODE:n - Switch display mode
    if (cmd.startsWith("DISP_MODE:")) {
        applyDispMode((uint8_t)cmd.substring(10).toInt());
    }
    // LOCK:0 | LOCK:1 - 锁定 / 解锁键盘
    //
    // 这条是给网页和 PC 端留的入口。键盘上双击静音键能锁、Fn+LOGO 能解锁，
    // 但"人不在键盘旁边"的时候总得有个远程的办法（比如键盘被猫按住了）。
    // 回一条 LOCKSTATE，网页那边可以直接读当前状态。
    else if (cmd.startsWith("LOCK:")) {
        int v = cmd.substring(5).toInt();
        lockSetLocked(v != 0);
        btLinkReplyf("LOCKSTATE", "%d", kbLocked ? 1 : 0);
    }
    // TIME:epoch - Sync time
    else if (cmd.startsWith("TIME:")) {
        time_t t = cmd.substring(5).toInt();
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        preferences.putUInt("set_epoch", (uint32_t)t);
        epochCache = (uint32_t)t;   // 内存副本跟着走，否则 CFGGET 拉出来的还是旧时间

        // ⚠ 以前这里**一声不吭**就结束了 —— 设没设上、设的是不是个离谱的
        //   值，主机这边完全不知道。手机端（ios/MESender）就靠这条回话
        //   确认"键盘真的收下了这个时间"，没有它就只能干等。
        //   回话带上**键盘自己认下来的那个 epoch**，主机对一下就知道
        //   固件那边是不是被截断/改写过。
        char tbuf[24];
        struct tm tmr;
        localtime_r(&t, &tmr);
        strftime(tbuf, sizeof(tbuf), "%H:%M", &tmr);
        btLinkReplyf("TIME", "OK:%lu:%s", (unsigned long)t, tbuf);
        triggerHud("时间已同步", tbuf, lv_color_hex(CLR_GREEN));
    }
    // ALARMSET:HH:MM | ALARMSET:OFF - Set / clear alarm
    else if (cmd.startsWith("ALARMSET:")) {
        String timeStr = cmd.substring(9);
        timeStr.trim();
        String up = timeStr;
        up.toUpperCase();

        // OFF / 0 关闹钟。kbctl.py 和 kbctl_hid.py 关闹钟发的是 `ALARMSET:OFF`，
        // 以前这里只认带冒号的写法，"OFF" 里没有 ':'，于是 indexOf 返回 -1，
        // 整个分支被静默跳过 —— 网页/Pc 端点了"关闭闹钟"，键盘上其实还开着，
        // 到点照样响。关不掉比开不了更气人，因为它没有任何报错。
        if (up == "OFF" || up == "0" || up == "DISABLE" || up == "CANCEL") {
            alarmEnabled = false;
            preferences.putBool("alarm_on", false);
            triggerHud("闹钟", "已关闭", lv_color_hex(CLR_TEXT_DIM));
            return;
        }

        int colonIdx = timeStr.indexOf(':');
        if (colonIdx > 0) {
            int h = timeStr.substring(0, colonIdx).toInt();
            int m = timeStr.substring(colonIdx + 1).toInt();
            if (h < 0 || h > 23 || m < 0 || m > 59) {
                // 越界值以前是直接截断存进 uint8_t 的，网页上填个 99:99
                // 就会存成 99:99，第二天永远等不到那一声。存之前先拒掉并说清楚。
                triggerHudBig("闹钟", "时间超出范围", lv_color_hex(CLR_RED));
                return;
            }
            alarmHour = (uint8_t)h;
            alarmMinute = (uint8_t)m;
            alarmEnabled = true;
            preferences.putUChar("alarm_h", alarmHour);
            preferences.putUChar("alarm_m", alarmMinute);
            preferences.putBool("alarm_on", alarmEnabled);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", alarmHour, alarmMinute);
            triggerHud("闹钟", buf, lv_color_hex(CLR_GREEN));
        }
    }
    // TIMERSET:<秒数> | TIMERSET:MM:SS | TIMERSET:HH:MM:SS | TIMERSET:STOP
    //
    // 三种写法都收，纯秒数排第一，因为**所有**远端客户端实际发的就是它：
    //   s3-setting.html:  `TIMERSET:` + (h*3600 + m*60 + s)   → "TIMERSET:300"
    //   kbctl.py:         "TIMERSET:%d" % total              → "TIMERSET:300"
    //   kbctl_hid.py:     同上
    // 而这里原来只认 HH:MM:SS（要求两个冒号），"300" 走不进来，
    // 整个分支被静默跳过 —— 网页点"启动"只弹了个 alert，键盘上什么也没发生。
    // 老版 s3/s3.ino 是三种都收的，移植到 LVGL 时这段容错被漏掉了。
    else if (cmd.startsWith("TIMERSET:")) {
        String v = cmd.substring(9);
        v.trim();
        String vUp = v;
        vUp.toUpperCase();

        if (vUp == "STOP" || vUp == "OFF" || vUp == "CANCEL") {
            stopCountdown(false);
        } else {
            int p1 = v.indexOf(':');
            uint32_t total = 0;
            if (p1 < 0) {
                total = (uint32_t)v.toInt();
            } else {
                int p2 = v.indexOf(':', p1 + 1);
                if (p2 < 0) {
                    total = (uint32_t)v.substring(0, p1).toInt() * 60UL
                          + (uint32_t)v.substring(p1 + 1).toInt();
                } else {
                    total = (uint32_t)v.substring(0, p1).toInt() * 3600UL
                          + (uint32_t)v.substring(p1 + 1, p2).toInt() * 60UL
                          + (uint32_t)v.substring(p2 + 1).toInt();
                }
            }
            if (total == 0) {
                // 时长非法/误码。原来这里什么都不做，用户看到的就是"按了没反应"
                triggerHudBig("倒计时", "时长不合法", lv_color_hex(CLR_RED));
                return;
            }
            startCountdown(total);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                     timerEditH, timerEditM, timerEditS);
            triggerHud("倒计时", buf, lv_color_hex(CLR_ACCENT));
        }
    }
    // MARQUEE:text - Set marquee text
    else if (cmd.startsWith("MARQUEE:")) {
        String text = cmd.substring(8);
        // 存储标语文本用于显示
        static char marqueeText[256] = {0};
        strncpy(marqueeText, text.c_str(), sizeof(marqueeText) - 1);
        marqueeText[sizeof(marqueeText) - 1] = '\0';
        // 可以在此添加标语显示逻辑
    }
    // REMAP:prof:clear:rules - Key remapping
    //
    // 第三个字段历史上两边对不上，是"映射清不掉"的全部原因：
    //   网页 s3-setting.html 发的是 `REMAP:0:1:...`（那个 1 是 1.x 遗留的布尔标记）
    //   固件这里只认 `clear`
    // 结果：保存时 clearCmd=="1" 走不进清空分支，规则被**追加**到旧规则后面，
    // 点"清空此方案"也是发 1，同样什么都不做 —— 界面上却已经 alert"已清空！"。
    // 现在两边都收：clear / 1 / 0 / CLEAR / reset 都当清空。
    //
    // 另外把语义定死成"**整份替换**"：只要带了 rules，就先把这个方案的条数归零
    // 再逐条写入。以前是纯追加，存 3 条再存 1 条就变成 4 条，旧的永远留在里面。
    else if (cmd.startsWith("REMAP:")) {
        String params = cmd.substring(6);
        int firstColon = params.indexOf(':');
        int secondColon = params.indexOf(':', firstColon + 1);
        if (firstColon > 0 && secondColon > firstColon) {
            int prof = params.substring(0, firstColon).toInt();
            String clearCmd = params.substring(firstColon + 1, secondColon);
            String rules = params.substring(secondColon + 1);
            clearCmd.toUpperCase();
            bool wantClear = (clearCmd == "CLEAR" || clearCmd == "RESET" ||
                              clearCmd == "1"  || clearCmd == "0");

            if (prof >= 0 && prof < TOTAL_PROFILES) {
                if (rules.length() > 0) {
                    // 整份替换。先把规则解析进一个临时表，再交给 cfgStoreRemap
                    // 一次做完"删旧 → 写新 → 回读校验"。
                    //
                    // ⚠ 为什么不再就地写：配置 JSON 的 remap 块也调同一个函数。
                    //   两条路必须落出**完全一样**的 NVS 布局，否则用户在 JSON
                    //   里改一遍、再用老协议存一遍，两边会互相覆盖出不同结果，
                    //   而现场没有任何异常。
                    RemapRule tmp[MAX_REMAP_RULES];
                    int n = 0;
                    int start = 0;
                    while (start < rules.length() && n < MAX_REMAP_RULES) {
                        int semicolon = rules.indexOf(';', start);
                        String rule = (semicolon >= 0) ? rules.substring(start, semicolon)
                                                       : rules.substring(start);
                        int comma = rule.indexOf(',');
                        if (comma > 0) {
                            tmp[n].fromKey = normalizeRemapKey((uint16_t)rule.substring(0, comma).toInt());
                            tmp[n].toKey   = normalizeRemapKey((uint16_t)rule.substring(comma + 1).toInt());
                            n++;
                        }
                        // semicolon == 0 也当作分隔符：规则串以 ';' 开头时
                        // indexOf 返回 0，而旧代码的 `semicolon > 0` 会把它当"没有分号"，
                        // 于是把 ";224,227;227,224" 整段当成一条去 parse。
                        start = (semicolon >= 0) ? semicolon + 1 : rules.length();
                    }
                    int verified = cfgStoreRemap(prof, tmp, n);
                    char buf[32];
                    if (verified == n) {
                        snprintf(buf, sizeof(buf), "%d 条已保存", verified);
                        triggerHud("按键重映射", buf, lv_color_hex(CLR_GREEN));
                    }
                    // 存不全时 cfgStoreRemap 内部已经弹了红字"只存住 x/y 条"，
                    // 这里不再弹第二次绿的 —— 两种 HUD 连着弹只会让人以为存好了。
                } else if (wantClear) {
                    // 只清不写：清掉这个方案的全部规则。cfgStoreRemap(prof,0,0)
                    // 就是"删 MAX_REMAP_RULES 个键 + 条数归零"，和写入路径
                    // 用的是同一段代码，NVS 布局自然一致。
                    cfgStoreRemap(prof, nullptr, 0);
                    LOG_PORT.printf("[REMAP] prof=%d cleared\n", prof);
                    // ⚠ 清空分支以前只打 UART，不进错误日志 —— 于是"映射莫名其妙
                    //   变 0 条"这种事在日志里查无实据。固件侧任何会让某个方案
                    //   变空的路径都必须留痕，否则永远分不清是"没存进去"还是
                    //   "被清掉了"。
                    ELWARN("REMAP", "方案%d 已清空", prof + 1);
                    triggerHud("按键重映射", "已清空", lv_color_hex(CLR_AMBER));
                }
            }
        }
    }
    // REMAPREAD:p - 把方案 p 现有的规则回读给网页
    // （回读只能靠蓝牙 notify，正文超 20 字节时分片发 —— 分片由 bt_link 做）
    else if (cmd.startsWith("REMAPREAD:")) {
        int prof = cmd.substring(9).toInt();
        // 这里只拼**正文**（"128,131;131,128"），头部、分片、收尾标记全交给 bt_link。
        //
        // 为什么必须分片：单条通知能带多少字节 = 协商后的 ATT MTU - 3，协商失败时
        // （手机端浏览器很常见）只有 23-3 = 20 字节，超出的**不是被截断而是整条
        // 通知被协议栈丢掉**。2 条规则整行 28 字节 → 整条丢 → 网页只等来一个超时
        // → 弹「没能从键盘读回确认」、列表刷新成 0 条。而 1 条规则刚好 20 字节能过，
        // 这个不对称一直没人认出来（"单条 win→ctrl 读得回来、Ctrl/Win 交换读不回来"）。
        char body[BT_TX_BODY_MAX];
        body[0] = '\0';
        int n = 0;
        if (prof >= 0 && prof < TOTAL_PROFILES) {
            for (int i = 0; i < remapCounts[prof] && n < (int)sizeof(body) - 16; i++) {
                n += snprintf(body + n, sizeof(body) - n, "%s%u,%u",
                               i ? ";" : "",
                               profileRemaps[prof][i].fromKey, profileRemaps[prof][i].toKey);
            }
            body[n] = '\0';
        }
        // prof 越界时 body 保持空：网页只会收到一条空正文，语义是"读不到"。
        btLinkReplyC("REMAPDUMP", body);
        // ⚠ 这里原来打的是 `[REMAP] read prof=%d -> %s`，把整条 out 原样灌进 LOG_PORT。
        //   看着像"多打一条调试信息"，实际有两个害处：
        //   1) LOG_PORT 会给每行加 "LOG:" 前缀再 notify，于是同一次读取会发出
        //      **两条内容几乎相同**的 notify。BLE 通知不排队，客户端没 ACK 时
        //      后一条会被合并丢弃 —— 网页两个并发的读回 waiter 里必有一个超时。
        //   2) 网页 onBleNotify 里 `LOG:` 分支优先级高于前缀匹配（那是故意的，
        //      免得 LOG 行截胡日志面板自己的 waiter），所以这条"兜底"行
        //      **永远匹配不上** REMAPDUMP 前缀，看着像双保险实际是零保险，
        //      还把整张映射表刷进网页日志面板。
        //   只记条数、正文字节数和分片数 —— 分片数是要紧的，它是"分片协议有没有
        //   被改坏"最直接的判据：MTU 协商失败时（手机上 ATT 载荷只有 20）一条规则
        //   1 片、两条 2 片，对不上就说明分片这块出问题了。
        const size_t bodyLen = strlen(body);
        // ⚠ 分片数必须按**当前**预算算。MTU 协商成功后每片能带两百多字节，
        //   还按 20 字节的旧账本算的话，报出来的片数是实际的十几倍 ——
        //   看着就像分片坏了，其实只是账本没跟上（这个坑在改 btLinkPayloadMax
        //   的时候差点真踩进去）。
        const size_t perFrag = btLinkPayloadMax() - (sizeof("REMAPDUMP:0:") - 1) - 1;   // 头 + 终止符
        if (perFrag == 0) {   // 预算连头都装不下，emitChunk 本来也发不出去
            LOG_PORT.printf("[REMAP] read prof=%d 预算装不下协议头\n", prof);
            return;
        }
        const size_t frags = bodyLen ? (bodyLen + perFrag - 1) / perFrag : 0;
        LOG_PORT.printf("[REMAP] read prof=%d cnt=%d bytes=%u frags=%u\n", prof,
                        (prof >= 0 && prof < TOTAL_PROFILES) ? remapCounts[prof] : 0,
                        (unsigned)bodyLen, (unsigned)(frags + 1));
        ELINFO("REMAP", "方案%d 回读 %d 条 共%u字节", prof + 1,
               (prof >= 0 && prof < TOTAL_PROFILES) ? remapCounts[prof] : 0,
               (unsigned)bodyLen);
        // 被读的方案和当前生效的方案不是同一个时单独喊一句：网页下拉框选的方案
        // 和键盘自己跑的方案本来就互相独立，存对了地方但按键不变时最容易混过去。
        if (prof >= 0 && prof < TOTAL_PROFILES && prof != (int)currentProfile) {
            ELWARN("REMAP", "读的是方案%d 键盘当前是方案%u", prof + 1,
                   (unsigned)(currentProfile + 1));
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "读取到 %d 条", (prof >= 0 && prof < TOTAL_PROFILES) ? remapCounts[prof] : 0);
        triggerHud("按键重映射", buf, lv_color_hex(CLR_ACCENT));
    }
    // MACRODUMP:p{N}_{MKey} - 把方案 N 的 MKey 宏回读给网页
    // 响应 key = 宏名（"p0_M1"），正文 = 宏体。
    //   `MACRODUMP:p0_M1:SEQ:abc...` / `MACRODUMP:p0_M1:CMB:128,4` /
    //   `MACRODUMP:p0_M1:NONE`（未设置）
    //
    // 以前这里是**自己拼一整行再 notify**，而一行 30 字节就够让手机端浏览器
    // 把整条通知丢掉（ATT 载荷上限 20 字节）。也就是说"M1 有宏"读不回来、
    // "M1 是空的"反而读得回来 —— 和 REMAPDUMP 那个 1 条/2 条不对称同一个病。
    // 现在只发正文，分片和收尾标记交给 bt_link。
    // 正文上限 = BT_TX_BODY_MAX，超了截断并标 ...TRUNC（网页要能提示"读回不完整"）。
    else if (cmd.startsWith("MACRODUMP:")) {
        String key = cmd.substring(10);
        String val = preferences.getString(key.c_str(), "");
        bool truncated = false;
        if (val.length() > BT_TX_BODY_MAX - 10) {
            truncated = true;
            val = val.substring(0, BT_TX_BODY_MAX - 10);
        }
        if (val.length() == 0) val = "NONE";   // 显式 NONE，让网页知道"该键无宏"
        else if (truncated) val += "...TRUNC";
        btLinkReplyC("MACRODUMP", val.c_str());
        LOG_PORT.printf("[MACRODUMP] %s len=%u truncated=%d\n",
                        key.c_str(), (unsigned)val.length(), truncated ? 1 : 0);
    }
    // GKEYDUMP:KEY[:state] - 把全局键配置回读给网页
    // 老用法 `GKEYDUMP:MA`           → 读 g_MA    (单套全局动作,保留兼容)
    // 新用法 `GKEYDUMP:MA:a`         → 读 g_MAa   (状态 A, 双状态切换)
    //       `GKEYDUMP:MA:b`          → 读 g_MAb   (状态 B)
    // 协议:`GKEYDUMP:MA:SW:1+CMB:128,4` / `GKEYDUMP:MA:a:NONE`
    // 响应 key 用 '_' 代替请求里的 ':'（"MA:a" → "MA_a"）：协议的 key 是
    // "VERB:KEY:<正文>" 三段切分，正文里本来就带冒号（"SW:1+CMB:…"），
    // key 里再带冒号就切不开了。
    else if (cmd.startsWith("GKEYDUMP:")) {
        String rest = cmd.substring(9);              // "MA" / "MA:a" / "MA:b"
        String val;
        String dumpTag = rest;
        // 双状态格式必须是 "<KEY>:<state>",即 length==4、第 2 个字符是 ':'、
        // 第 3 个字符是 'a' 或 'b'。KEY 这边只认 MA / MB(2 字符)。
        // 用 length==4 而不是 >=3 是为了避免把 "MA:" 这种半截命令误识别。
        if (rest.length() == 4 && rest[2] == ':' &&
            (rest[0] == 'M') && (rest[1] == 'A' || rest[1] == 'B') &&
            (rest[3] == 'a' || rest[3] == 'b')) {
            // 双状态格式:KEY:<a|b> 取 g_<KEY><state>
            String key   = rest.substring(0, 2);
            String state = rest.substring(3);
            String gKey  = "g_" + key + state;
            val = preferences.getString(gKey.c_str(), "");
        } else {
            // 兼容老格式:KEY 取 g_<KEY>(M1-M12 也走这条)
            String gKey = "g_" + rest;
            val = preferences.getString(gKey.c_str(), "");
        }
        bool truncated = false;
        if (val.length() > BT_TX_BODY_MAX - 10) {
            truncated = true;
            val = val.substring(0, BT_TX_BODY_MAX - 10);
        }
        if (val.length() == 0) val = "NONE";
        else if (truncated) val += "...TRUNC";
        btLinkReplyC("GKEYDUMP", val.c_str());
        LOG_PORT.printf("[GKEYDUMP] %s len=%u truncated=%d\n",
                        dumpTag.c_str(), (unsigned)val.length(), truncated ? 1 : 0);
    }
    // GKEYPHASE:KEY - 读 MA/MB 当前相位(下次按下将执行哪一套状态)
    // 响应:`GKEYPHASE:MA:<0|1>`。缺省 0(下次按 = 状态 A)。
    else if (cmd.startsWith("GKEYPHASE:")) {
        String key = cmd.substring(10);
        if (key != "MA" && key != "MB") {
            LOG_PORT.printf("[GKEYPHASE] unknown key '%s'\n", key.c_str());
        } else {
            char phKey[16];
            snprintf(phKey, sizeof(phKey), "g_%s_ph", key.c_str());
            uint8_t ph = preferences.getUChar(phKey, 0);
            if (ph > 1) ph = 0;   // 容错:被人手动写过 NVS 时强行纠正
            btLinkReplyf("GKEYPHASE", "%u", (unsigned)ph);
            LOG_PORT.printf("[GKEYPHASE] %s = %u\n", key.c_str(), (unsigned)ph);
        }
    }
    // GKEYPHASE_RESET:KEY - 把 MA/MB 相位重置回 0(下次按下回到状态 A)
    // 响应:`GKEYPHASE:KEY:0`,跟 GKEYPHASE 同格式方便前端共用解析。
    else if (cmd.startsWith("GKEYPHASE_RESET:")) {
        String key = cmd.substring(16);
        if (key != "MA" && key != "MB") {
            LOG_PORT.printf("[GKEYPHASE_RESET] unknown key '%s'\n", key.c_str());
        } else {
            char phKey[16];
            snprintf(phKey, sizeof(phKey), "g_%s_ph", key.c_str());
            preferences.putUChar(phKey, 0);
            btLinkReplyC("GKEYPHASE", "0");
            LOG_PORT.printf("[GKEYPHASE_RESET] %s -> 0\n", key.c_str());
            triggerHud("相位重置", key.c_str(), lv_color_hex(CLR_ACCENT));
        }
    }
    // GKEY_RESET:KEY - 只清掉某个全局键(MA/MB)的全部双状态 + 相位,不动 M1-M12。
    // 网页卡片里"清空该键"按钮用这个,避免误伤其他配置。
    // 与 MACROS_RESET(清全部宏+全部全局)的区别:GKEY_RESET 是单键级别。
    else if (cmd.startsWith("GKEY_RESET:")) {
        String key = cmd.substring(11);
        if (key != "MA" && key != "MB") {
            LOG_PORT.printf("[GKEY_RESET] unknown key '%s'\n", key.c_str());
        } else {
            int removed = 0;
            char k[16];
            // 三套:老单套 + 双状态 a/b + 相位
            snprintf(k, sizeof(k), "g_%s",   key.c_str()); if (preferences.remove(k)) removed++;
            snprintf(k, sizeof(k), "g_%sa",  key.c_str()); if (preferences.remove(k)) removed++;
            snprintf(k, sizeof(k), "g_%sb",  key.c_str()); if (preferences.remove(k)) removed++;
            snprintf(k, sizeof(k), "g_%s_ph",key.c_str()); if (preferences.remove(k)) removed++;
            LOG_PORT.printf("[GKEY_RESET] %s removed %d keys\n", key.c_str(), removed);
            char info[24];
            snprintf(info, sizeof(info), "%s / %d 项已清", key.c_str(), removed);
            triggerHud("全局键已清", info, lv_color_hex(CLR_GREEN));
        }
    }
    // SET:name:value - Macro definition
    //
    // 名字里已经带方案号时**不能再套一层**。网页发的是 `SET:p0_M1:SEQ:…`
    // （方案号由用户在下拉框里选，见 s3-setting.html 的 activeProfile），
    // 而这里以前无条件再拼一遍 currentProfile，于是存进去的键是 `p0_p0_M1`，
    // 而 executeMacro() 读的是 `p0_M1` —— 存的键和读的键永远对不上，
    // 表现就是"网页里保存的宏/序列，按键盘上的 M1-M12 毫无反应"。
    // 只有不带方案前缀的老写法（`SET:M1:…`）才补当前方案。
    else if (cmd.startsWith("SET:")) {
        String params = cmd.substring(4);
        int colonIdx = params.indexOf(':');
        if (colonIdx > 0) {
            String name = params.substring(0, colonIdx);
            String value = params.substring(colonIdx + 1);
            bool hasProfPrefix = (name.length() >= 3 && name[0] == 'p' &&
                                  name[1] >= '0' && name[1] <= '9' && name[2] == '_');
            String storeKey = name;
            if (!hasProfPrefix) {
                char pKey[32];
                snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, name.c_str());
                storeKey = pKey;
            }
            preferences.putString(storeKey.c_str(), value);
            char info[40];
            snprintf(info, sizeof(info), "%s / 方案%u", name.c_str(),
                     (unsigned)(currentProfile + 1));
            triggerHud("已写入宏", info, lv_color_hex(CLR_GREEN));
        }
    }
    // GSET:name[:state]:value - Global key assignment
    // 老用法 `GSET:MA:SW:1+CMB:...`  → 写 g_MA(单套全局动作,保留兼容)
    // 新用法 `GSET:MAa:SW:1+...`     → 写 g_MAa(状态 A)
    //       `GSET:MAa:NONE`          → 移除 g_MAa
    // 注意新用法的 name 段是 MAa / MAb / MBa / MBb(3 字符),老用法是 MA / MB(2 字符)。
    // 这里不解析 payload 语法,只做"按 name/state 落盘" —— 真正执行
    // 是 executeGlobalKey / executeToggleKey 的事,见那两处。
    else if (cmd.startsWith("GSET:")) {
        String params = cmd.substring(5);
        int colonIdx = params.indexOf(':');
        if (colonIdx > 0) {
            String name = params.substring(0, colonIdx);
            String value = params.substring(colonIdx + 1);
            // 判定是否带 state 后缀(新格式):name 必须是 3 字符,以 'M' 开头、
            // 第 2 字符是 'A' / 'B',第 3 字符是 'a' / 'b'。
            // 这样老格式 "MA" / "MB"(2 字符)不会误识别。
            bool hasState = (name.length() == 3) &&
                            (name[0] == 'M') &&
                            (name[1] == 'A' || name[1] == 'B') &&
                            (name[2] == 'a' || name[2] == 'b');
            char gKey[16];
            if (hasState) {
                // 新格式:name = "MAa" / "MAb" / "MBa" / "MBb" → g_MAa 等
                snprintf(gKey, sizeof(gKey), "g_%s", name.c_str());
                if (value == "NONE") {
                    preferences.remove(gKey);
                    triggerHud("已清全局键状态", name.c_str(), lv_color_hex(CLR_ACCENT));
                } else {
                    preferences.putString(gKey, value.c_str());
                    char info[24];
                    snprintf(info, sizeof(info), "%s", name.c_str());
                    triggerHud("已写入全局键状态", info, lv_color_hex(CLR_ACCENT));
                }
            } else {
                // 老格式:GSET:KEY:<payload> → g_<KEY>(单套全局动作,保留兼容)
                snprintf(gKey, sizeof(gKey), "g_%s", name.c_str());
                if (value == "NONE") {
                    preferences.remove(gKey);
                    triggerHud("已清全局键", name.c_str(), lv_color_hex(CLR_ACCENT));
                } else {
                    preferences.putString(gKey, value.c_str());
                    triggerHud("已写入全局键", name.c_str(), lv_color_hex(CLR_ACCENT));
                }
            }
        }
    }
    // MACROS_RESET: 清掉所有方案的 M1-M12 + 全局 MA/MB(含双状态切换)。
    // 保留 remap 规则 / 时钟 / 闹钟 / 灯光 / 壁纸 / SPIFFS 等其他 NVS 键，
    // 只删 p?\d_M?\d+\d 这种格式(方案专属宏)、g_MA / g_MB(老单套全局动作)、
    // g_MAa / g_MAb / g_MBa / g_MBb(双状态切换)、g_MA_ph / g_MB_ph(相位)。
    // ESP32 Preferences 没有按前缀删,只能逐 key remove。
    // 旧版错存的双层前缀 `p?\d_p?\d_M?\d`(见 SET: 那段的根因)也在范围里 —— 留着不删反而
    // 会让"清完后按 M1 仍然有反应"这种残留事件更难看,顺手清掉。
    else if (cmd == "MACROS_RESET") {
        int removed = 0;
        // 方案专属宏: 4 个方案 × 12 个 M 键
        for (int p = 0; p < 4; p++) {
            for (int m = 1; m <= 12; m++) {
                char pk[16];
                snprintf(pk, sizeof(pk), "p%d_M%d", p, m);
                if (preferences.remove(pk)) removed++;
                // 历史 bug: 部分键错存成 p?\d_p?\d_M?\d 形态
                snprintf(pk, sizeof(pk), "p%d_p%d_M%d", p, p, m);
                preferences.remove(pk);
            }
        }
        // 全局动作(老单套)
        if (preferences.remove("g_MA")) removed++;
        if (preferences.remove("g_MB")) removed++;
        // 双状态切换(状态 A / 状态 B / 相位)
        if (preferences.remove("g_MAa"))  removed++;
        if (preferences.remove("g_MAb"))  removed++;
        if (preferences.remove("g_MBa"))  removed++;
        if (preferences.remove("g_MBb"))  removed++;
        if (preferences.remove("g_MA_ph")) removed++;
        if (preferences.remove("g_MB_ph")) removed++;
        // M1~M12 各自挂的全局动作（g_M1~g_M12）。
        // ⚠ 以前这里漏了：网页按钮上写着"清空所有宏（M1~M12 × 2 个系统 + MA + MB）"，
        //   而 M1~M12 的全局动作留在 NVS 里没被清 —— 用户点了"全部清空"之后
        //   按 M1 还是会有反应，是那种"怎么又回来了"的幽灵事件。
        for (int m = 1; m <= 12; m++) {
            char gk[8];
            snprintf(gk, sizeof(gk), "g_M%d", m);
            if (preferences.remove(gk)) removed++;
        }
        LOG_PORT.printf("[MACROS_RESET] removed %d keys\n", removed);
        char info[24];
        snprintf(info, sizeof(info), "%d 项已清", removed);
        triggerHud("宏已全部清空", info, lv_color_hex(CLR_GREEN));
    }
    // REMAP_RESET: 清掉所有方案的按键映射 (REMAP)。
    // 跟 MACROS_RESET 一样保留灯光/时钟/壁纸/ME 文本,只删 rmp_cnt_<p> + rmp_<p>_<i>。
    // 主要给"网页里 ASCII 编码时代留下的脏规则"用:比如老 HTML 把 A 编成 97,
    // 按 A 走 remap 出来就是 97 → kbPress 落进 0x53~0x63 → pressRaw 发成 Num9。
    // 宏和 remap 是两套独立存储,所以 MACROS_RESET 不会碰这些。
    else if (cmd == "REMAP_RESET") {
        int removed = 0;
        for (int p = 0; p < 4; p++) {
            char key[16];
            snprintf(key, sizeof(key), "rmp_cnt_%d", p);
            if (preferences.remove(key)) removed++;
            for (int i = 0; i < MAX_REMAP_RULES; i++) {
                char itemKey[20];
                snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", p, i);
                if (preferences.remove(itemKey)) removed++;
            }
            remapCounts[p] = 0;
        }
        LOG_PORT.printf("[REMAP_RESET] removed %d keys\n", removed);
        char info[24];
        snprintf(info, sizeof(info), "%d 项已清", removed);
        triggerHud("映射已全部清空", info, lv_color_hex(CLR_GREEN));
    }
    // === BLE 日志通道控制 ===
    // 为什么单独拎出来:USB HID 模式下 UART0 基本看不到,调试被掐断,
    // 这里把 LOG_PORT.printf 的所有字节搬上 BLE 实时回传到网页日志面板。
    // 设计:
    //   LOG:on     - 打开实时流(新日志按行 push 进 btLinkStream,10Hz 节流)
    //   LOG:off    - 关掉流转发(缓冲仍然在落,只是不 notify 了)
    //   LOG:dump   - 把环形缓冲整段流式发出去(见 logDumpPump)
    //   LOG:clear  - 清空环形缓冲(不影响流转发开关)
    // 网页侧已经有一条通用的重组通道(见 BtLink),LOG / LOGDUMP 都在它上面,
    // 这里不用再关心"一片带多少字节""要不要补 END"这类事。
    else if (cmd == "LOG:on") {
        logStreamOn = true;
        btLinkSendC("LOG", "0", "STREAM_ON");
        LOG_PORT.println("[LOG] stream ON");
    }
    else if (cmd == "LOG:off") {
        logStreamOn = false;
        btLinkSendC("LOG", "0", "STREAM_OFF");
        LOG_PORT.println("[LOG] stream OFF");
    }
    else if (cmd == "LOG:clear") {
        logRingLen = 0;
        btLinkSendC("LOG", "0", "CLEARED");
        LOG_PORT.println("[LOG] buffer cleared");
    }
    #if LOG_CDC_MIRROR
    else if (cmd == "PANIC") {
        // 直接从 RTC 慢存把"上次 panic 的现场"回给主机。
        //
        // 为什么不能只靠开机日志：那段文字是开机时打进 BLE 日志环缓冲的，
        // 环只有 8KB，跑一会儿就被新日志顶掉了 —— 到时候"现场还在芯片里，
        // 但已经读不出来"。
        // 这个命令直接读慢存，**什么时候问都问得到**。
        if (!panicTraceHas()) {
            btLinkSendC("PANIC", "0", "NONE");
            LOG_PORT.println("[PANIC] 没有留存现场");
        } else {
            btLinkSendC("PANIC", "0", panicTraceText());
            LOG_PORT.printf("[PANIC] 回传现场 %u 字节\n", (unsigned)panicTraceLen());
        }
    }
    #endif
    else if (cmd == "LOG:dump") {
        // 这里**不做任何发送**，只是开一条流并记下要回拉多少字节。
        // 环形缓冲有 8KB，按 20 字节一片就是 400+ 片，一次性同步推完等于把
        // 主循环按住 8 秒 —— loop() 挂在任务看门狗上（10 秒、panic），
        // 期间键盘扫描和屏幕全停，用户看到的就是"一点日志就重启"。
        // 真正的发送在 loop() 的 logDumpPump() 里分摊进行。
        if (logRingBuf == nullptr || !btLinkReady()) {
            btLinkSendC("LOGDUMP", "0", "");   // 缓冲还没建好/没连上，也得回一条收尾
        } else if (logDumpActive) {
            LOG_PORT.println("[LOG] dump already running");
        } else if (logDumpSnap == nullptr) {
            ELWARN("LOG", "回拉快照没内存");
            btLinkSendC("LOGDUMP", "0", "");
        } else {
            // 冻结快照。**必须拿同一把锁** —— 蓝牙任务可能正在往环缓冲里写，
            // 不加锁就会拷到"写了一半"的缓冲，回拉出来的行是乱的。
            bool locked = false;
            if (s_logMutex != nullptr) {
                locked = (xSemaphoreTake(s_logMutex, pdMS_TO_TICKS(20)) == pdTRUE);
            }
            memcpy(logDumpSnap, logRingBuf, logRingLen);   // 冻结一份，见下
            if (locked) xSemaphoreGive(s_logMutex);
            logDumpLen = logRingLen;
            logDumpPos = 0;
            if (btLinkStreamBegin("LOGDUMP", "0", logDumpLen)) {
                logDumpActive = true;
                logDumpPump();     // 先推一轮，空缓冲也能立刻收到 END
            } else {
                // 上一轮回拉还在发（单槽没腾空），或者链路掉线了。
                // 无论如何要回一条收尾标记，否则网页会干等 15 秒。
                ELWARN("LOG", "回拉没开出去 上一轮可能还在发");
                btLinkReplyC("LOGDUMP", "");
            }
        }
    }
    // === BLE 配对记录（bond）管理 ===
    //
    // 为什么需要：
    //   主机（iOS/macOS 的 CoreBluetooth、Chrome）一旦和设备配过对，就会记下
    //   长期密钥（LTK）。如果设备这边把配对记录弄丢了（刷固件、NVS 被清、
    //   协议栈自己删掉），主机下次连接时会用旧密钥来配对，设备认不出来，
    //   于是**主机侧报 CBError Code=14 "Peer removed pairing information"**，
    //   连接在加密协商阶段就失败。
    //
    //   现场表现：手机能连、网页连不上；或者网页第一次连上之后就再也连不上
    //   （因为每次失败都会断链，而断链又会触发"广播不重启"那个老 bug）。
    //
    //   IDF 有现成的接口：esp_ble_get_bond_device_num/list、
    //   esp_ble_remove_bond_device()。Arduino 的 BLE 封装**没有**暴露，
    //   这里直接调 IDF 的。
    //
    //   BOND:list → 列出配对记录（地址 + 类型）
    //   BOND:clr  → 全部删掉，让主机下次按"新设备"重新配对
    else if (cmd == "BOND:list") {
        char buf[160];
        btBondList(buf, sizeof(buf));
        btLinkSendC("BOND", "0", buf);
        LOG_PORT.printf("[BOND] list: %s\n", buf);
    }
    else if (cmd == "BOND:clr") {
        int removed = btBondClearAll();
        char buf[48];
        snprintf(buf, sizeof(buf), "REMOVED:%d LEFT:%d",
                 removed, esp_ble_get_bond_device_num());
        btLinkSendC("BOND", "0", buf);
        LOG_PORT.printf("[BOND] cleared: %s\n", buf);
    }
    // ---------------- 配置 JSON：拉取 ----------------
    else if (cmd == "CFGGET") {
        // 和 LOG:dump 同一个套路：这里只开一条流并记下要发多少字节，
        // 真正的发送在 loop() 的 cfgJsonPump() 里分摊，绝不同步推完。
        if (!btLinkReady()) {
            btLinkReplyC("CFGERR", "蓝牙没连上");
        } else if (cfgTxActive) {
            btLinkReplyC("CFGERR", "上一次还没发完，稍后重试");
        } else if (!cfgBufEnsure()) {
            btLinkReplyC("CFGERR", "固件内存不够，发不出配置");
        } else {
            cfgBuildErr[0] = '\0';
            cfgTxLen = cfgBuildJson(cfgTxBuf, CFG_BUF_SIZE);
            if (cfgTxLen == 0) {
                // 缓冲不够 / 文档装不下 / 生成出来混进了换行。必须明确报错，
                // 不能发半份 JSON 出去 —— 见 cfgBuildJson 末尾 overflowed 那段。
                // 原因原样回给网页，别让用户对着"生成失败"猜是哪一种。
                if (cfgBuildErr[0] == '\0')
                    snprintf(cfgBuildErr, sizeof(cfgBuildErr), "生成 JSON 失败");
                ELWARN("CFG", "%s（缓冲上限 %d 字节）", cfgBuildErr, (int)CFG_BUF_SIZE);
                btLinkReplyC("CFGERR", cfgBuildErr);
            } else {
                cfgTxPos = 0;
                // btLinkReplyStreamBegin 的 key 自动取本条请求的请求号（#123），
                // 所以网页那边靠 (CFGDUMP, 请求号) 配对，不用猜。
                if (btLinkReplyStreamBegin("CFGDUMP", cfgTxLen)) {
                    cfgTxActive = true;
                    cfgJsonPump();      // 先推一轮；空缓冲也能立刻收到 END
                } else {
                    // 流式单槽被上一次回拉占着（同一时刻只允许一条流）。
                    // 这时候**不能**回 CFGDUMP 的收尾标记 —— 主机那边正在等
                    // CFGDUMP 的正文，混进一条空消息会让它以为"配置是空的"。
                    ELWARN("CFG", "开不出流 上一轮回拉可能还在发");
                    btLinkReplyC("CFGERR", "链路忙（上一轮还没发完），稍后重试");
                }
            }
        }
    }
    // ---------------- 配置 JSON：保存 ----------------
    else if (cmd.startsWith("CFGBEGIN:") || cmd.startsWith("CFGPATCH:")) {
        // 两条路只有收尾时调的模式不同，攒字节的部分共用 cfgRxBegin。
        // ⚠ "CFGBEGIN:" 和 "CFGPATCH:" 都正好 9 个字符，substring(9) 才是
        //   长度后面那一位 —— 写错一位会从 ':' 后面再切掉一个字符，
        //   toInt() 永远得 0，然后固件回一句莫名其妙的"长度不合法"。
        bool patch = cmd.startsWith("CFGPATCH:");
        cfgRxBegin(cmd.substring(9), patch);
    }
    else if (cmd.startsWith("CFGDATA:")) {
        ct_mark(CT_S_CFG_RX);        // 崩在这一段 = 攒包,和"落盘"是两回事
        String chunk = cmd.substring(8);
        if (!cfgRxActive) {
            // CFGBEGIN/CFGPATCH 丢了（老网页直接发 DATA，或那一包被协议栈吞了）。
            // 静默丢掉整段会变成"网页显示保存成功、键盘没变"，比报错难查得多。
            ELWARN("CFG", "CFGDATA 没有 CFGBEGIN，丢弃");
            btLinkReplyC("CFGSAVE", "ERR:缺少 CFGBEGIN");
            return;
        }
        cfgRxLastMs = millis();
        if (cfgRxLen + chunk.length() > cfgRxTotal) {
            // 比声明的还长 = 网页那边的切片和总长对不上，JSON 已经被污染
            ELWARN("CFG", "收到的字节超过声明的总长，丢弃");
            btLinkReplyC("CFGSAVE", "ERR:数据超长");
            cfgRxActive = false;
            return;
        }
        memcpy(cfgRxBuf + cfgRxLen, chunk.c_str(), chunk.length());
        cfgRxLen += chunk.length();
    }
    else if (cmd == "CFGEND" || cmd == "CFGPATCHEND") {
        if (!cfgRxActive) {
            btLinkReplyC("CFGSAVE", "ERR:没有在收");
            return;
        }
        cfgRxActive = false;
        // 以**收尾这条指令**说的模式为准，而不是开段时记的那个：网页要是把
        // CFGBEGIN 配 CFGPATCHEND 凑到一起，那一定是它自己串错了，按它要的
        // 那个理解比反过来更不容易出乎意料。
        bool isPatch = (cmd == "CFGPATCHEND");
        if (isPatch != cfgRxPatch)
            ELWARN("CFG", "开段说的是%s、收尾说的是%s（网页串错了），按收尾的算",
                   cfgRxPatch ? "增量" : "整份", isPatch ? "增量" : "整份");
        if (cfgRxLen != cfgRxTotal) {
            // 少收了几片。**不能**拿手上这段去解析：JSON 被截断后，
            // deserializeJson 有可能把最后那个没闭合的对象整个丢掉还"成功"，
            // 于是 cfgApplyDoc 会拿一个字段都不剩的文档来"应用"。
            ELWARN("CFG", "没收齐：声明 %u 实收 %u，丢弃",
                   (unsigned)cfgRxTotal, (unsigned)cfgRxLen);
            btLinkReplyC("CFGSAVE", "ERR:没收齐，可能丢片");
            return;
        }
        cfgRxBuf[cfgRxLen] = '\0';
        char err[96];
        err[0] = '\0';
        unsigned long t0 = millis();
        int n = cfgApplyDoc(cfgRxBuf, cfgRxLen, isPatch, err, sizeof(err));
        unsigned long spent = millis() - t0;
        // 存不进去的项名（cfgApplyDoc 里回读校验挑出来的）
        char failed[160] = {0};
        if (n < 0) {
            ELWARN("CFG", "应用失败：%s", err);
            btLinkReplyf("CFGSAVE", "ERR:%s", err);
        } else if (nNvsFail > 0 && cfgFailNames[0]) {
            // ⚠ 这一条**不是** ERR：值已经写进 RAM 并提交了，键盘当场就是新的，
            //   但有 n 项**没存进 flash**，断电会丢。如实告诉网页，别让它显示
            //   一句"已保存 N 项"把人骗过去 —— 页面必须把这件事说清楚。
            //   第四段带的是失败项名，网页会点名显示。
            snprintf(failed, sizeof(failed), "%s", cfgFailNames);
            ELWARN("CFG", "已应用 %d 项，但 %d 项没存进 flash：%s",
                   n, nNvsFail, failed);
            btLinkReplyf("CFGSAVE", "OK:%d:%lu:PARTIAL:%d:%s", n, (unsigned long)spent,
                         nNvsFail, failed);
            char info[64];
            snprintf(info, sizeof(info), "已保存 %d 项，但 %d 项没存住",
                     n, nNvsFail);
            triggerHud("配置 JSON", info, lv_color_hex(CLR_AMBER));
        } else {
            // 此刻值已经**提交进 flash**、可以断电了（见 cfgApplyDoc 里的
            // preferences.end()）。界面重建还挂在 cfgPend 上，接下来几轮 loop
            // 一件一件做 —— 所以这里说的是"已存盘"，屏幕可能晚几百毫秒才对上。
            btLinkReplyf("CFGSAVE", "OK:%d:%lu", n, (unsigned long)spent);
            char info[48];
            snprintf(info, sizeof(info), "已保存 %d 项设置", n);
            triggerHud("配置 JSON", info, lv_color_hex(CLR_GREEN));
        }
    }
    // ME 键文本：网页端把 UTF-8 文本转成 hex 后分片下发，这里只负责落盘。
    // 按下 ME 键时才由 executeMacro("ME") 读出来发给电脑（见那里的 [HEXS] 协议），
    // 所以下发阶段一个字都不往主机打 —— 蓝牙发消息 ≠ 立刻在电脑上打字。
    //
    // == 为什么不再是"每个 ME_DATA 都 open/append/close 一次" ==
    //
    // 那是"数据稍大一点就崩溃重启"的根因。原写法每片（128 个 hex 字符）做一次
    //     open(FILE_APPEND) → f.print(hex) → f.close()
    // 这一个 open 里有**两次按文件名解析**，而 SPIFFS 的按名解析是**全盘扫描**：
    //   · VFSFileImpl 构造函数先 stat() 一次（vfs_api.cpp:295）→ SPIFFS_stat
    //   · fopen 再 open 一次 → SPIFFS_open
    //   两者都走 spiffs_object_find_object_index_header_by_name
    //   → spiffs_obj_lu_find_entry_visitor：遍历**整个文件系统**的 object
    //   lookup 页，每遇到一个已分配的对象就把那 256 字节的索引头读到 RAM 里
    //   去 strcmp 文件名（spiffs_nucleus.c:1673）。
    //   所以**一次 open 的耗时取决于文件系统里存了多少东西**，而不只是这个文件多大。
    // 于是总开销 ≈ 片数 × 全盘扫描代价，两个因子都随 ME 文本变大而变大 ——
    // 平方级增长。小文本时每片几毫秒看不出来；文本一大，某一片的 open 就
    // 卡在 loop() 里，整块主循环都停住（键盘不扫描、屏幕不刷新）。
    // 另外每片还白搭一次 4096 字节的 stdio 缓冲 malloc/free
    // （vfs_api.cpp:305 的 setvbuf，因为 vfs_spiffs_stat 不填 st_blksize）。
    //
    // 现在改成 ME_START 时**只开一次文件**，ME_DATA 只做纯写，ME_END 才 close：
    //   · 每片不再有 stat、不再有按名解析、不再有 4KB 缓冲的分配释放
    //   · 中途失败/网页跑掉也不会卡着 fd —— loop() 里 5 秒超时会收尾
    // 顺带把错误真正暴露出来：open 失败、写进去的字节数对不上，都会当场报错，
    // 而不是等 ME_END 看到一个对不上的 size 才发现。
    //
    // 注：SPIFFS 是日志式文件系统，改写同一页=写新页+把旧页标删。重复
    // "截断重写"会把分区快速用旧，进而触发 SPIFFS_write 内联的 GC
    // （spiffs_hydrogen.c:1223 → spiffs_gc_check，最多 CONFIG_SPIFFS_GC_MAX_RUNS=10 轮），
    // 那同样是 loop() 里的长阻塞。少开少关也就少了一批页的翻烧。
    else if (cmd == "ME_START") {
        // 上一轮没正常收尾就再来一次（网页重传/中途断开）——先结掉旧 fd
        if (meFile) { meFile.close(); meFile = File(); }
        meFile = SPIFFS.open("/me_hex.txt", FILE_WRITE);
        meHexBytes = 0;
        meWriteErrors = 0;
        meLastDataMs = millis();
        if (!meFile) {
            LOG_PORT.println("[ME] START open FAILED");
            triggerHudBig("ME 文本", "文件打开失败", lv_color_hex(CLR_RED));
        } else {
            LOG_PORT.println("[ME] START (single fd, opened once)");
        }
    }
    else if (cmd.startsWith("ME_DATA:")) {
        String hex = cmd.substring(8);
        if (hex.length() == 0) return;
        meLastDataMs = millis();
        // ME_START 丢了（旧网页只发 ME_DATA，或者那条包被 BLE 吞了）：自己补开一次，
        // 否则整段文本会一声不响地丢掉，比报错更难查。
        if (!meFile) {
            LOG_PORT.println("[ME] ME_DATA without START, opening file");
            meFile = SPIFFS.open("/me_hex.txt", FILE_WRITE);
            if (!meFile) { meWriteErrors++; return; }
        }
        size_t want = hex.length();
        size_t got  = meFile.print(hex);
        if (got != want) meWriteErrors++;
        meHexBytes += (uint32_t)got;
    }
    // ME_END[:flag] —— 收尾。flag 可选，**缺省 = 只存不发**。
    //
    //   ME_END      → 只把文本存进 /me_hex.txt，等用户按键盘上的 ME 键才发
    //   ME_END:1    → 存进去之后**立刻**通过 USB HID 打给主机
    //   ME_END:0    → 同 ME_END（显式写 0）
    //
    // ⚠ 为什么把 flag 挂在 ME_END 上，而不是另外发一条"现在发"的指令：
    //   「存进去」和「发出去」必须是**同一条指令**里的两个动作 ——
    //   分成两条就多一个可能丢失的环节（手机那边 ME_END 发成功了、
    //   "发"那条指令却在蓝牙断开的瞬间丢了 → 键盘里存了，用户以为发出去了）。
    //   挂在 ME_END 上，收尾和发送在固件里是原子的，不存在半截状态。
    //
    // ⚠ 向后兼容：老网页发的是光秃秃的 "ME_END"，走 startsWith 之后
    //   flag 解析出来是 0，行为和以前**一模一样**（只存不发）。
    //   所以这个改动对现有网页端是零影响的，不需要同步升级页面。
    else if (cmd.startsWith("ME_END")) {
        // ---- 解析 flag ----
        // 格式 "ME_END" 或 "ME_END:<flag>"。flag 只认 '1'（其余一律当 0）。
        // 这里**故意不做更宽松的解析**：flag 的语义是"要不要动主机键盘"，
        // 解析得越宽松，越容易出现"用户以为没发、其实发出去了"这种反向事故。
        int meSendNow = 0;
        int colon = cmd.indexOf(':');
        if (colon > 0) {
            String flagStr = cmd.substring(colon + 1);
            // 只接受单个 '1'。带 #123 请求号的话，请求号已经在进 handleCommand
            // 之前被摘掉了，这里拿到的应该是干净的 flag。
            flagStr.trim();
            if (flagStr == "1") meSendNow = 1;
        }

        if (meFile) { meFile.close(); meFile = File(); }
        size_t sz = 0;
        if (SPIFFS.exists("/me_hex.txt")) {
            File f = SPIFFS.open("/me_hex.txt", FILE_READ);
            if (f) { sz = f.size(); f.close(); }
        }
        LOG_PORT.printf("[ME] END onDisk=%u appended=%u errs=%u sendNow=%d\n",
                        (unsigned)sz, (unsigned)meHexBytes, (unsigned)meWriteErrors,
                        meSendNow);
        if (sz == 0 || meWriteErrors > 0) {
            triggerHudBig("ME 文本", meWriteErrors ? "写入出错" : "存入失败",
                       lv_color_hex(CLR_RED));
            btLinkReplyf("MEEND", "ERR:%s", meWriteErrors ? "写入出错" : "存入失败");
        }
        else {
            // 存好了。回一句给主机（手机端要知道"确实存进去了"，
            // 而不是只能盯着键盘屏幕看）。
            btLinkReplyf("MEEND", "OK:%u:%d", (unsigned)sz, meSendNow);
        }

        // ---- 要"立即发送"就在这里起发 ----
        //
        // meOutStart() 只是"起个头"：开文件、记状态、报个 HUD，
        // 真正的打字由 loop() 里的 meOutPump() 分帧做（见那段说明：
        // 一把梭会把主循环按住 40 秒）。所以在这里调它是安全的 ——
        // handleCommand 跑在主任务里，起个头不阻塞。
        //
        // ⚠ 必须在 meFile.close() **之后**：SPIFFS 同一个路径不能同时
        //   一个 fd 开着写、一个 fd 开着读。顺序错了会拿到空文件或打不开。
        if (meSendNow) {
            meOutStart();
        }
    }
    // ME_SEND —— 让主机**按现在的内容**立刻打一遍，不用重新下发文本。
    //
    // 为什么要有这条：
    //   ME_END:1 已经能"边存边发"，但那要求手机手里有文本。
    //   如果文本早就存好了（比如上次存的、或者用户在电脑上敲的），
    //   手机只想"现在把它打出去"，就不该再传一遍十几 KB 的 hex ——
    //   传一遍要好几秒，还多一次写 SPIFFS 的机会。
    //   这条指令就是那个"只发不存"的动作，等价于用户按了一下 ME 键。
    //
    // 语义和物理 ME 键完全一致（走的是同一个 meOutStart），
    // 所以"按 ME 键"和"手机发 ME_SEND"不会有任何行为差异。
    else if (cmd.startsWith("ME_SEND")) {
        LOG_PORT.println("[ME] ME_SEND (send stored text now)");
        meOutStart();      // 起个头；真正的打字由 loop() 里的 meOutPump() 分帧做
        btLinkReplyf("MESENT", "OK");
    }
    // ME_TEXT:text —— 网页端一次性直发（旧写法）。仍然保留，
    // 但和新的 ME_START/ME_DATA/ME_END 一样不再立刻往主机打字，
    // 只是先存一份到 /me_hex.txt，按 ME 键才发。
    else if (cmd.startsWith("ME_TEXT:")) {
        String text = cmd.substring(8);
        if (text.length() == 0) return;
        // 转成 hex 存，和分片那条路落到同一个文件
        String hex;
        hex.reserve(text.length() * 2);
        for (unsigned i = 0; i < text.length(); i++) {
            char b[3];
            snprintf(b, sizeof(b), "%02X", (unsigned char)text[i]);
            hex += b;
        }
        File f = SPIFFS.open("/me_hex.txt", FILE_WRITE);
        if (f) { f.print(hex); f.close(); }
    }
    // LOGO_JPEG_START:size - Wallpaper upload start
    // 网页端声明本次要发多少字节的 JPEG。收到这条就切进二进制接收模式，
    // 后面 BLE 写包一律当原始字节处理，收满自动收尾（不需要 END 包，
    // 也就没有"二进制数据里混进命令"的歧义）。
    else if (cmd.startsWith("LOGO_JPEG_START:")) {
        uint32_t total = (uint32_t)cmd.substring(16).toInt();
        LOG_PORT.printf("[WALLPAPER] START, total=%lu\n", (unsigned long)total);

        // 常驻缓冲的容量和本次声明的长度是两回事（见 logoRxAlloc 的说明），
        // 所以这里必须拿**真实容量**再验一次。以前只验 total <= LOGO_RX_MAX，
        // 而实际那块内存可能比 total 还小 —— 于是固件自己放行了一个注定越界的传输。
        if (total == 0 || total > LOGO_RX_MAX) {
            abortLogoUpload("长度不合法");
        } else if (logoRxAlloc(total) == nullptr) {
            abortLogoUpload("内存不足");
        } else if (total > logoRxCap) {
            char why[40];
            snprintf(why, sizeof(why), "超出接收缓冲 %u KB", (unsigned)(logoRxCap / 1024));
            LOG_PORT.printf("[WALLPAPER] reject: total=%lu > cap=%lu\n",
                            (unsigned long)total, (unsigned long)logoRxCap);
            abortLogoUpload(why);
        } else {
            ct_mark(CT_S_WP_RX);      // 现场：正在收 JPEG
            // 重传：不释放缓冲，只把写入位置归零重新覆盖（见 logoRxAlloc 的说明）
            logoRxTotal  = total;
            logoRxGot    = 0;
            logoRxDone   = false;
            logoRxActive = true;
            logoRxLastMs = millis();
            char buf[28];
            snprintf(buf, sizeof(buf), "%u KB", (unsigned)(total / 1024));
            triggerHud("壁纸传输", buf, lv_color_hex(CLR_AMBER));
        }
    }
    // LOGO_RAW_START:<长度> - **解码在电脑端做**，板子只收成品像素。
    //
    // 这条路和上面的 LOGO_JPEG_START 是并列的两条，不是替代：
    // 网页端继续走 JPEG（十几 KB，省流量）；命令行/自动化走这条
    // （电脑端想怎么压都行，板子上没有 JPEG 解码器，也没有解码的崩溃面）。
    //
    // ⚠ 长度是**固定**的 240*240*2 = 115200，不是主机随口说的数 ——
    //   所以这里只认这一个值，多一个字节少一个字节都拒。
    //   （JPEG 那条之所以要一个 256KB 的常驻缓冲，就是因为长度可变。）
    else if (cmd.startsWith("LOGO_RAW_START:")) {
        uint32_t total = (uint32_t)cmd.substring(15).toInt();
        LOG_PORT.printf("[WALLPAPER] RAW START, total=%lu\n", (unsigned long)total);

        if (total != (uint32_t)WP_RAW_BYTES) {
            char why[40];
            snprintf(why, sizeof(why), "RAW 长度必须是 %u", (unsigned)WP_RAW_BYTES);
            abortLogoUpload(why);
        } else if (logoRawAlloc() == nullptr) {
            abortLogoUpload("RAW 缓冲内存不足");
        } else {
            ct_mark(CT_S_WP_RX);      // 现场：正在收 RAW 像素
            logoRxRaw   = true;
            logoRxTotal = total;
            logoRxGot   = 0;
            logoRxDone  = false;
            logoRxActive = true;
            logoRxLastMs = millis();
            // ⚠ 这里**故意不调 triggerHud**：HUD 是在 handleCommand 的调用栈里
            //   建 LVGL 对象的，那条路历史上出过事（见 setScreensaverMode 附近
            //   "cfgStoreRemap 是最后一条漏网的"那段注释）。成功之后在
            //   finishLogoRaw() 里再提示，画面上一样能看到。
        }
    }
    // LOGO_RAW_FLUSH - RAW 收尾。和 LOGO_JPEG_FLUSH 一样必须显式发：
    // 少了它，固件会"收满就自己收尾"，而在途的字节会被当成文本命令。
    else if (cmd == "LOGO_RAW_FLUSH") {
        LOG_PORT.printf("[WALLPAPER] RAW FLUSH got=%lu total=%lu\n",
                        (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
        if (!logoRxRaw) {
            // 没在接收就当空操作
        } else if (logoRxGot >= logoRxTotal) {
            logoRxDone = true;          // loop() 里真正去收尾
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "数据不全 %lu/%lu",
                     (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
            abortLogoUpload(buf);
        }
    }
    // BOOTDL —— 重启进下载模式（自己给自己留一条烧录的路）。
    //
    // ⚠⚠ 为什么必须有它：
    //   这块板子的 USB 被 TinyUSB 的 HID 键盘占着，产品固件跑起来之后
    //   主机上**看不到任何串口节点**（/dev/cu.usbmodem* 整个消失）。
    //   于是"产品固件在跑"这个状态下**没法再烧任何东西** ——
    //   esptool 连不上，因为根本没有串口。
    //   以前唯一的办法是让人手动按 BOOT+EN；这条指令把这件事自动化了：
    //   把 BOOT 脚(GPIO0)拉低再复位，复位那一瞬间 ROM 采到 BOOT=0
    //   就进下载模式，USB 重新枚举，串口节点就回来了。
    //
    //   做法：先把 GPIO0 配成推挽输出并拉低，等输出稳定（100ms），
    //   再 esp_restart()。**顺序不能反** —— 复位时它必须是低电平。
    //
    // ⚠ 真正的复位动作放在 loop() 末尾做（g_bootDlPending），
    //   因为这里正处在 btLinkPoll() 的调用栈里，而回话要等下一轮
    //   pumpTx() 才发得出去；直接在这里复位，回复永远发不出去，
    //   主机那边只看到"连接断了"。
    else if (cmd == "BOOTDL") {
        btLinkReplyC("BOOTDL", "重启进下载模式");
        g_bootDlPending = true;
    }
    // BOOTAPP —— 反向操作：把 BOOT 脚拉高再复位，让芯片回到"正常启动应用"。
    //
    // 用法：BOOTDL 进下载模式 → 主机烧录 → 烧完（或者 ROM 模式里
    // 收到这条指令）用 BOOTAPP 复位回应用，**全程不用碰 BOOT/EN 按钮**。
    // 原理见 loop() 里 g_bootAppPending 那段注释。
    else if (cmd == "BOOTAPP") {
        btLinkReplyC("BOOTAPP", "重启回应用模式");
        g_bootAppPending = true;
    }
    // ME:FIRE —— 从蓝牙手动触发一次 ME 输出，和按键盘上的 ME 键**同一条路**。
    //
    // 为什么要有它：
    //   ① 这条路以前只能靠手碰键盘上的 ME 键才能走，出了事没法在脚本里复现；
    //   ② 网页上多一个"试发"按钮，不用把文本存进去再按键；
    //   ③ 它是**验证**"发 ME 会不会把主循环按住"最直接的手段 ——
    //      发的同时从蓝牙发 PING，主机还能收到回话就说明主循环还活着。
    else if (cmd == "ME:FIRE") {
        btLinkReplyC("MEFIRE", "OK");
        meOutStart();
    }
    // LOGO_JPEG_FLUSH - 网页端把最后一包写完之后显式敲一下"发完了"。
    //
    // 以前是"收满 logoRxTotal 就自动收尾"，但 BLE 的包是异步到达的：
    // 固件在收到第 N 包、累加到刚好等于 total 的那一刻就可能在下一包还在路上时
    // 冲进解码，此时 logoRxActive 已经置 false，那几包在途的 JPEG 字节就会被
    // 当成文本命令灌进命令队列。显式收尾把"发完"这件事变成一个双方都确认的动作。
    else if (cmd == "LOGO_JPEG_FLUSH") {
        if (logoRxRaw) {
            // 当前是 RAW 模式：JPEG 的收尾命令不认，但**也不 abort** ——
            // 已经收到的 RAW 像素还在缓冲里，主机补发 LOGO_RAW_FLUSH 就能收尾。
            LOG_PORT.println("[WALLPAPER] 忽略 JPEG FLUSH：当前是 RAW 模式");
        } else {
            LOG_PORT.printf("[WALLPAPER] FLUSH got=%lu total=%lu\n",
                            (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
            if (!logoRxActive) {
                // 没在接收就当空操作，网页会接着轮 LOGO_STATUS 拿到真实状态
            } else if (logoRxGot >= logoRxTotal) {
                logoRxDone = true;          // loop() 里真正去解码
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "数据不全 %lu/%lu",
                         (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
                abortLogoUpload(buf);
            }
        }
    }
    // LOGO_STATUS - 网页轮询真实结果。
    // 网页以前是"发完字节就 alert 上传成功"，那是**假成功**：BLE 写成功只说明
    // 数据交给蓝牙了，不代表键盘解出来、落盘了。现在键盘主动回报状态。
    else if (cmd == "LOGO_STATUS") {
        // RAW 模式单独用一个前缀（"RAW:"），主机侧靠它区分"我在收哪条路的数据"。
        // 电脑端轮询这个状态来确认固件已经进入接收模式 —— 这是跨任务的握手点：
        // LOGO_RAW_START 是文本命令（loop 任务处理），而二进制分片走的是
        // 蓝牙协议栈任务的 probe。第一片二进制抢在 START 前面到的话，
        // 那时候固件还没进接收模式，这一片就会被当成文本命令吞掉。
        const char* state;
        if (logoRxActive) state = logoRxRaw ? "RAW" : (logoRxDone ? "READY" : "RECV");
        else              state = wpReady ? "OK" : "IDLE";
        btLinkReplyf("LOGOSTATUS", "%s:%lu/%lu", state,
                     (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
        // 同 REMAPREAD：不要把这条响应原样再灌一遍 LOG_PORT。
        // 网页那边是 waiter 路径，而带 "LOG:" 动词的这行会被日志面板的处理器
        // 先吃掉、永远匹配不上前缀，等于白发一次，还多制造一条内容相同的
        // notify（BLE 会把连续相同通知合并掉）。壁纸上传期间网页在高频轮
        // LOGO_STATUS，多发一条的代价被放大。
        LOG_PORT.printf("[WALLPAPER] status: %s:%lu/%lu\n", state,
                        (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
    }
    // NOTIFY:text -> ALERT_GREEN
    else if (cmd.startsWith("NOTIFY:")) {
        String text = cmd.substring(7);
        pushNotification(ALERT_GREEN, text);
    }
    // ALERT: - Notification alerts
    // 颜色后面不跟正文（就是 ALERT:RED）时传空串，pushNotification() 会
    // 从常用文案库里挑一句 —— 别再回一个英文 "Alert" 糊弄过去了。
    else if (cmd.startsWith("ALERT:")) {
        String sub = cmd.substring(6);
        if (sub == "OFF" || sub == "CLEAR") {
            clearNotifications();
        }
        else if (sub.startsWith("RED")) {
            String text = sub.startsWith("RED:") ? sub.substring(4) : "";
            pushNotification(ALERT_RED, text);
        }
        else if (sub.startsWith("GREEN")) {
            String text = sub.startsWith("GREEN:") ? sub.substring(6) : "";
            pushNotification(ALERT_GREEN, text);
        }
        else if (sub.startsWith("YELLOW")) {
            String text = sub.startsWith("YELLOW:") ? sub.substring(7) : "";
            pushNotification(ALERT_YELLOW, text);
        }
    }
    else if (cmd == "BTN:KNOB" || cmd == "BTN:LIGHT" || cmd.startsWith("ROT:")) {
        // 这三条和 C3 侧键是同一套语义（见 handleC3Command），
        // 主机/BLE 也能触发，所以统一走同一个实现，别各写一份。
        if (cmd.startsWith("ROT:")) {
            knobAdjust(cmd.substring(4) == "R" ? 1 : -1);
        } else if (cmd == "BTN:KNOB") {
            handleC3Command(cmd);
        } else {
            handleC3Command(cmd);
        }
    }
}

// 每轮 loop 推进一点回拉。必须放在 loop() 里（不是 LOG:dump 的处理分支里），
// 因为发包是分摊的：一次回拉几百片，BT_POLL_BUDGET_MS 之外的时间必须还给
// 键盘扫描、LVGL 和看门狗。
static void logDumpPump() {
    if (!logDumpActive) return;
    btLinkKeepAlive();

    if (!btLinkStreamBusy()) {        // 链断了 / 对端没了：收摊，别一直挂着
        logDumpActive = false;
        return;
    }
    size_t room = btLinkStreamRoom();
    if (room == 0) return;            // 单槽满了，等下一轮再喂

    size_t pos = logDumpPos;
    // 切在行尾，别在行中间断 —— 网页那边拼起来才是一行一行。
    size_t end = pos;
    while (end < logDumpLen && end - pos < 160) {
        if (logDumpSnap[end] == '\n') { end++; break; }
        end++;
    }
    if (end == pos) end++;            // 兜底：单行超过 160 字节就硬切
    // ⚠ 必须用 room 封顶，而且只推进真正写进去的字节。btLinkStreamWrite 装不下
    //   时是**截断**（返回 true），按整段推进游标的话，被截掉的尾巴永远没人
    //   再喂 —— 8KB 日志回拉会静默丢掉最后一段，网页那边看不出来。
    size_t n = end - pos;
    if (n > room) n = room;
    btLinkStreamWrite(logDumpSnap + pos, n);
    logDumpPos = pos + n;

    if (logDumpPos >= logDumpLen) {
        btLinkStreamEnd();            // 剩下的由 btLink 发完并补收尾标记
        logDumpActive = false;        // 注意别在这里等它发完：StreamBusy() 会告诉你
        LOG_PORT.printf("[LOG] dump %u bytes handed to link\n", (unsigned)logDumpLen);
    }
}

// ===========================
// 旋钮：按 currentMode 调对应的东西
// ===========================
// 界面态（菜单 / 设置页）里旋钮是导航和调参，只有回到主界面它才是"调灯"。
// 音量、屏幕亮度、静音都是 Consumer 页 usage，本机并不真的持有音量 ——
// 只能把 usage 丢给主机，由操作系统去调（这和原版 s3.ino 的做法一致）。
static void knobAdjust(int dir) {
    if (currentSysMode == SYS_MODE_HELP) {
        // 速查表里旋钮就是"翻页"。必须排在菜单分支**前面**：
        // 速查表是从菜单进来的，不挡在前面的话拧旋钮会去翻菜单的选中项
        // （菜单屏都看不见了，表现为"拧了没反应"）。
        helpScrollBy(dir);
        return;
    }
    if (currentSysMode == SYS_MODE_MENU) {
        // 旋钮在菜单里是"上下翻"，跟着当前层级走（一级翻大类、二级翻组内项）
        uint8_t rows = menuInSub ? curMenuGroup()->count : MENU_GROUP_COUNT;
        uint8_t& sel = menuInSub ? menuItemSel : menuSel;
        sel = (dir > 0) ? (uint8_t)((sel + 1) % rows)
                        : (uint8_t)((sel + rows - 1) % rows);
        build_menu();
        return;
    }
    if (IS_SETTING_MODE(currentSysMode)) { adjustSettingField(dir); return; }

    // 转到这个模式之前先把背光开关放开：不然"旋钮没反应"其实是灯本来就是灭的
    if (currentMode == MODE_LIGHT && !lightOn) {
        lightOn = true;
        preferences.putBool("light_on", true);
    }

    char pct[8];
    switch (currentMode) {
        case MODE_LIGHT: {
            if (dir > 0) brightness = (brightness <= 235) ? (uint8_t)(brightness + 20) : 255;
            else          brightness = (brightness >= 20)  ? (uint8_t)(brightness - 20) : 0;
            preferences.putUChar("brightness", brightness);
            snprintf(pct, sizeof(pct), "%d%%", brightness * 100 / 255);
            triggerHud("键盘背光", pct, lv_color_hex(CLR_AMBER));
            break;
        }
        case MODE_SCREEN_BRIGHTNESS: {
            // ⚠ 锁定期间**不许碰主机**：锁的意义就是"键盘不控制主机"。
            //   而旋钮恰好是用户"用静音键锁完之后最容易顺手拧一下"的东西 ——
            //   锁完一拧就把主机音量/屏幕亮度改了，非常招投诉。
            //   （背光 MODE_LIGHT 和灯效 MODE_CPG 都是本机自己的，照常能调。）
            if (kbLocked) { lockScreenFlash(); break; }
            if (dir > 0) ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_INCREMENT);
            else          ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_DECREMENT);
            ConsumerControl.release();
            triggerHud("屏幕亮度", dir > 0 ? "调亮" : "调暗", lv_color_hex(CLR_ACCENT));
            break;
        }
        case MODE_MUTE: {
            if (kbLocked) { lockScreenFlash(); break; }
            if (dir > 0) ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
            else          ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
            ConsumerControl.release();
            triggerHud("系统音量", dir > 0 ? "增加" : "减少", lv_color_hex(CLR_GREEN));
            break;
        }
        case MODE_CPG:
        default: {
            if (dir > 0) currentEffect = (uint8_t)((currentEffect + 1) % MAX_EFFECTS);
            else          currentEffect = (uint8_t)((currentEffect + MAX_EFFECTS - 1) % MAX_EFFECTS);
            preferences.putUChar("effect", currentEffect);
            triggerHud("灯效切换", effectNames[currentEffect], lv_color_hex(CLR_ACCENT));
            break;
        }
    }
}

// ===========================
// CPG 双击判定
// ===========================
//
// 为什么必须放在 S3 侧：双击的判据是"两次短按之间隔了多久"，而 **C3 小 MCU
// 不能刷固件**（它跑的是出厂那版）。C3 那边只认得两种按键事件：
//   · BTN:CPG      —— 每次松开都发一次（短按）
//   · BTN:CPG_HOLD —— 按住满 3 秒才发
// 它**不在按下的瞬间发任何东西**，所以 S3 能拿到的原始信息只有"什么时候收到
// 一条 BTN:CPG"。双击只能在这儿判。
//
// ⚠ 顺带说明一件做不到的事：**长按阈值没法从 3 秒改成 2 秒**。holdDuration
//   （3000ms）写死在 C3 固件的按钮表里，S3 只能在 3 秒那一刻才收到 BTN:CPG_HOLD，
//   3 秒之前它对"键还按着"这件事完全无知 —— 想缩短就只能刷 C3。
//
// 窗口取 350ms 是有依据的：切换灯效是"**看着结果**再按下一次"的反馈型操作，
// 两次按键天然被人的视觉反应时间（250~400ms）隔开；而故意双击的人也是这个量级。
// 窗口太紧 → 双击打不中，用户以为功能坏了；太松 → 连着快速切灯效时误触发重启
// （这个代价小：重启只花 30ms，键盘照常用，屏幕上还有 HUD 明说发生了什么）。
#define CPG_DOUBLE_TAP_MS 350
static unsigned long g_cpgTapMs    = 0;    // 上一条 BTN:CPG 的时刻
static bool         g_cpgTapArmed = false; // 是否还挂着一次"待配对"的单击

// C3 -> 本机的一条指令。语义照抄 s3.ino 原版，去掉 g_forceOff 那段
// （本工程的背光总开关叫 lightOn）。
static void handleC3Command(const String& cmdIn) {
    // ⚠ 响铃期间的**长按静音**，降级成短按静音。
    //
    // 为什么：用户长按静音八成是想让铃停下来，而不是想重启键盘。而重启的副作用
    // 还挺大 —— alarmLastFiredYday 是纯 RAM 变量（853 行），重启归零；墙钟又从
    // NVS 的 set_epoch 恢复（11795 行）。只要恢复出来的时间正好落在闹钟那一分钟，
    // 开机就再响一次，用户长按几下就成死循环了。老固件 s3.ino:3948 就是这么处理
    // 的（"响铃期间长按不当重启用"），lvgl_s3 搬过来时把这段丢了。
    //
    // 用"改写命令"而不是"复制一份逻辑"：降级之后下面所有分支看到的都是 BTN:MUTE
    // —— 宠物反应、静音发给主机、停铃、确认通知，全都原样走一遍，不会两处写
    // 两份再漂移。
    //
    // 非响铃时的长按静音不受影响，照旧整机重启（见下面 BTN:MUTE_HOLD 分支）。
    String cmd = cmdIn;
    if (cmd == "BTN:MUTE_HOLD" && ringingKind != RING_NONE) {
        cmd = "BTN:MUTE";
        // ⚠ 必须清掉双击配对标志：静音键自己还有一套"双击 = 锁定/解锁"
        //   （MUTE_DOUBLE_TAP_MS）。不清的话，这次降级会被当成双击的第二击，
        //   结果不是停铃，而是把键盘锁了 —— 闹钟还响着，键盘却按不动了。
        g_muteTapArmed = false;
    }

    if (cmd == "PONG") { c3Connected = true; return; }

    // 息屏期间任何 C3 动作都算"有人回来了"（旋钮碰一下也算）
    if (currentSysMode == SYS_MODE_SLEEP) gotoMainScreen();

    if (cmd == "ENC:+")      { knobAdjust(1);  return; }
    if (cmd == "ENC:-")      { knobAdjust(-1); return; }
    if (cmd.startsWith("ROT:")) { knobAdjust(cmd.substring(4) == "R" ? 1 : -1); return; }

    if (cmd == "BTN:KNOB") {
        if (currentSysMode == SYS_MODE_MENU) {
            handleMenuSelect();
        } else if (IS_SETTING_MODE(currentSysMode)) {
            saveSettingScreen();
        } else if (currentSysMode == SYS_MODE_HELP) {
            // 速查表里按旋钮 = 关闭（表已经翻完了，没什么要"确认"的）
            helpClose();
        } else {
            pulseRhythm();   // 顺手打一下律动，旋钮按下去是有反馈的
        }
        return;
    }

    if (cmd == "BTN:LIGHT") {
        // 菜单里按灯光键 = 退回主屏（和 MC 一样的手感，别把人困在菜单里）
        if (currentSysMode == SYS_MODE_MENU) { gotoMainScreen(); return; }
        // 响铃排在最前：闹钟/倒计时到点时，这一下就是"停铃"（用户指定的
        // 停铃键就是灯光键和静音键）。必须排在通知之前 —— 不然响铃期间先被
        // 通知确认吃掉，用户会以为按了没反应，而灯还在闪。
        if (ringingKind != RING_NONE) { stopRinging(); return; }
        // 告警排在最前：有待处理通知时，这一下是"我已知晓"，
        // 只掉最新一条，剩下的继续显示、继续闪。不切控制目标，
        // 免得通知刚确认完旋钮就调到别的东西上。
        if (acknowledgeAlert()) return;
        // 没通知时才轮到灯光键自己的职责：在 背光 / 屏幕亮度 之间切目标
        currentMode = (currentMode == MODE_LIGHT) ? MODE_SCREEN_BRIGHTNESS : MODE_LIGHT;
        triggerHud("控制目标", modeNamesCN[currentMode], lv_color_hex(CLR_AMBER));
        return;
    }
    if (cmd == "BTN:LIGHT_HOLD") {
        // 倒计时全屏的退出口（用户定的）：按住灯光键收回全屏，
        // 但**倒计时继续在后台跑** —— 屏只是"看一眼还剩多久"，
        // 收走屏不等于取消这次计时。跑完照样会响铃提醒。
        //
        // 注意这里不 return：退出全屏之后这一次长按仍然按原逻辑切背光总开关，
        // 免得"退出全屏"顺手把用户的灯关掉了、还得再按一次才能开回来。
        if (scr_countdown) {
            countdownShown = false;
            destroyCountdownScreen();
            if (currentSysMode == SYS_MODE_NORMAL) showScreen(ensureMainScreen());
            triggerHud("倒计时", "已收回屏幕，仍在计时", lv_color_hex(CLR_TEXT_DIM));
            return;
        }
        lightOn = !lightOn;
        preferences.putBool("light_on", lightOn);
        triggerHud("背光总开关", lightOn ? "已开启" : "已关闭", lv_color_hex(CLR_AMBER));
        return;
    }
    if (cmd == "BTN:MUTE") {
        // ---- 双击 = 锁定 / 解锁键盘 ----
        //
        // 和 CPG 双击（重启扩展芯片）同一套写法：两击之间 ≤350ms 就算双击。
        //
        // 第一击**照常静音**：静音键用得远比"锁定"频繁，为了双击把它延后
        // 350ms 再执行，手感会明显变钝。代价是双击的副作用是"静音 + 锁定"，
        // 两个语义共用一个键的必然结果（CPG 双击重启也是同理）。
        //
        // 锁着的时候双击 = 解锁，和 Fn+LOGO 等价。留着这条是因为万一 Fn 键
        // 接触不良，用户至少还有一条自救的路。
        // 背光总开关和控制目标。放在双击判据**之前**：双击命中时下面会 return，
        // 放后面的话第二击就漏了这两步，两击的行为对不齐。
        if (!lightOn) { lightOn = true; preferences.putBool("light_on", true); }
        currentMode = MODE_MUTE;

        unsigned long now = millis();
        if (g_muteTapArmed && (now - g_muteTapMs) <= MUTE_DOUBLE_TAP_MS) {
            g_muteTapArmed = false;   // 清掉，第三次点击当成一次全新的单击
            lockSetLocked(!kbLocked);
            return;
        }
        g_muteTapArmed = true;
        g_muteTapMs    = now;

        // ⚠ 锁定期间**不发**静音给主机：锁的意义就是"键盘不控制主机"。
        //   但"停铃"和"确认通知"是**本机**的事，照旧做 ——
        //   闹钟响着的时候没人能打字，静音键是唯一的"停下来"手段。
        if (kbLocked) {
            lockScreenFlash();     // 和其它键一样：闪红框 = 收到了，但被锁着
            if (ringingKind != RING_NONE) {
                stopRinging();
                return;
            }
            if (!acknowledgeAlert()) {
                triggerHudBig("键盘已锁定", "按键不会发给电脑", lv_color_hex(CLR_RED));
            }
            return;
        }

        ConsumerControl.press(CONSUMER_CONTROL_MUTE);
        ConsumerControl.release();
        // 静音键同时也是停铃键（和灯光键一起，用户指定的两个）。
        // 静音本身照常发给主机 —— 响铃期间用户很可能正在放声音，
        // 把静音也吞掉反而是帮倒忙。所以只停铃，不 return。
        if (ringingKind != RING_NONE) {
            stopRinging();
            return;
        }
        // 顺手把最新一条通知处理掉
        if (!acknowledgeAlert()) {
            triggerHud("静音控制", "静音切换", lv_color_hex(CLR_GREEN));
        }
        return;
    }
    if (cmd == "BTN:CPG") {
        // ---- 双击 = 重启键盘扩展芯片（MCP23017）----
        //
        // 第一次点击的灯效切换**照常立刻执行**。不能为了双击把它延后 350ms
        // 再切 —— 那会让"单按切灯效"这个原有功能的手感明显变钝，而它的使用
        // 频率比双击高得多。代价是：双击之后灯效会前进一档，这是"单按=切灯效
        // / 双击=重启芯片"两个语义共用一个键的必然结果。
        //
        // 第二次点击则**不再切灯效**（否则双击会连切两档），改成重启芯片。
        unsigned long now = millis();
        if (g_cpgTapArmed && (now - g_cpgTapMs) <= CPG_DOUBLE_TAP_MS) {
            g_cpgTapArmed = false;   // 清掉，第三次点击当成一次全新的单击
            bool ok = mcpRestartChip();
            if (ok) {
                triggerHudBig("键盘扩展已重启", "MCP23017 复位完成", lv_color_hex(CLR_GREEN));
            } else {
                triggerHudBig("键盘扩展重启失败", "正在自动恢复", lv_color_hex(CLR_RED));
            }
            return;
        }
        g_cpgTapArmed = true;
        g_cpgTapMs    = now;

        if (!lightOn) { lightOn = true; preferences.putBool("light_on", true); }
        currentMode = MODE_CPG;
        if (currentEffect == 0) currentEffect = 1;   // 跳过"关闭"这一档
        else currentEffect = (uint8_t)((currentEffect + 1) % MAX_EFFECTS);
        preferences.putUChar("effect", currentEffect);
        triggerHud("灯效切换", effectNames[currentEffect], lv_color_hex(CLR_ACCENT));
        return;
    }
    // CPG 长按 = 重启键盘扩展芯片（MCP23017），**不再**重启整个 S3。
    //
    // 为什么改：从 C3 过来的长按原本被当成"进下载模式重启"，直接 esp_restart()。
    // 但这套固件整机重启的代价太大（SPIFFS / BLE / USB 协议栈全套重来，几十秒
    // 黑屏），而用户按下这个键时想解决的往往只是"键盘不灵了" —— 那种情况
    // 把 MCP23017 复位一下就够了，不该连累整机。详见 mcpRestartChip()。
    //
    // 它现在是**双击之外的兜底**：双击（两次 BTN:CPG，间隔 ≤350ms）是快路径，
    // 长按是慢路径。两者互不干扰 —— C3 那边按住满 3 秒只发 BTN:CPG_HOLD、
    // 不会再补发 BTN:CPG，所以长按永远不会凑成双击。
    //
    // MUTE 长按保持原样（整机重启）—— 它是剩下的那一个整机重启入口。
    if (cmd == "BTN:CPG_HOLD") {
        g_cpgTapArmed = false;   // 长按期间不要把之前的单击当成双击的第一击
        bool ok = mcpRestartChip();
        if (ok) {
            triggerHudBig("键盘扩展已重启", "MCP23017 复位完成", lv_color_hex(CLR_GREEN));
        } else {
            triggerHudBig("键盘扩展重启失败", "正在自动恢复", lv_color_hex(CLR_RED));
        }
        return;
    }
    // 长按静音 = **整机重启**。响铃期间到不了这里 —— 函数开头已经把长按降级成
    // 短按静音了（停铃，不重启）。
    if (cmd == "BTN:MUTE_HOLD") {
        g_muteTapArmed = false;   // 长按期间不要把之前的单击当成双击的第一击
        triggerHudBig("系统重启", "请稍候", lv_color_hex(CLR_RED));
        // ⚠ 停 1.2 秒再重启，不多不少。
        //   这是**整机重启**入口（看门狗拆掉之后，它就是固件里唯一一处主动
        //   esp_restart()）。大提示只亮 2.4 秒，重启要是只等 600ms，用户
        //   根本来不及看清"系统重启"四个字 —— 而这四个字正是他唯一的确认。
        //   1.2 秒是"看清一行字"和"别让人以为死机了"之间的折中。
        pendingRestartMs = millis() + 1200;
        return;
    }
}

// ===========================
// 灯效引擎
// ===========================
static uint32_t colorHSV(uint16_t hue, uint8_t sat, uint8_t val) {
    uint8_t r, g, b;
    uint8_t base = ((255 - sat) * val) >> 8;
    switch ((hue / 10922) % 6) {
        case 0: r = val; g = (((val - base) * (hue % 10922)) / 10922) + base; b = base; break;
        case 1: r = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; g = val; b = base; break;
        case 2: r = base; g = val; b = (((val - base) * (hue % 10922)) / 10922) + base; break;
        case 3: r = base; g = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; b = val; break;
        case 4: r = (((val - base) * (hue % 10922)) / 10922) + base; g = base; b = val; break;
        default: r = val; g = base; b = (((val - base) * (10922 - (hue % 10922))) / 10922) + base; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void setLedRGB(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= 0 && index < TOTAL_LEDS) {
        ledRgb[index][0] = r;
        ledRgb[index][1] = g;
        ledRgb[index][2] = b;
    }
}

static void clearAllLeds() {
    for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, 0, 0, 0);
}

static void setMainLedsColor(uint8_t r, uint8_t g, uint8_t b) {
    // 上界是 NUM_MAIN_LEDS（16），不是写死的 15 —— 原版 s3.ino 这里用的就是
    // NUM_MAIN_LEDS，移植时被改成 15，于是 0 号之外的第 16 颗主灯（下标 15）
    // 在"纯红/纯绿/纯白"这些常亮灯效下永远是黑的。彗星/双彗星/彩虹也有同样的问题。
    for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, r, g, b);
}

// 三颗状态指示灯（对应 C3 上 16/17/18 号物理灯位）
// 亮色沿用原版 s3.ino 的取值：NUM 橙 / CAPS 蓝 / SCR 琥珀红。
// 这三颗的缩放在这里自己做，用灯光设置页里的"状态灯亮度"那一档 ——
// sendLedFrameToC3 只给 0~15 乘主背光 brightness，混进去的话
// 背光拧到最暗时锁状态会一起消失，而锁状态恰恰是必须一直看得见的信息。
static void renderIndicators(void) {
    uint8_t b = indBrightness;
    setLedRGB(16, (numLock    ? (uint8_t)(255 * b / 255) : 0),
                     (numLock    ? (uint8_t)(180 * b / 255) : 0), 0);
    setLedRGB(17, 0, (capsLock   ? (uint8_t)(150 * b / 255) : 0),
                     (capsLock   ? (uint8_t)(255 * b / 255) : 0));
    setLedRGB(18, (scrollLock ? (uint8_t)(255 * b / 255) : 0),
                     (scrollLock ? (uint8_t)( 50 * b / 255) : 0), 0);
}

static void drawBreathing(uint8_t maxR, uint8_t maxG, uint8_t maxB) {
    static uint16_t effectFrame = 0;
    float val = (exp(sin(effectFrame * 0.03)) - 0.36787944) * 108.0;
    float ratio = val / 255.0;
    if (ratio > 1.0) ratio = 1.0;
    if (ratio < 0.0) ratio = 0.0;
    setMainLedsColor(maxR * ratio, maxG * ratio, maxB * ratio);
    effectFrame++;
}

// ---- 按键特效 ----
// triggerKeyReaction() 在键盘扫描里"按键按下"时调一次，只是把状态置起来；
// 真正的绘制在 renderLightingEngine() 里做（那边才有 20ms 的稳定节拍）。
// 这样按键扫描那条热路径上只多一次赋值，不会因为画灯而拖慢扫描。
static void triggerKeyReaction(void) {
    if (keyFxStyle == KEYFX_OFF) return;

    // 颜色每按一次换一种。原版 s3.ino 的"自动"档就是这个配色表。
    static const uint32_t autoColors[6] = {
        0xFF0000, 0x0000FF, 0x00FF00, 0xB400FF, 0x00FFFF, 0xFFFFFF
    };
    keyFxColor = autoColors[keyFxColorIdx];
    keyFxColorIdx = (uint8_t)((keyFxColorIdx + 1) % 6);

    if (keyFxStyle == KEYFX_SHOOT) {
        // 找一发空闲的"子弹"；全都在飞就把第 0 发重置（原版就是这个兜底）
        bool spawned = false;
        for (int i = 0; i < KEYFX_MAX_SHOTS; i++) {
            if (keyFxShotStep[i] < 0) { keyFxShotStep[i] = 0; spawned = true; break; }
        }
        if (!spawned) keyFxShotStep[0] = 0;
    } else if (keyFxStyle == KEYFX_STACK) {
        // 已经堆了几格就保留几格，新按的这一发从那一格开始往里推
        if (keyFxStep > 0) keyFxStack = (uint8_t)((keyFxStack + 1) % NUM_MAIN_LEDS);
        keyFxStep = 0;
    } else {
        keyFxStep = 0;
    }
    keyFxActive = true;
}

// 画一帧按键特效，返回 false 表示这一段播完了（调用方把 keyFxActive 清掉）。
// 语义完全照抄原版 updateKeyReaction()：涟漪从中间向两侧扩散、
// 发射从最后一颗往第一颗扫、堆叠从最后一颗往回填。
static bool applyKeyReaction(void) {
    uint8_t cr = (uint8_t)((keyFxColor >> 16) & 0xFF);
    uint8_t cg = (uint8_t)((keyFxColor >>  8) & 0xFF);
    uint8_t cb = (uint8_t)( keyFxColor        & 0xFF);

    if (keyFxStyle == KEYFX_RIPPLE) {
        int left  = (NUM_MAIN_LEDS / 2 - 1) - keyFxStep;
        int right = (NUM_MAIN_LEDS / 2)     + keyFxStep;
        if (left >= 0) setLedRGB(left, cr, cg, cb);
        if (right < NUM_MAIN_LEDS) setLedRGB(right, cr, cg, cb);
        keyFxStep++;
        if (keyFxStep > NUM_MAIN_LEDS / 2) return false;

    } else if (keyFxStyle == KEYFX_SHOOT) {
        bool any = false;
        for (int i = 0; i < KEYFX_MAX_SHOTS; i++) {
            if (keyFxShotStep[i] < 0) continue;
            int pos = (NUM_MAIN_LEDS - 1) - keyFxShotStep[i];
            if (pos >= 0 && pos < NUM_MAIN_LEDS) setLedRGB(pos, cr, cg, cb);
            keyFxShotStep[i]++;
            if (keyFxShotStep[i] >= NUM_MAIN_LEDS) keyFxShotStep[i] = -1;
            else any = true;
        }
        if (!any) return false;

    } else if (keyFxStyle == KEYFX_STACK) {
        for (int i = 0; i < keyFxStack; i++) setLedRGB(i, cr, cg, cb);
        if (keyFxStep < NUM_MAIN_LEDS - keyFxStack) {
            setLedRGB((NUM_MAIN_LEDS - 1) - keyFxStep, cr, cg, cb);
        }
        keyFxStep++;
        if (keyFxStep >= NUM_MAIN_LEDS - keyFxStack) {
            keyFxStack++;
            if (keyFxStack >= NUM_MAIN_LEDS) keyFxStack = 0;
            return false;
        }
    }
    return true;
}

static void renderLightingEngine(void) {
    // 响铃优先于一切：闹钟/倒计时到点时，整排主背光全亮黄灯快闪，
    // 平时设的灯效、背光亮度、背光总开关这期间一律不生效。
    //
    // 为什么排在通知告警（下面那段）前面：通知是主机推过来的"消息"，
    // 响铃是"到点了、现在不动手就来不及"，后者才该抢整排灯。
    // 反过来的话，主机恰好在这时候推一条红告警，屏上写着"闹钟响了"、
    // 灯却在闪红，两边各说各话。
    if (ringingKind != RING_NONE) {
        static unsigned long lastRingBlinkMs = 0;
        static bool ringBlink = false;
        unsigned long nowMs = millis();
        if (nowMs - lastRingBlinkMs >= 250) { lastRingBlinkMs = nowMs; ringBlink = !ringBlink; }

        if (ringBlink) {
            // 琥珀 = (255,180,0)，和屏上描边、卡片标题同一个色
            for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, 255, 180, 0);
        } else {
            // 灭的那半拍只压到 1/4 而不是全灭：全灭在余光里等于"灯坏了"，
            // 留一点底色才看得出是在闪。255/180/0 各除 4 = 63/45/0。
            for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, 63, 45, 0);
        }
        renderIndicators();
        return;
    }

    // 告警优先：队列里有东西的时候，0~15 主背光整个让出来给告警闪，
    // 平时设的灯效和背光亮度这期间一律不生效 —— 告警就该是全场最显眼的东西。
    // 16~18 三颗锁状态灯照旧由 renderIndicators 驱动，锁状态不能被吞掉。
    AlertType alert = currentAlertType();
    if (alert != ALERT_NONE) {
        static unsigned long lastBlinkMs = 0;
        static bool blinkOn = false;
        unsigned long nowMs = millis();
        if (nowMs - lastBlinkMs >= 180) { lastBlinkMs = nowMs; blinkOn = !blinkOn; }

        if (blinkOn) {
            uint8_t r = 0, g = 0, b = 0;
            switch (alert) {
                case ALERT_RED:    r = 255;             break;
                case ALERT_GREEN:  g = 255;             break;
                case ALERT_YELLOW: r = 255; g = 180;    break;
                default:           r = 0; g = 200; b = 255; break;
            }
            for (int i = 0; i < NUM_MAIN_LEDS; i++) setLedRGB(i, r, g, b);
        } else {
            clearAllLeds();   // 只清 0~15，锁状态灯 16~18 留给 renderIndicators
        }
        renderIndicators();
        return;
    }

    // 背光总开关只管 0~15；16~18 三颗锁状态灯始终由 renderIndicators 驱动，
    // 不受背光开关和背光亮度影响（见 renderIndicators 的注释）
    if (!lightOn) {
        clearAllLeds();
    } else {
        // 根据 currentEffect 执行对应灯效
        switch (currentEffect) {
        case 0: clearAllLeds(); break;  // 关闭
        case 1: setMainLedsColor(255, 0, 0); break;      // 纯红
        case 2: setMainLedsColor(0, 255, 0); break;      // 纯绿
        case 3: setMainLedsColor(0, 0, 255); break;      // 纯蓝
        case 4: setMainLedsColor(0, 127, 255); break;     // 冰蓝
        case 5: setMainLedsColor(255, 255, 255); break;    // 纯白
        case 6: drawBreathing(255, 0, 0); break;         // 红呼吸
        case 7: drawBreathing(0, 255, 0); break;         // 绿呼吸
        case 8: drawBreathing(0, 0, 255); break;         // 蓝呼吸
        case 9: drawBreathing(0, 127, 255); break;        // 冰蓝呼吸
        case 10: { // 彗星
            static uint16_t effectFrame = 0;
            clearAllLeds();
            int totalSteps = (NUM_MAIN_LEDS - 1) * 2;
            int step = effectFrame % totalSteps;
            int pos = (step < NUM_MAIN_LEDS) ? step : (totalSteps - step);
            setLedRGB(pos, 255, 0, 50);
            for (int i = 0; i < NUM_MAIN_LEDS; i++) {
                int diff = abs(i - pos);
                if (diff == 1) setLedRGB(i, 80, 0, 15);
                else if (diff == 2) setLedRGB(i, 20, 0, 3);
            }
            effectFrame++;
            break;
        }
        case 11: { // 双彗星
            static uint16_t effectFrame = 0;
            clearAllLeds();
            int totalSteps = (NUM_MAIN_LEDS - 1) * 2;
            int step = effectFrame % totalSteps;
            int pos1 = (step < NUM_MAIN_LEDS) ? step : (totalSteps - step);
            int pos2 = (step < NUM_MAIN_LEDS) ? (NUM_MAIN_LEDS - 1 - step)
                                              : (step - NUM_MAIN_LEDS + 1);
            setLedRGB(pos1, 180, 0, 255);
            setLedRGB(pos2, 0, 180, 255);
            for (int i = 0; i < NUM_MAIN_LEDS; i++) {
                if (abs(i - pos1) == 1) setLedRGB(i, 50, 0, 80);
                if (abs(i - pos2) == 1) setLedRGB(i, 0, 50, 80);
            }
            effectFrame++;
            break;
        }
        case 12: { // 彩虹流光
            static uint16_t effectFrame = 0;
            for (int i = 0; i < NUM_MAIN_LEDS; i++) {
                uint32_t col = colorHSV(effectFrame + (i * 65536L / NUM_MAIN_LEDS), 255, 255);
                setLedRGB(i, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
            }
            effectFrame += 256;
            break;
        }
        default: clearAllLeds(); break;
        }

        // ---- 按键特效叠加层 ----
        // 必须压在**上面这套基础灯效之后**：先把 0~15 整体压暗到 ~40%，
        // 再把特效那几颗点亮，特效才压得住底。顺序反了会被灯效盖掉。
        // 40% 是原版 s3.ino updateKeyReaction() 里的 *102>>8，照抄。
        // 告警期间走不到这里（上面已经 return），16~18 锁状态灯也不受影响。
        if (keyFxActive && keyFxStyle != KEYFX_OFF) {
            for (int i = 0; i < NUM_MAIN_LEDS; i++) {
                ledRgb[i][0] = (uint8_t)((ledRgb[i][0] * 102) >> 8);
                ledRgb[i][1] = (uint8_t)((ledRgb[i][1] * 102) >> 8);
                ledRgb[i][2] = (uint8_t)((ledRgb[i][2] * 102) >> 8);
            }
            if (!applyKeyReaction()) keyFxActive = false;
        }
    }

    // 三颗锁状态灯：无论背光是开是关都要写，锁状态是必须一直看得见的信息
    renderIndicators();
}

static void sendLedFrameToC3(void) {
    uint8_t packet[61];
    packet[0] = 0xAA;
    packet[1] = 0x55;
    packet[2] = 0x01;

    // 0~15 主背光：按 brightness 缩放
    // 响铃期间强制满量：用户可能平时把亮度调到很低（夜里用），
    // 按那个比例缩下去，"闪"就几乎看不见了，闹钟等于没响。
    uint8_t ledScale = (ringingKind != RING_NONE) ? 255 : brightness;
    for (int i = 0; i < NUM_MAIN_LEDS; i++) {
        packet[3 + i * 3 + 0] = (ledRgb[i][0] * ledScale) / 255;
        packet[3 + i * 3 + 1] = (ledRgb[i][1] * ledScale) / 255;
        packet[3 + i * 3 + 2] = (ledRgb[i][2] * ledScale) / 255;
    }
    // 16~18 状态指示灯：renderIndicators() 已经乘过 indBrightness，这里原样发
    for (int i = NUM_MAIN_LEDS; i < TOTAL_LEDS; i++) {
        packet[3 + i * 3 + 0] = ledRgb[i][0];
        packet[3 + i * 3 + 1] = ledRgb[i][1];
        packet[3 + i * 3 + 2] = ledRgb[i][2];
    }
    packet[60] = 0xEE;
    Serial1.write(packet, sizeof(packet));
}

// ===========================
// 动态元素更新
// ===========================
// 100ms 跑一次就够了。这里的值全是慢变量：时钟只显示到分钟、日期一天变一次、
// 温湿度 15 分钟才读一次、击键数/锁状态是用户动作触发的。原来是每轮 loop()
// 都跑，而 loop() 里排着阻塞式 I2C 的键盘扫描，循环频率高达几百 Hz ——
// 就算下面的 setText/setBgColor 都做了变化检测，每轮仍然要白跑一遍
// time()/localtime()/strftime()（localtime 在 ESP32 上要算时区，并不便宜）。
#define DYNAMIC_REFRESH_MS 100UL
static void updateDynamicElements(void) {
    static unsigned long lastRunMs = 0;
    unsigned long nowMs = millis();
    // 锁状态刚翻转：跳过 100ms 节拍，立刻跑一次屏幕刷新。
    // 不这么做的话，刚按 Caps Lock 要等最坏 100ms 才看到顶部条/信息面板的灯变，
    // 体感上就是"按了没反应"。
    if (!lockStateDirty && nowMs - lastRunMs < DYNAMIC_REFRESH_MS) return;
    lastRunMs = nowMs;

    // 「方案+按键」这一页**不显示时钟**，它显示的是"刚按下的键 / 今日次数 /
    // 温湿度 / 锁灯" —— 一样都不依赖系统时间。所以必须在下面那个
    // "时间没校准就 return"的早退**之前**更新它。
    //
    // 要是跟其它风格一样塞在下面的 switch 里（switch 在早退之后），那在
    // 没对上 NTP 的时候这一页会一直停在初始状态：按什么键屏幕都不动 ——
    // 正好把这一页最核心的东西（按键反馈）废掉了。
    if (currentDispMode == DISP_MODE_PLAN_KEY) {
        updatePlanKeyElements();
    }

    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    // 时间还没校准（NTP 没拿到 / savedEpoch 是 0）—— 把锁脏位先放掉，
    // 不放的话下面这个 early return 会让 lockStateDirty 一直为真，
    // 下一轮 loop 又会进这里白跑一次 time()/localtime()，CPU 空转。
    // 锁 UI 这一帧确实没刷到，但 loop() 上半段已经把 LED 帧推上去了，
    // 肉眼上 Caps Lock 灯还是亮起来的；屏幕图标等时间校准后再由 100ms 节拍补上。
    if (!ti || ti->tm_year < 124) { lockStateDirty = false; return; }

    static char time_buf[16], date_buf[32], num_buf[24];

    strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
    // 主屏日期统一走中文（formatDateCN）。
    formatDateCN(date_buf, sizeof(date_buf), ti->tm_mon + 1, ti->tm_mday, ti->tm_wday);
    snprintf(num_buf, sizeof(num_buf), "%u", totalKeyCount);

    // 顶部条的锁灯 + 方案指示（6 种风格共用）
    // 三颗锁只占左上角 60px，离远了看不出"大写是不是开着"，所以除了圆点+外圈
    // 变色，状态**刚翻转**的那一颗再弹一条 HUD，给一个明确的中文提示。
    const bool locks[3] = { numLock, capsLock, scrollLock };
    static const char* lockNamesCN[3] = { "数字锁定", "大写锁定", "滚动锁定" };
    for (int i = 0; i < 3; i++) {
        setBgColor(topLockDot[i], locks[i] ? topLockOn[i] : CLR_TEXT_MUTE);
        setHidden(topLockRing[i], !locks[i]);
        // lockPrevValid == false 表示界面刚重建过：这一帧只记录不弹窗，
        // 否则重建后锁状态一样也会被判成"刚翻转"，回一次主屏弹三条提示。
        if (lockPrevValid && locks[i] != lockPrev[i]) {
            triggerHud(lockNamesCN[i], locks[i] ? "开启" : "关闭",
                       lv_color_hex(locks[i] ? lockLedColor[i] : CLR_TEXT_MUTE));
        }
        lockPrev[i] = locks[i];
    }
    lockPrevValid = true;

    // 方案指示：序号常显，系统图标只有方案 1(Windows) / 2(macOS) 才有；
    // 3/4 不是按系统分的方案，光看序号即可。
    updateTopBarProfile();

    switch (currentDispMode) {
        case DISP_MODE_GEEK: {
            setText(gk_lbl_clock, time_buf);
            setText(gk_lbl_date, date_buf);

            static char tbuf[16], hbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.1fC", shtTemp);
            snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);
            setText(gk_lbl_temp, tbuf);
            setText(gk_lbl_hum, hbuf);
            // 累计次数是卡片右上角的角标，纯数字就够，不用再加单位
            setText(gk_lbl_keys, num_buf);
            // 中间的按键反馈。常量显示，不再有"按键回显开关"（那个开关不落盘，
            // 重启就自己弹回来）。这个 label 是 montserrat_28，画不出汉字。
            setText(gk_lbl_lastkey, lastKeyPressed);
            break;
        }

        case DISP_MODE_BIG_CLOCK: {
            setText(bc_lbl_time, time_buf);
            setText(bc_lbl_date, date_buf);
            break;
        }

        case DISP_MODE_INFO_PANEL: {
            setText(ip_lbl_clock, time_buf);
            setText(ip_lbl_date, date_buf);
            setText(ip_lbl_keys, num_buf);
            // 最后一颗是"刚按下的键"。这个 label 挂的是 montserrat_48，
            // Montserrat 里没有汉字、也没有 CJK 回退（CJK 字体的 fallback 是单向的
            // —— simsun→montserrat 有，montserrat→simsun 没有），写中文上去是一片空白。
            setText(ip_lbl_lastkey, lastKeyPressed);
            // 锁状态三层一起变：圆点亮语义色、文字提到主文字色、整颗胶囊底色抬起来。
            // 之前 ip_circle_* 建完之后根本没人刷新，三颗灯从头到尾都是灭的。
            // setBgColor/setTextColor 会先读回比对，值没变就不写 —— 这是关键，
            // 见文件上方 setBgColor 的注释（LVGL 8.4 无条件 invalidate）。
            for (int i = 0; i < 3; i++) {
                bool on = locks[i];
                setBgColor(ipLockDot[i],  on ? lockLedColor[i] : CLR_STROKE);
                setTextColor(ipLockLbl[i], on ? CLR_TEXT : CLR_TEXT_MUTE);
                setBgColor(ipLockChip[i], on ? CLR_SURFACE_2 : CLR_SURFACE);
            }
            break;
        }

        case DISP_MODE_KEY_MON: {
            // 最后一颗是"刚按下的键"。montserrat_48 没有汉字、也没有 CJK 回退。
            setText(km_lbl_lastkey, lastKeyPressed);
            setText(km_lbl_keys, num_buf);
            setText(km_lbl_profile, profileNamesCN[currentProfile]);
            break;
        }

        case DISP_MODE_RHYTHM: {
            // 柱子动画不在这里跑 —— 它有自己的 16ms 快节拍 tickRhythm()。
            // 之前柱子也挂在下面这段 100ms 刷新里，一根柱子从 60 掉到 0 要 6 秒，
            // 看着就是"律动很慢"，其实是刷新率被 DYNAMIC_REFRESH_MS 锁死了。
            setText(rh_lbl_keys, num_buf);
            break;
        }

        case DISP_MODE_WALLPAPER: {
            setText(wp_lbl_time, time_buf);
            setText(wp_lbl_date, date_buf);
            break;
        }

        case DISP_MODE_HIGH_CONTRAST: {
            // 中段：时间 + 日期
            setText(hc_lbl_time, time_buf);
            setText(hc_lbl_date, date_buf);

            // 顶栏 3 颗 LED 圆点：亮 = 锁色 + 同色光晕，灭 = 透明空圈。
            for (int i = 0; i < 3; i++) {
                bool on = locks[i];
                lv_obj_set_style_bg_opa(hc_led_dot[i],
                    on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
                // 灭态时不显示阴影（光晕 = 亮态专属）
                lv_obj_set_style_shadow_opa(hc_led_dot[i],
                    on ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_border_color(hc_led_dot[i],
                    lv_color_hex(on ? CLR_STROKE : CLR_STROKE), LV_PART_MAIN);
            }
            // 顶栏右侧：方案图标 + 方案名（每次都刷，方案切换立刻反映）
            // 高对比度风格用 L 档(48px) 大图标，与 build 时一致
            lv_img_set_src(hc_img_profile, profile_icon_get(currentProfile, PROF_ICON_L));

            // 大红键名：双字号预创建，按字符数切换可见性。
            const char* keyText = lastKeyPressed;
            size_t keyLen = strlen(keyText);
            if (keyLen <= 4) {
                // 短键名：48 号
                lv_obj_clear_flag(hc_lbl_lastkey_48, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(hc_lbl_lastkey_28, LV_OBJ_FLAG_HIDDEN);
                setText(hc_lbl_lastkey_48, keyText);
            } else {
                // 长键名：28 号
                lv_obj_add_flag(hc_lbl_lastkey_48, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(hc_lbl_lastkey_28, LV_OBJ_FLAG_HIDDEN);
                setText(hc_lbl_lastkey_28, keyText);
            }

            // ---- 数据带已删除（KPM/TODAY/ACTIVE）腾出空间 ----
// 保留 KPM 计算供其他风格使用；不写入任何 lvgl 对象（指针为 nullptr）

            // 底部状态条：温度 / 湿度（带进度条）+ 字数（紧凑计数）
            //
            // shtAvailable 为 false（传感器没接上）时照旧显示 "--.-°C" / "--%"，
            // 跟屏保信息面板 buildSaverPanel() 的处理一致；进度条此时保持空槽，
            // 不拿 shtTemp 的初值 0 去画一根满格，骗用户以为读到的是 0%。
            static char tbuf[16], hbuf[16], charsBuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.1f°C", shtTemp);
            snprintf(hbuf, sizeof(hbuf), "%.0f%%", shtHumidity);
            setText(hc_lbl_strip_t_val, shtAvailable ? tbuf : "--.-°C");
            setText(hc_lbl_strip_h_val, shtAvailable ? hbuf : "--%");
            formatCountCompact(charsBuf, sizeof(charsBuf), totalKeyCount);
            setText(hc_lbl_strip_chars_val, charsBuf);

            // 进度条量程：
            //   温度 0..50°C —— 室内实际落在 15~35，这一档下 23°C 正好在中段，
            //     条的长度一眼能分辨"偏凉 / 舒服 / 偏热"。用 0..100 的话室内
            //     永远只有 1/4 长，条就废了。
            //   湿度 0..100% —— 传感器本来就是这个量程。
            // 两边都夹紧：温度偏移是 ±20°C，负温和超过 100% 的坏读数不能让条溢出槽外。
            float tpct = 0.0f, hpct = 0.0f;
            if (shtAvailable) {
                tpct = shtTemp / 50.0f;
                hpct = shtHumidity / 100.0f;
            }
            setBarFill(hc_bar_t_fill, 16, 199, tpct);
            setBarFill(hc_bar_h_fill, 16, 228, hpct);
            break;
        }
    }
    // 三层锁指示（顶部条圆点/外圈、INFO_PANEL 圆点/文字/胶囊）这一帧都写完了，
    // 锁脏位可以放掉。如果这里不 clear，下次 loop() 会再无条件跑一遍
    // updateDynamicElements()，白跑 time()/localtime()。
    lockStateDirty = false;
}

// ===========================
// 律动页动画（独立快节拍）
// ===========================
// 律动是全屏唯一的"动效"，必须跟得上手指。之前的实现把柱子的下落挂在
// updateDynamicElements() 里，而那个函数被 DYNAMIC_REFRESH_MS = 100ms 卡着
// （对时钟/温湿度是对的，对动画就是灾难）：一根柱子从峰值掉到基线要 6 秒，
// 打字的节奏全被抹平了，看上去就是"律动很慢"。
//
// 现在单独一个 16ms 节拍（~60fps），并且给一次击键同时点亮峰值柱和两侧邻柱，
// 打出连续波而不是一根孤柱。静止时 24 根柱子全部是 0，直接 return，
// 一个 LVGL 调用都不产生 —— 这是之前"节奏页卡死"的解药，不能丢。
#define RHYTHM_TICK_MS 16UL
// 柱高量程 0~25（对应像素高 4~104），每 tick 掉 1 → 峰值约 400ms 落回基线。
#define RHYTHM_DECAY 1

static void tickRhythm(void) {
    if (currentSysMode != SYS_MODE_NORMAL) return;
    if (currentDispMode != DISP_MODE_RHYTHM || rh_bg == nullptr) return;

    static unsigned long lastMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastMs < RHYTHM_TICK_MS) return;
    lastMs = nowMs;

    bool anyAlive = false;
    for (int i = 0; i < 24; i++) if (rhythmBars[i] > 0) { anyAlive = true; break; }
    if (!anyAlive) return;

    // 这里是全项目最热的一处：24 根柱子 × (尺寸 + 位置 + 底色) = 72 次写入。
    // setSizePos/setBgColor 会先读回比对，值没变就不写 —— 关键，别绕过它。
    for (int i = 0; i < 24; i++) {
        if (!rh_bars[i]) continue;
        uint8_t hgt = rhythmBars[i];
        if (hgt > RHYTHM_DECAY) rhythmBars[i] = hgt - RHYTHM_DECAY;
        else                     rhythmBars[i] = 0;

        uint8_t v = rhythmBars[i];
        int barH = 4 + v * 4;
        setSizePos(rh_bars[i], 6, barH, 2 + i * 10, 110 - barH);

        if (v == 0) {
            setBgColor(rh_bars[i], CLR_SURFACE_2);
        } else {
            // 颜色随高度在 青(0x22D3EE) → 紫(0xA78BFA) 之间线性插值
            uint16_t t = (uint16_t)(v * 255 / 25);
            uint8_t r = (uint8_t)(0x22 + (0xA7 - 0x22) * t / 255);
            uint8_t g = (uint8_t)(0xD3 + (0x8B - 0xD3) * t / 255);
            uint8_t b = (uint8_t)(0xEE + (0xFA - 0xEE) * t / 255);
            setBgColor(rh_bars[i], ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
}

// 击键激励：峰值柱拉满，左右各亮一档弱一点的，再远一点给一档余晖。
// 键盘在屏幕上扫出一段波，比孤零零一根柱子有"律动"的意思。
static void pulseRhythm(void) {
    int idx = (int)(totalKeyCount % 24);
    uint8_t peak = 21 + (uint8_t)(esp_random() % 5);   // 21~25
    rhythmBars[idx] = peak;
    if (rhythmBars[(idx + 1) % 24] < 14) rhythmBars[(idx + 1) % 24] = 14;
    if (rhythmBars[(idx + 23) % 24] < 14) rhythmBars[(idx + 23) % 24] = 14;
    if (rhythmBars[(idx + 2) % 24] < 8)  rhythmBars[(idx + 2) % 24] = 8;
    if (rhythmBars[(idx + 22) % 24] < 8)  rhythmBars[(idx + 22) % 24] = 8;
    if (rhythmBars[(idx + 3) % 24] < 4)  rhythmBars[(idx + 3) % 24] = 4;
    if (rhythmBars[(idx + 21) % 24] < 4) rhythmBars[(idx + 21) % 24] = 4;
}

// ===========================
// 主屏渲染（预创建模式）
// ===========================
// 重建主屏内容（**不**切屏）
// ===========================
// 故意不在这里切屏：这个函数有两类调用方 ——
//   1) "我就是要回主屏"（ESC 退出、设置保存/取消、录制结束、开机、串口切风格）
//      → 由调用方显式 showScreen(ensureMainScreen())
//   2) "我只是换一种主屏风格，用户还停在菜单里"（菜单里的"切换主屏风格"）
//      → 保持在菜单，切屏会把用户从菜单里踢出去
// 之前这里无条件 showScreen(scr_main)，第 2 类调用方会被误踢出菜单。
static void renderCurrentDisplayBase(void) {
    ct_mark(CT_S_REBUILD);
    init_styles();
    ensureMainScreen();

    // 删除旧显示组件
    if (gk_bg) { lv_obj_del(gk_bg); gk_bg = nullptr; }
    if (bc_bg) { lv_obj_del(bc_bg); bc_bg = nullptr; }
    if (ip_bg) { lv_obj_del(ip_bg); ip_bg = nullptr; }
    if (km_bg) { lv_obj_del(km_bg); km_bg = nullptr; }
    if (rh_bg) { lv_obj_del(rh_bg); rh_bg = nullptr; }
    if (wp_bg) { lv_obj_del(wp_bg); wp_bg = nullptr; }
    if (hc_bg) { lv_obj_del(hc_bg); hc_bg = nullptr; }
    if (pk_bg) { lv_obj_del(pk_bg); pk_bg = nullptr; }

    // **必须清掉子对象指针，否则就是野指针**
    // 上面的 lv_obj_del(ip_bg) 会连同 card / 三颗锁灯胶囊 / 所有 label 一起释放，
    // 但 LVGL 不会替我们把外部变量置空。之前这里只把 ip_bg 写成 nullptr，
    // ip_lbl_clock / ipLockDot[] / ipLockLbl[] / ipLockChip[] 全留在已释放的地址上。
    // 切走后的下一轮 loop，updateDynamicElements() 的 INFO_PANEL 分支照样每帧去写
    // 这些指针 —— setText/setBgColor 里的 nullptr 检查全部失效，直接踩坏堆 → 崩溃重启。
    // 这就是"切到信息面板就崩"的真凶：崩在切换之后，而不是切的那一瞬间。
    resetStylePointers();

    // ------------------------------------------------------------------
    // 诊断：按位掩码控制启用哪些主屏风格
    // ------------------------------------------------------------------
    // 现象：六种风格全开时进主屏整机卡死、按键全部失灵；全关时一切正常。
    // 这里只跳过"建内容"这一步，主屏仍然是 ensureMainScreen() 那块合法屏幕，
    // 所以切屏、息屏、菜单、设置、录制、按键扫描等外围逻辑都保持原样。
    //
    // 被关掉的风格，其 gk_/bc_/ip_/km_/rh_/wp_ 指针全是 nullptr，而
    // updateDynamicElements() 里对这些指针的写入全部经过 setText/setBgColor/
    // setTextColor/setSizePos，这四个 setter 第一行就是 if (o == nullptr) return。
    // 顶部条 topLockDot[] 同理（已并入 resetStylePointers）。所以那几种风格是空屏，
    // 键盘照常响应。
    //
    // 现在默认是 STYLE_BIT_ALL（六种全开）。真要临时关掉某一种排查问题，
    // 把对应的位从 MAIN_STYLE_MASK 里去掉即可。
    uint32_t modeBit = 1u << (currentDispMode & 0x1F);
    if ((MAIN_STYLE_MASK & modeBit) == 0) { mainContentValid = true; return; }

    switch (currentDispMode) {
        case DISP_MODE_GEEK: build_style_geek(); break;
        case DISP_MODE_BIG_CLOCK: build_style_bigclock(); break;
        case DISP_MODE_INFO_PANEL: build_style_info_panel(); break;
        case DISP_MODE_KEY_MON: build_style_keymon(); break;
        case DISP_MODE_RHYTHM: build_style_rhythm(); break;
        case DISP_MODE_WALLPAPER: build_style_wallpaper(); break;
        case DISP_MODE_HIGH_CONTRAST: build_style_high_contrast(); break;
        case DISP_MODE_PLAN_KEY:      build_style_plankey(); break;
    }
    mainContentValid = true;
}

// ===========================
// 开机诊断：把"为什么重启"直接打在屏幕上
// ===========================
// 之前遇到"用一段时间自己重启"只能靠猜：芯片复位后 USB 会重新枚举，
// Windows 侧的串口句柄随之失效，PC 上抓不到 panic 回溯（这是工具限制，不是固件问题）。
// 所以这里把 esp_reset_reason() 做成开机 HUD：真再重启时一眼就能分清是
//   掉电/上电、brownout 掉电压、任务看门狗超时、还是 panic（空指针/断言/内存耗尽）。
// 另外把 PSRAM 实际大小和内部堆余量也报出来 —— 内存耗尽正是之前的重启主因之一。
static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "上电启动";
        case ESP_RST_EXT:        return "外部复位";
        case ESP_RST_SW:         return "软件重启";
        case ESP_RST_PANIC:      return "异常崩溃";
        case ESP_RST_INT_WDT:    return "中断看门狗";
        case ESP_RST_TASK_WDT:   return "任务看门狗";
        case ESP_RST_WDT:        return "其它看门狗";
        case ESP_RST_DEEPSLEEP:  return "深度睡眠唤醒";
        case ESP_RST_BROWNOUT:   return "电压掉电";
        case ESP_RST_SDIO:       return "SDIO 复位";
        default:                 return "未知原因";
    }
}

// 把"上次跑了多久"压成一句短话。
// 开机 HUD 的正文只有 184px 宽、16px 汉字，最多塞得下 11 个字，所以不能写
// "运行 1 小时 23 分 45 秒"这种 —— 塞不下就被 LVGL 的 DOT 截成省略号，
// 最关键的那几个字反而没了。
static const char* uptimeText(uint32_t ms) {
    static char buf[12];
    uint32_t s = ms / 1000UL;
    if (s < 60UL)         snprintf(buf, sizeof(buf), "%u秒", (unsigned)s);
    else if (s < 3600UL)  snprintf(buf, sizeof(buf), "%u分", (unsigned)(s / 60UL));
    else                  snprintf(buf, sizeof(buf), "%u时", (unsigned)(s / 3600UL));
    return buf;
}

// 崩溃现场：把 RTC 慢存里留的阶段号翻成中文
static const char* ctStageName(uint32_t s) {
    switch (s) {
        case CT_S_IDLE:         return "循环末尾";
        case CT_S_LOOP:         return "主循环开头";
        case CT_S_BLE_DELAY:    return "蓝牙重连延时";
        case CT_S_PING:         return "串口心跳";
        case CT_S_SHT:          return "温湿度读取";
        case CT_S_SCAN:         return "键盘扫描";
        case CT_S_SCAN_I2C:     return "总线探测";
        case CT_S_SCAN_RECOVER: return "总线恢复";
        case CT_S_SCAN_KEY:     return "按键分发";
        case CT_S_LED:          return "灯效引擎";
        case CT_S_SLEEP:        return "自动息屏";
        case CT_S_MENU:         return "菜单重建";
        case CT_S_DYNAMIC:      return "动态刷新";
        case CT_S_HUD:          return "提示框显隐";
        case CT_S_NOTIF:        return "通知队列";
        case CT_S_LVGL:         return "屏幕刷屏";
        case CT_S_REBUILD:      return "主屏重建";
        case CT_S_HANG:         return "心跳中断";
        case CT_S_CFG_POST:     return "配置生效中";
        case CT_S_WP_RX:        return "壁纸接收中";
        case CT_S_WP_DECODE:    return "壁纸解码写盘";
        case CT_S_CFG_RX:       return "配置收包";
        case CT_S_CFG_APPLY:    return "配置落盘";
        default:                return "未知阶段";
    }
}

// 第二张 HUD（内存详情）延迟 1.4s 弹出，让第一张"重启原因"先被看到
// 开机诊断 HUD 队列：三张依次弹出（原因 → 内存 → 上次现场），
// 后面接的是两张**硬件自检**（键盘扩展 / C3 串口）。
// 为什么硬件自检必须走屏幕而不是串口：这块板子没有 CDC（日志只能进 PSRAM 缓冲，
// 要靠网页 LOG:dump 才拿得到），而"键盘全哑"的用户恰恰**没有键盘**去翻菜单 ——
// 这正是这类故障最难查的地方：唯一的证据通道被故障本身堵死了。
#define BOOT_HUD_MAX 6
struct BootHudItem {
    unsigned long atMs;
    char title[16];
    char value[40];
    uint32_t color;
};
static BootHudItem bootHud[BOOT_HUD_MAX];
static int bootHudCount = 0;
static int bootHudNext = 0;

static void pushBootHud(unsigned long delayMs, const char* title,
                        const char* value, uint32_t color) {
    if (bootHudCount >= BOOT_HUD_MAX) return;
    BootHudItem& it = bootHud[bootHudCount++];
    it.atMs = millis() + delayMs;
    snprintf(it.title, sizeof(it.title), "%s", title);
    snprintf(it.value, sizeof(it.value), "%s", value);
    it.color = color;
}

void bootDetailTick(void) {
    if (bootHudNext >= bootHudCount) return;
    BootHudItem& it = bootHud[bootHudNext];
    if ((long)(millis() - it.atMs) < 0) return;
    bootHudNext++;
    // 六张诊断卡：红色（出错）和琥珀色（警告）走大提示，绿/青（正常）走小提示。
    // 理由和别处一样 —— 正常状态没必要占屏，但"上次异常复位"必须一眼看见。
    lv_color_t c = lv_color_hex(it.color);
    if (hudUrgent(c)) triggerHudBig(it.title, it.value, c);
    else             triggerHud(it.title, it.value, c);
}

static void showBootDiagnostics(void) {
    esp_reset_reason_t why = esp_reset_reason();
    // PSRAM 容量用 heap_caps 查：这套 Arduino 工具链（IDF 4.4）里没有 esp_psram.h
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t heapFree = esp_get_free_heap_size();
    uint32_t heapMin = esp_get_minimum_free_heap_size();

    // 累计开机次数，用来区分"断电重上电"和"跑着跑着自己重启"。
    // NVS 自带磨损均衡，开机写一次不会伤 flash。
    uint32_t boots = preferences.getUInt("boot_cnt", 0) + 1;
    preferences.putUInt("boot_cnt", boots);

    LOG_PORT.printf("[BOOT] reason=%d (%s) psram=%u heap=%u minHeap=%u boots=%u\n",
                  (int)why, resetReasonName(why), (unsigned)psram,
                  (unsigned)heapFree, (unsigned)heapMin, (unsigned)boots);

    // 第一张：重启原因。非上电的用琥珀色，一眼能看出"这次不是正常启动"
    bool abnormal = (why != ESP_RST_POWERON && why != ESP_RST_DEEPSLEEP);
    lv_color_t bootClr = lv_color_hex(abnormal ? CLR_AMBER : CLR_GREEN);
    if (abnormal) triggerHudBig("重启原因", resetReasonName(why), bootClr);
    else          triggerHud("开机", resetReasonName(why), bootClr);

    // 第二张：PSRAM、内部堆余量，以及**主任务栈的最小剩余**。
    //
    // 栈这一项是专门为"连蓝牙就崩 / 清空方案就崩"加的：这两条路都走
    // handleCommand() → cfgBuildJson / cfgApplyDoc，而 handleCommand 本身是个几百个
    // else-if 的巨函数，局部 String / ArduinoJson 临时对象把栈帧撑得很大。
    // loopTask 默认只有 8KB，越界就是 panic；更麻烦的是 panic handler 自己也跑在
    // 溢出的栈上 → 二次异常 → 现场记录写不进去（"上次异常复位但没留下现场"）。
    //
    // uxTaskGetStackHighWaterMark 返回**字节**（ESP-IDF 改过这个 API，原版
    // FreeRTOS 返回的是"字数"：in bytes not words, unlike vanilla FreeRTOS）。
    // 之前这里多乘了 4，报出 21856B —— 比 loopTask 整个栈（8192B）还大，
    // 是个自相矛盾的数字。真实值 5464B。低于 1024 就说明真的贴着边了。
    uint32_t stackFreeBytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    char buf[40];
    if (psram > 0) {
        snprintf(buf, sizeof(buf), "余%uK 栈%uK 第%u次",
                 (unsigned)(heapFree / 1024), (unsigned)(stackFreeBytes / 1024), (unsigned)boots);
    } else {
        // PSRAM 没起来时 LVGL 对象池回落内部堆，余量会小很多
        snprintf(buf, sizeof(buf), "无PSRAM 余%uK 栈%uK",
                 (unsigned)(heapFree / 1024), (unsigned)(stackFreeBytes / 1024));
    }
    pushBootHud(1400, "内存", buf, CLR_ACCENT);
    // 同一份数字也落进错误日志：开机 HUD 一闪而过，日志是能翻回来查的。
    // 正文必须**一行放得下**（见 elog 行那段的约束：可用宽度约 166px）。
    ELINFO("MEM", "栈%uB 内%uK", (unsigned)stackFreeBytes, (unsigned)(heapFree / 1024));
    if (stackFreeBytes < 1024) {
        ELERR("MEM", "栈只剩%uB 危险", (unsigned)stackFreeBytes);
    }

    // 第三张：上次运行是怎么结束的。
    // 关键在于 panic 之后 USB 会重新枚举，PC 侧串口句柄失效，崩溃回溯抓不到，
    // 所以现场必须留在芯片里（RTC 慢存，见 crash_trace.h），这里回放出来。
    if (ct_prev.magic == CT_MAGIC) {
        if (ct_prev.ovf) {
            // 栈溢出**优先于**阶段名显示。它是 FreeRTOS 金丝雀亲自抓到的，
            // 比阶段号更接近"到底哪一步炸的"：阶段号只说明"崩在配置落盘
            // 这一大段里"，而这条直接点名是哪个任务把栈踩烂的 —— 如果是
            // loopTask，下一步就是砍 handleCommand 的栈帧；如果是别的任务，
            // 那我前面盯着的方向（配置下发路径）就整个是错的。
            snprintf(buf, sizeof(buf), "%s 余%uB %s",
                     ct_prev.ovfTask[0] ? ct_prev.ovfTask : "?",
                     (unsigned)ct_prev.ovfStack, ctStageName(ct_prev.stage));
            pushBootHud(2900, "上次栈溢出", buf, CLR_RED);
        } else if (ct_prev.ready == 0) {
            // 上一次没跑完 setup 就没了 —— 启动流程里就崩了，跟运行期是两回事
            snprintf(buf, sizeof(buf), "启动未完成 %s", ctStageName(ct_prev.stage));
            pushBootHud(2900, "上次死在", buf, CLR_RED);
        } else {
            // 正文：阶段 + "上次跑了多久"。
            //
            // "多久"是拆掉看门狗之后补回来的信息 —— 从前固件自己重启，重启
            // 原因只有"任务看门狗"四个字，什么也说明不了；现在固件不自己重启，
            // 这一条就能直接回答"是跑了很久才崩，还是刚起来就崩"：
            // 前者多半是某个具体功能触发，后者多半是启动期就坏了。
            //
            // 最后按的那个键不进这张卡（宽度塞不下），它在错误日志里。
            snprintf(buf, sizeof(buf), "%s %s",
                     ctStageName(ct_prev.stage), uptimeText(ct_prev.uptimeMs));
            pushBootHud(2900, "上次死在", buf, CLR_RED);
        }
    } else {
        // 没有记录 = 断电重启（RTC 慢存会掉电清零），不是软件崩溃
        snprintf(buf, sizeof(buf), "断电或首次烧录");
        pushBootHud(2900, "上次", buf, CLR_TEXT_DIM);
    }

    // 第四张：键盘扩展（MCP23017）硬件自检。判据见 mcpResetWireSelfTest。
    //
    // 这一张是"键盘全哑"时**唯一还能用**的证据：不依赖按键，也不依赖串口。
    // 之前只有内存/重启原因这些跟故障本身无关的项，等于让一个失灵的用户
    // 去读一份没告诉他坏在哪的体检报告。
    {
        char w[40];
        uint32_t rc = mcpAutoRecoverCount();
        // 自动复位次数跟着这张卡一起报。必须**跨重启**才有用：遇到"键盘失灵"
        // 的用户十有八九已经重启过一次，只放内存里的话开机看到的就是 0，
        // 等于这一项永远在骗人。
        //
        // 只在 rc>0 时才追加：屏只有 240 宽，常态下多出来的字纯属占地方，
        // 而"发生过复位"这件事本身才是要报出来的信息。
        if (rc) snprintf(w, sizeof(w), "%s IO=%02X 复位%lu",
                         mcpWireVerdict(), g_mcpWire.iodira, (unsigned long)rc);
        else   snprintf(w, sizeof(w), "%s IO=%02X", mcpWireVerdict(), g_mcpWire.iodira);
        // 复位线本身合格、但发生过自动复位 → 琥珀色提醒：能自愈说明这机器有过
        // I2C 干扰，值得留意；线不合格 → 红色，那是要动烙铁的。
        bool wireGood = g_mcpWire.strobeHigh && !g_mcpWire.strobeLow;
        uint32_t clr = !wireGood ? CLR_RED : (rc ? CLR_AMBER : CLR_GREEN);
        pushBootHud(4400, "键盘扩展", w, clr);
        // 同一条也落进日志：开机 HUD 一闪而过，日志是能翻回来查的
        if (wireGood) ELINFO("I2C", "自检 %s IO=%02X 累计自动复位 %lu 次",
                             mcpWireVerdict(), g_mcpWire.iodira, (unsigned long)rc);
        else         ELERR ("I2C", "自检 %s IO=%02X 累计自动复位 %lu 次",
                             mcpWireVerdict(), g_mcpWire.iodira, (unsigned long)rc);
    }

    // 第五张：C3 串口（灯效 / 灯光键 / 旋钮都走这条线）。
    // 故意排在 8s 而不是紧跟 setup 尾巴：PONG 和心跳要等 C3 先被问才来，
    // 刚开机就报"0 字节"，那条信息什么也证明不了，只会误导人去查 UART 接线。
    {
        char w[40];
        snprintf(w, sizeof(w), "%lu字节 %s",
                 (unsigned long)c3RxBytes, c3Connected ? "已握手" : "未握手");
        pushBootHud(8000, "C3 串口", w, c3RxBytes ? CLR_GREEN : CLR_AMBER);
    }

    // ⚠ ovf 这一项**必须**打出来。阶段号只说"崩在主循环末尾"，
    //   而 ovf 说的是"**哪个任务**把栈踩烂了、还剩多少字节" ——
    //   这是 FreeRTOS 金丝雀亲自抓到的硬证据，比阶段号硬得多。
    //   （以前这一项只进了开机 HUD，日志行里没有，于是每次都要人去盯屏幕。）
    LOG_PORT.printf("[BOOT] prev: hang=%u stage=%u(%s) key=%s uptimeMs=%u ovf=%u task=%s stack=%u\n",
                  (unsigned)ct_prev.hang, (unsigned)ct_prev.stage,
                  ctStageName(ct_prev.stage),
                  (ct_prev.keyCode == 0xFFFFFFFFUL) ? "none" : getKeyName((uint16_t)ct_prev.keyCode),
                  (unsigned)ct_prev.uptimeMs,
                  (unsigned)ct_prev.ovf,
                  ct_prev.ovfTask[0] ? ct_prev.ovfTask : "-",
                  (unsigned)ct_prev.ovfStack);
}

// 把"上一次是怎么结束的"补写成一条**持久**日志。
//
// 为什么必须在这里（也就是每次开机）做，而不是出错的那一刻：
//   panic / 看门狗复位的那一刻，芯片已经直接重启了，flash 写不进去
//   （而且 panic 里碰文件系统只会让复位更慢、风险更大）。
//   真正能活过复位的只有 crash_trace.h 的 RTC 慢存 —— 所以开机后
//   先把那份现场读出来，落成一条普通错误日志。
//   这样"昨晚自己重启了"这件事，重启后在菜单里就能翻到完整描述，
//   而不只是开机时一闪而过的那三张 HUD。
static void elog_notePrevRun(void) {
    esp_reset_reason_t why = esp_reset_reason();

    // 断电重启 / 主动重启不记：那是正常行为，记一堆"启动"条目只会把
    // 真正有用的错误挤掉（64 条槽位很宝贵）。
    bool abnormal = (why != ESP_RST_POWERON && why != ESP_RST_DEEPSLEEP
                     && why != ESP_RST_SW && why != ESP_RST_EXT);

    // 本次启动事件。异常复位时降成警告级，正常启动只记一条信息级。
    // 反复重启（跑不起来 → 反复重启）会连续产生多条"上次异常退出"，
    // 靠 elog_add 的去重合并成一条并累加次数，一眼能看出"崩了 N 次"。
    char boot[80];
    snprintf(boot, sizeof(boot), "启动 原因:%s", resetReasonName(why));
    if (abnormal) ELWARN("BOOT", "%s", boot);
    else         ELINFO("BOOT", "%s", boot);

    if (!abnormal) return;

    if (ct_prev.magic == CT_MAGIC) {
        const char* keyName = (ct_prev.keyCode == 0xFFFFFFFFUL)
                              ? "无" : getKeyName((uint16_t)ct_prev.keyCode);
        char msg[ELOG_MSG_MAX];
        if (ct_prev.ovf) {
            // 栈溢出单独一条、且排在最前面：这是 FreeRTOS 金丝雀直接抓到的，
            // 比阶段号更硬。阶段号只说"崩在配置落盘这一段"，这条点名**是哪个
            // 任务**把栈踩烂的 —— 排查方向完全由它决定。
            snprintf(msg, sizeof(msg), "栈溢出 %s 余%uB 于%s",
                     ct_prev.ovfTask[0] ? ct_prev.ovfTask : "?",
                     (unsigned)ct_prev.ovfStack, ctStageName(ct_prev.stage));
        } else if (ct_prev.ready == 0) {
            // 上一次连 setup 都没跑完 —— 死在启动流程里（多半是硬件/配置问题）
            snprintf(msg, sizeof(msg), "上次死在启动中 阶段:%s", ctStageName(ct_prev.stage));
        } else {
            snprintf(msg, sizeof(msg), "上次崩溃于%s 运行%u秒 按键:%s",
                     ctStageName(ct_prev.stage),
                     (unsigned)(ct_prev.uptimeMs / 1000UL), keyName);
        }
        // 崩溃是"用不了了"，错误级。
        ELERR("CRASH", "%s", msg);
    } else {
        // RTC 慢存没有现场：说明复位发生在 ct_begin() 之前，或者掉电时
        // 慢存已经掉电清零。能确定是异常复位但拿不到阶段，如实这么记。
        ELERR("CRASH", "上次异常复位 但没留下现场记录");
    }
}

// ===========================
// setup()
// ===========================
void setup() {
    // ⚠ 解除 GPIO0(BOOT) 的 IO hold —— 必须放在**最前面**。
    //
    // BOOTDL 会用 gpio_hold_en() 把 IO0 按住在低电平（这是它能跨复位
    // 保持"进下载模式"的关键）。但那个 hold **只有掉电才会消失**，
    // 于是"进过下载模式"之后，每一次复位都会重新落回下载模式 ——
    // 表现是"固件明明烧好了，插上还是进 ROM，app 起不来"。
    //
    // 所以每次开机第一件事就是把 hold 放开：让 IO0 回到正常状态，
    // 由物理 BOOT 键 / 外部电路决定下次是进 app 还是进下载模式。
    gpio_hold_dis(GPIO_NUM_0);

    // 崩溃现场记录：必须**第一时间**初始化。setup() 里后面任何一步 panic，
    // 现场也已经留在 RTC 慢存里了，下次开机能显示出来。
    ct_begin();

    LOG_PORT.begin(115200);
    delay(500);
    Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

    #if LOG_CDC_MIRROR
    // USB CDC 日志镜像：把 LOG_PORT 的内容再送一份到 USB。
    // 真正的 write 放在独立任务里 —— HWCDC::write 在缓冲满时会阻塞 100ms，
    // 放在主循环里等于自己把"键盘卡死"这个 bug 又请回来。
    //
    // ⚠ 默认关着（LOG_CDC_MIRROR=0）：当前配置下 Serial 是 UART0 而 USB 被
    //   TinyUSB HID 占着，开着只会让 UART0 的日志重复一遍、还多一个任务
    //   和主任务抢同一个串口。要用见文件头那段说明。
    if (s_logCdcQueue == nullptr) {
        s_logCdcQueue = xQueueCreate(LOG_CDC_BLOCKS, LOG_CDC_BLOCK);
        if (s_logCdcQueue) {
            xTaskCreate(logCdcTask, "log_cdc", 3072, nullptr, 1, nullptr);
            LOG_PORT.println("[LOG] USB CDC 日志镜像已开");
        }
    }
    // ⚠ 把 **IDF 自己的** panic / assert 输出也留下来。
    //
    // 为什么必须开：panic 的回溯（哪个函数、哪一行）是崩溃定位的**硬证据**，
    // 而它默认只走 UART0（CONFIG_ESP_CONSOLE_UART_DEFAULT）——
    // 没接 USB-TTL 就一个字都看不到。
    //
    // ⚠⚠ 但**不能**用 Serial.setDebugOutput(true)：那是把 ROM 的 putchar
    //   直接接到 TinyUSB 的 CDC TX FIFO，而那个 FIFO 不是线程安全的 ——
    //   蓝牙协议栈任务的 log_d() 和主循环的日志镜像任务会并发写它，
    //   实测会把 CDC 通道整个写哑，并让设备在传输途中随机重启。
    //   （2026-10-23 实测：开了它之后 CDC 一个字节都收不到。）
    //
    // ✅ 改用"先进内存环、再由日志镜像任务统一搬进 CDC"，
    //   并在 panic 钩子里把环的尾巴抄进 RTC 慢存 —— 下次开机直接打印。
    panicTraceInit();
    LOG_PORT.println("[LOG] ROM 输出已接管（进内存环），panic 现场会存进 RTC 慢存");
#endif

    // 早分配 PSRAM 日志环形缓冲:越早越好,从这里开始的所有 LOG_PORT.printf
    // 都能进缓冲,后面网页发 LOG:dump 能一次性拉到。失败降级到不缓冲
    // (Serial0 仍正常,只是网页看不到日志而已,不至于死锁)
    if (logRingBuf == nullptr) {
        logRingBuf = (char*)ps_malloc(LOG_BUF_CAP);
        if (logRingBuf) {
            memset(logRingBuf, 0, LOG_BUF_CAP);
            // 互斥锁：主循环和蓝牙协议栈任务可能同时写这个环缓冲
            // （见 s_logMutex 的说明）。拿不到锁就丢日志行，绝不阻塞蓝牙任务。
            if (s_logMutex == nullptr) {
                s_logMutex = xSemaphoreCreateMutex();
            }
            // 回拉快照：LOG:dump 期间环形缓冲还会被新日志改写，不冻结一份
            // 读出来的行是错位的。放 PSRAM，别占内部 DRAM。
            logDumpSnap = (char*)ps_malloc(LOG_BUF_CAP);
            if (logDumpSnap) memset(logDumpSnap, 0, LOG_BUF_CAP);
            LOG_PORT.println("[LOG] ring buffer ready (PSRAM 8KB)");
        } else {
            LOG_PORT.println("[LOG] ps_malloc failed, BLE log disabled");
        }
    }

    // ⚠⚠ **上次 panic 的现场必须打在这里，不能更早**：
    //   上面那个环形缓冲是 BLE 日志（LOG:dump）唯一的存放处，
    //   在它分配之前打的日志 LOG:dump 根本拉不到 ——
    //   于是"崩溃现场明明打印了却看不见"，白折腾一轮。
    //   （2026-10-23 踩过：黑匣子第一次就把这段放在缓冲分配之前，
    //     结果现场只进了 UART0 和 CDC，开机日志里一个字都没有。）
    #if LOG_CDC_MIRROR
    if (panicTraceHas()) {
        LOG_PORT.printf("[PANIC] ===== 上次 panic 现场（%u 字节）=====\n",
                        (unsigned)panicTraceLen());
        LOG_PORT.println(panicTraceText());
        LOG_PORT.println("[PANIC] ==========================================");
        panicTraceClear();
    }
    #endif

    // 内部 DRAM 的剩余量也报一下：BLE 库每个写事件都要 new 一份 std::string，
    // 万一那里漏了，现象就是"传到第 N 片突然 panic"—— 内部堆耗尽也会这么表现。
    LOG_PORT.printf("[MEM] 内部堆剩 %u 字节（最小 %u），PSRAM 剩 %u 字节\n",
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    setenv("TZ", "CST-8", 1);
    tzset();

    // USB HID
    USB.VID(0x303A);
    USB.PID(0x001F);
    USB.productName("YYQ-MX9.0");
    USB.manufacturerName("YYQ");
    Keyboard.onEvent(usbHidKeyboardEvent);
    Keyboard.begin();
    ConsumerControl.begin();
    SystemControl.begin();
    // 厂商通道必须先注册回调再 begin()，否则主机发下来的报告没人收
    VendorHID.onEvent(onHidVendorEvent);
    VendorHID.begin();
    // USB.begin() 的返回值是 tinyusb_init() 成功与否（**不是**"主机有没有枚举"），
    // 所以在这里判不会误报。返回 false = USB 协议栈压根没起来，
    // 键盘完全不能用，但屏上照样正常显示主屏 —— 用户只觉得"键盘没反应"，
    // 完全查不到是哪一环出的问题。这条必须落到错误日志里。
    if (!USB.begin()) {
        ELERR("USB", "协议栈启动失败 主机识别不到键盘");
    }

    // SPI TFT
    tftSPI.begin(TFT_SCL, -1, TFT_SDA, TFT_CS);
    tft.init(240, 240);
    tft.setSPISpeed(40000000);
    tft.setRotation(1);
    // 面板 GRAM 上电是随机内容，从 tft.init() 到 LVGL 第一次 flush 之间
    // （还要走完 SPIFFS / 壁纸 / 键位表）有好几秒，这段时间用户看到的就是满屏
    // "雪花点"。先刷成黑底，接下来的启动延迟至少看起来像正常开机而不是坏了。
    tft.fillScreen(ST77XX_BLACK);

    // LVGL
    lvgl_driver_init(&tft);
    LOG_PORT.println("[LVGL] Ready");

    // NVS
    preferences.begin("keyboard", false);

    // 配置 JSON 的收发缓冲（各 CFG_BUF_SIZE+1 字节，落在 PSRAM）。
    // 在这里就分配掉，而不是等第一条 CFGGET —— 那条命令是跑在 loop() 里的，
    // 头一次 ps_malloc 会和 LVGL/BLE 抢一下堆，虽然不至于失败，但把不确定的
    // 因素从按键路径上拿走更划算。失败只记日志：真的用到时 cfgBufEnsure
    // 会再试一次并给网页回一条明确的 CFGERR。
    if (cfgBufEnsure()) {
        ELINFO("CFG", "配置缓冲已就绪 %d 字节 x2", (int)CFG_BUF_SIZE);
    }

    // 可写性自检：写一个探针键，再读回来比一遍。
    //
    // 为什么要探针 —— NVS 分区在默认 8MB 分区表里只有 0x5000 = 20KB，
    // 而且它是**只增不减**的日志结构存储：每次 put 都往后面追加一条新记录，
    // 旧的靠 GC 慢慢回收，闪存擦写次数多了就再也塞不进**新键**。
    // （已有键能就地覆盖，新键需要空槽 —— 这就是"单条规则存得下、两条存不下"
    //  这种不对称的原因：多出来的那条恰好要开一个新键。）
    //
    // Preferences::put* 失败时只返回 0，不抛异常，而全工程的调用点几乎都不看
    // 返回值。结果是屏幕照旧显示"已保存"、按键当场也生效（RAM 里有这张表），
    // 断电后 NVS 里空无一物 —— 典型的"配好了能用，断一次电全没了"。
    // 这里开机先探一次，把这件事变成开机日志里看得见的告警，
    // 而不是让用户拿断电去试。
    {
        const uint32_t kProbeVal = 0xA5C30001UL;
        bool probeOk = (preferences.putUInt("nvs_probe", kProbeVal) == 4) &&
                       (preferences.getUInt("nvs_probe", 0) == kProbeVal);
        preferences.remove("nvs_probe");
        if (probeOk) {
            ELINFO("NVS", "分区可写 配置能正常保存");
        } else {
            // 这里只记日志、不弹 HUD：setup 走到这一行时主屏还没建起来，
            // triggerHud() 会去 lv_scr_act() 上建对象，太早调用不安全。
            // 记成 ERR 就够醒目了 —— 菜单第 13 项会挂红色"N 条"角标。
            ELERR("NVS", "分区写不进去 配置可能保存不上");
        }

        // ⚠ 光有"能不能写"不够：这个探针只答一句话，分区到底还剩多少没人知道。
        // 而这一整类症状（"保存说成功、重启就没"、"清空方案看着成功、开机还是老那两条"）
        // 的真因都是分区被写满，且**越满越慢** —— GC 抖动起来一次保存能顶几秒，
        // 又正好会把 6 秒卡死监测顶爆重启。把数字打出来，以后这类问题一眼可辨。
        {
            nvs_stats_t st;
            if (nvs_get_stats("nvs", &st) == ESP_OK && st.total_entries > 0) {
                uint32_t freePct = (uint32_t)st.free_entries * 100UL / st.total_entries;
                ELINFO("NVS", "槽位 %u/%u 已用,剩余 %u%%（越低写入越慢,见 btLinkKeepAlive 注释）",
                       (unsigned)st.used_entries, (unsigned)st.total_entries,
                       (unsigned)freePct);
                if (freePct < 15) {
                    ELWARN("NVS", "分区快满了:新宏/新映射很可能存不进去,建议清掉一些不用的配置");
                }
            }
        }
    }

    // 恢复时间
    time_t savedEpoch = (time_t)preferences.getUInt("set_epoch", 0);
    epochCache = (uint32_t)savedEpoch;
    if (savedEpoch > 0) {
        time_t curEpoch = time(nullptr);
        struct tm* curTm = localtime(&curEpoch);
        if (curTm != nullptr && curTm->tm_year < 120) {
            struct timeval tv = { .tv_sec = savedEpoch, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
        }
    }

    currentDispMode = preferences.getUChar("disp_mode", DISP_MODE_GEEK);
    saverMode = preferences.getUChar("saver_mode", SAVER_OFF);
    // 老固件可能存过 4（SAVER_PET，小橘屏保）。该功能已移除、档位表缩到 4 项，
    // 不兜住就会拿 4 去索引 saverModeNames[] —— 越界读。
    if (saverMode >= TOTAL_SAVER_MODES) saverMode = SAVER_OFF;
    if (saverMode >= TOTAL_SAVER_MODES) saverMode = SAVER_OFF;
    currentProfile = preferences.getUChar("curr_prof", 0);
    // ⚠ 方案从 4 个砍到 2 个（Windows / macOS）之后，NVS 里很可能还躺着
    //   curr_prof = 2 或 3 —— 那是旧固件存进去的"方案 3 / 方案 4"。
    //   **必须钳位**，不然 currentProfile 会越界去索引 profileRemaps[2]，
    //   那是在 .bss 里读一个根本不存在的方案的映射表（拿到的是相邻内存，
    //   表现是"开机后按某颗键发出一条莫名其妙的键码"，极难查）。
    //   越界值一律归到 0（Windows），并喊一嗓子，别静悄悄地换掉用户的设置。
    if (currentProfile >= TOTAL_PROFILES) {
        ELWARN("CFG", "NVS 里的 curr_prof=%u 已超出方案数 %d,归到方案0",
               (unsigned)currentProfile, (int)TOTAL_PROFILES);
        currentProfile = 0;
    }
    totalKeyCount = preferences.getUInt("keyCount", 0);

    // 今日击键：先按 NVS 的 today_y 比对本地今天，若不同则视作跨日清零。
    todayKeyCount = preferences.getUInt("today_key", 0);
    todayDateYmd   = (int)preferences.getUInt("today_y", 0);
    {
        time_t nowT = time(nullptr);
        struct tm tmNow;
        if (todayDateYmd > 0 && localtime_r(&nowT, &tmNow)) {
            int ymd = (tmNow.tm_year + 1900) * 10000
                    + (tmNow.tm_mon + 1) * 100
                    + tmNow.tm_mday;
            if (ymd != todayDateYmd) {
                todayKeyCount = 0;
                todayDateYmd  = ymd;
                preferences.putUInt("today_key", 0);
                preferences.putUInt("today_y", (uint32_t)ymd);
            }
        }
    }

    // 灯光五项：与"灯光设置"页一一对应，回车保存时才写 NVS
    lightOn       = preferences.getBool("light_on", true);
    brightness    = preferences.getUChar("brightness", 140);
    currentEffect = preferences.getUChar("effect", 1);
    indLevel      = preferences.getUChar("ind_level", 3);
    keyFxStyle    = preferences.getUChar("key_fx", KEYFX_RIPPLE);
    if (currentEffect >= MAX_EFFECTS) currentEffect = 1;
    if (indLevel >= IND_LEVEL_COUNT) indLevel = 3;
    if (keyFxStyle >= KEYFX_COUNT) keyFxStyle = KEYFX_RIPPLE;
    indBrightness = indLevelValues[indLevel];

    shtTempOffset = preferences.getFloat("sht_offset", SHT_TEMP_OFFSET_DEFAULT);
    // 老固件把这里当成"内部基准"存，值是 62.0（那个值在校准页里怎么调都不生效）。
    // 新语义下 62.0 会被当成 "+62°C 的偏移"加进去，读数直接爆掉 —— 所以
    // 跳出 ±20 范围的历史值一律丢掉，回落到默认补偿。
    // 用户真正调过的值（±20 以内）不受影响，校准结果不会丢。
    if (shtTempOffset < SHT_TEMP_OFFSET_MIN || shtTempOffset > SHT_TEMP_OFFSET_MAX) {
        LOG_PORT.printf("[SHT] stored offset %.1f out of range, reset to %.1f\n",
                        shtTempOffset, SHT_TEMP_OFFSET_DEFAULT);
        ELWARN("NVS", "温度偏移 %0.1f 越界 已重置", (double)shtTempOffset);
        shtTempOffset = SHT_TEMP_OFFSET_DEFAULT;
    }

    // I2C / MCP23017
    // 碰 I2C **之前**先把芯片硬复位一遍，让它从确定的寄存器状态开始。
    // 理由和时序见 mcpPowerOnReset()：寄存器掉电易失，只靠 I2C 去写是建立在
    // "寄存器已经能被写进去"这个假设上的，而这个假设在芯片上电那一刻并不成立。
    mcpPowerOnReset();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    // ⚠ 这个超时以前只有 recoverI2CBus() 里有，主路径一直吃 Arduino 默认的
    // 50ms。总线一旦半死不活（从机拉着 SDA 不放），一轮扫描 18 次写每笔都拖
    // 满 50ms ≈ 0.9s，主循环直接卡死。这里必须从一开始就 armed —— 平时总线
    // 正常时它一次都不触发，完全不影响扫描速度。
    Wire.setTimeOut(25);
    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        LOG_PORT.println("[MCP23017] Init failed, recovering...");
        ELERR("I2C", "开机初始化失败 正在恢复");
        recoverI2CBus();
    }

    // SHT31
    Wire_SHT.begin(SHT31_SDA, SHT31_SCL);
    Wire_SHT.setClock(400000);
    Wire_SHT.beginTransmission(SHT31_ADDR);
    if (Wire_SHT.endTransmission() == 0) {
        shtAvailable = true;
        sht31_update();
        lastSHTRead = millis();
    } else {
        // 少了这颗传感器温度湿度一直是空的。以前这里一声不吭，
        // 用户只看到"温度不显示"，完全不知道是传感器没接上还是固件坏了。
        ELERR("SHT", "温湿度传感器无响应 温度不会更新");
    }

    // BLE
    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* pService = pServer->createService(SERVICE_UUID);
    // 必须带 NOTIFY：网页的"从键盘读取"按钮要靠它把映射规则回读上来
    // （REMAP:prof:read -> 固件 notify 一条 REMAPDUMP:...）。只给 WRITE 的话，
    // 键盘没有任何回话通道，网页上就永远是空的，用户也分不清"没保存"还是"读不出来"。
    pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID,
        // 同时支持 PROPERTY_WRITE 和 PROPERTY_WRITE_NR：
        //   - WRITE_NR 是 JPEG 流式分片的关键，没有它 180 字节一包的 ACK 往返
        //     会把上传拖到十几秒，主机侧 BLE supervision timeout 一到就被踢。
        //     详见 https://github.com/.../issues（GATT Server is disconnected）。
        //   - WRITE 留给文本命令（sendBLE 那条路径）继续走 await 同步，
        //     这样命令发送完真的意味着从机收到了，不会被静默丢包。
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->setCallbacks(new MyCallbacks());
    // 必须显式 addDescriptor(BLE2902),不然 CCCD 描述符不在 GATT 数据库里,
    // 客户端(Chrome Web Bluetooth)调 startNotifications() 时往 CCCD 写订阅位会
    // 直接返回 "GATT Error: Not supported",网页上 read-back 全超时 —— 表现就是
    // "切了键啥也不显示"。NimBLE 不会自动加 CCCD,得手动。
    pCharacteristic->addDescriptor(new BLE2902());
    pService->start();

    // 把特征和指令派发入口交给 bt_link，然后所有蓝牙的收和发都只经过它。
    //
    // 顺序有讲究：
    //   · 必须在 setCallbacks(new MyCallbacks()) 之后 —— 协议栈一起来就可能
    //     有包进来，btLinkOnWrite() 会判 s_queue == nullptr 直接丢，不排队。
    //   · 必须在 startAdvertising() 之前 —— 连上就可能立刻发第一条指令。
    btLinkInit(pCharacteristic, handleCommand);
    // 二进制旁路：壁纸 JPEG 是裸字节，走文本通道会被 0x00 截碎。
    btLinkSetBinaryIo(logoRxProbe, logoRxSink);

    // ★ 必须在 startAdvertising() **之前**挂上：只有挂上之后启动的那次广播，
    //   才会把 ESP_GAP_BLE_ADV_START_COMPLETE_EVT 送过来，
    //   g_advState 才是准的（loop() 里的"断链后重试广播"全靠它判断）。
    BLEDevice::setCustomGapHandler(myGapHandler);

    // 自己留一份广播参数：和 Arduino BLEAdvertising 的默认值**完全一致**
    // （见 BLEAdvertising.cpp 构造函数：adv_int_min=0x20 / adv_int_max=0x40 /
    //   ADV_TYPE_IND / PUBLIC / ADV_CHNL_ALL / ALLOW_SCAN_ANY_CON_ANY）。
    // 自己留着是为了重启广播时能直接调 IDF 接口、拿到真实错误码
    // （Arduino 那层失败是静默的，见 btAdvRestart 的注释）。
    s_advParams.adv_int_min       = 0x20;
    s_advParams.adv_int_max       = 0x40;
    s_advParams.adv_type          = ADV_TYPE_IND;
    s_advParams.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    s_advParams.channel_map       = ADV_CHNL_ALL;
    s_advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    s_advParams.peer_addr_type    = BLE_ADDR_TYPE_PUBLIC;

    // 第一次广播仍然走 Arduino 的封装：它顺带把广播数据（设备名 + service UUID）
    // 配好，之后 btAdvRestart() 复用这份数据、不再重复配，广播不会被重建。
    BLEDevice::startAdvertising();
    g_advState    = ADV_PENDING;   // 等 ADV_START_COMPLETE 确认
    advDeadlineMs = millis() + 3000;
    advRetryAtMs  = millis() + 3000;

    // 文件系统：必须用 SPIFFS，不能再用 FFat。
    //
    // 板子默认分区表（platformio.ini 没配 board_build.partitions，板定义给的是
    // default_8MB.csv；Arduino IDE 那边是 default.csv）里**只有 spiffs 分区，
    // 没有 ffat 分区**。FFat.begin() 会去 find 一个 subtype=fat 的分区，
    // 找不到就直接 return false（串口里那句 "No fat partition found on flash"），
    // 于是后面每一次 FFat.open() 都返回一个无效的 File：
    //   · ME 文本写不进 /me_hex.txt → 按 ME 键报"尚未设置"、网页下发后 HUD 报"存入失败"
    //   · 壁纸解出来的图写不进 /logo.bin → 当次能显示，一重启就没了
    // SPIFFS 挂的就是表里那个 spiffs 分区（1.5MB），够放 115KB 的 logo + 一行文本，
    // 而且不用改分区表 —— 改分区表会挪动 app 分区、有抹掉 NVS 里宏/映射的风险。
    // ⚠ begin(true) 的 true = 挂不上就格式化重来。**整块板只能调这一次** ——
    // 再调一次会把用户配置（壁纸 /logo.bin、ME 文本）和下面的错误日志一起抹掉。
    bool fsMounted = SPIFFS.begin(true);

    // 探一下 SPIFFS 的单文件上限（见 probeSpiffsLimit 的说明）。
    // 这个上限**不在代码里**，是编译期配置 + 文件系统格式决定的，
    // 不探一次很容易想当然地以为"115KB 也能写成一个文件"——而实际上写不进去，
    // 每次开机重写一次还会变成 boot loop。
    if (fsMounted) {
        probeSpiffsLimit();
        // ⚠ 顺手把**剩余空间**也报出来：壁纸 115200 字节要落盘，
        //   分区满不满、还剩多少，必须一眼看得见，别靠猜。
        LOG_PORT.printf("[SPIFFS] 总 %u 字节，已用 %u 字节，剩 %u 字节\n",
                        (unsigned)SPIFFS.totalBytes(),
                        (unsigned)SPIFFS.usedBytes(),
                        (unsigned)(SPIFFS.totalBytes() - SPIFFS.usedBytes()));
    }

    // 错误日志：把上次运行留在 flash 上的日志读回 RAM，然后补一条"上次怎么结束的"。
    // 必须放在 SPIFFS.begin() 之后 —— elog 靠 SPIFFS 存取。放在这里是因为
    // 上面 USB / LVGL / NVS / I2C 这些初始化步骤的报错也都想一起记下来。
    elog_begin();
    elog_notePrevRun();
    if (!fsMounted || SPIFFS.totalBytes() == 0) {
        // 文件系统没挂上：elog 会降级成"只留内存里的记录，重启就丢"。
        // 这本身就是要记的第一号故障，必须让用户在菜单里看得到。
        ELERR("FS", "存储未挂载 日志重启后会丢失");
    }

    // 开机就把"上次是怎么崩的"落盘，不等 elog_tick 的 60 秒。
    //
    // 为什么不能等：崩溃记录是在**本次**开机才生成的，如果开机后不到一分钟
    // 又崩了，那条记录会跟着 RAM 一起没了 —— 而"开机就崩"恰恰是最需要
    // 留证的场景（配置坏了、硬件接触不良，都是一上电就复现）。
    //
    // 磨损上限：只在**真的有错误级条目**时才提前落盘（正常开机不写），
    // 并且用 NVS 里的时间戳限流到 5 分钟一次。这样一个"反复崩溃重启"的
    // 设备（开机间隔远短于 5 分钟）也只会 5 分钟写一次，不会把 flash 写废。
    {
        bool hasErr = false;
        for (int i = 0; i < elog_count() && !hasErr; i++) {
            if (elog_get(i)->level == EL_LVL_ERR) hasErr = true;
        }
        uint32_t nowS = (uint32_t)time(nullptr);
        uint32_t lastS = preferences.getULong("elog_fs", 0);
        if (hasErr && fsMounted &&
            (nowS < lastS || (nowS - lastS) > 300UL)) {   // 300s = 5 分钟
            elog_flush();
            preferences.putULong("elog_fs", nowS);
        }
    }

    // 开机把上次传的壁纸从盘上读回 PSRAM，重启不用让网页再传一遍
    loadWallpaperFromDisk();

    // 键盘矩阵
    initBaseMatrix();
    loadRemapsFromStorage();
    mcpApplyColumnConfig();          // 顺序/理由见该函数注释（先电平后方向）
    for (int r = 0; r < NUM_ROWS; r++) {
        pinMode(rowPins[r], INPUT_PULLUP);
    }
    // 复位线自检必须放在列配置刷好之后、显示之前：它要读写 GPIO 寄存器才测得准，
    // 结果又要赶在 showBootDiagnostics() 之前落到屏幕上。
    mcpResetWireSelfTest();

    // ---- 看门狗：整体拆掉 ----
    //
    // 为什么要拆：看门狗一触发就是**芯片复位**，而复位原因只会留下"任务看门狗"
    // 四个既有的字，既说不出卡在哪一步、也留不下现场。板子已经把复位线引出来
    // 了（README 引脚表：MCP23017 的 RESET 脚 → GPIO11），真出问题时可以自己
    // 按复位重来 —— 那次重启是**有意为之**的，原因也就说得清。
    // 反过来，固件自己重启会把问题彻底掩盖："用着用着突然重启"根本无从查。
    //
    // 三步互相独立，任何一步失败都不影响"不再重启"这个结论：
    //   1) 退订自己：loopTask 当初是这里 esp_task_wdt_add(NULL) 订阅进去的，
    //      不摘掉的话它一旦卡住 10 秒照样 panic。
    //   2) 把 TWDT 改成"1 小时超时 + 不 panic"：还留在名单里的任务（比如 idle）
    //      再怎么卡也触发不了复位。
    //   3) 名单空了就把整个 TWDT 关掉（还有 idle 任务时返回错误，忽略即可）。
    //
    // ⚠ 第 1 步的返回值决定第 2、3 步要不要做：
    //   ESP_ERR_INVALID_STATE = TWDT 压根没被初始化过（这才是常态 —— 整个
    //   固件只有下面这一处初始化它）。这时候**不要去 init()**，那等于凭空
    //   造一个看门狗出来。ESP_ERR_INVALID_ARG = TWDT 在、但 loopTask 不在
    //   名单里，同样要接着中和。
    esp_err_t wdtSt = esp_task_wdt_delete(NULL);
    if (wdtSt != ESP_ERR_INVALID_STATE) {
        #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_task_wdt_config_t twdt = { .timeout_ms = WDT_DISARM_TIMEOUT_S * 1000,
            .idle_core_mask = 0, .trigger_panic = false };
        esp_task_wdt_reconfigure(&twdt);
        #else
        // 2.x（IDF 4.4）没有 reconfigure：init() 在已初始化时本身就是"改配置"。
        esp_task_wdt_init(WDT_DISARM_TIMEOUT_S, false);
        #endif
        // 名单上还有 idle 任务时 deinit 会返回 ESP_ERR_INVALID_STATE —— 无所谓，
        // 第 2 步已经把"触发即复位"这个可能堵死了。
        esp_task_wdt_deinit();
    }

    // 初始化显示
    renderCurrentDisplayBase();
    gotoMainScreen();
    lastActivityTime = millis();

    // 锁定状态是从 NVS 读回来的 —— 锁着的键盘**重启之后仍然是锁着的**。
    // 锁的意义就是"不让别人打字"，重启就自动解锁的话，拔电重插就能绕过，
    // 这锁形同虚设。解锁入口：Fn + LOGO，或网页/BLE 的 LOCK:0 指令。
    //
    // 用 lockApplyState 而不是 lockSetLocked：NVS 里本来就存着"锁着"，
    // 再写一遍是无意义的擦写（NVS 有磨损，写的时候还会阻塞几十毫秒）。
    if (preferences.getBool(NVS_KEY_LOCKED, false)) {
        lockApplyState(true);
        LOG_PORT.println("[LOCK] 开机恢复：键盘处于锁定状态（Fn+LOGO 解锁）");
    }

    showBootDiagnostics();
    ct_ready();

    LOG_PORT.println("YYQ-MX9.0 LVGL Ready");
}

// ===========================
// 栈溢出钩子（覆盖 FreeRTOS 的 weak 默认实现）
// ===========================
// 详细理由见 crash_trace.h 里同名函数的注释。这里只说为什么要放在这个位置：
// Arduino 会自动生成函数原型，并把原型插到**第一个函数定义之前**。这个文件
// 第一个函数定义出现得很早，而本钩子要用 TaskHandle_t / crash_trace.h 里的
// 东西，放前面会被生成器把原型甩到 `#include <Arduino.h>` 之前，直接编译不过
// （之前踩过这个坑，见 ctQueueRemapHud 的注释）。
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
    (void)xTask;
    // 先记现场再复位。ovf=1 **最后**写 —— 它是"这份记录本身可信"的标志位，
    // 万一中间就二次异常了，至少 magic 还是上一次的有效值。
    ct_rec.uptimeMs = (uint32_t)millis();
    ct_rec.ovfStack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ct_copyTaskName(ct_rec.ovfTask, sizeof(ct_rec.ovfTask), pcTaskName);
    ct_rec.ovf = 1;

    // 走原来的复位流程。abort() 会让 panic handler 打印 Guru Meditation 回溯；
    // 保留它是为了在**恰好有人盯着串口**时仍能拿到回溯。现在没接 CDC，这段
    // 输出无处可去，但不影响本钩子留记录的作用。
    abort();
}

// ===========================
// loop()
// ===========================
unsigned long lastScanTime = 0;
// 125Hz 扫描。机械键盘的按键去抖是 20ms，8ms 的轮询周期能给出 ~40Hz 的
// 有效去抖分辨率，手感上和原来 2ms 没有可感知差别。
//
// 为什么不能再用 2ms：改成单次寄存器写选列之后，一次完整扫描（16 列 ×
// 1 次 I2C 写 + 16×20µs 稳定等待）仍要 ~2.1ms。2ms 的调度周期等于要求
// CPU 100% 全程给 I2C，loop() 里排在后面的 LVGL 一帧都跑不到 —— 屏幕停更
// 看起来就是卡死。8ms 让扫描只占约 26% 的 CPU，剩下的留给显示和 BLE。
const unsigned long SCAN_INTERVAL = 8;

void loop() {
    ct_mark(CT_S_LOOP);

    // 开机诊断弹窗队列（重启原因 / 内存 / 上次现场）
    bootDetailTick();

    // 延迟重启
    if (pendingRestartMs != 0 && (long)(millis() - pendingRestartMs) >= 0) {
        pendingRestartMs = 0;
        // 主动重启前先把日志落盘：重启后 RAM 就没了，不落盘这一轮记的
        // 故障信息会跟着一起消失（而"重启"本身正是最需要留证的场景）。
        elog_flush();
        esp_restart();
    }

    // 错误日志落盘：到点且有值得记的变化时才真写 flash，详见 elog.h。
    // 放在这里（主任务、循环开头）而不是各报错点，是为了保证写 flash
    // 这件耗时的事不会插在 I2C 扫描这类高频路径中间。
    elog_tick();

    // 内存水位巡检。内部 DRAM 耗尽是这个项目历史上最主要的重启原因
    // （LVGL 曾经一次性从内部堆划 64KB，见 lv_mem_port.h），
    // 而它的表现就是"跑着跑着自己重启"—— 事后无从查起。
    // 这里按 15 秒一次的节拍看一眼，低于阈值就记一条；去重会把它
    // 合并成一条并累加次数，所以"一直低"也只占一行。
    {
        static unsigned long lastMemCheck = 0;
        if (millis() - lastMemCheck > 15000) {
            lastMemCheck = millis();
            uint32_t freeHeap = (uint32_t)esp_get_free_heap_size();
            if (freeHeap < 8192) {
                ELERR("MEM", "内部内存仅剩 %uK 随时可能重启", (unsigned)(freeHeap / 1024));
            } else if (freeHeap < 20480) {
                ELWARN("MEM", "内部内存偏低 剩 %uK", (unsigned)(freeHeap / 1024));
            }
        }
    }

    // 蓝牙链路：一次搞定四件事，全在主任务里。
    //   ① 排空发送队列（每轮限时 BT_POLL_BUDGET_MS，大回拉自动摊到后面几轮）
    //   ② 收行超时兜底
    //   ③ 派发主机指令 —— 解析和所有 lv_* 调用都在这一行之后，
    //      和键盘触发的路径同属一个任务，绝不跨任务碰 LVGL 堆
    //   ④ 喂狗（收包、发片、每派发一条指令各喂一次）
    btLinkPoll();

    // LOG:dump 的流式回拉：每轮喂一段，喂完就收手。
    // 为什么不在 LOG:dump 的处理分支里一口气发完：8KB 按 20 字节一片是
    // 400+ 片，同步推完会把主循环按住好几秒 —— 键盘全卡，而且 loop() 挂在
    // 任务看门狗上，再多几条就变成"回拉日志把键盘搞重启"。
    logDumpPump();

    // 配置 JSON 的回拉：同一个套路。CFGGET 只是开流 + 记下要发多少，
    // 真正的分片在 loop() 里按 btLinkStreamRoom() 的节奏喂出去，
    // 所以一条几百字节的 JSON 也不会把主循环按住。
    cfgJsonPump();

    // 配置 JSON 保存后的界面副作用：**一轮只做一件**。
    // 之前这些全屏重建挤在一条指令里连着跑，整块主循环被占住好几秒
    // （表现是"保存配置时键盘卡住"）。摊到多轮 loop 之后，每一轮全屏重建
    // 前后都有键盘扫描和 LVGL 刷新。
    cfgPostPump();

    // ME 文本往主机发送：每轮只发几个字符（见 meOutPump 上面那段说明）。
    // ★ 必须放在 loop() 里 —— 一把梭会把主循环按住 40 秒，
    //   屏幕不刷新、按键不响应、**蓝牙一条都不回**，用户看着就是"键盘崩了"。
    meOutPump();

    // 配置 JSON 的接收超时兜底：网页发到一半关掉页面/走出蓝牙范围时，
    // cfgRxActive 必须放掉 —— 不然下一段 CFGBEGIN 之前的 CFGDATA 会被
    // 追加到上一段残留后面，攒出一份谁也解析不了的 JSON。
    if (cfgRxActive && (unsigned long)(millis() - cfgRxLastMs) > CFG_RX_TIMEOUT_MS) {
        cfgRxActive = false;
        ELWARN("CFG", "接收超时 声明 %u 实收 %u，已丢弃",
               (unsigned)cfgRxTotal, (unsigned)cfgRxLen);
    }

    // 壁纸：收齐了就收尾（几百毫秒，放主任务不占 BLE 主机任务）。
    // 传一半断线（关网页、走出范围）必须超时放掉缓冲，否则后面所有文本
    // 指令都会被当成 JPEG 数据吃掉，键盘直接"失联"。
    //
    // RAW 和 JPEG 两条路在这里分流：RAW 的像素在电脑端已经解好了，
    // 板子只搬字节，不用 JPEGDEC。
    if (logoRxDone) {
        if (logoRxRaw) finishLogoRaw();
        else           finishLogoUpload();
    }
    if (logoRxActive && !logoRxDone && millis() - logoRxLastMs > LOGO_RX_TIMEOUT_MS) {
        abortLogoUpload("传输中断");
    }

    // ME 文本：和壁纸同一个道理。网页发到一半关掉页面/走出蓝牙范围时，
    // 那个 fd 不能一直开着 —— SPIFFS 默认 maxOpenFiles=10，漏几个之后
    // 连 /logo.bin 都打不开。超时就收尾关闭，已写进去的部分保留。
    if (meFile && millis() - meLastDataMs > ME_RX_TIMEOUT_MS) {
        meFile.close();
        meFile = File();
        LOG_PORT.printf("[ME] rx timeout, closed (partial %u bytes)\n",
                        (unsigned)meHexBytes);
        ELWARN("ME", "接收超时 只存下 %u 字节", (unsigned)meHexBytes);
        triggerHud("ME 文本", "传输超时", lv_color_hex(CLR_AMBER));
    }

    // USB HID 厂商通道走同一套纪律：USB 回调只往环形缓冲里塞字节，
    // 解析放在这里，和键盘触发的路径在同一个任务里碰 LVGL。
    handleHidVendorCommands();

    // C3 侧键 / 旋钮：Serial1 的行解析。放这里而不是中断里 ——
    // 灯光键确认通知要重画面板、旋钮要弹 HUD，都在主任务里做。
    handleC3Events();

    // UART0 维护指令通道（USB-TTL / COM 口）。维护动作（清配对、看广播状态、
    // 复位、进出下载模式）走串口，不依赖蓝牙链路 —— 蓝牙连不上的时候
    // 这条通道是唯一的救生索。
    handleUartEvents();

    // 连接状态归 bt_link 管（它要在断链时清掉排队中的响应），这里只盯"刚断开"
    // 这一个边沿来重启广播，让主机能重新扫到。
    //
    // ⚠⚠⚠ 以前这里是**只试一次**：断链 → 等 500ms → startAdvertising() → 就没了。
    //   而 startAdvertising() 内部的失败是**静默**的（见上面 myGapHandler 那段注释），
    //   于是那一次重试只要踩中失败路径就再也没人管 —— 设备从此不再广播，
    //   主机再也扫不到它，只能拔电重插才活。
    //
    //   这就是"蓝牙时好时坏"的根因：设备没崩、没重启、没死机，
    //   就是**扫不到**了，而且现场什么日志都没有。
    //
    // 现在改成持续看护，判据全部来自协议栈事件 + 连接状态：
    //   · PENDING 超过 2 秒没等到 ADV_START_COMPLETE → 当成静默失败，重来
    //   · 协议栈明确拒了（status≠SUCCESS）→ 退避后重来（1s → 3s）
    //   · **连接期间强制认为"没在广播"**（关键，见下面那段说明）
    //   · 断链后等 800ms 再重启，让控制器把上一段广播收干净
    if (btLinkConnected()) {
        // ★★★ 这里是"蓝牙时好时坏"的**真正根因** ★★★
        //
        // 主机一连上，控制器会**自动停广播**，但它**不会**发
        // ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT —— 那个事件只在"代码主动调
        // stop_advertising()"时才发。
        //
        // 所以"现在到底有没有在广播"**绝对不能只看协议栈事件**：
        // 连上一次之后，状态会永远停在"在广播"，断链后看护逻辑认为
        // "还在广播，不用管"，于是**永远不再重启广播** ——
        // 设备从此再也扫不到，只能拔电重插才活。
        //
        // 实测复现（2026-10-22，tools/ble_cycle.py + UART0 日志）：
        //   3345ms ESP_GATTS_CONNECT_EVT
        //   3569ms ESP_GATTS_DISCONNECT_EVT
        //   之后 **一次 esp_ble_gap_start_advertising 都没有**
        //   → 12 秒内 BLE 扫不到设备
        //
        // 连接期间直接把状态钉成 IDLE（"肯定没在广播"），
        // 断链后看护逻辑就一定会把广播拉起来。
        g_advState = ADV_IDLE;
        oldDeviceConnected = true;
    } else {
        if (oldDeviceConnected) {
            oldDeviceConnected = false;
            // 断链后先给控制器 800ms 收尾时间：太早调 esp_ble_gap_start_advertising
            // 会被控制器拒（"刚断链、上一段广播还没停干净"）。
            // IDF 4.4 的 GAP 事件里**没有**"控制器断链完成"这个信号
            // （只有 GATTS 的 ESP_GATTS_DISCONNECT_EVT），所以只能按时间退避。
            advRetryAtMs  = millis() + 800;
            advFailStreak = 0;
            ct_mark(CT_S_BLE_DELAY);
        }
        // ① 发起之后迟迟没有协议栈确认 → 判定为静默失败
        if (g_advState == ADV_PENDING && (long)(millis() - advDeadlineMs) >= 0) {
            g_advState = ADV_FAILED;
            LOG_PORT.println("[ADV] 等确认超时，按失败处理");
        }
        // ② stop 收尾完了 → 真正 start
        if (g_advState == ADV_STOPPING && (long)(millis() - advDeadlineMs) >= 0) {
            btAdvStart();
            advRetryAtMs = millis() + (advFailStreak < 3 ? 1000 : 3000);
        }
        // ③ 确定没在广播 → 先 stop 再 start（见 btAdvStop 的注释）
        else if ((g_advState == ADV_IDLE || g_advState == ADV_FAILED) &&
                 (long)(millis() - advRetryAtMs) >= 0) {
            btAdvStop();
            advRetryAtMs = millis() + (advFailStreak < 3 ? 1000 : 3000);
        }
    }

    // Ping
    if (millis() - lastPingTime > 2000) {
        lastPingTime = millis();
        uint8_t ping[] = { 0xAA, 0x55, 0x02, 0xEE };
        Serial1.write(ping, sizeof(ping));
    }

    // 温湿度
    if (shtAvailable && millis() - lastSHTRead > SHT_READ_INTERVAL_MS) {
        ct_mark(CT_S_SHT);
        sht31_update();
        lastSHTRead = millis();
    }

    // KPM 推进器：每过 1 秒把头部指针后移一格，旧秒被新 0 覆盖。
    // 不在中断里做 —— 直接在主循环 1Hz 节拍上推。
    {
        unsigned long nowSec = millis() / 1000UL;
        if (nowSec != kpmRingLastSec) {
            // 每秒把"已经跑了多久"写进 RTC 慢存。
            //
            // ⚠ 看门狗拆掉之后，ct_rec.uptimeMs 没有人写了（原先是 6 秒卡死
            //   监测任务在判死的那一刻顺手写的）。这个字段得自己维护：开机
            //   诊断和错误日志都要靠它回答"上次是跑了很久才崩，还是刚起来就崩"
            //   —— 前者多半是某个具体功能触发，后者多半是启动期就坏了。
            ct_rec.uptimeMs = millis();
            long delta = (long)(nowSec - kpmRingLastSec);
            if (delta > 0 && delta < KPM_WINDOW_SEC * 4) {
                // 一次推进多格（开机/卡顿后兜底）；但绝不把整圈清成 0。
                for (long i = 0; i < delta; i++) {
                    kpmRingHead = (kpmRingHead + 1) % KPM_WINDOW_SEC;
                    kpmRing[kpmRingHead] = 0;
                }
            } else {
                // delta 异常大 / 溢出：直接重置整个缓冲
                memset(kpmRing, 0, sizeof(kpmRing));
                kpmRingHead = 0;
            }
            kpmRingLastSec = nowSec;
        }
    }

    // 键盘扫描
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        ct_mark(CT_S_SCAN);
        scanKeyboardMatrix();
    }

    // 闹钟到点判定 + 倒计时推进。函数内部自己按整秒节流，所以这里每轮调
    // 只是几次整数比较。放在按键扫描之后、灯光引擎之前：起铃要在这一轮
    // 就把灯效让出来，屏要在显示段把响铃卡片画出来。
    ct_mark(CT_S_SCAN);
    updateTimers();

    // 锁状态脏：不等 20ms 灯效节拍，立刻推一帧纯指示灯帧。
// renderLightingEngine() 每帧尾巴也会调 renderIndicators()，
// 这条分支只是把"按 Caps Lock → 看到屏幕/灯变化"的延迟从最坏 20ms 压到下一轮 loop。
// 状态灯帧不依赖主背光的 currentEffect / lightOn，所以单独跑一次也没副作用。
if (lockStateDirty) {
    renderIndicators();
    sendLedFrameToC3();
}

// 灯效
    if (millis() - lastLedFrameTime >= 20) {
        lastLedFrameTime = millis();
        ct_mark(CT_S_LED);
        renderLightingEngine();
        sendLedFrameToC3();
    }

    // 息屏
    //
    // 响铃中 / 倒计时全屏期间**不许**息屏：这两个状态的全部意义就是
    // "抬头看一眼"，屏幕一黑就等于没有。startRinging() 已经把
    // lastActivityTime 拨到当下，所以这里只要不放响铃漏过去就行。
    //
    // saverMode == SAVER_OFF（"屏保风格"选成"关闭"）时整段跳过：
    // 空闲到点什么都不做，主屏一直留在那儿。
    if (currentSysMode == SYS_MODE_NORMAL
        && ringingKind == RING_NONE
        && saverMode != SAVER_OFF
        && !(countdownShown && timerRunning)
        && !kbLocked
        && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
        currentSysMode = SYS_MODE_SLEEP;
        ct_mark(CT_S_SLEEP);
        // SAVER_BLACK 走老路径拆主屏；壁纸/时间温湿度则另起一块屏保屏，
        // 主屏原样留着，唤醒时切回去就行，省掉一次重建。
        enterScreensaver();
    }

    // 菜单重建（在主循环中处理，避免与 LVGL 冲突）
    if (menuNeedsRebuild) {
        menuNeedsRebuild = false;
        ct_mark(CT_S_MENU);
        build_menu();
    }

    // 主显示
    if (currentSysMode == SYS_MODE_NORMAL) {
        ct_mark(CT_S_DYNAMIC);
        // 倒计时全屏接管期间**照样**要跑 updateDynamicElements()，不能因为
        // "主屏看不见了"就跳过。
        //
        // 跳过会踩到一个很隐蔽的雷：lockStateDirty 是靠 updateDynamicElements()
        // 内部清掉的（见它开头那个 100ms 节拍 early return 和末尾的
        // lockStateDirty = false）。loop() 里另有 `if (lockStateDirty) {
        // renderIndicators(); sendLedFrameToC3(); }` —— 脏位不清就变成
        // **每一轮 loop 都往 C3 推一帧 61 字节的灯帧**（每秒几千帧），
        // Serial1 缓冲直接堵死，主循环被饿死，6 秒后监测任务判定卡死 →
        // esp_restart()。表现就是"倒计时一跑就重启"和"按了 Caps 就重启"。
        // 那个 early return 本来就把它限成 100ms 一次，主屏不可见时刷的是
        // 看不见的 label，没有性能负担 —— 当初那个"省 CPU"的判断是纯亏。
        updateDynamicElements();
        // 律动页的柱子动画走独立 16ms 节拍，跟上面 100ms 的慢刷新解耦。
        // 放在 updateDynamicElements() 里面就是"律动很慢"的根因。
        tickRhythm();
    } else if (currentSysMode == SYS_MODE_SLEEP) {
        // 息屏期间 updateDynamicElements() 不跑，屏保的时钟/温湿度自己刷
        updateScreensaver(false);
    }

    // ---- 倒计时全屏：该不该接管显示，每轮判一次 ----
    // 放这里而不是各调用点自己 showScreen()：进菜单/设置页要收、退出要放、
    // 倒计时跑完也要收，分散在三个地方判迟早会漏一处，表现就是"进了菜单
    // 底下还是倒计时"或者"跑完了屏幕还停在倒计时上"。
    updateCountdownVisibility();

    // 倒计时设置页在跑的时候要跟着走秒：那个"停止"按钮摆在那里，
    // 用户就是靠这块屏看还剩多久的，数字不跳等于按钮是假的。
    if (currentSysMode == SYS_MODE_SET_TIMER && timerRunning) {
        update_setting_timer_display();
    }

    // ---- 响铃卡片：lv_layer_top 上的浮层，压在任何一块屏上面 ----
    // 每轮都调，内部按 500ms 节流：起铃、停铃、描边翻转时才会真重建。
    // 它挂的是 top 层而不是活动屏，所以底下是主屏/菜单/设置页/黑屏都一样能盖住，
    // 不需要为了显示它去改 currentSysMode。
    drawRingOverlay();

    // ---- 锁屏：红框告警的闪烁节拍 ----
    // 每轮都调，内部自己判"该不该亮/该灭/该收"，不在闪的时候零开销。
    lockScreenTick();

    // HUD 消失
    ct_mark(CT_S_HUD);
    if (hud.active && millis() - hud.startMs > hud.showMs) {
        hudClear();
    }

    // 通知兜底超时：正常路径是按静音键确认掉，不自动消失。
    // 万一没人按键（比如键盘整晚没人碰），到点整队清掉，别让红灯闪一宿。
    ct_mark(CT_S_NOTIF);
    if (notifCount > 0 && millis() - notifStartMs > NOTIF_SAFETY_MS) {
        clearNotifications();
        triggerHud("通知超时", "已自动清除", lv_color_hex(CLR_TEXT_DIM));
    }

    // LVGL
    ct_mark(CT_S_LVGL);
    lvgl_driver_loop();
    delayMicroseconds(500);

    // ---- BOOTDL：重启进下载模式 ----
    //
    // 放在**一轮的最末尾**：前面那条回话（btLinkReplyC）是在上一轮
    // btLinkPoll() 的 pumpTx() 里发出去的，到这里它已经出去了。
    //
    // ⚠⚠⚠ **为什么不能用 esp_restart()**（这是 BOOTDL 一直没生效的真因）：
    //   复位会清掉 GPIO 的输出配置 —— IO0 变回输入，由外部上拉决定电平，
    //   于是 ROM 采样到 boot:0x1（高）→ **正常启动 app**，根本进不了下载模式。
    //   注释里原来写"先把 GPIO0 配成输出并拉低，复位时它才是低电平"——
    //   这个前提是错的：ESP32-S3 复位后 GPIO 的输出寄存器会被重置。
    //
    // ✅ 改用 deep sleep：ESP32-S3 的 **GPIO0 属于 RTC GPIO**，
    //   `rtc_gpio_hold()` 能在 deep sleep 期间保持输出电平。
    //   于是唤醒（无论定时唤醒还是之后的上电）时，ROM 采到的 IO0 仍是低
    //   → 进下载模式。
    //
    // ⚠ **为什么只用 rtc_gpio_hold、不用 gpio_hold_en**：
    //   gpio_hold_en 的 hold 只有掉电才会消失 —— 一旦用了它，
    //   "烧完固件 → esptool run → 复位"这一下又会因为 IO0 被按住在低电平
    //   而**再次落进下载模式**，app 起不来，还得以拔插收场。
    //   rtc_gpio_hold 的 hold 在唤醒时自己就没了：芯片醒来是干净的，
    //   烧完固件复位一下就正常跑 app。
    //   万一 rtc_gpio_hold 不奏效（芯片直接醒成 app），最坏结果是
    //   "BOOTDL 白按一次"，**不会把设备锁死**。
    //
    // 配一个 20 秒定时唤醒当保险：万一主机没来烧，芯片自己醒过来，
    // 但那时 IO0 仍是低的（RTC hold 还在），还是会进下载模式等主机 ——
    // 这正是 BOOTDL 的语义（"我现在停在下载模式"），不是故障。
    if (g_bootDlPending) {
        g_bootDlPending = false;
        LOG_PORT.println("[BOOTDL] 拉低 BOOT 脚（RTC hold）并进入下载模式");
        pinMode(GPIO_NUM_0, OUTPUT);
        digitalWrite(GPIO_NUM_0, LOW);
        delay(50);
        rtc_gpio_hold_en(GPIO_NUM_0);        // deep sleep 期间保持 IO0 为低
        esp_sleep_enable_timer_wakeup(20ULL * 1000000ULL);
        esp_deep_sleep_start();
        return;      // 理论上到不了这里
    }

    // BOOTAPP —— 反过来：把 BOOT 脚(GPIO0)**拉高**再复位，
    // 让芯片从 ROM 下载模式回到"正常从 flash 启动应用"。
    //
    // 为什么需要它（2026-10-22 实测）：
    //   ESP32-S3 的 **USB 会话在复位那一瞬间会把 IO0 压低**，
    //   于是只要 USB 线还插着，按 EN / 复位都会落回 ROM 下载模式 ——
    //   实测 GPIO_IN 的 IO0 位是 1（物理高电平）、GPIO_ENABLE 的 IO0 位是 0
    //   （谁也没驱动它），可复位时 ROM 依然采到 boot:0x0（IO0 低）。
    //   结果就是"烧完固件拔插才好"，而拔插不是每次都方便。
    //
    //   这里让固件自己把 GPIO0 配成输出并**拉高**，再复位：
    //   复位采样到的就是高电平 → ROM 正常启动 flash 里的应用。
    //   于是「进下载模式 → 烧录 → 回应用模式」全程不用碰 BOOT/EN。
    if (g_bootAppPending) {
        g_bootAppPending = false;
        LOG_PORT.println("[BOOTAPP] 拉高 BOOT 脚并复位");
        gpio_hold_dis(GPIO_NUM_0);          // 先解除可能残留的 hold
        pinMode(GPIO_NUM_0, OUTPUT);
        digitalWrite(GPIO_NUM_0, HIGH);
        delay(100);
        esp_restart();
        return;      // 理论上到不了这里
    }

    // 完整跑完一轮：心跳 +1，监测任务据此判断有没有卡死
    ct_mark(CT_S_IDLE);
}
