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

    /// 判断一个广播名看起来是不是 MX9 键盘。
    ///
    /// ⚠ 为什么只能靠**名字**认设备，不能靠 service UUID：
    ///
    ///   iOS 的 `scanForPeripherals(withServices:)` 只会报**广播包里带了那个
    ///   service UUID** 的外设。而 MX9 键盘的广播包里**只有名字**：
    ///   实测抓到的广播内容是
    ///       service_uuids:      []      ← 空
    ///       manufacturer_data:  {}
    ///       local_name:         "YYQ-MX9.0"
    ///
    ///   Arduino BLE 2.x 也没有 `setAdvertisedService` 这种 API，要在广播里
    ///   加 service UUID 得自己改 `esp_ble_gap_config_adv_data`（还要重新烧录）。
    ///
    ///   ⇒ 按 service 过滤 = **扫一辈子都扫不到**，而且**不报任何错**。
    ///     所以改成全扫 + 按广播名认。
    ///
    ///   认不准也不要紧：连上之后会走 GATT 服务发现，认错了会明确报
    ///   "设备上没有这个服务"，不会连到别的设备上去。
    public static func looksLikeKeyboard(name: String) -> Bool {
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if n.isEmpty { return false }
        if n == deviceName.lowercased() { return true }
        // ⚠ 只认 "MX9"（机型名），**不要**认 "YYQ" —— 用户名下还有别的设备
        //   叫 "yyq2 号"（另一台 HID 键盘 / 手机名），按 "YYQ" 认会认错。
        //   "MX9" 是 MX9 这个机型独有的，覆盖用户改过广播名的情况
        //   （比如 "my-mx9-keyboard"）。
        return n.contains("mx9")
    }

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
