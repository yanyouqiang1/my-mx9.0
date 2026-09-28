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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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
// BLE 一条 notify 包的上限（含 \0）。必须在 LogMirror 之前定义,
// 否则类体里的 char lineBuf[BLE_CMD_BUF_SIZE] 编译不过。
// 这个宏原本放在 BLE 段(第 593 行附近),挪上来只是换个位置,含义没变。
#define BLE_CMD_BUF_SIZE   256

// LogMirror::write() 收完一行就调 pushLogLine()。后者函数体在 BLE 全局段
// 之后(因为要用 pCharacteristic / deviceConnected),这里先前置声明。
static void pushLogLine(const char* line);

class LogMirror : public Print {
    HardwareSerial* real;
    char    lineBuf[BLE_CMD_BUF_SIZE];
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
static unsigned long logLastNotifyMs = 0;  // 节流：50Hz 上限

// 推一行进环形缓冲；若 LOG:on 已开，按行做一次 BLE notify（带节流）
// 函数体在文件下方 BLE 全局变量之后定义（需要 pCharacteristic / deviceConnected），
// 这里先声明供 LogMirror::write 调用。
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

#include <esp_heap_caps.h>

#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>

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
#define IS_SETTING_MODE(m) ((m) >= SYS_MODE_SET_TIME && (m) <= SYS_MODE_SET_LIGHT)
static uint8_t currentSysMode = SYS_MODE_NORMAL;
static bool menuNeedsRebuild = false;  // 菜单重建标志（在 loop 中处理）

// 显示模式
#define DISP_MODE_GEEK        0
#define DISP_MODE_BIG_CLOCK   1
#define DISP_MODE_INFO_PANEL  2
#define DISP_MODE_KEY_MON     3
#define DISP_MODE_RHYTHM      4
#define DISP_MODE_WALLPAPER   5
#define TOTAL_DISP_MODES      6

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸"
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
static bool shtAvailable = false;
static float shtTemp = 0.0f;
static float shtHumidity = 0.0f;
static float shtTempOffset = 62.0f;
static unsigned long lastSHTRead = 0;
#define SHT_READ_INTERVAL_MS 900000UL

// 时间
static time_t alarmLastFiredYday = -1;
static unsigned long lastActivityTime = 0;
static unsigned long pendingRestartMs = 0;
#define SLEEP_TIMEOUT_MS 60000UL

// 菜单
static uint8_t menuSel = 0;
#define MENU_ITEMS 12

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

// 菜单项（中文）
static const char* menuItemsCN[MENU_ITEMS] = {
    "1. 返回主屏",
    "2. 切换主屏风格",
    "3. 切换配置方案",
    "4. 按键回显开关",
    "5. 灯光设置",
    "6. 设置时间",
    "7. 闹钟设置",
    "8. 倒计时",
    "9. 刷新温湿度",
    "10. 温度校准",
    "11. 计数清零",
    "12. 屏保风格"
};

// 辅助变量
static bool showKeystrokes = true;
static char lastKeyPressed[8] = "-";

// ===========================
// 息屏 / 屏保风格
// ===========================
// 原来 SLEEP_TIMEOUT_MS 到了就 destroyMainScreen() 把主屏整个拆掉，屏幕全黑。
// 现在多给两种屏保：
//   SAVER_WALL 壁纸铺满，和"信息面板"每隔几秒**轮播**一张 —— 一直是图片
//              会看不到时间，一直是面板又浪费了壁纸
//   SAVER_INFO 只显示信息面板，不轮播
// 两种模式共用同一块 `sv_panel`（时间 / 日期 / 温湿度都收在这一块里，
// 不再是四个散落在屏幕四角的 label）。
// 屏保是一块独立屏幕（scr_saver），和主屏并存，唤醒时直接切回主屏即可。
#define SAVER_OFF   0
#define SAVER_WALL  1
#define SAVER_INFO  2
#define TOTAL_SAVER_MODES 3
static const char* saverModeNames[] = { "黑屏", "壁纸轮播", "信息面板" };
static uint8_t saverMode = SAVER_OFF;
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
// 布局常量：6 行 × 28 步进 = 168px，从 y=50 起排到 218，底部留 22px 给按键提示
#define MENU_VISIBLE_ITEMS 6
#define MENU_ITEM_H        26
#define MENU_PITCH         28
#define MENU_LIST_TOP      50
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

// 温度校准
static float calTempOriginal = 62.0f;
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
static lv_obj_t* set_alarm_field_labels[2] = { nullptr };

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
#define TOTAL_PROFILES 4
#define MAX_REMAP_RULES 32
struct RemapRule { uint16_t fromKey; uint16_t toKey; };
static RemapRule profileRemaps[TOTAL_PROFILES][MAX_REMAP_RULES];
static int remapCounts[TOTAL_PROFILES] = { 0 };
static uint8_t currentProfile = 0;
static const char* profileNamesCN[TOTAL_PROFILES] = {
    "方案1-Windows", "方案2-macOS", "方案3-游戏模式", "方案4-工作模式"
};

// BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool deviceConnected = false;
static bool oldDeviceConnected = false;
static unsigned long lastPingTime = 0;

// 推一行进环形缓冲；若 LOG:on 已开，按行做一次 BLE notify（带节流）。
// 这里是 LogMirror::write 会调到的"主行处理"——
// 函数体放在 BLE 全局之后，这样 pCharacteristic / deviceConnected 都在作用域里。
//
// **末尾必须带 \n**：网页端 onBleNotify 按 \n 切行分发，没 \n 就直接被
// parts.pop() 整个吞进 rest,for 循环空跑,日志框永远不显示 ——
// 这是第一版"开流没反应"的根因。所有 notify 一律包末尾加 \n。
//
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

    // 流转发：节流 10Hz（100ms/条），每条 notify 末尾加 \n
    if (logStreamOn && pCharacteristic != nullptr && deviceConnected) {
        unsigned long now = millis();
        if ((unsigned long)(now - logLastNotifyMs) >= 100) {
            logLastNotifyMs = now;
            char out[BLE_CMD_BUF_SIZE];
            int m = snprintf(out, sizeof(out) - 2, "LOG:%s", line);  // 留 2 字节给 \n\0
            if (m > 0 && m < (int)sizeof(out) - 2) {
                out[m]     = '\n';
                out[m + 1] = '\0';
                pCharacteristic->setValue((uint8_t*)out, (size_t)(m + 1));
                pCharacteristic->notify();
            }
        }
    }
}

// 主机指令队列（生产者 = NimBLE 主机任务，消费者 = loop()）。
// onWrite 只负责入队，绝不碰 LVGL —— 原因见 MyCallbacks::onWrite 的注释。
// BLE_CMD_BUF_SIZE 已在前面 LogMirror 之前定义。
#define BLE_CMD_QUEUE_LEN 8
static QueueHandle_t bleCmdQueue = nullptr;
static void handleCommand(const String& cmd);   // 真正定义在文件后段的命令解析入口

// ===========================
// BLE 文本通道的行缓冲
// ===========================
//
// 为什么必须按行拼：BLE 的一"条指令"是逻辑概念，实际是若干个 ATT 包。
// 固件里 BLEDevice::setMTU(517) 只是**请求**协商，MTU 没协商成功时
// （Windows 端很常见，实际落到 23~185 字节）网页的 writeValue 会被浏览器
// 拆成多包。而原来的 onWrite 是"一个包 = 一条完整命令"，于是
//   `SET:p0_M1:CMB:224,48` 这种 20 多字节的宏
//   `GSET:M1:PROFILE:1`      这种全局动作
// 会被切成两半分别进队，handleCommand 拿到的都是残缺指令，什么也不匹配 ——
// 表现就是"M1 相关的下发一律失败 / 不生效"，而短的（ALERT:RED）反而好使。
//
// 这里改成和 USB HID 那条通道一样的纪律：先把字节攒成一行，遇到 \n 提交；
// 没带 \n 的老客户端（以及网页上零散的单包指令）靠"包间静默 15ms"兜底提交。
// 这块缓冲只在 NimBLE 主机任务里被读写，不跨任务，不需要加锁。
#define BLE_LINE_IDLE_MS 15
static char          bleLine[BLE_CMD_BUF_SIZE];
static uint16_t      bleLineLen  = 0;
static unsigned long bleLineLastMs = 0;

