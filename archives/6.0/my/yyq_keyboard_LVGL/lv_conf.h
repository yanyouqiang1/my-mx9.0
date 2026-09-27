/**
 * @file lv_conf.h
 * ESP32-S3 + ST7789 (240x240) 专用 LVGL 8.3 配置文件
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   图形色彩深度配置
 *====================*/
/* 颜色深度: 16 (RGB565) 用于大部分 SPI 屏幕 */
#define LV_COLOR_DEPTH 16

/* ST7789 屏幕必须开启字节反转，否则颜色会发蓝或反色 */
#define LV_COLOR_16_SWAP 1

/*====================
   内存分配配置
 *====================*/
/* 使用内置简单分配器，分配 48KB 堆内存给 LVGL 对象 */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

/*====================
   输入与显示刷新配置
 *====================*/
/* 屏幕默认重绘周期 (毫秒) */
#define LV_DISP_DEF_REFR_PERIOD 20

/* 输入设备读取周期 (毫秒) */
#define LV_INDEV_DEF_READ_PERIOD 20

/*==================================
   中文字体与 UTF-8 编码支持 (核心)
 *==================================*/
/* 必须启用 UTF-8 编码支持，中文字符串才不会乱码 */
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/* 启用 LVGL 官方自带的 16px 简体中文 CJK 字库 */
#define LV_FONT_SIMSUN_16_CJK 1

/* 同时保留常用英文 ASCII 字体用于时钟与按键大号展示 */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

/* 设置系统默认全局字体为中文 */
#define LV_FONT_DEFAULT &lv_font_simsun_16_cjk

/*====================
   控件组件支持开关
 *====================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1

/* 额外复杂组件 */
#define LV_USE_ANIMIMG    1
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_METER      1
#define LV_USE_MSGBOX     1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        0

/*====================
   主题配置
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

#endif /*LV_CONF_H*/
