// pak.c — 資源包的實作（格式與用法見 pak.h）
//
// 讀資料都經過 pak_read()：現在是從記憶體複製，之後 Pico 版改成從 SD 卡讀，
// 只要改這一個函式，其他地方都不用動。

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "pak.h"
#include "gfx.h"
#include "platform.h"

#define HEADER_SIZE 48
#define ENTRY_SIZE  48
#define FORMAT_VERSION 1
#define BG_FIRST_COLOR 128     // 背景的顏色放在調色盤 128–255
#define BG_MAX_COLORS  128

static const uint8_t *pak = NULL;   // 平台交給我們的整個檔案
static size_t pak_len = 0;
static uint32_t pak_count = 0;      // 幾個項目

// 一組載入的角色動畫：整個項目複製一份（Lua 換遊戲時、或 sprfree 時釋放）
typedef struct {
    uint8_t *data;        // NULL = 這個位置是空的
    uint32_t size;
    int frames;
} SpriteSet;

static SpriteSet sprites[PAK_MAX_SPRITES + 1];   // 編號從 1 開始

// ============================================================
// 讀取小工具
// ============================================================

static int pak_read(uint32_t off, void *dst, uint32_t len)
{
    if (pak == NULL || (size_t)off + len > pak_len) return 1;
    memcpy(dst, pak + off, len);
    return 0;
}

// little-endian：低位的 byte 在前。一個 byte 一個 byte 組，不管 CPU 是哪種排法都對
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int s16(const uint8_t *p) { int v = u16(p); return v >= 32768 ? v - 65536 : v; }

typedef struct {
    uint32_t type, off, size;
} Entry;

// 依名稱找項目；找到回傳 0
static int find(const char *name, Entry *e)
{
    size_t nl = strlen(name);
    if (nl == 0 || nl > PAK_NAME_MAX) return 1;
    uint8_t raw[ENTRY_SIZE];
    for (uint32_t i = 0; i < pak_count; i++) {
        if (pak_read(HEADER_SIZE + i * ENTRY_SIZE, raw, ENTRY_SIZE)) return 1;
        // 名稱欄位 32 bytes，不足補 0：前 nl 個字相同，而且後面是 0（或剛好滿 32）
        if (memcmp(raw, name, nl) == 0 && (nl == PAK_NAME_MAX || raw[nl] == 0)) {
            e->type = u32(raw + 32);
            e->off = u32(raw + 36);
            e->size = u32(raw + 40);
            return 0;
        }
    }
    return 1;
}

// ============================================================
// 平台用
// ============================================================

int pak_set(const uint8_t *data, size_t len)
{
    pak = NULL;
    pak_len = 0;
    pak_count = 0;
    if (data == NULL) return 0;

    if (len < HEADER_SIZE || memcmp(data, "CPAK", 4) != 0) {
        platform_log("資源包格式不對（開頭不是 CPAK）");
        return 1;
    }
    if (u32(data + 4) != FORMAT_VERSION) {
        platform_log("資源包的版本太新，請更新 Comet");
        return 1;
    }
    uint32_t count = u32(data + 8);
    if ((size_t)HEADER_SIZE + (size_t)count * ENTRY_SIZE > len) {
        platform_log("資源包損壞（目錄超出檔案）");
        return 1;
    }
    // 先檢查每個項目都在檔案範圍內，之後讀取就不必擔心讀到檔案外面
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = data + HEADER_SIZE + i * ENTRY_SIZE;
        uint32_t off = u32(e + 36), size = u32(e + 40);
        if ((size_t)off + size > len) {
            platform_log("資源包損壞（項目超出檔案）");
            return 1;
        }
    }
    pak = data;
    pak_len = len;
    pak_count = count;
    return 0;
}

const uint8_t *pak_sha(void)
{
    return pak != NULL ? pak + 12 : NULL;
}

void pak_reset(void)
{
    for (int i = 1; i <= PAK_MAX_SPRITES; i++) {
        free(sprites[i].data);
        sprites[i].data = NULL;
    }
}

