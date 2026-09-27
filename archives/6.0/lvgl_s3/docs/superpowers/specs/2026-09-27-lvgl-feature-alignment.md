# YYQ-MX9.0 LVGL 版本功能对齐设计

## 概述

基于混合架构策略：UI核心用LVGL实现，高级功能（宏录制、通知、灯效）复用原版S3.ino逻辑。

## 目标

1. **功能对齐**：将lvgl_s3.ino对齐到原版S3.ino的全部12项菜单功能和6种显示风格
2. **Bug修复**：修复菜单Back导致重启的问题
3. **保持风格**：继续使用简约深色LVGL风格

## 架构设计

### 混合架构层次

```
┌─────────────────────────────────────────────────────────────┐
│                    LVGL 核心层                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ 主显示   │  │  菜单    │  │ 设置界面 │  │ HUD浮层  │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    功能复用层（S3.ino）                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ 宏录制   │  │ 通知系统 │  │ 灯效引擎 │  │ BLE指令  │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    硬件抽象层                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ 键盘矩阵  │  │ USB HID  │  │ SHT31    │  │ ST7789   │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 功能模块映射

| 功能 | 原版实现 | LVGL版实现 | 状态 |
|------|----------|------------|------|
| 键盘矩阵扫描 | s3.ino | lvgl_s3.ino | ✅ 已有 |
| USB HID | s3.ino | lvgl_s3.ino | ✅ 已有 |
| BLE通信 | s3.ino | lvgl_s3.ino | ✅ 已有 |
| SHT31温湿度 | s3.ino | lvgl_s3.ino | ✅ 已有 |
| 灯效引擎 | s3.ino | lvgl_s3.ino | ✅ 已有 |
| **6种显示风格** | s3.ino | 新建6套build | 🔄 需实现 |
| **12项菜单** | s3.ino | 新建LVGL List | 🔄 需实现 |
| **设置界面** | s3.ino | 新建LVGL界面 | 🔄 需实现 |
| **宏录制** | s3.ino | 复用逻辑 | 🔄 需实现 |
| **通知系统** | s3.ino | 新建LVGL MsgBox | 🔄 需实现 |
| **HUD浮层** | s3.ino | 已有/需扩展 | ⚠️ 需扩展 |

## 6种显示风格

### 1. 极客仪表盘 (DISP_MODE_GEEK)
- 顶部：NUM/CAPS/SCROLL LED + 方案名
- 中央：大号时钟 48px
- 日期：YYYY/MM/DD 星期
- 底部：温湿度 + 击键数

### 2. 大字时钟 (DISP_MODE_BIG_CLOCK)
- 纯黑背景
- 中央：大号时钟 48px
- 底部：日期 + 星期

### 3. 信息面板 (DISP_MODE_INFO_PANEL)
- 上：时钟 + 日期
- 中：三格实心圆（NUM/CAPS/SCROLL状态）
- 下：方案图标 + 实时按键 + 击键统计

### 4. 实时击键监控 (DISP_MODE_KEY_MON)
- 顶部标题栏
- 中央：最近按键（大字）
- 底部：击键统计 + 方案

### 5. 律动效果 (DISP_MODE_RHYTHM)
- 上：频谱律动柱
- 下：三格锁状态
- 击键时触发视觉反馈

### 6. 自定义壁纸 (DISP_MODE_WALLPAPER)
- 从FFat读取/logo.bin
- 底部叠加时钟显示

## 12项菜单

```
1. 返回主屏
2. 切换主屏风格
3. 切换配置方案
4. 按键回显开关
5. 键盘背光灯效
6. 状态灯亮度
7. 设置时间
8. 闹钟设置
9. 倒计时
10. 立即刷新温湿度
11. 温度校准
12. 敲击计数清零
```

### 菜单导航

| 操作 | 功能 |
|------|------|
| 上下键/旋钮 | 移动光标 |
| 回车/旋钮按下 | 选择菜单项 |
| ESC | 返回主屏（不重启） |

## 设置界面设计

### 时间设置
- 字段：年/月/日/时/分
- 操作：左右切换字段，上下/旋钮调整值
- 保存：回车保存，ESC取消

### 闹钟设置
- 字段：时/分/开关
- 操作：左右切换字段，上下调整值
- 开关：整块按钮切换

### 倒计时设置
- 字段：时/分/秒
- 操作：左右切换字段，上下调整值
- 启动按钮：开始/停止

### 温度校准
- 显示：当前温度（实时）
- 字段：偏移值（±0.5°C步进）
- 保存按钮

## Bug修复：菜单Back重启

### 问题原因
当前实现中，选择Back时调用`renderCurrentDisplayBase()`：
1. 删除当前screen (`lv_obj_del(scr_main)`)
2. 创建新的screen
3. 可能在LVGL内部状态未正确更新时触发问题

### 修复方案
```cpp
// 方案：使用Screen管理，避免重建
static lv_obj_t* scr_main = nullptr;
static lv_obj_t* scr_menu = nullptr;
static lv_obj_t* scr_settings = nullptr;
static lv_obj_t* scr_alarm = nullptr;
static lv_obj_t* scr_timer = nullptr;
static lv_obj_t* scr_caltemp = nullptr;

