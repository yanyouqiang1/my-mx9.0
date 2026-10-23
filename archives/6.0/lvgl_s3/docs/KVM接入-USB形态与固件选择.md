# KVM 接入：USB 形态与固件选择（**必读**）

> **一句话结论：给用户用的固件必须烧「产品环境」（`esp32-s3-devkitc-1`），
> 绝不能烧 `_debug` 环境。** `_debug` 的 USB 上多挂了一个 CDC 复合设备，
> 经 KVM 接入时会把 KVM 内部的 USB Hub 芯片搞挂，**鼠标会跟着一起不能用**。

最后更新：2026-10-23（实战验证通过）

---

## 1. 为什么这条这么重要

这块键盘的**实际使用方式就是经 KVM 接到 Mac 上**（KVM 同时接显示器、
键盘、鼠标）。所以「USB 上多挂一个 CDC」不是可有可无的细节 ——
它会**直接让用户连鼠标都动不了**，键盘本身也输入不进去。

2026-10-23 实战复现：插入 KVM 后，鼠标完全失灵，键盘也敲不出字。
换成产品固件后，**一切正常**（USB HID 正常枚举、鼠标正常、KVM 稳定）。

---

## 2. 两个环境的 USB 形态对比

| | 产品 `esp32-s3-devkitc-1` | `_debug` |
|---|---|---|
| `ARDUINO_USB_MODE` | **1** | 0 |
| `ARDUINO_USB_CDC_ON_BOOT` | 1 | 1 |
| USB 形态 | **纯 HID** | **HID + CDC 复合设备** |
| macOS 认成 | `Espressif ESP32-S3-DevKitC-1-N8`（HID） | `AppleUSBCDCCompositeDevice` |
| `/dev/cu.usbmodem*` 节点 | **无** | 有（可抓固件日志） |
| 键盘 HID 输入 | ✅ | ✅ |
| **经 KVM 接入** | ✅ **正常** | ❌ **拖垮同 Hub 上的鼠标** |
| 用途 | **给用户用 / 量产** | 只能自己调试时用 |

两个环境**共用同一份固件代码**，USB 形态差异全部来自 `platformio.ini`
里那一个宏。

### 为什么 `_debug` 会多一个 CDC

框架里 `Serial` 指向哪个实现，由 `ARDUINO_USB_MODE` 决定
（`cores/esp32/USBCDC.h:141`）：

```c
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE  // Serial used for USB CDC
extern USBCDC Serial;
#endif
```

`ARDUINO_USB_MODE=0` → `Serial` = TinyUSB 的 CDC → USB 上多一个 CDC 接口。
CDC 接口由 `ARDUINO_USB_CDC_ON_BOOT=1` 挂上去，于是 USB 变成
**HID + CDC 复合设备**。

### HID 本身不受影响（别误判）

`ESPUSB::begin()` 无论哪个模式都会调 `tinyusb_init()`
（`cores/esp32/USB.cpp`），HID 接口由 `USBHID.cpp:285` 单独
`tinyusb_enable_interface(USB_INTERFACE_HID, …)` 注册。
⇒ **产品固件照样是能用的 USB 键盘**，不是「`ARDUINO_USB_MODE=1` 就没 HID 了」。

### 一秒钟查证当前固件的 USB 形态

不用烧录就能看二进制里挂了哪些 USB 接口：

```bash
cd lvgl_s3
strings .pio/build/esp32-s3-devkitc-1/firmware.bin       | grep -E "TinyUSB (CDC|HID)"
# 输出只有 TinyUSB HID          → 纯 HID，产品形态

strings .pio/build/esp32-s3-devkitc-1_debug/firmware.bin | grep -E "TinyUSB (CDC|HID)"
# 输出有 TinyUSB CDC 和 HID     → 复合设备，debug 形态
```

**或在 macOS 上看枚举结果**：

```bash
ioreg -p IOUSB -c IOUSBHostDevice | grep -E "USB Product Name|idProduct"
```

- 出现 `AppleUSBCDCCompositeDevice` → 板上跑的是 **debug 固件**，经 KVM 会出事
- 只有纯 HID 的 `Espressif ESP32-S3-DevKitC-1-N8` → 产品固件，正常

---

## 3. 崩溃链（macOS 系统日志实锤，不是推测）

2026-10-23 14:15 的系统日志原文：

```
14:15:28 kernel: (IOUSBHostFamily) AppleUSBIORequest::complete:
    device 3 (IOUSBHostDevice@01110000) endpoint 0x83:
    status 0xe00002ed (transaction error): 0 bytes transferred
14:15:28 kernel: (IOUSBHostFamily) AppleUSBIORequest::complete:
    device 3 endpoint 0x82: status 0xe00002ed (transaction error)
14:15:28 kernel: (IOUSBHostFamily) AppleUSBIORequest::complete:
    device 3 endpoint 0x81: status 0xe00002ed (transaction error)
14:15:28 kernel: (AppleUSB20HubPort@01110000):
    AppleUSBHostPort::terminateDevice: destroying 0x1111/2222/0000:
    hardware connection lost
```

### 逐条解读

**① `0x1111/2222` 是谁？**

KVM 内部的 **VIA Labs USB2.0 Hub** 芯片。在 macOS 的设备树里能直接看到：

```
"USB Product Name" = "USB2.0 Hub"
"USB Vendor Name"  = "VIA Labs, Inc."
"idVendor" = 8457   (0x2109)
"idProduct" = 10263 (0x2827)
"bDeviceClass" = 9  ← 9 = Hub
```

它是 KVM 把「键盘 + 鼠标」共享给 Mac 的那一层。

**② 三个端点同时 transaction error → Hub 自己挂了**

