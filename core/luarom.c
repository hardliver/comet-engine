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
#include "ltable.h"
#include "ltm.h"

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
    int last_nup;   // 最後寫出的函式有幾個上值（不能讀回唯讀區檢查：主機上可能還在緩衝區，沒寫進 Flash）
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
                    // 不能用 setsvalue：它會讀 ts->tt 決定型別，但 ts 剛寫進唯讀區，
                    // 主機上可能還在緩衝區、沒寫進 Flash，讀到的是 0xFF。型別檔案裡就有（t）
                    val_(o).gc = obj2gco(ts);
                    settt_(o, ctb(t));
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
    b->last_nup = f.sizeupvalues;
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
        if (main == NULL || nup != b->last_nup) fail(b, "主函式格式錯誤");
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

// ============================================================================
// 凍結表格（freeze）
// ============================================================================
// 做法和 luarom_build 一樣「先寫子、再寫父」：表格裡的字串、子表格先複製進唯讀區，
// 拿到它們的新位址之後，才把這個表格的陣列部分、雜湊部分、表格本身寫進去。
// 雜湊部分（Node 陣列）原封不動照抄：每個 key 在哪一格、鏈結的相對位置都不變，
// 所以查表、pairs 的順序都和凍結前一模一樣（各平台結果才會一致）。

static RomAppend rom_append = NULL;
static char *fallback_arena;          // 沒有 Flash 的平台：借一塊 RAM 當唯讀區
static size_t fallback_used;
#define FALLBACK_SIZE (2 * 1024 * 1024)

void luarom_set_append(RomAppend fn) { rom_append = fn; }

void luarom_freeze_reset(void)
{
    // 上一款遊戲的 Lua 已經關掉，借來的那塊沒有人參照了，從頭用
    fallback_used = 0;
}

static void *fallback_append(const void *data, size_t len, size_t align)
{
    size_t at = (fallback_used + align - 1) / align * align;
    if (at + len > FALLBACK_SIZE) return NULL;
    memcpy(fallback_arena + at, data, len);
    fallback_used = at + len;
    return fallback_arena + at;
}

// 已經凍結過的物件：RAM 的位址 → 唯讀區的位址（同一個子表格被參照兩次，只複製一次）
typedef struct { const void *from; void *to; } PtrPair;
typedef struct {
    lua_State *L;
    PtrPair *map;
    size_t nmap, cap;
    const char *err;      // 出錯的原因（NULL = 沒錯）
} Freezer;

#define IN_PROGRESS ((void *)1)   // 正在凍結中（用來發現「互相參照成環」）

static void **map_slot(Freezer *f, const void *from)
{
    size_t mask = f->cap - 1;
    size_t i = ((size_t)from >> 3) & mask;
    while (f->map[i].from != NULL && f->map[i].from != from) i = (i + 1) & mask;
    if (f->map[i].from == NULL) {
        f->map[i].from = from;
        f->nmap++;
    }
    return &f->map[i].to;
}

static int map_grow(Freezer *f)
{
    if ((f->nmap + 1) * 2 <= f->cap) return 0;
    size_t ncap = f->cap * 2;
    PtrPair *m = calloc(ncap, sizeof *m);
    if (m == NULL) return 1;
    for (size_t i = 0; i < f->cap; ++i) {
        if (f->map[i].from == NULL) continue;
        size_t j = ((size_t)f->map[i].from >> 3) & (ncap - 1);
        while (m[j].from != NULL) j = (j + 1) & (ncap - 1);
        m[j] = f->map[i];
    }
    free(f->map);
    f->map = m;
    f->cap = ncap;
    return 0;
}

static void *fz_append(Freezer *f, const void *data, size_t len, size_t align)
{
    void *p = rom_append(data, len, align);
    if (p == NULL && f->err == NULL) f->err = "freeze: 唯讀區空間不夠";
    return p;
}

