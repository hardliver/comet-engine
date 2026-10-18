// script.c — Lua 和引擎之間的橋接
//
// 兩個方向：
//   引擎 → Lua：呼叫遊戲的 _init、_update、_draw（script_call）
//   Lua → 引擎：遊戲呼叫 cls、rect、btn 等，實際執行的是這裡的 C 函式

#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "script.h"
#include "engine.h"
#include "gfx.h"
#include "input.h"
#include "platform.h"

static lua_State *L = NULL;   // 目前這款遊戲的 Lua 執行環境
static int failed = 0;

// ============================================================
// 1. 給 Lua 呼叫的 API
// ============================================================
// 每個函式的格式都一樣：
//   參數放在 Lua 的「堆疊」上，第 1 個參數在位置 1，依此類推
//   要回傳的值推上堆疊，函式 return「回傳了幾個值」
//
// luaL_checkinteger：參數必須是整數，否則遊戲會出錯並指出哪一行。
// 傳 1.5 這種有小數的數字會被擋下來，這是刻意的：
// 規格要求遊戲只用整數，各平台的結果才會一致。

// cls(color)：清除畫面，color 可省略（預設 0 黑色）
static int l_cls(lua_State *L)
{
    lua_Integer c = luaL_optinteger(L, 1, 0);
    gfx_cls((uint8_t)c);
    return 0;
}

// pset(x, y, color)：畫一個點
static int l_pset(lua_State *L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer c = luaL_checkinteger(L, 3);
    gfx_pset((int)x, (int)y, (uint8_t)c);
    return 0;
}

// rect(x, y, w, h, color)：畫實心矩形
static int l_rect(lua_State *L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer w = luaL_checkinteger(L, 3);
    lua_Integer h = luaL_checkinteger(L, 4);
    lua_Integer c = luaL_checkinteger(L, 5);
    gfx_rect((int)x, (int)y, (int)w, (int)h, (uint8_t)c);
    return 0;
}

// btn(player, button)：按著回傳 true
static int l_btn(lua_State *L)
{
    lua_Integer p = luaL_checkinteger(L, 1);
    lua_Integer b = luaL_checkinteger(L, 2);
    lua_pushboolean(L, input_btn((int)p, (int)b));
    return 1;
}

// frame()：目前是第幾幀。遊戲要計時就用它，不用系統時間
static int l_frame(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)engine_frame_count());
    return 1;
}

static const luaL_Reg api[] = {
    { "cls",   l_cls   },
    { "pset",  l_pset  },
    { "rect",  l_rect  },
    { "btn",   l_btn   },
    { "frame", l_frame },
    { NULL, NULL }
};

// ============================================================
// 2. 建立沙盒：只開放安全、不會破壞同步的功能
// ============================================================

static void open_libs(lua_State *L)
{
    // 只開這幾個標準函式庫；io、os、package、debug 根本沒有編譯進來
    static const luaL_Reg libs[] = {
        { LUA_GNAME,       luaopen_base      },  // print、pairs、tostring…
        { LUA_COLIBNAME,   luaopen_coroutine },
        { LUA_TABLIBNAME,  luaopen_table     },
        { LUA_STRLIBNAME,  luaopen_string    },
        { LUA_MATHLIBNAME, luaopen_math      },
        { LUA_UTF8LIBNAME, luaopen_utf8      },
        { NULL, NULL }
    };
    for (const luaL_Reg *lib = libs; lib->func != NULL; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    // 拿掉會讀檔案的函式
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    // 拿掉亂數：每台機器結果不同，連線會不同步。之後由引擎提供同步亂數
    lua_getglobal(L, "math");
    lua_pushnil(L); lua_setfield(L, -2, "random");
    lua_pushnil(L); lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    // 註冊 Comet 的 API，變成全域函式
    lua_pushglobaltable(L);
    luaL_setfuncs(L, api, 0);
    lua_pop(L, 1);

    // 畫面大小也給遊戲用
    lua_pushinteger(L, SCREEN_W); lua_setglobal(L, "SCREEN_W");
    lua_pushinteger(L, SCREEN_H); lua_setglobal(L, "SCREEN_H");
}

// ============================================================
// 3. 載入與呼叫遊戲
// ============================================================

// 遊戲出錯：印出錯誤訊息，之後不再執行遊戲
static void report_error(void)
{
    const char *msg = lua_tostring(L, -1);
    platform_log(msg != NULL ? msg : "（沒有錯誤訊息）");
    lua_pop(L, 1);
    failed = 1;
}

int script_load(const char *src, size_t len, const char *name)
{
    if (L != NULL) {
        lua_close(L);   // 關掉上一款遊戲，釋放它用的全部記憶體
    }
    failed = 0;

    L = luaL_newstate();
    if (L == NULL) {
        platform_log("記憶體不足，無法啟動 Lua");
        failed = 1;
        return 1;
    }
    open_libs(L);

    // 編譯原始碼。"t" 代表只接受文字，不接受預先編譯好的二進位檔（比較安全）
    if (luaL_loadbufferx(L, src, len, name, "t") != LUA_OK) {
        report_error();   // 語法錯誤
        return 1;
    }
    // 執行檔案最外層的程式碼（定義函式、設定初始值）
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        report_error();
        return 1;
    }

    script_call("_init");
    return failed;
}

void script_call(const char *fn)
{
    if (L == NULL || failed) {
        return;
    }
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);   // 遊戲沒寫這個函式，跳過
        return;
    }
    // pcall = protected call：遊戲出錯時不會讓整個引擎當掉，而是回傳錯誤
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        report_error();
    }
}

int script_failed(void)
{
    return failed;
}

// ============================================================
// 4. Lua 的輸出導向（comet_lua_user.h 裡設定）
// ============================================================
// print("a", "b") 會分好幾次呼叫 comet_lua_write：
// "a"、"\t"、"b"、"\n"。先存起來，遇到換行再整行交給平台。

static char line[256];
static size_t line_len = 0;

static void flush_line(void)
{
    line[line_len] = '\0';
    platform_log(line);
    line_len = 0;
}

void comet_lua_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            flush_line();
        } else {
            if (line_len == sizeof(line) - 1) {
                flush_line();   // 一行太長，先送出
            }
            line[line_len++] = s[i];
        }
    }
}

// Lua 內部的警告，格式是 "....%s...."，把 %s 換成 arg
void comet_lua_write_error(const char *fmt, const char *arg)
{
    const char *p = strstr(fmt, "%s");
    if (p == NULL) {
        comet_lua_write(fmt, strlen(fmt));
        return;
    }
    comet_lua_write(fmt, (size_t)(p - fmt));
    comet_lua_write(arg, strlen(arg));
    comet_lua_write(p + 2, strlen(p + 2));
}
