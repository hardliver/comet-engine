// script.h — 執行 Lua 遊戲
// 遊戲是一個 Lua 文字檔，裡面寫三個函式（都可以省略）：
//   _init()    開始時一次
//   _update()  每幀一次：讀按鍵、改狀態
//   _draw()    每幀一次：畫出目前狀態

#ifndef SCRIPT_H
#define SCRIPT_H

#include <stddef.h>
#include <stdint.h>

// 載入並執行遊戲原始碼（會先關掉上一款遊戲）。
// name 是錯誤訊息裡顯示的檔名。is_system：系統卡匣（多一個 sys 表格）。成功回傳 0
int script_load(const char *src, size_t len, const char *name, int is_system);

// 設定下一次 script_load() 時 rnd() 的亂數種子（0 會被換成 1）
void script_set_seed(uint32_t seed);

// 只編譯、不執行：檢查語法。用一個暫時的 Lua 環境，不影響正在執行的遊戲。
// 沒錯回傳 0；有錯回傳 1，錯誤訊息寫進 err（最多 err_size byte，含結尾的 0）
int script_check(const char *src, size_t len, const char *name, char *err, size_t err_size);

// 呼叫遊戲裡名叫 fn 的全域函式；不存在就跳過
void script_call(const char *fn);

// 遊戲是否出錯（出錯後就不再呼叫遊戲）
int script_failed(void);

// 除錯用（Studio 的「狀態」分頁）：在正在執行的遊戲裡跑一段 Lua。
// mode 0：執行一行指令。先試「return 指令」（算式：顯示結果），不行再當一般敘述執行
// mode 2：和 0 一樣，但只是讀值（Studio 自己要用的，不停掉重播）
// mode 1：讀遊戲的全域表格 DEBUG_VIEW（一串算式字串，例如 { "DAY", "party" }），
//         把每個值攤成一行一行：「層數 \t 路徑 \t 名稱 \t 值」，表格的值是 {幾項}
// 結果寫進 out（最多 cap byte，含結尾的 0）。成功回傳 0，出錯回傳 1（out 是錯誤訊息）
int script_eval(const char *src, size_t len, int mode, char *out, size_t cap);

#endif
