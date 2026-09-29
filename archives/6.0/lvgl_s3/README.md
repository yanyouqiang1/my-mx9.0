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
- 顶部条（`dashTopBar()`）左三是三颗锁状态灯（NUM/CAP/SCR，只画圆点不写文字，
  外面套一圈同色细环，亮起时才显形），右边是方案指示：序号常显，1/2 号方案另配
  系统图标（Windows 四格窗 / 苹果），图标全部用 `lv_obj` 基本图形拼，改样式时
  别去动 `topIconWin[] / topIconMac[]` 的创建顺序（= 绘制顺序：果体 → 缺口 → 果柄 → 叶）。
- 三颗锁状态**翻转的那一刻**会弹一条 HUD（`updateDynamicElements` 里用
  `lockPrev[]` 比对上一帧）。顶栏那三颗灯太靠边，离远了看不出大写开没开，
  光靠变色不够。`lockPrevValid` 是"界面刚重建"的标志：重建后第一帧只记录不弹窗，
  否则每次回主屏都会一口气弹三条。
- **Montserrat 里没有汉字，也没有到中文字库的回退**（回退是单向的：
  `lv_font_simsun_16_cjk.fallback = &lv_font_montserrat_14`，反过来没有）。
  挂 `lv_font_montserrat_*` 的 label 写中文 = 一片空白。要显示中文就显式换
  `&lv_font_simsun_16_cjk`；纯 ASCII 的占位符（`--`）才可以用 Montserrat。
- 日期统一走 `formatDateCN()`（"9月27日 星期六"）。用了它的 label 必须是中文字库。
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
- 按键编码只有**一套**在矩阵里用：Arduino 的 "0x88 + HID usage"。
  0xE0~0xE7 在这里是**小键盘回车 / 小键盘 1~7**（0x88+0x58=0xE0=NumEnt，
  0x88+0x5F=0xE7=Num 7），**不是**修饰键 —— 别在 kbPress 里把它们当 0xE0~0xE7
  的 HID 修饰键处理，一处理整片小键盘就全错（7 会变成 Win 键）。
  网页"按键映射"下拉里那几个 "🔹 Left Ctrl"=224 才是 HID 风格，它们的换算
  统一在 `normalizeRemapKey()` 里做，只作用于 remap 表的读入/读出。
- **小键盘符号键（`/ * - +`）必须用 `0xDC~0xDF`，不能退回 ASCII**。
  这里踩过两次，两个方向的错都犯过：
  · 用 HID usage `0x53~0x63` → 和 ASCII `'a'/'b'/'c'`(0x61~0x63) 数值撞车，
    基础键盘按 A/B/C 会发成 Num 9 / Num 0 / Num .（见提交 `9973f87` 的 A→9 修复）。
  · 改用 ASCII 规避撞车 → `*`=42 走 `_asciimap` 变成**主键盘 Shift+8**、
    `+`=43 变成 Shift+=，发出来的压根不是小键盘那颗键（Mac 上数字小键盘就是废的）。
  正解是 `0x88 + HID usage`：`Num /`=0xDC `Num *`=0xDD `Num -`=0xDE `Num +`=0xDF。
  这一串既和 a/b/c 不撞车，又和网页"🔹 修饰键"224~231 不撞车，走 `kbPress` 的
  `>=0x88` 分支被减回 `0x54~0x57` 交给 `pressRaw`，发出的是**真正的小键盘键**。
  效果上等于"宏里选 Num \*"和"手按物理小键盘的 \*"发同一个码点。
  小键盘数字（1-9/0/`.`）仍用 ASCII —— `_asciimap` 翻成主键盘数字，字符是对的；
  换成 `0xE0~0xE7` 反而会被 `normalizeRemapKey()` 当修饰键折算掉。
  ⚠ 网页 `s3-setting.html` 的 `KEY_OPTIONS` 是同一套值的另一个副本，**改一处要改两处**。
- 同一个坑的第二个面：**别拿 Consumer 页的 usage 去比 `baseKey`**。
  0xE2 = 0x88+0x5A = **小键盘 2**，不是静音 —— 谁在按键分发里写
  `else if (baseKey == 0xE2)` 做 Mute，小键盘 2 就会静音且不再当数字键用。
  静音键物理上挂在小 MCU **C3** 上，走 Serial1 的 `BTN:MUTE`
  （`handleC3Command`），**矩阵里没有这一格**，不要试图在矩阵里加。

## 灯效

- 灯光设置页五个字段：背光开关 / 背光亮度 / 灯效 / 状态灯亮度 / **按键灯效**。
- 按键灯效（`keyFxStyle`，存 NVS `key_fx`）是**叠加层**：按下任意键时
  `triggerKeyReaction()` 置状态，`renderLightingEngine()` 每帧把 0~15 主背光
  先压暗到 ~40%（原版 `updateKeyReaction()` 的 `*102>>8`），再把特效那几颗点亮。
  涟漪=从正中向两侧扩散 / 发射=从末位往回扫子弹 / 堆叠=从末位往回填。
  颜色每按一次自动换一种（原版"自动"档的配色）。
- 叠加层必须写在基础灯效**之后**，顺序反了会被灯效盖掉；告警期间整段不走
  （`renderLightingEngine()` 在告警分支就 return 了），16~18 锁状态灯也不受影响。
- 主背光一共 **16 颗（0~15）**，16/17/18 是三颗锁状态灯。别再把 `0~15` 写成
  写死的 `15`（`i < 15`）—— 那样第 16 颗主灯在常亮灯效下永远是黑的。

## 息屏 / 屏保

- 菜单第 12 项在 黑屏 → 壁纸轮播 → 信息面板 之间循环（存 NVS `saver_mode`）。
- **壁纸轮播**：壁纸铺满 + 每 5 秒翻出信息面板。信息面板（时间/日期/温湿度）
  全在 `sv_panel` 一块卡里 —— 旧版是四个 label 散在屏幕各处，配上壁纸就是
  四处压图。没上传过壁纸时没有可轮播的图，直接常显面板。
- 进屏保先给图片（`updateScreensaver(true)` 里 `tick = 0` + 藏面板），
  用户最想先看到的是图。轮播计时用 tick 计数而不是时间戳：这个函数既被
  100ms 的节拍调、也被 `enterScreensaver()` 直接调一次，时间戳会被那次调用搅乱。
- 壁纸主屏（风格 6）的时钟在**右下角**一小块（112x40 / 28px 字），
  遮罩是 `LV_OPA_25`。原来 168x64 的板子杵在正中、48px 字，图基本没法看。

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
