/**
 * @file lv_conf.h
 *
 * ESP32-S3-N16R8 + ST7789 240x240 精简配置
 *
 * 适配说明：
 *   - 16MB Flash / 8MB PSRAM：LVGL 把 framebuffer / 对象池放到 PSRAM
 *   - 屏幕：240x240 RGB565（与现有项目一致）
 *   - 关闭一切用不到的功能（3D、Bidi、文件系统、性能监视器）
 *   - 字体：先全开 Montserrat（LVGL 自带，矢量、抗锯齿、好看）
 *           后期再烘焙思源黑体中文字体替换（见 docs/02_字体方案.md）
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   类型与对齐
   ====================*/
#define LV_HOR_RES_MAX          240
#define LV_VER_RES_MAX          240
#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP        0   // ST7789 RGB565 字节序：高位在前，不用 swap

/*====================
   内存与对象池
   ====================*/
// LVGL 会从这里一次性 alloc 大块 PSRAM 做对象池。
// 设大一点免得复杂场景下反复 alloc；8MB PSRAM 给 64KB 毫无压力。
#define LV_MEM_SIZE             (64U * 1024U)
#define LV_MEM_ADR              0
#define LV_MEM_CUSTOM            0    // 用 LVGL 自带 mem，不接 ps_malloc
#define LV_USE_BUILTIN_MALLOC    1
#define LV_MEM_MONITOR          0    // 关，省 RAM；想排查问题时打开

/*====================
   帧缓冲
   ====================*/
// N16R8 有 8MB PSRAM，开双缓冲减少撕裂
#define LV_DISP_DOUBLE_BUF       1
#define LV_DISP_REFR_PERIOD      33  // 30fps；小屏没必要 60fps，省 CPU
#define LV_DISP_DEF_REFR_PERIOD  33

/*====================
   性能监控 / 日志
   ====================*/
#define LV_USE_PERF_MONITOR      0   // 关
#define LV_USE_LOG               0   // 关；调试时打开打印到串口

/*====================
   关闭用不到的功能
   ====================*/
#define LV_USE_BIDI             0   // 阿拉伯文/希伯来文，不需要
#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_USE_FILE_EXPLORER    0
#define LV_USE_FFAT             0   // 我们有自己的 FFat 文件管理
#define LV_USE_FS_POSIX         0
#define LV_USE_GIF              0   // 后期要 JPG 解码就用 jpegdec 自己处理，不走 LVGL
#define LV_USE_BMP              0
#define LV_USE_PNG              0
#define LV_USE_SJPG             0   // 关闭 LVGL 自带 JPG 解码，复用项目里已有 JPEGDEC
#define LV_USE_QRCODE           0
#define LV_USE_FREETYPE         0   // 关闭 FreeType（前期用 LVGL 内置字）
#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_BASIC      1
#define LV_USE_THEME_MONO       0

/*====================
   动画
   ====================*/
#define LV_ANIM_INCLUDE         1
#define LV_ANIM_EASE            1   // 缓动函数全开（很省，但视觉效果差异大，值得开）
#define LV_ANIM_PATH            1
#define LV_USE_ANIM_CUSTOM      0

/*====================
   控件 - 大部分要保留
   ====================*/
#define LV_USE_ARC              1   // 极客仪表盘温湿度用 arc 进度条
#define LV_USE_BAR              1   // 通用进度条
#define LV_USE_BTN              1
#define LV_USE_BTNMATRIX        1   // 矩阵键盘 6 风格预览可能用到
#define LV_USE_CALENDAR         0   // 不需要日历
#define LV_USE_CANVAS           0   // 我们有 Adafruit GFX 在另一线程画
#define LV_USE_CHART            1   // 律动用折线图
#define LV_USE_CHECKBOX         1
#define LV_USE_DROPDOWN         1   // 设置菜单用
#define LV_USE_IMG              1   // 全屏壁纸
#define LV_USE_IMGBTN           1
#define LV_USE_KEYBOARD         1   // 蓝牙配对码 / 时间设置
#define LV_USE_LABEL            1
#define LV_USE_LED              1   // 三个锁状态指示
#define LV_USE_LINE             1   // 通用分隔线
#define LV_USE_LIST             1   // 设置菜单
#define LV_USE_MENU             0   // 不上菜单控件，自己堆 list 更可控
#define LV_USE_MSGBOX           1
#define LV_USE_OBJ_MASK         0   // 复杂遮罩，不需要
#define LV_USE_OBSERVER         1   // 主题切换 / 实时刷新都用得到
#define LV_USE_ROLLER           0
#define LV_USE_SLIDER           1   // 音量 / 亮度条
#define LV_USE_SPAN             1   // 多样式文本（粗细/颜色混排）
#define LV_USE_SPINBOX          1   // 数字编辑（闹钟 / 倒计时）
#define LV_USE_SPINNER          1   // loading
#define LV_USE_SWITCH           1   // 闹钟开关
#define LV_USE_TABLE            0
#define LV_USE_TABVIEW          0
#define LV_USE_TEXTAREA         1
#define LV_USE_TILEVIEW         0
#define LV_USE_WIN              0

/*====================
   字体（先用 LVGL 内置 Montserrat 系列）
   ====================*/
#define LV_FONT_MONTSERRAT_8     1
#define LV_FONT_MONTSERRAT_10    1
#define LV_FONT_MONTSERRAT_12    1
#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_16    1
#define LV_FONT_MONTSERRAT_18    1
#define LV_FONT_MONTSERRAT_20    1
#define LV_FONT_MONTSERRAT_22    0
#define LV_FONT_MONTSERRAT_24    1
#define LV_FONT_MONTSERRAT_26    0
#define LV_FONT_MONTSERRAT_28    1
#define LV_FONT_MONTSERRAT_32    0
#define LV_FONT_MONTSERRAT_36    0
#define LV_FONT_MONTSERRAT_40    0
#define LV_FONT_MONTSERRAT_48    1   // 大字时钟用 48

// 高级字体（占用更多 Flash / RAM，前期不启用）
#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_MONTSERRAT_48_COMPRESSED 0
#define LV_USE_FONT_COMPRESSED    0
#define LV_USE_FONT_SUBPX         0

// 自定义中文字体 - 思源黑体 SC 子集（由 bake_fonts.ps1 烤出）
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(lv_font_sans16);      \
    LV_FONT_DECLARE(lv_font_sans20);      \
    LV_FONT_DECLARE(lv_font_sans28);      \
    LV_FONT_DECLARE(lv_font_sans_bold48)
#define LV_FONT_DEFAULT         &lv_font_sans16

// 启用自定义 tick callback（让 driver 用 millis() 给 LVGL 时基）
// 0 = 用默认的 lv_tick_inc 机制；1 = 自己提供 lv_tick_set_cb
#define LV_USE_TICK_CUSTOM       1

/*====================
   数学
   ====================*/
#define LV_SPRINTF_USE_FLOAT      0   // 不打印 float（省栈）
#define LV_USE_FLOAT              0
#define LV_SQRT_FLOAT_PRECISION   100

/*====================
   调试
   ====================*/
#define LV_USE_ASSERT_MEM        0   // 关掉内存越界检查（要排查问题时打开）
#define LV_USE_ASSERT_OBJ        0
#define LV_USE_ASSERT_STYLE      0
#define LV_USE_USER_DATA         0   // 不要给对象塞用户数据指针

#endif /*LV_CONF_H*/