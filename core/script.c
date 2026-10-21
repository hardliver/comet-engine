// script.c — Lua 和引擎之間的橋接
//
// 兩個方向：
//   引擎 → Lua：呼叫遊戲的 _init、_update、_draw（script_call）
//   Lua → 引擎：遊戲呼叫 cls、rect、btn 等，實際執行的是這裡的 C 函式

#include <stdio.h>    // snprintf
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "script.h"
#include "engine.h"
#include "gfx.h"
#include "map.h"
#include "input.h"
#include "platform.h"
#include "save.h"
#include "pak.h"
#include "system.h"

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
// ============================================================
// 運算預算（engine_profile 開著時才計）：Lua 執行了幾個指令、呼叫了幾次繪圖 API
// ============================================================
uint32_t script_instr = 0;
uint32_t script_draw_calls = 0;
static int profiling = 0;
#define HOOK_EVERY 100   // 每 100 個 Lua 指令叫一次 hook（每個指令都叫太慢）

static void count_hook(lua_State *L, lua_Debug *ar)
{
    (void)L; (void)ar;
    script_instr += HOOK_EVERY;
}

void script_profile(int on)
{
    profiling = on;
    if (L != NULL) lua_sethook(L, on ? count_hook : NULL, on ? LUA_MASKCOUNT : 0, HOOK_EVERY);
}

#define DRAW_CALL() (script_draw_calls++)

static int l_cls(lua_State *L)
{
    DRAW_CALL();
    lua_Integer c = luaL_optinteger(L, 1, 0);
    gfx_cls((uint8_t)c);
    return 0;
}

// pset(x, y, color)：畫一個點
static int l_pset(lua_State *L)
{
    DRAW_CALL();
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer c = luaL_checkinteger(L, 3);
    gfx_pset((int)x, (int)y, (uint8_t)c);
    return 0;
}

// rect(x, y, w, h, color)：畫實心矩形
static int l_rect(lua_State *L)
{
    DRAW_CALL();
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

// stick(player, n)：搖桿位置，回傳 x, y 兩個值（-127～127）
// n：0 = 左搖桿，1 = 右搖桿。往右、往下是正的（和畫面座標相同）
static int l_stick(lua_State *L)
{
    lua_Integer p = luaL_checkinteger(L, 1);
    lua_Integer n = luaL_optinteger(L, 2, 0);
    lua_pushinteger(L, input_stick_x((int)p, (int)n));
    lua_pushinteger(L, input_stick_y((int)p, (int)n));
    return 2;   // 回傳兩個值
}

// frame()：目前是第幾幀。遊戲要計時就用它，不用系統時間
static int l_frame(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)engine_frame_count());
    return 1;
}

// rnd(n)：0 到 n-1 的隨機整數。
// 用「固定種子」的亂數產生器（xorshift32），同一款遊戲每次執行、
// 每個平台產生的數列都一樣，連線和重播才不會不同步。
// 種子由平台在載入遊戲前決定（engine_set_seed），並記錄在重播檔裡
static uint32_t rng_state = 1;
static uint32_t load_seed = 1;   // 下一次載入遊戲時的種子

void script_set_seed(uint32_t seed)
{
    load_seed = seed ? seed : 1;   // xorshift 的狀態是 0 的話會永遠是 0
}

static uint32_t rng_next(void)
{
    // xorshift32：只用位移和 XOR，在任何 CPU 上結果都相同
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int l_rnd(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 1, 1, "must be at least 1");
    lua_pushinteger(L, (lua_Integer)(rng_next() % (uint32_t)n));
    return 1;
}

// ---- 調色盤、相機、圖片 ----

