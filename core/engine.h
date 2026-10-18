// engine.h — 引擎主體：管理「一幀」
// 平台每秒呼叫 engine_frame() 60 次，引擎再去呼叫遊戲。

#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

#define FPS 60

// 平台用：開始時呼叫一次
void engine_init(void);

// 平台用：每一幀呼叫一次（每秒 60 次）
void engine_frame(void);

// 目前是第幾幀（從 0 開始）。遊戲要計時就用它，不用系統時間
uint32_t engine_frame_count(void);

// ---- 以下三個由「遊戲」提供，引擎負責在對的時間呼叫 ----
void game_init(void);     // 開始時一次：設定初始狀態
void game_update(void);   // 每幀一次：讀按鍵、移動、判斷碰撞
void game_draw(void);     // 每幀一次：把目前狀態畫出來

#endif
