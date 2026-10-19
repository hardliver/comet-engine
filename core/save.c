// save.c — 存檔的實作（用法見 save.h）
//
// 存檔的編碼（第 1 版）：
//   第 1 個 byte 是版本（1），後面是一個「值」：
//     'f'  false
//     't'  true
//     'i'  整數：zigzag varint（見下方 put_int）
//     's'  字串：varint 長度 + 內容
//     'T'  表格：varint 項目數 + 一對一對的（鍵、值）
//   表格的鍵只能是整數或字串。輸出時「整數鍵由小到大，再來字串鍵依 byte 排序」：
//   同樣內容的表格一定編出同樣的 bytes，不受 pairs() 順序影響。

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "save.h"
#include "engine.h"
#include "platform.h"
#include "replay.h"

#define SAVE_FORMAT 1
#define MAX_DEPTH   32     // 表格最多幾層；也用來擋「表格包含自己」
#define ID_MAX      63

static char game_id[ID_MAX + 1] = "_";
static int warned_no_id = 0;

// 編碼、解碼共用的緩衝區。放在這裡而不是堆疊上：Pico 的堆疊很小
static unsigned char io[SAVE_MAX];

// ============================================================
// 遊戲 id
// ============================================================

static int id_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '/' || c == '-' || c == '_' || c == '.';
}

// 在前 10 行裡找「-- id: 作者/遊戲」
void save_on_load(const char *src, size_t len)
{
    strcpy(game_id, "_");
    warned_no_id = 0;
    size_t pos = 0;
    for (int line = 0; line < 10 && pos < len; line++) {
        size_t end = pos;
        while (end < len && src[end] != '\n') end++;
        const char *tag = "-- id:";
        size_t tl = strlen(tag);
        if (end - pos > tl && memcmp(src + pos, tag, tl) == 0) {
            size_t a = pos + tl, b = end;
            while (a < b && src[a] == ' ') a++;
            while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\r')) b--;
            int ok = b > a && b - a <= ID_MAX;
            for (size_t i = a; ok && i < b; i++) ok = id_char_ok(src[i]);
            if (ok) {
                memcpy(game_id, src + a, b - a);
                game_id[b - a] = '\0';
            }
            return;
        }
        pos = end + 1;
    }
}

const char *save_game_id(void)
{
    return game_id;
}

static void warn_no_id(void)
{
    if (game_id[0] != '_' || game_id[1] != '\0' || warned_no_id) return;
    warned_no_id = 1;
    platform_log("提醒：這個遊戲沒有 id，存檔會和其他沒有 id 的遊戲共用。"
                 "在 comet.json 加上 \"id\": \"作者/遊戲\"，或在檔案第一行寫 -- id: 作者/遊戲");
}

// ============================================================
// 編碼：Lua 的值 → bytes
// ============================================================

typedef struct {
    lua_State *L;
    size_t len;
} Writer;

static void put(Writer *w, unsigned char b)
{
    if (w->len >= SAVE_MAX) luaL_error(w->L, "save：存檔太大（一個欄位最多 %d KB）", SAVE_MAX / 1024);
    io[w->len++] = b;
}

static void put_varint(Writer *w, uint64_t v)
{
    while (v >= 0x80) {
        put(w, (unsigned char)(v | 0x80));
        v >>= 7;
    }
    put(w, (unsigned char)v);
}

// zigzag：把負數也變成小的正數（0→0, -1→1, 1→2, -2→3 …），varint 才會短
static void put_int(Writer *w, lua_Integer n)
{
    uint64_t u = (uint64_t)n;
    put_varint(w, (u << 1) ^ (n < 0 ? UINT64_MAX : 0));
}

static void put_str(Writer *w, const char *s, size_t len)
{
    put_varint(w, len);
    for (size_t i = 0; i < len; i++) put(w, (unsigned char)s[i]);
}

// 字串鍵排序用
typedef struct {
    const char *s;
    size_t len;
} StrKey;

