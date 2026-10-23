import CoreBluetooth
import Foundation

/// MX9 键盘的 BLE 中心端（CoreBluetooth）。
///
/// ## 为什么 iPhone 上必须是原生 App，不能用网页
///
/// 网页端那套（仓库根目录的 `s3-setting.html`）走的是 **Web Bluetooth**
/// （`navigator.bluetooth`）。而 **Safari 在 iOS / iPadOS 上完全不支持这个 API**
/// —— WebKit 至今没有 Web Bluetooth 的实现，Chrome on iOS 也只是套了个 WebKit
/// 壳，继承同一个缺口。没有任何参数、开关或 polyfill 能把它打开。
///
/// 所以 iPhone 上「连蓝牙 + 发文本」这条路，**原生 App 是唯一可靠的形态**：
/// CoreBluetooth 是系统自带的，不挑浏览器、不用装东西、不需要 HTTPS。
///
/// （Android / 桌面 Chromium 上那个网页仍然能用，两边共用同一套协议。）
///
/// ## 和固件的约定
///
/// · 特征：WRITE + WRITE_NR + NOTIFY
/// · 主机 → 键盘：一条指令一行，以 **'\n'** 结尾
/// · 键盘 → 主机：一条消息 = `VERB:KEY:<正文片>` × N + `VERB:KEY:END`，以 **'\r'** 结尾
///   （打包/拆包在 `BLEReplyAssembler` 里）
@MainActor
public final class MX9BLE: NSObject, ObservableObject {

    // MARK: - 对外状态

    public enum LinkState: Equatable {
        case bluetoothOff
        case scanning
        case connecting(String)
        case discovering(String)
        case ready(String)
        case failed(String)

        public var isReady: Bool { if case .ready = self { return true }; return false }

        public var label: String {
            switch self {
            case .bluetoothOff: return "蓝牙未开启"
            case .scanning: return "正在扫描…"
            case .connecting(let n): return "正在连接 \(n)…"
            case .discovering(let n): return "正在发现服务 \(n)…"
            case .ready(let n): return "已连接 \(n)"
            case .failed(let m): return "失败：\(m)"
            }
        }
    }

    public struct Discovered: Identifiable, Equatable {
        public let id: UUID
        public let name: String
        public let rssi: Int
        /// 看起来就是 MX9 键盘（广播名对得上）。
        public let isKeyboard: Bool

        public init(id: UUID, name: String, rssi: Int, isKeyboard: Bool) {
            self.id = id
            self.name = name
            self.rssi = rssi
            self.isKeyboard = isKeyboard
        }
    }

    @Published public private(set) var state: LinkState = .bluetoothOff
    @Published public private(set) var discovered: [Discovered] = []
    @Published public private(set) var log: [String] = []
    /// 出错了就显示在界面上 —— 光写进日志用户是看不到的
    /// （"点了没反应"就是这么来的：日志里有一行，界面上一点变化都没有）。
    @Published public private(set) var lastError: String = ""

    /// 键盘主动推过来的消息（LOG 之类）。手机端一般用不上，留在日志里方便排查。
    public var onMessage: ((BLEReply) -> Void)?

    // MARK: - 内部

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var notifyCharacteristic: CBCharacteristic?
    private let assembler = BLEReplyAssembler()

    private var writeQueue: [Data] = []
    private var isWriting = false
    private var reqSeq = 0
    private var pendingRequests: [Int: CheckedContinuation<String, Error>] = [:]
    /// 用户想扫描（还没等蓝牙就绪时记下来，等就绪了自动开扫）。
    private var wantsScan = false

    /// 扫到的外设对象，按 identifier 存着。
    ///
    /// ⚠⚠⚠ **必须自己存着**，不能靠 `retrievePeripherals(withIdentifiers:)` 反查 ——
    ///   那个 API 只对系统**已经认识**的外设（连过/配对过）有效，
    ///   "刚扫到、还没连过"的外设反查回来是 nil ⇒
    ///   connect() 里直接 return ⇒ **点了没反应**。
    ///   扫描回调拿到的 CBPeripheral 才是正确的那一个。
    private var scannedPeripherals: [UUID: CBPeripheral] = [:]

    public override init() {
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
    }

    // MARK: - 对外操作

    public func startScan() {
        wantsScan = true
        guard central.state == .poweredOn else {
            // 蓝牙还没就绪 —— 别直接返回，否则这次扫描就永远丢掉了
            //（onAppear 调 startScan 时 central 往往还没 poweredOn）。
            appendLog("蓝牙还没就绪，等一下自动开始扫描…")
            return
        }
        beginScan(clearList: true)
    }

