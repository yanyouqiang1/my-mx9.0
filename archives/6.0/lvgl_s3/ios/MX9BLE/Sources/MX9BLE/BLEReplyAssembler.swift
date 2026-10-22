import Foundation

/// 固件发过来的一条完整消息。
public struct BLEReply {
    /// 动词（"MEEND" / "MESENT" / "LOG" …）。
    public let verb: String
    /// 键 —— 固件回话时用的是主机给的请求号，没有请求号时固件自己生成。
    public let key: String
    /// 正文（不含 VERB/KEY，也不含收尾标记）。
    public let body: String
    /// 固件声明的总字节数（只在流式回复里有；普通短回复为 nil）。
    public let declaredLength: Int?
    /// 收到的字节数与声明对不上 → 传输丢片。
    public let truncated: Bool

    public init(verb: String, key: String, body: String, declaredLength: Int? = nil, truncated: Bool = false) {
        self.verb = verb
        self.key = key
        self.body = body
        self.declaredLength = declaredLength
        self.truncated = truncated
    }
}

/// 把固件的 BLE 通知拼回一条条完整消息。
///
/// ⚠ 这是网页端 `s3-setting.html` 里 `BtLink` 那个拼包器的另一半，
///   **规则必须一模一样**，改一边要改另一边。
///
/// 规则：
///   · 包终止符是 **'\r'**（0x0D），不是 '\n'。
///     '\n' 在正文里太常见（日志回拉整个就是一堆换行分隔的行），
///     用它收尾会把一条日志劈成好几段。
///   · 一条消息 = `VERB:KEY:<正文片>` × N + `VERB:KEY:END`。
///     **收到 END 才算一条消息完整** —— 一片正文和"整条消息刚好装得下"
///     在电路上长得一模一样，接收端没有任何办法区分。
///   · 流式回复（CFGDUMP 那种）的第一片前面会粘一条 `LEN:<总字节数>`，
///     用来核对有没有丢片。
///
/// 按**字节**切 '\r'，最后再按 UTF-8 解码 —— 这样即使一片正好把一个
/// 汉字切成两半，也不会吐出豆腐块（0x0D 不可能出现在 UTF-8 多字节序列里，
/// 所以按字节切是安全的）。
public final class BLEReplyAssembler {

    private var pending = Data()
    private var buffers: [String: String] = [:]
    private var declared: [String: Int] = [:]

    public init() {}

    /// 喂进来一段通知字节，返回拼好的完整消息（可能 0 条、也可能多条）。
    public func feed(_ data: Data) -> [BLEReply] {
        pending.append(data)
        var out: [BLEReply] = []

        while let idx = pending.firstIndex(of: 0x0D) {
            let lineData = Data(pending[pending.startIndex..<idx])
            pending = Data(pending[pending.index(after: idx)...])
            if let reply = handleLine(lineData) {
                out.append(reply)
            }
        }
        return out
    }

    /// 换一条连接 = 上一条连接上没收完的正文缓冲全部作废。
    /// 留着的话，下一次同名消息会把两段半截正文拼起来。
    public func reset() {
        pending.removeAll(keepingCapacity: true)
        buffers.removeAll(keepingCapacity: true)
        declared.removeAll(keepingCapacity: true)
    }

    // MARK: - 内部

    private func handleLine(_ lineData: Data) -> BLEReply? {
        // 空行直接忽略。
        guard !lineData.isEmpty else { return nil }
        guard let line = String(data: lineData, encoding: .utf8) else { return nil }

        // 切三段：VERB / KEY / 正文。正文里可以有 ':'。
        guard let c1 = line.firstIndex(of: ":") else { return nil }
        let verb = String(line[line.startIndex..<c1])
        guard !verb.isEmpty else { return nil }
        let afterVerb = line.index(after: c1)
        guard let c2 = line[afterVerb...].firstIndex(of: ":") else { return nil }
        let key = String(line[afterVerb..<c2])
        var body = String(line[line.index(after: c2)...])

        let id = verb + "|" + key

        if body == "END" {
            // ⚠ 这里交出去的是**累积出来的正文**，不是这句 "END" 本身。
            //   "END" 是固件给每条消息加的收尾标记，不代表正文内容。
            let full = buffers[id] ?? ""
            buffers.removeValue(forKey: id)
            let expect = declared.removeValue(forKey: id)
            var truncated = false
            if let expect, expect >= 0 {
                // 固件按 **UTF-8 字节**算，这里也必须按字节，不能用 body.count
                //（一个汉字 count 1 但占 3 字节，用 count 会对不上）。
                let got = full.utf8.count
                if got != expect {
                    truncated = true
                }
            }
            return BLEReply(verb: verb, key: key, body: full,
                            declaredLength: expect, truncated: truncated)
        }

        // 长度握手行：认下来但**不拼进正文**。
        //
        // ⚠ 它几乎总是和正文的第一片**粘在同一个 body 里**：
        //   固件在 20 字节预算下唯一能用的发法就是把 LEN 塞进流式单槽的开头，
        //   和正文首片连续存放。所以这里要认两种形态：
        //     "LEN:356"          （后面那片正好是纯握手）
        //     "LEN:356{..."       （这一片顺手带了首片正文）
        //   真正的正文以 "{" 开头，所以用第二段是空或 "{" 来区分，
        //   不会误伤 LOGDUMP 那种正文里本来就带 "LEN:" 的情况。
        if let parsed = parseLengthHandshake(&body) {
            declared[id] = parsed
        }

        buffers[id] = (buffers[id] ?? "") + body
        return nil
    }

    /// 把 body 开头的 "LEN:<数字>" 摘下来，返回总字节数（没有就返回 nil）。
    private func parseLengthHandshake(_ body: inout String) -> Int? {
        guard body.hasPrefix("LEN:") else { return nil }
        let after = body.dropFirst(4)
        let digits = after.prefix { $0.isNumber }
        guard !digits.isEmpty else { return nil }
        let rest = after.dropFirst(digits.count)
        // 第二段必须为空或以 "{" 开头，否则这不是长度握手（是正文自己长得像）。
        guard rest.isEmpty || rest.hasPrefix("{") else { return nil }
        body = String(rest)
        return Int(digits)
    }
}
