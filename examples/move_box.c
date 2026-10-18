// move_box.c — 第一個「遊戲」：用方向鍵移動方塊
// 之後遊戲會改用 Lua 寫；現在先用 C，把引擎的結構建起來。

#include "../core/engine.h"
#include "../core/gfx.h"
#include "../core/input.h"

#define BOX_SIZE 16
#define SPEED    2      // 每幀移動幾個像素

// 遊戲狀態：方塊左上角的位置
static int box_x;
static int box_y;

void game_init(void)
{
    // 放在畫面正中央
    box_x = (SCREEN_W - BOX_SIZE) / 2;
    box_y = (SCREEN_H - BOX_SIZE) / 2;
}

void game_update(void)
{
    // 讀 1 號玩家（編號 0）的方向鍵
    if (input_btn(0, BTN_LEFT))  box_x -= SPEED;
    if (input_btn(0, BTN_RIGHT)) box_x += SPEED;
    if (input_btn(0, BTN_UP))    box_y -= SPEED;
    if (input_btn(0, BTN_DOWN))  box_y += SPEED;

    // 不准離開畫面
    if (box_x < 0) box_x = 0;
    if (box_y < 0) box_y = 0;
    if (box_x > SCREEN_W - BOX_SIZE) box_x = SCREEN_W - BOX_SIZE;
    if (box_y > SCREEN_H - BOX_SIZE) box_y = SCREEN_H - BOX_SIZE;
}

void game_draw(void)
{
    gfx_cls(1);                          // 深藍背景
    gfx_rect(20, 20, 100, 60, 8);        // 紅色方塊（裝飾）
    gfx_rect(200, 150, 60, 60, 11);      // 綠色方塊（裝飾）

    // A 鍵按著時方塊變黃色，平常是白色
    uint8_t color = input_btn(0, BTN_A) ? 10 : 7;
    gfx_rect(box_x, box_y, BOX_SIZE, BOX_SIZE, color);

    // 畫面底部：8 個按鍵指示燈，按著的亮橘色，沒按的暗灰色
    // 順序：左 右 上 下 A B Start Select
    for (int b = 0; b < 8; b++) {
        uint8_t c = input_btn(0, b) ? 9 : 5;
        gfx_rect(8 + b * 14, SCREEN_H - 14, 10, 8, c);
    }
}
