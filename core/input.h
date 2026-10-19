// input.h — 按鍵與搖桿輸入
// 平台負責「把輸入狀態交進來」，遊戲負責「讀輸入狀態」。
// 核心不知道輸入來自鍵盤、手把、觸控還是網路。

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define MAX_PLAYERS 4
#define MAX_BUTTONS 16   // 每位玩家 16 個按鍵（2 bytes），目前用到 13 個

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
    BTN_L      = 10,   // 左肩鍵（LB）
    BTN_R      = 11,   // 右肩鍵（RB）
    BTN_HOME   = 12,   // HOME（手把中間的 Guide 鍵）：在 Nova 裡回主選單
};

// 搖桿編號
enum {
    STICK_LEFT  = 0,
    STICK_RIGHT = 1,
};

// 搖桿數值範圍：-127（左／上）到 127（右／下），0 是中間。
// 用整數而不是小數，各平台結果才會一致
#define STICK_MAX 127

// 左搖桿推超過這個值，就同時算成按了方向鍵。
// 這樣只用 btn() 的遊戲，用搖桿也能玩
#define STICK_AS_BUTTON 64

// ---- 平台用 ----

// 設定某位玩家這一幀的按鍵狀態。第 n 個 bit 是 1 就是第 n 號鍵按著
void input_set(int player, uint16_t buttons);

// 設定某位玩家某支搖桿這一幀的位置（超出範圍會被限制在 ±127）
void input_set_stick(int player, int stick, int x, int y);

// 讀回某位玩家目前的按鍵 bit（重播錄影用）
uint16_t input_get_buttons(int player);

// ---- 遊戲用 ----

// 某個按鍵是不是按著（1 = 按著，0 = 沒按）。
// 方向鍵：十字鍵按著，或左搖桿推過一半，都算
int input_btn(int player, int button);

// 搖桿位置
int input_stick_x(int player, int stick);
int input_stick_y(int player, int stick);

#endif