// 把攒到的一行塞进队列。队列满时丢最老的一条，保证新指令一定能进去（宁旧不新）。
static void bleLineSubmit(void) {
    if (bleLineLen == 0 || bleCmdQueue == nullptr) { bleLineLen = 0; return; }
    bleLine[bleLineLen] = '\0';
    bleLineLen = 0;
    if (xQueueSend(bleCmdQueue, bleLine, 0) != pdTRUE) {
        char drop[BLE_CMD_BUF_SIZE];
        if (xQueueReceive(bleCmdQueue, drop, 0) == pdTRUE) {
            xQueueSend(bleCmdQueue, bleLine, 0);
        }
    }
}

// 在 loop() 里把队列排空，逐条喂给 handleCommand()。
// 一轮最多处理 4 条：BLE 灌进来的指令量很小，卡住主循环反而会让屏幕和键盘
// 一起变卡；而 4 条的量足以在两轮 loop 内把 8 深的队列清空。
static void drainBleCommands(void) {
    if (bleCmdQueue == nullptr) return;
    static char buf[BLE_CMD_BUF_SIZE];
    for (int i = 0; i < 4; i++) {
        if (xQueueReceive(bleCmdQueue, buf, 0) != pdTRUE) return;
        buf[BLE_CMD_BUF_SIZE - 1] = '\0';
        handleCommand(String(buf));
    }
}

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
static uint32_t     logoRxTotal = 0;
static uint32_t     logoRxGot   = 0;
static bool         logoRxDone  = false;
static bool         logoRxActive = false;    // 只有这个为 true 才处于二进制接收模式
static unsigned long logoRxLastMs = 0;
static uint16_t*    logoOutBuf  = nullptr;  // JPEGDEC 的绘制目标（解码期间有效）
static uint32_t     meHexBytes  = 0;         // ME_DATA 累计写进去的十六进制字符数（诊断用）

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
static lv_obj_t* topIconBox    = nullptr;      // 系统图标的槽位（14x18，本身透明）
static lv_obj_t* topIconWin[4] = { nullptr, nullptr, nullptr, nullptr };  // Windows 四格窗
static lv_obj_t* topIconMac[4] = { nullptr, nullptr, nullptr, nullptr };  // 苹果：果体/缺口/柄/叶

// 菜单
static lv_obj_t* menu_cont = nullptr;
static lv_obj_t* menu_items[MENU_ITEMS];
static lv_obj_t* menu_items_val[MENU_ITEMS];
static lv_obj_t* menu_title = nullptr;
static lv_obj_t* menu_position = nullptr;

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
#define STYLE_BIT_GEEK        (1u << DISP_MODE_GEEK)
#define STYLE_BIT_BIG_CLOCK   (1u << DISP_MODE_BIG_CLOCK)
#define STYLE_BIT_INFO_PANEL  (1u << DISP_MODE_INFO_PANEL)
#define STYLE_BIT_KEY_MON     (1u << DISP_MODE_KEY_MON)
#define STYLE_BIT_RHYTHM      (1u << DISP_MODE_RHYTHM)
#define STYLE_BIT_WALLPAPER   (1u << DISP_MODE_WALLPAPER)
#define STYLE_BIT_ALL         (STYLE_BIT_GEEK | STYLE_BIT_BIG_CLOCK | STYLE_BIT_INFO_PANEL | \
                               STYLE_BIT_KEY_MON | STYLE_BIT_RHYTHM | STYLE_BIT_WALLPAPER)
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

    wp_img = nullptr; wp_lbl_time = nullptr;

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
    for (int i = 0; i < 4; i++) { topIconWin[i] = nullptr; topIconMac[i] = nullptr; }
}
static void destroyMainScreen(void) {
    gk_bg = nullptr; bc_bg = nullptr; ip_bg = nullptr;
    km_bg = nullptr; rh_bg = nullptr; wp_bg = nullptr;
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
        }
        lv_obj_del(scr_main);
        scr_main = nullptr;
    }
    // currentScreen 指着刚被释放的屏幕，同样作废
    currentScreen = nullptr;
    mainContentValid = false;
}

// 屏保的实现在文件后段，这里先声明（gotoMainScreen() 要用）
static void destroyScreensaver(void);

// 回到主屏的统一出口：保证主屏和主屏内容都还在，然后真正切过去。
static void gotoMainScreen(void) {
    currentSysMode = SYS_MODE_NORMAL;
    menuSel = 0;
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
static void recoverI2CBus(void);
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
static const char* getKeyName(uint16_t code);
static void build_settings_time(void);
static void build_settings_alarm(void);
static void build_settings_timer(void);
static void build_settings_caltemp(void);
static void build_settings_light(void);
static void moveSettingField(int dir);
static void adjustSettingField(int delta);
static void saveSettingScreen(void);
static void cancelSettingScreen(void);
static void update_setting_time_display(void);
static void update_setting_alarm_display(void);
static void update_setting_timer_display(void);
static void update_setting_caltemp_display(void);
static void update_setting_light_display(void);
static void update_recording_display(void);
static void build_recording(void);
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
    if (!sht31_read_raw(rawT, rawH)) return;
    float t = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    float h = 100.0f * ((float)rawH / 65535.0f);
    shtTemp = t + shtTempOffset - 62.0f;
    shtHumidity = h;
}

