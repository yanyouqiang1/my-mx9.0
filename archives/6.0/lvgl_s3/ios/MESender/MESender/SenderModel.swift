import Combine
import SwiftUI
import MX9BLE

/// 下发逻辑（和界面分开，方便单独看）。
///
/// ⚠ 这里的**节奏**是有讲究的，别随手改快：
///
///   固件的指令队列只有 **8 格**，满了会**丢掉最老的一条**（`bt_link.cpp` 的
///   `lineSubmit()`：宁可旧指令丢，也不能让新指令进不去）。
///   一条 ME_DATA 丢了，存进键盘的文本就**少了一段**，而且**不报错** ——
///   静默的数据损坏比报错更难查。
///
///   网页端 `s3-setting.html` 每条之间等 50ms 就是这个原因。
///   这里用同样的 50ms：BLE 的写是**带响应**的（每条都有 ACK），
///   一条 ACK 往返本来就 15~30ms，再留一点余量，队列基本不会积压。
@MainActor
final class SenderModel: ObservableObject {

    @Published var text: String = ""
    @Published var mode: MEProtocol.SendMode = .storeAndSend
    @Published var progress: String = ""
    @Published var result: String = ""
    @Published var isSending = false
    /// 扫描时把周围的设备全列出来。
    ///
    /// 默认**打开**：现在是全扫（键盘广播包里没有 service UUID，没法按 service
    /// 过滤），万一 iOS 那边没把广播名传过来，只显示"像键盘的"就是一个**空列表**
    /// —— 用户连试都试不了。宁可列表吵一点，也得让人能点。
    /// 认得出键盘时它会排在最前面并标「键盘」。
    @Published var showAllDevices = true

    let ble = MX9BLE()

    /// 把 ble 的变化**转发**给 model 自己。
    ///
    /// ⚠⚠⚠ 不转发的话，View 永远看不到 ble 的变化 ——
    ///
    ///   View 里读的是 `model.ble.discovered` / `model.ble.state`，
    ///   但 SwiftUI 只观察 **model 自己的** objectWillChange。
    ///   `ble` 是**另一个** ObservableObject（`let ble = MX9BLE()`），
    ///   它变了**不会**通知 View。
    ///
    ///   后果：设备列表 / 连接状态 / 日志都停在旧值，要等**别的东西**
    ///   触发重绘才刷新。症状特别迷惑人：
    ///     · 扫到键盘了，列表不出现
    ///     · 点一下「只存入」这个和蓝牙毫无关系的按钮 → **列表突然全冒出来了**
    ///   那个按钮改了 `model.mode`，`model` 通知了 View，重绘时
    ///   才把 `ble.discovered` 读出来 —— 于是"像是"点它才刷新。
    private var bleRelay: AnyCancellable?