    /// 已经开始扫了，只是要**继续扫**（不清空已经找到的设备）。
    ///
    /// ⚠ 为什么要有这个：iOS 每次 App 回到前台都会**重发一次**
    ///   `centralManagerDidUpdateState(.poweredOn)`。如果那里直接调
    ///   `startScan()`（它会 `discovered.removeAll()`），那么
    ///   **每次切回 App 设备列表就被清空** —— 用户刚扫到键盘，
    ///   切个别的 App 再回来，列表空了，像是"扫不到"。
    private func beginScan(clearList: Bool) {
        if clearList { discovered.removeAll() }
        appendLog("开始扫描…")
        // ⚠⚠⚠ **这里不能用 withServices 过滤**，必须全扫 + 自己按名字认。
        //
        //   iOS 的 scanForPeripherals(withServices:) 只会报**广播包里带了那个
        //   service UUID** 的设备。而 MX9 键盘的广播包里**只有设备名**：
        //     实测（Mac 上 bleak 抓的广播包）：
        //       service_uuids:      []      ← 空
        //       manufacturer_data:  {}
        //       local_name:         "YYQ-MX9.0"
        //   固件那边 Arduino BLE 2.x 也没有 setAdvertisedService 这种 API，
        //   要在广播里加 service UUID 得自己改 esp_ble_gap_config_adv_data，
        //   还得重新烧录。
        //
        //   ⇒ 按 service 过滤的后果就是：**扫一辈子都扫不到**，
        //     而且没有任何报错（就是扫不到东西）。这正是"一直显示正在扫描"的真凶。
        //
        //   所以：全扫，然后按**广播名**认键盘。
        central.scanForPeripherals(
            withServices: nil,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        state = .scanning
    }

    public func stopScan() {
        wantsScan = false
        central.stopScan()
        if case .scanning = state { state = .bluetoothOff }
    }

    public func connect(_ item: Discovered) {
        wantsScan = false
        // ⚠ 用扫描时存下来的那个 CBPeripheral，**不要**用
        //   retrievePeripherals(withIdentifiers:) 反查 ——
        //   那个只对系统已经认识的外设有效，"刚扫到还没连过"的反查是 nil，
        //   于是这里直接 return，用户看到的就是"点了没反应"。
        guard let p = scannedPeripherals[item.id]
                ?? central.retrievePeripherals(withIdentifiers: [item.id]).first else {
            lastError = "找不到这个设备了，换一个再试试"
            appendLog("找不到这个设备了（\(item.name)）")
            return
        }
        lastError = ""
        central.stopScan()
        appendLog("连接 \(item.name)…")
        state = .connecting(item.name)
        peripheral = p
        p.delegate = self
        central.connect(p, options: nil)
    }

    public func disconnect() {
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        notifyCharacteristic = nil
        assembler.reset()
        failAllPendingRequests("已断开")
        state = .bluetoothOff
        appendLog("已断开")
    }

    /// 发一条**不需要回话**的指令（行尾 '\n' 由这里统一补）。
    @discardableResult
    public func send(_ command: String) -> Bool {
        guard peripheral != nil, notifyCharacteristic != nil else {
            appendLog("还没连上，发不出去")
            return false
        }
        print("[MX9] → \(command)")
        var data = Data(command.utf8)
        data.append(0x0A)   // '\n'
        enqueueWrite(data)
        return true
    }

    /// 发一条指令并**等固件回话**。
    ///
    /// 固件会认指令末尾的 `#<请求号>`，并用同一个号当回话的 key
    /// （见 `bt_link.cpp` 里 `btLinkPoll()` 摘请求号那段），
    /// 所以并发请求不会串台 —— 不需要靠"正文长得像"去猜。
    public func request(_ command: String,
                        verb: String,
                        timeout: TimeInterval = 8) async throws -> String {
        reqSeq = (reqSeq % 9999) + 1
        let id = reqSeq
        let line = "\(command)#\(id)"

        return try await withCheckedThrowingContinuation { cont in
            pendingRequests[id] = cont
            guard send(line) else {
                pendingRequests.removeValue(forKey: id)
                cont.resume(throwing: BLEError.notConnected)
                return
            }
            Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                guard let self else { return }
                if let cont = self.pendingRequests.removeValue(forKey: id) {
                    cont.resume(throwing: BLEError.timeout(verb))
                }
            }
        }
    }

    private func peripheralWithIdentifier(_ id: UUID) -> CBPeripheral? {
        central.retrievePeripherals(withIdentifiers: [id]).first
    }

    // MARK: - 写入

    private func enqueueWrite(_ data: Data) {
        guard let p = peripheral else { return }
        // ⚠ 一次写超过 MTU 是**静默丢包**的元凶，必须自己按协商后的上限切。
        //   iOS 协商出来的一般是 185 或 247（载荷 182 / 244），
        //   一条 "ME_DATA:" + 128 个 hex 字符 = 137 字节，装得下；
        //   但文本长的时候会有很多条，所以这个上限每次都要重新问。
        let maxLen = max(20, p.maximumWriteValueLength(for: .withResponse))
        var offset = 0
        while offset < data.count {
            let n = min(maxLen, data.count - offset)
            writeQueue.append(data.subdata(in: offset..<(offset + n)))
            offset += n
        }
        pumpWrites()
    }

