// save.h — 存檔：遊戲把資料存起來，下次開機再讀回來
//
// 遊戲這邊（Lua）：
//   save(欄位, 值)      把一個值存進第幾個欄位（0–7）。值可以是表格、字串、整數、true/false；
//                       save(欄位, nil) 刪除那個欄位
//   loadsave(欄位)      讀回來；沒有存檔回傳 nil
//
// 分工：
//   核心（這裡）：把 Lua 的值「編碼」成一串 bytes、再「解碼」回來；和重播檔配合
//   平台：只負責把 bytes 存到某個地方（網頁：瀏覽器；Mac：檔案；主機：SD 卡），
//         見 platform.h 的 platform_save_read／platform_save_write
//
// 存檔屬於哪個遊戲，看遊戲檔開頭的「-- id: 作者/遊戲」（合併工具會從 comet.json 的 id 寫進去）。
// 用 id 而不是遊戲指紋（SHA-256）：遊戲改版後指紋就變了，但存檔要能繼續用。
//
// 和重播的關係：讀進來的存檔會影響遊戲接下來怎麼跑，所以它和按鍵一樣是「輸入」。
//   錄影時：每次 loadsave 讀到的 bytes 都記進重播檔
//   播放時：loadsave 改從重播檔讀，save 不真的寫入（看別人的重播不會蓋掉自己的存檔）

#ifndef SAVE_H
#define SAVE_H

#include <stddef.h>

#define SAVE_SLOTS 8                 // 欄位 0–7
#define SAVE_MAX   (16 * 1024)       // 一個欄位最多 16 KB（編碼後）

struct lua_State;

// engine_load() 時呼叫：從遊戲原始碼開頭找出 id
void save_on_load(const char *src, size_t len);

// 目前遊戲的 id；遊戲沒寫 id 時是 "_"（所有沒有 id 的遊戲共用存檔）
const char *save_game_id(void);

// 把 save、loadsave 註冊成 Lua 的全域函式（script.c 建立 Lua 環境時呼叫）
void save_open_lua(struct lua_State *L);

#endif
