// engine.h — 引擎主體：管理「一幀」
// 平台每秒呼叫 engine_frame() 60 次，引擎再去呼叫遊戲。

#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>

#define FPS 60

// 平台用：開始時呼叫一次
void engine_init(void);

// 平台用：設定下一次 engine_load() 的亂數種子。
// 沒設定就是 1（每次都一樣，自動測試用）；正式遊玩時平台每次給一個新的，
// 遊戲才不會每次開局都遇到一樣的事。種子會記錄在重播檔裡
void engine_set_seed(uint32_t seed);
uint32_t engine_seed(void);

// 平台用：載入一款遊戲（Lua 原始碼）。成功回傳 0。
// 正在準備播放重播檔、但遊戲對不上時回傳 2，目前的遊戲繼續跑
int engine_load(const char *src, size_t len);

// 平台用：載入系統卡匣（Nova 的主選單）。和 engine_load 一樣，
// 但 Lua 裡多一個 sys 表格，而且之後遊戲執行中可以回 HOME（見 system.h）。
// 系統卡匣不錄重播（重播只錄遊戲）
int engine_load_system(const char *src, size_t len);

// 目前遊戲原始碼的 SHA-256（32 bytes）：同一個遊戲、同一個版本才會相同
const uint8_t *engine_cart_sha(void);

// 平台用：只檢查語法，不執行、不影響正在跑的遊戲（編輯器打字時用）。
// 沒錯回傳 0；有錯回傳 1，錯誤訊息用 engine_check_error() 取得
int engine_check(const char *src, size_t len);
const char *engine_check_error(void);

// 平台用：每一幀呼叫一次（每秒 60 次）
void engine_frame(void);

// 目前是第幾幀（從 0 開始）。遊戲要計時就用它，不用系統時間
uint32_t engine_frame_count(void);

#endif
