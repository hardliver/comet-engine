// system.h — 系統卡匣模式：Nova 的主選單（HOME、大廳、商城）也是一個 Comet 遊戲
//
// 兩種卡匣：
//   系統卡匣：engine_load_system() 載入，Lua 裡多一個 sys 表格（列出遊戲、啟動遊戲）
//   一般遊戲：engine_load() 載入，沒有 sys，碰不到其他遊戲、也碰不到系統
//
// 隔離的方法：同一時間只有一個 Lua 環境。
//   啟動遊戲時，系統卡匣的 Lua 環境整個關掉，再為遊戲開一個全新的；
//   回 HOME 時反過來，重新載入系統卡匣。遊戲不可能拿到系統的任何東西，因為它們不同時存在。
//   系統卡匣要記住的東西（游標停在哪一款）：用 sys.last_game()，或存檔
//
// 引擎不自己讀檔案、不下載：「載入哪個檔案」由平台決定。
// 系統卡匣呼叫 sys.launch(id) 時，引擎只記下「請求」；平台每一幀之後問 system_request()，
// 自己去拿遊戲原始碼（網頁 fetch、Mac 讀檔、主機讀 SD 卡），再呼叫 engine_load()。
// 這樣網頁的非同步下載也不會卡住引擎。
//
// 回 HOME：遊戲執行中按 HOME 鍵（手把的 Guide 鍵），或同時按住 Start＋Select 0.5 秒。
// 只有載入過系統卡匣（Nova 模式）才有作用；單獨玩一款遊戲時，這些按鍵照常交給遊戲。

#ifndef SYSTEM_H
#define SYSTEM_H

#ifndef SYSTEM_MAX_GAMES
#define SYSTEM_MAX_GAMES 64   // 平台可以用編譯參數改小（主機 RAM 少）
#endif
#define SYSTEM_ID_MAX    63
#define SYSTEM_NAME_MAX  63
#define SYSTEM_DESC_MAX  95      // 一句話介紹（UTF-8，中文約 30 字）
#define SYSTEM_HOME_HOLD 30      // Start＋Select 要按住幾幀（30 幀 = 0.5 秒）

// 平台要做的事
enum {
    SYS_REQ_NONE   = 0,
    SYS_REQ_LAUNCH = 1,   // 載入 system_request_game() 這款遊戲（engine_load）
    SYS_REQ_HOME   = 2,   // 回 HOME：重新載入系統卡匣（engine_load_system）
};

struct lua_State;

// ---- 平台用 ----

// 遊戲清單：系統卡匣用 sys.games() 看到的就是這份。名稱是 UTF-8
void system_clear_games(void);
int system_add_game(const char *id, const char *name);   // 成功回傳 0；滿了或 id 太長回傳 1
// 補上大廳要顯示的資訊（可以不呼叫）：一句話介紹、幾人遊玩（0 = 沒寫）。找不到 id 回傳 1
int system_set_info(const char *id, const char *desc, int players);

// 每一幀之後問一次：要做什麼？
int system_request(void);
const char *system_request_game(void);   // SYS_REQ_LAUNCH 時：遊戲 id
void system_request_done(void);          // 做完了（或做不到），清掉請求

// ---- 引擎內部用 ----

void system_on_load(int is_system);       // engine_load／engine_load_system 時
int system_is_running(void);              // 目前跑的是不是系統卡匣
void system_before_frame(void);           // 檢查回 HOME 的按鍵
void system_open_lua(struct lua_State *L); // 建立 sys 表格（只有系統卡匣）

#endif