static int cmp_int(const void *a, const void *b)
{
    lua_Integer x = *(const lua_Integer *)a, y = *(const lua_Integer *)b;
    return (x > y) - (x < y);
}

static int cmp_str(const void *a, const void *b)
{
    const StrKey *x = a, *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->s, y->s, n);
    if (c != 0) return c;
    return (x->len > y->len) - (x->len < y->len);
}

static void put_value(Writer *w, int idx, int depth);

// 表格：先數有幾個鍵，分成整數鍵、字串鍵兩堆，各自排序後輸出
static void put_table(Writer *w, int idx, int depth)
{
    lua_State *L = w->L;
    if (depth >= MAX_DEPTH) luaL_error(L, "save：表格太深（最多 %d 層），或表格裡放了自己", MAX_DEPTH);
    luaL_checkstack(L, 4, "save");

    size_t nint = 0, nstr = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);   // 只看鍵
        if (lua_isinteger(L, -1)) nint++;
        else if (lua_type(L, -1) == LUA_TSTRING) nstr++;
        else luaL_error(L, "save：表格的鍵只能是整數或字串（不能是 %s）", luaL_typename(L, -1));
    }

    // 用 Lua 的記憶體借暫存陣列（放在 Lua 堆疊上的 userdata）：
    // 出錯時 luaL_error 會跳走，這塊記憶體由 Lua 自動回收，不會漏掉
    lua_Integer *ints = lua_newuserdatauv(L, nint * sizeof(lua_Integer) + 1, 0);
    StrKey *strs = lua_newuserdatauv(L, nstr * sizeof(StrKey) + 1, 0);
    size_t i = 0, k = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (lua_isinteger(L, -1)) {
            ints[i++] = lua_tointeger(L, -1);
        } else {
            // 字串指標在表格還在的期間都有效（鍵被表格引用著）
            strs[k].s = lua_tolstring(L, -1, &strs[k].len);
            k++;
        }
    }
    qsort(ints, nint, sizeof(lua_Integer), cmp_int);
    qsort(strs, nstr, sizeof(StrKey), cmp_str);

    put(w, 'T');
    put_varint(w, nint + nstr);
    for (i = 0; i < nint; i++) {
        put(w, 'i');
        put_int(w, ints[i]);
        lua_rawgeti(L, idx, ints[i]);
        put_value(w, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
    for (k = 0; k < nstr; k++) {
        put(w, 's');
        put_str(w, strs[k].s, strs[k].len);
        lua_pushlstring(L, strs[k].s, strs[k].len);
        lua_rawget(L, idx);
        put_value(w, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);   // 兩個暫存陣列
}

static void put_value(Writer *w, int idx, int depth)
{
    lua_State *L = w->L;
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        put(w, lua_toboolean(L, idx) ? 't' : 'f');
        break;
    case LUA_TNUMBER:
        if (!lua_isinteger(L, idx)) luaL_error(L, "save：只能存整數，不能有小數（%f）", lua_tonumber(L, idx));
        put(w, 'i');
        put_int(w, lua_tointeger(L, idx));
        break;
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        put(w, 's');
        put_str(w, s, len);
        break;
    }
    case LUA_TTABLE:
        put_table(w, idx, depth);
        break;
    default:
        luaL_error(L, "save：不能存 %s（只能存表格、字串、整數、true/false）", luaL_typename(L, idx));
    }
}

// ============================================================
// 解碼：bytes → Lua 的值
// ============================================================

typedef struct {
    lua_State *L;
    const unsigned char *p;
    size_t len, pos;
} Reader;

// 解碼失敗（存檔損壞）都回傳 1，不呼叫 luaL_error：壞掉的存檔不該讓遊戲當掉

static int get_varint(Reader *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->pos >= r->len) return 1;
        unsigned char b = r->p[r->pos++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return 0; }
    }
    return 1;
}

