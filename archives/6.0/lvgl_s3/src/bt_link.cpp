/**
 * bt_link.cpp —— 蓝牙链路统一收发层（固件侧）
 *
 * 协议和用法见 bt_link.h。这里只讲三件实现上的事：
 *   1) 发包是"入队 + 增量泵"，不是同步推完（原因见 bt_link.h 顶部的看门狗段）
 *   2) 行组装只发生在 BLE 写回调里，绝不搬到 loop()（跨任务撕裂）
 *   3) 喂狗点分别落在收字节 / 发片 / 派发指令三处，各自保护的任务不一样
 */

#include "bt_link.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ============================================================ 静态状态
//
// ⚠ 全部落在 .bss，不吃栈。loopTask 的栈只有 8KB，而 JPEGDEC 一个对象就
//   12.6KB（见 lvgl_s3.ino 里 finishLogoUpload 的注释）——这个模块在
//   蓝牙收发路径上，绝不能成为下一个踩栈顶的。
static BLECharacteristic* s_ch       = nullptr;
static void (*s_onCmd)(const String&) = nullptr;
static bool     s_connected    = false;
static bool     s_inited       = false;

static unsigned long s_criticalUntil = 0;   // 关键响应的独占窗口
static unsigned long s_streamLastMs  = 0;   // 日志流节流

static BtBinaryProbe s_probe = nullptr;
static BtBinarySink  s_sink  = nullptr;

// 正在派发的那条请求自带的请求号（主机在指令末尾加的 `#123`）。
// 只在 btLinkPoll → onCommand 这一个调用栈里有效：派发是单线程的，
// 所以不需要锁，但它**绝不能**被别的路径当成"当前 key"用。
static char s_curReq[8] = { 0 };
static uint32_t s_autoKey = 0;   // 主机没带请求号时自增，保证 key 也能配对

static void autoKey(char* out, size_t cap) {
    snprintf(out, cap, "%lu", (unsigned long)(++s_autoKey % BT_REQ_ID_MAX) + 1);
}

// ---- 待发队列（只放"响应"这种小消息）----
struct BtMsg {
    bool     used;
    uint16_t sent;              // 已发出的正文字节
    uint16_t len;               // 正文字节数
    char     verb[BT_VERB_MAX];
    char     key[BT_KEY_MAX];
    // +1 那个字节是留给 '\0' 的：btLinkSend 允许正文正好占满 BT_TX_BODY_MAX，
    // 写 body[len] = 0 时不加这一格就会踩到**下一个结构体**的头一个字段。
    char     body[BT_TX_BODY_MAX + 1];
};
static BtMsg s_txq[BT_TXQ_LEN];

// ---- 流式单槽（大块数据，正文不进队列）----
struct BtStream {
    bool     active;
    bool     closed;            // 调用方已 btLinkStreamEnd()，剩下的发完即可收尾
    uint16_t fill;              // buf 里已填字节
    uint16_t pos;               // buf 里已发出字节
    char     verb[BT_VERB_MAX];
    char     key[BT_KEY_MAX];
    char     buf[BT_TX_BODY_MAX];
};
static BtStream s_st;

// ---- 接收 ----
static QueueHandle_t s_queue = nullptr;
// 行缓冲只在 NimBLE/bluedroid 的写回调任务里被读写（见文件顶部第 2 条），
// 所以不需要加锁，也**不允许**在 loop() 里碰它。
static char          s_line[BT_RX_BUF_SIZE];
static uint16_t      s_lineLen    = 0;
static unsigned long s_lineLastMs = 0;

// ============================================================ 小工具