static TString *fz_string(Freezer *f, TString *s)
{
    if (lua_isrom(s)) return s;   // 本來就在唯讀區（程式裡的字串常數）
    if (map_grow(f)) { f->err = "freeze: 記憶體不夠"; return s; }
    void **slot = map_slot(f, s);
    if (*slot != NULL) return *slot;
    size_t l = tsslen(s);
    size_t size = STR_HEAD + l + 1;
    char *buf = malloc(size);
    if (buf == NULL) { f->err = "freeze: 記憶體不夠"; return s; }
    memcpy(buf, s, size);
    TString *t = (TString *)buf;
    t->next = NULL;
    t->marked = 0;   // 灰色：GC 不會碰
    if (t->tt == LUA_VSHRSTR) {
        t->u.hnext = NULL;
    } else {
        t->hash = luaS_hashlongstr(s);   // 先在 RAM 那份算好雜湊（會寫 s，s 在 RAM 所以沒關係）
        t->extra = 1;
    }
    TString *r = fz_append(f, buf, size, ALIGNOF(TString));
    free(buf);
    if (r == NULL) return s;
    *map_slot(f, s) = r;
    return r;
}

static Table *fz_table(Freezer *f, Table *t);

// 把一個值換成唯讀區的版本（字串、表格）；不能凍結的東西記下錯誤
static void fz_value(Freezer *f, TValue *v)
{
    switch (ttypetag(v)) {
        case LUA_VNIL: case LUA_VFALSE: case LUA_VTRUE: case LUA_VEMPTY: case LUA_VABSTKEY:
            // 這些值只看型別標記，value_ 欄位沒有用到，裡面是記憶體的雜訊（每次開機不同）。
            // 清成 0：凍結的結果每次都一樣，主機才會判斷「和 Flash 上的一樣，不用寫」
            memset(&v->value_, 0, sizeof v->value_);
            return;
        case LUA_VNUMINT: case LUA_VNUMFLT:
            return;   // 數字：照抄
        case LUA_VSHRSTR: case LUA_VLNGSTR: {
            TString *s = fz_string(f, tsvalue(v));
            setsvalue(f->L, v, s);
            return;
        }
        case LUA_VTABLE: {
            Table *t = fz_table(f, hvalue(v));
            sethvalue(f->L, v, t);
            return;
        }
        default:
            if (f->err == NULL) f->err = "freeze: 表格裡不能有函式、userdata 或 thread";
            return;
    }
}

