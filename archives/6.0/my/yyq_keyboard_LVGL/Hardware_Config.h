#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

// ================= ST7789 240x240 SPI 屏幕 =================
#define TFT_SCL    4   // SPI SCK (时钟)
#define TFT_SDA   16   // SPI MOSI (数据)
#define TFT_DC    15   // 数据 / 命令选择
#define TFT_CS     5   // 片选 CS
#define TFT_RST   -1   // 硬件复位引脚 (接 3.3V 或由系统复位管理)

// ================= MCP23017 I2C 矩阵扩展芯片 =================
#define I2C_SDA   14
#define I2C_SCL   13
#define MCP23017_ADDR 0x20

// 矩阵行引脚 (直连 ESP32-S3 GPIO)
// 10 行 x 16 列 = 160 键位容量
#define ROW_PINS { 1, 2, 42, 41, 40, 39, 38, 47, 21, 12 }

// ================= SHT31 独立 I2C 总线 =================
#define SHT31_SDA 17
#define SHT31_SCL  7
#define SHT31_ADDR 0x44

// ================= 副板通信 UART =================
#define RX_PIN 10
#define TX_PIN  9
#define UART_BAUD 460800

#endif // HARDWARE_CONFIG_H