`0x81 / 0x82 / 0x83` 是 Hub 的控制 + 中断 + bulk 端点。三个全挂说明
**不是某个下游设备出问题，是 Hub 芯片自己没扛住**。
紧接着 `terminateDevice: hardware connection lost` → Hub 被销毁 →
**挂在它下面的鼠标跟着一起从总线上掉**。

**③ 触发它的是什么？**

我们这块板子的 CDC 复合设备：

```
14:15:36 Espressif ESP32-S3-DevKitC-1-N8@01110000: setConfigurationGated:
    AppleUSBCDCCompositeDevice selected configuration 1
14:15:36 WeChat@(null): openGated: failed to open Espressif ESP32-S3-DevKitC-1-N8@01110000:
    provider is already opened for exclusive access by AppleUSBCDCCompositeDevice
14:15:36 Google Chrome@(null): openGated: failed to open ...: 同上
```

CDC 复合设备在 macOS 上被当作**独占资源**。KVM 的 Hub 芯片在 12Mbps
全速链路上处理这类复合设备的控制传输（SET_CONFIGURATION 之后要拉一堆
描述符）容易超时 → 端点 transaction error → Hub 复位。

**④ 为什么键盘自己也不输入了？**

Hub 复位时下游设备一起被踢出总线，键盘的 HID 端点也跟着断。
所以「鼠标不动 + 键盘不输入」是**同一个原因的两个表现**，
不是两个独立的 bug。

---

## 4. 结论与规矩

### ✅ 规矩（写死，别改）

1. **给用户的板子一律烧产品环境**：
   ```bash
   cd lvgl_s3 && pio run -e esp32-s3-devkitc-1
   ```
2. **`_debug` 只能在自己工位上用**，而且**用完必须拔插一次 USB**
   （或改插到不带 KVM 的口上）再交还。
3. **量产 / 出厂固件 = 产品环境**，没有例外。

### ⚠ 需要产品环境的理由不止 KVM

产品环境还有两处优势：

- **产品固件没有 CDC 就没有 `/dev/cu.usbmodem*` 节点** → macOS 上不会多出
  一个串口设备，用户插上键盘时不会看到一个多余的「串口」出现在
  蓝牙/键盘设置里。
- **`_debug` 的 `LOG_CDC_MIRROR` 会让日志在 USB 上实时刷**。经 KVM 时
  这又是一份额外的 USB 流量，也可能在 Hub 带宽紧张时添乱。

### ❌ 明确**不是**这个原因的几件事

排查过程中被推翻的结论，留个记录免得下次重走：

| 曾经的猜测 | 实测结论 |
|---|---|
| 是我们固件加了串口功能导致的 | `_debug` 从 `992792d` 起就有 CDC，**本轮（`a031f4d`..HEAD`）根本没动过 USB 配置** —— `platformio.ini` 唯一的改动是 `CORE_DEBUG_LEVEL 5→3` |
| 是 JPEGDEC 解码崩了导致 USB 乱 | 崩溃根因是**日志洪水**（`CORE_DEBUG_LEVEL=5`），与 USB 形态无关，且已修复 |
| 是 BLE 抢占 CPU 拖垮 USB | BLE 走的是射频，和 USB 是两套 PHY；真凶是日志洪水 |
| 是 SPIFFS 写满导致复位 | 实测分区剩 1279347 字节，**远没满** |

---

## 5. 烧录注意（产品固件）

### 5.1 烧完**必须人工拔插一次 USB**

`esptool run` 之后芯片**仍然停在 ROM 下载模式** —— macOS 上还是
`USB JTAG_serial debug unit`，BLE 不广播，HID 也不出现。

这是 native USB 口的**硬限制**：USB 会话在复位那一瞬间会把 IO0 压低，
芯片复位后又被采样成低电平 → 又落回 ROM 下载模式。
（试过四种 DTR/RTS 时序 + `esptool run` + `--after hard_reset`，全部无效。）

⇒ **烧完拔掉 USB 线等 2 秒再插回去**，芯片走正常上电复位才能进 app 模式。

另一条路是固件自己的 `BOOTAPP` 命令（BLE 通道）：它把 GPIO0 配成输出
拉高再 `esp_restart()`，可以从下载模式拽回应用模式。

### 5.2 烧录只写 app0，不碰分区表和 NVS

```bash
# 只写 0x10000（app0）。分区表 / bootloader / NVS 都不碰 ——
# 用户的按键重映射、方案、灯光、壁纸全在 NVS 和 SPIFFS 里，碰了就全没了。
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --before no_reset --after no_reset \
  write_flash 0x10000 .pio/build/esp32-s3-devkitc-1/firmware.bin
```

### 5.3 正常现象速查

| 现象 | 含义 |
|---|---|
| macOS 上 `USB JTAG_serial debug unit` | 还在 ROM 下载模式 → **拔插一次 USB** |
| macOS 上 `YYQ-MX9.0`（纯 HID，无 CDC 节点） | ✅ 产品固件跑起来了 |
| macOS 上 `AppleUSBCDCCompositeDevice` | ❌ 跑的是 debug 固件，**经 KVM 会拖垮鼠标** |
| 有 `/dev/cu.usbmodem*` 节点 | 同上，是 debug 固件 |

---

## 6. 验证清单（每次换固件都过一遍）

- [ ] `strings firmware.bin | grep "TinyUSB CDC"` → **无输出**（纯 HID）
- [ ] 烧录后拔插一次 USB
- [ ] macOS 上枚举为 `YYQ-MX9.0`，**没有** CDC 节点
- [ ] 键盘输入正常
- [ ] **接上 KVM**，鼠标正常、键盘正常 ← **本项目的核心使用场景**
- [ ] BLE 能扫到 `YYQ-MX9.0`（配置页要靠它）
