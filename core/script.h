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

#endif
