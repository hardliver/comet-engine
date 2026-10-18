// input.c — 按鍵輸入的實作

#include "input.h"

// 每位玩家 2 bytes，16 個按鍵，一個 bit 一個鍵
static uint16_t buttons[MAX_PLAYERS];

void input_set(int player, uint16_t b)
{
    if (player < 0 || player >= MAX_PLAYERS) {
        return;
    }
    buttons[player] = b;
}

int input_btn(int player, int button)
{
    if (player < 0 || player >= MAX_PLAYERS) {
        return 0;
    }
    if (button < 0 || button >= MAX_BUTTONS) {
        return 0;
    }
    // 把第 button 個 bit 移到最右邊，再只留下最右邊那個 bit
    return (buttons[player] >> button) & 1;
}