// 预创建所有Screen，按需显示
void showScreen(lv_obj_t* target) {
    if (lv_scr_act() == target) return;
    lv_scr_load(target);
}

// 菜单Back：加载主屏Screen
void onMenuBack() {
    currentSysMode = SYS_MODE_NORMAL;
    showScreen(scr_main);
    // 不重建Screen，只切换显示
}
```

### 关键点
- 预创建所有需要的Screen
- 菜单Back时只切换Screen，不重建组件
- 使用`lv_scr_load()`而非`lv_obj_del()` + 重建

## 通知系统

### LVGL实现
```cpp
// 使用MsgBox作为通知
static void showNotification(const char* msg, uint32_t color) {
    lv_obj_t* mbox = lv_msgbox_create(nullptr, "通知", msg, "确认", true);
    // 设置样式
    lv_obj_set_style_bg_color(lv_obj_get_parent(mbox), lv_color_hex(color), 0);
}
```

### 通知类型
- ALERT:RED - 红灯报警
- ALERT:GREEN - 绿灯提示
- ALERT:YELLOW - 黄灯警告
- NOTIFY:xxx - 快捷提示

## 宏录制

### LVGL界面
- 顶部：录制状态（击键流/组合模式）
- 中部：已录按键列表
- 底部：提示（M1-M12收尾，MR切换/取消）

### 复用逻辑
- 复用S3.ino的`recKeyBuffer[]`
- 复用`finishMacroRecording()`
- 复用`executeSequenceAction()` / `executeComboAction()`

## 实现步骤

### Phase 1: 核心架构
1. [ ] Screen管理系统（预创建所有Screen）
2. [ ] Bug修复（菜单Back）
3. [ ] 12项LVGL菜单

### Phase 2: 显示风格
4. [ ] 极客仪表盘完善
5. [ ] 大字时钟完善
6. [ ] 信息面板
7. [ ] 实时击键监控
8. [ ] 律动效果
9. [ ] 自定义壁纸

### Phase 3: 设置界面
10. [ ] 时间设置
11. [ ] 闹钟设置
12. [ ] 倒计时设置
13. [ ] 温度校准

### Phase 4: 高级功能
14. [ ] 宏录制界面
15. [ ] 通知系统
16. [ ] BLE指令扩展

## 文件结构

```
lvgl_s3/
├── src/
│   ├── lvgl_s3.ino          # 主程序
│   ├── lv_conf.h            # LVGL配置
│   ├── lvgl_st7789_driver.cpp/h  # 驱动
│   └── lv_font_simsun_16_cjk.c   # 中文字体
├── docs/
│   └── superpowers/
│       └── specs/
│           ├── 2026-09-27-lvgl-core-design.md
│           └── 2026-09-27-lvgl-feature-alignment.md  # 本文档
└── platformio.ini
```

## 颜色方案（简约深色）

```cpp
#define CLR_BLACK   0x000000  // 背景
#define CLR_WHITE   0xFFFFFF  // 主文字
#define CLR_CYAN    0x00E5FF  // 强调色
#define CLR_GRAY    0x808080  // 次要文字
#define CLR_DARK    0x1A1A1A  // 卡片背景
#define CLR_ACCENT  0x00AACC  // 按钮强调
#define CLR_RED     0xFF4444  // 报警
#define CLR_GREEN   0x44FF44  // 成功
#define CLR_YELLOW  0xFFFF44  // 警告
```

## 状态

- [x] 设计完成
- [ ] Phase 1: 核心架构
- [ ] Phase 2: 显示风格
- [ ] Phase 3: 设置界面
- [ ] Phase 4: 高级功能
