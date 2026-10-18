// engine.c — 引擎主體的實作

#include "engine.h"

static uint32_t frame_count = 0;

void engine_init(void)
{
    frame_count = 0;
    game_init();
}

void engine_frame(void)
{
    game_update();   // 先更新狀態
    game_draw();     // 再畫出來
    frame_count++;
}

uint32_t engine_frame_count(void)
{
    return frame_count;
}
