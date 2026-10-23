// luarom.c — 把預先編譯好的 Lua 程式放進唯讀記憶體（說明見 luarom.h）
//
// 讀的格式就是 Lua 自己的位元組碼（lua_dump 的輸出），所以這個檔案照著
// third_party/lua/lundump.c 的順序讀；差別只在「東西放哪裡」：
//   lundump.c：每個陣列、字串都 malloc 在 RAM，之後還會回頭填欄位
//   這裡：全部依序接在唯讀區的後面（Flash 不能回頭改），所以
//     - 內容一讀到就能確定的（指令、行號）：直接寫
//     - 要等「之後才讀到的東西」才能填好的（常數表要指向字串、函式本身要指向子函式）：
//       先在 RAM 暫存，等需要的東西都寫進唯讀區了，再整塊寫出去。
//       所以子函式一定比父函式先寫，函式（Proto）本身是它那一組裡最後寫的。
//
// 這裡用到 Lua 的內部結構（lobject.h 等），因為要自己組出 Proto、TString。

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luarom.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lfunc.h"
#include "lgc.h"
#include "lundump.h"

// 某個型別需要的對齊（C99 沒有 _Alignof，用這個小技巧：
// 把它放在一個 char 後面，編譯器補了幾個 byte，就是它的對齊）
#define ALIGNOF(T) offsetof(struct { char c; T t; }, t)

// ============================================================================
// 建立過程的狀態
// ============================================================================

typedef struct {
    uint32_t off1;   // 字串在唯讀區的位置 + 1（0 = 空位）
    uint32_t hash;
} StrSlot;

typedef struct {
    lua_State *L;
    RomReader read;
    void *read_ud;
    RomWriter *w;
    unsigned int seed;   // Lua 字串雜湊的種子（Comet 固定是 0）

    // 短字串去重複：同樣內容的短字串在唯讀區只放一份。
    // 用「開放定址」雜湊表：算出位置，被占了就往下一格找
    StrSlot *slots;
    size_t nslots, nstrings;

    // 出錯時跳回 luarom_build（見下面 fail 的說明）
    jmp_buf jb;
    char *err;
    size_t err_size;
    void *tmp;   // 出錯時要釋放的暫存記憶體（同時只會有一塊大的）
} Builder;

// 出錯：記下原因，跳回 luarom_build 的 setjmp。
// 為什麼用 setjmp/longjmp：讀取在很多層函式裡（函式裡有子函式，一直遞迴下去），
// 每一層都檢查回傳值會讓程式很長。longjmp 直接回到最外層，但要注意：
// 中間各層 malloc 的東西不會自動釋放，所以暫存記憶體要記在 b->tmp、由最外層負責 free。
static void fail(Builder *b, const char *why)
{
    snprintf(b->err, b->err_size, "luarom: %s", why);
    longjmp(b->jb, 1);
}

// ============================================================================
// 讀：照 lundump.c 的格式
// ============================================================================

static void rd(Builder *b, void *buf, size_t n)
{
    if (n > 0 && b->read(b->read_ud, buf, n) != 0) fail(b, "檔案太短（讀取失敗）");
}

static lu_byte rd_byte(Builder *b)
{
    lu_byte x;
    rd(b, &x, 1);
    return x;
}

// 不定長度的整數：每個 byte 放 7 位元，最後一個 byte 的最高位元是 1
static size_t rd_unsigned(Builder *b, size_t limit)
{
    size_t x = 0;
    int c;
    limit >>= 7;
    do {
        c = rd_byte(b);
        if (x >= limit) fail(b, "整數太大");
        x = (x << 7) | (c & 0x7f);
    } while ((c & 0x80) == 0);
    return x;
}

static int rd_int(Builder *b) { return (int)rd_unsigned(b, INT_MAX); }
static size_t rd_size(Builder *b) { return rd_unsigned(b, (size_t)-1); }

// ============================================================================
// 寫：依序接在唯讀區後面
// ============================================================================