// ============================================================
// 背景
// ============================================================

// 載入背景：像素直接讀進背景陣列，深度解 RLE。回傳錯誤訊息，成功回傳 NULL
static const char *load_bg(const Entry *e)
{
    uint8_t head[6];
    if (e->size < 6 || pak_read(e->off, head, 6)) return "bad background";
    int w = u16(head), h = u16(head + 2), n = u16(head + 4);
    if (w != SCREEN_W || h != SCREEN_H) return "background must be 320x240";
    if (n < 1 || n > BG_MAX_COLORS) return "background must have 1-128 colors";

    uint32_t pos = e->off + 6;
    uint32_t end = e->off + e->size;
    uint8_t rgb[BG_MAX_COLORS * 3];
    if (pos + (uint32_t)n * 3 > end || pak_read(pos, rgb, (uint32_t)n * 3)) return "bad background palette";
    pos += (uint32_t)n * 3;

    uint8_t *px = gfx_bg_pixels();
    uint8_t *depth = gfx_bg_depth();
    if (px == NULL || depth == NULL) return "out of memory for background";
    uint32_t npx = (uint32_t)w * (uint32_t)h;
    if (pos + npx > end || pak_read(pos, px, npx)) return "bad background pixels";
    pos += npx;
    // 圖裡的編號是 0 .. n-1，畫面上用 128 .. 127 + n
    for (uint32_t i = 0; i < npx; i++) {
        if (px[i] >= n) px[i] = 0;    // 超出範圍的編號（檔案損壞）當成第 0 色
        px[i] = (uint8_t)(px[i] + BG_FIRST_COLOR);
    }
    for (int i = 0; i < n; i++) {
        gfx_pal(BG_FIRST_COLOR + i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }

    // 深度：RLE（個數、值）一對一對，直到填滿 160 × 120
    memset(depth, 0, BG_DEPTH_W * BG_DEPTH_H);
    uint8_t dh[4];
    if (pos + 4 > end || pak_read(pos, dh, 4)) return "bad background depth";
    pos += 4;
    int dw = u16(dh), dhh = u16(dh + 2);
    if (dw == 0 && dhh == 0) {
        gfx_bg_set_loaded(1);
        return NULL;   // 這張背景沒有遮擋物
    }
    if (dw != BG_DEPTH_W || dhh != BG_DEPTH_H) return "background depth must be 160x120";
    uint32_t filled = 0, total = BG_DEPTH_W * BG_DEPTH_H;
    uint8_t pair[2];
    while (filled < total) {
        if (pos + 2 > end || pak_read(pos, pair, 2)) return "background depth is too short";
        pos += 2;
        uint32_t cnt = pair[0];
        if (cnt == 0 || filled + cnt > total) return "bad background depth run";
        memset(depth + filled, pair[1], cnt);
        filled += cnt;
    }
    gfx_bg_set_loaded(1);
    return NULL;
}

// ============================================================
// 角色動畫
// ============================================================

// 第 f 格的資訊。資料在載入時檢查過，這裡不用再檢查範圍
static void frame_info(const SpriteSet *s, int f, int *w, int *h, int *ox, int *oy, const uint8_t **px)
{
    const uint8_t *t = s->data + 2 + f * 12;
    *w = u16(t);
    *h = u16(t + 2);
    *ox = s16(t + 4);
    *oy = s16(t + 6);
    *px = s->data + u32(t + 8);
}

static const char *check_sprites(const uint8_t *d, uint32_t size, int *frames)
{
    if (size < 2) return "bad sprite set";
    int n = u16(d);
    if (n < 1 || 2 + (uint32_t)n * 12 > size) return "bad sprite set header";
    for (int f = 0; f < n; f++) {
        const uint8_t *t = d + 2 + f * 12;
        uint32_t w = (uint32_t)u16(t), h = (uint32_t)u16(t + 2), off = u32(t + 8);
        if (w == 0 || h == 0 || (uint64_t)off + (uint64_t)w * h > size) return "sprite frame outside the entry";
    }
    *frames = n;
    return NULL;
}

static SpriteSet *get_set(lua_State *L, int arg)
{
    lua_Integer h = luaL_checkinteger(L, arg);
    luaL_argcheck(L, h >= 1 && h <= PAK_MAX_SPRITES && sprites[h].data != NULL, arg, "not a loaded sprite set");
    return &sprites[h];
}

// ============================================================
// Lua API
// ============================================================

static Entry need(lua_State *L, int arg, uint32_t type)
{
    const char *name = luaL_checkstring(L, arg);
    Entry e;
    if (pak == NULL) luaL_error(L, "no resource pack loaded (put the .pak next to the game)");
    if (find(name, &e)) luaL_error(L, "resource pack has no '%s'", name);
    if (e.type != type) luaL_error(L, "'%s' has the wrong type", name);
    return e;
}

// pakhas(name)：資源包裡有沒有這一項
static int l_pakhas(lua_State *L)
{
    Entry e;
    lua_pushboolean(L, pak != NULL && find(luaL_checkstring(L, 1), &e) == 0);
    return 1;
}

// bgload(name)：載入背景（像素、128–255 的顏色、深度）。舊的背景被蓋掉
static int l_bgload(lua_State *L)
{
    Entry e = need(L, 1, PAK_BG);
    const char *err = load_bg(&e);
    if (err != NULL) {
        gfx_bg_set_loaded(0);
        return luaL_error(L, "%s: %s", lua_tostring(L, 1), err);
    }
    return 0;
}

// bg()：把背景畫到畫面上（通常是 _draw 的第一行，取代 cls）
static int l_bg(lua_State *L)
{
    (void)L;
    gfx_bg_draw();
    return 0;
}

// bgdepth(x, y)：畫面 (x, y) 的遮擋物離鏡頭幾公分（0 = 沒有）
static int l_bgdepth(lua_State *L)
{
    lua_pushinteger(L, gfx_bg_depth_at((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2)));
    return 1;
}

// palload(name)：套用資源包裡的調色盤（角色、介面共用的顏色）
static int l_palload(lua_State *L)
{
    Entry e = need(L, 1, PAK_PAL);
    uint8_t head[4];
    if (e.size < 4 || pak_read(e.off, head, 4)) return luaL_error(L, "bad palette");
    int first = u16(head), n = u16(head + 2);
    if (first + n > 256 || 4 + (uint32_t)n * 3 > e.size) return luaL_error(L, "bad palette");
    uint8_t rgb[256 * 3];
    pak_read(e.off + 4, rgb, (uint32_t)n * 3);
    for (int i = 0; i < n; i++) gfx_pal(first + i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    return 0;
}

// paltint(first, count, r, g, b)：把 first 起 count 個顏色乘上 r/256、g/256、b/256
static int l_paltint(lua_State *L)
{
    gfx_paltint((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5));
    return 0;
}

// sprload(name)：載入一組角色動畫，回傳編號（1 起算）
static int l_sprload(lua_State *L)
{
    Entry e = need(L, 1, PAK_SPRITES);
    int slot = 0;
    for (int i = 1; i <= PAK_MAX_SPRITES; i++) {
        if (sprites[i].data == NULL) { slot = i; break; }
    }
    if (slot == 0) return luaL_error(L, "too many sprite sets (max %d, use sprfree)", PAK_MAX_SPRITES);
    uint8_t *d = malloc(e.size);
    if (d == NULL) return luaL_error(L, "out of memory loading '%s'", lua_tostring(L, 1));
    pak_read(e.off, d, e.size);
    int frames = 0;
    const char *err = check_sprites(d, e.size, &frames);
    if (err != NULL) {
        free(d);
        return luaL_error(L, "%s: %s", lua_tostring(L, 1), err);
    }
    sprites[slot].data = d;
    sprites[slot].size = e.size;
    sprites[slot].frames = frames;
    lua_pushinteger(L, slot);
    return 1;
}

// sprfree(h)：釋放一組角色動畫（離開這一區、不會再出現的敵人）
static int l_sprfree(lua_State *L)
{
    SpriteSet *s = get_set(L, 1);
    free(s->data);
    s->data = NULL;
    return 0;
}

// sprcount(h)：這組動畫有幾格
static int l_sprcount(lua_State *L)
{
    lua_pushinteger(L, get_set(L, 1)->frames);
    return 1;
}

// sprinfo(h, frame)：回傳 w, h, ox, oy（原尺寸，ox/oy 是左上角相對腳底的位置）
static int l_sprinfo(lua_State *L)
{
    SpriteSet *s = get_set(L, 1);
    lua_Integer f = luaL_checkinteger(L, 2);
    luaL_argcheck(L, f >= 0 && f < s->frames, 2, "frame out of range");
    int w, h, ox, oy;
    const uint8_t *px;
    frame_info(s, (int)f, &w, &h, &ox, &oy, &px);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    lua_pushinteger(L, ox);
    lua_pushinteger(L, oy);
    return 4;
}

// sprdraw(h, frame, x, y, scale, flip, depth)：
//   (x, y) 是腳底在畫面上的位置；scale：256 = 原尺寸（遠的角色小、近的大）；
//   flip：左右翻轉；depth：離鏡頭幾公分，背景裡比較近的東西會擋住角色（0 或省略 = 不擋）
static int l_sprdraw(lua_State *L)
{
    SpriteSet *s = get_set(L, 1);
    lua_Integer f = luaL_checkinteger(L, 2);
    luaL_argcheck(L, f >= 0 && f < s->frames, 2, "frame out of range");
    int x = (int)luaL_checkinteger(L, 3);
    int y = (int)luaL_checkinteger(L, 4);
    int scale = (int)luaL_optinteger(L, 5, 256);
    int flip = lua_toboolean(L, 6);
    int depth = (int)luaL_optinteger(L, 7, 0);
    if (scale <= 0) return 0;
    if (scale > 256 * 16) scale = 256 * 16;

    int w, h, ox, oy;
    const uint8_t *px;
    frame_info(s, (int)f, &w, &h, &ox, &oy, &px);
    // 翻轉時，左上角相對腳底的位置也要鏡像：原本在左邊 ox，翻過來變成右邊 -(ox + w)
    if (flip) ox = -(ox + w);
    // 放大縮小都以腳底為中心。用「左緣、右緣分別換算」再相減，
    // 相鄰兩格動畫的邊緣才會對齊，不會因為四捨五入抖一個像素
    int x0 = x + (ox * scale >> 8), x1 = x + ((ox + w) * scale >> 8);
    int y0 = y + (oy * scale >> 8), y1 = y + ((oy + h) * scale >> 8);
    gfx_sprite_draw(px, w, h, x0, y0, x1 - x0, y1 - y0, flip, depth);
    return 0;
}

// pakdata(name)：資源包裡的一段資料，當成字串回傳（大量對話文字、表格）
static int l_pakdata(lua_State *L)
{
    Entry e = need(L, 1, PAK_DATA);
    luaL_Buffer b;
    char *dst = luaL_buffinitsize(L, &b, e.size);
    pak_read(e.off, dst, e.size);
    luaL_pushresultsize(&b, e.size);
    return 1;
}

void pak_open_lua(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "pakhas",   l_pakhas },
        { "pakdata",  l_pakdata },
        { "bgload",   l_bgload },
        { "bg",       l_bg },
        { "bgdepth",  l_bgdepth },
        { "palload",  l_palload },
        { "paltint",  l_paltint },
        { "sprload",  l_sprload },
        { "sprfree",  l_sprfree },
        { "sprcount", l_sprcount },
        { "sprinfo",  l_sprinfo },
        { "sprdraw",  l_sprdraw },
        { NULL, NULL },
    };
    for (const luaL_Reg *r = api; r->name != NULL; r++) lua_register(L, r->name, r->func);
}