static Table *fz_table(Freezer *f, Table *t)
{
    if (lua_isrom(t)) return t;   // 已經凍結
    if (map_grow(f)) { f->err = "freeze: 記憶體不夠"; return t; }
    void **slot = map_slot(f, t);
    if (*slot == IN_PROGRESS) { f->err = "freeze: 表格互相參照（成環），不能凍結"; return t; }
    if (*slot != NULL) return *slot;
    if (t->metatable != NULL) { f->err = "freeze: 有 metatable 的表格不能凍結"; return t; }
    *slot = IN_PROGRESS;

    // metamethod 快取：這個表格如果被當成 metatable，Lua 會在 flags 記下「沒有 __index」之類的，
    // 那是寫入。先算好全部記上（在唯讀區就不會再寫）。最高位元（BITRAS）照原本的
    unsigned int flags = t->flags & BITRAS;
    for (int e = 0; e <= TM_EQ; e++) {
        if (isabstkey(luaH_getshortstr(t, G(f->L)->tmname[e]))) flags |= 1u << e;
    }

    unsigned int asize = luaH_realasize(t);
    unsigned int nsize = isdummy(t) ? 0 : (unsigned int)sizenode(t);

    // 陣列部分、雜湊部分先在 RAM 複製一份，把裡面的字串、子表格換成唯讀區的版本
    TValue *arr = asize ? malloc(asize * sizeof(TValue)) : NULL;
    Node *nodes = nsize ? malloc(nsize * sizeof(Node)) : NULL;
    if ((asize && arr == NULL) || (nsize && nodes == NULL)) {
        free(arr); free(nodes);
        f->err = "freeze: 記憶體不夠";
        return t;
    }
    // 結構裡有對齊用的空隙（TValue 16 bytes 只用到 9 bytes），直接整塊複製會把記憶體裡的雜訊
    // 一起抄過去，每次開機都不一樣，主機就會以為內容變了而重寫 Flash。所以先清成 0，只填有意義的欄位
    memset(arr, 0, asize * sizeof(TValue));
    memset(nodes, 0, nsize * sizeof(Node));
    for (unsigned int i = 0; i < asize; i++) {
        arr[i].value_ = t->array[i].value_;
        arr[i].tt_ = t->array[i].tt_;
        fz_value(f, &arr[i]);
    }
    for (unsigned int i = 0; i < nsize; i++) {
        Node *n = &nodes[i];
        const Node *o = &t->node[i];
        n->u.value_ = o->u.value_;
        n->u.tt_ = o->u.tt_;
        n->u.key_tt = o->u.key_tt;
        n->u.next = o->u.next;
        n->u.key_val = o->u.key_val;
        fz_value(f, gval(n));
        if (keyisdead(n)) {
            memset(&n->u.key_val, 0, sizeof n->u.key_val);   // 已經刪掉的 key：指向的物件可能被回收，清掉
        } else if (keytt(n) == LUA_VNIL || keytt(n) == LUA_VFALSE || keytt(n) == LUA_VTRUE) {
            memset(&n->u.key_val, 0, sizeof n->u.key_val);   // 空位或布林 key：同上，只有雜訊
        } else if (keyiscollectable(n)) {
            TValue k;
            getnodekey(f->L, &k, n);
            fz_value(f, &k);
            n->u.key_val = k.value_;
        }
    }

    // 表格本身也一樣：先清成 0（對齊用的空隙），再一個一個欄位填
    Table copy;
    memset(&copy, 0, sizeof copy);
    copy.next = NULL;
    copy.tt = t->tt;
    copy.marked = 0;          // 灰色：GC 不會碰
    copy.flags = (lu_byte)flags;
    copy.lsizenode = t->lsizenode;
    copy.alimit = t->alimit;
    copy.array = NULL;
    copy.node = t->node;      // 沒有雜湊部分時指向 Lua 共用的空節點（下面有的話會換掉）
    copy.lastfree = NULL;
    copy.metatable = NULL;
    copy.gclist = NULL;
    if (f->err == NULL && asize) copy.array = fz_append(f, arr, asize * sizeof(TValue), ALIGNOF(TValue));
    if (f->err == NULL && nsize) {
        Node *rn = fz_append(f, nodes, nsize * sizeof(Node), ALIGNOF(Node));
        copy.node = rn;
        // lastfree 只有新增 key 時才用（凍結的表格不會新增），但不能是 NULL（NULL 代表沒有雜湊部分）
        if (rn != NULL) copy.lastfree = rn + (t->lastfree - t->node);
    }
    // 沒有雜湊部分（nsize == 0）：node 照舊指向 Lua 共用的空節點、lastfree 是 NULL
    free(arr);
    free(nodes);
    if (f->err != NULL) { *map_slot(f, t) = NULL; return t; }

    Table *r = fz_append(f, &copy, sizeof copy, ALIGNOF(Table));
    *map_slot(f, t) = r != NULL ? r : NULL;
    return r != NULL ? r : t;
}

int luarom_freeze(lua_State *L, int idx)
{
    if (!lua_istable(L, idx)) {
        lua_pushstring(L, "freeze: 需要一個表格");
        return 1;
    }
    if (rom_append == NULL && lua_rom_size != 0 && fallback_arena == NULL) {
        // 平台已經有唯讀區（程式放在裡面）卻沒提供寫入方式：不能另外借一塊，
        // 否則會蓋掉原本的唯讀區範圍，程式裡的字串就認不得了
        lua_pushstring(L, "freeze: 平台沒有提供唯讀區的寫入方式");
        return 1;
    }
    if (rom_append == NULL) {
        // 沒有 Flash 的平台：第一次凍結時借一塊 RAM 當唯讀區
        // （只有在還沒有唯讀區時才這樣做；主機、Mac 的 --rom 由平台設定 rom_append）
        if (fallback_arena == NULL) {
            fallback_arena = malloc(FALLBACK_SIZE);
            if (fallback_arena == NULL) { lua_pushstring(L, "freeze: 記憶體不夠"); return 1; }
            luarom_activate(fallback_arena, FALLBACK_SIZE);
        }
        rom_append = fallback_append;
    }
    Freezer f = { L, NULL, 0, 256, NULL };
    f.map = calloc(f.cap, sizeof(PtrPair));
    if (f.map == NULL) { lua_pushstring(L, "freeze: 記憶體不夠"); return 1; }
    // lua_topointer：表格的話就是它的 Table 結構位址
    Table *t = fz_table(&f, (Table *)lua_topointer(L, idx));
    free(f.map);
    if (f.err != NULL) {
        lua_pushstring(L, f.err);
        return 1;
    }
    sethvalue2s(L, L->top.p, t);
    L->top.p++;
    return 0;
}
