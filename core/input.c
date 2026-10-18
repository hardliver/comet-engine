// input.c — 按鍵與搖桿輸入的實作

#include "input.h"

// 每位玩家：16 個按鍵（一個 bit 一個鍵）+ 2 支搖桿（各有 x、y）
static uint16_t buttons[MAX_PLAYERS];
static int8_t   sticks[MAX_PLAYERS][2][2];   // [玩家][搖桿][0=x, 1=y]

static int valid_player(int p) { return p >= 0 && p < MAX_PLAYERS; }
static int valid_stick(int s)  { return s == STICK_LEFT || s == STICK_RIGHT; }

static int clamp_stick(int v)
{
    if (v < -STICK_MAX) return -STICK_MAX;
    if (v >  STICK_MAX) return  STICK_MAX;
    return v;
}

void input_set(int player, uint16_t b)
{
    if (!valid_player(player)) return;
    buttons[player] = b;
}

void input_set_stick(int player, int stick, int x, int y)
{
    if (!valid_player(player) || !valid_stick(stick)) return;
    sticks[player][stick][0] = (int8_t)clamp_stick(x);
    sticks[player][stick][1] = (int8_t)clamp_stick(y);
}

int input_btn(int player, int button)
{
    if (!valid_player(player)) return 0;
    if (button < 0 || button >= MAX_BUTTONS) return 0;

    // 先看按鍵本身：把第 button 個 bit 移到最右邊，再只留下最右邊那個 bit
    if ((buttons[player] >> button) & 1) return 1;

    // 方向鍵：左搖桿推得夠遠也算
    int x = sticks[player][STICK_LEFT][0];
    int y = sticks[player][STICK_LEFT][1];
    switch (button) {
        case BTN_LEFT:  return x <= -STICK_AS_BUTTON;
        case BTN_RIGHT: return x >=  STICK_AS_BUTTON;
        case BTN_UP:    return y <= -STICK_AS_BUTTON;
        case BTN_DOWN:  return y >=  STICK_AS_BUTTON;
        default:        return 0;
    }
}

int input_stick_x(int player, int stick)
{
    if (!valid_player(player) || !valid_stick(stick)) return 0;
    return sticks[player][stick][0];
}

int input_stick_y(int player, int stick)
{
    if (!valid_player(player) || !valid_stick(stick)) return 0;
    return sticks[player][stick][1];
}