// pal(index, r, g, b)：設定第 index 號顏色（0–255），r g b 是 0–255
static int l_pal(lua_State *L)
{
    lua_Integer i = luaL_checkinteger(L, 1);
    lua_Integer r = luaL_checkinteger(L, 2);
    lua_Integer g = luaL_checkinteger(L, 3);
    lua_Integer b = luaL_checkinteger(L, 4);
    luaL_argcheck(L, i >= 0 && i <= 255, 1, "color index must be 0-255");
    gfx_pal((int)i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    return 0;
}

// dpal(from, to)：之後畫的顏色 from 換成 to；dpal({ [from] = to, … })：一次換好幾個；dpal()：全部還原
static int l_dpal(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        gfx_dpal_reset();
        return 0;
    }
    if (lua_istable(L, 1)) {
        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_isinteger(L, -2) && lua_isinteger(L, -1)) {
                gfx_dpal((int)lua_tointeger(L, -2), (int)lua_tointeger(L, -1));
            }
            lua_pop(L, 1);
        }
        return 0;
    }
    lua_Integer from = luaL_checkinteger(L, 1);
    lua_Integer to = luaL_checkinteger(L, 2);
    luaL_argcheck(L, from >= 0 && from <= 255, 1, "color index must be 0-255");
    luaL_argcheck(L, to >= 0 && to <= 255, 2, "color index must be 0-255");
    gfx_dpal((int)from, (int)to);
    return 0;
}

// palget(i)：調色盤第 i 號的顏色，回傳 r, g, b
static int l_palget(lua_State *L)
{
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i <= 255, 1, "color index must be 0-255");
    uint8_t r, g, b;
    gfx_palget((int)i, &r, &g, &b);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}

// imgcolors(圖, sx, sy, w, h)：這個範圍用到哪些顏色，回傳由小到大的陣列（不含透明的 0）。
// 範圍省略就是整張圖。用途：做換色（dpal）的對照表前，先知道要換哪些顏色
static int l_imgcolors(lua_State *L)
{
    int img = (int)luaL_checkinteger(L, 1);
    int sx = (int)luaL_optinteger(L, 2, 0);
    int sy = (int)luaL_optinteger(L, 3, 0);
    int w = (int)luaL_optinteger(L, 4, 1 << 20);
    int h = (int)luaL_optinteger(L, 5, 1 << 20);
    uint8_t used[256];
    gfx_image_colors(img, sx, sy, w, h, used);
    lua_newtable(L);
    int n = 0;
    for (int c = 1; c < 256; c++) {
        if (used[c]) {
            lua_pushinteger(L, c);
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

// camera(x, y)：之後的繪圖都往左上平移 (x, y)；camera() 歸零
static int l_camera(lua_State *L)
{
    gfx_camera((int)luaL_optinteger(L, 1, 0), (int)luaL_optinteger(L, 2, 0));
    return 0;
}

// base64 解碼：遊戲檔是文字檔，圖片資料用 base64 存成文字。
// 每 4 個字元代表 3 個 byte。回傳解出幾個 byte，格式錯誤回傳 -1
static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static long b64_decode(const char *in, size_t len, uint8_t *out, size_t out_max)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_max) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (long)n;
}

// newimage(w, h, data, tile)：建立圖片，回傳圖片編號。
// data 是 base64 文字，解開後是 w × h 個顏色編號（0 = 透明）。
// 通常不用自己寫：comet.json 的 "images" 會自動產生這一行
static int l_newimage(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1);
    lua_Integer h = luaL_checkinteger(L, 2);
    size_t len;
    const char *data = luaL_checklstring(L, 3, &len);
    lua_Integer tile = luaL_optinteger(L, 4, 16);
    luaL_argcheck(L, w > 0 && h > 0 && w <= 4096 && h <= 4096, 1, "bad image size");

    size_t size = (size_t)w * (size_t)h;
    // 暫時借用 Lua 的記憶體來放解碼結果，Lua 會自己回收
    uint8_t *buf = lua_newuserdatauv(L, size, 0);
    long n = b64_decode(data, len, buf, size);
    if (n != (long)size) {
        return luaL_error(L, "image data has %d pixels, expected %d", (int)n, (int)size);
    }
    int id = gfx_image_new((int)w, (int)h, buf, (int)tile);
    if (id == 0) return luaL_error(L, "cannot create image (too many images or out of memory)");
    lua_pushinteger(L, id);
    return 1;
}

