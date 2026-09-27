# YYQ-MX9.0 LVGL 功能对齐实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将lvgl_s3.ino对齐到原版S3.ino的全部功能，包括6种显示风格、12项菜单、设置界面、宏录制、通知系统，并修复菜单Back重启bug。

**Architecture:** 混合架构 - UI核心用LVGL实现，高级功能复用S3.ino逻辑。采用Screen预创建模式避免重建导致的bug。

**Tech Stack:** ESP32-S3, LVGL, Arduino, PlatformIO

## Global Constraints

- 屏幕分辨率：240x240 (ST7789)
- 键盘矩阵：10x16
- 颜色格式：LVGL 32-bit color
- 宏录制最大按键数：64
- 温湿度读取间隔：15分钟

---

## 文件结构

```
lvgl_s3/src/
├── lvgl_s3.ino          # 主程序（需大规模重构）
├── lv_conf.h             # LVGL配置（已有）
├── lvgl_st7789_driver.cpp/h  # 驱动（已有）
└── lv_font_simsun_16_cjk.c   # 中文字体（已有）
```

---

## Task 1: Screen管理系统重构 + Bug修复

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 建立Screen预创建模式，修复菜单Back重启问题

**Interfaces:**
- Produces: `showScreen(lv_obj_t*)`, `initAllScreens()`, `scr_main`, `scr_menu`

- [ ] **Step 1: 添加Screen全局变量**

在文件顶部全局变量区域添加：

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

// 当前活动的Screen
static lv_obj_t* currentScreen = nullptr;

// Screen切换函数（替代直接删除重建）
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    if (currentScreen == target) return;  // 已在目标Screen
    
    currentScreen = target;
    lv_scr_load(target);
}
```

- [ ] **Step 2: 创建showScreen函数实现**

```cpp
// 显示指定Screen
static void showScreen(lv_obj_t* target) {
    if (target == nullptr) return;
    if (currentScreen == target) return;
    currentScreen = target;
    lv_scr_load(target);
}
```

- [ ] **Step 3: 修改renderCurrentDisplayBase使用Screen管理**

将现有实现改为只初始化一次，后续切换使用showScreen：

```cpp
static void renderCurrentDisplayBase(void) {
    init_styles();
    
    // 首次初始化：创建Screen
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        
        if (currentDispMode == DISP_MODE_GEEK) {
            build_style_geek();
        } else if (currentDispMode == DISP_MODE_BIG_CLOCK) {
            build_style_bigclock();
        }
        // ... 其他风格添加到这里
    }
    
    // 切换到主Screen
    showScreen(scr_main);
}
```

- [ ] **Step 4: 修改build_menu函数使用Screen管理**

```cpp
static void build_menu(void) {
    if (scr_menu == nullptr) {
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
        
        // 菜单标题
        lv_obj_t* title = lv_label_create(scr_menu);
        lv_label_set_text(title, "MENU");
        // ... 完整实现
    }
    showScreen(scr_menu);
}
```

- [ ] **Step 5: 修改菜单选择逻辑（Back不重启）**

```cpp
// 在scanKeyboardMatrix的菜单处理部分，修改Back逻辑：
} else if (menuSel == MENU_ITEMS - 1) {  // Back项
    currentSysMode = SYS_MODE_NORMAL;
    showScreen(scr_main);  // 直接切换，不重建
}
```

- [ ] **Step 6: 验证编译**

运行: `pio run -e esp32-s3-devkitc-1`
预期: 编译成功

- [ ] **Step 7: 提交**

```bash
git add lvgl_s3/src/lvgl_s3.ino
git commit -m "fix(lvgl_s3): Screen预创建模式，修复菜单Back重启bug"
```

---

## Task 2: 扩展菜单到12项

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 将3项菜单扩展到12项，与原版S3.ino对齐

**Interfaces:**
- Consumes: `menuSel`, `MENU_ITEMS`
- Produces: `build_menu()`, `handleMenuSelect()`

- [ ] **Step 1: 更新菜单项数量和数组**

```cpp
// 菜单
static uint8_t menuSel = 0;
#define MENU_ITEMS 12

// 菜单项名称（中英文混合）
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

- [ ] **Step 2: 重写build_menu支持12项**

```cpp
static void build_menu(void) {
    if (scr_menu == nullptr) {
        scr_menu = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_menu, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        // Screen已存在，清除旧内容
        lv_obj_clean(scr_menu);
    }
    
    // 标题
    lv_obj_t* title = lv_label_create(scr_menu);
    lv_label_set_text(title, "YYQ 键盘系统 OS");
    lv_obj_set_style_text_font(title, &lv_font_simsun_16_cjk, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 位置指示
    char posBuf[16];
    snprintf(posBuf, sizeof(posBuf), "%d/%d", menuSel + 1, MENU_ITEMS);
    lv_obj_t* pos = lv_label_create(scr_menu);
    lv_label_set_text(pos, posBuf);
    lv_obj_set_style_text_color(pos, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(pos, LV_ALIGN_TOP_LEFT, 10, 10);
    
    // 菜单项列表（可滚动）
    const int ITEM_H = 30;
    const int START_Y = 40;
    const int VISIBLE = 6;
    
    int startIdx = (menuSel >= VISIBLE) ? (menuSel - VISIBLE + 1) : 0;
    
    for (int i = 0; i < VISIBLE && (startIdx + i) < MENU_ITEMS; i++) {
        int idx = startIdx + i;
        bool selected = (idx == menuSel);
        
        lv_obj_t* btn = lv_btn_create(scr_menu);
        lv_obj_set_size(btn, 200, ITEM_H - 4);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, START_Y + i * ITEM_H);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, 
            selected ? lv_color_hex(CLR_CYAN) : lv_color_hex(CLR_DARK), 
            LV_PART_MAIN);
        
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, menuItemsCN[idx]);
        lv_obj_set_style_text_font(lbl, &lv_font_simsun_16_cjk, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl,
            selected ? lv_color_hex(CLR_BLACK) : lv_color_hex(CLR_WHITE),
            LV_PART_MAIN);
        lv_obj_center(lbl);
    }
    
    showScreen(scr_menu);
}
```

