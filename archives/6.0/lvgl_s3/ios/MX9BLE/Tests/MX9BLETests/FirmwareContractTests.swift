import XCTest
@testable import MX9BLE

/// 按**固件的真实行为**模拟 `handleCommand()` 里的 ME 分支。
///
/// 固件那边（`lvgl_s3/src/lvgl_s3.ino` + `src/bt_link.cpp`）：
///   · 收到的每行先入队（`bt_link.cpp` 的 `lineSubmit()`），loop() 再排空
///   · 队列只有 **8 格**，满了**丢最老的一条**（宁可旧指令丢，也不能让新指令进不去）
///   · 进 handleCommand 之前，末尾的 `#<请求号>` 被摘掉，用同一个号当回话的 key
///   · ME_START          → SPIFFS.open("/me_hex.txt", FILE_WRITE)（截断）
///   · ME_DATA:<hex>     → 纯写，不关文件
///   · ME_END[:1]        → 关闭文件，回 "OK:<落盘字节数>:<flag>"
///                        flag == 1 时立刻调用 meOutStart()（= 往主机打字）
///   · ME_SEND           → 不下发新文本，直接 meOutStart()
///
/// 这个模拟器存在的意义：**两边各写各的，接口对不上是迟早的事**。
/// 手机端和固件端是不同语言、不同人改的，只有把"两边合起来跑一遍"，
/// 才可能在没有硬件的时候发现接口错位。
final class FakeFirmware {

    /// /me_hex.txt 的内容（hex 字符）。
    private(set) var stored = ""
    /// 固件有没有被要求"立刻发送"。
    private(set) var didSendNow = false
    /// 固件那边模拟"键盘正在往主机打字"。
    private(set) var isSending = false

    /// 队列满了丢最老的一条（固件的真实行为）。
    var dropOldest = true

    private var queue: [(cmd: String, key: String)] = []
    private let queueDepth = 8
    private var autoKey = 0

    /// 主机写进来一条指令。
    ///
    /// 固件这边**只是入队**，真正处理要等 loop() 排空队列 ——
    /// 所以这个方法和 `pump()` 必须分开，不然就模拟不出"队列满了丢指令"。
    ///
    /// - Parameter requestID: 主机加的 `#<请求号>`。传 nil 就模拟"主机没带请求号"，
    ///   固件会自己生成一个自增的 key 当回话的 key。
    @discardableResult
    func write(_ raw: String, requestID: Int? = nil) -> Int {
        var cmd = raw
        if let requestID { cmd += "#\(requestID)" }

        // ---- 固件摘请求号（见 bt_link.cpp 的 btLinkPoll）----
        var key = ""
        if let h = cmd.lastIndex(of: "#"), h != cmd.startIndex {
            let tail = String(cmd[cmd.index(after: h)...])
            if !tail.isEmpty, tail.allSatisfy({ $0.isNumber }) {
                key = tail
                cmd = String(cmd[..<h])
            }
        }
        if key.isEmpty {
            autoKey += 1
            key = String(autoKey)
        }

        queue.append((cmd, key))
        if queue.count > queueDepth {
            if dropOldest { queue.removeFirst() } else { queue.removeLast() }
        }
        return queue.count
    }

    /// 固件的 loop() 排空队列（一次最多 4 条，见 btLinkPoll）。
    /// 返回这一轮产生的回话包（完整的一串通知）。
    func pump(maxCommands: Int = 4) -> [String] {
        var out: [String] = []
        for _ in 0..<maxCommands {
            guard !queue.isEmpty else { break }
            let (cmd, key) = queue.removeFirst()
            out += respond(to: cmd, key: key)
        }
        return out
    }

    /// 把队列彻底排空，返回所有回话。
    ///
    /// ⚠ 结束条件是"队列空了"，**不是"这一轮没产出回话"** ——
    ///   ME_DATA 是没有回话的，只处理掉 4 条 ME_DATA 就收工的话，
    ///   队列里还压着的 ME_END 就永远不会被处理。
    func drain() -> [String] {
        var out: [String] = []
        while !queue.isEmpty {
            out += pump()
        }
        return out
    }

