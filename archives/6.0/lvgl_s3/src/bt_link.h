/**
 * bt_link.h —— 蓝牙链路统一收发层（固件侧）
 *
 * ============================ 这个模块解决什么 ============================
 *
 * 在它出现之前，"从键盘发一句话给电脑"这件事在 lvgl_s3.ino 里有 5 份各自
 * 实现的版本（REMAPDUMP / MACRODUMP / GKEYDUMP / LOGDUMP / LOG 流），每份
 * 各自决定分片大小、片间间隔、要不要收尾标记，抄错一处就丢一次包。丢包的表现
 * 很统一：网页那边只能"等超时"，然后误以为键盘里是 0 条配置。
 *
 * 根因是一条 BLE 通知能带多少字节是**协商出来的**，协商失败时（手机端浏览器
 * 很常见）上限就是默认的 23-3 = **20 字节**。超出的部分不是被截断，而是**整条
 * 通知被协议栈丢掉**。这解释了历史上那个一直归因错的不对称：
 *     1 条规则 → "REMAPDUMP:0:128,131\n"        = 20 字节 → 刚好塞得下 ✓
 *     2 条规则 → "REMAPDUMP:0:128,131;131,128\n" = 28 字节 → 整条丢弃 ✗
 * 也就是"单条 win→ctrl 读得回来、Ctrl/Win 交换读不回来"。
 *
 * 所以分片不是可选项，是**必须**。本模块把它做成"调用方看不见"的内部细节：
 * 业务代码只调 btLinkSend(verb, key, body)，分片、间隔、收尾标记全在这里。
 *
 * ============================ 线上协议（两边共用） ============================
 *
 * 一条"消息" = 若干片 + 一条收尾标记。VERB/KEY 都非空且不含 ':' 和 '\n'。
 *   · 正文片   `VERB:KEY:<正文片段>\n`   0~N 条
 *   · 收尾标记 `VERB:KEY:END\n`         **每条消息都有**，哪怕正文只有 0 字节
 * 主机侧的重组规则（网页端 BtLink 实现了同一份）：
 *   收到某 (VERB,KEY) 的一片 → 按 "VERB:KEY" 找缓冲，找到就追加，没有就新建
 *   收到 END → 拼完整条，交给 waiter / 处理器
 *
 * 为什么收尾标记**无条件**发，而不是"只有分片时才发"：
 *   一片正文和"整条消息刚好装得下"在电路上长得一模一样，接收端没有任何办法
 *   区分"这是完整的一条"和"这是第一条、后面还有"。曾经想靠"一段时间没再来
 *   片就当它完了"来兜底，但本模块的发包是**摊到多次 loop 上**的（见下），
 *   两片之间天然可能隔几十毫秒，空闲判据会在传输中途误判，把一条 8KB 的
 *   消息劈成两条半截的 → 日志面板里冒出两段残缺内容。
 *   多发一条 END 的代价是每条响应多一个包，10Hz 的日志流也就 20 包/秒，
 *   换来接收端彻底没有歧义，这笔买卖非常划算。
 *
 * 反过来说，"正文为 0 字节"必须表达成"只有一条 END"：REMAPREAD 读一个空方案、
 * MACRODUMP 读一个没设的宏，"读到了、确实一条都没有"是一个**有意义的**结果。
 * 沉默会被网页当成"读不到"。
 *
 * ============================ 为什么发包不能一口气同步推完 ============================
 *
 * 分片之后，"一次回 240 字节"就是十几包。如果每包之间 delay 一下、在
 * handleCommand 里一口气推完，那么一条 8KB 的日志回拉 = 400+ 包 × ~20ms ≈ 8 秒。
 * loop() 是在任务看门狗上的（setup() 里 esp_task_wdt_add(NULL)，超时 10 秒、
 * trigger_panic = true），**推着推着就把自己看门狗喂爆重启了** —— 这正是
 * "蓝牙发送的时候会导致软件重启"的机制。
 *
 * 所以本模块的发包是**增量泵**：btLinkSend() 只是入队，真正 notify 发生在
 * btLinkPoll()（loop() 里），每轮最多花 BT_POLL_BUDGET_MS 毫秒、片间喂一次
 * 狗。结果是：键盘扫描和屏幕照常刷新，看门狗永远饿不着，大回拉自动变成
 * "慢慢滴出去"而不是"卡死 8 秒"。
 *
 * ============================ 接收侧为什么必须喂狗 ============================
 *
 * BLE 写回调跑在蓝牙协议栈自己的任务里，那个任务**没有**订阅任务看门狗，
 * 在里面调 esp_task_wdt_reset() 是空操作。真正有风险的是主任务：收到数据
 * 之后解码、写 SPIFFS、重画界面都发生在 loop() 里。所以喂狗点必须放在
 * ① 每次收到字节 ② 每发出一片 ③ 每派发一条指令 ④ 任何等待之前。
 * ①②③ 由本模块负责，④ 之后的长活（解码/落盘）由业务代码自己分段喂
 * （参考 finishLogoUpload / kbSafeDelay）。
 */