// 寫入 len bytes（先補 0 到 align 的倍數），回傳寫入位置的位址
static void *emit(Builder *b, const void *data, size_t len, size_t align)
{
    static const char zeros[16] = {0};
    RomWriter *w = b->w;
    while (w->used % align != 0) {
        size_t pad = align - w->used % align;
        if (pad > sizeof zeros) pad = sizeof zeros;
        if (w->used + pad > w->capacity) fail(b, "唯讀區空間不夠");
        if (w->write(w->ud, w->used, zeros, pad) != 0) fail(b, "寫入失敗");
        w->used += pad;
    }
    if (w->used + len > w->capacity) fail(b, "唯讀區空間不夠");
    size_t at = w->used;
    if (len > 0 && w->write(w->ud, at, data, len) != 0) fail(b, "寫入失敗");
    w->used += len;
    return w->base + at;
}

// 從檔案讀 len bytes，直接接著寫進唯讀區（分小塊，不用整塊放進 RAM）
static void *emit_stream(Builder *b, size_t len, size_t align)
{
    char chunk[256];
    void *start = NULL;
    size_t done = 0;
    if (len == 0) return emit(b, NULL, 0, align);
    while (done < len) {
        size_t n = len - done < sizeof chunk ? len - done : sizeof chunk;
        rd(b, chunk, n);
        void *p = emit(b, chunk, n, done == 0 ? align : 1);
        if (done == 0) start = p;
        done += n;
    }
    return start;
}

// ============================================================================
// 字串
// ============================================================================

// 字串物件的大小：表頭 + 內容 + 結尾的 0
#define STR_HEAD offsetof(TString, contents)

// 在去重複表找同樣內容的短字串，有就回傳它的位址
static TString *find_short(Builder *b, const char *s, size_t l, unsigned int h)
{
    size_t mask = b->nslots - 1;
    for (size_t i = h & mask; b->slots[i].off1 != 0; i = (i + 1) & mask) {
        if (b->slots[i].hash != h) continue;
        // 讀回唯讀區裡的那個字串來比內容
        union { TString ts; char raw[STR_HEAD + LUAI_MAXSHORTLEN + 1]; } u;
        size_t off = b->slots[i].off1 - 1;
        b->w->read(b->w->ud, off, u.raw, STR_HEAD + l);
        if (u.ts.shrlen == l && memcmp(u.raw + STR_HEAD, s, l) == 0)
            return (TString *)(b->w->base + off);
    }
    return NULL;
}

static void remember_short(Builder *b, TString *ts, unsigned int h)
{
    // 超過一半滿就加大一倍（越滿越慢）
    if ((b->nstrings + 1) * 2 > b->nslots) {
        size_t n = b->nslots * 2;
        StrSlot *s = calloc(n, sizeof *s);
        if (s == NULL) fail(b, "記憶體不夠（字串表）");
        for (size_t i = 0; i < b->nslots; ++i) {
            if (b->slots[i].off1 == 0) continue;
            size_t j = b->slots[i].hash & (n - 1);
            while (s[j].off1 != 0) j = (j + 1) & (n - 1);
            s[j] = b->slots[i];
        }
        free(b->slots);
        b->slots = s;
        b->nslots = n;
    }
    size_t mask = b->nslots - 1, i = h & mask;
    while (b->slots[i].off1 != 0) i = (i + 1) & mask;
    b->slots[i].off1 = (uint32_t)((char *)ts - b->w->base) + 1;
    b->slots[i].hash = h;
    b->nstrings++;
}

