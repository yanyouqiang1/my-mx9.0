/**
 * @file elog.cpp
 *
 * elog.h 的实现。设计取舍写在 elog.h 顶部，这里只讲代码本身的坑。
 */

#include "elog.h"
#include <SPIFFS.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// ---- 环形缓冲 ----
// head 指向"下一条要写的位置"，count 是当前有效条数。
// recs[0] 永远是最老的一条，recs[count-1] 是最新的。
static ELogRec recs[ELOG_CAP];
static int     head   = 0;
static int     count  = 0;

static bool           dirty      = false;   // 有"值得落盘"的变化
static unsigned long  lastFlushMs = 0;
static bool           begun      = false;   // elog_begin 每次开机只做一次

// 2020-01-01 之前的时间戳 = 系统时间还没校准（开机默认 1970）。
// 这种 epoch 不显示，避免用户看到一排 1970 以为日志坏了。
#define EL_WALL_MIN_EPOCH 1577836800UL

static inline bool wallOk(uint32_t epoch) { return epoch >= EL_WALL_MIN_EPOCH; }

bool elog_wallValid(void) {
    for (int i = 0; i < count; i++) {
        if (wallOk(recs[i].epoch)) return true;
    }
    return false;
}

// 是否"值得为这次变化写一次 flash"。
//
// 只在 count 是 2 的幂（2/4/8/16…）或 50 的整数倍时才算。
// 这样一条每 500ms 报一次的错误，一天只落盘十几次而不是几千次，
// 屏幕上的"x3825 次"照样实时更新 —— 数字在 RAM 里累加，不依赖落盘。
static bool countIsNotable(uint16_t c) {
    if (c <= 1) return true;
    if ((c & (c - 1)) == 0) return true;   // 2 的幂
    if (c % 50 == 0) return true;          // 每 50 次也留一个点
    return false;
}

int elog_count(void) { return count; }

const ELogRec* elog_get(int idx) {
    if (idx < 0 || idx >= count) return nullptr;
    // 环形：逻辑第 0 条 = (head - count + ELOG_CAP) % ELOG_CAP
    int start = head - count;
    if (start < 0) start += ELOG_CAP;
    return &recs[(start + idx) % ELOG_CAP];
}

int elog_error_count(void) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (recs[i].level == EL_LVL_ERR) n++;
    }
    return n;
}

// 不去重的"追加一条"。elog_add 和 elog_begin 都要用，抽出来保证
// "新条目永远排在最老条目之后"这条顺序规则只有一份实现。
static void appendRec(const ELogRec& r) {
    recs[head] = r;
    head = (head + 1) % ELOG_CAP;
    if (count < ELOG_CAP) count++;
}

