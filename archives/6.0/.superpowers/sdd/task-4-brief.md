# Task 4: 实现设置界面

## Goal
实现时间设置、闹钟设置、倒计时、温度校准四个设置界面。

## 设置界面定义
```cpp
#define SYS_MODE_SET_TIME     3
#define SYS_MODE_SET_ALARM    4
#define SYS_MODE_SET_TIMER    5
#define SYS_MODE_CAL_TEMP     6

#define IS_SETTING_MODE(m) ((m) >= SYS_MODE_SET_TIME && (m) <= SYS_MODE_CAL_TEMP)
```

## 全局变量
```cpp
// 时间设置
static int timeEditY = 2026, timeEditMo = 1, timeEditD = 1, timeEditH = 0, timeEditMi = 0;
static int timeFieldIdx = 0;

// 闹钟设置
static bool alarmEditOn = false;
static int alarmEditH = 7, alarmEditM = 0;
static int alarmFieldIdx = 0;
static bool alarmEnabled = false;
static uint8_t alarmHour = 7, alarmMinute = 0;

// 倒计时设置
static int timerEditH = 0, timerEditM = 5, timerEditS = 0;
static int timerFieldIdx = 0;
static bool timerRunning = false;
static unsigned long timerStartMs = 0;
static uint32_t timerTotalSec = 0;
static uint32_t timerRemainSec = 0;

// 温度校准
static float calTempOriginal = 62.0f;
static int calTempField = 0;
```

## 界面实现

### 1. 时间设置 (build_settings_time)
- 显示：年-月-日 / 时:分
- 字段：年/月/日/时/分（5个字段）
- 操作：左右切换字段，上下调整值
- 底部：提示文字

### 2. 闹钟设置 (build_settings_alarm)
- 显示：时:分
- 开关：开关按钮
- 底部：提示文字

### 3. 倒计时设置 (build_settings_timer)
- 显示：时:分:秒
- 按钮：开始/停止
- 底部：提示文字

### 4. 温度校准 (build_settings_caltemp)
- 显示：当前温度（大字）
- 偏移值调整
- 保存按钮

## 输入处理
在scanKeyboardMatrix中添加：
```cpp
if (IS_SETTING_MODE(currentSysMode)) {
    if (baseKey == KEY_LEFT_ARROW) moveSettingField(-1);
    else if (baseKey == KEY_RIGHT_ARROW) moveSettingField(1);
    else if (baseKey == KEY_UP_ARROW) adjustSettingField(1);
    else if (baseKey == KEY_DOWN_ARROW) adjustSettingField(-1);
    else if (baseKey == KEY_RETURN) saveSettingScreen();
    else if (baseKey == KEY_ESC) cancelSettingScreen();
    return;
}
```

## 保存和取消
```cpp
static void saveSettingScreen(void);
static void cancelSettingScreen(void);
```

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功：`pio run -e esp32-s3-devkitc-1`

## 依赖
- Task 1, 2, 3
