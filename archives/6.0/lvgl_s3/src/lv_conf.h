/**
 * @file lv_conf.h
 *
 * ESP32-S3-N16R8 + ST7789 240x240 LVGL 配置
 *
 * 适配说明：
 *   - 8MB PSRAM：framebuffer / 对象池放到 PSRAM
 *   - 屏幕：240x240 RGB565
 *   - 关闭用不到的功能（3D、Bidi、文件系统）
 *   - 字体：LVGL 内置 Montserrat（ASCII 全覆盖，无需外部字库）
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   类型与分辨率
   ====================*/
#define LV_HOR_RES_MAX          240
#define LV_VER_RES_MAX          240
#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP        0   // ST7789 RGB565 字节序，高位在前

/*====================
   内存与对象池
   ====================*/
// 对象池走 PSRAM（板子是 N16R8，8MB 八线 PSRAM）。
// 之前是 LV_MEM_CUSTOM=0 + LV_MEM_SIZE=64KB —— 那 64KB 静态占着内部 DRAM，
// 和 BLE / FFat / 19.2KB 帧缓冲 / 任务栈抢同一块 327KB，内部堆被压到很紧，
// 这是"用一段时间自己重启"的根因之一。
// LVGL 在这块内存里只做指针运算和普通读写、不做 DMA，放 PSRAM 安全。
// 分配器见 lv_mem_port.h，PSRAM 不可用时自动回落 malloc。
#define LV_MEM_CUSTOM            1
#define LV_MEM_CUSTOM_INCLUDE    "lv_mem_port.h"
#define LV_MEM_CUSTOM_ALLOC      lv_port_alloc
#define LV_MEM_CUSTOM_REALLOC    lv_port_realloc
#define LV_MEM_CUSTOM_FREE       lv_port_free
#define LV_MEM_SIZE             (64U * 1024U)   // LV_MEM_CUSTOM=1 时不生效，保留做文档
#define LV_MEM_ADR              0
#define LV_USE_BUILTIN_MALLOC    1
#define LV_MEM_MONITOR          0

/*====================
   帧缓冲
   ====================*/
#define LV_DISP_DOUBLE_BUF       0
#define LV_DISP_REFR_PERIOD     33   // 30fps
#define LV_DISP_DEF_REFR_PERIOD 33

/*====================
   性能监控 / 日志
   ====================*/
#define LV_USE_PERF_MONITOR     0
#define LV_USE_LOG              0

/*====================
   关闭用不到的功能
   ====================*/
#define LV_USE_BIDI              0
#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_USE_FILE_EXPLORER    0
#define LV_USE_FFAT             0
#define LV_USE_FS_POSIX         0
#define LV_USE_GIF              0
#define LV_USE_BMP              0
#define LV_USE_PNG              0
#define LV_USE_SJPG             0
#define LV_USE_QRCODE           0
#define LV_USE_FREETYPE         0
#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_BASIC      1
#define LV_USE_THEME_MONO       0

/*====================
   动画
   ====================*/
#define LV_ANIM_INCLUDE         1
#define LV_ANIM_EASE            1
#define LV_ANIM_PATH            1
#define LV_USE_ANIM_CUSTOM      0

/*====================
   控件
   ====================*/
#define LV_USE_ARC              1
#define LV_USE_BAR              1
#define LV_USE_BTN              1
#define LV_USE_BTNMATRIX        1
#define LV_USE_CALENDAR         0
#define LV_USE_CANVAS           0
#define LV_USE_CHART            1
#define LV_USE_CHECKBOX         1
#define LV_USE_DROPDOWN         1
#define LV_USE_IMG              1
#define LV_USE_IMGBTN           1
#define LV_USE_KEYBOARD         1
#define LV_USE_LABEL            1
#define LV_USE_LED              1
#define LV_USE_LINE             1
#define LV_USE_LIST             1
#define LV_USE_MENU             0
#define LV_USE_MSGBOX           1
#define LV_USE_OBJ_MASK         0
#define LV_USE_OBSERVER         1
#define LV_USE_ROLLER           0
#define LV_USE_SLIDER           1
#define LV_USE_SPAN             1
#define LV_USE_SPINBOX          1
#define LV_USE_SPINNER          1
#define LV_USE_SWITCH           1
#define LV_USE_TABLE            0
#define LV_USE_TABVIEW          0
#define LV_USE_TEXTAREA         1
#define LV_USE_TILEVIEW         0
#define LV_USE_WIN              0

/*====================
   字体
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
#define LV_FONT_MONTSERRAT_48    1

#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_MONTSERRAT_48_COMPRESSED 0
// 字库生成时带了 --no-compress，所以这里保持 1 只是当保险：万一以后又换成压缩字库，
// 至少不会掉进 lv_font_fmt_txt.c 的
// "Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled" 分支直接
// return NULL —— 那个分支的表现是所有中文全部空白，只有 Montserrat 的数字还在。
#define LV_USE_FONT_COMPRESSED    1
#define LV_USE_FONT_SUBPX         0

// 中文子集字体（黑体 16px，ASCII + 源码里实际用到的 300 个汉字）
// 由 gen_font.py 生成，缺失字形回落到 Montserrat 14
#define LV_FONT_SIMSUN_16_CJK    1

// 把自定义字体声明给整个 LVGL（lv_font.h 会展开这一行），
// 否则 lvgl_s3.ino 里引用 &lv_font_simsun_16_cjk 会编译不过
#define LV_FONT_CUSTOM_DECLARE  extern const lv_font_t lv_font_simsun_16_cjk;

// 默认字体用中文字体：凡是没显式指定字体的控件（theme 带的、以及漏写的）
// 都从它取字，漏配字体的地方就再也不会显示成豆腐块/空白。
// 它自带 ASCII，所以英文数字一样正常。
#define LV_FONT_DEFAULT         &lv_font_simsun_16_cjk

/*====================
   数学
   ====================*/
#define LV_SPRINTF_USE_FLOAT    0
#define LV_USE_FLOAT            0
#define LV_SQRT_FLOAT_PRECISION 100

/*====================
   调试
   ====================*/
#define LV_USE_ASSERT_MEM       0
#define LV_USE_ASSERT_OBJ       0
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_USER_DATA        0

#endif /*LV_CONF_H*/