    /// 一次只写一包，等固件 ACK 了再写下一包。
    ///
    /// 用带响应的写（`.withResponse`）而不是不带响应的：
    /// 指令必须**可靠送达**，而且我们要知道它到底发出去没有 ——
    /// 浏览器那边 `writeValue` 就是这个语义。
    private func pumpWrites() {
        guard !isWriting, let p = peripheral, let ch = notifyCharacteristic,
              !writeQueue.isEmpty else { return }
        isWriting = true
        let chunk = writeQueue.removeFirst()
        // ⚠ 必须写死 `.withResponse`（带响应的写）：
        //   指令要**可靠送达**，而且我们要知道它到底发出去没有。
        //   CoreBluetooth 对 `.withoutResponse` 不给任何确认，写丢了自己都不知道。
        p.writeValue(chunk, for: ch, type: .withResponse)
    }

    private func failAllPendingRequests(_ reason: String) {
        for (_, cont) in pendingRequests {
            cont.resume(throwing: BLEError.failed(reason))
        }
        pendingRequests.removeAll()
    }

    public func appendLog(_ s: String) {
        log.append(s)
        if log.count > 200 { log.removeFirst(log.count - 200) }
        // 同时打到 stdout：真机上没别的办法看日志，
        // `xcrun devicectl device process launch --console` 能把这里抓出来。
        print("[MX9] \(s)")
    }

    // MARK: - 回话派发

    private func handle(reply: BLEReply) {
        if reply.truncated {
            appendLog("⚠ \(reply.verb) 传输丢片：固件声明 \(reply.declaredLength ?? -1) 字节")
        }
        // 先看有没有人在等这条。
        if let key = Int(reply.key), let cont = pendingRequests.removeValue(forKey: key) {
            cont.resume(returning: reply.body)
            return
        }
        onMessage?(reply)
    }
}

// MARK: - 错误

public enum BLEError: LocalizedError {
    case notConnected
    case timeout(String)
    case failed(String)

    public var errorDescription: String? {
        switch self {
        case .notConnected: return "还没连上键盘"
        case .timeout(let verb): return "等待 \(verb) 回话超时"
        case .failed(let m): return m
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension MX9BLE: CBCentralManagerDelegate {

    nonisolated public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let raw = central.state
        Task { @MainActor in
            switch raw {
            case .poweredOn:
                appendLog("蓝牙已开启")
                // ⚠ 之前用户点"扫描"时蓝牙还没就绪，这一次扫描就被**悄悄丢掉**了
                //   （startScan 里 `guard central.state == .poweredOn else { return }`），
                //   表现就是"一直显示正在扫描但什么都没有"。
                //   现在记住意图，等就绪了自动补上。
                //
                // ⚠ iOS 每次 App 回到前台都会**重发一次**这个回调，
                //   所以这里不能调 startScan()（它会清空设备列表 ——
                //   用户切个别的 App 再回来，列表就空了，像是"扫不到"）。
                //   只在还没扫到东西时补扫。
                if wantsScan && !state.isReady {
                    beginScan(clearList: discovered.isEmpty)
                }
            case .poweredOff:
                state = .bluetoothOff
                appendLog("蓝牙已关闭")
            case .unauthorized:
                state = .failed("没有蓝牙权限，去「设置 → 隐私与安全性 → 蓝牙」里允许")
            case .unsupported:
                state = .failed("这台设备不支持蓝牙")
            case .resetting:
                state = .bluetoothOff
            case .unknown:
                state = .bluetoothOff
            @unknown default:
                state = .bluetoothOff
            }
        }
    }

    nonisolated public func centralManager(_ central: CBCentralManager,
                                           didDiscover peripheral: CBPeripheral,
                                           advertisementData: [String: Any],
                                           rssi RSSI: NSNumber) {
        // ⚠ iOS/macOS **不保证**未连接设备的 `peripheral.name` 有值 ——
        //   名字常常只出现在广播包里。必须去 advertisementData 里取，
        //   否则会扫到一个"没名字的设备"，用户根本认不出哪个是键盘。
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let name = advName ?? peripheral.name ?? ""
        let id = peripheral.identifier
        let rssi = RSSI.intValue
        let isKeyboard = MEProtocol.looksLikeKeyboard(name: name)
        Task { @MainActor in
            // ★ 把扫描回调拿到的外设对象存下来，连接时直接用它。
            self.scannedPeripherals[id] = peripheral
            if let idx = self.discovered.firstIndex(where: { $0.id == id }) {
                self.discovered[idx] = Discovered(id: id, name: name.isEmpty ? "未知设备" : name,
                                                 rssi: rssi, isKeyboard: isKeyboard)
            } else {
                self.discovered.append(Discovered(id: id, name: name.isEmpty ? "未知设备" : name,
                                                  rssi: rssi, isKeyboard: isKeyboard))
                if isKeyboard {
                    self.appendLog("发现键盘：\(name) (RSSI \(rssi))")
                } else if name.isEmpty {
                    self.appendLog("扫到一个无名设备（RSSI \(rssi)）")
                } else {
                    self.appendLog("扫到设备：\(name) (RSSI \(rssi))")
                }
            }
        }
    }

    nonisolated public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        let name = peripheral.name ?? MEProtocol.deviceName
        Task { @MainActor in
            self.appendLog("已连接 \(name)，正在发现服务…")
            self.state = .discovering(name)
            peripheral.delegate = self
            peripheral.discoverServices([CBUUID(string: MEProtocol.serviceUUIDString)])
        }
    }

    nonisolated public func centralManager(_ central: CBCentralManager,
                                           didFailToConnect peripheral: CBPeripheral,
                                           error: Error?) {
        let msg = error?.localizedDescription ?? "未知原因"
        Task { @MainActor in
            self.appendLog("连接失败：\(msg)")
            self.lastError = "连接失败：\(msg)"
            self.state = .failed(msg)
            self.failAllPendingRequests(msg)
        }
    }

    nonisolated public func centralManager(_ central: CBCentralManager,
                                           didDisconnectPeripheral peripheral: CBPeripheral,
                                           error: Error?) {
        Task { @MainActor in
            self.appendLog("连接断开")
            self.state = .bluetoothOff
            self.notifyCharacteristic = nil
            self.assembler.reset()
            self.failAllPendingRequests("连接断开")
        }
    }
}

// MARK: - CBPeripheralDelegate

extension MX9BLE: CBPeripheralDelegate {

