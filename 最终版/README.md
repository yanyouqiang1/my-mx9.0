# YYQ-MX9.0 机械键盘

## 硬件架构

| 组件 | 型号 | 功能 |
|------|------|------|
| 主控 | ESP32-S3 | 键盘矩阵扫描、按键发送、USB/BLE通讯 |
| 协处理器 | ESP32-C3 | 旋钮控制、灯光效果管理 |
| 扩展芯片 | MCP23017 | I2C 16列扫描扩展 |
| 灯带 | WS2812 × 16 | 主灯条RGB灯光 |
| 指示灯 | WS2812 × 3 | 底部模式指示灯 |

## 通讯架构

```
┌─────────┐     串口(RX/TX)     ┌─────────┐
│   S3    │ ◄──────────────────► │   C3    │
│ (主控)  │                      │ (旋钮)  │
└────┬────┘                      └────┬────┘
     │                                │
     │ USB HID                        │ BLE
     ▼                                ▼
┌─────────┐                   ┌─────────────┐
│  电脑   │                   │ 蓝牙串口助手 │
└─────────┘                   └─────────────┘
```

## 引脚连接

### S3 → C3 串口通讯
- S3 TX (GPIO9) → C3 RX (GPIO20)
- S3 RX (GPIO10) → C3 TX (GPIO10)

### C3 旋钮/按键
- GPIO0: 旋钮按键
- GPIO1: CPG模式按键
- GPIO2: 静音按键
- GPIO3: 灯光按键
- GPIO5: 编码器A相(CLK)
- GPIO6: 编码器B相(DT)
- GPIO21: WS2812主灯带
- GPIO9: 底部指示灯

## 蓝牙服务

### S3 BLE
- Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- Characteristic UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

### C3 BLE (Nordic UART)
- Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX Characteristic: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`

## 编译环境

- 开发板: ESP32S3 Dev Module
- Flash Size: 16MB
- Partition Scheme: 16MB Flash (3MB APP, 9MB FATFS)
- USB Mode: USB CDC + JTAG

## 指令协议

### S3 → C3 (串口)
| 指令 | 说明 |
|------|------|
| `V+` | 音量增加 |
| `V-` | 音量减少 |
| `N_ME` | 记忆更新完成 |
| `N_REBOT` | 键盘重启 |
| `N_ROOT` | 进入刷机模式 |
| `N_KEY_PRESS` | 按键按下 |

### 蓝牙 → C3
| 指令 | 说明 |
|------|------|
| `R` | 红灯闪烁 |
| `G` | 绿灯闪烁 |
| `B` | 蓝灯闪烁 |
| `Y` | 黄灯闪烁 |
| `S` | 关闭提示 |

## 功能快捷键

| 组合 | 功能 |
|------|------|
| FN + PLAY | 进入睡眠 |
| FN + NEXT | 唤醒电脑 |
| FN + PREV | 关机 |
| LOGO 单击 | 切换LOGO灯效 |
| LOGO 长按2-8秒 | 普通重启 |
| LOGO 长按≥8秒 | 进入ROM刷机 |

## License

MIT
