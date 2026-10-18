// gfx.h — 繪圖模組的「說明書」
// 別的檔案只要 #include "gfx.h"，就知道有哪些函式可以用。

#ifndef GFX_H   // 防止同一個檔案被 include 兩次
#define GFX_H

#include <stdint.h>  // 提供 uint8_t（0–255 的整數，剛好 1 byte）

// 遊戲畫面大小（暫定，規格 v0.1 才凍結）
#define SCREEN_W 320
#define SCREEN_H 240

// 清除整個畫面，填滿 color
void gfx_cls(uint8_t color);

// 在 (x, y) 畫一個點；超出畫面就不畫
void gfx_pset(int x, int y, uint8_t color);

// 畫實心矩形，左上角 (x, y)，寬 w、高 h；超出畫面的部分不畫
void gfx_rect(int x, int y, int w, int h, uint8_t color);

// 給平台層讀取用：畫面陣列（SCREEN_W * SCREEN_H 個顏色編號）
const uint8_t *gfx_framebuffer(void);

// 給平台層讀取用：調色盤（256 色 × RGB 3 bytes）
const uint8_t *gfx_palette(void);

#endif