    nonisolated public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            let msg = error.localizedDescription
            Task { @MainActor in
                self.appendLog("发现服务失败：\(msg)")
                self.lastError = "发现服务失败：\(msg)"
                self.state = .failed(msg)
            }
            return
        }
        let uuid = CBUUID(string: MEProtocol.serviceUUIDString)
        guard let service = peripheral.services?.first(where: { $0.uuid == uuid }) else {
            Task { @MainActor in
                self.appendLog("设备上没有这个服务")
                self.lastError = "这个设备上没有 MX9 的服务，可能不是键盘"
                self.state = .failed("设备上没有这个服务")
            }
            return
        }
        Task { @MainActor in
            self.appendLog("找到服务，正在找特征…")
        }
        peripheral.discoverCharacteristics([CBUUID(string: MEProtocol.characteristicUUIDString)], for: service)
    }

    nonisolated public func peripheral(_ peripheral: CBPeripheral,
                                       didDiscoverCharacteristicsFor service: CBService,
                                       error: Error?) {
        if let error {
            let msg = error.localizedDescription
            Task { @MainActor in
                self.appendLog("发现特征失败：\(msg)")
                self.lastError = "发现特征失败：\(msg)"
                self.state = .failed(msg)
            }
            return
        }
        let uuid = CBUUID(string: MEProtocol.characteristicUUIDString)
        guard let ch = service.characteristics?.first(where: { $0.uuid == uuid }) else {
            Task { @MainActor in
                self.appendLog("设备上没有这个特征")
                self.lastError = "这个设备上没有 MX9 的特征，可能不是键盘"
                self.state = .failed("设备上没有这个特征")
            }
            return
        }
        Task { @MainActor in
            self.notifyCharacteristic = ch
            peripheral.setNotifyValue(true, for: ch)
            let name = self.state.isReady ? (peripheral.name ?? MEProtocol.deviceName) : MEProtocol.deviceName
            self.state = .ready(name)
            self.appendLog("就绪，可以发了")
        }
    }

    nonisolated public func peripheral(_ peripheral: CBPeripheral,
                                       didUpdateValueFor characteristic: CBCharacteristic,
                                       error: Error?) {
        if let error {
            let msg = error.localizedDescription
            Task { @MainActor in self.appendLog("收到通知出错：\(msg)") }
            return
        }
        guard let data = characteristic.value else { return }
        Task { @MainActor in
            for reply in self.assembler.feed(data) {
                self.handle(reply: reply)
            }
        }
    }

    nonisolated public func peripheral(_ peripheral: CBPeripheral,
                                       didWriteValueFor characteristic: CBCharacteristic,
                                       error: Error?) {
        Task { @MainActor in
            self.isWriting = false
            if let error {
                self.appendLog("写入失败：\(error.localizedDescription)")
            }
            self.pumpWrites()
        }
    }
}
