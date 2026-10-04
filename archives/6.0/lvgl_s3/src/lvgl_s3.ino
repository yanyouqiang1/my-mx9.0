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
#include <esp_task_wdt.h>
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

class LogMirror : public Print {
    HardwareSerial* real;
    char    lineBuf[BT_RX_BUF_SIZE];
    size_t  lineLen;
public:
    LogMirror(HardwareSerial* r) : real(r), lineLen(0) {}

    // Serial0.begin() 这种成员转发：LOG_PORT.begin(115200) 必须能落到 UART0
    void begin(unsigned long baud) { if (real) real->begin(baud); }
    void end() { if (real) real->end(); }

    size_t write(uint8_t c) override {
        if (real) real->write(c);
        if (c == '\n') {
            // 遇到 \n 才提交一行；\r 单独出现也提交一次（兼容 println 的 \r\n）
            lineBuf[lineLen] = '\0';
            pushLogLine(lineBuf);
            lineLen = 0;
        } else if (c == '\r') {
            // 忽略，\n 那一拍已经处理过
        } else if (lineLen < sizeof(lineBuf) - 2) {
            lineBuf[lineLen++] = (char)c;
        } else {
            // 行太长，截断并把当前缓冲当成完整一行推出去（兜底，防止 lineLen 溢到下一行）
            lineBuf[sizeof(lineBuf) - 2] = '\0';
            pushLogLine(lineBuf);
            lineBuf[0] = (char)c;
            lineLen = 1;
        }
        return 1;
    }

    size_t write(const uint8_t* buf, size_t size) override {
        if (real && size) real->write(buf, size);
        for (size_t i = 0; i < size; i++) write(buf[i]);
        return size;
    }
};

// 全局唯一代理：所有 LOG_PORT 都走它，再转发到 Serial0 + 落环形缓冲
static LogMirror LogPortProxy(&Serial0);
#undef  LOG_PORT
#define LOG_PORT LogPortProxy

// === BLE 日志通道缓冲 ===
// PSRAM 上分一块 8KB 环形缓冲（按行存，超出时丢最老的整行）。
// 板子是 N16R8 = 8MB OPI PSRAM，8KB 占比 0.1%，随便用。
#define LOG_BUF_CAP     8192
static char*  logRingBuf   = nullptr;   // ps_malloc，setup() 里建
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
#define CFG_PEND_PROF    0x04
#define CFG_PEND_SAVER   0x08
static uint8_t cfgPend = 0;

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
#include "elog.h"

#include <esp_heap_caps.h>

#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>
#include "profile_icons.h"   // 4 方案 × 3 档 (S/M/L) RGB565 图标，由 mmx 生成

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
#define WDT_TIMEOUT  10

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
// 宠物页。追加在末尾（0~10 已占满），**不是**"设置模式"：
// 1. 不能落进 IS_SETTING_MODE 的 3~7 连续区间，否则被 moveSettingField /
//    adjustSettingField 那套接管，按键语义全错。
// 2. ⚠️ 更要紧的是：**绝对不要把它加进 scanKeyboardMatrix() 里的 inUiMode
//    表达式**（那个变量在 5899 附近）。inUiMode 一旦命中，5899 之后那一整块
//    会把按键吃干净、一律不发主机 —— 表现就是"宠物页里一个字都打不出来"。
//    故意不加：宠物页里矩阵键照常走普通键盘分支发给主机，宠物只作反应。
//    宠物页的全部操作走 C3 实体键（静音/旋钮/灯光长按），那些键矩阵里没有。
#define SYS_MODE_PET       11
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
#define TOTAL_DISP_MODES          7

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸", "高对比度"
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

// ---- 宠物的类型声明放在这里，不放在宠物那一节 ----
// Arduino 把 .ino 预处理成 .cpp 时会**给每个函数自动生成一份原型**，插在
// "第一个函数定义"之前（本文件大约 1000 行处）。原型里出现自定义类型时，
// 那个类型必须在插入点之前就可见，否则报 "'PetScene' does not name a type"。
// 同类的坑 formatDateCN 那段注释也记过（参数改成收 int 就是为了躲它）。
// 所以凡是出现在函数签名里的自定义类型，一律跟着 AlertType 放在这一段。
enum PetScene { SCENE_NONE = 0, SCENE_PEEK, SCENE_FULL, SCENE_SAVER };
enum PetPose {
    POSE_IDLE = 0, POSE_GREET, POSE_HAPPY, POSE_HUNGRY,
    POSE_SLEEP, POSE_GRUMPY, POSE_SICK
};

// 心情分档：台词按 moodMin 过滤，所以 mood 的用途只有"决定说什么口吻"
#define PET_MOOD_SAD    30
#define PET_MOOD_OK     65

struct PetState {
    uint8_t  mood;           // 0..100
    uint16_t bond;           // 亲密度，长期累积
    uint8_t  pose;
    uint32_t poseUntilMs;    // 姿态自然结束的时刻（到点自动回 IDLE）
    uint32_t lastPettedMs;   // 上次被摸（算抚摸冷却）
    uint32_t lastSeenMs;     // 上次见到人（算"你走了多久"）
    uint8_t  fedToday;
    uint32_t fedYmd;
    uint8_t  peekToday;      // 今日探头次数，对 PET_PEEK_DAILY_MAX
    uint32_t peekYmd;
    uint8_t  bothered;       // 连续被打扰次数，>=2 暂停一天
    uint32_t muteYmd;        // 主动探头暂停到哪一天
    bool     peekOn;         // 主动打招呼总开关，默认开
    uint8_t  fromH;          // 显示时段起（小时）
    uint8_t  toH;            // 显示时段止（小时）
};

// 让位清单的三个函数声明放这里（宠物本体在 3500 行之后，这里先用）：
// resetStylePointers()（1400 行出头）要调 petYieldClear()，壁纸和高对比度
// 两个 build_style_* 也要调 petYieldAdd()，它们都比宠物本体早。
static void petYieldAdd(lv_obj_t* o);
static void petYieldClear(void);
static void petSetBottomYield(bool yield);

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
static bool keebMatrix[NUM_ROWS][NUM_COLS] = {0};
// 记录这颗键"按下时是否真的发给过主机"，松键时据此决定要不要补一个 release。
// 没有它的话，菜单里按 ESC 这类被界面吞掉的键会在松手时给主机发一个无头的 release。
static bool hostKeyHeld[NUM_ROWS][NUM_COLS] = {0};
static unsigned long lastDebounceTime[NUM_ROWS][NUM_COLS] = {0};
static uint32_t totalKeyCount = 0;
static uint16_t baseMatrix[NUM_ROWS][NUM_COLS] = { 0 };
static bool fnPressed = false;

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