// ===========================
// BLE 回调
// ===========================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    // **这里绝不能直接调 handleCommand()**
    //
    // onWrite 跑在 NimBLE 的主机任务里，而 LVGL 的全部对象都归 loop() 那个
    // Arduino 任务所有。handleCommand() 一进去就是 triggerHud() /
    // drawNotifPanel() / renderCurrentDisplayBase()，全都直接 lv_obj_create /
    // lv_obj_del，既没拿 lvgl_port_lock，又和 loop() 里的
    // updateDynamicElements() / lvgl_driver_loop() 并发操作同一堆对象 ——
    // 两个任务同时改 LVGL 堆，坏掉的内存布局直接变成 panic 重启。
    //
    // 症状：主机发 ALERT:RED / ALERT:GREEN（或者 DISP_MODE:n 切风格）在弹通知的
    // 瞬间整机死机，而单独用键盘切风格一切正常 —— 差别就在"谁发起的"。
    //
    // 正确做法：回调里只把原始字节丢进队列，真正的解析和 LVGL 操作留给
    // loop()（见 drainBleCommands()），那条路径和按键触发的路径在同一个任务里。
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string raw = pCharacteristic->getValue();
        if (bleCmdQueue == nullptr || raw.empty()) return;

        // 壁纸二进制模式：这一包是 JPEG 原始字节，里面有 0x00，
        // 走下面的 memcpy + 队列会被在第一个 0 处截断，所以在这里就分流掉。
        // 唯一要放回命令通道的是网页端重传时补发的 LOGO_JPEG_START —— 此时
        // 固件还卡在上一轮的接收模式里，得让它先看见这条命令才复位。
        // 用 "LOGO_" 做暗号是安全的：JPEG 首字节必然是 0xFF，撞不上 ASCII。
        //
        // 判断条件必须用 logoRxActive，不能用 "logoRxBuf != nullptr"：
        // 接收缓冲现在是常驻的（传完也不释放），用指针判断会导致传完之后
        // 蓝牙进来的**所有文本指令**都被当成 JPEG 字节吃掉 —— 表现为
        // 上传一次壁纸之后，ME 键和其它蓝牙配置全部失灵。
        if (logoRxActive && !logoRxDone &&
            !(raw.size() >= 5 && memcmp(raw.data(), "LOGO_", 5) == 0)) {
            handleLogoChunk((const uint8_t*)raw.data(), raw.size());
            return;
        }

        // 文本通道：先按行拼，攒满一整行（\n 结尾，或包间静默 15ms）再入队。
        // 一个包 = 一条命令的老做法会把长指令（宏、全局动作）切成两半，
        // 详见 bleLineSubmit() 上面的说明。
        unsigned long now = millis();
        if (bleLineLen > 0 && (unsigned long)(now - bleLineLastMs) > BLE_LINE_IDLE_MS) {
            bleLineSubmit();
        }
        for (size_t i = 0; i < raw.size(); i++) {
            char c = (char)raw[i];
            if (c == '\0') continue;                       // 补长度的 0，不是内容
            if (c == '\n' || c == '\r') { bleLineSubmit(); continue; }
            if (bleLineLen >= BLE_CMD_BUF_SIZE - 1) bleLineSubmit();   // 超长行，断掉重来
            bleLine[bleLineLen++] = c;
        }
        bleLineLastMs = now;
    }
};

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
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 三颗锁：外圈（亮起时才显形）+ 圆点。
    // 圆点从 6px 提到 8px，外面再套一圈同色细环 —— 单靠"小圆点换颜色"在
    // 240px 的屏上离远了根本看不出来，加一圈之后亮度面积翻倍。
    // 环和点的间距只有 1px（环 12px、点 8px），靠边框自身 1px 撑开。
    const uint32_t onColors[3] = { lockLedColor[0], lockLedColor[1], lockLedColor[2] };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* ring = lv_obj_create(bar);
        lv_obj_set_size(ring, 12, 12);
        lv_obj_align(ring, LV_ALIGN_LEFT_MID, 12 + i * 18, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, lv_color_hex(onColors[i]), LV_PART_MAIN);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        topLockRing[i] = ring;

        lv_obj_t* d = iconRect(bar, 8, 8, LV_ALIGN_LEFT_MID, 14 + i * 18, 0,
                               CLR_STROKE, LV_RADIUS_CIRCLE);
        topLockDot[i] = d;
        topLockOn[i] = onColors[i];
    }

    // 方案序号（1~4），贴着右边
    topProfileNum = lv_label_create(bar);
    mkLabel(topProfileNum, &lv_font_montserrat_14, CLR_ACCENT);
    lv_label_set_text(topProfileNum, "1");
    lv_obj_align(topProfileNum, LV_ALIGN_RIGHT_MID, -10, 0);

    // 系统图标槽位。本身透明、空的，里面两组图标按当前方案显示/隐藏
    // （由 updateDynamicElements() 驱动）。
    topIconBox = lv_obj_create(bar);
    lv_obj_set_size(topIconBox, 14, 18);
    lv_obj_align(topIconBox, LV_ALIGN_RIGHT_MID, -26, 0);
    lv_obj_set_style_bg_opa(topIconBox, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(topIconBox, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(topIconBox, 0, LV_PART_MAIN);
    lv_obj_clear_flag(topIconBox, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 方案1 (Windows)：四格窗 ----
    const lv_coord_t paneX[4] = { 0, 7, 0, 7 };
    const lv_coord_t paneY[4] = { 2, 2, 9, 9 };
    for (int i = 0; i < 4; i++) {
        topIconWin[i] = iconRect(topIconBox, 6, 6, LV_ALIGN_TOP_LEFT,
                                 paneX[i], paneY[i], CLR_ACCENT, 1);
        setHidden(topIconWin[i], currentProfile != 0);
    }

    // ---- 方案2 (macOS)：苹果 ----
    // 果体 = 一只压扁的圆；顶上用**底色**画一颗小圆把果体的边缘咬掉一块，
    // 咬出来的凹陷就是苹果顶上那道缺口（它贴在顶栏上，底色恒为 CLR_SURFACE，
    // 所以"用底色画"是安全的）；最后叠果柄和一片叶子。
    // 建对象的顺序 = 绘制顺序，必须是 果体 → 缺口 → 柄 → 叶。
    topIconMac[0] = iconRect(topIconBox, 14, 12, LV_ALIGN_BOTTOM_MID, 0, 0,
                             CLR_ACCENT, LV_RADIUS_CIRCLE);          // 果体
    topIconMac[1] = iconRect(topIconBox, 6, 6, LV_ALIGN_TOP_MID, 0, 4,
                             CLR_SURFACE, LV_RADIUS_CIRCLE);         // 缺口
    topIconMac[2] = iconRect(topIconBox, 2, 5, LV_ALIGN_TOP_MID, 0, 0,
                             CLR_ACCENT, 1);                         // 果柄
    topIconMac[3] = iconRect(topIconBox, 6, 3, LV_ALIGN_TOP_MID, 3, 1,
                             CLR_ACCENT, 1);                         // 叶子
    for (int i = 0; i < 4; i++) setHidden(topIconMac[i], currentProfile != 1);
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
    for (int i = 0; i < 4; i++) {
        setHidden(topIconWin[i], currentProfile != 0);
        setHidden(topIconMac[i], currentProfile != 1);
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

    gk_bg = makeRootPanel(ensureMainScreen(), CLR_BG);

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
static uint8_t* logoRxAlloc(uint32_t total) {
    if (logoRxBuf != nullptr) return logoRxBuf;
    logoRxBuf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (logoRxBuf == nullptr) logoRxBuf = (uint8_t*)malloc(total);   // PSRAM 不可用时回落
    if (logoRxBuf != nullptr) memset(logoRxBuf, 0, total);
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
    triggerHud("壁纸传输", reason, lv_color_hex(CLR_RED));
    // 失败也要回报，否则网页只能干等到超时
    if (pCharacteristic) {
        char out[80];
        snprintf(out, sizeof(out), "LOGOSTATUS:FAIL:%.40s", reason);
        pCharacteristic->setValue((uint8_t*)out, strlen(out));
        pCharacteristic->notify();
    }
}

// 二进制接收：这段是 JPEG 原始字节，里面必然有 0x00，
// 只能按长度整段取，绝不能走 c_str()（会在第一个 0 处截断）。
static void handleLogoChunk(const uint8_t* data, size_t len) {
    if (logoRxBuf == nullptr) return;
    logoRxLastMs = millis();

    uint32_t room = logoRxTotal - logoRxGot;
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
    if (pCharacteristic) {
        char okmsg[24] = "LOGOSTATUS:OK:0/0";
        pCharacteristic->setValue((uint8_t*)okmsg, strlen(okmsg));
        pCharacteristic->notify();
    }
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
    lv_obj_t* plate = lv_obj_create(wp_bg);
    lv_obj_set_size(plate, 112, 40);
    lv_obj_align(plate, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    mkCard(plate, CLR_SURFACE_2, 10);
    lv_obj_set_style_bg_opa(plate, LV_OPA_70, LV_PART_MAIN);

    wp_lbl_time = lv_label_create(plate);
    mkLabel(wp_lbl_time, &lv_font_montserrat_28, CLR_TEXT);
    lv_label_set_text(wp_lbl_time, "--:--");
    lv_obj_center(wp_lbl_time);
}

// ===========================
// 息屏 / 屏保
// ===========================
// 屏保是一块独立的屏幕对象，和主屏并存：
//   SAVER_OFF  沿用老行为 —— 拆掉主屏，屏幕全黑
//   SAVER_WALL 壁纸铺满，和"信息面板"每 5 秒轮播一次
//   SAVER_INFO 只显示信息面板
// 唤醒统一走 gotoMainScreen()：先 showScreen(主屏) 再 destroyScreensaver()，
// 顺序反了就会删掉活动屏，LVGL 8.4 会把 disp->act_scr 置成 NULL。
static void destroyScreensaver(void) {
    sv_img = nullptr;
    sv_panel = nullptr;
    sv_lbl_time = nullptr; sv_lbl_date = nullptr;
    sv_lbl_temp = nullptr; sv_lbl_hum = nullptr;
    if (scr_saver != nullptr) {
        lv_obj_del(scr_saver);
        scr_saver = nullptr;
    }
    if (currentScreen == scr_saver) currentScreen = nullptr;
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

// 菜单第 12 项：黑屏 → 壁纸轮播 → 信息面板 → 黑屏
static void cycleScreensaverMode(void) {
    saverMode = (saverMode + 1) % TOTAL_SAVER_MODES;
    preferences.putUChar("saver_mode", saverMode);

    // 正在屏保状态下切换：立刻按新模式重建，用户不用等下一次超时
    if (currentSysMode == SYS_MODE_SLEEP) {
        destroyScreensaver();
        if (saverMode == SAVER_OFF) destroyMainScreen();
        else enterScreensaver();
    } else if (saverMode == SAVER_OFF && scr_main == nullptr) {
        // 从屏保模式退回来，但主屏早就被拆了 —— 现在就得补回来
        renderCurrentDisplayBase();
    }
    build_menu();
    triggerHud("屏保风格", saverModeNames[saverMode], lv_color_hex(CLR_ACCENT));
}

static void enterScreensaver(void) {
    if (saverMode == SAVER_OFF) {
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

static void handleMenuSelect(void) {
    switch (menuSel) {
        case 0:  // 返回主屏
            gotoMainScreen();
            break;
        case 1:  // 切换主屏风格 - 循环切换
            cycleDisplayStyle();
            break;
        case 2:  // 切换配置方案 - 循环切换
            switchProfile((currentProfile + 1) % TOTAL_PROFILES);
            break;
        case 3:  // 按键回显开关
            showKeystrokes = !showKeystrokes;
            triggerHud("按键回显", showKeystrokes ? "开启" : "关闭",
                lv_color_hex(showKeystrokes ? CLR_GREEN : CLR_RED));
            break;
        case 4:  // 灯光设置（背光开关 / 亮度 / 灯效 / 状态灯亮度 都在里面）
            build_settings_light();
            break;
        case 5:  // 设置时间
            build_settings_time();
            break;
        case 6:  // 闹钟设置
            build_settings_alarm();
            break;
        case 7:  // 倒计时
            build_settings_timer();
            break;
        case 8:  // 刷新温湿度
            if (shtAvailable) {
                sht31_update();
                triggerHud("温湿度", "已刷新", lv_color_hex(CLR_GREEN));
            } else {
                triggerHud("温湿度", "未找到", lv_color_hex(CLR_RED));
            }
            break;
        case 9:  // 温度校准
            build_settings_caltemp();
            break;
        case 10: // 计数清零
            totalKeyCount = 0;
            preferences.putUInt("keyCount", 0);
            triggerHud("击键计数", "已清零", lv_color_hex(CLR_ACCENT));
            break;
        case 11: // 屏保风格（黑屏 / 壁纸 / 时间温湿度）
            cycleScreensaverMode();
            break;
    }
}

// ===========================
// 构建：菜单（12 项，真滚动）
// ===========================
// 之前滚动是假的：yPos 恒等于 i*32，menuScrollOffset 只用来改透明度。
// 滚到第二页时，可见窗口是第 1~6 项、y 落在 32~192，而容器只有 192 高，
// 第 4/5/6 项直接被容器裁掉 —— 所以看起来"一翻页就全黑了"。
// 现在 yPos 跟着 menuScrollOffset 走，是真的把列表窗口往上滚。
static void build_menu(void) {
    // 计算滚动偏移（保证选中项始终在可见窗口内，且不越界）
    if (menuSel < menuScrollOffset) {
        menuScrollOffset = menuSel;
    } else if (menuSel >= menuScrollOffset + MENU_VISIBLE_ITEMS) {
        menuScrollOffset = menuSel - MENU_VISIBLE_ITEMS + 1;
    }
    int maxOffset = MENU_ITEMS - MENU_VISIBLE_ITEMS;
    if (maxOffset < 0) maxOffset = 0;
    if (menuScrollOffset > maxOffset) menuScrollOffset = maxOffset;
    if (menuScrollOffset < 0) menuScrollOffset = 0;

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
        lv_label_set_text(menu_position, "1/12");
        lv_obj_center(menu_position);

        // ---- 列表容器（只做裁剪，不画底）----
        menu_cont = lv_obj_create(scr_menu);
        lv_obj_set_size(menu_cont, 216, MENU_VISIBLE_ITEMS * MENU_PITCH);
        lv_obj_align(menu_cont, LV_ALIGN_TOP_MID, 0, MENU_LIST_TOP);
        lv_obj_set_style_bg_opa(menu_cont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(menu_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(menu_cont, 0, LV_PART_MAIN);

        // ---- 12 个菜单项（只建一次，之后只改位置和配色）----
        for (int i = 0; i < MENU_ITEMS; i++) {
            lv_obj_t* btn = lv_obj_create(menu_cont);
            lv_obj_set_size(btn, 212, MENU_ITEM_H);
            mkCard(btn, CLR_SURFACE, 7);

            // 选中态左侧强调条
            lv_obj_t* sel = lv_obj_create(btn);
            lv_obj_set_size(sel, 3, MENU_ITEM_H - 8);
            lv_obj_set_pos(sel, 0, 4);
            lv_obj_set_style_bg_color(sel, lv_color_hex(CLR_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sel, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(sel, 2, LV_PART_MAIN);
            lv_obj_set_style_border_width(sel, 0, LV_PART_MAIN);
            lv_obj_clear_flag(sel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* lbl = lv_label_create(btn);
            mkLabel(lbl, &lv_font_simsun_16_cjk, CLR_TEXT);
            lv_label_set_text(lbl, menuItemsCN[i]);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);

            // 右侧序号
            lv_obj_t* num = lv_label_create(btn);
            mkLabel(num, &lv_font_montserrat_12, CLR_TEXT_MUTE);
            static char numBuf[MENU_ITEMS][4];
            snprintf(numBuf[i], sizeof(numBuf[i]), "%02d", i + 1);
            lv_label_set_text(num, numBuf[i]);
            lv_obj_align(num, LV_ALIGN_RIGHT_MID, -12, 0);

            // 当前值角标：目前只有「屏保风格」用得上，其余项留空、设成透明
            lv_obj_t* val = lv_label_create(btn);
            mkLabel(val, &lv_font_simsun_16_cjk, CLR_ACCENT);
            lv_label_set_text(val, "");
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, -34, 0);
            if (i != MENU_ITEMS - 1) lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);
            menu_items_val[i] = val;

            menu_items[i] = btn;
        }
    }

    // 屏保风格那一项把当前值直接顶在条目右边，省得进二级界面才知道选了什么
    if (menu_items_val[MENU_ITEMS - 1]) {
        setText(menu_items_val[MENU_ITEMS - 1],
                saverMode < TOTAL_SAVER_MODES ? saverModeNames[saverMode] : "");
    }

    // ---- 更新页码 ----
    if (menu_position) {
        static char posBuf[16];
        snprintf(posBuf, sizeof(posBuf), "%d/%d", menuSel + 1, MENU_ITEMS);
        setText(menu_position, posBuf);
    }

    // ---- 更新每项的位置（真滚动）与配色 ----
    for (int i = 0; i < MENU_ITEMS; i++) {
        lv_obj_t* btn = menu_items[i];
        if (btn == nullptr) continue;

        int slot = i - menuScrollOffset;                 // 在窗口里的第几行
        bool isVisible = (slot >= 0 && slot < MENU_VISIBLE_ITEMS);
        bool isSelected = (i == menuSel);

        if (isVisible) lv_obj_set_pos(btn, 0, slot * MENU_PITCH);
        // 窗口外的项移出容器并隐藏（不删，重建成本高）
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
            lv_obj_set_style_text_color(num,
                lv_color_hex(isSelected ? CLR_ACCENT : CLR_TEXT_MUTE), LV_PART_MAIN);
        }
    }

    // ---- 底部按键提示 ----
    static lv_obj_t* hint = nullptr;
    if (hint == nullptr) {
        hint = lv_label_create(scr_menu);
        mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(hint, "上下选择 · 回车确认 · ESC 返回");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    showScreen(scr_menu);
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
    char nbuf[8];
    snprintf(nbuf, sizeof(nbuf), "%d", notifCount);
    lv_obj_t* cntNum = lv_label_create(scr_notif);
    mkLabel(cntNum, &lv_font_montserrat_20, accent);
    lv_label_set_text(cntNum, nbuf);
    lv_obj_align(cntNum, LV_ALIGN_TOP_RIGHT, -18, 12);

    lv_obj_t* cntLbl = lv_label_create(scr_notif);
    mkLabel(cntLbl, &lv_font_simsun_16_cjk, CLR_TEXT);
    lv_label_set_text(cntLbl, "条待处理");
    lv_obj_align_to(cntLbl, cntNum, LV_ALIGN_OUT_LEFT_TOP, -3, 3);

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
    if (set_alarm_sw) {
        if (alarmEnabled) lv_obj_add_state(set_alarm_sw, LV_STATE_CHECKED);
        else               lv_obj_clear_state(set_alarm_sw, LV_STATE_CHECKED);
    }
    setText(set_alarm_state_lbl, alarmEnabled ? "已开启" : "已关闭");
    if (set_alarm_state_lbl) {
        lv_obj_set_style_text_color(set_alarm_state_lbl,
            lv_color_hex(alarmEnabled ? CLR_GREEN : CLR_TEXT_MUTE), LV_PART_MAIN);
    }
    for (int i = 0; i < 2; i++) markField(set_alarm_field_labels[i], i == alarmFieldIdx);
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
static lv_obj_t* settingShell(lv_obj_t* scr, const char* titleCN, const char* titleEn) {
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

    lv_obj_t* hint = lv_label_create(scr);
    mkLabel(hint, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
    lv_label_set_text(hint, "←→ 切换 · ↑↓ 调整 · 回车保存 · ESC 取消");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    return scr;
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

        // 开关 + 文字状态
        set_alarm_sw = lv_switch_create(card);
        lv_obj_set_size(set_alarm_sw, 52, 28);
        lv_obj_align(set_alarm_sw, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(set_alarm_sw, lv_color_hex(CLR_TEXT), LV_PART_KNOB);

        set_alarm_state_lbl = lv_label_create(card);
        mkLabel(set_alarm_state_lbl, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(set_alarm_state_lbl, "已关闭");
        lv_obj_align(set_alarm_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

        const char* fieldNames[2] = { "时", "分" };
        int fieldX[2] = { -30, 30 };
        for (int i = 0; i < 2; i++) {
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
        settingShell(scr_settings_timer, "倒计时", "TIMER");

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
        settingShell(scr_settings_caltemp, "温度校准", "CALIBRATE");

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

        lv_obj_t* hint2 = lv_label_create(scr_settings_caltemp);
        mkLabel(hint2, &lv_font_simsun_16_cjk, CLR_TEXT_MUTE);
        lv_label_set_text(hint2, "↑↓ 调整偏移量");
        lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 92);
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

        // 大字预览卡
        lv_obj_t* card = lv_obj_create(scr_settings_light);
        lv_obj_set_size(card, 204, 80);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -34);
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
        const int chipX[2] = { -52, 52 };
        const int chipY[3] = { 26, 53, 80 };
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
        lv_obj_t* shell_hint = lv_obj_get_child(scr_recording, 3);
        if (shell_hint) {
            setText(shell_hint, "M1-M12 保存 · MR 下一步 · ESC 取消");
            lv_obj_set_width(shell_hint, 204);
            lv_label_set_long_mode(shell_hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(shell_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
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
            alarmFieldIdx = (alarmFieldIdx + dir + 2) % 2;
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
            shtTempOffset = constrain(shtTempOffset + delta * 0.5f, 50.0f, 80.0f);
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
            alarmEnabled = lv_obj_has_state(set_alarm_sw, LV_STATE_CHECKED);
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
    }
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
// 修法:把 CMB 小键盘的编码从 HID Usage(0x53-0x63)统一改成 ASCII,这样
//   Num / = '/' = 0x2F    _asciimap[0x2F] = 0x38 → HID /
//   Num * = '*' = 0x2A    _asciimap[0x2A] = 0x25|SHIFT → SHIFT+8 → '*'
//   Num - = '-' = 0x2D    _asciimap[0x2D] = 0x2D → HID -
//   Num + = '+' = 0x2B    _asciimap[0x2B] = 0x2e|SHIFT → SHIFT+= → '+'
//   Num Enter = '\n'=0x0A  _asciimap[0x0A] = 0x28 → HID Enter
//   Num 1-9 = '1'-'9'     _asciimap[0x31..0x39] = 0x1E-0x26 → HID 1-9
//   Num 0 = '0'           _asciimap[0x30] = 0x27 → HID 0
//   Num . = '.'           _asciimap[0x2E] = 0x37 → HID .
// 全部走 Keyboard.press() 就好,数值歧义彻底没了。
//
// 这条修法的关键证据是 s3-setting.html:1056 那条已知 bug 注释,
// 之前一直没修是因为没看出 0x53-0x63 跟 ASCII 'a'-'c' 的数值撞车。
static inline void kbPress(uint8_t code) {
    // [DEBUG a→9] 临时诊断:打出实际走的分支,定位 baseKey 是多少、走的是哪条路径
    const char* path = "press";
    if (code >= 0x80 && code < 0x88) {
        path = "pressRaw(mod+0x60)";
        Keyboard.pressRaw((uint8_t)(code + 0x60));
    } else if (code != 0) {
        path = "press(_asciimap)";
        Keyboard.press(code);
    } else {
        path = "skip(null)";
    }
    LOG_PORT.printf("[KB] press code=0x%02X path=%s\n", code, path);
}

static inline void kbRelease(uint8_t code) {
    if (code >= 0x80 && code < 0x88) Keyboard.releaseRaw((uint8_t)(code + 0x60));
    else if (code != 0) Keyboard.release(code);
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
            recoverI2CBus();
            return;
        }
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
                        const bool inUiMode = (currentSysMode == SYS_MODE_MENU)
                                              || IS_SETTING_MODE(currentSysMode)
                                              || inRecMode;

                        if (inUiMode) {
                            // ---- 界面态：这一整块把按键吃干净，一律不发到主机 ----

                            // 菜单：方向键移动 / 回车选中 / ESC·MC 退回主屏
                            if (currentSysMode == SYS_MODE_MENU) {
                                if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
                                    menuSel = (menuSel + 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
                                    menuSel = (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
                                    build_menu();
                                } else if (baseKey == KEY_RETURN) {
                                    handleMenuSelect();
                                } else if (baseKey == KEY_ESC || baseKey == K_MC) {
                                    gotoMainScreen();
                                }
                            }
                            // 设置子界面：左右切字段 / 上下调值 / 回车保存 / ESC·MC 取消
                            else if (IS_SETTING_MODE(currentSysMode)) {
                                if (baseKey == KEY_LEFT_ARROW) moveSettingField(-1);
                                else if (baseKey == KEY_RIGHT_ARROW) moveSettingField(1);
                                else if (baseKey == KEY_UP_ARROW) adjustSettingField(1);
                                else if (baseKey == KEY_DOWN_ARROW) adjustSettingField(-1);
                                else if (baseKey == KEY_RETURN) saveSettingScreen();
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
                                menuSel = 0;
                                menuScrollOffset = 0;
                                menuNeedsRebuild = true;
                                // 不再弹"系统菜单/请选择功能"HUD：菜单本身就是提示，
                                // 每次进菜单都闪一次只是噪音，还压住菜单第一屏。

                            }
                            // MA / MB 全局键
                            else if (baseKey == K_MA) {
                                executeGlobalKey("MA");
                            }
                            else if (baseKey == K_MB) {
                                executeGlobalKey("MB");
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
                                    // LOGO 短按 = 切换主屏风格；Fn+LOGO 仍然保留重启
                                    if (fnPressed) {
                                        triggerHud("系统重启", "请稍候", lv_color_hex(CLR_RED));
                                        pendingRestartMs = millis() + 600;
                                    } else {
                                        cycleDisplayStyle();
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
                            logLastNotifyMs = 0;
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
                            // LOGO 的切风格动作已经挪到"按下"时做了（press 更跟手，
                            // 放在 release 上会让人以为没反应），这里不用再管。
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
static void recoverI2CBus(void) {
    Wire.end();
    delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(25);
    if (mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        for (int c = 0; c < NUM_COLS; c++) {
            mcp.pinMode(c, OUTPUT);
            mcp.digitalWrite(c, HIGH);
        }
    }
}

// ===========================
// 命令处理
// ===========================
static void handleCommand(const String& cmd) {
    // DISP_MODE:n - Switch display mode
    if (cmd.startsWith("DISP_MODE:")) {
        int mode = cmd.substring(10).toInt();
        if (mode >= 0 && mode < TOTAL_DISP_MODES) {
            currentDispMode = mode;
            preferences.putUChar("disp_mode", currentDispMode);
            renderCurrentDisplayBase();
            // 主机切风格时用户可能正停在菜单/设置界面，别把他踢出当前界面
            if (currentSysMode == SYS_MODE_NORMAL) showScreen(ensureMainScreen());
            triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_ACCENT));
        }
    }
    // TIME:epoch - Sync time
    else if (cmd.startsWith("TIME:")) {
        time_t t = cmd.substring(5).toInt();
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        preferences.putUInt("set_epoch", (uint32_t)t);
        triggerHud("时间", "已同步", lv_color_hex(CLR_GREEN));
    }
    // ALARMSET:HH:MM - Set alarm
    else if (cmd.startsWith("ALARMSET:")) {
        String timeStr = cmd.substring(9);
        int colonIdx = timeStr.indexOf(':');
        if (colonIdx > 0) {
            alarmHour = (uint8_t)timeStr.substring(0, colonIdx).toInt();
            alarmMinute = (uint8_t)timeStr.substring(colonIdx + 1).toInt();
            alarmEnabled = true;
            preferences.putUChar("alarm_h", alarmHour);
            preferences.putUChar("alarm_m", alarmMinute);
            preferences.putBool("alarm_on", alarmEnabled);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", alarmHour, alarmMinute);
            triggerHud("闹钟", buf, lv_color_hex(CLR_GREEN));
        }
    }
    // TIMERSET:HH:MM:SS or TIMERSET:STOP
    else if (cmd.startsWith("TIMERSET:")) {
        String timeStr = cmd.substring(9);
        if (timeStr == "STOP") {
            timerRunning = false;
            timerRemainSec = timerTotalSec;
            triggerHud("倒计时", "已停止", lv_color_hex(CLR_AMBER));
        } else {
            int firstColon = timeStr.indexOf(':');
            int secondColon = timeStr.lastIndexOf(':');
            if (firstColon > 0 && secondColon > firstColon) {
                timerEditH = timeStr.substring(0, firstColon).toInt();
                timerEditM = timeStr.substring(firstColon + 1, secondColon).toInt();
                timerEditS = timeStr.substring(secondColon + 1).toInt();
                timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
                timerRemainSec = timerTotalSec;
                timerRunning = true;
                timerStartMs = millis();
                char buf[16];
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timerEditH, timerEditM, timerEditS);
                triggerHud("倒计时", buf, lv_color_hex(CLR_ACCENT));
            }
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
                // 清掉这个方案的全部规则：条数归零 + 逐条擦掉 NVS 里的键，
                // 只置 0 的话那些 rmp_p_i 键还留在 NVS 里，换固件/读档时会诈尸。
                if (wantClear) {
                    for (int i = 0; i < remapCounts[prof]; i++) {
                        char itemKey[20];
                        snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
                        preferences.remove(itemKey);
                    }
                    remapCounts[prof] = 0;
                    memset(profileRemaps[prof], 0, sizeof(profileRemaps[prof]));
                    char key[16];
                    snprintf(key, sizeof(key), "rmp_cnt_%d", prof);
                    preferences.putInt(key, 0);
                }

                if (rules.length() > 0) {
                    // 整份替换：先把旧的清掉，再写新的
                    for (int i = 0; i < remapCounts[prof]; i++) {
                        char itemKey[20];
                        snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, i);
                        preferences.remove(itemKey);
                    }
                    remapCounts[prof] = 0;

                    int start = 0;
                    while (start < rules.length() && remapCounts[prof] < MAX_REMAP_RULES) {
                        int semicolon = rules.indexOf(';', start);
                        String rule = (semicolon > 0) ? rules.substring(start, semicolon) : rules.substring(start);
                        int comma = rule.indexOf(',');
                        if (comma > 0) {
                            profileRemaps[prof][remapCounts[prof]].fromKey = normalizeRemapKey((uint16_t)rule.substring(0, comma).toInt());
                            profileRemaps[prof][remapCounts[prof]].toKey = normalizeRemapKey((uint16_t)rule.substring(comma + 1).toInt());

                            char itemKey[20];
                            snprintf(itemKey, sizeof(itemKey), "rmp_%d_%d", prof, remapCounts[prof]);
                            uint32_t val = ((uint32_t)profileRemaps[prof][remapCounts[prof]].fromKey << 16) | profileRemaps[prof][remapCounts[prof]].toKey;
                            preferences.putUInt(itemKey, val);
                            remapCounts[prof]++;
                        }
                        start = (semicolon > 0) ? semicolon + 1 : rules.length();
                    }
                    char key[16];
                    snprintf(key, sizeof(key), "rmp_cnt_%d", prof);
                    preferences.putInt(key, remapCounts[prof]);
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%d 条已保存", remapCounts[prof]);
                    LOG_PORT.printf("[REMAP] prof=%d saved %d rules\n", prof, remapCounts[prof]);
                    triggerHud("按键重映射", buf, lv_color_hex(CLR_GREEN));
                } else if (wantClear) {
                    LOG_PORT.printf("[REMAP] prof=%d cleared\n", prof);
                    triggerHud("按键重映射", "已清空", lv_color_hex(CLR_AMBER));
                }
            }
        }
    }
    // REMAP:prof:read - 把这个方案现有的规则回读给网页
    // （回读只能靠 BLE notify，所以这里往同一个特征里 notify 一条 REMAPDUMP:）
    else if (cmd.startsWith("REMAPREAD:")) {
        int prof = cmd.substring(9).toInt();
        char out[BLE_CMD_BUF_SIZE];
        if (prof < 0 || prof >= TOTAL_PROFILES) {
            snprintf(out, sizeof(out), "REMAPDUMP:%d:ERR", prof);
        } else {
            int n = snprintf(out, sizeof(out), "REMAPDUMP:%d:", prof);
            for (int i = 0; i < remapCounts[prof] && n < (int)sizeof(out) - 16; i++) {
                n += snprintf(out + n, sizeof(out) - n, "%s%u,%u",
                               i ? ";" : "",
                               profileRemaps[prof][i].fromKey, profileRemaps[prof][i].toKey);
            }
        }
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)out, strlen(out));
            pCharacteristic->notify();
        }
        LOG_PORT.printf("[REMAP] read prof=%d -> %s\n", prof, out);
        char buf[24];
        snprintf(buf, sizeof(buf), "读取到 %d 条", (prof >= 0 && prof < TOTAL_PROFILES) ? remapCounts[prof] : 0);
        triggerHud("按键重映射", buf, lv_color_hex(CLR_ACCENT));
    }
    // MACRODUMP:p{N}_{MKey} - 把方案 N 的 MKey 宏回读给网页
    // 协议:`MACRODUMP:p0_M1:SEQ:abc...` / `MACRODUMP:p0_M1:CMB:128,4` /
    //       `MACRODUMP:p0_M1:NONE`(未设置)
    // 注意:BLE notify 受 BLE_CMD_BUF_SIZE=256 限制,SEQ 击键流超过 ~240 字节会被截断。
    // 实际击键流(尤其是带 [ENTER] 的)很少超 100 字节,但极长字符串会丢尾,网页要明确提示。
    else if (cmd.startsWith("MACRODUMP:")) {
        String key = cmd.substring(10);
        String val = preferences.getString(key.c_str(), "");
        // 拼成 `MACRODUMP:<key>:<val>`,val 为空时显式 NONE 让网页知道"该键无宏"
        char out[BLE_CMD_BUF_SIZE];
        bool truncated = false;
        if (val.length() == 0) {
            snprintf(out, sizeof(out), "MACRODUMP:%s:NONE", key.c_str());
        } else {
            // 留 1 字节给 '\0',val 太长则截断并标 TRUNC
            size_t keyLen = strlen("MACRODUMP:") + key.length() + 1;  // "MACRODUMP:" + key + ":"
            if (keyLen + val.length() >= sizeof(out) - 8) {
                truncated = true;
                val = val.substring(0, sizeof(out) - 8 - keyLen - 1);
            }
            snprintf(out, sizeof(out), "MACRODUMP:%s:%s%s",
                     key.c_str(), val.c_str(), truncated ? "...TRUNC" : "");
        }
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)out, strlen(out));
            pCharacteristic->notify();
        }
        LOG_PORT.printf("[MACRODUMP] %s len=%u truncated=%d\n",
                        key.c_str(), (unsigned)val.length(), truncated ? 1 : 0);
    }
    // GKEYDUMP:KEY - 把全局键 (MA/MB) 配置回读给网页
    // 协议:`GKEYDUMP:MA:SW:1+CMB:128,4` / `GKEYDUMP:MA:NONE`
    else if (cmd.startsWith("GKEYDUMP:")) {
        String key = cmd.substring(9);
        String gKey = "g_" + key;
        String val = preferences.getString(gKey.c_str(), "");
        char out[BLE_CMD_BUF_SIZE];
        snprintf(out, sizeof(out), "GKEYDUMP:%s:%s", key.c_str(),
                 val.length() ? val.c_str() : "NONE");
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)out, strlen(out));
            pCharacteristic->notify();
        }
        LOG_PORT.printf("[GKEYDUMP] %s len=%u\n", key.c_str(), (unsigned)val.length());
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
    // GSET:name:value - Global key assignment
    else if (cmd.startsWith("GSET:")) {
        String params = cmd.substring(5);
        int colonIdx = params.indexOf(':');
        if (colonIdx > 0) {
            String name = params.substring(0, colonIdx);
            String value = params.substring(colonIdx + 1);
            char gKey[32];
            snprintf(gKey, sizeof(gKey), "g_%s", name.c_str());
            preferences.putString(gKey, value);
            triggerHud("已写入全局键", name.c_str(), lv_color_hex(CLR_ACCENT));
        }
    }
    // MACROS_RESET: 清掉所有方案的 M1-M12 + 全局 MA/MB。
    // 保留 remap 规则 / 时钟 / 闹钟 / 灯光 / 壁纸 / SPIFFS 等其他 NVS 键，
    // 只删 p?\d_M?\d+\d 这种格式(方案专属宏)和 g_MA / g_MB(全局动作)。
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
        // 全局动作
        if (preferences.remove("g_MA")) removed++;
        if (preferences.remove("g_MB")) removed++;
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
    //   LOG:on     - 打开实时流(新日志按行 notify `LOG:<text>\n`,10Hz 节流)
    //   LOG:off    - 关掉流转发(缓冲仍然在落,只是不 notify 了)
    //   LOG:dump   - 把环形缓冲整段按行 notify `LOGDUMP:<chunk>\n` ... `LOGDUMP:END\n`
    //   LOG:clear  - 清空环形缓冲(不影响流转发开关)
    // 网页侧 onBleNotify 已经按 \n 拼行分发,加 LOG: / LOGDUMP: 两条分支即可显示。
    // **所有响应末尾必须带 \n**:网页端的 split('\n') 才能切出完整行给 LOG_PORT_UI /
    // appendLogLine,不然会被 parts.pop() 整个吞进 rest,for 循环空跑。
    else if (cmd == "LOG:on") {
        logStreamOn = true;
        logLastNotifyMs = 0;            // 立即允许第一条(开流响应那条不卡)
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)"LOG:STREAM_ON\n", 15);
            pCharacteristic->notify();
        }
        LOG_PORT.println("[LOG] stream ON");
    }
    else if (cmd == "LOG:off") {
        logStreamOn = false;
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)"LOG:STREAM_OFF\n", 16);
            pCharacteristic->notify();
        }
        LOG_PORT.println("[LOG] stream OFF");
    }
    else if (cmd == "LOG:clear") {
        logRingLen = 0;
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)"LOG:CLEARED\n", 12);
            pCharacteristic->notify();
        }
        LOG_PORT.println("[LOG] buffer cleared");
    }
    else if (cmd == "LOG:dump") {
        if (pCharacteristic && logRingBuf) {
            // 单包上限 BLE_CMD_BUF_SIZE - "LOGDUMP:" 前缀开销(~8字节) -
            // 末尾\0 - 留点余量 -> 一片不超过 220 字节。
            // 按整行切(不要在行中间断),保证网页那边拼起来是一行一行。
            // 末尾必加 \n:网页按 \n 切行,缺一个整片就废了。
            const size_t chunkMax = 220;
            size_t pos = 0;
            bool sent = false;
            while (pos < logRingLen) {
                size_t end = pos;
                while (end < logRingLen && end - pos < chunkMax) {
                    if (logRingBuf[end] == '\n') { end++; break; }
                    end++;
                }
                if (end == pos) { end++; }   // 兜底:行比 chunkMax 还长就硬切
                char out[BLE_CMD_BUF_SIZE];
                int m = snprintf(out, sizeof(out) - 2, "LOGDUMP:");
                int n = 0;
                if (m > 0 && m < (int)sizeof(out) - 2) {
                    n = snprintf(out + m, sizeof(out) - 2 - m, "%.*s",
                                 (int)(end - pos), logRingBuf + pos);
                }
                int total = (n > 0) ? m + n : m;
                if (total < (int)sizeof(out) - 2) {
                    out[total++] = '\n';     // 末尾必加 \n
                    out[total]   = '\0';
                }
                if (total > 0) {
                    pCharacteristic->setValue((uint8_t*)out, (size_t)total);
                    pCharacteristic->notify();
                    sent = true;
                }
                pos = end;
                // 让 NimBLE 任务跑一下,处理刚才发出的 notify。
                // delay(5) 在每包之间堵 5ms,8KB 缓冲全 dump 完会卡 200ms+,
                // 屏幕和键盘扫描都会肉眼可见地抖。yield() 等价于
                // vTaskDelay(1),NimBLE 任务抢到时间片继续发包,
                // 但 loop() 这边差不多 1ms 内又回来 —— 实际效果接近不卡。
                yield();
            }
            if (!sent) {
                // 缓冲空也要回一个,免得网页干等
                pCharacteristic->setValue((uint8_t*)"LOGDUMP:\n", 9);
                pCharacteristic->notify();
            }
            pCharacteristic->setValue((uint8_t*)"LOGDUMP:END\n", 13);
            pCharacteristic->notify();
        }
        LOG_PORT.printf("[LOG] dump %u bytes\n", (unsigned)logRingLen);
    }
    // ME 键文本：网页端把 UTF-8 文本转成 hex 后分片下发，这里只负责落盘。
    // 按下 ME 键时才由 executeMacro("ME") 读出来发给电脑（见那里的 [HEXS] 协议），
    // 所以下发阶段一个字都不往主机打 —— 蓝牙发消息 ≠ 立刻在电脑上打字。
    else if (cmd == "ME_START") {
        File f = SPIFFS.open("/me_hex.txt", FILE_WRITE);
        if (f) f.close();
        meHexBytes = 0;
        LOG_PORT.println("[ME] START");
    }
    else if (cmd.startsWith("ME_DATA:")) {
        String hex = cmd.substring(8);
        if (hex.length() == 0) return;
        File f = SPIFFS.open("/me_hex.txt", FILE_APPEND);
        if (f) { f.print(hex); f.close(); meHexBytes += hex.length(); }
        else LOG_PORT.println("[ME] append FAILED (SPIFFS open failed)");
    }
    else if (cmd == "ME_END") {
        size_t sz = 0;
        if (SPIFFS.exists("/me_hex.txt")) {
            File f = SPIFFS.open("/me_hex.txt", FILE_READ);
            if (f) { sz = f.size(); f.close(); }
        }
        LOG_PORT.printf("[ME] END onDisk=%u appended=%u\n", (unsigned)sz, (unsigned)meHexBytes);
        if (sz == 0) triggerHud("ME 文本", "存入失败", lv_color_hex(CLR_RED));
        else {
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

        if (total == 0 || total > LOGO_RX_MAX) {
            triggerHud("壁纸传输", "长度不合法", lv_color_hex(CLR_RED));
        } else if (logoRxAlloc(total) == nullptr) {
            triggerHud("壁纸传输", "内存不足", lv_color_hex(CLR_RED));
        } else {
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
        char out[64];
        const char* state = logoRxActive ? (logoRxDone ? "READY" : "RECV")
                                         : (wpReady ? "OK" : "IDLE");
        snprintf(out, sizeof(out), "LOGOSTATUS:%s:%lu/%lu",
                 state, (unsigned long)logoRxGot, (unsigned long)logoRxTotal);
        if (pCharacteristic) {
            pCharacteristic->setValue((uint8_t*)out, strlen(out));
            pCharacteristic->notify();
        }
        LOG_PORT.printf("[WALLPAPER] status: %s\n", out);
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

// ===========================
// 旋钮：按 currentMode 调对应的东西
// ===========================
// 界面态（菜单 / 设置页）里旋钮是导航和调参，只有回到主界面它才是"调灯"。
// 音量、屏幕亮度、静音都是 Consumer 页 usage，本机并不真的持有音量 ——
// 只能把 usage 丢给主机，由操作系统去调（这和原版 s3.ino 的做法一致）。
static void knobAdjust(int dir) {
    if (currentSysMode == SYS_MODE_MENU) {
        menuSel = (dir > 0) ? (menuSel + 1) % MENU_ITEMS
                            : (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
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
        // 静音键同时也是告警的确认键：顺手把最新一条处理掉
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
    uint8_t ledScale = brightness;
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
        setBgColor(topLockDot[i], locks[i] ? topLockOn[i] : CLR_STROKE);
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
            // 中间的按键反馈，跟"按键回显开关"走（"--" 而不是中文，
            // 这个 label 是 montserrat_28，画不出汉字）
            setText(gk_lbl_lastkey, showKeystrokes ? lastKeyPressed : "--");
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
            // 菜单第 4 项「按键回显开关」控制的就是这里：关掉后不再显示具体按键名。
            // 关闭态用 "--" 而不是中文"已关闭"：这个 label 挂的是 montserrat_48，
            // 而 Montserrat 里没有汉字也没有 CJK 回退（CJK 字体的 fallback 是单向的
            // —— simsun→montserrat 有，montserrat→simsun 没有），
            // 写中文上去就是一片空白，看着像坏了。
            setText(ip_lbl_lastkey, showKeystrokes ? lastKeyPressed : "--");
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
            // "--" 而不是"已关闭"：同 INFO_PANEL 分支，这个 label 是 montserrat_48，
            // 画不出汉字。
            setText(km_lbl_lastkey, showKeystrokes ? lastKeyPressed : "--");
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
    USB.begin();

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

    // 恢复时间
    time_t savedEpoch = (time_t)preferences.getUInt("set_epoch", 0);
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
    totalKeyCount = preferences.getUInt("keyCount", 0);

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

    shtTempOffset = preferences.getFloat("sht_offset", 62.0f);

    // I2C / MCP23017
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    if (!mcp.begin_I2C(MCP23017_ADDR, &Wire)) {
        LOG_PORT.println("[MCP23017] Init failed, recovering...");
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
    }

    // BLE
    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);
    // 必须在 BLE 起来之前建好：onWrite 里判的就是 bleCmdQueue != nullptr
    bleCmdQueue = xQueueCreate(BLE_CMD_QUEUE_LEN, BLE_CMD_BUF_SIZE);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* pService = pServer->createService(SERVICE_UUID);
    // 必须带 NOTIFY：网页的"从键盘读取"按钮要靠它把映射规则回读上来
    // （REMAP:prof:read -> 固件 notify 一条 REMAPDUMP:...）。只给 WRITE 的话，
    // 键盘没有任何回话通道，网页上就永远是空的，用户也分不清"没保存"还是"读不出来"。
    pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->setCallbacks(new MyCallbacks());
    // 必须显式 addDescriptor(BLE2902),不然 CCCD 描述符不在 GATT 数据库里,
    // 客户端(Chrome Web Bluetooth)调 startNotifications() 时往 CCCD 写订阅位会
    // 直接返回 "GATT Error: Not supported",网页上 read-back 全超时 —— 表现就是
    // "切了键啥也不显示"。NimBLE 不会自动加 CCCD,得手动。
    pCharacteristic->addDescriptor(new BLE2902());
    pService->start();
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
    SPIFFS.begin(true);

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
        esp_restart();
    }

    // BLE 重连
    // 主机指令在这里统一落到 LVGL 任务：NimBLE 回调只入队，
    // 解析和所有 lv_* 调用都在这一行之后，和键盘触发的路径同属一个任务。
    drainBleCommands();

    // 壁纸：收齐了就解码（几百毫秒，放主任务不占 BLE 主机任务）。
    // 传一半断线（关网页、走出范围）必须超时放掉缓冲，否则后面所有文本
    // 指令都会被当成 JPEG 数据吃掉，键盘直接"失联"。
    if (logoRxDone) finishLogoUpload();
    if (logoRxActive && !logoRxDone && millis() - logoRxLastMs > LOGO_RX_TIMEOUT_MS) {
        abortLogoUpload("传输中断");
    }

    // USB HID 厂商通道走同一套纪律：USB 回调只往环形缓冲里塞字节，
    // 解析放在这里，和键盘触发的路径在同一个任务里碰 LVGL。
    handleHidVendorCommands();

    // C3 侧键 / 旋钮：Serial1 的行解析。放这里而不是中断里 ——
    // 灯光键确认通知要重画面板、旋钮要弹 HUD，都在主任务里做。
    handleC3Events();

    if (!deviceConnected && oldDeviceConnected) {
        ct_mark(CT_S_BLE_DELAY);
        delay(500);
        BLEDevice::startAdvertising();
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

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

    // 键盘扫描
    if (millis() - lastScanTime >= SCAN_INTERVAL) {
        lastScanTime = millis();
        ct_mark(CT_S_SCAN);
        scanKeyboardMatrix();
    }

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
    if (currentSysMode == SYS_MODE_NORMAL && millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
        currentSysMode = SYS_MODE_SLEEP;
        ct_mark(CT_S_SLEEP);
        // SAVER_OFF 走老路径拆主屏；壁纸/时间温湿度则另起一块屏保屏，
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
        updateDynamicElements();
        // 律动页的柱子动画走独立 16ms 节拍，跟上面 100ms 的慢刷新解耦。
        // 放在 updateDynamicElements() 里面就是"律动很慢"的根因。
        tickRhythm();
    } else if (currentSysMode == SYS_MODE_SLEEP) {
        // 息屏期间 updateDynamicElements() 不跑，屏保的时钟/温湿度自己刷
        updateScreensaver(false);
    }

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
