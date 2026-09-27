# Task 6: 实现通知系统

## Status: DONE

## Commit
- Commit: `8dcf8cc` - feat(lvgl_s3): 实现通知系统 (ALERT/NOTIFY)

## 实现内容

### 1. AlertType 枚举和通知队列变量
```cpp
enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };

#define MAX_NOTIFS 8
static uint8_t notifCount = 0;
static AlertType notifQueue[MAX_NOTIFS];
static char notifTexts[MAX_NOTIFS][128];
```

### 2. LVGL 通知面板对象
```cpp
static lv_obj_t* scr_notif = nullptr;
static lv_obj_t* notif_bg = nullptr;
static lv_obj_t* notif_label = nullptr;
static unsigned long notifShowMs = 0;
static unsigned long notifStartMs = 0;
```

### 3. pushNotification() 函数
- 支持入队最多8条通知
- 超出容量时移除最老的通知
- 自动调用 drawNotifPanel() 更新显示

### 4. drawNotifPanel() LVGL UI
- 通知面板显示在屏幕顶部中央
- 根据通知类型显示不同颜色:
  - ALERT_RED: 红色边框 (#FF4444)，深红背景
  - ALERT_GREEN: 绿色边框 (#44FF44)，深绿背景
  - ALERT_YELLOW: 黄色边框 (#FFFF44)，深黄背景
- 显示时间: 3秒后自动移除最老通知

### 5. clearNotifications() 函数
- 清空所有通知队列

### 6. handleCommand() 扩展
支持的BLE指令:
- `NOTIFY:text` -> ALERT_GREEN
- `ALERT:RED[:text]` -> ALERT_RED
- `ALERT:GREEN[:text]` -> ALERT_GREEN
- `ALERT:YELLOW[:text]` -> ALERT_YELLOW
- `ALERT:OFF` -> 清空通知
- `CLEAR` -> 清空通知

### 7. 自动消失逻辑
在主循环中添加通知超时检查，每3秒自动移除最老的一条通知。

## 修改文件
- `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
- 用户自行烧录固件验证
- BLE发送指令测试:
  - `NOTIFY:Hello` - 显示绿色通知
  - `ALERT:RED:Error!` - 显示红色警告
  - `ALERT:YELLOW:Warning` - 显示黄色警告
  - `ALERT:OFF` - 清空所有通知
