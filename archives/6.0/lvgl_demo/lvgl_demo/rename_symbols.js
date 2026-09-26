/**
 * 字体符号重命名脚本 v2
 * 给每个字体 .c 文件的内部静态变量加唯一后缀，消除多文件符号冲突。
 *
 * 关键：struct 字段名（如 .glyph_bitmap = xxx 中的 glyph_bitmap）不能改，
 * 只能改等号右边的变量名，以及变量声明。
 *
 * 使用方式: node rename_symbols.js
 * 运行目录: lvgl_demo/lvgl_demo/
 */

const fs = require('fs');
const path = require('path');

const files = [
  { name: 'lv_font_sans16.c',      suffix: '_16' },
  { name: 'lv_font_sans20.c',      suffix: '_20' },
  { name: 'lv_font_sans28.c',      suffix: '_28' },
  { name: 'lv_font_sans_bold48.c', suffix: '_bold48' },
];

// 静态变量符号 (变量名，不是类型名，不是 struct 字段名)
const symbols = [
  'kern_classes',
  'kern_left_class_mapping',
  'kern_right_class_mapping',
  'kern_class_values',
  'cmaps',
  'unicode_list_1',
  'unicode_list_2',
  'unicode_list_3',
  'glyph_bitmap',
  'glyph_dsc',
  'font_dsc',
  'cache',
];

// 两种重命名策略：
// 1. 声明行（包含 "static "）：替换整行所有出现
// 2. 其他行：只替换 "= xxx" 中右边的变量引用（struct 字段 .glyph_bitmap 不在 "=" 右边）
function renameLine(line, symbol, suffix) {
  const replacer = symbol + suffix;

  // 策略1: 声明行 — static 声明包含这个符号 → 整行替换
  if (/^\s*static\b/.test(line) && line.includes(symbol)) {
    // 使用单词边界替换所有出现（声明中的类型名/变量名都会改，这是对的）
    const regex = new RegExp(`(?<![\\w])${symbol}(?![\\w])`, 'g');
    return line.replace(regex, replacer);
  }

  // 策略2: 赋值/初始化行 (.field = variable) — 只替换 "=" 右边的引用
  // 匹配: 任意前缀 = SYMBOL, 或 = SYMBOL[
  // 前缀不包含 word char + . 组合（即不是 .field = 形式）
  // 先找 "= symbol" 或 "=symbol" 的模式
  const assignRegex = new RegExp(`(?<==\\s*)${symbol}(?![\\w])`, 'g');
  if (assignRegex.test(line)) {
    return line.replace(assignRegex, replacer);
  }

  // 策略3: &variable 或 , variable 在逗号分隔的上下文中（如 &kern_classes,
  // 用于初始化 &variable 形式）— 但排除 .field 形式
  // 简化：只处理 = 和 & 后面紧跟的情况
  const refRegex = new RegExp(`(?<![\\w.])&${symbol}(?![\\w])`, 'g');
  if (refRegex.test(line)) {
    return line.replace(refRegex, '&' + replacer);
  }

  return line;
}

files.forEach(({ name, suffix }) => {
  const filePath = path.join(__dirname, name);
  let lines = fs.readFileSync(filePath, 'utf8').split('\n');

  symbols.forEach(symbol => {
    lines = lines.map(line => renameLine(line, symbol, suffix));
  });

  fs.writeFileSync(filePath, lines.join('\n'), 'utf8');
  console.log(`[OK] ${name} ← 加后缀 ${suffix}`);
});

console.log('\n全部完成。请重新编译。');
