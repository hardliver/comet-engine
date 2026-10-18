// gfx.c — 繪圖模組的實作
// 這個檔案只做「把顏色編號寫進陣列」，不知道畫面最後會出現在哪裡。

#include "gfx.h"

// 畫面：每個像素 1 byte，存的是顏色編號（0–255）
// static：只有這個檔案能直接碰它，別人只能透過函式
static uint8_t framebuffer[SCREEN_W * SCREEN_H];

// 調色盤：256 色，每色 R、G、B 各 1 byte
// 只填前 16 色，其餘預設為 0（黑色）。正式調色盤留到規格 v0.1
static uint8_t palette[256 * 3] = {
      0,   0,   0,   // 0  黑
     29,  43,  83,   // 1  深藍
    126,  37,  83,   // 2  深紫
      0, 135,  81,   // 3  深綠
    171,  82,  54,   // 4  棕
     95,  87,  79,   // 5  深灰
    194, 195, 199,   // 6  淺灰
    255, 241, 232,   // 7  白
    255,   0,  77,   // 8  紅
    255, 163,   0,   // 9  橘
    255, 236,  39,   // 10 黃
      0, 228,  54,   // 11 綠
     41, 173, 255,   // 12 藍
    131, 118, 156,   // 13 灰紫
    255, 119, 168,   // 14 粉紅
    255, 204, 170,   // 15 膚色
};

void gfx_cls(uint8_t color)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        framebuffer[i] = color;
    }
}

void gfx_pset(int x, int y, uint8_t color)
{
    // 超出畫面就直接離開。C 不會幫你檢查陣列邊界，
    // 寫到陣列外面會默默蓋掉別的資料，這種 bug 最難找。
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        return;
    }
    framebuffer[y * SCREEN_W + x] = color;
}

void gfx_rect(int x, int y, int w, int h, uint8_t color)
{
    // 先算出矩形的右下角（不含）
    int x1 = x + w;
    int y1 = y + h;

    // 裁切：把矩形縮到畫面範圍內
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;

    // 裁切後已在畫面內，可以直接寫陣列，不用每點再檢查
    for (int py = y; py < y1; py++) {
        for (int px = x; px < x1; px++) {
            framebuffer[py * SCREEN_W + px] = color;
        }
    }
}

const uint8_t *gfx_framebuffer(void)
{
    return framebuffer;
}

const uint8_t *gfx_palette(void)
{
    return palette;
}
