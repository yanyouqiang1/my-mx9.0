# Task 3: 添加6种显示风格

## Goal
实现全部6种显示风格。

## 显示模式定义
```cpp
#define DISP_MODE_GEEK        0
#define DISP_MODE_BIG_CLOCK   1
#define DISP_MODE_INFO_PANEL  2
#define DISP_MODE_KEY_MON     3
#define DISP_MODE_RHYTHM      4
#define DISP_MODE_WALLPAPER   5
#define TOTAL_DISP_MODES       6

static const char* dispModeNames[TOTAL_DISP_MODES] = {
    "极客仪表盘", "大字时钟", "信息面板", "击键监控", "律动", "壁纸"
};
```

## 6种风格实现

### 1. 极客仪表盘 (DISP_MODE_GEEK) - 已有，完善
- 顶部：NUM/CAPS/SCROLL LED + 方案名
- 中央：大号时钟
- 日期
- 底部：温湿度 + 击键数

### 2. 大字时钟 (DISP_MODE_BIG_CLOCK) - 已有
- 纯黑背景
- 中央：大号时钟 48px
- 底部：日期

### 3. 信息面板 (DISP_MODE_INFO_PANEL) - 新增
- 上：时钟 + 日期
- 中：三格实心圆（NUM/CAPS/SCROLL状态）
- 下：方案图标 + 实时按键 + 击键统计

### 4. 实时击键监控 (DISP_MODE_KEY_MON) - 新增
- 顶部标题栏
- 中央：最近按键（大字）
- 底部：击键统计 + 方案

### 5. 律动效果 (DISP_MODE_RHYTHM) - 新增
- 上：频谱律动柱（24根）
- 下：统计显示

### 6. 自定义壁纸 (DISP_MODE_WALLPAPER) - 新增
- 从FFat读取/logo.bin
- 底部叠加时钟显示

## 实现要点

### 组件变量声明
```cpp
// 信息面板
static lv_obj_t* ip_bg = nullptr;
static lv_obj_t* ip_lbl_clock = nullptr;
static lv_obj_t* ip_lbl_date = nullptr;
static lv_obj_t* ip_circle_num = nullptr;
static lv_obj_t* ip_circle_caps = nullptr;
static lv_obj_t* ip_circle_scr = nullptr;

// 击键监控
static lv_obj_t* km_bg = nullptr;
static lv_obj_t* km_lbl_lastkey = nullptr;
static lv_obj_t* km_lbl_keys = nullptr;

// 律动
static lv_obj_t* rh_bg = nullptr;
static lv_obj_t* rh_lbl_keys = nullptr;
static uint8_t rhythmBars[24] = {0};

// 壁纸
static lv_obj_t* wp_bg = nullptr;
static lv_obj_t* wp_lbl_time = nullptr;
```

### renderCurrentDisplayBase修改
```cpp
static void renderCurrentDisplayBase(void) {
    if (scr_main == nullptr) {
        scr_main = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_main, lv_color_hex(CLR_BLACK), LV_PART_MAIN);
    } else {
        lv_obj_clean(scr_main);
    }
    
    switch (currentDispMode) {
        case DISP_MODE_GEEK: build_style_geek(); break;
        case DISP_MODE_BIG_CLOCK: build_style_bigclock(); break;
        case DISP_MODE_INFO_PANEL: build_style_info_panel(); break;
        case DISP_MODE_KEY_MON: build_style_keymon(); break;
        case DISP_MODE_RHYTHM: build_style_rhythm(); break;
        case DISP_MODE_WALLPAPER: build_style_wallpaper(); break;
    }
    
    showScreen(scr_main);
}
```

### updateDynamicElements修改
支持所有6种风格的动态更新。

## 文件
- 修改: `E:/ai-project/mx9.0/archives/6.0/lvgl_s3/src/lvgl_s3.ino`

## 验证
编译成功：`pio run -e esp32-s3-devkitc-1`

## 依赖
- Task 1, 2完成的Screen管理系统和菜单系统
