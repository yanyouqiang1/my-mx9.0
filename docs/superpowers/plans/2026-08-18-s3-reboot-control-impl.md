# S3 重启/刷机控制迁移至 C3 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 S3 的重启/刷机控制迁移到 C3，实现更可靠的远程控制

**Architecture:** C3 检测 CPG/MUTE 长按 3 秒，通过 UART 发送 N_S3_REBOT/N_S3_ROOT 命令给 S3，S3 执行重启或进入刷机模式

**Tech Stack:** Arduino-ESP32, ESP-IDF

## Global Constraints

- 长按阈值: 3000ms
- 命令格式: `N_S3_REBOT`, `N_S3_ROOT`
- 视觉反馈: 发送命令后闪烁 3 次
- 防抖: 长按只触发一次，释放后重置状态

---

## File Structure

```
fireware/
├── c3/
│   └── c3.ino     # 修改: 增加长按检测，发送 N_S3_* 命令
└── s3/
    └── s3-audio.ino  # 修改: 删除 LOGO 长按逻辑，增加 Serial1 处理
```

---

## Task Map

| Task | 内容 | 文件 |
|------|------|------|
| 1 | 修改 C3 增加长按检测和命令发送 | fireware/c3/c3.ino |
| 2 | 修改 S3 删除 LOGO 逻辑，增加 Serial1 处理 | fireware/s3/s3-audio.ino |

---

## Task 1: 修改 C3 增加长按检测和命令发送

**Files:**
- Modify: `fireware/c3/c3.ino`

**Interfaces:**
- Produces: C3 发送 `N_S3_REBOT` 和 `N_S3_ROOT` 命令
- Consumes: C3 的物理按键输入 (CPG GPIO1, MUTE GPIO2)

- [ ] **Step 1: 添加长按检测变量**

在文件顶部变量声明区域添加:

```cpp
// ================= S3 控制长按检测 =================
unsigned long cpgLongPressStart = 0;
unsigned long muteLongPressStart = 0;
bool cpgLongPressSent = false;
bool muteLongPressSent = false;
```

- [ ] **Step 2: 添加 flashConfirm() 闪烁确认函数**

在现有函数之后添加:

