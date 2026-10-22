/**
 * ble_probe —— 蓝牙链路层探针固件
 *
 * ============================ 为什么要有这个 ============================
 *
 * 现象：连上就崩、发 ME 就崩、蓝牙时好时坏。
 * 但产品固件（lvgl_s3.ino）有 LVGL / 传感器 / NVS / USB HID 四套东西缠在一起，
 * 崩了到底是"链路层自己有问题"还是"链路层和界面抢东西"，根本分不开。
 *
 * 所以把 **src/bt_link.cpp 原样**（软链接过来，不是抄一份）单独拎出来跑：
 *   · 没有 LVGL、没有屏幕、没有传感器 → 内存干净、行为确定
 *   · 崩了就一定是链路层
 *   · USB 只做 CDC（不做 HID），所以 /dev/cu.usbmodem* 一直能读日志
 *
 * ============================ 这个固件要回答的问题 ============================
 *
 * Q1. **断线之后，设备还能被重新扫到吗？**
 *     产品固件在 loop() 里做："断了 → 阻塞 500ms → BLEDevice::startAdvertising()"
 *     **只试一次**，而且靠一个固定的 500ms 赌栈已经清理干净。
 *     如果那一次失败（比如协议栈还没释放连接），设备就**永远不再广播**，
 *     除非重新上电 —— 表现就是"蓝牙时好时坏"。
 *     → 用 PROBE:ADVDELAY / PROBE:ADVRETRY 扫一遍"多久能广播成功、试几次才成功"。
 *
 * Q2. **conn_id 到底是几？重连之后会变吗？**
 *     bt_link.cpp 里 btLinkPayloadMax() 写的是 `s_srv->getPeerMTU(0)` —— 硬编码 0。
 *     而 bluedroid 给 GATT server 的 conn_id = tcb_idx（连接槽位号），
 *     **不保证是 0**：上一个连接还没被回收干净时，新连接会拿到槽位 1。
 *     → 库里 `find(conn_id)->second.mtu` **没有 end() 检查**，查不到就是读垃圾。
 *
 * Q3. **协商后的真实 MTU 是多少？** 决定一条通知最多能带多少字节。
 *
 * Q4. **超 MTU 的通知会怎样？**（库里的 notify() 日志说"Truncating"但其实没截断）
 *
 * Q5. **20KB 的回拉会丢多少片？**（协议栈拥塞时通知是**静默**丢掉的）
 *
 * ============================ 协议 ============================
 *
 * 和产品固件完全一致：
 *   服务   4fafc201-1fb5-459e-8fcc-c5c9c331914b
 *   特征   beb5483e-36e1-4688-b7f5-ea07361b26a8   (WRITE | WRITE_NR | NOTIFY)
 *   指令   一行一条，以 '\n' 结尾
 *
 * 额外加几个**只给测试用**的指令（产品固件没有）：
 *   PROBE:RESET       → esp_restart()，远程重启（免得每次都要人按 RESET 键）
 *   PROBE:DUMP        → 打印 peer 表 + 预算
 *   PROBE:ADVDELAY:n  → 断线后 n 毫秒再重新广播
 *   PROBE:ADVRETRY:n  → 重新广播最多试几次（0 = 只试一次，像产品固件）
 *   PROBE:MTU         → 回一条正文刚好等于当前预算的通知
 *   PROBE:BIG:n       → 回一条正文 n 字节的通知（故意超 MTU）
 *   PROBE:STREAM:n    → 回一条 n 字节的流式正文（量丢片率）
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLE2902.h>
#include "bt_link.h"
#include "crash_trace.h"

// crash_trace.h 里这三个符号的唯一实现在产品固件里，这里补一份（探针不用真崩溃现场）
RTC_DATA_ATTR CrashTrace ct_rec;
CrashTrace ct_prev;
volatile uint32_t    ct_heartbeat = 0;

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ---------------------------------------------------------------- 崩溃阶段标记
//
// panic 之后芯片会复位，栈上的现场全没了。但 **RTC 慢存（RTC_DATA_ATTR）在
// 软件复位/panic 之后还在**（只有掉电才丢）—— 所以把"崩之前正在干什么"记进去，
// 下次开机打出来，就知道它死在哪一步。
//
// 这套办法和产品固件里的 crash_trace.h 是同一个思路，只是这里更粗：只记阶段，
// 不记栈。
#define PH_BOOT        0
#define PH_IDLE        1
#define PH_DISCONNECT  2   // 正在 onDisconnect 回调里
#define PH_ADV_WAIT    3   // 等待重新广播的窗口
#define PH_ADV_CALL    4   // 正在 BLEDevice::startAdvertising() 里
#define PH_CMD         5   // 正在处理主机指令
#define PH_NOTIFY      6   // 正在发通知
#define PH_CONNECT     7   // 正在 onConnect 回调里

static RTC_DATA_ATTR uint32_t s_phase = PH_BOOT;
static RTC_DATA_ATTR uint32_t s_connSeq = 0;

// 阶段名（放在 .ino 里是为了字体生成脚本能扫到中文，这里用英文更稳）
static const char* phaseName(uint32_t p) {
    switch (p) {
        case PH_BOOT:       return "BOOT";
        case PH_IDLE:       return "IDLE";
        case PH_DISCONNECT: return "DISCONNECT-cb";
        case PH_ADV_WAIT:   return "WAIT-READV";
        case PH_ADV_CALL:   return "CALL-startAdvertising";
        case PH_CMD:        return "HANDLE-CMD";
        case PH_NOTIFY:     return "SEND-NOTIFY";
        case PH_CONNECT:    return "CONNECT-cb";
        default:            return "?";
    }
}

static BLECharacteristic* pChar   = nullptr;
static BLEServer*          pServer = nullptr;
static unsigned long       lastDumpMs = 0;
static uint32_t            connSeq  = 0;   // 第几次连接

// ---- 流式喂数据的状态（由 loop() 推进，不在指令回调里等）----
static char*   strmBuf    = nullptr;
static size_t  strmLen    = 0;
static size_t  strmPos    = 0;
static bool    strmActive = false;
static unsigned long strmT0 = 0;

// ---- 断线后重新广播 ----
static bool        needReAdv   = false;   // 断了、还没重新广播
static unsigned long discMs     = 0;      // 断开的时刻
static unsigned long advDelayMs = 500;     // 断开后多久广播（产品固件写死 500）
static int         advMaxTry   = 1;       // 最多试几次（产品固件只试 1 次）
static int         advTries    = 0;
static unsigned long lastAdvMs  = 0;

// ---------------------------------------------------------------- 诊断打印

// 把 BLEServer 内部那张 peer 表原样打出来。
// ⚠ 这张表在 **蓝牙协议栈自己的任务** 里被 insert/erase，
//   而我们（loopTask）在读它 —— 严格说这本身就是一处跨任务竞争。
static void dumpPeers(const char* tag) {
    if (pServer == nullptr) { Serial.printf("[%s] no server\n", tag); return; }
    auto peers = pServer->getPeerDevices(false);
    Serial.printf("[%s] peers=%u", tag, (unsigned)peers.size());
    for (auto& kv : peers) {
        Serial.printf("  {conn_id=%u mtu=%u}", (unsigned)kv.first, (unsigned)kv.second.mtu);
    }
    Serial.println();
}

// 关键：这个"查 0 号连接"在 conn_id 不是 0 的时候会发生什么。
static void probePayload(const char* tag) {
    if (pServer == nullptr) { Serial.printf("[%s] no server\n", tag); return; }
    auto peers = pServer->getPeerDevices(false);
    bool hasZero = peers.find(0) != peers.end();
    uint16_t mtu0 = 0;
    if (hasZero) mtu0 = peers.find(0)->second.mtu;
    uint16_t mtuRaw = pServer->getPeerMTU(0);   // ← bt_link.cpp 里实际调用的那一句
    Serial.printf("[%s] hasConnId0=%d mtu0(map)=%u getPeerMTU(0)=%u  btLinkPayloadMax()=%u\n",
                  tag, (int)hasZero, (unsigned)mtu0, (unsigned)mtuRaw,
                  (unsigned)btLinkPayloadMax());
    if (!hasZero) {
        Serial.println("    >>> getPeerMTU(0) 查不到 0 号连接 —— 库里没有 end() 检查，"
                       "这一句读的是 end() 迭代器后面的垃圾内存 <<<");
    }
}

// ---------------------------------------------------------------- 指令

static void dumpCccd(const char* tag);   // 定义在下面（要挂在 BLE2902 回调之后）

static void handleProbeCmd(const String& cmd) {
    if (cmd.startsWith("PROBE:RESET")) {
        // 远程重启 —— 有了它就不用每次都麻烦人按板上的 RESET 键
        // （烧录完芯片会卡在 ROM 下载模式，USB 通道自己出不来）。
        Serial.println("[CMD] PROBE:RESET -> esp_restart()");
        Serial.flush();
        delay(200);
        esp_restart();
        return;
    }
    if (cmd.startsWith("PROBE:DUMP")) {
        dumpPeers("dump");
        probePayload("dump");
        btLinkReplyC("PROBEDUMP", "ok");
        return;
    }
    if (cmd.startsWith("PROBE:ADVDELAY:")) {
        advDelayMs = (unsigned long)cmd.substring(14).toInt();
        Serial.printf("[CMD] PROBE:ADVDELAY -> %lums\n", advDelayMs);
        btLinkReplyC("PROBEADV", "ok");
        return;
    }
    if (cmd.startsWith("PROBE:ADVRETRY:")) {
        advMaxTry = cmd.substring(14).toInt();
        if (advMaxTry < 0) advMaxTry = 0;
        Serial.printf("[CMD] PROBE:ADVRETRY -> %d 次\n", advMaxTry);
        btLinkReplyC("PROBEADV", "ok");
        return;
    }
    if (cmd.startsWith("PROBE:STREAM:")) {
        // 模拟产品固件的"整份配置回拉"：一条 ~20KB 的流式正文，
        // 用来量"到底丢多少片"（固件声明的总长 vs 主机实收的字节数）。
        size_t n = (size_t)cmd.substring(12).toInt();
        if (n == 0) n = 20480;
        char* buf = (char*)malloc(n);
        if (!buf) { btLinkReplyC("PROBERR", "malloc failed"); return; }
        for (size_t i = 0; i < n; i++) buf[i] = (char)('0' + (i % 10));
        Serial.printf("[CMD] PROBE:STREAM 总长=%u 预算=%u\n",
                      (unsigned)n, (unsigned)btLinkPayloadMax());
        if (!btLinkReplyStreamBegin("PROBESTREAM", n)) {
            Serial.println("  开不出流");
            btLinkReplyC("PROBERR", "stream busy");
            free(buf);
            return;
        }
        // ⚠⚠ **不能在指令回调里喂流式槽位**！
        //   槽位只有靠 btLinkPoll() → pumpTx() 才会被抽干，而 btLinkPoll()
        //   此刻正卡在 s_onCmd() 这一层上 —— 于是 btLinkStreamRoom() 永远是 0，
        //   这个 while 死循环，**整机卡死**（心跳都没了）。
        //   产品固件没这个毛病：它的 cfgJsonPump()/logDumpPump() 是在 loop() 里调的。
        //   这里照抄产品固件的做法：回调里只登记，喂数据交给 loop()。
        strmBuf   = buf;
        strmLen   = n;
        strmPos   = 0;
        strmActive = true;
        strmT0    = millis();
        return;
    }
    if (cmd.startsWith("PROBE:MTU")) {
        size_t budget = btLinkPayloadMax();
        String body;
        body.reserve(budget);
        for (size_t i = 0; i < budget; i++) body += (char)('A' + (i % 26));
        Serial.printf("[CMD] PROBE:MTU budget=%u\n", (unsigned)budget);
        btLinkReplyC("PROBEMTU", body.c_str());
        return;
    }
    if (cmd.startsWith("PROBE:BIG:")) {
        size_t n = (size_t)cmd.substring(10).toInt();
        String body;
        body.reserve(n);
        for (size_t i = 0; i < n; i++) body += (char)('a' + (i % 26));
        Serial.printf("[CMD] PROBE:BIG n=%u  当前预算=%u %s\n",
                      (unsigned)n, (unsigned)btLinkPayloadMax(),
                      (n > btLinkPayloadMax() ? "<<< 故意超 MTU" : ""));
        btLinkReplyC("PROBEBIG", body.c_str());
        return;
    }
    if (cmd.startsWith("PROBE:ME:")) {
        int n = cmd.substring(9).toInt();
        if (n < 0) n = 0;
        Serial.printf("[CMD] PROBE:ME 片数=%d\n", n);
        btLinkReplyC("PROBEME", "START");
        for (int i = 0; i < n; i++) {
            char chunk[140];
            int len = snprintf(chunk, sizeof(chunk), "ME_DATA:");
            for (int k = 0; k < 128; k++) chunk[len + k] = (char)('0' + ((i + k) % 10));
            chunk[len + 128] = '\0';
            btLinkReplyC("PROBEMED", chunk);
        }
        btLinkReplyC("PROBEME", "END");
        return;
    }
    if (cmd.startsWith("PROBE:SELFTEST")) {
        // 直接调库里的 notify() 发一个 4 字节的通知，
        // 顺便把 notify() 之前/之后的 CCCD 状态打出来。
        dumpCccd("notify-前");
        Serial.println("[SELFTEST] 直接调 pChar->notify()");
        pChar->setValue((uint8_t*)"TEST", 4);
        pChar->notify();
        dumpCccd("notify-后");
        btLinkReplyC("PROBESELF", "done");
        return;
    }
    if (cmd.startsWith("PROBE:PING")) {
        btLinkReplyC("PROBEPONG", "ok");
        return;
    }
}

static void handleCommand(const String& cmd) {
    s_phase = PH_CMD;
    Serial.printf("[CMD] %s\n", cmd.c_str());
    if (cmd.startsWith("PROBE:")) {
        handleProbeCmd(cmd);
        return;
    }
    btLinkReplyC("PROBEECHO", cmd.c_str());
}

// ---------------------------------------------------------------- CCCD 订阅状态
//
// ⚠ 这是"主机收不到通知"这条线索的关键。
//   库里的 BLECharacteristic::notify() 会先问 CCCD："订阅开了吗？"
//   没开就**静默返回**（只打一条 log_v），一包都不发 —— 现象就是
//   "固件明明回了，网页就是收不到"。
//   所以这里自己挂一个回调，把主机写进 CCCD 的内容原样打出来。
class MyCCCDCallbacks : public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor* d) override {
        BLE2902* c = (BLE2902*)d;
        uint8_t* raw = d->getValue();      // BLEDescriptor::getValue() 返回的是裸指针
        if (raw == nullptr) return;
        uint8_t bits = raw[0];
        // 库里的 BLE2902 自己没实现 onWrite，得我们自己把标志位翻上去
        c->setNotifications((bits & 0x01) != 0);
        c->setIndications((bits & 0x02) != 0);
        Serial.printf("[CCCD] 主机写入 0x%02X -> notify=%d indicate=%d\n",
                      bits, (int)c->getNotifications(), (int)c->getIndications());
    }
};

// 打印当前 CCCD 状态
static void dumpCccd(const char* tag) {
    if (pChar == nullptr) return;
    BLEDescriptor* d = pChar->getDescriptorByUUID((uint16_t)0x2902);
    if (d == nullptr) { Serial.printf("[CCCD %s] 没有 0x2902 描述符！\n", tag); return; }
    BLE2902* c = (BLE2902*)d;
    uint8_t* raw = d->getValue();
    Serial.printf("[CCCD %s] raw=0x%02X%02X notify=%d indicate=%d\n", tag,
                  raw[1], raw[0],
                  (int)c->getNotifications(), (int)c->getIndications());
}

// ---------------------------------------------------------------- BLE 回调

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override {
        s_phase = PH_CONNECT;
        connSeq++;
        s_connSeq = connSeq;
        // ⚠ 和产品固件一模一样：只把 server 指针交出去，**不记 conn_id**。
        btLinkSetConnected(true, s);
        Serial.printf("\n=== CONNECT #%u ===\n", (unsigned)connSeq);
        dumpPeers("connect");
        probePayload("connect");
        dumpCccd("connect-立刻");
        s_phase = PH_IDLE;
    }
    void onDisconnect(BLEServer* s) override {
        s_phase = PH_DISCONNECT;
        btLinkSetConnected(false);
        Serial.printf("\n=== DISCONNECT #%u ===\n", (unsigned)connSeq);
        dumpPeers("disconnect");
        // 记下断开的时刻，等会儿在 loop() 里按 advDelayMs 重新广播
        needReAdv = true;
        discMs    = millis();
        advTries  = 0;
        Serial.printf("[ADV] 断开，计划 %lums 后重新广播，最多试 %d 次\n",
                      advDelayMs, advMaxTry);
        s_phase = PH_IDLE;
    }
    void onMtuChanged(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
        Serial.printf("[MTU EVT] conn_id=%u mtu=%u\n",
                      (unsigned)param->mtu.conn_id, (unsigned)param->mtu.mtu);
        dumpPeers("mtu");
        probePayload("mtu");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string raw = pCharacteristic->getValue();
        if (raw.empty()) return;
        btLinkOnWrite((const uint8_t*)raw.data(), raw.size());
    }
};

// ---------------------------------------------------------------- setup / loop

void setup() {
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) delay(10);
    Serial.println("\n\n==== ble_probe 起跑 ====");
    Serial.printf("reset_reason=%d  BT_PAYLOAD_MIN=%d  BT_PAYLOAD_NEGO_MAX=%d\n",
                  (int)esp_reset_reason(), (int)BT_PAYLOAD_MIN, (int)BT_PAYLOAD_NEGO_MAX);
    Serial.printf("freeHeap=%u\n", (unsigned)ESP.getFreeHeap());
    // ⚠ 上次崩在哪儿：阶段标记存在 RTC 慢存里，panic/复位都丢不掉
    Serial.printf(">>> 上次崩溃阶段: %s (%u)  连接轮数=%u  reset_reason=%d\n",
                  phaseName(s_phase), (unsigned)s_phase, (unsigned)s_connSeq,
                  (int)esp_reset_reason());
    s_phase = PH_BOOT;

    BLEDevice::init("YYQ-MX9.0");
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* pService = pServer->createService(SERVICE_UUID);
    pChar = pService->createCharacteristic(CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY);
    pChar->setCallbacks(new MyCallbacks());
    // CCCD 必须挂自己的回调，否则主机写进来的订阅位没人翻标志，
    // notify() 会以为"没订阅"而静默丢弃所有通知。
    BLE2902* pCccd = new BLE2902();
    pCccd->setCallbacks(new MyCCCDCallbacks());
    pChar->addDescriptor(pCccd);
    pService->start();

    btLinkInit(pChar, handleCommand);
    BLEDevice::startAdvertising();
    Serial.println("[SETUP] 广播已开，等待连接");
}

void loop() {
    btLinkPoll();

    // ---- 流式回拉：在 loop() 里一截一截喂，喂完再 End ----
    if (strmActive) {
        size_t room = btLinkStreamRoom();
        if (room == 0) {
            btLinkDelay(2);
        } else {
            size_t k = strmLen - strmPos;
            if (k > room) k = room;
            btLinkStreamWrite(strmBuf + strmPos, k);
            strmPos += k;
        }
        if (strmPos >= strmLen || (millis() - strmT0) > 20000) {
            btLinkStreamEnd();
            Serial.printf("[CMD] PROBE:STREAM 喂完 %u/%u 字节，耗时 %lums\n",
                          (unsigned)strmPos, (unsigned)strmLen,
                          (unsigned long)(millis() - strmT0));
            free(strmBuf);
            strmBuf = nullptr;
            strmActive = false;
        }
    }

    // ---- 断线后重新广播（产品固件在 loop() 里做同一件事，但只试一次）----
    if (needReAdv) {
        unsigned long since = millis() - discMs;
        if (since >= advDelayMs && (unsigned long)(millis() - lastAdvMs) >= 500) {
            lastAdvMs = millis();
            advTries++;
            s_phase = PH_ADV_CALL;      // ★ 崩在这里就说明是 startAdvertising 干的
            BLEDevice::startAdvertising();
            s_phase = PH_ADV_WAIT;
            Serial.printf("[ADV] 第 %d/%d 次重新广播（断开后 %lums）\n",
                          advTries, advMaxTry, since);
            if (advTries >= advMaxTry) needReAdv = false;
        }
    }

    // ⚠ 无条件心跳：**不看串口有没有人收**，每秒打一行。
    static uint32_t beat = 0;
    static unsigned long lastBeatMs = 0;
    if (millis() - lastBeatMs >= 1000) {
        lastBeatMs = millis();
        beat++;
        Serial.printf("[beat %u] uptime=%lums connected=%d payloadMax=%u heap=%u\n",
                      (unsigned)beat, (unsigned long)millis(),
                      (int)btLinkConnected(), (unsigned)btLinkPayloadMax(),
                      (unsigned)ESP.getFreeHeap());
    }

    if (millis() - lastDumpMs > 2000) {
        lastDumpMs = millis();
        if (btLinkConnected()) {
            dumpPeers("tick");
            probePayload("tick");
        }
    }
    delay(5);
}
