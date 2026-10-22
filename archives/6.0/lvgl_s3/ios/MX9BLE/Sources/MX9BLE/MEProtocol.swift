import Foundation

/// MX9 键盘 BLE 协议 —— **纯逻辑**部分，不碰 CoreBluetooth。
///
/// 这样切分是有意的：协议（hex 怎么转、指令怎么拼、回复怎么拼包）
/// 是这个项目里**最容易改错**的部分，而它**不需要蓝牙就能测**。
/// 放进一个纯 Foundation 的库里，`swift test` 就能把它钉死，
/// 不用每次都把手机拿出来连一遍键盘。
///
/// 对应的固件侧代码：
///   · 收指令     `lvgl_s3/src/lvgl_s3.ino` 的 `handleCommand()`（ME_START / ME_DATA / ME_END）
///   · 发回复     `lvgl_s3/src/bt_link.cpp` 的 `btLinkReplyf()`
///   · 页面参照   仓库根目录的 `s3-setting.html`（网页端同一个协议的另一半）
public enum MEProtocol {

    // MARK: - 设备标识

    /// 固件的广播名（`lvgl_s3.ino` 里 `BLEDevice::init("YYQ-MX9.0")`）。
    public static let deviceName = "YYQ-MX9.0"

    /// 服务 UUID —— 固件 `SERVICE_UUID`。
    public static let serviceUUIDString = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"

    /// 特征 UUID —— 固件 `CHARACTERISTIC_UUID`（WRITE + WRITE_NR + NOTIFY）。
    public static let characteristicUUIDString = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

    // MARK: - ME 文本协议

    /// 一条 `ME_DATA:` 带多少个 hex 字符。
    ///
    /// 128 是网页端 `s3-setting.html` 用的值，**两边必须一样**。
    /// 约束来自固件：单条指令行最多 255 字节（`BT_RX_BUF_SIZE`），
    /// 超长的行会被**丢掉尾部**而不是截断提交 —— 那样半截数据会被
    /// 当成一条新命令解析，静默写坏配置。
    /// "ME_DATA:"(8) + 128 = 136，离 255 还很远。
    public static let hexChunkSize = 128

    /// 文本下发完之后，键盘要不要**立刻**把内容打在电脑上。
    ///
    /// - `store`：只存进 `/me_hex.txt`，等用户按键盘上的 ME 键。
    /// - `storeAndSend`：存进去之后立刻通过 USB HID 打给主机。
    ///
    /// 两种模式下 `/me_hex.txt` 的内容**都会**被更新 ——
    /// 区别只是"要不要现在就把它打出去"。
    public enum SendMode: String, Sendable, CaseIterable {
        case store
        case storeAndSend

        public var label: String {
            switch self {
            case .store: return "只存入"
            case .storeAndSend: return "存入并立即发送"
            }
        }

        public var hint: String {
            switch self {
            case .store:
                return "存进键盘后，需要按键盘上的 ME 键才会发给电脑。"
            case .storeAndSend:
                return "存进键盘后立刻通过键盘打给电脑，不用再按 ME 键。"
            }
        }
    }

    // MARK: - hex

    private static let hexDigits: [Character] = Array("0123456789abcdef")

    /// UTF-8 文本 → hex 字符串。
    ///
    /// 用**小写**，和网页端 `s3-setting.html` 的
    /// `view[i].toString(16).padStart(2, '0')` 一致。
    /// 这不是格式要求（固件那边只是原样转发 ASCII 字符，大小写由电脑端解码器说了算），
    /// 而是"两条已经跑通的路径要长得一样" —— 少一个变量少一类玄学问题。
    public static func hexEncode(_ data: Data) -> String {
        var out = String()
        out.reserveCapacity(data.count * 2)
        for b in data {
            out.append(hexDigits[Int(b >> 4)])
            out.append(hexDigits[Int(b & 0x0F)])
        }
        return out
    }

    /// hex 字符串 → 字节（给单测和"读回来看看"用）。
    public static func hexDecode(_ hex: String) -> Data? {
        let chars = Array(hex)
        guard chars.count % 2 == 0 else { return nil }
        var out = Data()
        out.reserveCapacity(chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = chars[i].hexDigitValue, let lo = chars[i + 1].hexDigitValue else { return nil }
            out.append(UInt8(hi << 4 | lo))
            i += 2
        }
        return out
    }

    // MARK: - 指令拼装

    /// 生成下发一段文本所需的**全部**指令（按顺序）。
    ///
    /// 形如：
    /// ```
    /// ME_START
    /// ME_DATA:<hex 第 1 片>
    /// ME_DATA:<hex 第 2 片>
    /// ...
    /// ME_END          // 只存入
    /// ME_END:1        // 存入并立即发送
    /// ```
    ///
    /// 每条指令**不带**行尾 `\n` —— 行尾由发送层统一补
    /// （固件那边按 `\n` 攒行，见 `bt_link.cpp` 的 `btLinkOnWrite`）。
    public static func meCommands(text: String, mode: SendMode) -> [String] {
        meCommands(hex: hexEncode(Data(text.utf8)), mode: mode)
    }

    public static func meCommands(hex: String, mode: SendMode) -> [String] {
        var cmds: [String] = ["ME_START"]
        var idx = hex.startIndex
        while idx < hex.endIndex {
            let end = hex.index(idx, offsetBy: hexChunkSize, limitedBy: hex.endIndex) ?? hex.endIndex
            cmds.append("ME_DATA:" + hex[idx..<end])
            idx = end
        }
        // ★ `ME_END:1` 这个 flag 是后加的，固件认两种：
        //   `ME_END`    → 只存不发（老网页发的就是这个，行为不变）
        //   `ME_END:1`  → 存完立刻打给主机
        cmds.append("ME_END" + (mode == .storeAndSend ? ":1" : ""))
        return cmds
    }

    /// 让键盘把**当前存着的**文本立刻打一遍（不下发新文本）。
    ///
    /// 等价于用户按了一下键盘上的 ME 键 —— 走的是固件里同一个 `meOutStart()`。
    public static let sendStoredCommand = "ME_SEND"
}