#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------- 预算常量
//
// ⚠ 千万别用 BLEDevice::getMTU() 算预算：它返回的是 **m_localMTU**，也就是
//   BLEDevice::setMTU(517) 自己设进去的那个本地**请求**值，不是协商结果。
//   拿它当上限会算出 514，而实际协商是 min(517, 主机支持) —— 手机/浏览器
//   常常只给 247 甚至 185，于是每一条通知都会超。超了会怎样？看
//   BLECharacteristic::notify：它**只打一条 log_w 就把数据截断**，不报错、
//   不抛异常 —— 收件方拿到的是**残缺的正文**，而这正是丢片那一类
//   "静默数据损坏"。所以必须取 `pServer->getPeerMTU()`（真正协商后的值），
//   见 btLinkPayloadMax()。
//
// 两个值：
//   BT_PAYLOAD_MIN      没连上 / 拿不到协商结果时用。必须是最保守的那个 ——
//                      这条路上错一步就是静默截断。
//   BT_PAYLOAD_NEGO_MAX 协商成功后允许的上限。留了余量（不用满 244）：
//                      一包几百字节会让 BLE 协议栈在单个 connection event 里
//                      长时间占用，个别主机上反而不稳；244 已经能把
//                      "每包 9 字节"提速 27 倍，把 8KB 日志回拉从 6.7 秒
//                      压到 0.3 秒以内，够用了。
#define BT_PAYLOAD_MIN      20      // 保守值：没协商成功时按这个走
#define BT_PAYLOAD_NEGO_MAX 244     // 协商成功后的上限（不是请求值，是实际值）

// 包终止符是 **'\r'**，不是 '\n'。这不是随手选的：
//
// 一条通知 = `VERB:KEY:<正文片段><终止符>`，而终止符本身必须是正文里
// **不可能出现**的字节，否则正文里的它会被误当成包尾，把一条消息劈成两半。
// '\n' 恰恰是正文里最常见的字节 —— LOG 回拉整个就是一堆换行分隔的日志行。
// 用 '\n' 收尾时，网页按 '\n' 切包，一段带换行的正文会被切成好几个"包"，
// 换行符自己被当成分隔符吃掉，日志面板里 8KB 历史日志会挤成**一大行**。
//
// 所以：\n 留给正文当数据，\r 当包尾。主机侧用 split("\r") 切包，
// 不要 trim 每一段（trim 会把正文首尾的换行也吃掉）。
//
// 反方向（主机 → 键盘）仍然用 '\n' 收尾：那是"一条请求一行"的行协议，
// 由 btLinkOnWrite 按行攒，和这里不是一回事。
#define BT_PKT_END          '\r'
#define BT_PKT_END_STR      "\r"