// 讀一個字串（可以是「沒有」），寫進唯讀區，回傳它的位址
static TString *load_string(Builder *b)
{
    size_t size = rd_size(b);
    if (size == 0) return NULL;   // 沒有字串
    size_t l = size - 1;

    if (l <= LUAI_MAXSHORTLEN) {
        // 短字串：表頭 + 內容一起在 RAM 組好，一次寫出去
        union { TString ts; char raw[STR_HEAD + LUAI_MAXSHORTLEN + 1]; } u;
        memset(&u, 0, sizeof u);
        rd(b, u.raw + STR_HEAD, l);
        unsigned int h = luaS_hash(u.raw + STR_HEAD, l, b->seed);
        TString *same = find_short(b, u.raw + STR_HEAD, l, h);
        if (same != NULL) return same;
        u.ts.next = NULL;
        u.ts.tt = LUA_VSHRSTR;
        u.ts.marked = 0;          // 灰色：GC 不會碰它（見 luarom.h）
        u.ts.extra = 0;
        u.ts.shrlen = (lu_byte)l;
        u.ts.hash = h;            // 和 RAM 裡同內容的字串雜湊值相同，表格才找得到
        u.ts.u.hnext = NULL;
        TString *ts = emit(b, u.raw, STR_HEAD + l + 1, ALIGNOF(TString));
        remember_short(b, ts, h);
        return ts;
    }

    // 長字串：雜湊值要從最後一個字元往前算（luaS_hash 的寫法），
    // 而表頭在內容前面，所以要先把整個內容讀進 RAM 算好，再寫表頭和內容
    char *buf = malloc(STR_HEAD + l + 1);
    if (buf == NULL) fail(b, "記憶體不夠（長字串）");
    b->tmp = buf;
    memset(buf, 0, STR_HEAD);
    rd(b, buf + STR_HEAD, l);
    buf[STR_HEAD + l] = '\0';
    TString *t = (TString *)buf;
    t->next = NULL;
    t->tt = LUA_VLNGSTR;
    t->marked = 0;
    t->extra = 1;                                      // 1 = 雜湊值已經算好（Lua 就不會回頭寫）
    t->shrlen = 0xFF;                                  // 0xFF = 長字串
    t->hash = luaS_hash(buf + STR_HEAD, l, b->seed);   // 和 luaS_hashlongstr 算法相同
    t->u.lnglen = l;
    TString *ts = emit(b, buf, STR_HEAD + l + 1, ALIGNOF(TString));
    b->tmp = NULL;
    free(buf);
    return ts;
}

// ============================================================================
// 函式（Proto）
// ============================================================================

static Proto *load_function(Builder *b, TString *psource);

// 函式需要的 RAM 暫存：這幾個陣列要等之後的東西寫完才能填好
typedef struct {
    Upvaldesc *upvalues;
    Proto **p;
    LocVar *locvars;
} Pending;

