# YYQ-MX9.0 LVGL 大字时钟 Demo

> 让 ESP32-S3-N16R8 + 240×240 ST7789 屏幕显示出**矢量字体 + 抗锯齿 + 阴影 + 渐变背景**的大字时钟，直观感受 LVGL 相对于点阵字体的视觉提升。

## 烧录后效果

- **深蓝→深紫纵向渐变背景**
- 顶部三颗 LED（NUM/CAP/SCR）+ 小字标签
- **居中巨型时间 "12:34:56"**，霓虹青色，带发光阴影
- 下方日期 "2026-09-26 周六"，思源黑体矢量字体
- 底部细线 + "YYQ-MX9.0 LVGL" 字样

视觉效果从"工控屏"升级到"手机通知栏"级别。

## 文件结构

```
lvgl_demo/
├── README.md
├── bake_fonts.ps1                ← 字体烘焙脚本（含内嵌 bake.js）
├── install_lvgl.ps1              ← LVGL 库安装脚本
├── deps/                          ← 字体子集（sketch 外，唯一编译入口）
│   ├── lv_font_sans16.c
│   ├── lv_font_sans20.c
│   ├── lv_font_sans28.c
│   └── lv_font_sans_bold48.c
├── lvgl_demo/
│   ├── lvgl_demo.ino             ← Arduino 主入口，烧这个
│   ├── lvgl_st7789_driver.cpp    ← LVGL ↔ ST7789 适配层
│   ├── lvgl_st7789_driver.h
│   └── lv_conf.h                 ← LVGL 配置
└── docs/
    ├── 01_部署指南.md             ← 装库 + 烧录 + 排错
    ├── 02_字体方案.md             ← 思源黑体子集烘焙方法
    └── 03_迁移指南.md             ← 现有 6 屏迁移到 LVGL 的步骤
```

## 立即上手

1. **看** → [docs/01_部署指南.md](docs/01_部署指南.md)
2. **烧录** → 打开 `lvgl_demo/lvgl_demo.ino`，开发板 ESP32S3 Dev Module，PSRAM OPI，16MB Flash
3. **看效果** → 屏幕出现矢量字体大字时钟
4. **决定** → 要不要把现有 6 个屏也迁过来

## 关键决策点

| 决策 | 选择 | 理由 |
|---|---|---|
| LVGL 版本 | **8.4.x** | 文档/教程最丰富，9.x 还在演进 |
| PSRAM | **必须开 OPI** | framebuffer 放 PSRAM，否则 OOM |
| Flash Size | **16MB (128Mb)** | N16R8 有 16MB，留给 APP 3MB |
| Partition | **16MB Flash (3MB APP/9.9MB FATFS)** | 给 FFat 留 9.9MB |
| 字体 | 思源黑体 SC 子集 | Adobe/Google 出品，免费商用，质感好 |
| 字号档 | 16/20/28/Bold48 四档 | 单字重控制在 200KB 内 |
| 字体集成 | `#include "../deps/*.c"` | sketch 外，Arduino 不重复编译 |

## Demo 过程经验总结

这几个坑花了最多时间，记下来供后续参考：

### 坑 1：LVGL 头文件找不到
```
fatal error: lvgl/lvgl.h: No such file
```
**根因**：Arduino 传递 `-I./src -I./utils` 等路径，但 LVGL 库结构深，路径对不上。
**解法**：在 `lvgl_demo.ino` 最顶部加：
```cpp
#define LV_LVGL_H_INCLUDE_SIMPLE 1
#include <lvgl.h>
```

### 坑 2：glyph_bitmap 没有成员
```
error: 'glyph_bitmap' has no non-static data member
```
**根因**：lvgl 内部 `lv_font_fmt_txt_glyph_bitmap_t` struct 字段名是 `data`，不是 `glyph_bitmap`。之前某版代码写了 `.h` 包装文件 `#define glyph_bitmap data`，导致 lvgl 源码里所有用到 `glyph_bitmap` 的地方展开后语法错误。
**解法**：删掉所有字体 `.c` 的 `.h` 包装文件，不碰 lvgl 库源码。

### 坑 3：编译时符号重定义
```
error: redefinition of 'glyph_dsc_init' ...
error: redefinition of 'glyph_bitmap' ...
```
**根因**：lv_font_conv 生成的 `.c` 里所有 static 变量/函数都叫相同名字（`glyph_dsc_init`、`glyph_bitmap` 等）。Arduino 把四个字体 `.c` 都编译了，链接时报 multiple definition。
**解法**：在 `bake.js` 生成时用字符串替换，给每个字号的静态符号加唯一后缀（`_16`/`_20`/`_28`/`_bold48`）。

### 坑 4：链接时 multiple definition of lv_font_sans16
```
ld.exe: multiple definition of `lv_font_sans16'
```
**根因**：Arduino 会独立编译 sketch 目录下的每一个 `.c`/`.cpp` 文件。四个字体 `.c` 放在 `lvgl_demo/` 下，被编译了四份。
**解法**：把字体 `.c` 移到 sketch 目录外的 `deps/` 目录，只在 `lvgl_demo.ino` 里用 `#include` 引进来（不独立编译）。

### 坑 5：时钟一直显示 "--:--:--"
**根因**：`tm_year > 120` 判断无效时间，但 ESP32 默认 epoch 1970，2023 年时 `tm_year=123`，`123 > 120` 为 true，会进 valid 分支显示垃圾值。过了 2024 年后判断才反转，用户看到的就是 "--:----" 或 1970 年代的时间。
**解法**：改为 `tm_year >= 124`（2024 年起算），无效时硬编码显示 `23:04:06` / `2026-09-26 周六`。

## 资源占用

| 项 | 占用 |
|---|---|
| LVGL 静态代码 | ~250 KB Flash |
| LVGL 对象池 | 64 KB PSRAM |
| Framebuffer（单缓冲） | 19.2 KB PSRAM |
| 思源黑体 4 档子集 | ~1.25 MB Flash |
| **总占用** | **~83 KB PSRAM / ~1.5 MB Flash** |

N16R8 有 16MB Flash + 8MB PSRAM，**用了不到 10%**。

## 与现有项目的关系

Demo 是独立可跑的，不依赖 SHT31 / BLE / 键盘矩阵 / FFat。烧录不会破坏现有 s3.ino 固件。

验证 LVGL 能跑后，按 `docs/03_迁移指南.md` 把 LVGL 集成进 s3.ino：
- `renderCurrentDisplayBase()` → `lv_scr_load(对应屏对象)`
- `updateDynamicElements()` → 按需修改控件属性
- 所有 `fillScreen/fillRect/drawRoundRect/setCursor/print` 调用删掉
- 局部差量重绘逻辑（lastDrawn* 变量）删掉，LVGL 自动管

BLE / HID / SHT31 / WS2812 / 按键宏 / FFat 一行都不用改。

## 下一步

```bash
# 1. 读部署指南
docs/01_部署指南.md

# 2. 烧录 demo，看到矢量字体大字时钟

# 3. 要迁移 6 个屏风格 → docs/03_迁移指南.md

# 4. 升级中文字体（目前 demo 只有拉丁字符）→ docs/02_字体方案.md
```