    init() {
        bleRelay = ble.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }
    }

    /// 下发一条 ME_DATA 之后至少停这么久（毫秒）。
    private let chunkGapMs: UInt64 = 50

    var canSend: Bool {
        !isSending && ble.state.isReady && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    // MARK: - 下发

    func send() async {
        guard !isSending else { return }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            result = "⚠️ 文本是空的"
            return
        }
        guard ble.state.isReady else {
            result = "⚠️ 还没连上键盘"
            return
        }

        isSending = true
        result = ""
        defer { isSending = false }

        let cmds = MEProtocol.meCommands(text: text, mode: mode)
        let expectedHex = MEProtocol.hexEncode(Data(text.utf8)).count
        result = ""

        // 前面的 ME_START / ME_DATA 逐条发；最后一条 ME_END 带请求号，等回话。
        for (i, cmd) in cmds.enumerated() {
            if i == cmds.count - 1 {
                break
            }
            guard ble.send(cmd) else {
                result = "❌ 第 \(i + 1) 条没发出去（连接断了？）"
                return
            }
            progress = "下发中… \(i + 1)/\(cmds.count - 1) 条"
            try? await Task.sleep(nanoseconds: chunkGapMs * 1_000_000)
        }

        let last = cmds[cmds.count - 1]
        progress = "等待键盘回话…"
        do {
            let body = try await ble.request(last, verb: "MEEND")
            print("[MX9] ← MEEND \(body)")
            progress = ""
            handleReply(body, expectedHex: expectedHex)
        } catch {
            print("[MX9] ✗ \(error.localizedDescription)")
            progress = ""
            result = "❌ 没等到键盘回话：\(error.localizedDescription)"
        }
    }

    // MARK: - 同步时间

    /// 键盘最后一次确认的时间（epoch 秒）；nil = 还没同步过。
    @Published var keyboardTime: Int?

    /// 界面上显示的键盘时间。
    var keyboardTimeText: String {
        guard let t = keyboardTime else { return "未同步" }
        return Self.formatTime(t)
    }

    static func formatTime(_ epoch: Int) -> String {
        let d = Date(timeIntervalSince1970: TimeInterval(epoch))
        let f = DateFormatter()
        f.locale = Locale(identifier: "zh_CN")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f.string(from: d)
    }

    /// 把手机的当前时间同步给键盘。
    ///
    /// 走固件的 `TIME:<epoch>`：settimeofday + 存 NVS + 更新内存副本。
    ///
    /// ⚠ **要兼容两种固件**：
    ///   · 新固件会回 `TIME:<key>:OK:<epoch>:<HH:MM>` —— 拿回话里的 epoch
    ///     和发过去的对一下，对不上就说明固件那边收到的不是我们发的那个值，
    ///     **不能当成功**（静默的错时间比不同步更难查）。
    ///   · 老固件**一声不吭**（原来的 TIME: 分支不回话），但命令照样送达、
    ///     照样会 settimeofday。所以超时就归到"已下发、未确认"，
    ///     用**琥珀色**而不是绿色提示 —— 不能拿没确认的东西报成功。
    func syncTime() async {
        guard ble.state.isReady else {
            result = "⚠️ 还没连上键盘"
            return
        }
        guard !isSending else { return }

        isSending = true
        defer { isSending = false }

        let epoch = Int(Date().timeIntervalSince1970)
        do {
            let body = try await ble.request(MEProtocol.timeCommand(epoch: epoch),
                                            verb: "TIME", timeout: 4)
            print("[MX9] ← TIME \(body)")
            guard let got = MEProtocol.parseTimeReply(body), got == epoch else {
                result = "⚠️ 键盘回了「\(body)」，和发过去的时间对不上，没同步成功"
                return
            }
            keyboardTime = got
            result = "✅ 时间已同步：\(Self.formatTime(got))"
            print("[MX9] ✅ 时间已同步 \(Self.formatTime(got))")
        } catch let e as BLEError {
            print("[MX9] ← TIME \(e.localizedDescription)")
            if case .timeout = e {
                // 老固件：命令送到了（BLE 的写是带响应的，收到 ACK 才算送达），
                // 但它不回话，没法确认它认下来的值是不是我们发的那个。
                keyboardTime = epoch
                result = "⏳ 时间已下发：\(Self.formatTime(epoch))\n"
                    + "键盘固件较旧，没有回话确认（刷了新固件就能看到确认）。"
            } else {
                result = "❌ 同步时间失败：\(e.localizedDescription)"
            }
        } catch {
            result = "❌ 同步时间失败：\(error.localizedDescription)"
        }
    }

    /// 让键盘把**当前存着的**文本立刻再打一遍（不下发新文本）。
    func resendStored() async {        guard ble.state.isReady else {
            result = "⚠️ 还没连上键盘"
            return
        }
        isSending = true
        defer { isSending = false }
        do {
            _ = try await ble.request(MEProtocol.sendStoredCommand, verb: "MESENT")
            result = "✅ 已让键盘把存着的文本重新打一遍"
        } catch {
            result = "❌ \(error.localizedDescription)"
        }
    }

    /// 解析固件的 `MEEND` 回话：`OK:<落盘字节数>:<flag>` 或 `ERR:<原因>`。
    private func handleReply(_ body: String, expectedHex: Int) {
        let parts = body.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        if parts.first == "ERR" {
            result = "❌ 键盘说：\(parts.dropFirst().joined(separator: ":"))"
            return
        }
        guard parts.count >= 3, parts[0] == "OK" else {
            result = "❌ 回话看不懂：\(body)"
            return
        }
        let stored = Int(parts[1]) ?? -1
        let sentNow = (parts[2] == "1")

        // ⚠ 落盘字节数必须和发过去的 hex 字符数一样。
        //   不一样 = 传输丢片了（队列满时固件会丢最老的一条指令，
        //   少了的那片就是文本里缺的一段，而且不报错）。
        //   这里是**唯一**能发现它的机会 —— 键盘不会说自己少收了。
        if stored != expectedHex {
            result = "⚠️ 传输丢片：应为 \(expectedHex) 字符，键盘只存了 \(stored)。\n"
                + "多半是连接不稳，重发一次试试。"
            return
        }

        if sentNow && mode == .storeAndSend {
            result = "✅ 已存入并立即发送（\(expectedHex / 2) 字节文本）"
            print("[MX9] ✅ 已存入并立即发送（\(expectedHex / 2) 字节）")
        } else {
            result = "✅ 已存入（\(expectedHex / 2) 字节文本）。按键盘上的 ME 键就会发出去。"
            print("[MX9] ✅ 已存入（\(expectedHex / 2) 字节）")
        }
    }
}
