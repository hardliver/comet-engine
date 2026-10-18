// input.h — 按鍵輸入
// 平台負責「把按鍵狀態交進來」，遊戲負責「讀按鍵狀態」。
// 核心不知道按鍵來自鍵盤、手把還是網路。

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define MAX_PLAYERS 4
#define MAX_BUTTONS 16   // 每位玩家 16 個按鍵（2 bytes），目前用到 10 個

// 按鍵編號（暫定，規格 v0.1 才凍結）
// 0–7 是紅白機也有的按鍵；8 以後是現代手把才有的
enum {
    BTN_LEFT   = 0,
    BTN_RIGHT  = 1,
    BTN_UP     = 2,
    BTN_DOWN   = 3,
    BTN_A      = 4,
    BTN_B      = 5,
    BTN_START  = 6,
    BTN_SELECT = 7,
    BTN_X      = 8,
    BTN_Y      = 9,
};

// 平台用：設定某位玩家這一幀的按鍵狀態。
// buttons 的每一個 bit 代表一個按鍵，第 n 個 bit 是 1 就是第 n 號鍵按著
void input_set(int player, uint16_t buttons);

// 遊戲用：某位玩家的某個按鍵是不是按著（1 = 按著，0 = 沒按）
int input_btn(int player, int button);

#endif