void btLinkKeepAlive() {
    // 保护的是**调用它的那个任务**。setup() 里 esp_task_wdt_add(NULL) 订阅的是
    // loopTask，所以只有从 loopTask 调进来才真的有效；蓝牙协议栈任务没订阅，
    // 在回调里调是空操作（返回 ESP_ERR_NOT_FOUND，无副作用）—— 保留这一句是为了
    // 让"收包路径上也有喂狗点"这件事在代码上成立，将来谁把协议栈任务也
    // esp_task_wdt_add() 了，不用再回来补一遍。
    //
    // 另一个订阅者是 idle 任务（CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y，
    // sdkconfig 默认开），它只有自己能给自己的看门狗喂食 → 下面所有等待一律
    // 走 delay()（会让出 CPU、idle 跑得起来），本模块里没有一处忙等。
    esp_task_wdt_reset();
}

void btLinkDelay(unsigned long ms) {
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        btLinkKeepAlive();
        delay(1);
    }
}

// VERB:KEY: 的长度
static inline size_t headLen(const char* verb, const char* key) {
    return strlen(verb) + 1 + strlen(key) + 1;
}

// 这一组 VERB/KEY 一次能带多少正文（不含 '\n'）
static inline size_t chunkRoom(const char* verb, const char* key) {
    size_t h = headLen(verb, key);
    if (h + 1 >= BT_PAYLOAD_MAX) return 0;   // 头 + '\n' 都塞不下，参数非法
    return BT_PAYLOAD_MAX - h - 1;
}

// VERB/KEY 是协议的寻址部分，必须非空、不含 ':' 和 '\n'，
// 长度还得在预算内 —— 不校验的话一帧错位，整条流后面全串台。
static bool headOk(const char* verb, const char* key) {
    if (verb == nullptr || key == nullptr) return false;
    size_t vl = strlen(verb), kl = strlen(key);
    if (vl == 0 || kl == 0) return false;
    if (vl >= BT_VERB_MAX || kl >= BT_KEY_MAX) return false;
    if (strchr(verb, ':') || strchr(verb, '\n') || strchr(verb, '\r')) return false;
    if (strchr(key,  ':') || strchr(key,  '\n') || strchr(key,  '\r')) return false;
    // ⚠ 收尾标记那一包也必须装得下：`VERB:KEY:END<终止符>` = 头 + 4 字节。
    //   这一条不是洁癖：`MACRODUMP` + key `p3_M12` 的头就有 17 字节，加上
    //   "END" 和终止符正好 21 > 20 —— 而超预算的通知不是被截断，是**整条被
    //   协议栈丢掉**，于是这条消息永远收不到正文也等不到收尾，网页只看到超时，
    //   而且日志里什么异常都没有。（正文分片本身装得下，所以只查正文会漏掉它。）
    if (headLen(verb, key) + BT_END_MARK_LEN > BT_PAYLOAD_MAX) return false;
    return true;
}

