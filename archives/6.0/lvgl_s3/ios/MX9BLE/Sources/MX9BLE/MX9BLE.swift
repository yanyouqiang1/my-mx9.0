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
    }

    @Published public private(set) var state: LinkState = .bluetoothOff
    @Published public private(set) var discovered: [Discovered] = []
    @Published public private(set) var log: [String] = []

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
        guard central.state == .poweredOn else { return }
        discovered.removeAll()
        appendLog("开始扫描…")
        central.scanForPeripherals(
            withServices: [CBUUID(string: MEProtocol.serviceUUIDString)],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        state = .scanning
    }

    public func stopScan() {
        central.stopScan()
        if case .scanning = state { state = .bluetoothOff }
    }

    public func connect(_ item: Discovered) {
        guard let p = central.retrievePeripherals(withIdentifiers: [item.id]).first
                ?? peripheralWithIdentifier(item.id) else {
            appendLog("找不到这个设备了")
            return
        }
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
                if case .bluetoothOff = state { state = .bluetoothOff }
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
        Task { @MainActor in
            if let idx = self.discovered.firstIndex(where: { $0.id == id }) {
                self.discovered[idx] = Discovered(id: id, name: name.isEmpty ? "未知设备" : name, rssi: rssi)
            } else {
                self.discovered.append(Discovered(id: id, name: name.isEmpty ? "未知设备" : name, rssi: rssi))
                if name == MEProtocol.deviceName {
                    self.appendLog("发现键盘：\(name) (RSSI \(rssi))")
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
                self.state = .failed(msg)
            }
            return
        }
        let uuid = CBUUID(string: MEProtocol.serviceUUIDString)
        guard let service = peripheral.services?.first(where: { $0.uuid == uuid }) else {
            Task { @MainActor in
                self.appendLog("设备上没有这个服务")
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
                self.state = .failed(msg)
            }
            return
        }
        let uuid = CBUUID(string: MEProtocol.characteristicUUIDString)
        guard let ch = service.characteristics?.first(where: { $0.uuid == uuid }) else {
            Task { @MainActor in
                self.appendLog("设备上没有这个特征")
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