// 列表最多能同时显示几行（一级 6 个大类，二级最多 3 项，6 够用）
#define MENU_ROW_MAX 6

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
    MA_PET_PEEK_ON,     // 宠物：主动打招呼开关
    MA_PET_WINDOW,      // 宠物：显示时段
    MA_PET_OPEN,        // 宠物：进全屏页
    MA_PET_RENAME,      // 宠物：改名
    MA_PET_RESET        // 宠物：清空亲密度
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
// 宠物组**追加在末尾**（第 7 个）。理由和屏保档位一样：分组编号会影响用户的
// 肌肉记忆，插在中间会让原来"第 N 组"的位置全变。
static const MenuEntry menuEntriesPet[] = {
    { "主动打招呼",   MA_PET_PEEK_ON },
    { "显示时段",     MA_PET_WINDOW },
    { "去找小橘",     MA_PET_OPEN },
    { "改名字",       MA_PET_RENAME },
    { "清空亲密度",   MA_PET_RESET }
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
    { "宠物",   "PET",      menuEntriesPet,      sizeof(menuEntriesPet)      / sizeof(MenuEntry) }
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
//   SAVER_WALL 壁纸铺满，和"信息面板"每隔几秒**轮播**一张 —— 一直是图片
//              会看不到时间，一直是面板又浪费了壁纸
//   SAVER_INFO 只显示信息面板，不轮播
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
#define SAVER_WALL  1   // 壁纸轮播
#define SAVER_INFO  2   // 信息面板
#define SAVER_OFF   3   // 关闭：空闲也不息屏
#define TOTAL_SAVER_MODES 4
static const char* saverModeNames[] = { "黑屏", "壁纸轮播", "信息面板", "关闭" };
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
// 一级正好 6 个大类，所以这 6 行是一屏全放得下的，不用翻页。
#define MENU_VISIBLE_ITEMS 6
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
static void handleC3Command(const String& cmd);   // 定义在文件后段
static void knobAdjust(int dir);                  // 定义在文件后段
static void handleC3Events(void) {
    static String serialBuffer = "";
    while (Serial1.available() > 0) {
        char c = (char)Serial1.read();
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

// Screen切换函数
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
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
// 壁纸右下角那块 112x40 的时钟板。原来是 build_style_wallpaper 里的局部变量，
// 探头的"底部让位"要隐藏它，提成全局才有句柄。
static lv_obj_t* wp_plate = nullptr;

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
static uint32_t     meHexBytes  = 0;         // ME_DATA 累计写进去的十六进制字符数（诊断用）
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
#define STYLE_BIT_ALL             (STYLE_BIT_GEEK | STYLE_BIT_BIG_CLOCK | STYLE_BIT_INFO_PANEL | \
                                   STYLE_BIT_KEY_MON | STYLE_BIT_RHYTHM | STYLE_BIT_WALLPAPER | \
                                   STYLE_BIT_HIGH_CONTRAST)
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
    // 让位清单里存的也是这些风格对象的指针。切风格时旧容器已经被删了，
    // 清单不跟着清就是野指针 —— 探头一收起就往已释放内存写 HIDDEN flag。
    // 各 build_style_* 末尾会重新填（只有高对比度和壁纸两种会填）。
    petYieldClear();
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

    wp_img = nullptr; wp_lbl_time = nullptr; wp_plate = nullptr;

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
void bootDetailTick(void);
static void renderCurrentDisplayBase(void);
static void updateDynamicElements(void);
static void tickRhythm(void);
static void pulseRhythm(void);
static void triggerHud(const char* title, const char* value, lv_color_t color);
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
// ---- 宠物（定义在"HUD 浮层"那一节之前）----
// runMenuAction() 在 3119 就得调 petEnterFull()，scanKeyboardMatrix() 的 LOGO
// 分支和 loop() 也要调，所以这一整组必须先声明。
static void petPoll(void);
static void petEnterFull(void);
static void petExitFull(void);
static void petDismiss(void);
static void petDestroyPeek(void);
static void petBuildPage(void);
static bool petPeekAllowed(void);
static bool petPetted(uint8_t n);
static void petFed(void);
static void petLoadFromNvs(void);
static void petSaveMood(void);
static void petMenuAction(MenuAction act);
static const char* petName(void);
static const char* petLineFor(uint8_t pose);
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
class MyServerCallbacks : public BLEServerCallbacks {
    // pServer 要转给 bt_link：协商后的 MTU 只在 server 上（characteristic 的
    // getService() 是私有的，拿不到），拿不到就一直按 20 字节保守发。
    void onConnect(BLEServer* pServer)    { btLinkSetConnected(true, pServer); }
    void onDisconnect(BLEServer* pServer) { btLinkSetConnected(false); }
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
    logoRxTotal = 0;
    logoRxGot   = 0;
    logoRxDone  = false;
    logoRxActive = false;
    LOG_PORT.printf("[WALLPAPER] abort: %s\n", reason);
    // 传输中断是"用户/主机那边出问题"，但对排查很有价值：能看出
    // 壁纸是不是经常传到一半就断（多半是蓝牙距离或网页被关掉了）。
    ELWARN("WALL", "壁纸传输中断 %s", reason);
    triggerHud("壁纸传输", reason, lv_color_hex(CLR_RED));
    // 失败也要回报，否则网页只能干等到超时。
    // ⚠ 网页在传输期间会高频轮 LOGO_STATUS，多发一条同内容的通知会被 BLE
    //   合并掉（通知不排队），所以这里只发这一条，别再顺手 LOG 一遍。
    btLinkReplyf("LOGOSTATUS", "FAIL:%.40s", reason ? reason : "?");
}

// 二进制接收：这段是 JPEG 原始字节，里面必然有 0x00，
// 只能按长度整段取，绝不能走 c_str()（会在第一个 0 处截断）。
static void handleLogoChunk(const uint8_t* data, size_t len) {
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
    esp_task_wdt_reset();

    uint16_t* out = (uint16_t*)heap_caps_malloc(WP_PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) { abortLogoUpload("out of PSRAM"); return; }

    logoOutBuf = out;
    memset(out, 0, WP_PIXELS * 2);   // 先铺黑：JPEG 没盖满 240x240 时不留上一张的残影
    esp_task_wdt_reset();

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
    if (jpeg.openRAM(logoRxBuf, (int)logoRxGot, logoJpegDraw)) {
        LOG_PORT.printf("[WALLPAPER] jpeg header %dx%d, decoding...\n",
                        jpeg.getWidth(), jpeg.getHeight());
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        ok = (jpeg.decode(0, 0, 0) != 0);
        jpeg.close();
    }
    logoOutBuf = nullptr;
    esp_task_wdt_reset();
    LOG_PORT.printf("[WALLPAPER] decode %s\n", ok ? "ok" : "FAILED");

    // 注意：写盘那 200 多毫秒里旧的 wpPixels 必须保持有效 —— 屏还在用它画壁纸，
    // 这里提前 free 就是 use-after-free，wpImgDsc.data 还会指向已释放的地址。
    // 所以先把旧指针扣下，等新图彻底接上再释放。
    uint16_t* oldPixels = wpPixels;

    if (ok) {
        // 落盘一份，重启后直接从 SPIFFS 读回。分块写 + 喂狗：115KB 一次写完
        // 在 SPIFFS 上是毫秒级的，loop 任务挂在任务看门狗上，稳妥点分段喂。
        File f = SPIFFS.open("/logo.bin", FILE_WRITE);
        if (f) {
            const uint8_t* p = (const uint8_t*)out;
            size_t left = (size_t)WP_PIXELS * 2;
            while (left) {
                size_t n = left > 16384 ? 16384 : left;
                f.write((uint8_t*)p, n);
                p += n; left -= n;
                esp_task_wdt_reset();
            }
            f.close();
            LOG_PORT.printf("[WALLPAPER] /logo.bin written\n");
        } else {
            LOG_PORT.printf("[WALLPAPER] open /logo.bin FAILED\n");
        }
    }

    logoRxTotal = 0;
    logoRxGot   = 0;

    if (!ok) {
        heap_caps_free(out);
        triggerHud("壁纸传输", "解码失败", lv_color_hex(CLR_RED));
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

    triggerHud("壁纸更新", "上传完成", lv_color_hex(CLR_GREEN));

    // 主动回报：网页轮询 LOGO_STATUS 时能拿到确定答案。
    // 以前网页"字节发完"就 alert 上传成功，那是假成功 —— BLE 写成功只说明
    // 数据交给蓝牙了，不代表键盘解出来、落盘了。
    btLinkReplyC("LOGOSTATUS", "OK:0/0");
}

// 开机：SPIFFS 里有 /logo.bin 就读回 PSRAM，重启不用重传。
static void loadWallpaperFromDisk(void) {
    if (!SPIFFS.exists("/logo.bin")) return;
    File f = SPIFFS.open("/logo.bin", FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz != (size_t)(WP_PIXELS * 2)) return;   // 尺寸对不上就当没有，宁可空着

    uint16_t* buf = (uint16_t*)heap_caps_malloc(WP_PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) return;
    File r = SPIFFS.open("/logo.bin", FILE_READ);
    if (!r) { heap_caps_free(buf); return; }
    size_t rd = r.read((uint8_t*)buf, WP_PIXELS * 2);
    r.close();
    if (rd != (size_t)(WP_PIXELS * 2)) { heap_caps_free(buf); return; }

    if (wpPixels) heap_caps_free(wpPixels);
    wpPixels = buf;
    bindWallpaperSource();
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

    // 压一层半透明黑，保证上面的字在任意壁纸上都读得出来。
    // 从 LV_OPA_50(=127) 降到 LV_OPA_20(=51)：字已经缩到右下角一小块、
    // 而且有自己的底板了，全屏遮罩再压一半，整个画面就只剩"糊"——
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

    // 时钟挪到**右下角**一小块。
    // 原来是 168x64 的板子杵在正中间、48px 大字 —— 图被挡掉一大块，
    // 用户的原话是"图片都看不清了"。缩到 28px 贴右下角，只压住一个角。
    wp_plate = lv_obj_create(wp_bg);
    lv_obj_set_size(wp_plate, 112, 40);
    lv_obj_align(wp_plate, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    mkCard(wp_plate, CLR_SURFACE_2, 10);
    lv_obj_set_style_bg_opa(wp_plate, LV_OPA_70, LV_PART_MAIN);

    wp_lbl_time = lv_label_create(wp_plate);
    mkLabel(wp_lbl_time, &lv_font_montserrat_28, CLR_TEXT);
    lv_label_set_text(wp_lbl_time, "--:--");
    lv_obj_center(wp_lbl_time);

    // 探头横条 y=168..240 会盖住这块板（它在 y=190..230），登记进让位清单。
    // 提为全局指针就是为了这一行 —— 原来是局部变量，出函数就没句柄了。
    petYieldClear();
    petYieldAdd(wp_plate);
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

    // ---- 底部让位清单 ----
    // 探头横条 y=168..240 正好盖住这一整条（y=178..235），探头那 8 秒要把
    // 它们全藏起来，否则温湿度条会被切成一半露在横条上方（见 mock_pet_yield.png）。
    //
    // 为什么是 12 个平铺对象而不是一个容器：这些 label / bar 全是 hc_bg 的
    // **直接子对象**（box 只是装饰框）。收进一个容器就得把 10 个对象的 y 从
    // 180/199/209/228 改成 2/21/31/50，动的是上面逐像素调好的版面。所以只登记
    // 指针，不动版面。
    petYieldClear();
    petYieldAdd(hc_bot_box);
    petYieldAdd(hc_bot_div);
    petYieldAdd(hc_lbl_strip_t);
    petYieldAdd(hc_lbl_strip_t_val);
    petYieldAdd(hc_bar_t_track);
    petYieldAdd(hc_bar_t_fill);
    petYieldAdd(hc_lbl_strip_h);
    petYieldAdd(hc_lbl_strip_h_val);
    petYieldAdd(hc_bar_h_track);
    petYieldAdd(hc_bar_h_fill);
    petYieldAdd(hc_lbl_strip_chars);
    petYieldAdd(hc_lbl_strip_chars_val);
}

// ===========================
// 息屏 / 屏保
// ===========================
// 屏保是一块独立的屏幕对象，和主屏并存：
//   SAVER_BLACK 沿用老行为 —— 拆掉主屏，屏幕全黑
//   SAVER_WALL 壁纸铺满，和"信息面板"每 5 秒轮播一次
//   SAVER_INFO 只显示信息面板
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

// 屏保信息面板：时间 / 日期 / 温湿度全收在**这一块**面板里。
// 旧版是四个 label 散在屏幕各处（时间居中偏上、日期居中、温湿度各贴一个下角），
// 配上壁纸就是四处压图 —— 轮播模式下必须收拢成一张卡，不然图片根本没得看。
static void buildSaverPanel(void) {
    sv_panel = lv_obj_create(scr_saver);
    lv_obj_set_size(sv_panel, 204, 112);
    // 壁纸轮播时贴底（让出图片的上 2/3），纯信息模式居中
    if (saverMode == SAVER_WALL) lv_obj_align(sv_panel, LV_ALIGN_BOTTOM_MID, 0, -16);
    else                         lv_obj_align(sv_panel, LV_ALIGN_CENTER, 0, 0);
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
}

// 菜单里"屏保风格"那一项：黑屏 → 壁纸轮播 → 信息面板 → 关闭 → 黑屏
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
    triggerHud("屏保风格", saverModeNames[saverMode], lv_color_hex(CLR_ACCENT));
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
    // 没上传过壁纸时没有可轮播的图，直接常显面板
    setHidden(sv_panel, (saverMode == SAVER_WALL) && wpReady);

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

    // 壁纸轮播：每 5 秒在"纯壁纸"和"信息面板"之间翻一次。
    // 计时器用 tick 计数而不是记时间戳 —— 这个函数既被 100ms 的节拍调，
    // 也被 enterScreensaver() 直接调一次（force），用时间戳会被那次调用搅乱。
    if (saverMode == SAVER_WALL && wpReady) {
        static uint8_t tick = 0;
        if (force) {
            tick = 0;
            setHidden(sv_panel, true);      // 进屏保先给图片（用户最想先看到的是图）
        } else {
            tick++;
            if (tick >= 50) {               // 50 × 100ms = 5s
                tick = 0;
                setHidden(sv_panel, !lv_obj_has_flag(sv_panel, LV_OBJ_FLAG_HIDDEN));
            }
        }
    } else {
        setHidden(sv_panel, false);
    }

    // 面板被藏起来（纯壁纸那一相）时不用白刷标签
    if (sv_lbl_time == nullptr || lv_obj_has_flag(sv_panel, LV_OBJ_FLAG_HIDDEN)) return;

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
    triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_ACCENT));
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
                triggerHud("温湿度", "已刷新", lv_color_hex(CLR_GREEN));
            } else {
                triggerHud("温湿度", "未找到", lv_color_hex(CLR_RED));
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
            triggerHud("击键计数", "已清零", lv_color_hex(CLR_ACCENT));
            break;
        case MA_OPEN_ELOG:       // 错误日志：重启后在这里翻出错内容
            build_elog();
            break;
        // 宠物五项。petMenuAction() 内部按 MenuAction 再分派一次，和
        // runMenuAction 一样走枚举，不按数组下标。
        case MA_PET_PEEK_ON:
        case MA_PET_WINDOW:
        case MA_PET_OPEN:
        case MA_PET_RENAME:
        case MA_PET_RESET:
            petMenuAction(act);
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
// 一级 6 行、二级最多 3 行，都不超过 MENU_VISIBLE_ITEMS，所以实际用不到滚动；
// 但滚动逻辑留着，二级项以后变多也不会被卡在窗口外。
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
// 键盘宠物
// ===========================
// 一只会自己找你的猫。造型是纯 lv_obj 基本图形拼的（圆 + 圆角矩形 + 细线），
// 不是贴图：LV_COLOR_SCREEN_TRANSP=0 且禁用 transform_zoom/angle（见 README），
// 画不出三角形也没有半透明发光，所以耳朵是圆的、阴影走 shadow_opa。
// 零新增资源、零新增 RAM、零解码器。
//
// 四个出场形态互斥，仲裁集中在 petGate()：
//   NONE  不在场
//   PEEK  底部横条探头，固定 8s，长按 LOGO 收掉
//   FULL  全屏宠物页（菜单 / 长按 LOGO 进）
//   SAVER 屏保里的宠物（v2，SAVER_PET 档位）
//
// 键位：**全部走 C3 实体键**（静音 / 旋钮 / 灯光长按），矩阵键一个都不占 ——
// 那些键矩阵里没有，参与不了正常打字，用它们互动才不会影响输入。

// PetScene / PetPose / PetState 三个类型声明在文件前面的"通知系统"那一段
// （跟 AlertType 放一起）—— 因为 Arduino 会把函数原型插到第一个函数定义之前，
// 签名里出现自定义类型时类型必须在那之前可见。

static PetState pet = { 60, 0, POSE_IDLE, 0, 0, 0, 0, 0, 0, 0, 0, true, 23, 7 };
static PetScene petScene = SCENE_NONE;

// 调参都在这儿，改完直接看效果
#define PET_PEEK_IDLE_MIN_MS   25000UL   // 闲置多久才进探头候选池
#define PET_PEEK_GAP_MIN_MS    20000UL   // 两次判定的最小间隔
#define PET_PEEK_GAP_JIT_MS    20000UL   // 判��间隔的随机抖动上限
#define PET_PEEK_CHANCE         30       // 每次判定的触发概率（%）
#define PET_PEEK_DAILY_MAX       8       // 每日探头上限
#define PET_PEEK_HOLD_MS       8000UL    // 探头停留时长
#define PET_PET_COOLDOWN_MS     800UL    // 抚摸冷却，防连点刷亲密度
#define PET_BOTHER_LIMIT         2       // 连续被关几次就安静一天
#define PET_LOGO_HOLD_MS       600UL    // LOGO 长按阈值

static unsigned long petPeekNextCheckMs = 0;
static unsigned long petPeekStartMs   = 0;
static unsigned long petLastLineMs   = 0;
static uint32_t      petLastLineCls  = 0xFFFFFFFF;   // 上一句的分类，防同分类连播

// LOGO 长按：矩阵键本来没有长按检测（_HOLD 全来自 C3 小 MCU）
static unsigned long logoDownMs    = 0;
static bool          logoHoldFired = false;
static bool          logoIsDown     = false;

// ===========================
// 台词表
// ===========================
// **行宽是硬约束**：全屏气泡 216px、探头台词 168px，汉字 16px，
// 所以每句必须 ≤10 字（探头的限制更紧），宁可少写也不要超。
// 加新台词前跑 `python text_width.py "你的台词"` 量一下。
//
// 分类位（0x01 探头 / 0x02 全屏 / 0x04 时段 / 0x08 状态）只用于冷却去重。
struct PetLine {
    uint8_t     pose;    // POSE_*，和当前姿态不符的不挑
    uint8_t     moodMin; // mood < moodMin 的不挑
    uint8_t     cls;     // 分类位
    const char* text;
};

static const PetLine petLines[] = {
    // ---- 探头专用（≤10 字）----
    { POSE_GREET,   0, 0x01, "你回来啦！" },
    { POSE_GREET,   0, 0x01, "嘿，我在这儿" },
    { POSE_IDLE,    0, 0x01, "歇会儿也好" },
    { POSE_HAPPY,   0, 0x01, "刚才摸我了？" },
    { POSE_HUNGRY,  0, 0x01, "肚子叫了…" },
    { POSE_HUNGRY,  0, 0x01, "到饭点啦" },
    { POSE_IDLE,    0, 0x01, "不急，慢慢打" },
    { POSE_GRUMPY,  0, 0x01, "哼，不理你了" },
    { POSE_SLEEP,   0, 0x01, "困了，先眯一会" },
    { POSE_SICK,    0, 0x01, "好热，趴着不动" },
    { POSE_SICK,    0, 0x01, "太冷了，蜷起来" },
    { POSE_IDLE,    0, 0x01, "今天还没吃饭呢" },
    { POSE_HAPPY,   0, 0x01, "今天你很勤快" },
    { POSE_IDLE,    0, 0x01, "水记得喝一口" },

    // ---- 全屏专用（≤13 字）----
    { POSE_IDLE,    0, 0x02, "摸两下试试" },
    { POSE_HAPPY,   0, 0x02, "呼噜呼噜…" },
    { POSE_HAPPY,   PET_MOOD_OK, 0x02, "你今天对我真好" },
    { POSE_GRUMPY,  0, 0x02, "摸我得用静音键" },
    { POSE_HUNGRY,  0, 0x02, "旋钮按一下就喂我" },
    { POSE_SICK,    0, 0x02, "太热了，我不动" },
    { POSE_SLEEP,   0, 0x02, "你不在我就睡了" },
    { POSE_GREET,   0, 0x02, "好久没见，想你了" },

    // ---- 时段问候（≤10 字，探头用）----
    { POSE_IDLE,    0, 0x04, "早上好呀" },
    { POSE_IDLE,    0, 0x04, "中午要吃饭了" },
    { POSE_IDLE,    0, 0x04, "下午加油鸭" },
    { POSE_IDLE,    0, 0x04, "这么晚还不睡" },
    { POSE_GREET,   0, 0x04, "今天也辛苦啦" },
    { POSE_IDLE,    0, 0x04, "记得站起来走走" },

    // ---- 心情低落（mood 低时才有）----
    { POSE_GRUMPY,  0, 0x08, "你好久没理我了" },
    { POSE_IDLE,    0, 0x08, "有点想你了" },
    { POSE_SLEEP,   0, 0x08, "不太想动…" },
};
#define PET_LINE_COUNT (sizeof(petLines) / sizeof(PetLine))

// 宠物名。NVS 里没存过就给默认"小橘"，最长 8 字节（三个汉字）。
static char petNameBuf[12] = { 0 };
static const char* petName(void) {
    if (petNameBuf[0] == '\0') return "小橘";
    return petNameBuf;
}

// 时段问候：几点说什么。时间不准（NTP 缺失）时也只是问候错时段，不崩
static const char* petHourLine(uint8_t h) {    if (h < 5)  return "这么晚还不睡";
    if (h < 11) return "早上好呀";
    if (h < 14) return "中午要吃饭了";
    if (h < 18) return "下午加油鸭";
    if (h < 23) return "今天也辛苦啦";
    return "这么晚还不睡";
}

// 抽一句。clsMask 选分类，pose 选姿态，都传 0xFF / POSE_IDLE 就是不筛。
static const char* petPickLine(uint8_t clsMask, uint8_t pose, uint8_t mood) {
    static const PetLine* hit[8];
    uint8_t n = 0;
    for (uint16_t i = 0; i < PET_LINE_COUNT; i++) {
        const PetLine* p = &petLines[i];
        if (clsMask != 0xFF && (p->cls & clsMask) == 0) continue;
        if (p->pose != POSE_IDLE && p->pose != pose) continue;
        if (mood < p->moodMin) continue;
        if ((p->cls & 0xFF) == petLastLineCls) continue;   // 同一分类不连播
        if (n < 8) hit[n++] = p;
    }
    if (n == 0) return nullptr;   // 全被冷却掉了，调用方决定是不是不说话
    const PetLine* pick = hit[esp_random() % n];
    petLastLineCls = pick->cls;
    petLastLineMs  = millis();
    return pick->text;
}

static const char* petLineFor(uint8_t pose) {
    if (pose == POSE_IDLE) pose = pet.pose;
    return petPickLine(0xFF, pose, pet.mood);
}

// ===========================
// 宠物绘制：几何猫
// ===========================
// 全部是 lv_obj 基本图形：圆（radius=CIRCLE）、圆角矩形、细矩形当线。
// 一个三角形都画不出来（transform_angle 被禁），所以耳朵是圆的。
//
// 每个零件一个全局指针，姿态切换只改坐标/半径/显隐，**不重建对象** ——
// 重建的话 8 秒探头的 8s 内会闪好几下，而且每次重建都是几十个对象的分配。

#define CLR_PET       0xF5A97F
#define CLR_PET_DARK  0xC4805F
#define CLR_PET_BODY  0xE8C39E
#define CLR_PET_BLUSH 0xE8897A

// 全屏页
static lv_obj_t* pt_bg      = nullptr;
static lv_obj_t* pt_lblName = nullptr;
static lv_obj_t* pt_moodBg  = nullptr;
static lv_obj_t* pt_moodBar = nullptr;
static lv_obj_t* pt_shadow  = nullptr;
static lv_obj_t* pt_tail    = nullptr;
static lv_obj_t* pt_body    = nullptr;
static lv_obj_t* pt_earL    = nullptr;
static lv_obj_t* pt_earR    = nullptr;
static lv_obj_t* pt_earIL   = nullptr;
static lv_obj_t* pt_earIR   = nullptr;
static lv_obj_t* pt_head    = nullptr;
static lv_obj_t* pt_stripe1 = nullptr;
static lv_obj_t* pt_stripe2 = nullptr;
static lv_obj_t* pt_stripe3 = nullptr;
static lv_obj_t* pt_eyeL    = nullptr;
static lv_obj_t* pt_eyeR    = nullptr;
static lv_obj_t* pt_lidL    = nullptr;
static lv_obj_t* pt_lidR    = nullptr;
static lv_obj_t* pt_eyeBrowL= nullptr;
static lv_obj_t* pt_eyeBrowR= nullptr;
static lv_obj_t* pt_nose    = nullptr;
static lv_obj_t* pt_mouth   = nullptr;
static lv_obj_t* pt_blushL  = nullptr;
static lv_obj_t* pt_blushR  = nullptr;
static lv_obj_t* pt_bubble  = nullptr;
static lv_obj_t* pt_line1   = nullptr;
static lv_obj_t* pt_line2   = nullptr;
static lv_obj_t* pt_hint    = nullptr;
static lv_obj_t* pt_zs      = nullptr;   // 睡觉的 Z

// 探头横条（lv_layer_top 浮层）
static lv_obj_t* pk_bar     = nullptr;
static lv_obj_t* pk_head    = nullptr;
static lv_obj_t* pk_earL    = nullptr;
static lv_obj_t* pk_earR    = nullptr;
static lv_obj_t* pk_earIL   = nullptr;
static lv_obj_t* pk_earIR   = nullptr;
static lv_obj_t* pt_eyePkL  = nullptr;   // 探头的眼
static lv_obj_t* pt_eyePkR  = nullptr;
static lv_obj_t* pt_nosePk  = nullptr;
static lv_obj_t* pt_mouthPk = nullptr;
static lv_obj_t* pk_line1   = nullptr;
static lv_obj_t* pk_line2   = nullptr;
static lv_obj_t* pk_line3   = nullptr;
static lv_obj_t* pk_close   = nullptr;   // 右上角的 ✕ 提示

// 一根实心圆点/圆片。radius 传 LV_RADIUS_CIRCLE 就是正圆。
static lv_obj_t* petDot(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t* o = iconRect(parent, w, h, LV_ALIGN_TOP_LEFT, x, y, color, LV_RADIUS_CIRCLE);
    return o;
}

// 细线段：x1,y1 到 x2,y2，斜的用"竖直细条 + 旋转"做不到（transform_angle 被禁），
// 所以斜线只能画成"一串小方块"。这里给一条竖线/横线用（胡须省略，改用腮红+嘴传达）。
static lv_obj_t* petHLine(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    return iconRect(parent, w, h, LV_ALIGN_TOP_LEFT, x, y, color, 0);
}

// ---- 几何猫（全屏版）----
// 基准几何：头部中心固定在 (120, 104)，下面所有 y 都从这里推，
// 别再写死 56 / 134 这种"看起来能用"的魔数 —— 挪一次猫头就会全错。
#define PET_CX      120
#define PET_CY      104
#define PT_EAR_Y    (PET_CY - 48)
#define PT_TAIL_Y   134
#define PT_TAIL_X   152
#define PT_MOUTH_X  111
#define PT_MOUTH_Y  119
#define PT_LID_XL   99
#define PT_LID_XR   127
#define PT_BROW_XL  99
#define PT_BROW_XR  128

static void petBuildBody(lv_obj_t* parent) {
    pt_shadow = petDot(parent, PET_CX - 40, PET_CY + 48, 80, 14, 0x000000);
    pt_tail   = petHLine(parent, PT_TAIL_X, PT_TAIL_Y, 20, 6, CLR_PET);
    pt_body   = iconRect(parent, 68, 40, LV_ALIGN_TOP_LEFT, PET_CX - 34, PET_CY + 8, CLR_PET_BODY, 19);
    pt_earL   = petDot(parent, PET_CX - 37, PT_EAR_Y, 26, 26, CLR_PET);
    pt_earR   = petDot(parent, PET_CX + 11, PT_EAR_Y, 26, 26, CLR_PET);
    pt_earIL  = petDot(parent, PET_CX - 31, PT_EAR_Y + 7, 12, 12, CLR_PET_DARK);
    pt_earIR  = petDot(parent, PET_CX + 19, PT_EAR_Y + 7, 12, 12, CLR_PET_DARK);
    pt_head   = petDot(parent, PET_CX - 34, PET_CY - 43, 68, 68, CLR_PET);
    pt_stripe1= petHLine(parent, PET_CX - 8,  PET_CY - 29, 2, 10, CLR_PET_DARK);
    pt_stripe2= petHLine(parent, PET_CX - 1,  PET_CY - 29, 2, 10, CLR_PET_DARK);
    pt_stripe3= petHLine(parent, PET_CX + 6,  PET_CY - 29, 2, 10, CLR_PET_DARK);
    pt_eyeL   = petDot(parent, PET_CX - 20, PET_CY - 10, 12, 12, 0x1A1A1A);
    pt_eyeR   = petDot(parent, PET_CX + 8,  PET_CY - 10, 12, 12, 0x1A1A1A);
    pt_lidL   = petHLine(parent, PT_LID_XL, PET_CY - 10, 14, 10, CLR_PET);
    pt_lidR   = petHLine(parent, PT_LID_XR, PET_CY - 10, 14, 10, CLR_PET);
    pt_eyeBrowL=petHLine(parent, PT_BROW_XL, PET_CY - 20, 13, 3, CLR_PET_DARK);
    pt_eyeBrowR=petHLine(parent, PT_BROW_XR, PET_CY - 20, 13, 3, CLR_PET_DARK);
    pt_nose   = petDot(parent, PET_CX - 3, PET_CY + 8, 7, 5, 0xD2694A);
    pt_mouth  = petHLine(parent, PT_MOUTH_X, PT_MOUTH_Y, 18, 3, 0x1A1A1A);
    pt_blushL = petDot(parent, PET_CX - 30, PET_CY + 8, 12, 7, 0xE0A090);
    pt_blushR = petDot(parent, PET_CX + 18, PET_CY + 8, 12, 7, 0xE0A090);
    lv_obj_t* zbox = iconRect(parent, 16, 16, LV_ALIGN_TOP_LEFT,
                              PET_CX + 40, PET_CY - 50, 0x0B0F17, 0);
    pt_zs = lv_label_create(zbox);
    mkLabel(pt_zs, &lv_font_simsun_16_cjk, CLR_ACCENT);
    lv_label_set_text(pt_zs, "Z");
    lv_obj_align(pt_zs, LV_ALIGN_CENTER, 0, 0);   // label 相对那块小方块居中
}

// 姿态 → 改属性。**只改变了的东西**（setSizePos/setBgColor 内部有比对，
// 8 秒探头里反复切姿态也不会把屏幕刷爆）。
static void petApplyPose(uint8_t pose) {
    bool happy  = (pose == POSE_GREET || pose == POSE_HAPPY);
    bool hungry = (pose == POSE_HUNGRY);
    bool grumpy = (pose == POSE_GRUMPY);
    bool sleep  = (pose == POSE_SLEEP);
    bool sick   = (pose == POSE_SICK);

    // 眼：开心/困/病 → 眼珠藏起来，用眼皮线表达
    setHidden(pt_eyeL, happy || sleep || sick);
    setHidden(pt_eyeR, happy || sleep || sick);
    setHidden(pt_lidL, !(happy || sleep || sick));
    setHidden(pt_lidR, !(happy || sleep || sick));
    // 眼皮高度：困=半闭（盖一半），病=几乎全闭
    uint8_t lidH = sick ? 9 : 6;
    setSizePos(pt_lidL, 14, lidH, PT_LID_XL, PET_CY - 10);
    setSizePos(pt_lidR, 14, lidH, PT_LID_XR, PET_CY - 10);

    // 眉毛：只有生气才显（斜度画不出来，用显隐代替）
    setHidden(pt_eyeBrowL, !grumpy);
    setHidden(pt_eyeBrowR, !grumpy);

    // 嘴：一条短横线，靠"位置 + 长度"表达情绪
    int mW = 18, mY = PT_MOUTH_Y;
    if (happy)  mY -= 3;
    if (hungry) { mW = 14; mY += 3; }
    if (grumpy) { mW = 16; mY += 4; }
    if (sleep)  { mW = 10; mY += 0; }
    if (sick)   { mW = 12; mY += 2; }
    setSizePos(pt_mouth, mW, 3, PT_MOUTH_X + (18 - mW) / 2, mY);

    // 腮红：开心时明显
    setBgColor(pt_blushL, happy ? CLR_PET_BLUSH : 0xE0A090);
    setBgColor(pt_blushR, happy ? CLR_PET_BLUSH : 0xE0A090);

    // 耳朵：开心竖起来（上移 3），生气压平（下移 3 + 压扁）
    int earY = PT_EAR_Y, earH = 26;
    if (happy) earY -= 3;
    if (grumpy) { earY += 3; earH = 21; }
    setSizePos(pt_earL, 26, earH, PET_CX - 37, earY);
    setSizePos(pt_earR, 26, earH, PET_CX + 11, earY);
    // 耳内侧跟着耳朵走
    setSizePos(pt_earIL, 12, earH > 23 ? 12 : 9, PET_CX - 31, earY + 7);
    setSizePos(pt_earIR, 12, earH > 23 ? 12 : 9, PET_CX + 19, earY + 7);

    // 尾巴：开心翘起来（往上 14px + 拉长），其余垂着
    setSizePos(pt_tail, happy ? 22 : 20, 6, PT_TAIL_X, happy ? PT_TAIL_Y - 14 : PT_TAIL_Y);

    // 生病整体压暗（没有 alpha 通道可用，只能换颜色）
    uint32_t fur = sick ? 0xD9A98A : CLR_PET;
    setBgColor(pt_head, fur);
    setBgColor(pt_earL, fur);
    setBgColor(pt_earR, fur);
    setBgColor(pt_body, sick ? 0xD8BFA0 : CLR_PET_BODY);

    // 睡觉的 Z
    setHidden(pt_zs, !sleep);
}

// ===========================
// 全屏宠物页（SCENE_FULL）
// ===========================
// 竖向预算（每一行都是量过的，别乱加）：
//   0..30    顶栏：名字 + 心情条
//   56..166  猫 110x112
//   168..218 对话气泡（2 行，行盒 19px）
//   219..238 键位提示（1 行）
// 台词最多 2 行 x 13 字；键位提示一行放得下"静音摸 · 旋钮撸 · 灯长退"（168px）。
static lv_obj_t* scr_pet = nullptr;

static void petDestroyPage(void) {
    if (scr_pet) {
        // 删之前必须先换屏：LVGL 8.4 删掉活动屏会把 disp->act_scr 置 NULL
        // （见 swapAwayIfActive 的注释，本项目已经为这个坑栽过两次）
        swapAwayIfActive(scr_pet, nullptr);
        if (currentScreen == scr_pet) currentScreen = nullptr;
        lv_obj_del(scr_pet);
        scr_pet = nullptr;
    }
    pt_bg = pt_lblName = pt_moodBg = pt_moodBar = nullptr;
    pt_shadow = pt_tail = pt_body = nullptr;
    pt_earL = pt_earR = pt_earIL = pt_earIR = pt_head = nullptr;
    pt_stripe1 = pt_stripe2 = pt_stripe3 = nullptr;
    pt_eyeL = pt_eyeR = pt_lidL = pt_lidR = nullptr;
    pt_eyeBrowL = pt_eyeBrowR = nullptr;
    pt_nose = pt_mouth = pt_blushL = pt_blushR = nullptr;
    pt_bubble = pt_line1 = pt_line2 = pt_hint = pt_zs = nullptr;
}

// 两行台词。long 模式一律 DOT 兜底：超宽就变省略号，绝不画到屏幕外。
static void petSetSpeech(const char* l1, const char* l2) {
    if (pt_line1 == nullptr) return;
    setText(pt_line1, l1 ? l1 : "");
    setText(pt_line2, l2 ? l2 : "");
    setHidden(pt_line2, (l2 == nullptr || l2[0] == '\0'));
}

static void petRefreshPage(void) {
    if (scr_pet == nullptr) return;

    // 心情条
    uint8_t m = pet.mood > 100 ? 100 : pet.mood;
    setSizePos(pt_moodBar, (m * 128) / 100, 10, 100, 9);
    setBgColor(pt_moodBar, m > PET_MOOD_OK ? CLR_GREEN
                                          : (m > PET_MOOD_SAD ? CLR_AMBER : CLR_RED));

    petApplyPose(pet.pose);
    petSetSpeech(petLineFor(pet.pose), nullptr);
}

static void petBuildPage(void) {
    petDestroyPage();
    scr_pet = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_pet, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(scr_pet, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr_pet, LV_OBJ_FLAG_SCROLLABLE);
    pt_bg = makeRootPanel(scr_pet, CLR_BG);

    // 顶栏：名字（中文库）+ 心情条槽
    pt_lblName = lv_label_create(pt_bg);
    mkLabel(pt_lblName, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_obj_set_width(pt_lblName, 80);
    lv_label_set_long_mode(pt_lblName, LV_LABEL_LONG_DOT);
    lv_label_set_text(pt_lblName, petName());
    lv_obj_align(pt_lblName, LV_ALIGN_TOP_LEFT, 12, 4);

    pt_moodBg = iconRect(pt_bg, 128, 10, LV_ALIGN_TOP_LEFT, 100, 9, CLR_SURFACE_2, 5);
    pt_moodBar = iconRect(pt_moodBg, 0, 10, LV_ALIGN_TOP_LEFT, 0, 0, CLR_GREEN, 5);
    lv_obj_clear_flag(pt_moodBg, LV_OBJ_FLAG_SCROLLABLE);
    petHLine(pt_bg, 12, 28, 216, 1, CLR_STROKE);

    petBuildBody(pt_bg);

    // 对话气泡
    pt_bubble = iconRect(pt_bg, 216, 50, LV_ALIGN_TOP_LEFT, 12, 168, 0x141A26, 8);
    lv_obj_set_style_border_width(pt_bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(pt_bubble, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    pt_line1 = lv_label_create(pt_bubble);
    mkLabel(pt_line1, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_obj_set_width(pt_line1, 200);
    lv_label_set_long_mode(pt_line1, LV_LABEL_LONG_DOT);
    lv_obj_align(pt_line1, LV_ALIGN_TOP_LEFT, 8, 6);
    pt_line2 = lv_label_create(pt_bubble);
    mkLabel(pt_line2, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_obj_set_width(pt_line2, 200);
    lv_label_set_long_mode(pt_line2, LV_LABEL_LONG_DOT);
    lv_obj_align(pt_line2, LV_ALIGN_TOP_LEFT, 8, 25);

    // 键位提示：一行。全是 C3 实体键，矩阵键一个都不占。
    pt_hint = mkHintLine(pt_bg, 220, "静音摸 · 旋钮撸 · 灯长退");

    petRefreshPage();
}

static void petEnterFull(void) {
    if (petScene == SCENE_FULL && scr_pet) return;
    petDestroyPeek();
    petScene = SCENE_FULL;
    pet.pose = POSE_GREET;
    pet.poseUntilMs = millis() + 4000;
    petBuildPage();
    currentSysMode = SYS_MODE_PET;
    showScreen(scr_pet);
    lastActivityTime = millis();
    pet.lastSeenMs = millis();
}

static void petExitFull(void) {
    if (petScene != SCENE_FULL) return;
    petScene = SCENE_NONE;
    petDestroyPage();
    gotoMainScreen();
}

// ===========================
// 底部让位清单
// ===========================
// 探头横条 y=168..240，会盖住底部。查过 7 种主屏风格，**只有两种**底部有
// 内容落在这个区间：高对比度的底部状态条（y=178..235）和壁纸右下角的
// 时钟板（y=190..230）。其余五种内容都在 y<168，天然不冲突。
//
// ⚠️ 底部条不是一棵树，是 12 个平铺对象：10 个具名全局（6 个 label + 4 个
// 进度条）加上装饰框和竖分隔线两个 iconRect，后两个在 build_style_* 里是
// 局部变量、出了函数就没句柄。**所以不要 re-parent 成一个容器** —— 那要把
// 10 个对象的 y 从 180/199/209/228 改成 2/21/31/50，动的是已经逐像素调好的
// 版面，还正好踩上 README 里 pad 偏移那条雷。收一个指针清单是最小改动。
//
// 数组开 16 而不是 12：开 12 的话以后有人加一个对象就静默越界。
#define PET_YIELD_MAX 16
static lv_obj_t* petYieldObjs[PET_YIELD_MAX] = { nullptr };
static uint8_t   petYieldCount    = 0;

static void petYieldAdd(lv_obj_t* o) {
    if (o == nullptr) return;
    if (petYieldCount >= PET_YIELD_MAX) return;   // 满了就丢：宁可漏让位也别越界
    petYieldObjs[petYieldCount++] = o;
}

static void petYieldClear(void) {
    for (uint8_t i = 0; i < PET_YIELD_MAX; i++) petYieldObjs[i] = nullptr;
    petYieldCount = 0;
}

static void petSetBottomYield(bool yield) {
    for (uint8_t i = 0; i < petYieldCount; i++) {
        if (petYieldObjs[i] == nullptr) continue;
        setHidden(petYieldObjs[i], yield);
    }
}

// ===========================
// 探头横条（SCENE_PEEK）
// ===========================
// 挂在 lv_layer_top 上：底下是主屏 / 菜单 / 设置页都一样能盖住，
// 不需要为了显示它去改 currentSysMode（同 drawRingOverlay 的做法）。
// 竖向预算：72px 只装得下 3 行（72/19 = 3.8）—— 台词 2 行 + 关闭提示 1 行。
// 台词区 x=64..232 = 168px -> 每行 10 字。✕ 放提示行右端而不是右上角，
// 右上角会吃掉台词第一行的宽度。

#define PK_TOP    168
#define PK_HEAD_CX 36
#define PK_HEAD_CY 204

static void petDestroyPeek(void) {
    if (pk_bar) {
        lv_obj_del(pk_bar);        // 挂在 layer_top 上，删它不影响活动屏
        pk_bar = nullptr;
    }
    pk_head = pk_earL = pk_earR = pk_earIL = pk_earIR = nullptr;
    pt_eyePkL = pt_eyePkR = pt_nosePk = pt_mouthPk = nullptr;
    pk_line1 = pk_line2 = pk_line3 = pk_close = nullptr;
    petSetBottomYield(false);
}

// 探头的猫：只画到下巴，肩膀在横条之外，视觉上就是"从屏底探出来"
static void petBuildPeek(void) {
    petDestroyPeek();
    pk_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(pk_bar, 240, 72);
    lv_obj_set_pos(pk_bar, 0, PK_TOP);
    lv_obj_set_style_bg_color(pk_bar, lv_color_hex(0x141A26), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pk_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(pk_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(pk_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pk_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(pk_bar, LV_OBJ_FLAG_SCROLLABLE);

    int cx = PK_HEAD_CX, cy = PK_HEAD_CY;
    pk_earL  = petDot(pk_bar, cx - 21, cy - 30, 14, 14, CLR_PET);
    pk_earR  = petDot(pk_bar, cx + 7,  cy - 30, 14, 14, CLR_PET);
    pk_earIL = petDot(pk_bar, cx - 18, cy - 26, 6, 6, CLR_PET_DARK);
    pk_earIR = petDot(pk_bar, cx + 12, cy - 26, 6, 6, CLR_PET_DARK);
    pk_head  = petDot(pk_bar, cx - 20, cy - 26, 40, 40, CLR_PET);
    pt_eyePkL  = petDot(pk_bar, cx - 12, cy - 12, 8, 8, 0x1A1A1A);
    pt_eyePkR  = petDot(pk_bar, cx + 4,  cy - 12, 8, 8, 0x1A1A1A);
    pt_nosePk  = petDot(pk_bar, cx - 3,  cy - 1,  6, 4, 0xD2694A);
    pt_mouthPk = petHLine(pk_bar, cx - 8, cy + 7, 16, 2, 0x1A1A1A);

    pk_line1 = lv_label_create(pk_bar);
    pk_line2 = lv_label_create(pk_bar);
    pk_line3 = lv_label_create(pk_bar);
    lv_obj_t* ls[3] = { pk_line1, pk_line2, pk_line3 };
    uint32_t   cols[3] = { CLR_TEXT, CLR_TEXT_DIM, CLR_TEXT_DIM };
    for (int i = 0; i < 3; i++) {
        mkLabel(ls[i], &lv_font_simsun_16_cjk, cols[i]);
        lv_obj_set_width(ls[i], 168);
        lv_label_set_long_mode(ls[i], LV_LABEL_LONG_DOT);
        lv_obj_align(ls[i], LV_ALIGN_TOP_LEFT, 64, 3 + 19 * i);
    }
    lv_label_set_text(pk_line3, "长按 LOGO 收起我");

    // 关闭提示的 ✕：一个 16px 圆圈 + 里面一个 Montserrat 的 "X"。
    // 为什么用字形而不是两条斜线：transform_angle 被禁（见 README），
    // 斜线只能画成一串小方块，边缘全是锯齿。ASCII 的 X 正好就是那个形状，
    // 而且 Montserrat 一定有它 —— 不用碰字库。
    lv_obj_t* cb = iconRect(pk_bar, 16, 16, LV_ALIGN_TOP_LEFT, 212, 211, 0x1A1A1A, 8);
    pk_close = lv_label_create(cb);
    mkLabel(pk_close, &lv_font_montserrat_14, CLR_TEXT_DIM);
    lv_label_set_text(pk_close, "X");
    lv_obj_align(pk_close, LV_ALIGN_CENTER, 0, 0);

    petSetBottomYield(true);
}

// 探头说什么。优先时段问候，其次按心情挑一句。
static void petPeekSpeak(void) {
    if (pk_line1 == nullptr) return;
    time_t t = time(nullptr);
    struct tm tmNow;
    const char* hour = nullptr;
    if (localtime_r(&t, &tmNow)) hour = petHourLine((uint8_t)tmNow.tm_hour);
    setText(pk_line1, hour ? hour : "我在这儿");
    const char* l2 = petPickLine(0x02, pet.pose, pet.mood);
    if (l2 == nullptr) l2 = petPickLine(0x01, pet.pose, pet.mood);
    setText(pk_line2, l2 ? l2 : "");
    setHidden(pk_line2, l2 == nullptr);
}

static void petStartPeek(void) {
    petDestroyPeek();
    petScene = SCENE_PEEK;
    petPeekStartMs = millis();
    pet.pose = POSE_GREET;
    petBuildPeek();
    petPeekSpeak();
    pet.peekToday++;
    preferences.putUChar("pet_peek_n", pet.peekToday);
}

// 用户主动关掉：收起 + 记一次打扰。连续 2 次就安静一天。
static void petDismiss(void) {
    if (petScene != SCENE_PEEK) return;
    petDestroyPeek();
    petScene = SCENE_NONE;
    pet.pose = POSE_GRUMPY;
    pet.mood = pet.mood > 20 ? pet.mood - 10 : 0;
    if (pet.bothered < 250) pet.bothered++;
    petSaveMood();
    if (pet.bothered >= PET_BOTHER_LIMIT) {
        time_t t = time(nullptr);
        struct tm tmNow;
        if (localtime_r(&t, &tmNow)) {
            uint32_t ymd = (tmNow.tm_year + 1900) * 10000
                         + (tmNow.tm_mon + 1) * 100 + tmNow.tm_mday;
            pet.muteYmd = ymd + 1;                 // 安静到明天同一时刻
            pet.bothered = 0;
            preferences.putUInt("pet_mute_y", pet.muteYmd);
        }
        triggerHud("小橘", "那我安静一天", lv_color_hex(CLR_TEXT_DIM));
    }
}

// ===========================
// 互动
// ===========================
// 摸 / 撸：静音键和旋钮进来。有冷却，否则按住旋钮能一路刷满亲密度。
// 互动时**不回顶部窗口**（832-834 那段）—— 在宠物页里回主屏毫无意义。
static bool petPetted(uint8_t n) {
    unsigned long now = millis();
    if (now - pet.lastPettedMs < PET_PET_COOLDOWN_MS) return false;
    pet.lastPettedMs = now;
    if (pet.mood < 250 - n) pet.mood = (uint8_t)(pet.mood + n);
    if (pet.bond < 60000) pet.bond += 1;
    pet.pose = POSE_HAPPY;
    pet.poseUntilMs = now + 2500;

    // 在探头里摸它 = 被欢迎，打扰计数减一
    if (petScene == SCENE_PEEK && pet.bothered > 0) pet.bothered--;

    if (petScene == SCENE_FULL) {
        // ⚠️ 一次互动里**只能调一次** petPickLine()：它内部会写 petLastLineCls
        // 做"同分类不连播"的去重，第二次调用会把第一次刚抽中的分类直接排除掉。
        const char* l = petPickLine(0x02, pet.pose, pet.mood);
        petApplyPose(pet.pose);
        petSetSpeech(l ? l : "呼噜呼噜…", nullptr);
    } else if (petScene == SCENE_PEEK) {
        petPeekSpeak();
    }
    return true;
}

static void petFed(void) {
    time_t t = time(nullptr);
    struct tm tmNow;
    uint32_t ymd = 0;
    if (localtime_r(&t, &tmNow)) {
        ymd = (tmNow.tm_year + 1900) * 10000 + (tmNow.tm_mon + 1) * 100 + tmNow.tm_mday;
    }
    if (ymd && pet.fedYmd != ymd) {   // 跨日：喂食次数归零
        pet.fedYmd = ymd;
        pet.fedToday = 0;
        preferences.putUInt("pet_fed_y", ymd);
    }
    pet.fedToday++;
    preferences.putUChar("pet_fed_n", pet.fedToday);
    pet.mood = pet.mood < 250 ? (uint8_t)(pet.mood + 15) : 100;
    pet.pose = POSE_HAPPY;
    pet.poseUntilMs = millis() + 3000;
    if (petScene == SCENE_FULL) {
        petApplyPose(pet.pose);
        petSetSpeech("吃饱了，谢谢你", nullptr);
        petRefreshPage();
    } else if (petScene == SCENE_PEEK) {
        petPeekSpeak();
    }
}

static void petSaveMood(void) {
    preferences.putUChar("pet_mood", pet.mood);
    preferences.putUInt("pet_bond", pet.bond);
}

// ===========================
// 出场仲裁
// ===========================
// 判定**必须集中在这一个函数里** —— 照 updateCountdownVisibility 的教训：
// 散在各处迟早漏一处，表现就是"进了菜单底下还冒探头"。
//
// 顺序有讲究：SCENE_FULL 必须排在"界面态"这条**前面**。宠物页自己是
// SYS_MODE_PET（!= SYS_MODE_NORMAL），放后面的话永远走不到，用户一进
// 宠物页它就把自己关掉了。
static PetScene petGate(void) {
    if (ringingKind != RING_NONE)           return SCENE_NONE;   // 响铃压过一切
    if (petScene == SCENE_FULL)            return SCENE_FULL;   // 用户主动进的不踢
    if (currentSysMode == SYS_MODE_SLEEP)   return SCENE_SAVER;  // 屏保归屏保管
    if (currentSysMode != SYS_MODE_NORMAL)  return SCENE_NONE;   // 菜单/设置/录制/日志
    if (countdownShown && timerRunning)     return SCENE_NONE;
    if (notifCount > 0)                     return SCENE_NONE;   // 通知告警优先
    if (millis() - lastActivityTime < 3000) return SCENE_NONE;   // 刚敲过键，别打断
    if (!pet.peekOn)                        return SCENE_NONE;
    return petPeekAllowed() ? SCENE_PEEK : SCENE_NONE;
}

// 探头该不该来：时段、每日上限、24h 静默、判定节拍。**全都硬约束** ——
// 会主动出现的东西不收敛频率，三天就被拔电。
static bool petPeekAllowed(void) {
    if (pet.bothered >= PET_BOTHER_LIMIT)   return false;
    unsigned long now = millis();
    if (now < petPeekNextCheckMs)           return false;
    // 20~40s 随机间隔：固定间隔会被训练出"到点必有东西"的预期
    petPeekNextCheckMs = now + PET_PEEK_GAP_MIN_MS
                       + (esp_random() % PET_PEEK_GAP_JIT_MS);

    if (now - lastActivityTime < PET_PEEK_IDLE_MIN_MS) return false;
    if (pet.peekToday >= PET_PEEK_DAILY_MAX)           return false;

    time_t t = time(nullptr);
    struct tm tmNow;
    if (!localtime_r(&t, &tmNow)) return false;
    uint32_t ymd = (tmNow.tm_year + 1900) * 10000 + (tmNow.tm_mon + 1) * 100 + tmNow.tm_mday;

    // 24 小时静默。⚠ 判据是 ymd >= muteYmd（跨日的整数比较），不是判"今天
    // 还在暂停期里"：muteYmd 存的是到期那天的 YYYYMMDD，用等号判会在到期
    // 那天早上 00:00 就解封，比"安静一整天"实际短了 24 小时。
    if (pet.muteYmd != 0) {
        if (ymd >= pet.muteYmd) {
            pet.muteYmd = 0;
            pet.bothered = 0;
            preferences.putUInt("pet_mute_y", 0);
            preferences.putUChar("pet_bother", 0);
        } else {
            return false;
        }
    }

    // 跨日：探头次数清零
    if (ymd != pet.peekYmd) {
        pet.peekYmd = ymd;
        pet.peekToday = 0;
        pet.bothered = 0;
        preferences.putUInt("pet_peek_y", ymd);
        preferences.putUChar("pet_peek_n", 0);
    }

    // 显示时段。⚠ from > to 是跨零点（默认 23:00-07:00 就是），
    // 写成 h >= from && h < to 会永远不成立，等于探头被静默关掉。
    uint8_t h = (uint8_t)tmNow.tm_hour;
    bool inWin;
    if (pet.fromH == pet.toH)              inWin = true;
    else if (pet.fromH < pet.toH)          inWin = (h >= pet.fromH && h < pet.toH);
    else                                   inWin = (h >= pet.fromH || h < pet.toH);
    if (!inWin) return false;

    return (int)(esp_random() % 100) < PET_PEEK_CHANCE;
}

// ===========================
// 每轮轮询
// ===========================
// 头像条的消失、姿态回 IDLE、探头超时都在这里。
// ⚠️ 这里**绝不能**顺手去跳过 updateDynamicElements()：lockStateDirty 是靠
// 它内部清的，跳过会退化成每轮 loop 都往 C3 推 61 字节灯帧、Serial1 堵死，
// 6 秒后监测任务判定卡死 → esp_restart()。见 loop() 里 9680 那段注释。
static void petPoll(void) {
    unsigned long now = millis();

    // 姿态到点自然回 IDLE（不是在外部 tick 驱动的）
    if (pet.pose != POSE_IDLE && pet.poseUntilMs != 0 && now >= pet.poseUntilMs) {
        pet.pose = POSE_IDLE;
        pet.poseUntilMs = 0;
    }

    // 探头的 8 秒到了就自己缩回去
    if (petScene == SCENE_PEEK && now - petPeekStartMs > PET_PEEK_HOLD_MS) {
        petDestroyPeek();
        petScene = SCENE_NONE;
    }

    // 长期不互动 → 越来越闷
    if (petScene == SCENE_NONE && pet.lastSeenMs != 0
        && now - pet.lastSeenMs > 1800000UL) {
        if (pet.mood > 5) pet.mood--;
    }

    PetScene want = petGate();
    if (want == SCENE_PEEK && petScene != SCENE_PEEK) {
        petStartPeek();
    } else if (want == SCENE_NONE && petScene == SCENE_PEEK) {
        petDestroyPeek();          // 让位：响铃/通知/进菜单/刚敲了键
        petScene = SCENE_NONE;
    }
}

// ===========================
// NVS
// ===========================
// ⚠️ pet_peek_on 的默认值是 **true**：Preferences::getBool 的第二个参数是
// "键不存在时返回什么"，传 false 等于默认关，与需求相反。
//
// 状态一律走 GSET:/直接 preferences 写，**别用 SET:** —— SET: 会自动给键名
// 加方案号前缀（见 handleCommand 里的 hasProfPrefix）。
static void petLoadFromNvs(void) {
    pet.mood     = preferences.getUChar("pet_mood", 60);
    pet.bond     = preferences.getUShort("pet_bond", 0);
    pet.fedToday = preferences.getUChar("pet_fed_n", 0);
    pet.fedYmd   = preferences.getUInt("pet_fed_y", 0);
    pet.peekToday= preferences.getUChar("pet_peek_n", 0);
    pet.peekYmd  = preferences.getUInt("pet_peek_y", 0);
    pet.bothered = preferences.getUChar("pet_bother", 0);
    pet.muteYmd  = preferences.getUInt("pet_mute_y", 0);
    pet.peekOn   = preferences.getBool("pet_peek_on", true);
    pet.fromH    = preferences.getUChar("pet_from_h", 23);
    pet.toH      = preferences.getUChar("pet_to_h", 7);
    if (pet.mood > 100) pet.mood = 60;
    if (pet.fromH > 23) pet.fromH = 23;
    if (pet.toH   > 23) pet.toH   = 7;
    pet.pose = POSE_IDLE;
    pet.poseUntilMs = 0;
    petPeekNextCheckMs = millis() + PET_PEEK_GAP_MIN_MS;
    petLastLineMs = 0;
    petLastLineCls = 0xFFFFFFFF;

    String nm = preferences.getString("pet_name", "");
    nm.trim();
    if (nm.length() == 0) {
        snprintf(petNameBuf, sizeof(petNameBuf), "小橘");
        preferences.putString("pet_name", petNameBuf);
    } else {
        nm.toCharArray(petNameBuf, sizeof(petNameBuf));
        petNameBuf[sizeof(petNameBuf) - 1] = '\0';
    }
}

// ===========================
// 菜单动作
// ===========================
// 直接吃 MenuAction 枚举，不再另编一套动作号 —— 两套编号迟早会对不上，
// 而且宏必须在文本上先定义（runMenuAction 在 3000 行，这里在 3900 行，
// 用 #define 做转发会报"未声明"）。枚举和函数则不受位置限制。
static uint8_t petMenuField = 0;   // 时段那一项里：0=起始小时 1=结束小时

static void petMenuAction(MenuAction act) {
    char buf[24];
    switch (act) {
        case MA_PET_PEEK_ON:
            pet.peekOn = !pet.peekOn;
            preferences.putBool("pet_peek_on", pet.peekOn);
            triggerHud("主动打招呼", pet.peekOn ? "已开启" : "已关闭",
                       lv_color_hex(pet.peekOn ? CLR_GREEN : CLR_TEXT_DIM));
            break;
        case MA_PET_WINDOW:
            // ←→ 调起、↑↓ 调止。跨零点合法（23->07），不拦。
            if (petMenuField == 0) {
                pet.fromH = (uint8_t)((pet.fromH + 1) % 24);
            } else {
                pet.toH = (uint8_t)((pet.toH + 1) % 24);
            }
            preferences.putUChar("pet_from_h", pet.fromH);
            preferences.putUChar("pet_to_h", pet.toH);
            snprintf(buf, sizeof(buf), "%02u:00 - %02u:00", pet.fromH, pet.toH);
            triggerHud("显示时段", buf, lv_color_hex(CLR_ACCENT));
            break;
        case MA_PET_OPEN:
            petEnterFull();
            break;
        case MA_PET_RENAME:
            triggerHud("改名", "暂未开放", lv_color_hex(CLR_TEXT_DIM));
            break;
        case MA_PET_RESET:
            pet.bond = 0;
            pet.mood = 60;
            pet.bothered = 0;
            petSaveMood();
            preferences.putUChar("pet_bother", 0);
            triggerHud("亲密度", "已清零", lv_color_hex(CLR_ACCENT));
            break;
        default:
            break;
    }
}

// ===========================
// HUD 浮层（操作反馈提示条）
// ===========================
// 这张卡是"操作有没有生效"的唯一反馈，所以按可读性优先设计：
//   纯黑底 + 2px 语义色描边 —— 240x240 上对比最强的组合，不管底下是什么画面都压得住；
//   标题（三级色）与数值（主文字色）分层，数值用中文字体最大号；
//   标题和数值都水平居中，余光扫一眼就能读到。
// 之前的版本是暗面卡片 + 左侧竖条，底色和主屏卡片几乎同色，
// 提示"看起来像没有"，用户根本不知道刚才那下按没按上。
#define HUD_H 64
static void build_hud(void) {
    if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }

    scr_hud = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_hud, 216, HUD_H);
    lv_obj_align(scr_hud, LV_ALIGN_CENTER, 0, 30);
    mkCard(scr_hud, 0x000000, 12);
    lv_obj_set_style_border_width(scr_hud, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr_hud, hud.color, LV_PART_MAIN);
    lv_obj_move_foreground(scr_hud);

    // 左侧语义色竖条：颜色跟着提示语义走，一眼能分辨成功/警告/错误
    lv_obj_t* bar = lv_obj_create(scr_hud);
    lv_obj_set_size(bar, 5, HUD_H - 12);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(bar, hud.color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 标题（小、三级色、居中）
    lv_obj_t* title = lv_label_create(scr_hud);
    mkLabel(title, &lv_font_simsun_16_cjk, CLR_TEXT_DIM);
    lv_label_set_text(title, hud.title);
    lv_obj_set_width(title, 184);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 9);

    // 数值（大、主文字色、居中）—— 卡片主体
    lv_obj_t* value = lv_label_create(scr_hud);
    mkLabel(value, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(value, hud.value);
    lv_obj_set_width(value, 184);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void triggerHud(const char* title, const char* value, lv_color_t color) {
    hud.active = true;
    strncpy(hud.title, title, sizeof(hud.title) - 1);
    hud.title[sizeof(hud.title) - 1] = '\0';
    strncpy(hud.value, value, sizeof(hud.value) - 1);
    hud.value[sizeof(hud.value) - 1] = '\0';
    hud.color = color;          // 原样存 lv_color_t，别再 lv_color_to32 来回转
    hud.startMs = millis();
    hud.showMs = 1500;

    build_hud();
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

    if (notifCount == 0) {
        triggerHud("通知已处理", "全部清除", lv_color_hex(CLR_GREEN));
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "还剩 %d 条", notifCount);
        triggerHud("已确认", buf, lv_color_hex(CLR_GREEN));
    }
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

    // 响铃比 HUD 重要：先把浮层让出来，别让一条 3 秒的提示压在响铃上面
    if (hud.active) {
        hud.active = false;
        if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
    }

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
    const bool wantIt = countdownShown && timerRunning
                        && (ringingKind == RING_NONE)
                        && (currentSysMode == SYS_MODE_NORMAL);
    if (!wantIt) {
        if (scr_countdown) destroyCountdownScreen();
        return;
    }
    if (currentScreen != scr_countdown) buildCountdownScreen();
    else                               updateCountdownScreenText();
}

// 启停倒计时的唯一入口：设时长、起表、把全屏叫起来。
// 网页的 TIMERSET: 和键盘设置页的回车键都走这里，避免两处各写一份
// "谁负责把 countdownShown 置上"的初始化。
static void startCountdown(uint32_t totalSec) {
    if (totalSec == 0) {
        triggerHud("倒计时", "时长不合法", lv_color_hex(CLR_RED));
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
            triggerHud("倒计时", "时长不合法", lv_color_hex(CLR_RED));
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

            // 第二行：**级别字样 + 正文**（16px 中文，行高 19，可折两行）。
            // 级别放在正文前面而不是单独一行，是为了把行高压回 58：
            // 每条只占两行，三条就填满列表区。
            lv_obj_t* l2 = lv_label_create(row);
            mkLabel(l2, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(l2, "");
            lv_obj_set_width(l2, 206);
            lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
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
            triggerHud("清空日志", "再按一次删除键", lv_color_hex(CLR_AMBER));
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
            triggerHud("时间", "已保存", lv_color_hex(CLR_GREEN));
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
            triggerHud("闹钟", alarmEnabled ? "已开启" : "已关闭",
                lv_color_hex(alarmEnabled ? CLR_GREEN : CLR_TEXT_DIM));
            break;
        }
        case SYS_MODE_SET_TIMER: {
            timerRunning = false;
            timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
            timerRemainSec = timerTotalSec;
            triggerHud("倒计时", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
        case SYS_MODE_CAL_TEMP: {
            preferences.putFloat("sht_offset", shtTempOffset);
            triggerHud("温度校准", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
        case SYS_MODE_SET_LIGHT: {
            preferences.putBool("light_on", lightOn);
            preferences.putUChar("brightness", brightness);
            preferences.putUChar("effect", currentEffect);
            preferences.putUChar("ind_level", indLevel);
            preferences.putUChar("key_fx", keyFxStyle);
            triggerHud("灯光", "已保存", lv_color_hex(CLR_GREEN));
            break;
        }
    }
    gotoMainScreen();
}

static void cancelSettingScreen(void) {
    switch (currentSysMode) {
        case SYS_MODE_SET_TIME:
            triggerHud("时间", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_SET_ALARM:
            triggerHud("闹钟", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_SET_TIMER:
            timerRunning = false;
            triggerHud("倒计时", "已取消", lv_color_hex(CLR_AMBER));
            break;
        case SYS_MODE_CAL_TEMP:
            shtTempOffset = calTempOriginal;
            triggerHud("温度校准", "已取消", lv_color_hex(CLR_AMBER));
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
            triggerHud("灯光", "已取消", lv_color_hex(CLR_AMBER));
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
    triggerHud("配置方案", profileNamesCN[currentProfile], lv_color_hex(CLR_VIOLET));
    // 顶部条的序号 + 系统图标立刻跟上。等 updateDynamicElements() 那 100ms 也行，
    // 但它开头有"系统时间未校准就 return"的分支，靠它会出现图标不跟着换。
    updateTopBarProfile();
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

static uint16_t getMappedKey(uint16_t originalKey) {
    for (int i = 0; i < remapCounts[currentProfile]; i++) {
        if (profileRemaps[currentProfile][i].fromKey == originalKey) {
            return profileRemaps[currentProfile][i].toKey;
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

// delay() 期间 loopTask 是被挂起的，而 loopTask 挂在任务看门狗上
// （setup() 里 esp_task_wdt_add(NULL)），所以宏里一个 SLEEP(1000) 就能触发
// 看门狗复位。凡是宏里要等的地方都走这个函数，边等边喂狗。
static void kbSafeDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        esp_task_wdt_reset();
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

static void executeMacro(String keyName) {
    // ME 键是特殊的：它带的内容不是"按什么键"，而是一整段要贴给主机的文本。
    // 网页端蓝牙过来时只写 /me_hex.txt（HEX 字符串），按下 ME 键才真的打字。
    // 外面套 [HEXS] / [HEXE] 是主机端识别用的标记 —— 纯 ASCII 打字发中文根本
    // 做不到，hex 走一遍协议才能把 UTF-8 字节原样送到主机。
    if (keyName == "ME") {
        LOG_PORT.println("[ME] key pressed");
        if (!SPIFFS.exists("/me_hex.txt")) {
            LOG_PORT.println("[ME] /me_hex.txt not found");
            triggerHud("ME 文本", "尚未设置", lv_color_hex(CLR_TEXT_DIM));
            return;
        }
        File f = SPIFFS.open("/me_hex.txt", FILE_READ);
        if (!f) {
            LOG_PORT.println("[ME] open /me_hex.txt FAILED");
            triggerHud("ME 文本", "打开失败", lv_color_hex(CLR_RED));
            return;
        }
        const size_t total = f.size();
        LOG_PORT.printf("[ME] sending %u bytes, freeHeap=%u\n",
                        (unsigned)total, (unsigned)ESP.getFreeHeap());
        Keyboard.print("[HEXS]");
        delay(20);
        size_t sent = 0;
        size_t skipped = 0;
        uint32_t lastReportMs = millis();
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            // ★ 关键安全修：跳过所有 >= 0x80 的字节。
            // Keyboard.print((char)c) 走的是 USBHIDKeyboard::press()，
            // 对 0x80~0xFF 会进入 `modifiers |= (1 << (k-0x80))` 分支。
            // 当 k-0x80 >= 8（即 k >= 0x88）时，左移数超过 modifier byte 位宽
            // 是 C/C++ 未定义行为 —— Xtensa 上 5K+ 中文 UTF-8 字节（0xE4/0xB8/0xAD
            // 这种）反复触发，踩坏堆就会重启。中文 ME 真正能发的通道是另开的
            // BLE notify raw byte，不是这次的范围 —— 安全起见先一律跳过。
            if (c >= 0x80) { skipped++; sent++; continue; }
            Keyboard.print((char)c);
            sent++;
            // 每 64 字节额外让一次时间片 + 喂一次狗 —— 1ms delay() 不够给 USB host
            // 留 ACK 时间片，端点缓冲吃紧时 SendReport 可能短暂阻塞。
            if ((sent & 0x3F) == 0) {
                esp_task_wdt_reset();
                delay(2);
            }
            // 每 ~500ms 报一次进度，万一真崩了能看出崩在哪一段
            if (millis() - lastReportMs > 500) {
                esp_task_wdt_reset();
                LOG_PORT.printf("[ME] %u/%u (skip=%u) freeHeap=%u\n",
                                (unsigned)sent, (unsigned)total,
                                (unsigned)skipped, (unsigned)ESP.getFreeHeap());
                lastReportMs = millis();
            }
        }
        f.close();
        delay(20);
        Keyboard.print("[HEXE]");
        LOG_PORT.printf("[ME] done %u/%u (skip=%u) freeHeap=%u\n",
                        (unsigned)sent, (unsigned)total,
                        (unsigned)skipped, (unsigned)ESP.getFreeHeap());
        if (skipped > 0) {
            char buf[40];
            snprintf(buf, sizeof(buf), "已发 %u 跳过 %u", (unsigned)(sent - skipped), (unsigned)skipped);
            triggerHud("ME 文本(部分)", buf, lv_color_hex(CLR_AMBER));
        } else {
            triggerHud("ME 文本", "已发送", lv_color_hex(CLR_GREEN));
        }
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
    // I2C 健康检查：原版 s3.ino:3484-3491 每 500ms 探测一次 MCP23017，
    // 失败就 recoverI2CBus() 重新初始化整条总线。LVGL 版把这一段整个丢了，
    // 后果是 I2C 一旦被干扰（干扰按键灯/插拔 USB/BLE 突发），键盘就永久失灵，
    // 只能断电重启才恢复。补回来。
    static unsigned long lastI2CCheck = 0;
    if (millis() - lastI2CCheck > 500) {
        lastI2CCheck = millis();
        ct_mark(CT_S_SCAN_I2C);
        Wire.beginTransmission(MCP23017_ADDR);
        if (Wire.endTransmission() != 0) {
            ct_mark(CT_S_SCAN_RECOVER);
            // 探测失败先记一条（"总线失联"），恢复结果由 recoverI2CBus()
            // 自己在失败时再记一条"恢复失败"。两条分开的意义是：
            // 光看"恢复失败"不知道是偶发还是持续，光看"失联"不知道有没有救回来。
            ELWARN("I2C", "芯片无响应 正在恢复总线");
            if (!recoverI2CBus()) {
                return;   // 没救回来，这一轮扫描直接跳过（下面读到的全是垃圾）
            }
            ELINFO("I2C", "总线已恢复");
        }
    }

    // LOGO 长按判定。矩阵键本来**没有**长按检测（工程里所有 _HOLD 都来自
    // C3 小 MCU 的 Serial1 上报），这里按"按下时记时刻 + 每拍看一次还按着没有"
    // 自己实现。放在列循环前面，一拍一次就够（SCAN_INTERVAL = 8ms）。
    //
    // ⚠️ Fn 按住时**完全不进**这段：Fn+LOGO 是系统重启（见按键分发里
    // baseKey == K_LOGO 那个分支），长按不能把它抢走。
    if (logoIsDown && !logoHoldFired && !fnPressed
        && millis() - logoDownMs >= PET_LOGO_HOLD_MS) {
        logoHoldFired = true;
        if (petScene == SCENE_FULL)      petExitFull();    // 页内：退出
        else if (petScene == SCENE_PEEK) petDismiss();     // 探头：收起 + 记打扰
        else                             petEnterFull();   // 平时：进宠物页
    }

    for (int c = 0; c < NUM_COLS; c++) {
        // 选列只用一次寄存器写。
        //
        // 原来的 mcp.digitalWrite(c, LOW) 在 Adafruit 库里是**读-改-写**：
        // Adafruit_MCP23XXX::digitalWrite 走 RegisterBits::write()，先读回整个
        // 端口字节、改一位、再写回 —— 一次 digitalWrite = 2 次 I2C 事务。
        // 16 列 × (LOW + HIGH) = 32 次 digitalWrite = **64 次 I2C 事务**，
        // 400kHz 下单次事务 ~112µs，光 I2C 就要 ~7ms，再加上 16×20µs 的列稳定
        // 等待，一次完整扫描 ~7.3ms。而 loop() 里 SCAN_INTERVAL 是 2ms ——
        // **需求是硬件能力的 3.5 倍**。扫描是阻塞 I2C，又排在 lvgl_driver_loop()
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
            if (currentState != keebMatrix[r][c]) {
                if (millis() - lastDebounceTime[r][c] > DEBOUNCE_DELAY) {
                    lastDebounceTime[r][c] = millis();
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
                        const bool inUiMode = (currentSysMode == SYS_MODE_MENU)
                                              || IS_SETTING_MODE(currentSysMode)
                                              || inRecMode
                                              || inElogMode;

                        // 宠物页**故意不在** inUiMode 里：那个分支会把按键吃干净、
                        // 一律不发主机，进去就一个字都打不出来。宠物页要的是
                        // 矩阵键照常发主机、宠物只作反应。
                        //
                        // 但 ESC 得有个说法：不管当前是什么界面，按 ESC 都先
                        // 回到主屏（这是全键盘通用的逃生出口）。否则宠物页在
                        // 场时按 ESC 会被当成普通字符发给主机，主机那边收到一个
                        // 莫名其妙的 ESC —— 而这里又不能简单地"因为不在界面态就
                        // 照常发"，那和"宠物页是全屏页面"的事实矛盾。
                        if (currentSysMode == SYS_MODE_PET && baseKey == 0x29 /* ESC */) {
                            petExitFull();
                            hostKeyHeld[r][c] = false;   // 这键没发过 press，别补 release
                            continue;
                        }

                        if (inUiMode) {
                            // ---- 界面态：这一整块把按键吃干净，一律不发到主机 ----

                            // 错误日志页：上下翻 / DEL 清空 / ESC 回菜单。
                            // 必须排在菜单分支**之前**判断吗？不必 —— SYS_MODE_ELOG
                            // 和 SYS_MODE_MENU 是互斥的 currentSysMode，顺序无所谓，
                            // 但放在最前面读起来更清楚。
                            if (inElogMode) {
                                elogKey(baseKey);
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
                                    // LOGO 现在要区分长按（进/退宠物页）和短按
                                    // （切主屏风格），所以动作**从按下挪到松手**。
                                    // 按下只记时刻；松手时没触发过长按才算短按。
                                    // ⚠️ Fn+LOGO 的重启保持"按下即执行"，不受影响 ——
                                    //    它是唯一一个要求"按下就有反应"的组合键。
                                    if (fnPressed) {
                                        triggerHud("系统重启", "请稍候", lv_color_hex(CLR_RED));
                                        pendingRestartMs = millis() + 600;
                                    } else {
                                        logoIsDown = true;
                                        logoDownMs = millis();
                                        logoHoldFired = false;
                                    }
                                } else if (baseKey == K_PLAY) {
                                    if (fnPressed) {
                                        SystemControl.press(SYSTEM_CONTROL_STANDBY);
                                        SystemControl.release();
                                    } else {
                                        ConsumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
                                        mediaPlaying = !mediaPlaying;
                                        triggerHud("媒体播放", mediaPlaying ? "播放" : "暂停",
                                            lv_color_hex(CLR_GREEN));
                                    }
                                } else if (baseKey == K_NEXT) {
                                    if (!fnPressed) {
                                        ConsumerControl.press(CONSUMER_CONTROL_SCAN_NEXT);
                                        mediaPlaying = true;
                                        triggerHud("媒体播放", "下一曲", lv_color_hex(CLR_ACCENT));
                                    }
                                } else if (baseKey == K_PREV) {
                                    if (!fnPressed) {
                                        ConsumerControl.press(CONSUMER_CONTROL_SCAN_PREVIOUS);
                                        mediaPlaying = true;
                                        triggerHud("媒体播放", "上一曲", lv_color_hex(CLR_ACCENT));
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
                                logoIsDown = false;
                                if (!logoHoldFired) {
                                    if (petScene == SCENE_FULL) petExitFull();
                                    else if (petScene == SCENE_PEEK) { /* 吞掉：不切风格 */ }
                                    else cycleDisplayStyle();
                                }
                                logoHoldFired = false;
                            }
                            // PLAY/NEXT/PREV 的 Consumer 松键照旧
                            if (baseKey == K_PLAY || baseKey == K_NEXT || baseKey == K_PREV) {
                                ConsumerControl.release();
                            }
                        }
                        else if (hostKeyHeld[r][c]) {
                            // 只有"按下时确实发给过主机"的键才需要补一个松键。
                            // 界面态里被吞掉的键（菜单导航、ESC、设置字段…）按下时没发过
                            // press，这里再发 release 就会给主机一个无头的松键事件。
                            hostKeyHeld[r][c] = false;
                            uint16_t mappedKey = getMappedKey(baseKey);
                            kbRelease((uint8_t)mappedKey);
                        }
                    }
                }
            }
        }
        // 全部列恢复空闲高电平（同时保住"下一轮开始时两个端口都是 0xFF"这个前提）
        mcp.writeGPIOA(0xFF);
        mcp.writeGPIOB(0xFF);
    }
}

// ===========================
// I2C 恢复
// ===========================
// 返回 true = 总线和 MCP23017 都救回来了。
//
// 这条路径以前是静默的：begin_I2C 失败也照样返回，调用方无从判断，
// 于是"键盘突然失灵"这件事在日志里一个字都没有 —— 而它恰恰是
// 最需要留证的一类故障（总线受干扰 / 接触不良 / 上电时序没满足）。
// 现在失败会直接进错误日志，菜单里能翻到"总线恢复失败 x N 次"。
static bool recoverI2CBus(void) {
    Wire.end();
    delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);
    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        // 去重会把反复的同类失败合并成一条并累加次数，
        // 所以这里可以放心每次都记。
        ELERR("I2C", "总线恢复失败 键盘可能失灵");
        return false;
    }
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.pinMode(c, OUTPUT);
        mcp.digitalWrite(c, HIGH);
    }
    return true;
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
    CFG_POST_PROF,     // 当前方案：顶部条的序号和图标要跟上
    CFG_POST_CLOCK,    // 时间：立刻 settimeofday
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
static const CfgField CFG_FIELDS[] = {
    { "disp_mode",   CFG_U8,   &currentDispMode,  0, TOTAL_DISP_MODES - 1, "disp_mode",   CFG_POST_DISP  },
    { "saver_mode",  CFG_U8,   &saverMode,        0, TOTAL_SAVER_MODES - 1,"saver_mode",  CFG_POST_SAVER },
    { "curr_prof",   CFG_U8,   &currentProfile,   0, TOTAL_PROFILES - 1,   "curr_prof",   CFG_POST_PROF  },
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
    if (prof < 0 || prof >= TOTAL_PROFILES) return 0;
    if (nRules > MAX_REMAP_RULES) nRules = MAX_REMAP_RULES;

    // 1. 老的全删。⚠ 不能只把条数置 0：那些 rmp_<p>_<i> 键还留在 NVS 里，
    //    换固件/读档时会诈尸，而且再也清不掉。
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
        triggerHud("按键重映射", buf, lv_color_hex(CLR_RED));
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
                if (val[0] == '\0') {
                    preferences.remove(nk);
                    ELINFO("CFG", "宏 %s(%s) 已删", nk, k);
                } else {
                    preferences.putString(nk, val);
                    String back = preferences.getString(nk, "");
                    if (back != val) {
                        // 记失败项（宏，.ino 里放不了新函数，见 CFG_FAIL_ADD）
                        CFG_FAIL_ADD(nk);
                        ELWARN("CFG", "宏 %s 没存住(NVS 写不下,分区可能满了)", nk);
                    } else {
                        ELINFO("CFG", "宏 %s(%s) 已写 %u 字节", nk, k, (unsigned)strlen(val));
                    }
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
            if (val[0] == '\0') {
                preferences.remove(nk.c_str());
                ELINFO("CFG", "全局键 %s 已删", k);
            } else {
                preferences.putString(nk.c_str(), val);
                String back = preferences.getString(nk.c_str(), "");
                if (back != val) {
                    // 记失败项（同上，宏）
                    CFG_FAIL_ADD(k);
                    ELWARN("CFG", "全局键 %s 没存住(NVS 写不下,分区可能满了)", k);
                } else {
                    ELINFO("CFG", "全局键 %s 已写 %u 字节", k, (unsigned)strlen(val));
                }
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
        case CFG_POST_PROF:  pendMask |= CFG_PEND_PROF;  break;
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
// 前后都有键盘扫描、LVGL 刷新和 esp_task_wdt_reset()，10 秒看门狗永远饿不着。
// 一份配置最多摊 4 轮，肉眼看不出延迟。
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
    else if (cfgPend & CFG_PEND_PROF) {
        cfgPend &= ~CFG_PEND_PROF;
        switchProfile(currentProfile);
    }
    else if (cfgPend & CFG_PEND_SAVER) {
        cfgPend &= ~CFG_PEND_SAVER;
        unsigned long t0 = millis();
        setScreensaverMode(saverMode);
        ELINFO("CFG", "屏保模式重建用了 %lums", (unsigned long)(millis() - t0));
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
    triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_ACCENT));
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
    // TIME:epoch - Sync time
    else if (cmd.startsWith("TIME:")) {
        time_t t = cmd.substring(5).toInt();
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        preferences.putUInt("set_epoch", (uint32_t)t);
        epochCache = (uint32_t)t;   // 内存副本跟着走，否则 CFGGET 拉出来的还是旧时间
        triggerHud("时间", "已同步", lv_color_hex(CLR_GREEN));
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
                triggerHud("闹钟", "时间超出范围", lv_color_hex(CLR_RED));
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
                triggerHud("倒计时", "时长不合法", lv_color_hex(CLR_RED));
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
        triggerHud("滚动字幕", text.substring(0, min(16u, text.length())).c_str(), lv_color_hex(CLR_ACCENT));
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
            memcpy(logDumpSnap, logRingBuf, logRingLen);   // 冻结一份，见下
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
    // 卡在 loop() 里，而 loop() 身上压着 10 秒任务看门狗（WDT_TIMEOUT）和
    // 6 秒卡死监测（crash_trace），任何一个先到都是当场复位。
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
            triggerHud("ME 文本", "文件打开失败", lv_color_hex(CLR_RED));
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
    else if (cmd == "ME_END") {
        if (meFile) { meFile.close(); meFile = File(); }
        size_t sz = 0;
        if (SPIFFS.exists("/me_hex.txt")) {
            File f = SPIFFS.open("/me_hex.txt", FILE_READ);
            if (f) { sz = f.size(); f.close(); }
        }
        LOG_PORT.printf("[ME] END onDisk=%u appended=%u errs=%u\n",
                        (unsigned)sz, (unsigned)meHexBytes, (unsigned)meWriteErrors);
        if (sz == 0 || meWriteErrors > 0) {
            triggerHud("ME 文本", meWriteErrors ? "写入出错" : "存入失败",
                       lv_color_hex(CLR_RED));
        } else {
            char info[32];
            snprintf(info, sizeof(info), "%u 字节已存", (unsigned)sz);
            triggerHud("ME 文本", info, lv_color_hex(CLR_GREEN));
        }
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
        char info[32];
        snprintf(info, sizeof(info), "%u 字已存", (unsigned)text.length());
        triggerHud("ME 文本", info, lv_color_hex(CLR_GREEN));
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
    // LOGO_JPEG_FLUSH - 网页端把最后一包写完之后显式敲一下"发完了"。
    //
    // 以前是"收满 logoRxTotal 就自动收尾"，但 BLE 的包是异步到达的：
    // 固件在收到第 N 包、累加到刚好等于 total 的那一刻就可能在下一包还在路上时
    // 冲进解码，此时 logoRxActive 已经置 false，那几包在途的 JPEG 字节就会被
    // 当成文本命令灌进命令队列。显式收尾把"发完"这件事变成一个双方都确认的动作。
    else if (cmd == "LOGO_JPEG_FLUSH") {
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
    // LOGO_STATUS - 网页轮询真实结果。
    // 网页以前是"发完字节就 alert 上传成功"，那是**假成功**：BLE 写成功只说明
    // 数据交给蓝牙了，不代表键盘解出来、落盘了。现在键盘主动回报状态。
    else if (cmd == "LOGO_STATUS") {
        const char* state = logoRxActive ? (logoRxDone ? "READY" : "RECV")
                                         : (wpReady ? "OK" : "IDLE");
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
            if (dir > 0) ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_INCREMENT);
            else          ConsumerControl.press(CONSUMER_CONTROL_BRIGHTNESS_DECREMENT);
            ConsumerControl.release();
            triggerHud("屏幕亮度", dir > 0 ? "调亮" : "调暗", lv_color_hex(CLR_ACCENT));
            break;
        }
        case MODE_MUTE: {
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

// C3 -> 本机的一条指令。语义照抄 s3.ino 原版，去掉响铃/g_forceOff 那两段
// （本工程没有闹钟响铃流程，背光总开关叫 lightOn）。
static void handleC3Command(const String& cmd) {
    if (cmd == "PONG") { c3Connected = true; return; }

    // 息屏期间任何 C3 动作都算"有人回来了"（旋钮碰一下也算）
    if (currentSysMode == SYS_MODE_SLEEP) gotoMainScreen();

    // ---- 宠物：接管 C3 实体键 ----
    // 这些键物理上挂在小 MCU 上、**矩阵里没有这一格**（README:122-123），
    // 所以用它们互动完全不影响打字。设计原则是"借道不吞键"：宠物拿到
    // 一次反应，但该键的原有语义照常落回下面 —— 摸猫的时候手按到静音键，
    // 它喵一声，你的电脑同时也静音了（和 8285 那段静音处理是同一个态度）。
    //
    // ⚠️ 绝对不碰 BTN:CPG_HOLD / BTN:MUTE_HOLD：那两个是"进下载模式重启"。
    if (currentSysMode == SYS_MODE_PET) {
        if (cmd == "BTN:MUTE")   { petPetted(2); }              // 摸头
        else if (cmd.startsWith("ROT:") || cmd == "ENC:+"
                 || cmd == "ENC:-") { petPetted(1); }            // 撸（连续转）
        else if (cmd == "BTN:KNOB") { petFed(); }                // 喂食
        else if (cmd == "BTN:LIGHT_HOLD") {                       // 长按灯光 = 退出
            petExitFull();
            return;
        }
        // 其余 C3 键（灯效 / 短按灯光）不在宠物页接管，掉回下面照常处理
    } else if (petScene == SCENE_PEEK) {
        // 探头在场：只"顺便"算一次被摸，**不吞任何键**。探头出现时用户多半
        // 正在打字，吞键就卡壳了。
        if (cmd == "BTN:MUTE" || cmd == "BTN:KNOB"
            || cmd.startsWith("ROT:") || cmd == "ENC:+" || cmd == "ENC:-") {
            petPetted(1);
        } else if (cmd == "BTN:LIGHT_HOLD") {
            petDismiss();
            return;
        }
    }

    if (cmd == "ENC:+")      { knobAdjust(1);  return; }
    if (cmd == "ENC:-")      { knobAdjust(-1); return; }
    if (cmd.startsWith("ROT:")) { knobAdjust(cmd.substring(4) == "R" ? 1 : -1); return; }

    if (cmd == "BTN:KNOB") {
        if (currentSysMode == SYS_MODE_MENU) {
            handleMenuSelect();
        } else if (IS_SETTING_MODE(currentSysMode)) {
            saveSettingScreen();
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
        if (!lightOn) { lightOn = true; preferences.putBool("light_on", true); }
        currentMode = MODE_MUTE;
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
        if (!lightOn) { lightOn = true; preferences.putBool("light_on", true); }
        currentMode = MODE_CPG;
        if (currentEffect == 0) currentEffect = 1;   // 跳过"关闭"这一档
        else currentEffect = (uint8_t)((currentEffect + 1) % MAX_EFFECTS);
        preferences.putUChar("effect", currentEffect);
        triggerHud("灯效切换", effectNames[currentEffect], lv_color_hex(CLR_ACCENT));
        return;
    }
    // 两个长按都是进下载模式重启
    if (cmd == "BTN:MUTE_HOLD" || cmd == "BTN:CPG_HOLD") {
        triggerHud("系统重启", "请稍候", lv_color_hex(CLR_RED));
        pendingRestartMs = millis() + 600;
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
        default:                return "未知阶段";
    }
}

// 第二张 HUD（内存详情）延迟 1.4s 弹出，让第一张"重启原因"先被看到
// 开机诊断 HUD 队列：三张依次弹出（原因 → 内存 → 上次现场）。
// 之前只支持一张，第二个字段只能靠额外的 pending 标志硬塞；换成队列后
// 后面再加诊断项直接 push 就行。
#define BOOT_HUD_MAX 3
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
    triggerHud(it.title, it.value, lv_color_hex(it.color));
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
    triggerHud(abnormal ? "重启原因" : "开机", resetReasonName(why),
               lv_color_hex(abnormal ? CLR_AMBER : CLR_GREEN));

    // 第二张：PSRAM 与内部堆余量（内存耗尽是之前重启的主因之一）
    char buf[40];
    if (psram > 0) {
        snprintf(buf, sizeof(buf), "PSRAM%uM 余%uK 第%u次",
                 (unsigned)(psram / (1024 * 1024)), (unsigned)(heapFree / 1024), (unsigned)boots);
    } else {
        // PSRAM 没起来时 LVGL 对象池回落内部堆，余量会小很多
        snprintf(buf, sizeof(buf), "无PSRAM 余%uK 第%u次",
                 (unsigned)(heapFree / 1024), (unsigned)boots);
    }
    pushBootHud(1400, "内存", buf, CLR_ACCENT);

    // 第三张：上次运行是怎么结束的。
    // 关键在于 panic 之后 USB 会重新枚举，PC 侧串口句柄失效，崩溃回溯抓不到，
    // 所以现场必须留在芯片里（RTC 慢存，见 crash_trace.h），这里回放出来。
    if (ct_prev.magic == CT_MAGIC) {
        char keyName[16];
        if (ct_prev.keyCode == 0xFFFFFFFFUL) {
            snprintf(keyName, sizeof(keyName), "无");
        } else {
            snprintf(keyName, sizeof(keyName), "%s", getKeyName((uint16_t)ct_prev.keyCode));
        }
        if (ct_prev.ready == 0) {
            // 上一次没跑完 setup 就没了 —— 启动流程里就崩了，跟运行期是两回事
            snprintf(buf, sizeof(buf), "启动未完成 %s", ctStageName(ct_prev.stage));
            pushBootHud(2900, "上次死在", buf, CLR_RED);
        } else {
            snprintf(buf, sizeof(buf), "%s%s键%s",
                     ctStageName(ct_prev.stage), ct_prev.hang ? " 卡死" : "", keyName);
            pushBootHud(2900, ct_prev.hang ? "上次卡在" : "上次死在", buf,
                        ct_prev.hang ? CLR_AMBER : CLR_RED);
        }
    } else {
        // 没有记录 = 断电重启（RTC 慢存会掉电清零），不是软件崩溃
        snprintf(buf, sizeof(buf), "断电或首次烧录");
        pushBootHud(2900, "上次", buf, CLR_TEXT_DIM);
    }

    LOG_PORT.printf("[BOOT] prev: hang=%u stage=%u(%s) key=%s uptimeMs=%u\n",
                  (unsigned)ct_prev.hang, (unsigned)ct_prev.stage,
                  ctStageName(ct_prev.stage),
                  (ct_prev.keyCode == 0xFFFFFFFFUL) ? "none" : getKeyName((uint16_t)ct_prev.keyCode),
                  (unsigned)ct_prev.uptimeMs);
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
        if (ct_prev.ready == 0) {
            // 上一次连 setup 都没跑完 —— 死在启动流程里（多半是硬件/配置问题）
            snprintf(msg, sizeof(msg), "上次死在启动中 阶段:%s", ctStageName(ct_prev.stage));
        } else {
            snprintf(msg, sizeof(msg), "上次%s于%s 运行%u秒 按键:%s",
                     ct_prev.hang ? "卡死" : "崩溃",
                     ctStageName(ct_prev.stage),
                     (unsigned)(ct_prev.uptimeMs / 1000UL), keyName);
        }
        // 卡死是我们自己监测任务判定的（不是 panic），级别给错误；
        // 崩溃同理。两者都是"用不了了"，都是错误级。
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
    // 崩溃现场记录：必须**第一时间**初始化。setup() 里后面任何一步 panic，
    // 现场也已经留在 RTC 慢存里了，下次开机能显示出来。
    ct_begin();

    LOG_PORT.begin(115200);
    delay(500);
    Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

    // 早分配 PSRAM 日志环形缓冲:越早越好,从这里开始的所有 LOG_PORT.printf
    // 都能进缓冲,后面网页发 LOG:dump 能一次性拉到。失败降级到不缓冲
    // (Serial0 仍正常,只是网页看不到日志而已,不至于死锁)
    if (logRingBuf == nullptr) {
        logRingBuf = (char*)ps_malloc(LOG_BUF_CAP);
        if (logRingBuf) {
            memset(logRingBuf, 0, LOG_BUF_CAP);
            // 回拉快照：LOG:dump 期间环形缓冲还会被新日志改写，不冻结一份
            // 读出来的行是错位的。放 PSRAM，别占内部 DRAM。
            logDumpSnap = (char*)ps_malloc(LOG_BUF_CAP);
            if (logDumpSnap) memset(logDumpSnap, 0, LOG_BUF_CAP);
            LOG_PORT.println("[LOG] ring buffer ready (PSRAM 8KB)");
        } else {
            LOG_PORT.println("[LOG] ps_malloc failed, BLE log disabled");
        }
    }

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
    // 宠物：14 项状态。pet_peek_on 默认 true（getBool 第二个参数是
    // "键不存在时返回什么"，传 false 就等于默认关）。
    petLoadFromNvs();
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
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
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

    BLEDevice::startAdvertising();

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
    for (int c = 0; c < NUM_COLS; c++) {
        mcp.pinMode(c, OUTPUT);
        mcp.digitalWrite(c, HIGH);
    }
    for (int r = 0; r < NUM_ROWS; r++) {
        pinMode(rowPins[r], INPUT_PULLUP);
    }

    // 看门狗
    #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t twdt = { .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&twdt);
    esp_task_wdt_add(NULL);
    #else
    esp_task_wdt_init(WDT_TIMEOUT, true);
    esp_task_wdt_add(NULL);
    #endif

    // 初始化显示
    renderCurrentDisplayBase();
    gotoMainScreen();
    lastActivityTime = millis();

    showBootDiagnostics();
    ct_ready();
    ct_startMonitor();

    LOG_PORT.println("YYQ-MX9.0 LVGL Ready");
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
    esp_task_wdt_reset();
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
    // 这是"保存配置导致键盘重启"的修复点 —— 之前这些全屏重建挤在一条指令里
    // 连着跑，中间一次狗都没喂，直接顶爆 10 秒任务看门狗。摊到多轮 loop 之后，
    // 每一次全屏重建前后都有键盘扫描、LVGL 和 esp_task_wdt_reset()。
    cfgPostPump();

    // 配置 JSON 的接收超时兜底：网页发到一半关掉页面/走出蓝牙范围时，
    // cfgRxActive 必须放掉 —— 不然下一段 CFGBEGIN 之前的 CFGDATA 会被
    // 追加到上一段残留后面，攒出一份谁也解析不了的 JSON。
    if (cfgRxActive && (unsigned long)(millis() - cfgRxLastMs) > CFG_RX_TIMEOUT_MS) {
        cfgRxActive = false;
        ELWARN("CFG", "接收超时 声明 %u 实收 %u，已丢弃",
               (unsigned)cfgRxTotal, (unsigned)cfgRxLen);
    }

    // 壁纸：收齐了就解码（几百毫秒，放主任务不占 BLE 主机任务）。
    // 传一半断线（关网页、走出范围）必须超时放掉缓冲，否则后面所有文本
    // 指令都会被当成 JPEG 数据吃掉，键盘直接"失联"。
    if (logoRxDone) finishLogoUpload();
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

    // 连接状态归 bt_link 管（它要在断链时清掉排队中的响应），这里只盯"刚断开"
    // 这一个边沿来重启广播，让主机能重新扫到。
    if (!btLinkConnected() && oldDeviceConnected) {
        ct_mark(CT_S_BLE_DELAY);
        btLinkDelay(500);          // 喂狗版 delay()，别在这 500ms 里饿着
        BLEDevice::startAdvertising();
        oldDeviceConnected = btLinkConnected();
    }
    if (btLinkConnected() && !oldDeviceConnected) oldDeviceConnected = true;

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

    // ---- 宠物：姿态回 IDLE / 探头 8 秒超时 / 出场仲裁 ----
    // ⚠️ 绝不能为了"宠物页看不见主屏"就跳过上面那个 updateDynamicElements()：
    // lockStateDirty 是靠它内部清的，跳过会退化成每轮 loop 都往 C3 推 61 字节
    // 灯帧，Serial1 堵死，6 秒后监测任务判定卡死 → esp_restart()。
    petPoll();

    // ---- 响铃卡片：lv_layer_top 上的浮层，压在任何一块屏上面 ----
    // 每轮都调，内部按 500ms 节流：起铃、停铃、描边翻转时才会真重建。
    // 它挂的是 top 层而不是活动屏，所以底下是主屏/菜单/设置页/黑屏都一样能盖住，
    // 不需要为了显示它去改 currentSysMode。
    drawRingOverlay();

    // HUD 消失
    ct_mark(CT_S_HUD);
    if (hud.active && millis() - hud.startMs > hud.showMs) {
        hud.active = false;
        if (scr_hud) { lv_obj_del(scr_hud); scr_hud = nullptr; }
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

    // 完整跑完一轮：心跳 +1，监测任务据此判断有没有卡死
    ct_mark(CT_S_IDLE);
}