    private func respond(to cmd: String, key: String) -> [String] {
        if cmd == "ME_START" {
            stored = ""          // FILE_WRITE = 截断重写
            isSending = false
            return []
        }

        if cmd.hasPrefix("ME_DATA:") {
            stored += String(cmd.dropFirst("ME_DATA:".count))
            return []
        }

        if cmd.hasPrefix("ME_END") {
            // 固件认 "ME_END" 和 "ME_END:1"，其余后缀一律当 0。
            // ⚠ 后缀要把 ':' 一起去掉 —— 固件那边是
            //   `cmd.indexOf(':')` → `substring(colon + 1)`，
            //   拿到的是冒号**后面**那一段。
            var suffix = String(cmd.dropFirst("ME_END".count))
            if suffix.hasPrefix(":") { suffix = String(suffix.dropFirst()) }
            let sendNow = (suffix == "1")
            didSendNow = sendNow
            if sendNow { isSending = true }
            let body = "OK:\(stored.count):\(sendNow ? 1 : 0)"
            return ["MEEND:\(key):\(body)\r", "MEEND:\(key):END\r"]
        }

        if cmd.hasPrefix("ME_SEND") {
            isSending = true
            return ["MESENT:\(key):OK\r", "MESENT:\(key):END\r"]
        }

        return []
    }

    /// 键盘里存着的 hex 解出来的文本（也就是真正会被打到电脑上的内容）。
    var storedText: String? {
        guard let data = MEProtocol.hexDecode(stored) else { return nil }
        return String(data: data, encoding: .utf8)
    }
}

final class FirmwareContractTests: XCTestCase {

    /// 模拟 App 的正常节奏：写一条 → 让固件排空队列 → 再写下一条。
    /// （真实 App 每条之间等 50ms，固件的 loop() 这期间早就排空好几次了。）
    private func runPaced(_ fw: FakeFirmware, _ cmds: [String], lastRequestID: Int? = nil) -> [BLEReply] {
        let assembler = BLEReplyAssembler()
        var replies: [BLEReply] = []
        for (i, cmd) in cmds.enumerated() {
            let isLast = (i == cmds.count - 1)
            fw.write(cmd, requestID: isLast ? lastRequestID : nil)
            for packet in fw.drain() {
                replies += assembler.feed(Data(packet.utf8))
            }
        }
        return replies
    }

