import XCTest
@testable import MX9BLE

final class MEProtocolTests: XCTestCase {

    // MARK: - hex

    func testHexEncodeKnownVectors() {
        // "Hi" = 0x48 0x69
        XCTAssertEqual(MEProtocol.hexEncode(Data("Hi".utf8)), "4869")
        // 空串 → 空
        XCTAssertEqual(MEProtocol.hexEncode(Data()), "")
        // 中文 "中" = UTF-8 E4 B8 AD
        XCTAssertEqual(MEProtocol.hexEncode(Data("中".utf8)), "e4b8ad")
        // 全部 256 个字节都过一遍，确认没有大小写/补零错误
        var all = Data()
        for i in 0...255 { all.append(UInt8(i)) }
        let hex = MEProtocol.hexEncode(all)
        XCTAssertEqual(hex.count, 512)
        XCTAssertEqual(hex.prefix(4), "0001")
        XCTAssertEqual(hex.suffix(4), "feff")
        // 小写（要和网页端 s3-setting.html 一致）
        XCTAssertFalse(hex.contains(where: { $0.isUppercase }))
    }

    func testHexDecodeRoundTrip() {
        let original = "hello 中 world 🌍"
        let hex = MEProtocol.hexEncode(Data(original.utf8))
        let back = MEProtocol.hexDecode(hex)
        XCTAssertNotNil(back)
        XCTAssertEqual(String(data: back!, encoding: .utf8), original)
        // 奇数长度 / 非法字符都要返回 nil，不能瞎猜
        XCTAssertNil(MEProtocol.hexDecode("abc"))
        XCTAssertNil(MEProtocol.hexDecode("zz"))
    }

    // MARK: - 指令拼装

    func testMECommandsStoreMode() {
        let cmds = MEProtocol.meCommands(text: "abc", mode: .store)
        XCTAssertEqual(cmds, ["ME_START", "ME_DATA:616263", "ME_END"])
        // 只存入 → 结尾**不带** flag
        XCTAssertTrue(cmds.last!.hasSuffix("ME_END"))
        XCTAssertFalse(cmds.last!.contains(":"))
    }

    func testMECommandsSendImmediatelyAddsFlag() {
        let cmds = MEProtocol.meCommands(text: "abc", mode: .storeAndSend)
        XCTAssertEqual(cmds, ["ME_START", "ME_DATA:616263", "ME_END:1"])
    }

    func testMECommandsChunking() {
        // 300 个 ASCII 字符 'A' = 600 个 hex 字符（'A' = 0x41 → "41"）
        // 600 = 128×4 + 88 → 5 片
        //   前 4 片各 128 个 hex 字符 = 64 个 "41"
        //   最后一片 88 个 hex 字符 = 44 个 "41"
        let text = String(repeating: "A", count: 300)
        let cmds = MEProtocol.meCommands(text: text, mode: .store)
        XCTAssertEqual(cmds.count, 1 + 5 + 1)
        XCTAssertEqual(cmds[0], "ME_START")
        for i in 1...4 {
            XCTAssertEqual(cmds[i], "ME_DATA:" + String(repeating: "41", count: 64))
        }
        XCTAssertEqual(cmds[5], "ME_DATA:" + String(repeating: "41", count: 44))
        XCTAssertEqual(cmds[6], "ME_END")

        // 拼回去必须还原成原来的文本 —— 这是"分片/拼包"唯一真正要保证的事
        var rebuilt = ""
        for cmd in cmds where cmd.hasPrefix("ME_DATA:") {
            rebuilt += String(cmd.dropFirst("ME_DATA:".count))
        }
        XCTAssertEqual(MEProtocol.hexDecode(rebuilt).map { String(data: $0, encoding: .utf8) }, text)
    }

