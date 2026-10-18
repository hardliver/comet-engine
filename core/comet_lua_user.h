// comet_lua_user.h — Comet 對 Lua 的設定
// 編譯 Lua 時用 -DLUA_USER_H 帶入，Lua 的 lua.h 會 include 這個檔。
// 用意：不改 Lua 原始碼，只「覆蓋」它預留的幾個設定點。

#ifndef COMET_LUA_USER_H
#define COMET_LUA_USER_H

#include <stddef.h>

// ---- 1. 同步（determinism）----

// Lua 的字串雜湊預設混入時間和記憶體位址當種子，每次執行都不同，
// 會讓 pairs() 走訪表格的順序每次不一樣。固定成 0。
#define luai_makeseed(L)    0u

// table.sort 對大陣列會用時間挑基準點，同值元素的排序結果會不同。固定成 0。
#define l_randomizePivot()  0u

// ---- 2. 輸出 ----

// Lua 的 print() 和錯誤訊息預設寫到 stdout／stderr，
// 但核心不碰終端機，所以導向 script.c 的函式，再交給平台。
void comet_lua_write(const char *s, size_t len);
void comet_lua_write_error(const char *fmt, const char *arg);

#define lua_writestring(s, l)          comet_lua_write((s), (l))
#define lua_writeline()                comet_lua_write("\n", 1)
#define lua_writestringerror(s, p)     comet_lua_write_error((s), (p))

#endif
