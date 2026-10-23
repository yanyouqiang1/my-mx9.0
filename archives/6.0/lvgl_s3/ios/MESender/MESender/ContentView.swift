import SwiftUI
import MX9BLE

struct ContentView: View {
    @StateObject private var model = SenderModel()
    @FocusState private var editorFocused: Bool

    var body: some View {
        NavigationStack {
            Form {
                connectionSection
                textSection
                modeSection
                actionSection
                if !model.result.isEmpty { resultSection }
                logSection
            }
            .navigationTitle("MX9 键盘助手")
            .toolbar {
                ToolbarItem(placement: .keyboard) {
                    Spacer()
                    Button("收起键盘") { editorFocused = false }.padding(.trailing, 8)
                }
            }
            .onAppear {
                model.ble.startScan()
            }
        }
    }

    // MARK: - 连接

    private var connectionSection: some View {
        Section("蓝牙连接") {
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                Text(model.ble.state.label)
                    .foregroundStyle(.secondary)
                Spacer()
                if model.ble.state.isReady {
                    Button("断开") { model.ble.disconnect() }
                        .buttonStyle(.bordered)
                } else {
                    Button("扫描") { model.ble.startScan() }
                        .buttonStyle(.bordered)
                }
            }

            // 错误必须显示在界面上 —— 光写日志的话，用户只会看到
            // "点了没反应"（上一版就是这样：找不到设备只记了一行日志）。
            if !model.ble.lastError.isEmpty {
                Label(model.ble.lastError, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote)
                    .foregroundStyle(.red)
            }

            // 同步时间放在**连接区**里，而不是"动作"区 ——
            // 它是设备级的操作（不是 ME 文本的一部分），而且必须连上了才有意义，
            // 挨着连接状态放最顺：知道连上了，才谈得上"把时间给它"。
            //
            // ⚠ 常驻显示（没连上时置灰），不藏起来 ——
            //   藏了就没人知道有这功能了。
            HStack {
                Label("键盘时间", systemImage: "clock")
                    .font(.subheadline)
                Spacer()
                Text(model.keyboardTimeText)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Button("同步") {
                    Task { await model.syncTime() }
                }
                .buttonStyle(.bordered)
                .disabled(model.isSending || !model.ble.state.isReady)
            }

            if !model.ble.state.isReady {
                if !model.ble.discovered.isEmpty {
                    Toggle("显示全部设备", isOn: $model.showAllDevices)
                        .font(.footnote)

                    // 键盘排最前面，其余按信号强度排
                    ForEach(sortedDevices) { d in
                        Button {
                            model.ble.connect(d)
                        } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    HStack(spacing: 6) {
                                        Text(d.name).foregroundStyle(.primary)
                                        if d.isKeyboard {
                                            Text("键盘")
                                                .font(.caption2)
                                                .padding(.horizontal, 5)
                                                .padding(.vertical, 1)
                                                .background(Color.green.opacity(0.2))
                                                .foregroundStyle(.green)
                                                .cornerRadius(4)
                                        }
                                    }
                                    Text("RSSI \(d.rssi) dBm")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                } else if model.ble.state == .scanning {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("正在找 \(MEProtocol.deviceName)…")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    Text("找不到的话：确认键盘通电、离手机近一点，然后点「扫描」重来。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    /// 键盘排最前，其次按信号强度（RSSI 大 = 近）。
    ///
    /// 默认**只显示**像键盘的设备 —— 因为现在是全扫（广播包里没有 service
    /// UUID，没法按 service 过滤），周围一堆手表/耳机/别的键盘会全冒出来。
    /// 「显示全部设备」打开就能看到完整列表，排查时用。
    private var sortedDevices: [MX9BLE.Discovered] {
        let list = model.showAllDevices
            ? model.ble.discovered
            : model.ble.discovered.filter { $0.isKeyboard }
        return list.sorted { a, b in
            if a.isKeyboard != b.isKeyboard { return a.isKeyboard }
            return a.rssi > b.rssi
        }
    }

    private var statusColor: Color {
        switch model.ble.state {
        case .ready: return .green
        case .failed: return .red
        case .bluetoothOff: return .gray
        default: return .orange
        }
    }

    // MARK: - 文本

    private var textSection: some View {
        Section {
            // 清空按钮浮在输入框**右上角**。
            //
            // 为什么放这儿而不是别的地方：
            //   · 视线落点 —— 刚写完一段话，眼睛就在这一带，不用找按钮
            //   · 不占纵向空间（这个 Form 已经挺长了）
            //   · 和 iOS 的通用心智一致（备忘录/微信/聊天框的"×"都在右上角）
            //   · 只在**有内容时**才出现，空的时候不添乱
            ZStack(alignment: .topTrailing) {
                TextEditor(text: $model.text)
                    .frame(minHeight: 130)
                    .focused($editorFocused)
                    .font(.body)
                    .overlay(alignment: .topLeading) {
                        if model.text.isEmpty {
                            Text("要发给电脑的文字…（支持中文）")
                                .foregroundStyle(.tertiary)
                                .padding(.top, 8)
                                .padding(.leading, 5)
                                .allowsHitTesting(false)
                        }
                    }

                if !model.text.isEmpty {
                    Button {
                        model.text = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 16))
                            .symbolRenderingMode(.palette)
                            .foregroundStyle(.secondary, Color(.secondarySystemGroupedBackground))
                    }
                    .buttonStyle(.plain)
                    .padding(.top, 6)
                    .padding(.trailing, 10)
                    .disabled(model.isSending)
                    .accessibilityLabel("清空文本")
                }
            }
        } header: {
            Text("要发送的文本")
        } footer: {
            Text("键盘用的是 hex 通道，所以中文要先转成 hex 再传过去，内容不会变成乱码。")
        }
    }

    // MARK: - 发送方式

    private var modeSection: some View {
        Section {
            Picker("发送方式", selection: $model.mode) {
                ForEach(MEProtocol.SendMode.allCases, id: \.self) { m in
                    Text(m.label).tag(m)
                }
            }
            .pickerStyle(.segmented)
            Text(model.mode.hint)
                .font(.footnote)
                .foregroundStyle(.secondary)
        } header: {
            Text("发送方式")
        }
    }

    // MARK: - 动作

    private var actionSection: some View {
        Section {
            Button {
                editorFocused = false
                Task { await model.send() }
            } label: {
                HStack {
                    Text(model.isSending ? "发送中…" : "下发到键盘")
                    Spacer()
                    if model.isSending { ProgressView() }
                }
            }
            .disabled(!model.canSend)

            Button {
                Task { await model.resendStored() }
            } label: {
                Text("重发键盘里存着的文本")
            }
            .disabled(!model.ble.state.isReady || model.isSending)

            if !model.progress.isEmpty {
                Text(model.progress)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("动作")
        } footer: {
            Text("「重发」不重新传文本 —— 它让键盘把当前存着的 ME 文本再打一遍，等价于按一下键盘上的 ME 键。")
        }
    }

    // MARK: - 结果

    private var resultSection: some View {
        Section("结果") {
            Text(model.result)
                .font(.callout)
                .textSelection(.enabled)
        }
    }

    // MARK: - 日志

    private var logSection: some View {
        Section("日志") {
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(model.ble.log.enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            .frame(maxHeight: 180)
        }
    }
}

#Preview {
    ContentView()
}