    /// 每条指令行必须落在固件的单行上限（BT_RX_BUF_SIZE = 256）之内。
    /// 超了固件会**丢掉尾部**，剩下的半截会被当成一条新命令解析。
    func testEveryCommandLineFitsFirmwareLineBuffer() {
        let limit = 255
        for text in ["a", String(repeating: "中", count: 1000), String(repeating: "x", count: 5000)] {
            for cmd in MEProtocol.meCommands(text: text, mode: .storeAndSend) {
                XCTAssertLessThanOrEqual(cmd.utf8.count, limit,
                                         "指令太长（\(cmd.utf8.count) 字节）：\(cmd.prefix(40))…")
            }
        }
    }

    func testMECommandsEmptyText() {
        // 空文本也要能生成指令（固件那边会报"存入失败"，由 App 层拦）
        let cmds = MEProtocol.meCommands(text: "", mode: .store)
        XCTAssertEqual(cmds, ["ME_START", "ME_END"])
    }

    // MARK: - 回复拼包

    private func feed(_ assembler: BLEReplyAssembler, _ s: String) -> [BLEReply] {
        assembler.feed(Data(s.utf8))
    }

    func testSingleChunkReply() {
        let a = BLEReplyAssembler()
        var replies = feed(a, "MEEND:42:OK:600:1\r")
        XCTAssertTrue(replies.isEmpty, "还没收到 END，不该算一条完整消息")
        replies = feed(a, "MEEND:42:END\r")
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies[0].verb, "MEEND")
        XCTAssertEqual(replies[0].key, "42")
        XCTAssertEqual(replies[0].body, "OK:600:1")
        XCTAssertFalse(replies[0].truncated)
    }

    func testMultiChunkReply() {
        let a = BLEReplyAssembler()
        _ = feed(a, "LOGDUMP:1:第一段\r")
        _ = feed(a, "LOGDUMP:1:第二段\r")
        let replies = feed(a, "LOGDUMP:1:END\r")
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies[0].body, "第一段第二段")
    }

    /// 通知可能在**任意字节**上被切开 —— 尤其可能切在一个汉字中间。
    /// 拆包必须按字节做，最后再解 UTF-8，否则会吐豆腐块。
    func testNotificationSplitInsideUTF8Character() {
        let a = BLEReplyAssembler()
        let payload = Array("MEEND:7:OK:你好世界\rMEEND:7:END\r".utf8)
        // 在 "你" 的 UTF-8 字节中间切开（E4 B8 AD 的第 2 个字节）
        let cut = payload.index(payload.startIndex, offsetBy: 11)
        var replies = a.feed(Data(payload[payload.startIndex..<cut]))
        XCTAssertTrue(replies.isEmpty)
        replies = a.feed(Data(payload[cut...]))
        XCTAssertEqual(replies.count, 1)
        XCTAssertEqual(replies[0].body, "OK:你好世界")
    }

    func testLengthHandshakeBothForms() {
        // 形态一：LEN 单独一条（正文 "{abc}" = 5 字节）
        let a = BLEReplyAssembler()
        _ = feed(a, "CFGDUMP:3:LEN:5\r")
        let r1 = feed(a, "CFGDUMP:3:{abc}\r")
        XCTAssertTrue(r1.isEmpty)
        let r2 = feed(a, "CFGDUMP:3:END\r")
        XCTAssertEqual(r2[0].body, "{abc}")
        XCTAssertEqual(r2[0].declaredLength, 5)
        XCTAssertFalse(r2[0].truncated)

        // 形态二：LEN 和首片正文粘在一起（固件在 20 字节预算下的唯一发法）
        let b = BLEReplyAssembler()
        _ = feed(b, "CFGDUMP:4:LEN:5{abc}\r")
        let r3 = feed(b, "CFGDUMP:4:END\r")
        XCTAssertEqual(r3[0].body, "{abc}")
        XCTAssertEqual(r3[0].declaredLength, 5)
        XCTAssertFalse(r3[0].truncated)
    }

    func testTruncatedReplyIsFlagged() {
        let a = BLEReplyAssembler()
        _ = feed(a, "CFGDUMP:5:LEN:100\r")
        let r = feed(a, "CFGDUMP:5:{abc}\r")
        XCTAssertTrue(r.isEmpty)
        let done = feed(a, "CFGDUMP:5:END\r")
        XCTAssertTrue(done[0].truncated, "字节数对不上必须报丢片")
    }

    /// 正文里本来就带 "LEN:" 的情况不能被误当成长度握手。
    func testBodyThatLooksLikeLengthHandshakeIsNotConsumed() {
        let a = BLEReplyAssembler()
        _ = feed(a, "LOG:1:LEN:abc\r")
        let r = feed(a, "LOG:1:END\r")
        XCTAssertEqual(r[0].body, "LEN:abc")
        XCTAssertNil(r[0].declaredLength)
    }

    // MARK: - 广播名匹配

    /// ⚠ 这条测试是"一直扫不到键盘"那个 bug 的护栏。
    ///
    /// 键盘的广播包里**没有 service UUID**（实测 service_uuids 是空的），
    /// 所以 iOS 只能靠 `scanForPeripherals(withServices: nil)` 全扫，
    /// 再按**广播名**认设备。名字认错了，App 就永远找不到键盘。
    func testLooksLikeKeyboard() {
        XCTAssertTrue(MEProtocol.looksLikeKeyboard(name: "YYQ-MX9.0"))
        XCTAssertTrue(MEProtocol.looksLikeKeyboard(name: "yyq-mx9.0"))   // 大小写
        XCTAssertTrue(MEProtocol.looksLikeKeyboard(name: "  YYQ-MX9.0  "))
        // 用户改过广播名的情况
        XCTAssertTrue(MEProtocol.looksLikeKeyboard(name: "YYQ-MX9.0-2"))
        XCTAssertTrue(MEProtocol.looksLikeKeyboard(name: "my-mx9-keyboard"))

        // 周围的别的设备不能被误认成键盘
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: ""))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "   "))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "iPhone"))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "yyq2 号"))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "Apple Watch"))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "HUAWEI FreeBuds Pro 4"))
        XCTAssertFalse(MEProtocol.looksLikeKeyboard(name: "未知设备"))
    }

    // MARK: - 时间

    func testTimeCommandFormat() {
        // 固件的 TIME: 分支是 cmd.substring(5).toInt() —— 冒号后必须是纯数字
        XCTAssertEqual(MEProtocol.timeCommand(epoch: 1791456000), "TIME:1791456000")
        XCTAssertEqual(MEProtocol.timeCommand(epoch: 0), "TIME:0")
    }

    /// ⚠ 固件那边 Arduino 的 String::toInt() 是 long（32 位有符号），
    ///   2038 年前都没问题，但这条要在测试里钉住 —— 哪天超过 2^31
    ///   就会被固件截断，键盘时间直接错 68 年，而且**不报错**。
    func testTimeEpochFitsFirmware32BitSigned() {
        let now = Int(Date().timeIntervalSince1970)
        XCTAssertLessThan(now, Int(Int32.max))
        // 解析回来的必须和发出去的一模一样
        let reply = "OK:\(now):17:30"
        XCTAssertEqual(MEProtocol.parseTimeReply(reply), now)
    }

    func testParseTimeReplyRejectsGarbage() {
        XCTAssertNil(MEProtocol.parseTimeReply("ERR:失败"))
        XCTAssertNil(MEProtocol.parseTimeReply("OK"))
        XCTAssertNil(MEProtocol.parseTimeReply("OK:"))
        XCTAssertNil(MEProtocol.parseTimeReply("OK:abc:17:30"))
        XCTAssertNil(MEProtocol.parseTimeReply(""))
    }

    func testResetDropsPartialBuffers() {
        let a = BLEReplyAssembler()
        _ = feed(a, "LOG:9:半截\r")
        a.reset()
        let r = feed(a, "LOG:9:END\r")
        // reset 之后原来那条的缓冲没了，只有一条空消息
        XCTAssertEqual(r.count, 1)
        XCTAssertEqual(r[0].body, "")
    }
}