    /// 手机端生成的指令，固件那边跑一遍，内容必须原样回来。
    func testStoreModeRoundTrip() {
        let fw = FakeFirmware()
        let text = "Hello 世界 🌍 [ENTER]"

        let cmds = MEProtocol.meCommands(text: text, mode: .store)
        let replies = runPaced(fw, cmds)

        XCTAssertEqual(fw.storedText, text, "键盘里存的文本必须和发过去的一模一样")
        XCTAssertFalse(fw.didSendNow, "不勾立即发送时，固件不能自己发")
        XCTAssertFalse(fw.isSending)

        // 没带请求号时固件**也会回话**（key 是它自己生成的自增号）。
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies[0].verb, "MEEND")
        XCTAssertFalse(replies[0].key.isEmpty)
        XCTAssertTrue(replies[0].body.hasPrefix("OK:"))
    }

    /// 勾了「立即发送」：固件存完之后必须立刻发起发送。
    func testSendImmediatelyRoundTrip() {
        let fw = FakeFirmware()
        let text = "立即发出"

        let cmds = MEProtocol.meCommands(text: text, mode: .storeAndSend)
        XCTAssertEqual(cmds.last, "ME_END:1", "勾了立即发送，收尾指令必须带 :1")

        _ = runPaced(fw, cmds)

        XCTAssertEqual(fw.storedText, text)
        XCTAssertTrue(fw.didSendNow, "固件必须被要求立刻发送")
        XCTAssertTrue(fw.isSending)
    }

    /// 带请求号 → 固件回话，App 能对上号并拿到落盘字节数。
    func testReplyCarriesStoredSizeAndMatchesExpectation() {
        let fw = FakeFirmware()
        let text = "对一下号"

        let cmds = MEProtocol.meCommands(text: text, mode: .storeAndSend)
        let replies = runPaced(fw, cmds, lastRequestID: 42)

        XCTAssertEqual(replies.count, 1)
        let reply = replies[0]
        XCTAssertEqual(reply.verb, "MEEND")
        XCTAssertEqual(reply.key, "42")
        XCTAssertFalse(reply.truncated)

        // 正文是 "OK:<落盘字节数>:<flag>"
        let parts = reply.body.split(separator: ":").map(String.init)
        XCTAssertEqual(parts.count, 3)
        XCTAssertEqual(parts[0], "OK")
        XCTAssertEqual(Int(parts[1]), MEProtocol.hexEncode(Data(text.utf8)).count,
                       "固件报的落盘字节数必须等于发过去的 hex 字符数")
        XCTAssertEqual(parts[2], "1")
    }

    /// ⚠ 传输丢片的场景：固件队列满了丢掉最老的一条 ME_DATA，
    ///   存进去的文本就少了一段 —— **固件自己不会报错**。
    ///   App 必须靠"落盘字节数对不上"发现它。
    func testDroppedChunkIsDetectableBySizeMismatch() {
        let fw = FakeFirmware()
        fw.dropOldest = true
        let assembler = BLEReplyAssembler()

        // 造一段够长的文本，让 ME_DATA 超过 8 条
        let text = String(repeating: "长", count: 2000)   // 6000 字节 → 12000 hex → 94 片
        let cmds = MEProtocol.meCommands(text: text, mode: .store)
        XCTAssertGreaterThan(cmds.count, 10)

        // ⚠ 这一次**不排空队列**（模拟主机一口气把 94 条全灌进去）：
        //   队列只有 8 格 → 固件丢掉最老的一条 ME_DATA → 文本少了一段。
        var replies: [BLEReply] = []
        for (i, cmd) in cmds.enumerated() {
            let isLast = (i == cmds.count - 1)
            fw.write(cmd, requestID: isLast ? 7 : nil)
            if isLast {
                for packet in fw.drain() {
                    replies += assembler.feed(Data(packet.utf8))
                }
            }
        }

        XCTAssertEqual(replies.count, 1)
        let parts = replies[0].body.split(separator: ":").map(String.init)
        XCTAssertEqual(parts.count, 3)
        let storedSize = Int(parts[1]) ?? -1
        let expected = MEProtocol.hexEncode(Data(text.utf8)).count

        XCTAssertLessThan(storedSize, expected, "模拟器应该真的丢了一片")
        // ★ 关键：App 靠这个不等式发现丢片（SenderModel.handleReply 就是这么判的）
        XCTAssertNotEqual(storedSize, expected)
    }

    /// ME_SEND：不下发文本也能让键盘把存着的打一遍。
    func testSendStoredCommandTriggersSendWithoutNewText() {
        let fw = FakeFirmware()
        let assembler = BLEReplyAssembler()

        // 先存一段
        for cmd in MEProtocol.meCommands(text: "存起来", mode: .store) {
            fw.write(cmd)
            _ = fw.drain()
        }
        XCTAssertFalse(fw.isSending)

        fw.write(MEProtocol.sendStoredCommand, requestID: 9)
        var replies: [BLEReply] = []
        for packet in fw.drain() {
            replies += assembler.feed(Data(packet.utf8))
        }

        XCTAssertTrue(fw.isSending, "ME_SEND 必须让键盘开始发送")
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies[0].verb, "MESENT")
        XCTAssertEqual(replies[0].key, "9")
        XCTAssertEqual(replies[0].body, "OK")
        // 内容没变
        XCTAssertEqual(fw.storedText, "存起来")
    }

    /// 老固件只认光秃秃的 "ME_END" —— 网页端不勾立即发送时发的就是它，
    /// 行为必须和以前完全一样（只存不发）。
    func testPlainMEEndMeansStoreOnly() {
        let fw = FakeFirmware()
        for cmd in ["ME_START", "ME_DATA:4142", "ME_END"] {
            fw.write(cmd)
            _ = fw.drain()
        }
        XCTAssertEqual(fw.storedText, "AB")
        XCTAssertFalse(fw.didSendNow)
        XCTAssertFalse(fw.isSending)
    }
}