- [ ] **Step 3: 实现handleMenuSelect**

```cpp
static void handleMenuSelect(void) {
    switch (menuSel) {
        case 0:  // 返回主屏
            currentSysMode = SYS_MODE_NORMAL;
            showScreen(scr_main);
            break;
        case 1:  // 切换主屏风格
            currentDispMode = (currentDispMode + 1) % TOTAL_DISP_MODES;
            preferences.putUChar("disp_mode", currentDispMode);
            triggerHud("显示风格", dispModeNames[currentDispMode], lv_color_hex(CLR_CYAN));
            build_menu();
            break;
        case 2:  // 切换配置方案
            switchProfile((currentProfile + 1) % TOTAL_PROFILES);
            build_menu();
            break;
        case 3:  // 按键回显开关
            // toggle showKeystrokes
            build_menu();
            break;
        case 4:  // 键盘背光灯效
            currentEffect = (currentEffect % MAX_EFFECTS) + 1;
            triggerHud("灯效", effectNames[currentEffect], lv_color_hex(CLR_MAGENTA));
            build_menu();
            break;
        case 5:  // 状态灯亮度
            indLevel = (indLevel + 1) % IND_LEVEL_COUNT;
            indBrightness = indLevelValues[indLevel];
            preferences.putUChar("ind_level", indLevel);
            triggerHud("状态灯", indLevelNames[indLevel], lv_color_hex(CLR_WHITE));
            build_menu();
            break;
        case 6:  // 设置时间
            showScreen(scr_settings_time);
            break;
        case 7:  // 闹钟设置
            showScreen(scr_settings_alarm);
            break;
        case 8:  // 倒计时
            showScreen(scr_settings_timer);
            break;
        case 9:  // 刷新温湿度
            if (shtAvailable) {
                sht31_update();
                lastSHTRead = millis();
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1fC  %.0f%%", shtTemp, shtHumidity);
                triggerHud("温湿度", buf, lv_color_hex(CLR_CYAN));
            }
            build_menu();
            break;
        case 10: // 温度校准
            showScreen(scr_settings_caltemp);
            break;
        case 11: // 计数清零
            totalKeyCount = 0;
            preferences.putUInt("keyCount", 0);
            triggerHud("计数", "已清零", lv_color_hex(CLR_GREEN));
            build_menu();
            break;
    }
}
```

- [ ] **Step 4: 更新键盘扫描中的菜单处理**

确保ESC键返回主屏不重启：

```cpp
// 菜单模式下的处理
if (currentSysMode == SYS_MODE_MENU) {
    if (baseKey == KEY_DOWN_ARROW || baseKey == KEY_RIGHT_ARROW) {
        menuSel = (menuSel + 1) % MENU_ITEMS;
        build_menu();
    } else if (baseKey == KEY_UP_ARROW || baseKey == KEY_LEFT_ARROW) {
        menuSel = (menuSel + MENU_ITEMS - 1) % MENU_ITEMS;
        build_menu();
    } else if (baseKey == KEY_RETURN) {
        handleMenuSelect();
    } else if (baseKey == KEY_ESC) {
        currentSysMode = SYS_MODE_NORMAL;
        showScreen(scr_main);  // 不重建，只切换
    }
    return;
}
```

- [ ] **Step 5: 验证编译**

运行: `pio run -e esp32-s3-devkitc-1`
预期: 编译成功

- [ ] **Step 6: 提交**

```bash
git add lvgl_s3/src/lvgl_s3.ino
git commit -m "feat(lvgl_s3): 扩展菜单到12项"
```

---

## Task 3: 添加6种显示风格

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 实现全部6种显示风格

**Interfaces:**
- Consumes: `currentDispMode`, `TOTAL_DISP_MODES`
- Produces: `build_style_geek()`, `build_style_bigclock()`, `build_style_info_panel()`, `build_style_keymon()`, `build_style_rhythm()`, `build_style_wallpaper()`

- [ ] **Step 1: 添加显示模式常量**

```cpp
// 显示模式
#define DISP_MODE_GEEK        0
#define DISP_MODE_BIG_CLOCK   1
#define DISP_MODE_INFO_PANEL  2
#define DISP_MODE_KEY_MON     3
#define DISP_MODE_RHYTHM      4
#define DISP_MODE_WALLPAPER    5
#define TOTAL_DISP_MODES       6

static uint8_t currentDispMode = DISP_MODE_GEEK;
static uint8_t previewDispMode = DISP_MODE_GEEK;

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸"
};
```

- [ ] **Step 2: 添加极客仪表盘LVGL组件变量**

```cpp
// 极客仪表盘
static lv_obj_t* gk_bg = nullptr;
static lv_obj_t* gk_lbl_clock = nullptr;
static lv_obj_t* gk_lbl_date = nullptr;
static lv_obj_t* gk_lbl_temp = nullptr;
static lv_obj_t* gk_lbl_hum = nullptr;
static lv_obj_t* gk_lbl_keys = nullptr;
static lv_obj_t* gk_led_num = nullptr;
static lv_obj_t* gk_led_caps = nullptr;
static lv_obj_t* gk_led_scr = nullptr;
static lv_obj_t* gk_lbl_profile = nullptr;
```

