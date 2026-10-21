// 自动生成，请勿手改。由 gen_lock_img.py 生成。
#pragma once
#include <stdint.h>

#define LOCK_IMG_W 132
#define LOCK_IMG_H 176

// RGB565 像素数组（放在 flash，lv_img_dsc.data 直接指它）
extern const uint16_t lock_img_pixels[LOCK_IMG_W * LOCK_IMG_H];