// 收尾标记 "END" + 终止符 = 4 字节。VERB/KEY 的长度上限由它反推：
// 一条消息的收尾那一包必须装得下当前预算（btLinkPayloadMax()），否则整条消息永远收不到
// （超预算的通知不是被截断，是**整条被协议栈丢掉**，日志里什么异常都没有）。
//
// 这条约束比看上去紧：最长的 verb（MACRODUMP，9 字节）只给 KEY 剩 5 字节，
// 而 "p3_M12" 正好 6 字节 —— 差一个字符就发不出去。所以**请求/响应的 key
// 一律用 btLinkReply 的自动请求号**（1~9999，4 字节以内），不要自己拿
// 业务字符串当 key。btLinkSend(verb, key, …) 只留给推送类流量。
#define BT_END_MARK_LEN     4
#define BT_REQ_ID_MAX       9999

// 片间必须留缝：BLE 通知不排队，贴太近后片会被合并/丢弃。
// 一条 notify 本身就要 7~15ms（写 attribute + 交给协议栈），这个 6ms 只是额外保险。
#define BT_CHUNK_GAP_MS     6

// 一次 btLinkPoll() 最多花多久在发包上。超时立刻把 CPU 还给键盘扫描和 LVGL。
// 大回拉（8KB 日志）会自动分摊到很多轮 loop 上，而不是卡住主循环。
#define BT_POLL_BUDGET_MS   25

// 发完一条"关键响应"后开一小段独占窗口，期间日志流不许抢通道。
// 日志晚 250ms 出现无所谓，关键响应丢了网页就再也对不上了。
#define BT_CRITICAL_HOLD_MS 250

// 日志流节流：一次 write attribute + notify 约 7~15ms，10Hz 才留得出空档，
// 丢一些日志无所谓，关键时刻（按键路径）才挤得进来。
#define BT_STREAM_THROTTLE_MS 100

// 接收侧：一行最多攒多少字节（含 '\0'），也是指令队列的单条上限。
#define BT_RX_BUF_SIZE      256
#define BT_CMD_QUEUE_LEN    8

// 包间静默多久算"这条发完了"的兜底判据。
// ⚠ 这个值从 15 提到 80，是修"下发 2 条规则只存进 1 条"的根因：Web Bluetooth 的
//   writeValue 会按协商出来的 ATT MTU 拆包，MTU 协商失败时（落到 20~185 字节）
//   一条 29 字节的 `REMAP:0:clear:224,227;227,224\n` 会被切成 20 + 9。
//   15ms 的空闲判定会在**两个包之间**误判成"这条发完了"。80ms 远大于任何真实
//   的包间隔，又短到不会让不带 \n 的老客户端觉得卡。真正的终止符始终是 '\n'。
#define BT_LINE_IDLE_MS     80

// 发送队列深度 / 单条消息正文上限。
// 只有"响应"走队列（都是 NVS 里读出来的一行，最长几百字节）；
// 大块数据（日志缓冲、文件内容）走流式接口，正文不进队列、边读边发。
#define BT_TXQ_LEN          4
#define BT_TX_BODY_MAX      240

#define BT_VERB_MAX         12      // "LOGDUMP" / "REMAPDUMP" / "MACRODUMP" 都在预算内
#define BT_KEY_MAX          24      // 方案号 / 宏名 "p0_M1" / 自动序号

// ---------------------------------------------------------------- 初始化
//
// ch      : 已经 create 好、setCallbacks 好、startAdvertising 之前的那个特征
// onCmd   : 指令派发回调。**在 loop() 任务里被调用**，所以里面可以碰 LVGL、
//           NVS、SPI —— 和键盘触发的路径是同一个任务，绝不会跨任务抢 LVGL 堆。
void btLinkInit(BLECharacteristic* ch, void (*onCmd)(const String&));

// 连接状态。由 MyServerCallbacks 的 onConnect / onDisconnect 驱动。
// 断开时正在排队的消息会被清掉（发出去也没人收，白占队列）。
// srv 是协商后 MTU 的唯一来源（BLECharacteristic::getService() 是私有的，
// 拿不到 server），由 onConnect 回调传进来。不传就永远退回 BT_PAYLOAD_MIN。
void btLinkSetConnected(bool connected, BLEServer* srv = nullptr);
bool btLinkConnected();

