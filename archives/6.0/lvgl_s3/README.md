# LVGL 核心版固件 (lvgl_s3)

基于 ESP32-S3-N16R8 + ST7789 240x240 + LVGL 8.4.x

## 风格：简约纯黑

- 深黑背景 #000000
- 白色/青色数字文字
- 顶部 NUM/CAPS/SCROLL LED 状态
- 中央大号时钟 (Montserrat 48)
- 日期 + 温湿度 + 击键统计

## 功能（核心优先）

1. **键盘矩阵** - 10x16 完整扫描
2. **USB HID** - 键盘输入 + 媒体键
3. **时间显示** - 时钟 + 日期
4. **温湿度** - SHT31 传感器
5. **简化菜单** - 切换显示模式
6. **HUD 通知** - 操作反馈

## 显示模式

1. **极客仪表盘** - 时间/温湿度/击键
2. **大字时钟** - 简约大号时间

## 硬件

- ESP32-S3-N16R8
- ST7789 240x240 LCD
- MCP23017 键盘矩阵
- SHT31 温湿度传感器
- WS2812 LED (19颗)

## 烧录

```bash
pio run -t upload
```

## 改完界面文案后必须重生成字体

界面里的中文是靠 `src/lv_font_simsun_16_cjk.c` 这个子集字库渲染的。
**在 .ino 里新增/修改任何中文后，要重跑一次字体生成**，否则新字会是空白：

```bash
python gen_font.py    # 重新生成 lv_font_simsun_16_cjk.c + charset.txt
python check_font.py  # 确认 MISSING 为 0
```

`gen_font.py` 会扫 .ino 里的字符串字面量 + 一份兜底词表，用 `C:\Windows\Fonts\simhei.ttf`
生成 16px / bpp=4 的子集，并挂上 `fallback = &lv_font_montserrat_14`。

> ⚠️ 别把参数改回 `--symbol charset.txt`。这版 lv_font_conv 的 `--symbols`
> **不读文件**，参数值会被当成字面字符（见 `node_modules/lv_font_conv/lib/collect_font_data.js`），
> 结果就是字库里一个汉字都没有、界面全是豆腐块。脚本改成传 `-r` 码点区间（纯 ASCII，
> 不会被 Windows 的 cmd + GBK 代码页改写）。

字体相关约定：

- `LV_FONT_DEFAULT` 指向中文字体 → 凡是没显式指定字体的控件都能出中文
- 需要显示中文的 label 显式用 `&lv_font_simsun_16_cjk`（方案名 / HUD / 通知条）
- 纯英文数字的控件继续用 `lv_font_montserrat_*`，观感更好

## 屏幕与按键的约定

- 主屏是真正的 `lv_obj_t` 屏幕对象，由 `ensureMainScreen()` 建/取，
  6 种主屏风格都建在它上面。**不要**改回 `lv_obj_create(lv_scr_act())`。
- 回主屏统一走 `gotoMainScreen()`，它负责模式复位 + 内容重建 + 切屏。
- 按键分发顺序必须和 `s3/s3.ino` 原版一致：界面态（菜单 → 设置 → 录制）先接管，
  `K_MC` 放最后。在界面态里 `K_MC` 和 `ESC` 都是"退出"。

## 目录结构

```
lvgl_s3/
├── src/
│   ├── lvgl_s3.ino          ← 主程序
│   ├── lv_conf.h             ← LVGL 配置
│   ├── lvgl_st7789_driver.cpp/h  ← 驱动
│   ├── lv_font_simsun_16_cjk.c   ← 中文字体（改文案后需重新生成）
│   └── charset.txt          ← 字集清单（gen_font.py 自动维护）
├── gen_font.py              ← 生成中文字体
├── check_font.py            ← 校验字形覆盖
├── docs/superpowers/specs/  ← 设计文档
└── platformio.ini            ← 构建配置
```
