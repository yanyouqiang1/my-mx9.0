# Task 1: Screen管理系统重构 + Bug修复

## Goal
建立Screen预创建模式，修复菜单Back重启bug。

## 问题描述
当前lvgl_s3.ino中，选择Back时调用`renderCurrentDisplayBase()`会删除并重建Screen，导致LVGL状态异常引发重启。

## 解决方案
使用Screen预创建模式：
1. 预创建所有需要的Screen（scr_main, scr_menu等）
2. 使用`lv_scr_load()`切换Screen
3. 不再使用`lv_obj_del()` + 重建的方式

## 具体实现

### 1. 添加全局变量
```cpp
// ===========================
// LVGL Screen 管理
// ===========================
static lv_obj_t* scr_main = nullptr;
static lv_obj_t* scr_menu = nullptr;
static lv_obj_t* scr_settings_time = nullptr;
static lv_obj_t* scr_settings_alarm = nullptr;
static lv_obj_t* scr_settings_timer = nullptr;
static lv_obj_t* scr_settings_caltemp = nullptr;
static lv_obj_t* scr_recording = nullptr;
static lv_obj_t* currentScreen = nullptr;

// Screen切换函数
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    if (currentScreen == target) return;
    currentScreen = target;
    lv_scr_load(target);
}
```

### 2. 修改renderCurrentDisplayBase
首次调用时创建Screen并缓存，后续调用只切换显示：
```cpp
static void renderCurrentDisplayBase(void) {
    init_styles();
    
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        // 根据currentDispMode构建内容
        if (currentDispMode == DISP_MODE_GEEK) {
            build_style_geek();
        } else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
            build_style_bigclock();
        }
    }
    
    showScreen(scr_main);
}
```

### 3. 修改build_menu
```cpp
static void build_menu(void) {
    if (scr_menu == nullptr) {
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        // 构建菜单内容
    }
    showScreen(scr_menu);
}
```

### 4. 菜单Back逻辑
```cpp
// 在scanKeyboardMatrix的菜单处理中：
} else if (menuSel == MENU_ITEMS - 1) {  // Back项
    currentSysMode = SYS_MODE_NORMAL;
    showScreen(scr_main);  // 直接切换，不重建
}
```

### 5. ESC键处理
```cpp
} else if (baseKey == KEY_ESC) {
    currentSysMode = SYS_MODE_NORMAL;
    showScreen(scr_main);  // 不重建，只切换
}
```

## 颜色定义（已有）
- CLR_BLACK: 0x000000
- CLR_WHITE: 0xFFFFFF
- CLR_CYAN: 0x00E5FF
- CLR_GRAY: 0x808080
- CLR_DARK: 0x1A1A1A

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功即可：`pio run -e esp32-s3-devkitc-1`