// 链路是否可用（特征已绑 + 有人连着）。发包前先问这个。
bool btLinkReady();

// 一条通知现在实际能带多少字节（含包终止符）。
//
// 读的是**真正协商后的** MTU（pServer->getPeerMTU），不是 setMTU 请求的值。
// 没连上 / 没协商完 / 拿不到 → 退回 BT_PAYLOAD_MIN(20)，绝不乐观。
// 详见 bt_link.cpp 里 btLinkPayloadMax() 的注释（那里写清了为什么不能用
// BLEDevice::getMTU，以及超了会静默截断）。
//
// 只是**读一下**，可以随便调；但每片分片都会调，所以别在里面加阻塞操作。
size_t btLinkPayloadMax();

// ---------------------------------------------------------------- 二进制旁路
//
// BLE 的写回调收到的东西不一定是指令：壁纸 JPEG 是裸字节，里面必然有 0x00，
// 按行拼会碎成渣。这条路要能挂到命令通道前面。
//
// ⚠ 两个回调都跑在蓝牙协议栈任务里：必须不阻塞、不碰 LVGL / NVS / SPI，
//   只允许 memcpy 之类的短操作。
typedef bool (*BtBinaryProbe)(const uint8_t* data, size_t len);
typedef void (*BtBinarySink)(const uint8_t* data, size_t len);
void btLinkSetBinaryIo(BtBinaryProbe probe, BtBinarySink sink);

// ---------------------------------------------------------------- 发送：响应
//
// 三选一，都是"发一句话"，分片/收尾/间隔自己处理。
//   btLinkSend ("REMAPDUMP", "0", rules)          // 正文已算好
//   btLinkSendC("GKEYDUMP", "MA:a", "NONE")       // C 字符串
//   btLinkSendf("GKEYPHASE", "MA", "%u", ph)      // printf 风格
// 返回 false = 没发出去（没连上 / 队列满 / 参数非法），调用方可以据此提示。
bool btLinkSend (const char* verb, const char* key, const char* body, size_t len);
bool btLinkSendC(const char* verb, const char* key, const char* body);
bool btLinkSendf(const char* verb, const char* key, const char* fmt, ...)
     __attribute__((format(printf, 3, 4)));

// ---------------------------------------------------------------- 发送：回话
//
// **处理请求的分支里一律用这两个，别用 btLinkSend。**
//
// 请求可以带一个请求号：主机在指令末尾追加 `#123`，btLinkPoll 派发之前会把它
// 摘掉（handleCommand 看到的还是干净的原指令），并记住它；btLinkReply 就用这个
// 请求号当 key。于是：
//   · key 永远是 1~4 位，VERB 再长也装得进 20 字节（见 BT_END_MARK_LEN 那段）
//   · 并发请求不会串台，哪怕两个响应的正文长得一样
//   · 主机侧靠 (verb, 请求号) 配对，不需要猜"这条是回给哪个请求的"
// 主机没带请求号（旧客户端 / 纯推送）时自动退化成自增 key，一样能收。
bool btLinkReplyC(const char* verb, const char* body);
bool btLinkReplyf(const char* verb, const char* fmt, ...)
     __attribute__((format(printf, 2, 3)));

// ---------------------------------------------------------------- 发送：日志流
//
// 高频、允许丢的一类（固件自己的 printf 回显）。内部做 10Hz 节流，
// 并且在关键响应的独占窗口内自动让路。VERB 固定由调用方给（"LOG"）。
bool btLinkStream(const char* verb, const char* text);

// 重置日志流的节流计时器，让下一条立刻放行。
// 按键处理就是典型用例：一次按键会连着打两条日志（"[KB] press code=…" 和
// "[KB] base=0x… mapped=0x…"），10Hz 节流会把第二条整个吞掉 ——
// 而"这次按的键到底发成什么码点"恰恰是排查按键问题最关键的那条证据。
// 节流本身不能撤（不撤的话 50Hz 会在协议栈发送队列里堆几十包，关键响应
// 发不出去），所以给一条显式的"这条别节流"通道。
void btLinkStreamResetThrottle();

