# Task 5: 实现宏录制

## Goal
实现击键流录制和组合键录制功能。

## 宏录制模式
```cpp
#define SYS_MODE_REC_SEQ 7
#define SYS_MODE_REC_CMB 8

#define MAX_REC_KEYS 64
static uint16_t recKeyBuffer[MAX_REC_KEYS];
static int recKeyCount = 0;
static uint8_t currentRecMode = 0;  // 0=SEQ, 1=CMB
static bool recNeedsRedraw = true;
```

## 录制界面 (build_recording)
```cpp
static void build_recording(void) {
    // 标题：根据模式显示不同颜色
    // 计数：已录/上限
    // 键列表：显示已录按键
    // 提示：MR切模式/取消，M1-M12收尾
}
```

## 键盘扫描处理
```cpp
// MR键：进入录制模式
if (baseKey == K_MR && currentState) {
    currentSysMode = SYS_MODE_REC_SEQ;
    recKeyCount = 0;
    currentRecMode = 0;
    build_recording();
}

// 录制模式下
if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
    // M1-M12：收尾保存
    // MR：切换模式或取消
    // 普通按键：记录
}
```

## 保存宏
```cpp
static void finishMacroRecording(const String& targetKey);
```

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功：`pio run -e esp32-s3-devkitc-1`

## 依赖
- Task 1, 2