- [ ] **Step 3: 添加其他风格的组件变量**

```cpp
// 大字时钟
static lv_obj_t* bc_bg = nullptr;
static lv_obj_t* bc_lbl_time = nullptr;
static lv_obj_t* bc_lbl_date = nullptr;

// 信息面板
static lv_obj_t* ip_bg = nullptr;
static lv_obj_t* ip_lbl_clock = nullptr;
static lv_obj_t* ip_lbl_date = nullptr;
static lv_obj_t* ip_circle_num = nullptr;
static lv_obj_t* ip_circle_caps = nullptr;
static lv_obj_t* ip_circle_scr = nullptr;
static lv_obj_t* ip_lbl_profile = nullptr;
static lv_obj_t* ip_lbl_keys = nullptr;
static lv_obj_t* ip_lbl_lastkey = nullptr;

// 击键监控
static lv_obj_t* km_bg = nullptr;
static lv_obj_t* km_lbl_title = nullptr;
static lv_obj_t* km_lbl_lastkey = nullptr;
static lv_obj_t* km_lbl_keys = nullptr;
static lv_obj_t* km_lbl_profile = nullptr;

// 律动
static lv_obj_t* rh_bg = nullptr;
static lv_obj_t* rh_cont_bars = nullptr;
static lv_obj_t* rh_lbl_keys = nullptr;
static uint8_t rhythmBars[24];

// 壁纸
static lv_obj_t* wp_bg = nullptr;
static lv_obj_t* wp_img = nullptr;
static lv_obj_t* wp_lbl_time = nullptr;
```

- [ ] **Step 4: 实现信息面板 build_style_info_panel()**

```cpp
static void build_style_info_panel(void) {
    if (ip_bg) { lv_obj_del(ip_bg); ip_bg = nullptr; }
    
    ip_bg = lv_obj_create(scr_main);
    lv_obj_set_size(ip_bg, 240, 240);
    lv_obj_set_pos(ip_bg, 0, 0);
    lv_obj_set_style_bg_color(ip_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    
    // 时钟和日期（上区）
    ip_lbl_clock = lv_label_create(ip_bg);
    lv_label_set_text(ip_lbl_clock, "--:--");
    lv_obj_set_style_text_font(ip_lbl_clock, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_clock, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(ip_lbl_clock, LV_ALIGN_TOP_LEFT, 10, 10);
    
    ip_lbl_date = lv_label_create(ip_bg);
    lv_label_set_text(ip_lbl_date, "--/-- --");
    lv_obj_set_style_text_font(ip_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_date, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(ip_lbl_date, LV_ALIGN_TOP_RIGHT, -10, 10);
    
    // 分隔线
    lv_obj_t* line = lv_line_create(ip_bg);
    static lv_point_t pts[] = {{0, 60}, {240, 60}};
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x333333), LV_PART_MAIN);
    
    // 三格锁状态（中区）
    const int circleY = 120;
    const int circleR = 25;
    
    // NUM
    ip_circle_num = lv_obj_create(ip_bg);
    lv_obj_set_size(ip_circle_num, 60, 60);
    lv_obj_align(ip_circle_num, LV_ALIGN_TOP_LEFT, 20, circleY);
    lv_obj_set_style_radius(ip_circle_num, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ip_circle_num, lv_color_hex(numLock ? 0x00FF00 : CLR_DARK), LV_PART_MAIN);
    lv_obj_t* lbl = lv_label_create(ip_circle_num);
    lv_label_set_text(lbl, "N");
    lv_obj_set_style_text_color(lbl, lv_color_hex(numLock ? CLR_BLACK : CLR_GRAY), LV_PART_MAIN);
    lv_obj_center(lbl);
    
    // CAPS
    ip_circle_caps = lv_obj_create(ip_bg);
    lv_obj_set_size(ip_circle_caps, 60, 60);
    lv_obj_align(ip_circle_caps, LV_ALIGN_TOP_MID, 0, circleY);
    lv_obj_set_style_radius(ip_circle_caps, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ip_circle_caps, lv_color_hex(capsLock ? 0x00FFFF : CLR_DARK), LV_PART_MAIN);
    lbl = lv_label_create(ip_circle_caps);
    lv_label_set_text(lbl, "C");
    lv_obj_set_style_text_color(lbl, lv_color_hex(capsLock ? CLR_BLACK : CLR_GRAY), LV_PART_MAIN);
    lv_obj_center(lbl);
    
    // SCR
    ip_circle_scr = lv_obj_create(ip_bg);
    lv_obj_set_size(ip_circle_scr, 60, 60);
    lv_obj_align(ip_circle_scr, LV_ALIGN_TOP_RIGHT, -20, circleY);
    lv_obj_set_style_radius(ip_circle_scr, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ip_circle_scr, lv_color_hex(scrollLock ? 0xFFFF00 : CLR_DARK), LV_PART_MAIN);
    lbl = lv_label_create(ip_circle_scr);
    lv_label_set_text(lbl, "S");
    lv_obj_set_style_text_color(lbl, lv_color_hex(scrollLock ? CLR_BLACK : CLR_GRAY), LV_PART_MAIN);
    lv_obj_center(lbl);
    
    // 分隔线
    line = lv_line_create(ip_bg);
    static lv_point_t pts2[] = {{0, 190}, {240, 190}};
    lv_line_set_points(line, pts2, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x333333), LV_PART_MAIN);
    
    // 方案和统计（下区）
    ip_lbl_profile = lv_label_create(ip_bg);
    lv_label_set_text(ip_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_set_style_text_color(ip_lbl_profile, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(ip_lbl_profile, LV_ALIGN_BOTTOM_LEFT, 10, -30);
    
    ip_lbl_keys = lv_label_create(ip_bg);
    lv_label_set_text_fmt(ip_lbl_keys, "%u", totalKeyCount);
    lv_obj_set_style_text_font(ip_lbl_keys, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ip_lbl_keys, lv_color_hex(CLR_GREEN), LV_PART_MAIN);
    lv_obj_align(ip_lbl_keys, LV_ALIGN_BOTTOM_MID, 0, -30);
    
    ip_lbl_lastkey = lv_label_create(ip_bg);
    lv_label_set_text(ip_lbl_lastkey, "-");
    lv_obj_set_style_text_color(ip_lbl_lastkey, lv_color_hex(CLR_YELLOW), LV_PART_MAIN);
    lv_obj_align(ip_lbl_lastkey, LV_ALIGN_BOTTOM_RIGHT, -10, -30);
}
```

