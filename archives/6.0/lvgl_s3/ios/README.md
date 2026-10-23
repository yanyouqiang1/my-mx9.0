# MX9 键盘 · iOS 端

在 iPhone 上给 MX9 键盘写 ME 文本，然后让它发到电脑上。

```
lvgl_s3/ios/
  MX9BLE/      协议库（纯逻辑 + CoreBluetooth），swift test 可测
  MESender/    SwiftUI App（xcodegen 生成工程）
```

---

## 为什么 iPhone 上必须用原生 App，不能用网页

仓库根目录的 `s3-setting.html` 走的是 **Web Bluetooth**（`navigator.bluetooth`）。
而 **Safari 在 iOS / iPadOS 上根本不支持这个 API** —— WebKit 至今没有
Web Bluetooth 的实现，Chrome on iOS 也只是套了 WebKit 壳，继承同一个缺口。
没有任何参数、开关或 polyfill 能把它打开。

| 平台 | 能不能用网页版 |
|---|---|
| Windows / Android / ChromeOS（Chromium） | ✅ 能 |
| macOS（Chrome 141+ / Safari 26+） | ✅ 能 |
| **iPhone / iPad（任何浏览器）** | ❌ **不能** |

所以 iPhone 这条路，**原生 App 是唯一可靠的形态**：CoreBluetooth 是系统自带的，
不挑浏览器、不用装东西、不需要 HTTPS、不需要装任何证书。

App 只做 ME 这一件事（输入 → 存入 → 可选立即发送），不追求复刻整个网页。

---

## 跑起来

### 1. 装到手机上（推荐用脚本）

```bash
cd lvgl_s3/ios
./install.sh
```

脚本会：生成 Xcode 工程 → 编译并签名 → 装到手机 → 启动。

⚠ **一次性前置条件**：Xcode → **Settings → Accounts → + → Apple Account**，
用 **374580618@qq.com** 登录一次。

没登录的话第 3 步会报：

```
error: No Accounts: Add a new account in Accounts settings.
error: No profiles for 'com.yanyouqiang.mx9.MESender' were found.
```

这是因为**建 provisioning profile 必须有开发者门户的凭据**，本机没有 ——
Xcode 里虽然还留着个人 Team（`RQ6477DX7S`）和签名证书，但账号列表是空的，
没法自动为新 bundle ID 建 profile。

⚠ 也可以手动指定设备：`./install.sh <UDID>`。

### 2. 手动装（想在 Xcode 里点 ▶︎ 就用这个）

```bash
cd lvgl_s3/ios/MESender
xcodegen generate
open MESender.xcodeproj
```

工程用的是**自动签名**，Team 已经填成 `RQ6477DX7S`（个人 Team）。
在 Xcode 里：

1. 选中 `MESender` target → **Signing & Capabilities**
2. 勾上 **Automatically manage signing**，Team 选「有强 严 (Personal Team)」
3. 把 **Bundle ID** 改成别人没用过的（默认是 `com.yanyouqiang.mx9.MESender`）
4. 用数据线连 iPhone，选中它，点 ▶︎ 运行

第一次运行会弹蓝牙权限请求 —— **必须允许**，否则扫不到键盘。

### ⚠ 免费账号的 profile 只有 7 天

个人开发者（免费账号）的 provisioning profile **7 天就过期**。过期之后
装不上（报签名错误或 profile 失效），重跑一次 `./install.sh` 刷新就行。

要长期免折腾就得用付费开发者账号（profile 有效期 1 年）。

### 3. 用

1. 点「扫描」，列表里选 `YYQ-MX9.0`
2. 在文本框里写要发的内容（支持中文）
3. 选发送方式：
   - **只存入** —— 存进键盘，按键盘上的 ME 键才发出去
   - **存入并立即发送** —— 存进去之后立刻通过键盘打在电脑上
4. 点「下发到键盘」

「重发键盘里存着的文本」不重新传文本 —— 它让键盘把当前存着的 ME 文本
再打一遍（固件的 `ME_SEND`），等价于远程按了一下 ME 键。

---

## 协议

完整说明在 [`../README.md` 的「ME 键文本通道」一节`](../README.md)。这里只列
App 用到的部分。

### 主机 → 键盘（一条指令一行，以 `\n` 结尾）

```
ME_START
ME_DATA:<hex 片段>     每片 ≤128 个 hex 字符
ME_END[:flag]          flag=1 → 存完立刻发出去
ME_SEND                把当前存着的打一遍
```

hex 是**小写**的 UTF-8 十六进制，和网页端 `s3-setting.html` 一致。

### 键盘 → 主机（`VERB:KEY:<正文片>` × N + `VERB:KEY:END`，以 `\r` 结尾）

```
MEEND:<key>:OK:<落盘字节数>:<flag>
MESENT:<key>:OK
```

`key` 是主机加的请求号（指令末尾的 `#<数字>`），固件会摘掉它并用同一个号
当回话的 key —— 并发请求不会串台。

### 两个必须守着的规矩

1. **发 `ME_DATA` 要有节奏**（每条间隔 50ms）。
   固件的指令队列只有 8 格，满了会**丢掉最老的一条** —— 丢一条就是文本少一段，
   而且固件**不报错**。

2. **拿落盘字节数对一下**。
   固件回 `OK:<落盘字节数>`，这个数必须等于你发过去的 hex 字符数
   （`2 × UTF-8 字节数`）。对不上就是丢片了，要提示用户重发。
   App 里的实现在 `SenderModel.handleReply()`。

---

## 测试

协议逻辑（hex 编码、分片、回复拼包）**不需要蓝牙就能测**：

```bash
cd lvgl_s3/ios/MX9BLE
swift test
```

测试分两层：

- `MEProtocolTests` —— 协议本身的规则（hex、分片、拆包、丢片检测）
- `FirmwareContractTests` —— 用一个**照着固件真实行为写的模拟器**
  （`FakeFirmware`）把手机端和固件端合起来跑一遍

第二层是关键：手机端和固件端是不同语言、不同人改的，只有合起来跑一遍，
才可能在没有硬件的时候发现接口错位。改协议时先改测试。

---

## 目录

```
MX9BLE/Sources/MX9BLE/
  MEProtocol.swift        hex 编码、指令拼装（纯 Foundation，可单测）
  BLEReplyAssembler.swift 通知 → 完整消息（纯 Foundation，可单测）
  MX9BLE.swift            CoreBluetooth 中心端

MESender/MESender/
  MESenderApp.swift       @main
  ContentView.swift       界面
  SenderModel.swift       下发逻辑 + 丢片检测
```
