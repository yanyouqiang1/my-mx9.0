# Task 6: 实现通知系统

## Goal
实现ALERT/NOTIFY指令和LVGL通知显示。

## 通知类型
```cpp
enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };

#define MAX_NOTIFS 8
static uint8_t notifCount = 0;
static AlertType notifQueue[MAX_NOTIFS];
static char notifTexts[MAX_NOTIFS][128];
```

## 实现
```cpp
static void pushNotification(AlertType type, const String& text);

static void drawNotifPanel(void) {
    if (notifCount == 0) return;
    // 显示最新通知
}
```

## BLE指令扩展
```cpp
// ALERT:RED[:text]
// ALERT:GREEN[:text]
// ALERT:YELLOW[:text]
// NOTIFY:text
// ALERT:OFF / CLEAR
```

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功：`pio run -e esp32-s3-devkitc-1`

## 依赖
- Task 1, 2