// 讀一個值並推上 Lua 堆疊。成功回傳 0
static int get_value(Reader *r, int depth)
{
    lua_State *L = r->L;
    if (depth > MAX_DEPTH || r->pos >= r->len) return 1;
    if (!lua_checkstack(L, 3)) return 1;
    uint64_t u;
    switch (r->p[r->pos++]) {
    case 'f': lua_pushboolean(L, 0); return 0;
    case 't': lua_pushboolean(L, 1); return 0;
    case 'i':
        if (get_varint(r, &u)) return 1;
        lua_pushinteger(L, (lua_Integer)((u >> 1) ^ (0 - (u & 1))));   // zigzag 還原
        return 0;
    case 's':
        if (get_varint(r, &u) || u > r->len - r->pos) return 1;
        lua_pushlstring(L, (const char *)r->p + r->pos, (size_t)u);
        r->pos += (size_t)u;
        return 0;
    case 'T': {
        if (get_varint(r, &u) || u > r->len) return 1;   // 每項至少 1 byte，不可能比檔案還多
        lua_createtable(L, 0, 0);
        for (uint64_t i = 0; i < u; i++) {
            if (r->pos >= r->len || (r->p[r->pos] != 'i' && r->p[r->pos] != 's')) return 1;
            if (get_value(r, depth + 1)) return 1;   // 鍵
            if (get_value(r, depth + 1)) return 1;   // 值
            lua_rawset(L, -3);
        }
        return 0;
    }
    default:
        return 1;
    }
}

// ============================================================
// 讀寫一個欄位：經過重播，再交給平台
// ============================================================

// 讀到 io，回傳長度；沒有存檔回傳 -1
static int slot_read(int slot)
{
    uint32_t frame = engine_frame_count();
    if (replay_state() == REPLAY_PLAYING) {
        int n = replay_play_save(frame, slot, io, SAVE_MAX);
        if (n != -2) return n;
        // 對不上：播放已經停了，改讀真正的存檔，玩家可以接著玩
    }
    int n = platform_save_read(game_id, slot, io, SAVE_MAX);
    if (n < -1 || n > SAVE_MAX) n = -1;
    replay_record_save(frame, slot, io, n);
    return n;
}

static void slot_write(int slot, const unsigned char *data, int len)
{
    if (replay_state() == REPLAY_PLAYING) return;   // 播放中不寫，不蓋掉玩家自己的存檔
    if (platform_save_write(game_id, slot, data, len) != 0) {
        platform_log("存檔失敗（儲存空間不足？）");
    }
}

// ============================================================
// Lua API
// ============================================================

static int check_slot(lua_State *L)
{
    lua_Integer slot = luaL_checkinteger(L, 1);
    luaL_argcheck(L, slot >= 0 && slot < SAVE_SLOTS, 1, "slot must be 0-7");
    return (int)slot;
}

// save(欄位, 值)：值是 nil 就刪除
static int l_save(lua_State *L)
{
    int slot = check_slot(L);
    warn_no_id();
    if (lua_isnoneornil(L, 2)) {
        slot_write(slot, NULL, 0);
        return 0;
    }
    Writer w = { L, 0 };
    put(&w, SAVE_FORMAT);
    put_value(&w, 2, 0);
    slot_write(slot, io, (int)w.len);
    return 0;
}

// loadsave(欄位)：回傳存的值；沒有存檔或存檔損壞回傳 nil
static int l_loadsave(lua_State *L)
{
    int slot = check_slot(L);
    int n = slot_read(slot);
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }
    Reader r = { L, io, (size_t)n, 0 };
    int top = lua_gettop(L);
    if (io[0] != SAVE_FORMAT) {
        platform_log(io[0] > SAVE_FORMAT ? "存檔的版本比引擎新，請更新引擎" : "存檔損壞，當作沒有存檔");
        lua_pushnil(L);
        return 1;
    }
    r.pos = 1;
    if (get_value(&r, 0) || r.pos != r.len) {
        lua_settop(L, top);
        platform_log("存檔損壞，當作沒有存檔");
        lua_pushnil(L);
    }
    return 1;
}

void save_open_lua(lua_State *L)
{
    lua_register(L, "save", l_save);
    lua_register(L, "loadsave", l_loadsave);
}