// spr(img, n, x, y, flip_x, flip_y)：畫圖片的第 n 格（小格大小在 newimage 時決定）
static int l_spr(lua_State *L)
{
    DRAW_CALL();
    gfx_spr((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
            (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
            lua_toboolean(L, 5), lua_toboolean(L, 6));
    return 0;
}

// sspr(img, sx, sy, sw, sh, dx, dy, dw, dh, flip_x, flip_y)：
// 畫圖片的任意範圍，可以放大縮小。dw、dh 省略就是原尺寸
static int l_sspr(lua_State *L)
{
    DRAW_CALL();
    int img = (int)luaL_checkinteger(L, 1);
    int sx = (int)luaL_checkinteger(L, 2);
    int sy = (int)luaL_checkinteger(L, 3);
    int sw = (int)luaL_checkinteger(L, 4);
    int sh = (int)luaL_checkinteger(L, 5);
    int dx = (int)luaL_checkinteger(L, 6);
    int dy = (int)luaL_checkinteger(L, 7);
    int dw = (int)luaL_optinteger(L, 8, sw);
    int dh = (int)luaL_optinteger(L, 9, sh);
    gfx_sspr(img, sx, sy, sw, sh, dx, dy, dw, dh, lua_toboolean(L, 10), lua_toboolean(L, 11));
    return 0;
}

// newfont(height, count, data)：載入字型，取代原本的字型。
// data 是 base64，解開後是 count 個字、每個字 5 + height × 2 byte。
// 通常不用自己寫：遊戲有用到 text() 時，合併工具會自動產生這一行
static int l_newfont(lua_State *L)
{
    lua_Integer height = luaL_checkinteger(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);
    size_t len;
    const char *data = luaL_checklstring(L, 3, &len);
    luaL_argcheck(L, height >= 1 && height <= 32, 1, "bad font height");
    luaL_argcheck(L, count >= 1 && count <= 65535, 2, "bad glyph count");

    size_t size = (size_t)count * (size_t)(5 + height * 2);
    uint8_t *buf = lua_newuserdatauv(L, size, 0);   // 暫時借用 Lua 的記憶體，Lua 會自己回收
    long n = b64_decode(data, len, buf, size);
    if (n != (long)size) {
        return luaL_error(L, "font data has %d bytes, expected %d", (int)n, (int)size);
    }
    int r = gfx_font_new((int)height, (int)count, buf);
    if (r == -2) return luaL_error(L, "font glyphs are not sorted by code point");
    if (r != 0) return luaL_error(L, "cannot load font (out of memory)");
    return 0;
}

// 沒有字型時呼叫 text()：通常是遊戲檔沒有經過合併工具
static void check_font(lua_State *L)
{
    if (!gfx_font_loaded()) {
        luaL_error(L, "no font loaded: build the game with Comet Studio or tools/cart.mjs, "
                      "which adds the glyphs text() needs");
    }
}

// text(s, x, y, color)：畫文字，回傳寬度（像素）。color 省略時是 7（白）。
// s 也可以是數字，例如 text(hp, 10, 10)
static int l_text(lua_State *L)
{
    DRAW_CALL();
    check_font(L);
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    lua_Integer x = luaL_checkinteger(L, 2);
    lua_Integer y = luaL_checkinteger(L, 3);
    lua_Integer c = luaL_optinteger(L, 4, 7);
    lua_pushinteger(L, gfx_text(s, len, (int)x, (int)y, (uint8_t)c));
    return 1;
}

// textw(s)：文字畫出來會有多寬（像素），不畫。用來置中、換行
static int l_textw(lua_State *L)
{
    check_font(L);
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    lua_pushinteger(L, gfx_text_width(s, len));
    return 1;
}

// ---- 地圖 ----

// newmap(w, h, { 圖片, ... }, { 圖層名稱 = "base64", ... })：建立地圖，回傳地圖編號。
// 每個圖層解開後是 w × h 個格子編號，每個 2 byte。
// 通常不用自己寫：comet.json 的 "maps" 會自動產生這一行
static int l_newmap(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1);
    lua_Integer h = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TTABLE);
    luaL_argcheck(L, w > 0 && h > 0 && w <= 1024 && h <= 1024, 1, "bad map size");

    // 第 3 個參數：用了哪些圖。lua_geti 取出表格的第 i 個元素放到堆疊最上面
    int imgs[MAX_TILESETS];
    int n = (int)luaL_len(L, 3);
    luaL_argcheck(L, n >= 1 && n <= MAX_TILESETS, 3, "a map uses 1 to 4 images");
    for (int i = 0; i < n; i++) {
        lua_geti(L, 3, i + 1);
        imgs[i] = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    int m = map_new((int)w, (int)h, imgs, n);
    if (m == -1) return luaL_error(L, "all images of a map must have the same tile size");
    if (m == 0) return luaL_error(L, "cannot create map (bad image, too many maps or out of memory)");

    // 第 4 個參數：圖層。lua_next 一個一個走過表格的「名稱 = 資料」
    size_t size = (size_t)w * (size_t)h * 2;
    lua_pushnil(L);
    while (lua_next(L, 4) != 0) {               // 堆疊：... 名稱 資料
        const char *name = luaL_checkstring(L, -2);
        size_t len;
        const char *data = luaL_checklstring(L, -1, &len);
        uint8_t *buf = lua_newuserdatauv(L, size, 0);
        if (b64_decode(data, len, buf, size) != (long)size) {
            return luaL_error(L, "map layer '%s' has wrong size", name);
        }
        int r = map_add_layer(m, name, buf);
        if (r == -2) return luaL_error(L, "map layer '%s' uses a tile number larger than its images have", name);
        if (r != 0) return luaL_error(L, "cannot add map layer '%s'", name);
        lua_pop(L, 2);                          // 拿掉 buf 和資料，留下名稱給 lua_next 找下一個
    }
    lua_pushinteger(L, m);
    return 1;
}

// map_get 等函式回傳負數代表找不到地圖或圖層：變成清楚的錯誤訊息
static void check_map_result(lua_State *L, int r, const char *layer)
{
    if (r == -2) luaL_error(L, "map does not exist");
    if (r == -1) luaL_error(L, "map has no layer '%s'", layer);
}

// map(地圖, 圖層, ox, oy)：畫一個圖層，地圖左上角放在 (ox, oy)（省略是 0, 0）
static int l_map(lua_State *L)
{
    DRAW_CALL();
    int m = (int)luaL_checkinteger(L, 1);
    const char *layer = luaL_checkstring(L, 2);
    int ox = (int)luaL_optinteger(L, 3, 0);
    int oy = (int)luaL_optinteger(L, 4, 0);
    check_map_result(L, map_draw(m, layer, ox, oy), layer);
    return 0;
}

// mget(地圖, 圖層, cx, cy)：第 cx 欄、第 cy 列那一格的編號（0 = 空的，地圖外面也是 0）
static int l_mget(lua_State *L)
{
    const char *layer = luaL_checkstring(L, 2);
    int r = map_get((int)luaL_checkinteger(L, 1), layer,
                    (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4));
    check_map_result(L, r, layer);
    lua_pushinteger(L, r);
    return 1;
}

// mset(地圖, 圖層, cx, cy, 編號)：改一格
static int l_mset(lua_State *L)
{
    const char *layer = luaL_checkstring(L, 2);
    int r = map_set((int)luaL_checkinteger(L, 1), layer,
                    (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                    (int)luaL_checkinteger(L, 5));
    check_map_result(L, r, layer);
    return 0;
}

// mapw(地圖)、maph(地圖)：地圖幾格寬、幾格高
static int l_mapw(lua_State *L) { lua_pushinteger(L, map_w((int)luaL_checkinteger(L, 1))); return 1; }
static int l_maph(lua_State *L) { lua_pushinteger(L, map_h((int)luaL_checkinteger(L, 1))); return 1; }

static const luaL_Reg api[] = {
    { "cls",   l_cls   },
    { "pset",  l_pset  },
    { "rect",  l_rect  },
    { "btn",   l_btn   },
    { "stick", l_stick },
    { "frame", l_frame },
    { "rnd",   l_rnd   },
    { "pal",      l_pal      },
    { "dpal",     l_dpal     },
    { "palget",   l_palget   },
    { "imgcolors", l_imgcolors },
    { "camera",   l_camera   },
    { "newimage", l_newimage },
    { "spr",      l_spr      },
    { "sspr",     l_sspr     },
    { "newfont",  l_newfont  },
    { "text",     l_text     },
    { "textw",    l_textw    },
    { "newmap",   l_newmap   },
    { "map",      l_map      },
    { "mget",     l_mget     },
    { "mset",     l_mset     },
    { "mapw",     l_mapw     },
    { "maph",     l_maph     },
    { NULL, NULL }
};

// ============================================================
// 2. 建立沙盒：只開放安全、不會破壞同步的功能
// ============================================================

static void open_libs(lua_State *L, int is_system)
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
    save_open_lua(L);   // save、loadsave（在 save.c）
    pak_open_lua(L);    // bgload、sprdraw 等資源包函式（在 pak.c）
    // 系統 API 只給系統卡匣。一般遊戲的 Lua 環境裡根本沒有 sys 這個名字
    if (is_system) system_open_lua(L);

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

int script_load(const char *src, size_t len, const char *name, int is_system)
{
    if (L != NULL) {
        lua_close(L);   // 關掉上一款遊戲，釋放它用的全部記憶體
    }
    failed = 0;
    rng_state = load_seed;   // 同一個種子 → 同一串亂數

    L = luaL_newstate();
    if (L == NULL) {
        platform_log("記憶體不足，無法啟動 Lua");
        failed = 1;
        return 1;
    }
    open_libs(L, is_system);
    script_profile(profiling);   // 新的 Lua 環境：hook 要重新裝

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

int script_check(const char *src, size_t len, const char *name, char *err, size_t err_size)
{
    // 另外開一個暫時的 Lua 環境（T），和正在執行的遊戲（L）完全分開
    lua_State *T = luaL_newstate();
    if (T == NULL) {
        snprintf(err, err_size, "記憶體不足，無法檢查語法");
        return 1;
    }
    // luaL_loadbufferx 只把原始碼編譯成 Lua 的內部指令，不會執行任何一行。
    // 語法錯誤（少了 end、括號沒配對……）在這一步就會被發現
    int r = luaL_loadbufferx(T, src, len, name, "t");
    if (r == LUA_OK) {
        err[0] = '\0';
    } else {
        const char *msg = lua_tostring(T, -1);
        snprintf(err, err_size, "%s", msg != NULL ? msg : "syntax error");
    }
    lua_close(T);   // 編譯結果用不到，整個環境丟掉
    return r != LUA_OK;
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
// 除錯：在執行中的遊戲裡跑 Lua（Studio 的「狀態」分頁）
// ============================================================
// 幫手寫成 Lua：把值變成文字（指令結果用一行、狀態用樹狀的好幾行）。
// 每次呼叫時才編譯，不放進遊戲的全域變數，遊戲看不到它。
// 第 3 個參數 compile 是 C 提供的「編譯一段文字」，不靠遊戲的 load（遊戲可能把它蓋掉）

static const char DBG_LUA[] =
    "local mode, src, compile = ...\n"
    "local function lit(v)\n"
    "  if type(v) == 'string' then return (string.format('%q', v):gsub('\\\\\\n', '\\\\n')) end\n"
    "  return tostring(v)\n"
    "end\n"
    "local function sorted(t)\n"
    "  local ks = {}\n"
    "  for k in pairs(t) do ks[#ks + 1] = k end\n"
    "  table.sort(ks, function(a, b)\n"
    "    local ta, tb = type(a), type(b)\n"
    "    if ta ~= tb then return ta < tb end\n"
    "    if ta == 'number' or ta == 'string' then return a < b end\n"
    "    return tostring(a) < tostring(b)\n"
    "  end)\n"
    "  return ks\n"
    "end\n"
    "local function keypath(k)\n"
    "  if type(k) == 'string' and k:match('^[%a_][%w_]*$') then return '.' .. k, k end\n"
    "  local s = '[' .. lit(k) .. ']'\n"
    "  return s, s\n"
    "end\n"
    "local function count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end\n"
    "local function short(v, depth)\n"
    "  if type(v) ~= 'table' then return lit(v) end\n"
    "  if depth >= 2 then return '{…' .. count(v) .. '}' end\n"
    "  local parts, n = {}, 0\n"
    "  for _, k in ipairs(sorted(v)) do\n"
    "    n = n + 1\n"
    "    if n > 12 then parts[#parts + 1] = '…'; break end\n"
    "    local _, label = keypath(k)\n"
    "    parts[#parts + 1] = label .. ' = ' .. short(v[k], depth + 1)\n"
    "  end\n"
    "  return '{ ' .. table.concat(parts, ', ') .. ' }'\n"
    "end\n"
    "if mode ~= 1 then\n"
    "  local f = compile('return ' .. src)\n"
    "  if not f then\n"
    "    local err\n"
    "    f, err = compile(src)\n"
    "    if not f then error(err, 0) end\n"
    "  end\n"
    "  local r = table.pack(f())\n"
    "  if r.n == 0 then return '（完成）' end\n"
    "  local parts = {}\n"
    "  for i = 1, r.n do parts[i] = short(r[i], 0) end\n"
    "  return table.concat(parts, '\\t')\n"
    "end\n"
    "local out, lines = {}, 0\n"
    "local LIMIT = 500\n"
    "local function tree(v, path, label, depth, seen)\n"
    "  lines = lines + 1\n"
    "  if lines > LIMIT then return end\n"
    "  if type(v) == 'table' then\n"
    "    out[#out + 1] = depth .. '\\t' .. path .. '\\t' .. label .. '\\t{' .. count(v) .. '}'\n"
    "    if seen[v] or depth >= 6 then return end\n"
    "    seen[v] = true\n"
    "    for _, k in ipairs(sorted(v)) do\n"
    "      local p, l = keypath(k)\n"
    "      tree(v[k], path .. p, l, depth + 1, seen)\n"
    "    end\n"
    "    seen[v] = nil\n"
    "  else\n"
    "    out[#out + 1] = depth .. '\\t' .. path .. '\\t' .. label .. '\\t' .. lit(v)\n"
    "  end\n"
    "end\n"
    "local view = rawget(_G, 'DEBUG_VIEW')\n"
    "if type(view) ~= 'table' then return '' end\n"
    "for _, expr in ipairs(view) do\n"
    "  local f, err = compile('return ' .. expr)\n"
    "  local ok, v = false, err\n"
    "  if f then ok, v = pcall(f) end\n"
    "  if ok then tree(v, expr, expr, 0, {})\n"
    "  else out[#out + 1] = '0\\t' .. expr .. '\\t' .. expr .. '\\t（錯誤：' .. tostring(v) .. '）' end\n"
    "end\n"
    "if lines > LIMIT then out[#out + 1] = '0\\t\\t…\\t（太多了，只顯示前 ' .. LIMIT .. ' 行）' end\n"
    "return table.concat(out, '\\n')\n";

// compile(文字)：編譯一段 Lua，成功回傳函式，失敗回傳 nil, 錯誤訊息
static int l_dbg_compile(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (luaL_loadbufferx(L, s, len, "=除錯", "t") != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

int script_eval(const char *src, size_t len, int mode, char *out, size_t cap)
{
    if (L == NULL) {
        snprintf(out, cap, "遊戲還沒有執行");
        return 1;
    }
    int top = lua_gettop(L);
    if (luaL_loadbufferx(L, DBG_LUA, sizeof(DBG_LUA) - 1, "=除錯幫手", "t") != LUA_OK) {
        snprintf(out, cap, "%s", lua_tostring(L, -1));
        lua_settop(L, top);
        return 1;
    }
    lua_pushinteger(L, mode);
    lua_pushlstring(L, src, len);
    lua_pushcfunction(L, l_dbg_compile);
    int r = lua_pcall(L, 3, 1, 0);
    const char *s = lua_tostring(L, -1);
    snprintf(out, cap, "%s", s != NULL ? s : (r == LUA_OK ? "" : "（沒有錯誤訊息）"));
    lua_settop(L, top);
    return r != LUA_OK;
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
