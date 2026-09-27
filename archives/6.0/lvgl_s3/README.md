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
- 6 种风格的根容器统一用 `makeRootPanel()` 建，**不要**再手写 `lv_obj_create` +
  `set_size/pos`。原因：LVGL 默认主题给普通 `lv_obj` 套了 `card` 样式（`pad_all = 13px`
  @DPI130），而 `lv_obj_set_pos()` 会走 `lv_obj_move_to()`，**把父对象的 pad+border
  叠进子对象坐标**。于是任何"贴在 (0,0)"的子对象都会右下偏 13px，右边和下边被裁掉
  —— 表现就是"整个界面往右移了一点"、壁纸模式下尤其明显。用 `lv_obj_align()` 的
  子对象不受影响（对齐算 content 区中心，pad 对称时中心不变），所以很容易漏。
- 顶部条（`dashTopBar()`）左三是三颗锁状态灯（NUM/CAP/SCR，只画圆点不写文字），
  右边是方案指示：序号常显，1/2 号方案另配系统图标（Windows 四格窗 / 苹果），
  图标全部用 `lv_obj` 基本图形拼，改样式时别去动 `topIconWin[] / topIconMac[]` 的
  创建顺序（= 绘制顺序：果体 → 缺口 → 果柄 → 叶）。
- **别用 `transform_zoom` / `transform_angle` 放大字号或做视觉效果**：
  LVGL 8.4 的软件渲染器对需要 alpha 的中间图层有一道闸门
  （`lv_draw_sw_layer.c`: `LV_COLOR_SCREEN_TRANSP == 0 && HAS_ALPHA → return NULL`），
  本项目没开 `LV_COLOR_SCREEN_TRANSP`，走这条路的控件会被**静默地整个不画**。
  要大字就上大字号的字库。
- 回主屏统一走 `gotoMainScreen()`，它负责模式复位 + 内容重建 + 切屏。
- 按键分发顺序必须和 `s3/s3.ino` 原版一致：界面态（菜单 → 设置 → 录制）先接管，
  `K_MC` 放最后。在界面态里 `K_MC` 和 `ESC` 都是"退出"。
- 宏/序列/组合键的执行链和原版 `s3/s3.ino` 一致：`executeGlobalKey()` 吃
  `GSET` 的 `SW:x+SEQ:…` / `SW:x+CMB:…`，`executeMacro()` 吃按方案的
  `p<n>_<键名>`（`SEQ:` / `CMB:` 两种）。网页端 `s3-setting.html` 发的
  `SET:p0_M1:…` 名字里已经带方案号，固件端**不要再拼一层** `p<currentProfile>_`。

## 文件系统与分区（别踩）

- 壁纸 `/logo.bin` 和 ME 文本 `/me_hex.txt` 存在 **SPIFFS** 上，**不是 FFat**。
- 板子默认分区表（PlatformIO 的 `default_8MB.csv` / Arduino IDE 的 `default.csv`）
  里只有 `spiffs` 分区，**没有 ffat 分区**。`FFat.begin()` 找不到 subtype=fat 的分区
  会直接返回 false，之后每次 `FFat.open()` 都是一个无效 File —— 表现就是
  "ME 文本存入失败""壁纸重启就丢"。
- 所以烧录时的分区方案必须是**默认那几档**（带 spiffs 的）；选了
  "16MB Flash (3MB APP/9.9MB FATFS)" 这类 FATFS 方案，SPIFFS 反而会挂不上。
  （`lvgl_demo/` 那个 README 让人选 FATFS，是历史遗留，别照做。）
- 分区表别改：宏 / 按键重映射 / 方案 / 灯光 / 校准值都在 `nvs` 分区，
  挪动偏移等于把用户配置全抹掉。

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
