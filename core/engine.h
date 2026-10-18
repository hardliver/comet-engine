// engine.h — 引擎主體：管理「一幀」
// 平台每秒呼叫 engine_frame() 60 次，引擎再去呼叫遊戲。

#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>

#define FPS 60

// 平台用：開始時呼叫一次
void engine_init(void);

// 平台用：載入一款遊戲（Lua 原始碼）。成功回傳 0
int engine_load(const char *src, size_t len);

// 平台用：只檢查語法，不執行、不影響正在跑的遊戲（編輯器打字時用）。
// 沒錯回傳 0；有錯回傳 1，錯誤訊息用 engine_check_error() 取得
int engine_check(const char *src, size_t len);
const char *engine_check_error(void);

// 平台用：每一幀呼叫一次（每秒 60 次）
void engine_frame(void);

// 目前是第幾幀（從 0 開始）。遊戲要計時就用它，不用系統時間
uint32_t engine_frame_count(void);

#endif