- [ ] **Step 5: 实现击键监控 build_style_keymon()**

```cpp
static void build_style_keymon(void) {
    if (km_bg) { lv_obj_del(km_bg); km_bg = nullptr; }
    
    km_bg = lv_obj_create(scr_main);
    lv_obj_set_size(km_bg, 240, 240);
    lv_obj_set_style_bg_color(km_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    
    // 标题栏
    km_lbl_title = lv_label_create(km_bg);
    lv_label_set_text(km_lbl_title, "实时击键监控");
    lv_obj_set_style_text_font(km_lbl_title, &lv_font_simsun_16_cjk, LV_PART_MAIN);
    lv_obj_set_style_text_color(km_lbl_title, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
    lv_obj_align(km_lbl_title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 最近按键显示
    km_lbl_lastkey = lv_label_create(km_bg);
    lv_label_set_text(km_lbl_lastkey, "-");
    lv_obj_set_style_text_font(km_lbl_lastkey, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(km_lbl_lastkey, lv_color_hex(CLR_GREEN), LV_PART_MAIN);
    lv_obj_align(km_lbl_lastkey, LV_ALIGN_CENTER, 0, -20);
    
    // 统计
    km_lbl_keys = lv_label_create(km_bg);
    lv_label_set_text_fmt(km_lbl_keys, "总计: %u", totalKeyCount);
    lv_obj_set_style_text_color(km_lbl_keys, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(km_lbl_keys, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    
    km_lbl_profile = lv_label_create(km_bg);
    lv_label_set_text(km_lbl_profile, profileNamesCN[currentProfile]);
    lv_obj_set_style_text_color(km_lbl_profile, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(km_lbl_profile, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
}
```

- [ ] **Step 6: 实现律动效果 build_style_rhythm()**

```cpp
static void build_style_rhythm(void) {
    if (rh_bg) { lv_obj_del(rh_bg); rh_bg = nullptr; }
    
    rh_bg = lv_obj_create(scr_main);
    lv_obj_set_size(rh_bg, 240, 240);
    lv_obj_set_style_bg_color(rh_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    
    // 初始化频谱柱
    memset(rhythmBars, 0, sizeof(rhythmBars));
    
    // 频谱容器
    rh_cont_bars = lv_obj_create(rh_bg);
    lv_obj_set_size(rh_cont_bars, 240, 150);
    lv_obj_set_pos(rh_cont_bars, 0, 0);
    lv_obj_set_style_bg_color(rh_cont_bars, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    
    // 创建24根频谱柱
    for (int i = 0; i < 24; i++) {
        lv_obj_t* bar = lv_obj_create(rh_cont_bars);
        lv_obj_set_size(bar, 8, 4);
        lv_obj_set_pos(bar, i * 10 + 1, 146);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    }
    
    // 统计
    rh_lbl_keys = lv_label_create(rh_bg);
    lv_label_set_text_fmt(rh_lbl_keys, "%u", totalKeyCount);
    lv_obj_set_style_text_font(rh_lbl_keys, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(rh_lbl_keys, lv_color_hex(CLR_YELLOW), LV_PART_MAIN);
    lv_obj_align(rh_lbl_keys, LV_ALIGN_BOTTOM_MID, 0, -10);
}
```

- [ ] **Step 7: 实现壁纸模式 build_style_wallpaper()**

```cpp
static void build_style_wallpaper(void) {
    if (wp_bg) { lv_obj_del(wp_bg); wp_bg = nullptr; }
    
    wp_bg = lv_obj_create(scr_main);
    lv_obj_set_size(wp_bg, 240, 240);
    lv_obj_set_style_bg_color(wp_bg, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    
    // 尝试加载logo.bin
    if (FFat.exists("/logo.bin")) {
        File f = FFat.open("/logo.bin", FILE_READ);
        if (f) {
            // 使用ImgeButton或手动绘制像素
            // 简化：直接用黑色背景+时钟
            f.close();
        }
    }
    
    // 时钟叠加
    wp_lbl_time = lv_label_create(wp_bg);
    lv_label_set_text(wp_lbl_time, "--:--");
    lv_obj_set_style_text_font(wp_lbl_time, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(wp_lbl_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(wp_lbl_time, LV_ALIGN_BOTTOM_MID, 0, -20);
}
```