// ---------------------------------------------------------------- 发送：流式大块
//
// 和 btLinkSend 的区别是**正文不进队列**：调用方有节奏地喂片段，模块边收边发，
// 于是 8KB 这种量级也不会在某一次 loop 里堆 400 个包。
//
//   if (!btLinkStreamBusy()) {
//       btLinkStreamBegin("LOGDUMP", "0", total);
//       ... 循环 btLinkStreamWrite(片段)，靠 btLinkStreamFull() 做背压 ...
//       btLinkStreamEnd();
//   }
//
// ⚠ **total 不是"诊断信息"，协议真的依赖它。** StreamBegin 之后、正文第一片
//   之前会先发一条 `<VERB>:<KEY>:LEN:<总字节数>`，主机收到 END 时拿它和实际
//   收到的字节数对一遍，对不上就报"传输丢片"而不是把残缺正文交出去。
//   （早期版本这里写的是 `(void)totalLen`，于是主机没有任何办法察觉自己
//   收到的是一份缺了片的配置 —— 而缺片是**静默**的：一条 ATT 通知带不下
//   就被协议栈整条丢掉，收尾标记 END 照样会到。最坏情况下那份残缺正文
//   恰好还是合法 JSON，于是页面照着它回写，缺掉的字段被静默清零。）
//
// 背压：单槽缓冲（BT_TX_BODY_MAX），喂满了 btLinkStreamWrite 返回 false，
// 调用方下一轮 loop 再喂。整段数据任何时刻只有一份副本，且落在 .bss 不吃栈。
bool btLinkStreamBegin(const char* verb, const char* key, size_t totalLen);
bool btLinkStreamWrite(const char* data, size_t len);
bool btLinkStreamWrite(const uint8_t* data, size_t len);
void btLinkStreamEnd();
bool btLinkStreamBusy();      // 还有片段没喂完 / 没发完 → 调用方继续喂
bool btLinkStreamFull();      // 单槽缓冲满了 → 调用方这一轮别再喂了
size_t btLinkStreamRoom();    // 单槽还能塞多少字节 → 给自己的切片封顶（必看）

// **处理请求的分支里起一条流式回复，用这个**，别用 btLinkStreamBegin 配业务 key。
//
// 和 btLinkReply 的理由一样：key 自动取当前请求号（主机在指令末尾加的 `#123`），
// 主机不用凑对。配置 JSON 回拉这类"请求 → 大块正文"就靠它：
//     else if (cmd == "CFGGET") {
//         size_t n = cfgBuildJson(cfgJsonBuf, sizeof(cfgJsonBuf));
//         if (!btLinkReplyStreamBegin("CFGDUMP", n))
//             btLinkReplyf("CFGERR", "链路忙，稍后重试");
//     }
// 之后照样在 loop() 里按 btLinkStreamRoom() 的节奏喂片段、btLinkStreamEnd() 收尾。
//
// ⚠ 请求号只在**指令派发那个调用栈里**有效，所以这个函数必须在 handleCommand
//   里面调（和 btLinkReplyC 一样）。出了这个栈再想起流，主机那边没有号可对。
bool btLinkReplyStreamBegin(const char* verb, size_t totalLen);

// ---------------------------------------------------------------- 接收
//
// onWrite 回调里只调这一个：喂狗 → 二进制旁路判定 → 按行攒 → 入队。
// 回调返回后立刻还给协议栈，绝不在这里解析指令、绝不碰 LVGL。
void btLinkOnWrite(const uint8_t* data, size_t len);

// loop() 里调：收尾超时行 → 发包泵 → 派发指令队列（全部在主任务里）。
void btLinkPoll();

// 喂狗。放在任何可能被长时间占用的循环体里；btLinkDelay() 是它的等待版。
void btLinkKeepAlive();
void btLinkDelay(unsigned long ms);

// 供旁路协议（ME 文本这类自己开文件收的协议）复用同一条指令队列。
QueueHandle_t btLinkCmdQueue();
