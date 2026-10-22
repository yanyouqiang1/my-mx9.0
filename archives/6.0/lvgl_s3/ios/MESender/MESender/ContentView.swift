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

            if !model.ble.state.isReady && !model.ble.discovered.isEmpty {
                ForEach(model.ble.discovered) { d in
                    Button {
                        model.ble.connect(d)
                    } label: {
                        HStack {
                            VStack(alignment: .leading) {
                                Text(d.name).foregroundStyle(.primary)
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
            }
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
