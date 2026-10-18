// gfx.h — 繪圖模組的「說明書」
// 別的檔案只要 #include "gfx.h"，就知道有哪些函式可以用。

#ifndef GFX_H   // 防止同一個檔案被 include 兩次
#define GFX_H

#include <stdint.h>  // 提供 uint8_t（0–255 的整數，剛好 1 byte）

// 遊戲畫面大小（暫定，規格 v0.1 才凍結）
#define SCREEN_W 320
#define SCREEN_H 240

// 最多同時載入幾張圖片
#define MAX_IMAGES 64

// 換遊戲時呼叫：調色盤回到預設、相機歸零、釋放所有圖片
void gfx_reset(void);

// ---- 基本繪圖 ----

// 清除整個畫面，填滿 color（不受相機影響）
void gfx_cls(uint8_t color);

// 在 (x, y) 畫一個點；超出畫面就不畫
void gfx_pset(int x, int y, uint8_t color);

// 畫實心矩形，左上角 (x, y)，寬 w、高 h；超出畫面的部分不畫
void gfx_rect(int x, int y, int w, int h, uint8_t color);

// ---- 調色盤 ----

// 設定第 index 號顏色。0–15 是預設的 16 色，16 以後通常給圖片用
void gfx_pal(int index, uint8_t r, uint8_t g, uint8_t b);

// ---- 相機 ----
// 設定之後，所有繪圖（cls 除外）都會往左上平移 (x, y)。
// 地圖比畫面大的時候，移動相機就是捲動畫面
void gfx_camera(int x, int y);

// ---- 圖片 ----
// 圖片的每個像素是一個顏色編號；編號 0 是「透明」，畫的時候會跳過。

// 建立一張圖片，複製 pixels（w × h 個顏色編號）。
// tile：切成幾 × 幾的小格（spr 用），通常是 16。
// 成功回傳圖片編號（1 起算），失敗回傳 0
int gfx_image_new(int w, int h, const uint8_t *pixels, int tile);

// 畫圖片的一部分：從圖片 (sx, sy) 取 sw × sh 的範圍，畫到畫面 (dx, dy)，
// 放大成 dw × dh（和 sw、sh 相同就是原尺寸）。flip_x、flip_y：左右、上下翻轉
void gfx_sspr(int img, int sx, int sy, int sw, int sh,
              int dx, int dy, int dw, int dh, int flip_x, int flip_y);

// 畫圖片的第 n 格（由左到右、由上到下，從 0 開始數）
void gfx_spr(int img, int n, int dx, int dy, int flip_x, int flip_y);

// 圖片的寬、高（不存在回傳 0）
int gfx_image_w(int img);
int gfx_image_h(int img);

// ---- 給平台層讀取用 ----

// 畫面陣列（SCREEN_W * SCREEN_H 個顏色編號）
const uint8_t *gfx_framebuffer(void);

// 調色盤（256 色 × RGB 3 bytes）
const uint8_t *gfx_palette(void);

#endif