- [ ] **Step 8: 修改renderCurrentDisplayBase支持所有风格**

```cpp
static void renderCurrentDisplayBase(void) {
    init_styles();
    
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_main);
    }
    
    switch (currentDispMode) {
        case DISP_MODE_GEEK:
            build_style_geek();
            break;
        case DISP_MODE_BIG_CLOCK:
            build_style_bigclock();
            break;
        case DISP_MODE_INFO_PANEL:
            build_style_info_panel();
            break;
        case DISP_MODE_KEY_MON:
            build_style_keymon();
            break;
        case DISP_MODE_RHYTHM:
            build_style_rhythm();
            break;
        case DISP_MODE_WALLPAPER:
            build_style_wallpaper();
            break;
    }
    
    showScreen(scr_main);
}
```

- [ ] **Step 9: 更新updateDynamicElements**

```cpp
static void updateDynamicElements(void) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (!ti || ti->tm_year < 124) return;
    
    char time_buf[16], date_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M", ti);
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d %s", 
             ti->tm_mday, ti->tm_mon + 1, 
             ti->tm_wday == 0 ? "Sun" : (ti->tm_wday == 1 ? "Mon" : ""));
    
    if (currentDispMode == DISP_MODE_GEEK) {
        if (gk_lbl_clock) lv_label_set_text(gk_lbl_clock, time_buf);
        if (gk_lbl_date) snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d",
                                  ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
        // ... 更新其他组件
    }
    // ... 其他风格的处理
}
```

- [ ] **Step 10: 验证编译**

运行: `pio run -e esp32-s3-devkitc-1`
预期: 编译成功

- [ ] **Step 11: 提交**

```bash
git add lvgl_s3/src/lvgl_s3.ino
git commit -m "feat(lvgl_s3): 添加全部6种显示风格"
```

---

## Task 4: 实现设置界面

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 实现时间设置、闹钟设置、倒计时、温度校准四个设置界面

- [ ] **Step 1: 添加设置界面的全局变量**