static Proto *load_function_inner(Builder *b, TString *psource, Pending *pd)
{
    Proto f;
    memset(&f, 0, sizeof f);

    f.source = load_string(b);
    if (f.source == NULL) f.source = psource;   // 沒寫就和外層函式同一個檔案
    f.linedefined = rd_int(b);
    f.lastlinedefined = rd_int(b);
    f.numparams = rd_byte(b);
    f.is_vararg = rd_byte(b);
    f.maxstacksize = rd_byte(b);

    // 指令：直接寫
    f.sizecode = rd_int(b);
    f.code = emit_stream(b, (size_t)f.sizecode * sizeof(Instruction), ALIGNOF(Instruction));

    // 常數：字串先寫進唯讀區，常數表（TValue 陣列）在 RAM 組好再整塊寫
    f.sizek = rd_int(b);
    {
        TValue *k = calloc((size_t)f.sizek + 1, sizeof(TValue));
        if (k == NULL) fail(b, "記憶體不夠（常數表）");
        b->tmp = k;
        for (int i = 0; i < f.sizek; i++) {
            TValue *o = &k[i];
            int t = rd_byte(b);
            switch (t) {
                case LUA_VNIL:   setnilvalue(o); break;
                case LUA_VFALSE: setbfvalue(o); break;
                case LUA_VTRUE:  setbtvalue(o); break;
                case LUA_VNUMFLT: { lua_Number x; rd(b, &x, sizeof x); setfltvalue(o, x); break; }
                case LUA_VNUMINT: { lua_Integer x; rd(b, &x, sizeof x); setivalue(o, x); break; }
                case LUA_VSHRSTR:
                case LUA_VLNGSTR: {
                    b->tmp = NULL;   // load_string 可能用到 tmp；k 先交給 pd 以外的地方記著
                    TString *ts = NULL;
                    // 讀字串時如果出錯，k 會漏掉沒釋放；建立失敗本來就會放棄整個遊戲，可以接受
                    ts = load_string(b);
                    b->tmp = k;
                    if (ts == NULL) fail(b, "常數字串格式錯誤");
                    setsvalue(b->L, o, ts);
                    break;
                }
                default: fail(b, "未知的常數種類");
            }
        }
        f.k = emit(b, k, (size_t)f.sizek * sizeof(TValue), ALIGNOF(TValue));
        b->tmp = NULL;
        free(k);
    }

    // 上值（upvalue）：名字在除錯資訊裡才讀到，先放 RAM
    f.sizeupvalues = rd_int(b);
    pd->upvalues = calloc((size_t)f.sizeupvalues + 1, sizeof(Upvaldesc));
    if (pd->upvalues == NULL) fail(b, "記憶體不夠（upvalue）");
    for (int i = 0; i < f.sizeupvalues; i++) {
        pd->upvalues[i].instack = rd_byte(b);
        pd->upvalues[i].idx = rd_byte(b);
        pd->upvalues[i].kind = rd_byte(b);
    }

    // 子函式：遞迴，先寫好它們，再寫指向它們的陣列
    f.sizep = rd_int(b);
    pd->p = calloc((size_t)f.sizep + 1, sizeof(Proto *));
    if (pd->p == NULL) fail(b, "記憶體不夠（子函式）");
    for (int i = 0; i < f.sizep; i++) pd->p[i] = load_function(b, f.source);
    f.p = emit(b, pd->p, (size_t)f.sizep * sizeof(Proto *), ALIGNOF(Proto *));

    // 除錯資訊：行號直接寫；區域變數、上值的名字先寫字串再寫陣列
    f.sizelineinfo = rd_int(b);
    f.lineinfo = emit_stream(b, (size_t)f.sizelineinfo, 1);
    f.sizeabslineinfo = rd_int(b);
    {
        AbsLineInfo *a = calloc((size_t)f.sizeabslineinfo + 1, sizeof *a);
        if (a == NULL) fail(b, "記憶體不夠（行號）");
        b->tmp = a;
        for (int i = 0; i < f.sizeabslineinfo; i++) {
            a[i].pc = rd_int(b);
            a[i].line = rd_int(b);
        }
        f.abslineinfo = emit(b, a, (size_t)f.sizeabslineinfo * sizeof *a, ALIGNOF(AbsLineInfo));
        b->tmp = NULL;
        free(a);
    }
    f.sizelocvars = rd_int(b);
    pd->locvars = calloc((size_t)f.sizelocvars + 1, sizeof(LocVar));
    if (pd->locvars == NULL) fail(b, "記憶體不夠（區域變數）");
    for (int i = 0; i < f.sizelocvars; i++) {
        pd->locvars[i].varname = load_string(b);
        pd->locvars[i].startpc = rd_int(b);
        pd->locvars[i].endpc = rd_int(b);
    }
    f.locvars = emit(b, pd->locvars, (size_t)f.sizelocvars * sizeof(LocVar), ALIGNOF(LocVar));
    int nnames = rd_int(b);
    if (nnames != 0) {
        if (nnames != f.sizeupvalues) fail(b, "上值名稱數量不對");
        for (int i = 0; i < nnames; i++) pd->upvalues[i].name = load_string(b);
    }
    f.upvalues = emit(b, pd->upvalues, (size_t)f.sizeupvalues * sizeof(Upvaldesc), ALIGNOF(Upvaldesc));

    // 最後才是函式本身
    f.next = NULL;
    f.tt = LUA_VPROTO;
    f.marked = 0;     // 灰色：GC 不會碰它
    f.gclist = NULL;
    return emit(b, &f, sizeof f, ALIGNOF(Proto));
}

static Proto *load_function(Builder *b, TString *psource)
{
    Pending pd = { NULL, NULL, NULL };
    // 子函式出錯時 longjmp 會直接跳過這裡，pd 會漏掉；
    // 建立失敗就整個放棄，下次開機重來，RAM 也會隨 lua_State 一起重建，可以接受
    Proto *f = load_function_inner(b, psource, &pd);
    free(pd.upvalues);
    free(pd.p);
    free(pd.locvars);
    return f;
}

// ============================================================================
// 對外的函式
// ============================================================================

