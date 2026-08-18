# S3 重启/刷机控制迁移至 C3 设计

## 1. 背景

### 1.1 问题描述

当前 S3 的 LOGO 按键长按逻辑存在可靠性问题：
- S3 需要同时处理键盘矩阵扫描和 LOGO 按键检测
- 当 I2C 总线（MCP23017）繁忙或故障时，LOGO 按键可能无法被及时检测
- 用户反馈在需要进入刷机模式时，键盘按键有时会失效

### 1.2 解决方案

将 S3 的重启/刷机控制功能迁移到 C3：
- C3 作为独立的 LED 控制器，有自己的按键检测逻辑
- 即使 S3 键盘扫描完全失效，C3 仍能正常工作
- 通过 UART 发送命令控制 S3

## 2. 命令定义

### 2.1 C3 → S3 命令

| 命令 | 说明 | 触发条件 |
|------|------|---------|
| `N_S3_REBOT` | 重启 S3 | CPG 长按 ≥ 3秒 |
| `N_S3_ROOT` | 进入 S3 刷机模式 | MUTE 长按 ≥ 3秒 |

### 2.2 触发时序

```
用户长按 CPG 3秒
    ↓
C3 检测到长按，发送 Serial1.println("N_S3_REBOT")
    ↓
C3 指示灯闪烁 3 次确认
    ↓
S3 收到命令，执行 esp_restart()
```

## 3. C3 改动

### 3.1 按键逻辑修改

**原有逻辑（保留短按）：**
- CPG 短按：切换 CPG 灯效模式
- MUTE 短按：切换音量控制模式

**新增逻辑（长按处理）：**
- CPG 长按 ≥ 3秒：发送 `N_S3_REBOT`
- MUTE 长按 ≥ 3秒：发送 `N_S3_ROOT`

### 3.2 代码结构

在 `handleButtons()` 中新增长按检测：

```cpp
// CPG 长按检测
if (cpgReading == LOW) {
    if (!cpgLongPressTriggered) {
        cpgLongPressStart = now;
        cpgLongPressTriggered = true;
    }
    if (!cpgLongPressSent && (now - cpgLongPressStart >= 3000)) {
        Serial1.println("N_S3_REBOT");
        flashConfirm();  // 闪烁3次确认
        cpgLongPressSent = true;
    }
}

// MUTE 长按检测
if (muteReading == LOW) {
    if (!muteLongPressTriggered) {
        muteLongPressStart = now;
        muteLongPressTriggered = true;
    }
    if (!muteLongPressSent && (now - muteLongPressStart >= 3000)) {
        Serial1.println("N_S3_ROOT");
        flashConfirm();  // 闪烁3次确认
        muteLongPressSent = true;
    }
}
```

### 3.3 视觉反馈

发送命令后，C3 底部指示灯闪烁 3 次确认：

```cpp
void flashConfirm() {
    for (int i = 0; i < 3; i++) {
        indicatorStrip.fill(indicatorStrip.Color(255, 255, 255));
        indicatorStrip.show();
        delay(100);
        indicatorStrip.clear();
        indicatorStrip.show();
        delay(100);
    }
}
```

## 4. S3 改动

### 4.1 删除原有 LOGO 按键逻辑

删除以下代码：
- LOGO 长按 2-8秒 重启逻辑
- LOGO 长按 > 8秒 刷机模式逻辑
- 相关状态变量 `lastState[1][6]` 检测

### 4.2 新增 Serial1 命令处理

在 Serial1 处理部分添加：

```cpp
if (cmd == "N_S3_REBOT") {
    Serial.println("C3 触发 S3 重启");
    delay(100);
    esp_restart();
}
else if (cmd == "N_S3_ROOT") {
    Serial.println("C3 触发 S3 进入刷机模式");
    delay(100);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
```

## 5. 文件变更

| 文件 | 改动 |
|------|------|
| `fireware/c3/c3.ino` | 增加 CPG/MUTE 长按检测，发送 N_S3_REBOT/N_S3_ROOT |
| `fireware/s3/s3-audio.ino` | 删除 LOGO 长按逻辑，新增 Serial1 接收 N_S3_* 命令 |

## 6. 测试验证

### 6.1 测试用例

| 测试 | 预期结果 |
|------|---------|
| CPG 短按 < 3秒 | 切换 CPG 灯效，S3 不重启 |
| CPG 长按 ≥ 3秒 | S3 重启，C3 指示灯闪烁3次 |
| MUTE 短按 < 3秒 | 切换音量模式，S3 不重启 |
| MUTE 长按 ≥ 3秒 | S3 进入刷机模式，C3 指示灯闪烁3次 |
| S3 键盘扫描正常时测试 | C3 命令仍能正常触发 S3 重启 |

### 6.2 边界条件

- 按下后立即松开（< 100ms）：不触发任何功能
- 长按后不松开：只触发一次，不会重复发送
- 快速连按：每次短按都正常响应短按功能

## 7. 设计约束

1. 长按阈值：3000ms
2. 命令格式：`N_S3_XXX` 前缀明确标识目标设备
3. 视觉反馈：命令发送后闪烁 3 次
4. 防抖处理：长按只触发一次，释放后重置状态