```cpp
// 时间设置
static int timeEditY = 2026, timeEditMo = 1, timeEditD = 1, timeEditH = 0, timeEditMi = 0;
static int timeFieldIdx = 0;

// 闹钟设置
static bool alarmEditOn = false;
static int alarmEditH = 7, alarmEditM = 0;
static int alarmFieldIdx = 0;

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

- [ ] **Step 2: 实现时间设置界面 build_settings_time()**

```cpp
static void build_settings_time(void) {
    if (scr_settings_time == nullptr) {
        scr_settings_time = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_settings_time, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_settings_time);
    }
    
    // 标题
    lv_obj_t* title = lv_label_create(scr_settings_time);
    lv_label_set_text(title, "设置时间");
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 日期显示
    char dateStr[32];
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", 
             timeEditY, timeEditMo, timeEditD);
    lv_obj_t* date = lv_label_create(scr_settings_time);
    lv_label_set_text(date, dateStr);
    lv_obj_set_style_text_font(date, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(date, 
        timeFieldIdx < 3 ? lv_color_hex(CLR_CYAN) : lv_color_hex(CLR_WHITE), 
        LV_PART_MAIN);
    lv_obj_align(date, LV_ALIGN_TOP_MID, 0, 50);
    
    // 时间显示
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeEditH, timeEditMi);
    lv_obj_t* time = lv_label_create(scr_settings_time);
    lv_label_set_text(time, timeStr);
    lv_obj_set_style_text_font(time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(time, 
        timeFieldIdx >= 3 ? lv_color_hex(CLR_GREEN) : lv_color_hex(CLR_WHITE), 
        LV_PART_MAIN);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, 0);
    
    // 提示
    lv_obj_t* hint = lv_label_create(scr_settings_time);
    lv_label_set_text(hint, "左右换字段 上下调值\n回车保存 ESC取消");
    lv_obj_set_style_text_color(hint, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    showScreen(scr_settings_time);
}
```

- [ ] **Step 3: 实现闹钟设置界面 build_settings_alarm()**

```cpp
static void build_settings_alarm(void) {
    if (scr_settings_alarm == nullptr) {
        scr_settings_alarm = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_settings_alarm, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_settings_alarm);
    }
    
    // 标题
    lv_obj_t* title = lv_label_create(scr_settings_alarm);
    lv_label_set_text(title, "闹钟设置");
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 时间
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", alarmEditH, alarmEditM);
    lv_obj_t* time = lv_label_create(scr_settings_alarm);
    lv_label_set_text(time, timeStr);
    lv_obj_set_style_text_font(time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(time, lv_color_hex(CLR_ORANGE), LV_PART_MAIN);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, -20);
    
    // 开关按钮
    lv_obj_t* sw = lv_switch_create(scr_settings_alarm);
    lv_obj_set_size(sw, 80, 40);
    lv_obj_align(sw, LV_ALIGN_CENTER, 0, 40);
    if (alarmEditOn) lv_obj_add_state(sw, LV_STATE_CHECKED);
    
    lv_obj_t* swLabel = lv_label_create(scr_settings_alarm);
    lv_label_set_text(swLabel, alarmEditOn ? "开" : "关");
    lv_obj_set_style_text_color(swLabel, 
        alarmEditOn ? lv_color_hex(CLR_GREEN) : lv_color_hex(CLR_GRAY), 
        LV_PART_MAIN);
    lv_obj_align(swLabel, LV_ALIGN_CENTER, 0, 85);
    
    // 提示
    lv_obj_t* hint = lv_label_create(scr_settings_alarm);
    lv_label_set_text(hint, "左右换字段 上下调值\n回车保存 ESC取消");
    lv_obj_set_style_text_color(hint, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    showScreen(scr_settings_alarm);
}
```

- [ ] **Step 4: 实现倒计时界面 build_settings_timer()**

类似闹钟设置，添加时:分:秒三个字段和启动/停止按钮

- [ ] **Step 5: 实现温度校准界面 build_settings_caltemp()**

```cpp
static void build_settings_caltemp(void) {
    if (scr_settings_caltemp == nullptr) {
        scr_settings_caltemp = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_settings_caltemp, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_settings_caltemp);
    }
    
    // 标题
    lv_obj_t* title = lv_label_create(scr_settings_caltemp);
    lv_label_set_text(title, "温度校准");
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 当前温度显示
    char tempStr[16];
    snprintf(tempStr, sizeof(tempStr), "%.1f C", shtTemp);
    lv_obj_t* temp = lv_label_create(scr_settings_caltemp);
    lv_label_set_text(temp, tempStr);
    lv_obj_set_style_text_font(temp, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(temp, LV_ALIGN_CENTER, 0, -40);
    
    // 偏移值
    char offsetStr[24];
    snprintf(offsetStr, sizeof(offsetStr), "偏移: %+.1f C", shtTempOffset);
    lv_obj_t* offset = lv_label_create(scr_settings_caltemp);
    lv_label_set_text(offset, offsetStr);
    lv_obj_set_style_text_color(offset, lv_color_hex(CLR_YELLOW), LV_PART_MAIN);
    lv_obj_align(offset, LV_ALIGN_CENTER, 0, 20);
    
    // 提示
    lv_obj_t* hint = lv_label_create(scr_settings_caltemp);
    lv_label_set_text(hint, "上下调偏移值\n回车保存 ESC取消");
    lv_obj_set_style_text_color(hint, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    showScreen(scr_settings_caltemp);
}
```

- [ ] **Step 6: 实现设置界面的输入处理**

```cpp
// 在scanKeyboardMatrix中添加设置模式的处理
if (IS_SETTING_MODE(currentSysMode)) {
    if (baseKey == KEY_LEFT_ARROW) {
        moveSettingField(-1);
    } else if (baseKey == KEY_RIGHT_ARROW) {
        moveSettingField(1);
    } else if (baseKey == KEY_UP_ARROW) {
        adjustSettingField(1);
    } else if (baseKey == KEY_DOWN_ARROW) {
        adjustSettingField(-1);
    } else if (baseKey == KEY_RETURN) {
        saveSettingScreen();
    } else if (baseKey == KEY_ESC) {
        cancelSettingScreen();
    }
    return;
}

#define IS_SETTING_MODE(m) ((m) >= SYS_MODE_SET_TIME && (m) <= SYS_MODE_CAL_TEMP)
```

- [ ] **Step 7: 实现saveSettingScreen和cancelSettingScreen**

```cpp
static void saveSettingScreen(void) {
    if (currentSysMode == SYS_MODE_SET_TIME) {
        struct tm t = {};
        t.tm_year = timeEditY - 1900;
        t.tm_mon = timeEditMo - 1;
        t.tm_mday = timeEditD;
        t.tm_hour = timeEditH;
        t.tm_min = timeEditMi;
        t.tm_sec = 0;
        
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        preferences.putUInt("set_epoch", (uint32_t)epoch);
        triggerHud("时间", "已保存", lv_color_hex(CLR_GREEN));
    }
    // ... 其他设置的保存
    
    currentSysMode = SYS_MODE_MENU;
    build_menu();
}

static void cancelSettingScreen(void) {
    if (currentSysMode == SYS_MODE_CAL_TEMP) {
        shtTempOffset = calTempOriginal;
    }
    currentSysMode = SYS_MODE_MENU;
    build_menu();
}
```

- [ ] **Step 8: 验证编译**

- [ ] **Step 9: 提交**

---

## Task 5: 实现宏录制

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 实现击键流录制和组合键录制功能

- [ ] **Step 1: 添加宏录制全局变量**

```cpp
// 宏录制
#define MAX_REC_KEYS 64
static uint16_t recKeyBuffer[MAX_REC_KEYS];
static int recKeyCount = 0;
static uint8_t currentRecMode = 0;  // 0=SEQ, 1=CMB
```

- [ ] **Step 2: 实现录制界面 build_recording()**

```cpp
static void build_recording(void) {
    if (scr_recording == nullptr) {
        scr_recording = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_recording, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_recording);
    }
    
    // 标题
    const char* titleStr = (currentRecMode == 0) ? "录制击键流" : "录制组合模式";
    lv_obj_t* title = lv_label_create(scr_recording);
    lv_label_set_text(title, titleStr);
    lv_obj_set_style_text_color(title, 
        currentRecMode == 0 ? lv_color_hex(CLR_YELLOW) : lv_color_hex(CLR_MAGENTA), 
        LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 计数
    char cntBuf[16];
    snprintf(cntBuf, sizeof(cntBuf), "%d/%d", recKeyCount, MAX_REC_KEYS);
    lv_obj_t* cnt = lv_label_create(scr_recording);
    lv_label_set_text(cnt, cntBuf);
    lv_obj_set_style_text_color(cnt, lv_color_hex(CLR_CYAN), LV_PART_MAIN);
    lv_obj_align(cnt, LV_ALIGN_TOP_RIGHT, -10, 10);
    
    // 键列表区域
    lv_obj_t* list = lv_obj_create(scr_recording);
    lv_obj_set_size(list, 220, 140);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_bg_color(list, lv_color_hex(CLR_DARK), LV_PART_MAIN);
    lv_obj_set_style_radius(list, 8, LV_PART_MAIN);
    
    // 显示已录按键
    lv_obj_t* keys = lv_label_create(list);
    lv_label_set_text(keys, "");
    // ... 拼接按键名称
    lv_obj_set_style_text_color(keys, lv_color_hex(CLR_WHITE), LV_PART_MAIN);
    lv_obj_align(keys, LV_ALIGN_TOP_LEFT, 10, 10);
    
    // 提示
    lv_obj_t* hint = lv_label_create(scr_recording);
    lv_label_set_text(hint, "MR切模式/取消\nM1-M12收尾");
    lv_obj_set_style_text_color(hint, lv_color_hex(CLR_GRAY), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    
    showScreen(scr_recording);
}
```

- [ ] **Step 3: 在键盘扫描中处理录制逻辑**

```cpp
// 在scanKeyboardMatrix的MR处理中添加：
if (baseKey == K_MR && currentState) {
    if (currentSysMode == SYS_MODE_NORMAL) {
        currentSysMode = SYS_MODE_REC_SEQ;
        recKeyCount = 0;
        currentRecMode = 0;
        build_recording();
    }
    return;
}

// 在录制模式下处理其他按键
if (currentSysMode == SYS_MODE_REC_SEQ || currentSysMode == SYS_MODE_REC_CMB) {
    if (baseKey >= K_M1 && baseKey <= K_M12) {
        // 收尾：保存宏
        finishMacroRecording(getMacroNameByCode(baseKey));
        return;
    }
    if (baseKey == K_MR && currentState) {
        // 切换模式或取消
        if (currentRecMode == 0) {
            currentRecMode = 1;
            recKeyCount = 0;
        } else {
            currentSysMode = SYS_MODE_NORMAL;
            showScreen(scr_main);
        }
        build_recording();
        return;
    }
    if (recKeyCount < MAX_REC_KEYS && baseKey < MACRO_BASE) {
        recKeyBuffer[recKeyCount++] = baseKey;
        build_recording();
    }
    return;
}
```

- [ ] **Step 4: 实现finishMacroRecording**

```cpp
static void finishMacroRecording(const String& targetKey) {
    if (recKeyCount == 0) {
        currentSysMode = SYS_MODE_NORMAL;
        triggerHud("录制", "已取消", lv_color_hex(CLR_RED));
        showScreen(scr_main);
        return;
    }
    
    String payload = "";
    if (currentRecMode == 0) {
        payload = "SEQ:";
        for (int i = 0; i < recKeyCount; i++) {
            payload += getKeyName(recKeyBuffer[i]);
        }
    } else {
        payload = "CMB:";
        for (int i = 0; i < recKeyCount; i++) {
            payload += String(recKeyBuffer[i]);
            if (i < recKeyCount - 1) payload += ",";
        }
    }
    
    char pKey[32];
    snprintf(pKey, sizeof(pKey), "p%d_%s", currentProfile, targetKey.c_str());
    preferences.putString(pKey, payload);
    
    currentSysMode = SYS_MODE_NORMAL;
    triggerHud("录制", "已保存", lv_color_hex(CLR_GREEN));
    showScreen(scr_main);
}
```

- [ ] **Step 5: 验证编译**

- [ ] **Step 6: 提交**

---

## Task 6: 实现通知系统

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 实现ALERT/NOTIFY指令和LVGL通知显示

- [ ] **Step 1: 添加通知系统变量**

```cpp
// 通知系统
#define MAX_NOTIFS 8
static uint8_t notifCount = 0;
static AlertType notifQueue[MAX_NOTIFS];
static char notifTexts[MAX_NOTIFS][128];
static bool notifDirty = true;

enum AlertType { ALERT_NONE, ALERT_RED, ALERT_GREEN, ALERT_YELLOW };
```

- [ ] **Step 2: 实现pushNotification**

```cpp
static void pushNotification(AlertType type, const String& text) {
    if (type == ALERT_NONE) return;
    if (notifCount >= MAX_NOTIFS) {
        for (int i = 0; i < MAX_NOTIFS - 1; i++) {
            notifQueue[i] = notifQueue[i + 1];
            strcpy(notifTexts[i], notifTexts[i + 1]);
        }
        notifCount = MAX_NOTIFS - 1;
    }
    
    notifQueue[notifCount] = type;
    strncpy(notifTexts[notifCount], text.c_str(), 127);
    notifTexts[notifCount][127] = '\0';
    notifCount++;
    notifDirty = true;
    
    // 唤醒屏幕
    lastActivityTime = millis();
    if (currentSysMode == SYS_MODE_SLEEP) {
        currentSysMode = SYS_MODE_NORMAL;
        showScreen(scr_main);
    }
}
```

- [ ] **Step 3: 扩展handleCommand支持ALERT/NOTIFY**

```cpp
static void handleCommand(const String& cmd) {
    // ... 现有命令处理
    
    bool isNotify = cmd.startsWith("NOTIFY:");
    if (cmd.startsWith("ALERT:") || isNotify) {
        String rest = cmd.substring(isNotify ? 7 : 6);
        if (rest == "OFF" || rest == "CLEAR") {
            notifCount = 0;
            notifDirty = true;
        } else if (isNotify) {
            pushNotification(ALERT_GREEN, rest);
        } else {
            // 解析ALERT:TYPE:text
            int sep = rest.indexOf(':');
            String typeTok = (sep > 0) ? rest.substring(0, sep) : rest;
            String body = (sep > 0) ? rest.substring(sep + 1) : "";
            
            if (typeTok == "RED") pushNotification(ALERT_RED, body);
            else if (typeTok == "GREEN") pushNotification(ALERT_GREEN, body);
            else if (typeTok == "YELLOW") pushNotification(ALERT_YELLOW, body);
        }
        return;
    }
    
    // ... 其他命令
}
```

- [ ] **Step 4: 实现通知显示**

```cpp
static void drawNotifPanel(void) {
    if (notifCount == 0) return;
    
    AlertType type = notifQueue[notifCount - 1];
    uint32_t color = (type == ALERT_RED) ? 0xFF4444 : 
                     (type == ALERT_GREEN) ? 0x44FF44 : 0xFFFF44;
    
    // 创建通知卡片
    lv_obj_t* mbox = lv_msgbox_create(lv_layer_top(), NULL, notifTexts[notifCount - 1], "确认", true);
    lv_obj_set_style_bg_color(lv_obj_get_parent(mbox), lv_color_hex(color & 0x00FFFFFF), 0);
    lv_obj_set_style_radius(lv_obj_get_parent(mbox), 12, 0);
    lv_obj_move_foreground(mbox);
    
    // 3秒后自动消失
    lv_obj_t* autoClose = lv_timer_create([](lv_timer_t* t) {
        lv_obj_del(lv_obj_get_parent(t->user_data));
        lv_timer_del(t);
    }, 3000, mbox);
}
```

- [ ] **Step 5: 验证编译**

- [ ] **Step 6: 提交**

---

## Task 7: BLE指令扩展

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 完整实现S3.ino的所有BLE指令

- [ ] **Step 1: 扩展handleCommand**

```cpp
// DISP_MODE指令
else if (cmd.startsWith("DISP_MODE:")) {
    uint8_t mode = cmd.substring(10).toInt();
    if (mode < TOTAL_DISP_MODES) {
        currentDispMode = mode;
        preferences.putUChar("disp_mode", mode);
        renderCurrentDisplayBase();
        triggerHud("显示风格", dispModeNames[mode], lv_color_hex(CLR_CYAN));
    }
}
// TIMERSET指令
else if (cmd.startsWith("TIMERSET:")) {
    String v = cmd.substring(9);
    if (v == "STOP") {
        timerRunning = false;
        timerRemainSec = 0;
        triggerHud("倒计时", "已停止", lv_color_hex(CLR_RED));
    } else {
        // 解析时长 HH:MM:SS
        int p1 = v.indexOf(':');
        if (p1 > 0) {
            timerEditH = v.substring(0, p1).toInt();
            int p2 = v.indexOf(':', p1 + 1);
            if (p2 > 0) {
                timerEditM = v.substring(p1 + 1, p2).toInt();
                timerEditS = v.substring(p2 + 1).toInt();
            }
        }
        // 启动计时器
        timerRunning = true;
        timerStartMs = millis();
        timerTotalSec = timerEditH * 3600 + timerEditM * 60 + timerEditS;
        timerRemainSec = timerTotalSec;
        triggerHud("倒计时", "已开始", lv_color_hex(CLR_GREEN));
    }
}
// MARQUEE指令
else if (cmd.startsWith("MARQUEE:")) {
    // 存储标语
    String text = cmd.substring(8);
    preferences.putString("marquee", text);
    triggerHud("标语", "已保存", lv_color_hex(CLR_CYAN));
}
```

- [ ] **Step 2: 验证编译**

- [ ] **Step 3: 提交**

```bash
git add lvgl_s3/src/lvgl_s3.ino
git commit -m "feat(lvgl_s3): 实现通知系统和BLE指令扩展"
```

---

## Task 8: 完整测试

**Files:**
- Modify: `lvgl_s3/src/lvgl_s3.ino`

**Goal:** 确保所有功能正常工作

- [ ] **Step 1: 检查所有功能是否实现**

对照原版S3.ino检查：
- [ ] 12项菜单全部可用
- [ ] 6种显示风格全部切换正常
- [ ] 4个设置界面全部可用
- [ ] 宏录制功能正常
- [ ] 通知系统正常
- [ ] BLE指令全部可用
- [ ] 菜单Back不重启

- [ ] **Step 2: 验证编译**

运行: `pio run -e esp32-s3-devkitc-1`
预期: 编译成功

- [ ] **Step 3: 最终提交**

```bash
git add lvgl_s3/
git commit -m "feat(lvgl_s3): 完成全部功能对齐"
```

---

## 实现顺序

1. **Task 1**: Screen管理系统重构 + Bug修复（基础架构）
2. **Task 2**: 扩展菜单到12项（菜单系统）
3. **Task 3**: 添加6种显示风格（显示系统）
4. **Task 4**: 实现设置界面（设置系统）
5. **Task 5**: 实现宏录制（高级功能）
6. **Task 6**: 实现通知系统（高级功能）
7. **Task 7**: BLE指令扩展（通信系统）
8. **Task 8**: 完整测试

## 状态

- [ ] Task 1: Screen管理系统重构 + Bug修复
- [ ] Task 2: 扩展菜单到12项
- [ ] Task 3: 添加6种显示风格
- [ ] Task 4: 实现设置界面
- [ ] Task 5: 实现宏录制
- [ ] Task 6: 实现通知系统
- [ ] Task 7: BLE指令扩展
- [ ] Task 8: 完整测试