static void check_header(Builder *b)
{
    char buf[16];
    rd(b, buf, 4);
    if (memcmp(buf, LUA_SIGNATURE, 4) != 0) fail(b, "不是 Lua 位元組碼");
    if (rd_byte(b) != LUAC_VERSION) fail(b, "Lua 版本不同");
    if (rd_byte(b) != LUAC_FORMAT) fail(b, "格式不同");
    rd(b, buf, sizeof(LUAC_DATA) - 1);
    if (memcmp(buf, LUAC_DATA, sizeof(LUAC_DATA) - 1) != 0) fail(b, "檔案損壞");
    if (rd_byte(b) != sizeof(Instruction)) fail(b, "Instruction 大小不同");
    if (rd_byte(b) != sizeof(lua_Integer)) fail(b, "lua_Integer 大小不同");
    if (rd_byte(b) != sizeof(lua_Number)) fail(b, "lua_Number 大小不同");
    lua_Integer i;
    rd(b, &i, sizeof i);
    if (i != LUAC_INT) fail(b, "整數格式不同");
    lua_Number n;
    rd(b, &n, sizeof n);
    if (n != LUAC_NUM) fail(b, "浮點數格式不同");
}

int luarom_build(lua_State *L, RomReader read, void *read_ud, RomWriter *w,
                 const void **root, char *err, size_t err_size)
{
    // volatile：setjmp 之後會改、longjmp 回來還要用的區域變數必須加，
    // 否則編譯器可能把它放在暫存器裡，跳回來時是舊的值
    Builder *volatile b = calloc(1, sizeof(Builder));
    if (b == NULL) {
        snprintf(err, err_size, "luarom: 記憶體不夠");
        return 1;
    }
    b->L = L;
    b->read = read;
    b->read_ud = read_ud;
    b->w = w;
    b->seed = G(L)->seed;
    b->err = err;
    b->err_size = err_size;
    b->nslots = 1024;
    b->slots = calloc(b->nslots, sizeof(StrSlot));
    w->used = 0;

    int result = 1;
    if (b->slots == NULL) {
        snprintf(err, err_size, "luarom: 記憶體不夠");
    } else if (setjmp(b->jb) == 0) {
        check_header(b);
        int nup = rd_byte(b);
        Proto *main = load_function(b, NULL);
        if (main == NULL || nup != main->sizeupvalues) fail(b, "主函式格式錯誤");
        *root = main;
        err[0] = '\0';
        result = 0;
    }
    // 不管成功失敗都會走到這裡（失敗是 longjmp 回到 setjmp，setjmp 回傳 1）
    free(b->tmp);
    free(b->slots);
    free(b);
    return result;
}

void luarom_activate(const char *base, size_t size)
{
    lua_rom_base = (char *)base;
    lua_rom_size = size;
}

void luarom_push_main(lua_State *L, const void *root)
{
    Proto *p = (Proto *)root;
    // 和 lua_load 做的事一樣：建一個閉包（RAM），程式本體指向唯讀區的 Proto，
    // 第一個上值設成全域表（_ENV）
    LClosure *cl = luaF_newLclosure(L, p->sizeupvalues);
    cl->p = p;   // 不需要 luaC_objbarrier：p 是灰色，GC 本來就不會去看它
    setclLvalue2s(L, L->top.p, cl);
    L->top.p++;
    luaF_initupvals(L, cl);
    if (cl->nupvalues >= 1) {
        const TValue *gt = &hvalue(&G(L)->l_registry)->array[LUA_RIDX_GLOBALS - 1];
        setobj(L, cl->upvals[0]->v.p, gt);
        luaC_barrier(L, cl->upvals[0], gt);
    }
}

// lua_dump 呼叫的寫入函式：簽名是 Lua 規定的，轉給呼叫者的 out
typedef struct { int (*out)(void *, const void *, size_t); void *ud; } DumpCtx;

static int dump_writer(lua_State *L, const void *p, size_t n, void *ud)
{
    (void)L;
    DumpCtx *c = ud;
    return c->out(c->ud, p, n);
}

int luarom_compile(lua_State *L, const char *src, size_t len, const char *name,
                   int (*out)(void *ud, const void *p, size_t n), void *out_ud,
                   char *err, size_t err_size)
{
    if (luaL_loadbufferx(L, src, len, name, "t") != LUA_OK) {
        snprintf(err, err_size, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    DumpCtx ctx = { out, out_ud };
    // 最後一個參數 0 = 保留除錯資訊（行號、變數名稱），遊戲出錯時訊息才看得懂
    int r = lua_dump(L, dump_writer, &ctx, 0);
    lua_pop(L, 1);
    if (r != 0) {
        snprintf(err, err_size, "luarom: 寫出位元組碼失敗");
        return 1;
    }
    err[0] = '\0';
    return 0;
}
