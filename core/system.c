// system.c — 系統卡匣模式的實作（說明見 system.h）

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "system.h"
#include "input.h"
#include "platform.h"
#include "save.h"

typedef struct {
    char id[SYSTEM_ID_MAX + 1];
    char name[SYSTEM_NAME_MAX + 1];
    char desc[SYSTEM_DESC_MAX + 1];
    int players;
} GameEntry;

static GameEntry games[SYSTEM_MAX_GAMES];
static int game_count = 0;

static int nova_mode = 0;      // 載入過系統卡匣：回 HOME 的按鍵才有作用
static int running_system = 0; // 目前跑的是系統卡匣
static int home_hold = 0;      // Start＋Select 已經按住幾幀

static int request = SYS_REQ_NONE;
static char request_id[SYSTEM_ID_MAX + 1];
static char last_game[SYSTEM_ID_MAX + 1];   // 上一款玩的遊戲（回 HOME 時記下）

// ============================================================
// 平台用
// ============================================================

void system_clear_games(void)
{
    game_count = 0;
}

// 把 src 複製到 dst（最多 max 個 byte）。太長就截斷；截在 UTF-8 字的中間會變成亂碼，
// 所以往前退到完整的字為止
static void copy_utf8(char *dst, const char *src, size_t max)
{
    size_t n = strlen(src);
    if (n > max) {
        n = max;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   // 10xxxxxx = 字的中間
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int system_add_game(const char *id, const char *name)
{
    if (game_count >= SYSTEM_MAX_GAMES || strlen(id) > SYSTEM_ID_MAX) return 1;
    GameEntry *g = &games[game_count++];
    strcpy(g->id, id);
    copy_utf8(g->name, name, SYSTEM_NAME_MAX);
    g->desc[0] = '\0';
    g->players = 0;
    return 0;
}

int system_set_info(const char *id, const char *desc, int players)
{
    for (int i = 0; i < game_count; i++) {
        if (strcmp(games[i].id, id) == 0) {
            copy_utf8(games[i].desc, desc ? desc : "", SYSTEM_DESC_MAX);
            games[i].players = players < 0 ? 0 : players;
            return 0;
        }
    }
    return 1;
}

int system_request(void) { return request; }
const char *system_request_game(void) { return request_id; }
void system_request_done(void) { request = SYS_REQ_NONE; }

// ============================================================
// 引擎內部用
// ============================================================

void system_on_load(int is_system)
{
    running_system = is_system;
    if (is_system) nova_mode = 1;
    home_hold = 0;
    request = SYS_REQ_NONE;
}

int system_is_running(void)
{
    return running_system;
}

void system_before_frame(void)
{
    if (!nova_mode || running_system || request != SYS_REQ_NONE) return;

    int home = 0, combo = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        uint16_t b = input_get_buttons(p);
        if (b & (1u << BTN_HOME)) home = 1;
        if ((b & (1u << BTN_START)) && (b & (1u << BTN_SELECT))) combo = 1;
    }
    home_hold = combo ? home_hold + 1 : 0;
    if (home || home_hold >= SYSTEM_HOME_HOLD) {
        strcpy(last_game, save_game_id());   // 讓系統卡匣知道剛剛玩的是哪一款
        request = SYS_REQ_HOME;
    }
}

// ============================================================
// Lua：sys 表格（只有系統卡匣有）
// ============================================================

// sys.games()：{ { id = "hardliver/wildkin", name = "Wildkin 野伴", desc = "……", players = 1 }, ... }
// desc 沒寫是 ""，players 沒寫是 0
static int l_games(lua_State *L)
{
    lua_createtable(L, game_count, 0);
    for (int i = 0; i < game_count; i++) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, games[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, games[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, games[i].desc);
        lua_setfield(L, -2, "desc");
        lua_pushinteger(L, games[i].players);
        lua_setfield(L, -2, "players");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// sys.launch(id)：請平台啟動這款遊戲（這一幀結束後才換）。清單裡沒有回傳 false
static int l_launch(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    for (int i = 0; i < game_count; i++) {
        if (strcmp(games[i].id, id) == 0) {
            strcpy(request_id, id);
            request = SYS_REQ_LAUNCH;
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// sys.last_game()：剛剛從哪一款遊戲回到 HOME；開機後還沒玩過回傳 nil
static int l_last_game(lua_State *L)
{
    if (last_game[0] == '\0') lua_pushnil(L);
    else lua_pushstring(L, last_game);
    return 1;
}

void system_open_lua(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        { "games",     l_games     },
        { "launch",    l_launch    },
        { "last_game", l_last_game },
        { NULL, NULL }
    };
    luaL_newlib(L, funcs);
    lua_setglobal(L, "sys");
}