```cpp
// ================= S3 控制命令发送确认闪烁 =================
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

- [ ] **Step 3: 修改 CPG 按键处理逻辑**

在 `handleButtons()` 函数中，找到 CPG 按键处理部分（约在 357-392 行），修改为:

```cpp
// CPG 按键处理
bool cpgReading = digitalRead(PIN_CPG);
if (cpgReading != lastCpgState) {
    lastCpgTime = now;
    lastCpgState = cpgReading;
}
if ((now - lastCpgTime) > 50) {
    static bool cpgTriggered = false;
    if (cpgReading == LOW && !cpgTriggered) {
        cpgTriggered = true;
        cpgLongPressStart = now;
        cpgLongPressSent = false;

        // 【物理唤醒】
        if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
            Serial.println("强关解除：唤醒灯光系统");
        }

        exitSystemState();
        if (currentMode != MODE_CPG) {
            currentMode = MODE_CPG;
            Serial.println("模式切换: CPG模式");
        } else {
            currentEffect = (currentEffect + 1) % MAX_EFFECTS;
            if (currentEffect == 0) currentEffect = 1;
            Serial.printf("CPG模式内切换灯效: %d\n", currentEffect);
        }

        updateIndicators();
        effectFrame = 0;
        g_forceRedraw = true;
    } else if (cpgReading == HIGH) {
        if (cpgTriggered) {
            // 检查是否长按 >= 3秒
            if (!cpgLongPressSent && (now - cpgLongPressStart >= 3000)) {
                Serial1.println("N_S3_REBOT");
                flashConfirm();
                Serial.println("已发送 N_S3_REBOT 给 S3");
            }
            cpgTriggered = false;
            cpgLongPressSent = false;
        }
    }
}
```

- [ ] **Step 4: 修改 MUTE 按键处理逻辑**

在 `handleButtons()` 函数中，找到 MUTE 按键处理部分（约在 277-303 行），修改为:

```cpp
// 静音按键 -> 切换音量控制模式
bool muteReading = digitalRead(PIN_MUTE);
if (muteReading != lastMuteState) {
    lastMuteTime = now;
    lastMuteState = muteReading;
}
if ((now - lastMuteTime) > 50) {
    static bool muteTriggered = false;
    if (muteReading == LOW && !muteTriggered) {
        muteTriggered = true;
        muteLongPressStart = now;
        muteLongPressSent = false;

        // 【物理唤醒】：如果在强关状态，任何按键点击都将其唤醒并切入该模式
        if (g_forceOff) {
            g_forceOff = false;
            currentEffect = 1;
            g_forceRedraw = true;
            Serial.println("强关解除：唤醒灯光系统");
        }

        exitSystemState();
        currentMode = MODE_MUTE;
        updateIndicators();
        Serial.println("模式切换: 控制音量");
    } else if (muteReading == HIGH) {
        if (muteTriggered) {
            // 检查是否长按 >= 3秒
            if (!muteLongPressSent && (now - muteLongPressStart >= 3000)) {
                Serial1.println("N_S3_ROOT");
                flashConfirm();
                Serial.println("已发送 N_S3_ROOT 给 S3");
            }
            muteTriggered = false;
            muteLongPressSent = false;
        }
    }
}
```

- [ ] **Step 5: 提交**

```bash
git add fireware/c3/c3.ino
git commit -m "feat(c3): add long-press detection for S3 reboot control"
```

---

## Task 2: 修改 S3 删除 LOGO 逻辑，增加 Serial1 处理

**Files:**
- Modify: `fireware/s3/s3-audio.ino`

**Interfaces:**
- Consumes: C3 发送的 `N_S3_REBOT`, `N_S3_ROOT` 命令
- Produces: S3 执行重启或进入刷机模式

- [ ] **Step 1: 在 loop() 中添加 N_S3_* 命令处理**

在 `loop()` 函数中，找到 Serial1 处理部分（处理 V+/V- 的地方），在同一个 while 循环后添加:

```cpp
// 处理来自 C3 的 S3 控制命令
while (Serial1.available()) {
    String cmd = Serial1.readStringUntil('\n');
    cmd.trim();
    if (cmd == "V+") {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
        ConsumerControl.release();
    } else if (cmd == "V-") {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
        ConsumerControl.release();
    }
    else if (cmd == "N_S3_REBOT") {
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
}
```

- [ ] **Step 2: 删除 LOGO 长按逻辑**

找到 `scanKeyboardMatrix()` 函数中关于 LOGO 按键的处理（约在 334-350 行），删除以下代码:

```cpp
else if (keycode == K_LOGO) {
    unsigned long pressedDuration = millis() - lastDebounceTime[r][c];

    if (pressedDuration >= 2000 && pressedDuration < 8000) {
        Serial1.println("N_REBOT");
        Serial.println("检测到长按 LOGO 2S~8S 释放，发送 N_REBOT 普通重启...");
        delay(200);
        esp_restart();
    }
    else if (pressedDuration < 2000) {
        cherryLogoEnabled = !cherryLogoEnabled;
        preferences.putBool("cherryLogo", cherryLogoEnabled);
        Serial.printf("CHERRY_LOGO 状态已切换为: %s\n", cherryLogoEnabled ? "开启 (ON)" : "关闭 (OFF)");

        executeMacro("LOGO"); // 正常执行原有的 LOGO 宏代码
    }
}
```

同时删除 loop() 中检测 LOGO 8秒强制刷机的代码（约在 442-460 行）:

```cpp
// ======== 增强功能 1：长按 LOGO (Row 1, Col 6) 达到 8 秒时，执行强制硬件 ROM 刷机重置 ========
if (lastState[1][6]) { // LOGO 状态为按下
    unsigned long pressedDuration = millis() - lastDebounceTime[1][6];
    if (pressedDuration >= 8000) {
        Serial.println("检测到长按 LOGO 达到 8 秒，强行向 C3 发送 N_ROOT 并写入寄存器复位至硬件 ROM 刷机模式...");

        for (int k = 0; k < 15; k++) {
            Serial1.println("N_ROOT");
            delay(100);
        }

        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }
}
// =======================================================================================
```

- [ ] **Step 3: 提交**

```bash
git add fireware/s3/s3-audio.ino
git commit -m "feat(s3): remove LOGO long-press logic, add N_S3_* serial command handling"
```

---

## Self-Review Checklist

1. **Spec coverage:**
   - [x] C3 CPG 长按 3秒 发送 N_S3_REBOT
   - [x] C3 MUTE 长按 3秒 发送 N_S3_ROOT
   - [x] C3 闪烁 3 次确认
   - [x] S3 删除 LOGO 长按逻辑
   - [x] S3 新增 Serial1 接收 N_S3_* 命令

2. **Placeholder scan:** 无 TBD/TODO

3. **Type consistency:**
   - C3 发送命令: `Serial1.println("N_S3_REBOT")`
   - S3 接收命令: `cmd == "N_S3_REBOT"`
   - 命令格式一致 ✓

---

## 依赖关系

```
Task 1 (C3) → Task 2 (S3)  (无依赖，可并行实现)
```

---

**Plan complete and saved to `docs/superpowers/plans/2026-08-18-s3-reboot-control-impl.md`**

Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