void elog_add(uint8_t level, const char* tag, const char* fmt, ...) {
    if (tag == nullptr) tag = "?";
    if (fmt == nullptr) fmt = "";

    char msg[ELOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // ---- 去重：和最新一条同 tag 同正文就只加计数 ----
    if (count > 0) {
        ELogRec& last = recs[(head - 1 + ELOG_CAP) % ELOG_CAP];
        if (last.level == level
            && strncmp(last.tag, tag, ELOG_TAG_MAX) == 0
            && strncmp(last.msg, msg, ELOG_MSG_MAX) == 0) {
            if (last.count < 0xFFFF) last.count++;
            last.stampMs = (uint32_t)millis();
            if (countIsNotable(last.count)) dirty = true;
            return;
        }
    }

    // ---- 新增一条 ----
    ELogRec r;
    r.stampMs = (uint32_t)millis();
    r.epoch   = (uint32_t)time(nullptr);
    r.count   = 1;
    r.level   = level;
    snprintf(r.tag, sizeof(r.tag), "%s", tag);
    snprintf(r.msg, sizeof(r.msg), "%s", msg);

    appendRec(r);
    dirty = true;
    // 满了就自然覆盖最老的一条：head 回绕，start 也跟着回绕，逻辑上仍然
    // "recs 里是最近 ELOG_CAP 条，按时间从旧到新"。
}

// ---- 落盘 ----

// 把一条写进 File。返回 false 表示写失败。
static bool writeRec(File& f, const ELogRec& r) {
    // tag 里如果有 '|' 会把字段拆坏，所以这里只保留可打印 ASCII，
    // 非 ASCII 一律替换成 '?' —— 标签本来就只该是 ASCII 模块名。
    //
    // ⚠ 判 '\0' 必须**在替换之前**。反过来写的话终止符自己会先被判成
    // "不可打印"而被替换成 '?'，break 之后 tag 就变成了 "BOOT?" / "X?" ——
    // 读回来标签全带一个问号，而且这种错在屏上看着还挺像正常标签。
    char tag[ELOG_TAG_MAX + 1];
    for (int i = 0; i < ELOG_TAG_MAX; i++) {
        if (r.tag[i] == '\0') { tag[i] = '\0'; break; }
        unsigned char c = (unsigned char)r.tag[i];
        tag[i] = (c >= 0x20 && c < 0x7F && c != '|') ? (char)c : '?';
    }
    tag[ELOG_TAG_MAX] = '\0';

    // 正文里的 '|' 会把字段拆坏，换成 '/'（半角斜杠，字库里有）。
    // 不要换成全角竖线 ｜(U+FF5C) —— 它不在 lv_conf.h 圈定的符号范围里，
    // 屏上会画成方框。'|' 和换行回车直接丢弃。
    char msg[ELOG_MSG_MAX + 1];
    size_t m = 0;
    for (int i = 0; r.msg[i] != '\0' && m < sizeof(msg) - 1; i++) {
        unsigned char c = (unsigned char)r.msg[i];
        if (c == '|')      msg[m++] = '/';
        else if (c == '\n' || c == '\r') break;
        else               msg[m++] = (char)c;
    }
    msg[m] = '\0';

    char line[ELOG_MSG_MAX * 2 + 64];
    int n = snprintf(line, sizeof(line), "%lu|%u|%u|%s|%s\n",
                     (unsigned long)r.epoch, (unsigned)r.level,
                     (unsigned)r.count, tag, msg);
    if (n <= 0) return false;
    return f.write((const uint8_t*)line, (size_t)n) == n;
}

void elog_flush(void) {
    // SPIFFS 没挂上就别试了（totalBytes()==0），否则每 60 秒白 open 一次。
    if (SPIFFS.totalBytes() == 0) return;

    // ⚠ FILE_WRITE 会**先截断**再写。所以中途写失败（掉电/坏块）时文件是残缺的，
    // 不是"保持原样"。这没关系：读取端逐行解析、坏行直接跳过，
    // 于是残缺文件仍然能读出前面那部分有效记录，最坏丢最后一条。
    // 换成"写临时文件再改名"并不能变得更安全 —— ESP32 的 FS::rename 要求
    // 目标文件不存在，为了改名先 rm 掉旧的，等于把原子性又还回去了。
    File f = SPIFFS.open(ELOG_FILE, FILE_WRITE);
    if (!f) return;

    f.print("#ELOG1\n");
    for (int i = 0; i < count; i++) {
        if (!writeRec(f, *elog_get(i))) { f.close(); return; }
    }
    f.close();

    dirty = false;
    lastFlushMs = millis();
}

void elog_tick(void) {
    if (!dirty) return;
    if ((unsigned long)(millis() - lastFlushMs) < ELOG_FLUSH_MIN_MS) return;
    elog_flush();
}

// ---- 读取 ----

// 解析一行 "<epoch>|<level>|<count>|<tag>|<msg>"。不合规返回 false。
static bool parseRec(char* line, ELogRec& r) {
    char* f[5];
    int nf = 0;
    f[nf++] = line;
    for (char* p = line; *p != '\0' && nf < 5; p++) {
        if (*p == '|') { *p = '\0'; f[nf++] = p + 1; }
    }
    if (nf != 5) return false;   // 字段数不对 = 这行坏了，跳过

    char* end = nullptr;
    unsigned long ep = strtoul(f[0], &end, 10);
    if (end == f[0]) return false;
    r.epoch = (uint32_t)ep;

    long lv = strtol(f[1], nullptr, 10);
    if (lv < 0 || lv > 2) return false;
    r.level = (uint8_t)lv;

    long ct = strtol(f[2], nullptr, 10);
    if (ct < 1 || ct > 0xFFFF) return false;
    r.count = (uint16_t)ct;

    snprintf(r.tag, sizeof(r.tag), "%s", f[3]);
    snprintf(r.msg, sizeof(r.msg), "%s", f[4]);

    // stampMs 在读回来的条目上没有意义（那是上一次运行的相对时间），
    // 置 0 表示"未知"，显示层会退回用 epoch。
    r.stampMs = 0;
    return true;
}

void elog_begin(void) {
    // 每次开机只做一次。第二次调用会把"内存里现有的条目"当成"挂载前的条目"
    // 再接一遍盘上的内容 —— 那批条目会被重复计入。短路掉比算错强。
    if (begun) return;

    // ⚠ **不能直接清空重读。**
    //
    // setup() 里 USB / I2C / 温湿度这些初始化的报错，是通过 elog_add 记进来的，
    // 而它们的时机在 SPIFFS.begin() **之前**（那时文件系统还没挂上，读不了盘）。
    // 换句话说：调用 elog_begin() 时，内存环形缓冲里已经躺着几条本次启动的
    // 故障记录了 —— 恰恰是"最该留住的那几条"。这里要是 head=0; count=0
    // 把它们抹掉，SPIFFS 挂不上、MCP23017 没起来、USB 协议栈失败
    // 这三类最关键的启动故障就一条都不剩了。
    //
    // 所以先把内存里已有的条目暂存下来，读完盘上的旧日志，再把它们**追加到末尾**
    // —— 它们本来就更新，排在后面才是对的顺序。
    static ELogRec pre[ELOG_PRE_MAX];
    int preCount = count;
    if (preCount > ELOG_PRE_MAX) preCount = ELOG_PRE_MAX;
    for (int i = 0; i < preCount; i++) pre[i] = *elog_get(i);

    head = 0;
    count = 0;
    dirty = false;
    lastFlushMs = millis();     // 刚读完别马上又写回去
    begun = true;

    if (SPIFFS.totalBytes() != 0 && SPIFFS.exists(ELOG_FILE)) {
        File f = SPIFFS.open(ELOG_FILE, FILE_READ);
        if (f) {
            // 最多装 ELOG_CAP 条有效的。文件里超过容量时**只保留最新的那些**，
            // 从头装、装满就停 —— 这样和环形缓冲"新覆盖旧"的语义一致。
            char line[ELOG_MSG_MAX * 2 + 64];
            bool sawHeader = false;
            while (f.available()) {
                size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
                if (n == 0) break;
                line[n] = '\0';
                if (line[0] == '#') { sawHeader = true; continue; }
                if (!sawHeader) continue;      // 没有魔数头，当作不是日志文件

                ELogRec r;
                if (!parseRec(line, r)) continue;   // 坏行跳过，不影响其余
                appendRec(r);
            }
            f.close();
        }
    }

    // 把挂载前记的那几条接回去，并置脏（它们还没落过盘）
    for (int i = 0; i < preCount; i++) appendRec(pre[i]);
    if (preCount > 0) dirty = true;
}

void elog_clear(void) {
    head = 0;
    count = 0;
    dirty = false;
    // 清空要把 begun 一起复位：否则清空之后再调 elog_begin() 会被"只做一次"
    // 的短路挡掉，日志就再也读不回盘上的内容了。正常流程里清空发生在
    // 运行期（菜单里按 DEL），不会再有 elog_begin，但复位这个标志让
    // "清空 = 回到出厂状态"这个语义是完整的。
    begun = false;
    if (SPIFFS.totalBytes() != 0) SPIFFS.remove(ELOG_FILE);
    lastFlushMs = millis();
}
