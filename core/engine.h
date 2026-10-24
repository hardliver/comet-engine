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

// 開發工具用（Studio 的「狀態」分頁）：在正在執行的遊戲裡跑一段 Lua（mode 見 script.h 的 script_eval）。
// 指令（mode 0）會改變遊戲狀態，所以正在錄的重播會停掉（之後的重播已經不是單純的輸入結果）；
// mode 1、2 只讀值，不停。
// 成功回傳 0；結果或錯誤訊息用 engine_eval_result() 取得
int engine_eval(const char *src, size_t len, int mode);
const char *engine_eval_result(void);

// ---- 運算預算（Level 1 = Pico 2，RP2350 150 MHz）----
// 開發工具用：開著時每幀記錄 Lua 指令數、畫了幾個像素、呼叫了幾次繪圖 API，換算成「預估 Pico 要花幾個 CPU 週期」。
// 換算係數是估計值（Lua 5.4 在 Cortex-M33 上一個指令約 40 週期、寫一個像素約 8 週期、一次 Lua→C 呼叫約 300 週期），
// 硬體到了之後（路線圖 2-4）用實機量測校正。一幀的預算 = 150 MHz ÷ 60 = 2,500,000 週期。
#define L1_CYCLES_PER_FRAME 2500000u
#define L1_CYC_PER_INSTR 40u
#define L1_CYC_PER_PIXEL 8u
#define L1_CYC_PER_CALL 300u
void engine_profile(int on);
uint32_t engine_prof_instr(void);    // 上一幀的 Lua 指令數
uint32_t engine_prof_pixels(void);   // 上一幀畫的像素
uint32_t engine_prof_calls(void);    // 上一幀的繪圖 API 呼叫次數
uint32_t engine_prof_load(void);     // 上一幀的預估負載（千分比；1000 = 剛好用完 Pico 一幀的時間）

// 平台用：每一幀呼叫一次（每秒 60 次）
void engine_frame(void);

// 目前是第幾幀（從 0 開始）。遊戲要計時就用它，不用系統時間
uint32_t engine_frame_count(void);

// 跑一幀但不呼叫 _draw（畫面維持上一張）。主機畫不完一幀時用來追上進度：
// 遊戲邏輯仍是每秒 60 次。注意：_draw 裡如果改了遊戲狀態，跳過時就不會改（遊戲狀態應該只在 _update 改）
void engine_frame_nodraw(void);

// 量時間（主機調效能用）：fn 回傳「現在幾微秒」；之後每幀記下 _update、_draw、聲音各花多久
void engine_set_timer(uint32_t (*fn)(void));
void engine_last_times(uint32_t *update_us, uint32_t *draw_us, uint32_t *audio_us);

#endif