static void copyField(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;     // headOk() 已经拦过，这里只是兜底
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// ============================================================ 初始化

void btLinkInit(BLECharacteristic* ch, void (*onCmd)(const String&)) {
    s_ch    = ch;
    s_onCmd = onCmd;
    if (!s_inited) {
        s_queue = xQueueCreate(BT_CMD_QUEUE_LEN, BT_RX_BUF_SIZE);
        s_inited = true;
    }
}

void btLinkSetConnected(bool connected) {
    s_connected = connected;
    if (!connected) {
        // 断开后正在排队的响应发出去也没人收，白占队列，还会把下一条挤掉。
        for (int i = 0; i < BT_TXQ_LEN; i++) s_txq[i].used = false;
        memset(&s_st, 0, sizeof(s_st));
    }
}

bool btLinkConnected() { return s_connected; }

bool btLinkReady()    { return s_inited && s_ch != nullptr && s_connected; }

QueueHandle_t btLinkCmdQueue() { return s_queue; }

void btLinkSetBinaryIo(BtBinaryProbe probe, BtBinarySink sink) {
    s_probe = probe;
    s_sink  = sink;
}

// ============================================================ 发送：底层

// 真正往协议栈塞一包。setValue 会把内容拷进 std::string，所以这里复用静态
// 缓冲是安全的（不必等上一包发完）。
static void notifyPacket(char* pkt, size_t n) {
    if (s_ch == nullptr || n == 0) return;
    s_ch->setValue((uint8_t*)pkt, n);   // 只拷进 std::string，函数返回后随便覆写
    s_ch->notify();
}

// 拼一片并发出去。正文里带 '\n' 时优先在行尾断开，保证主机那边
// "按 \n 切行"看到的仍是一行一行（LOG 流和 LOGDUMP 靠这个可读）。
// 返回这一片实际带走的正文字节数；0 = 这一片发不了（预算不足）。
static uint16_t emitChunk(const char* verb, const char* key,
                          const char* body, uint16_t len, uint16_t off) {
    static char pkt[BT_PAYLOAD_MAX + 8];
    size_t room = chunkRoom(verb, key);
    if (room == 0) return 0;

    uint16_t n = (uint16_t)(len - off);
    if (n > room) n = (uint16_t)room;
    if (n == 0) return 0;
    // 行对齐：这一段里能找到 '\n' 就断在行尾，'\n' 本身带上（主机那边
    // "按 \n 切行"看到的才是一行一行）。**从 i=1 开始找**是必须的：
    // 上一片如果刚好断在某个 '\n' 前面，这一片的首字节就又是 '\n'，从 0
    // 开始找会切出 n=0 的空片，而空片在主机那边和"一条刚好装得下的空
    // 消息"长得一模一样，会把 waiter 提前兑现成空串。
    for (uint16_t i = 1; i < n; i++) {
        if (body[off + i] == '\n') { n = (uint16_t)(i + 1); break; }
    }

    size_t h = headLen(verb, key);
    memcpy(pkt, verb, strlen(verb));
    pkt[strlen(verb)] = ':';
    memcpy(pkt + strlen(verb) + 1, key, strlen(key));
    pkt[h - 1] = ':';
    memcpy(pkt + h, body + off, n);
    pkt[h + n] = BT_PKT_END;
    notifyPacket(pkt, h + n + 1);
    return n;
}

static void emitEnd(const char* verb, const char* key) {
    static char pkt[BT_PAYLOAD_MAX + 8];
    int m = snprintf(pkt, sizeof(pkt), "%s:%s:END" BT_PKT_END_STR, verb, key);
    if (m > 0 && m < (int)sizeof(pkt)) notifyPacket(pkt, (size_t)m);
}

// ============================================================ 发送：响应

bool btLinkSend(const char* verb, const char* key, const char* body, size_t len) {
    if (!btLinkReady() || body == nullptr) return false;
    if (!headOk(verb, key)) return false;
    if (len > BT_TX_BODY_MAX) return false;
    // 头 + '\n' 装不下 = 这一组 VERB/KEY 一片正文都带不了，入队也是白占。
    if (chunkRoom(verb, key) == 0) return false;

    for (int i = 0; i < BT_TXQ_LEN; i++) {
        if (s_txq[i].used) continue;
        BtMsg& m = s_txq[i];
        m.used = true;
        m.sent = 0;
        m.len  = (uint16_t)len;
        copyField(m.verb, sizeof(m.verb), verb);
        copyField(m.key,  sizeof(m.key),  key);
        memcpy(m.body, body, len);
        m.body[len] = '\0';
        // 从这一刻起给日志流开独占窗口：响应已经排队了，通道要留给它。
        s_criticalUntil = millis() + BT_CRITICAL_HOLD_MS;
        return true;
    }
    return false;   // 队列满：调用方应该等一轮 btLinkPoll() 再试
}

bool btLinkSendC(const char* verb, const char* key, const char* body) {
    if (body == nullptr) return false;
    return btLinkSend(verb, key, body, strlen(body));
}

bool btLinkSendf(const char* verb, const char* key, const char* fmt, ...) {
    char body[BT_TX_BODY_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return false;
    if (n >= (int)sizeof(body) - 1) return false;   // 别静默截断，宁可不发
    body[n] = '\0';
    return btLinkSend(verb, key, body, (size_t)n);
}

// 处理请求的分支里回话用这两个：key 取当前请求号（见 bt_link.h 的说明）。
bool btLinkReplyC(const char* verb, const char* body) {
    if (body == nullptr) return false;
    char key[BT_KEY_MAX];
    if (s_curReq[0]) copyField(key, sizeof(key), s_curReq);
    else             autoKey(key, sizeof(key));
    return btLinkSendC(verb, key, body);
}

bool btLinkReplyf(const char* verb, const char* fmt, ...) {
    char body[BT_TX_BODY_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(body) - 1) return false;   // 别静默截断
    body[n] = '\0';
    return btLinkReplyC(verb, body);
}

// ============================================================ 发送：日志流

bool btLinkStream(const char* verb, const char* text) {
    if (!btLinkReady() || text == nullptr || text[0] == '\0') return false;
    if (!headOk(verb, "0")) return false;
    unsigned long now = millis();
    if (now < s_criticalUntil) return false;                       // 让路给关键响应
    if ((unsigned long)(now - s_streamLastMs) < BT_STREAM_THROTTLE_MS) return false;
    s_streamLastMs = now;

    char key[BT_KEY_MAX];
    autoKey(key, sizeof(key));
    return btLinkSend(verb, key, text, strlen(text));
}

void btLinkStreamResetThrottle() { s_streamLastMs = 0; }

// ============================================================ 发送：流式大块

bool btLinkStreamBegin(const char* verb, const char* key, size_t totalLen) {
    (void)totalLen;   // 只是诊断信息，协议不依赖它
    if (!btLinkReady() || s_st.active) return false;
    if (!headOk(verb, key)) return false;
    memset(&s_st, 0, sizeof(s_st));
    s_st.active = true;
    copyField(s_st.verb, sizeof(s_st.verb), verb);
    copyField(s_st.key,  sizeof(s_st.key),  key);
    s_criticalUntil = millis() + BT_CRITICAL_HOLD_MS;
    return true;
}

// 正文再长也只往单槽里塞，满了返回 false 让调用方下一轮再来 —— 于是
// 8KB 的回拉变成"每轮 loop 喂一段、发一段"，而不是一次堆 400 个包。
static bool streamFeed(const void* data, size_t len) {
    if (!s_st.active) return false;
    if (s_st.fill >= BT_TX_BODY_MAX) return false;
    size_t room = BT_TX_BODY_MAX - s_st.fill;
    if (len > room) len = room;
    memcpy(s_st.buf + s_st.fill, data, len);
    s_st.fill += (uint16_t)len;
    return len > 0;
}

bool btLinkStreamWrite(const char* data, size_t len) {
    return streamFeed(data, len);
}

bool btLinkStreamWrite(const uint8_t* data, size_t len) {
    return streamFeed(data, len);
}

void btLinkStreamEnd() {
    if (!s_st.active) return;
    s_st.closed = true;
}

bool btLinkStreamBusy() { return s_st.active; }
bool btLinkStreamFull() { return s_st.active && s_st.fill >= BT_TX_BODY_MAX; }

// 单槽里还能再塞多少字节。**调用方必须用它给自己的切片封顶**：
// btLinkStreamWrite() 装不下时会截断（返回 true），而调用方通常会按"这一整段
// 都写进去了"来推进游标 —— 于是被截掉的尾巴永远没人再喂，静默丢数据。
// 正确写法：
//     size_t n = min(slice, btLinkStreamRoom());
//     btLinkStreamWrite(p, n);
//     pos += n;                      // 只推进真正写进去的 n
size_t btLinkStreamRoom() {
    if (!s_st.active) return 0;
    return (size_t)(BT_TX_BODY_MAX - s_st.fill);
}

// ============================================================ 发包泵

// 发一片。响应队列优先于流：一条 20 字节的响应不该排在 8KB 日志后面。
static bool pumpOne() {
    for (int i = 0; i < BT_TXQ_LEN; i++) {
        BtMsg& m = s_txq[i];
        if (!m.used) continue;
        // 正文为空的响应：**只发一条收尾标记**，没有正文片。
        // "读到了，而且键盘上确实一条都没有" 是一个**有意义的**结果（REMAPREAD
        // 读一个空方案、MACRODUMP 读一个没设的宏），不能因为正文 0 字节就
        // 什么都不发 —— 网页那边等的就是收尾标记，沉默 = 超时 = 误报"读不到"。
        // ⚠ 这个分支必须在 emitChunk 之前：正文为 0 时 emitChunk 返回 0，
        //   会被后面的"预算不足就丢弃"当成失败，连收尾标记一起丢掉。
        if (m.len == 0) {
            emitEnd(m.verb, m.key);
            m.used = false;
            return true;
        }
        uint16_t n = emitChunk(m.verb, m.key, m.body, m.len, m.sent);
        if (n == 0) { m.used = false; return false; }   // 预算不足，丢弃而不是死循环
        m.sent = (uint16_t)(m.sent + n);
        if (m.sent >= m.len) {
            emitEnd(m.verb, m.key);
            m.used = false;
        }
        return true;
    }

    if (s_st.active && s_st.pos < s_st.fill) {
        uint16_t n = emitChunk(s_st.verb, s_st.key, s_st.buf, s_st.fill, s_st.pos);
        if (n == 0) { s_st.pos = s_st.fill; return false; }
        s_st.pos = (uint16_t)(s_st.pos + n);
        return true;
    }
    // 缓冲喂空了但还没 End()：不能收尾，等调用方继续喂。
    if (s_st.active && s_st.closed && s_st.pos >= s_st.fill) {
        emitEnd(s_st.verb, s_st.key);
        s_st.active = false;
        return true;   // END 本身也算一包，让调用方看到进度
    }
    return false;
}

// 每轮 loop 最多花 BT_POLL_BUDGET_MS 毫秒在发包上。
// 这一条就是"蓝牙发送时不再重启"的兜底：主循环永远不会被人为地按住
// 十来秒把任务看门狗喂爆；大回拉自动摊到后面几十轮 loop 上，
// 期间键盘扫描、灯光、屏幕全都照常。
static void pumpTx() {
    if (s_ch == nullptr) return;
    unsigned long t0 = millis();
    do {
        if (!pumpOne()) return;
        btLinkDelay(BT_CHUNK_GAP_MS);
    } while ((unsigned long)(millis() - t0) < BT_POLL_BUDGET_MS);
}

// ============================================================ 接收

// 把攒到的一行塞进队列。队列满时丢最老的一条，保证新指令一定能进去（宁旧不新）。
static void lineSubmit() {
    if (s_lineLen == 0 || s_queue == nullptr) { s_lineLen = 0; return; }
    s_line[s_lineLen] = '\0';
    s_lineLen = 0;
    if (xQueueSend(s_queue, s_line, 0) != pdTRUE) {
        static char drop[BT_RX_BUF_SIZE];
        if (xQueueReceive(s_queue, drop, 0) == pdTRUE) {
            xQueueSend(s_queue, s_line, 0);
        }
    }
}

void btLinkOnWrite(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return;
    btLinkKeepAlive();   // 收包路径上的喂狗点（见 bt_link.h 里的说明）
    if (s_queue == nullptr) return;

    // ① 二进制旁路：壁纸 JPEG 这种裸字节里有 0x00，按行拼会碎。
    //    判定和落地都交给业务侧注册的回调，本模块只负责"在文本通道之前先问一句"。
    if (s_probe != nullptr && s_probe(data, len)) {
        if (s_sink != nullptr) {
            s_sink(data, len);
            btLinkKeepAlive();
        }
        return;
    }

    // ② 文本通道：先把字节攒成一行，遇到 \n 提交。
    //    一个包 = 一条命令的老做法会把长指令（宏、全局动作、整张映射表）
    //    切成两半分别入队，handleCommand 拿到的都是残缺指令，什么也不匹配 ——
    //    表现就是"M1 相关的下发一律失败 / 不生效"，而短的（ALERT:RED）反而好使。
    unsigned long now = millis();
    if (s_lineLen > 0 && (unsigned long)(now - s_lineLastMs) > BT_LINE_IDLE_MS) {
        lineSubmit();   // 不带 \n 的老客户端：靠包间静默兜底提交
    }
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\0') continue;                       // 补长度的 0，不是内容
        if (c == '\n' || c == '\r') { lineSubmit(); continue; }
        // 超长行：**丢尾部**，不要 lineSubmit()。
        // 提交的话，剩下的字节会变成一条全新的命令被解析 —— 那正是
        // "一条长命令被劈成两半、半截也能匹配上"的毒：截断点只要落在冒号
        // 之后，残行照样是个合法命令，会静默改坏配置。丢尾巴最坏只是这条
        // 超长命令不完整、匹配不上，配置不会错。
        if (s_lineLen >= BT_RX_BUF_SIZE - 1) continue;
        s_line[s_lineLen++] = c;
    }
    s_lineLastMs = now;
}

// ============================================================ poll（loop 里调）

void btLinkPoll() {
    btLinkKeepAlive();

    // 发包泵放在派发之前：先把排队的响应推出去，网页那边才等得到东西。
    pumpTx();

    if (s_queue == nullptr) return;

    // 一轮最多处理 4 条：蓝牙灌进来的指令量很小，卡住主循环反而会让屏幕和
    // 键盘一起变卡；而 4 条的量足以在两轮 loop 内把 8 深的队列清空。
    static char buf[BT_RX_BUF_SIZE];
    for (int i = 0; i < 4; i++) {
        if (xQueueReceive(s_queue, buf, 0) != pdTRUE) return;
        buf[BT_RX_BUF_SIZE - 1] = '\0';

        // 摘掉主机在末尾加的请求号 `#123`。
        // 必须**在**进 handleCommand 之前摘：MACRODUMP:p3_M12#7 要是原样传下去，
        // handleCommand 会拿 "p3_M12#7" 去 NVS 里找宏，key 根本不存在。
        // 指令本身以 '#<数字>' 结尾的情况本项目没有，所以无条件摘是安全的。
        s_curReq[0] = '\0';
        size_t bl = strlen(buf);
        // 从尾部倒着找 '#'，且 '#' 与末尾之间必须全是数字，否则不是请求号
        // （比如宏正文里本来就带 '#' 的情况，不该被误当成请求号）。
        for (size_t k = bl; k > 0; k--) {
            if (buf[k - 1] == '#') {
                bool allDigits = (k < bl);
                for (size_t j = k; j < bl; j++) {
                    if (buf[j] < '0' || buf[j] > '9') { allDigits = false; break; }
                }
                if (allDigits && bl - k >= 1 && bl - k < sizeof(s_curReq)) {
                    memcpy(s_curReq, buf + k, bl - k);
                    s_curReq[bl - k] = '\0';
                    buf[k - 1] = '\0';
                }
                break;
            }
            if (!(buf[k - 1] >= '0' && buf[k - 1] <= '9')) break;
        }

        if (s_onCmd != nullptr) s_onCmd(String(buf));
        s_curReq[0] = '\0';
        // 每条指令之间喂一次狗：handleCommand 里会做 NVS 读写、SPIFFS 落盘、
        // 甚至整屏重建，这些都可能耗到秒级。喂点放在"指令之间"而不是
        // "指令里面"，是因为里面是业务代码，管不了。
        btLinkKeepAlive();
    }
}
