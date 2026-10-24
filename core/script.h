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

// 平台可以提供另一種載入方式（主機：程式已經在唯讀區，不需要編譯原始碼）。
// 設定之後，script_load 不編譯 src，改呼叫 loader：成功要在堆疊上留下主函式、回傳 0；
// 失敗在堆疊上留下錯誤訊息、回傳非 0。設成 NULL 恢復一般方式。
// src 仍然要傳（引擎用它算遊戲指紋、讀 id），內容可以只是卡匣的開頭幾行。
struct lua_State;   // 只用到指標，不需要整個 lua.h
typedef int (*ScriptLoader)(struct lua_State *L, void *ud);
void script_set_loader(ScriptLoader loader, void *ud);

// 設定下一次 script_load() 時 rnd() 的亂數種子（0 會被換成 1）
void script_set_seed(uint32_t seed);

// 遊戲已經在跑：馬上換掉亂數的狀態（主機用：載入時用固定種子，載入後才換成隨機的。
// 這樣載入時放進 Flash 的東西每次開機都一樣，不用重寫；見 platform/pico/main.c）。
// 注意：重播檔只記載入時的種子，用了這個就不能重播
void script_reseed(uint32_t seed);

// 只編譯、不執行：檢查語法。用一個暫時的 Lua 環境，不影響正在執行的遊戲。
// 沒錯回傳 0；有錯回傳 1，錯誤訊息寫進 err（最多 err_size byte，含結尾的 0）
int script_check(const char *src, size_t len, const char *name, char *err, size_t err_size);

// 呼叫遊戲裡名叫 fn 的全域函式；不存在就跳過
void script_call(const char *fn);

// 遊戲是否出錯（出錯後就不再呼叫遊戲）
int script_failed(void);

// 遊戲的 Lua 目前用了多少記憶體（bytes，含還沒回收的垃圾）。沒有遊戲回傳 0
size_t script_mem(void);

// 除錯用（Studio 的「狀態」分頁）：在正在執行的遊戲裡跑一段 Lua。
// mode 0：執行一行指令。先試「return 指令」（算式：顯示結果），不行再當一般敘述執行
// mode 2：和 0 一樣，但只是讀值（Studio 自己要用的，不停掉重播）
// mode 1：讀遊戲的全域表格 DEBUG_VIEW（一串算式字串，例如 { "DAY", "party" }），
//         把每個值攤成一行一行：「層數 \t 路徑 \t 名稱 \t 值」，表格的值是 {幾項}
// 結果寫進 out（最多 cap byte，含結尾的 0）。成功回傳 0，出錯回傳 1（out 是錯誤訊息）
int script_eval(const char *src, size_t len, int mode, char *out, size_t cap);

// 運算預算：開著時累計 Lua 指令數（每 100 個算一次）、繪圖 API 呼叫次數。engine.c 每幀讀、歸零
extern uint32_t script_instr;
extern uint32_t script_draw_calls;
void script_profile(int on);

// 平台的看門狗（主機除錯用）：Lua 每執行 every_instr 個指令，呼叫 fn 一次，告訴它現在在哪一行
// （例如 "game:1234"）。平台可以用來發現「一幀跑太久、卡在哪裡」
typedef void (*ScriptWatch)(const char *where);
void script_set_watch(ScriptWatch fn, unsigned every_instr);

// 給 C 這邊要「一大塊」記憶體的地方用（地圖圖層的複本、圖片）：malloc 失敗時，
// 先叫 Lua 做一次完整的垃圾回收、把不用的記憶體還回來，再試一次。還是不夠才回傳 NULL。
// 主機只有約 230 KB 可用，Lua 的垃圾常常占一大塊；不先回收，C 這邊就會「明明夠卻要不到」。
// 只能在「Lua 呼叫進來的 C 函式」裡用（那時候回收是安全的）。
void *script_malloc(size_t n);

// 出錯時在錯誤訊息後面附上呼叫堆疊（包含協程裡的）。要在載入遊戲前設定。預設關閉（主機打開）
void script_set_traceback(int on);

#endif
