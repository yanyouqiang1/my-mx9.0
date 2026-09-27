# Task 2: 扩展菜单到12项

## Goal
将3项菜单扩展到12项，与原版S3.ino对齐。

## 菜单项列表
```cpp
#define MENU_ITEMS 12

static const char* menuItemsCN[MENU_ITEMS] = {
    "1. 返回主屏",
    "2. 切换主屏风格",
    "3. 切换配置方案",
    "4. 按键回显开关",
    "5. 键盘背光灯效",
    "6. 状态灯亮度",
    "7. 设置时间",
    "8. 闹钟设置",
    "9. 倒计时",
    "10. 刷新温湿度",
    "11. 温度校准",
    "12. 计数清零"
};
```

## 实现要求

### 1. 更新全局变量
在lvgl_s3.ino中添加：
- MENU_ITEMS = 12
- menuItemsCN[12] 数组

### 2. 重写build_menu函数
- 支持12项菜单
- 使用可滚动列表（可见6项）
- 顶部显示标题 + 位置指示
- 选中项高亮显示

### 3. 实现handleMenuSelect函数
```cpp
static void handleMenuSelect(void) {
    switch (menuSel) {
        case 0:  // 返回主屏 - 使用showScreen(scr_main)
        case 1:  // 切换主屏风格 - 循环切换6种风格
        case 2:  // 切换配置方案 - 循环切换4种方案
        case 3:  // 按键回显开关 - toggle showKeystrokes
        case 4:  // 键盘背光灯效 - 循环切换灯效
        case 5:  // 状态灯亮度 - 循环切换4档亮度
        case 6:  // 设置时间 - showScreen(scr_settings_time)
        case 7:  // 闹钟设置 - showScreen(scr_settings_alarm)
        case 8:  // 倒计时 - showScreen(scr_settings_timer)
        case 9:  // 刷新温湿度 - 手动触发读取
        case 10: // 温度校准 - showScreen(scr_settings_caltemp)
        case 11: // 计数清零 - 清零并显示HUD
    }
}
```

### 4. 更新键盘扫描中的菜单处理
- 上下键/左右键移动光标
- 回车键选择
- ESC键返回主屏（使用showScreen）

### 5. 需要添加的辅助变量
```cpp
static bool showKeystrokes = true;

// 灯效相关
static const char* effectNames[] = {
    "关闭", "纯红", "纯绿", "纯蓝", "冰蓝", "纯白",
    "红呼吸", "绿呼吸", "蓝呼吸", "冰蓝呼吸", "流光", "彗星", "幻彩"
};
#define MAX_EFFECTS 13

// 状态灯亮度
#define IND_LEVEL_COUNT 4
static const char* indLevelNames[] = { "关", "低", "中", "高" };
static const uint8_t indLevelValues[] = { 0, 64, 140, 255 };
static uint8_t indLevel = 3;
static uint8_t indBrightness = 255;
```

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功：`pio run -e esp32-s3-devkitc-1`

## 依赖
- Task 1完成的Screen管理系统（showScreen函数）
