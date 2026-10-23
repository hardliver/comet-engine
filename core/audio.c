// audio.c — 音效與音樂的合成（用法見 audio.h）
//
// 聲音是怎麼算出來的：
//   每個聲道有一個「相位」（phase，32 位元整數），每個取樣加上一個「增量」（由音高決定）。
//   相位從 0 加到 2^32 再繞回 0，就是波形的一個週期；增量越大，一秒繞越多圈，聲音越高。
//   把相位換成波形的高低（三角波、方波……）、乘上音量，4 個聲道加起來就是喇叭要出的聲音。
//   全部都是整數運算（unsigned 整數溢位在 C 裡是定義好的「繞回 0」），所以各平台算出來一模一樣。

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "audio.h"

typedef struct {
    uint8_t pitch, wave, vol, fx;
} Note;

typedef struct {
    Note notes[SFX_NOTES];
    uint8_t len;          // 幾個音符
    uint8_t speed;        // 一個音符多長（1/120 秒為單位）
    int8_t loop_start;    // 沒有循環是 -1
    int8_t loop_end;
} Sound;

enum { WAVE_TRI, WAVE_SQUARE, WAVE_PULSE, WAVE_SAW, WAVE_NOISE, WAVE_COUNT };
enum { FX_NONE, FX_SLIDE, FX_VIBRATO, FX_FADE, FX_COUNT };

typedef struct {
    Sound snd;            // 正在播的音效（自己複製一份：Lua 那邊的資料之後怎麼變都不影響）
    int active;
    int from_music;       // 1 = 音樂在用這個聲道；0 = sfx() 播的音效
    int note;             // 播到第幾個音符
    uint32_t t;           // 這個音符已經播了幾個取樣
    uint32_t phase;
    uint16_t lfsr;        // 雜訊用的亂數（線性回饋移位暫存器，見 noise_step）
    uint8_t prev_pitch;   // 滑音從哪個音高滑過來
} Channel;

static Channel ch[AUDIO_CHANNELS];

// 音樂：開始時把用到的音效複製進池子，每一段（pattern）記 4 個聲道各用池子的第幾個（-1 = 這段不出聲）
static struct {
    int active;
    Sound pool[AUDIO_SONG_SFX];
    char names[AUDIO_SONG_SFX][24];
    int pool_len;
    int8_t pats[AUDIO_SONG_PATS][AUDIO_CHANNELS];
    int n_pats;
    int loop_to;          // 播完最後一段跳回第幾段；-1 = 不循環，播完就停
    int cur;              // 正在播第幾段
    uint32_t pat_t, pat_len;
} song;

static int16_t out[AUDIO_FRAME_MAX];
static int out_len;

// ============================================================
// 音高 → 相位增量
// ============================================================
// C2（65.41 Hz）起 12 個半音的增量：增量 = 頻率 × 2^32 ÷ 22050，頻率 = 65.41 × 2^(半音 ÷ 12)（用 Python 算好貼上）。
// 高八度 = 頻率兩倍 = 增量左移 1 位。音高 63 是 D#7，左移 5 位還放得進 32 位元
static const uint32_t PITCH_BASE[12] = {
    12740059, 13497623, 14300233, 15150569, 16051469, 17005939,
    18017165, 19088521, 20223583, 21426140, 22700205, 24050029,
};

static uint32_t pitch_inc(int pitch)
{
    return PITCH_BASE[pitch % 12] << (pitch / 12);
}

// 一個音符幾個取樣：speed × 22050 ÷ 120 = speed × 183.75。寫成 speed × 735 ÷ 4（先乘再除，整數不失真）
static uint32_t note_len(int speed)
{
    return (uint32_t)speed * 735 / 4;
}

// 雜訊：15 位元的線性回饋移位暫存器（Game Boy 的雜訊聲道也是這樣做的）。
// 每次把最低兩位做 XOR，放到最高位，整個往右移一格；會跑過 32767 種不重複的狀態，聽起來就是沙沙聲
static uint16_t noise_step(uint16_t r)
{
    uint16_t bit = (r ^ (r >> 1)) & 1;
    return (uint16_t)((r >> 1) | (bit << 14));
}

// 相位（取最高 8 位：0–255）→ 波形的高低（大約 -128～127）
static int wave_value(Channel *c, int wave)
{
    int p = (int)(c->phase >> 24);
    switch (wave) {
    case WAVE_TRI:    return p < 128 ? p * 2 - 128 : 383 - p * 2;
    case WAVE_SQUARE: return p < 128 ? 80 : -80;      // 方波聽起來比較大聲，所以振幅小一點
    case WAVE_PULSE:  return p < 64 ? 80 : -80;       // 四分之一週期是高的：比較尖、比較細
    case WAVE_SAW:    return (p - 128) * 3 / 4;
    default:          return (c->lfsr & 1) ? 80 : -80;
    }
}

// 一個聲道的一個取樣（大約 -889～889）；音效播完就把聲道關掉
static int channel_sample(Channel *c)
{
    if (!c->active) return 0;
    const Sound *s = &c->snd;
    const Note *n = &s->notes[c->note];
    uint32_t dur = note_len(s->speed);
    int value = 0;

    if (n->vol > 0) {
        uint32_t inc = pitch_inc(n->pitch);
        if (n->fx == FX_SLIDE) {
            // 從上一個音的增量，照這個音播了多少比例，慢慢變到這個音的增量（有號的 64 位元，避免相減變負數溢位）
            int64_t a = pitch_inc(c->prev_pitch);
            inc = (uint32_t)(a + ((int64_t)inc - a) * c->t / dur);
        } else if (n->fx == FX_VIBRATO) {
            // 每 64 個取樣走一步、32 步一個來回的三角形（約 6 Hz），讓增量上下 ±3%
            int k = (int)(c->t / 64 % 32);
            int tri = k < 16 ? k - 8 : 24 - k;
            inc = (uint32_t)((int64_t)inc + (int64_t)(inc / 256) * tri);
        }
        uint32_t old = c->phase;
        c->phase += inc;
        if ((old >> 28) != (c->phase >> 28)) c->lfsr = noise_step(c->lfsr);   // 雜訊：相位每走 1/16 圈換一個亂數
        value = wave_value(c, n->wave) * n->vol;
        if (n->fx == FX_FADE) value = (int)((int64_t)value * (int64_t)(dur - c->t) / (int64_t)dur);
    }

    // 往下一個取樣；這個音符播完就換下一個
    c->t++;
    if (c->t >= dur) {
        c->t = 0;
        c->prev_pitch = n->pitch;
        c->note++;
        if (s->loop_start >= 0 && c->note > s->loop_end) c->note = s->loop_start;
        if (c->note >= s->len) c->active = 0;
    }
    return value;
}

static void channel_start(int i, const Sound *s, int from_music)
{
    Channel *c = &ch[i];
    c->snd = *s;                        // 結構可以直接用 = 複製（整塊記憶體照抄）
    c->active = s->len > 0;
    c->from_music = from_music;
    c->note = 0;
    c->t = 0;
    c->prev_pitch = s->len > 0 ? s->notes[0].pitch : 0;
    if (c->lfsr == 0) c->lfsr = 1;      // 全部是 0 的話雜訊會一直是 0（XOR 出來還是 0）
}

// ============================================================
// 音樂
// ============================================================

// 開始播第 cur 段：每個聲道照這段的安排換音效；被 sfx() 占用的聲道先不動（音效播完，下一段再回來）
static void song_start_pattern(void)
{
    song.pat_t = 0;
    song.pat_len = 1;
    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        int k = song.pats[song.cur][i];
        if (k >= 0) {
            const Sound *s = &song.pool[k];
            uint32_t len = note_len(s->speed) * s->len;
            if (len > song.pat_len) song.pat_len = len;   // 這一段的長度 = 最長的那個音效
        }
        if (ch[i].active && !ch[i].from_music) continue;
        if (k >= 0) channel_start(i, &song.pool[k], 1);
        else ch[i].active = 0;
    }
}

static void song_step(void)
{
    if (!song.active) return;
    song.pat_t++;
    if (song.pat_t < song.pat_len) return;
    song.cur++;
    if (song.cur >= song.n_pats) {
        if (song.loop_to < 0) {
            song.active = 0;
            return;
        }
        song.cur = song.loop_to;
    }
    song_start_pattern();
}

// ============================================================
// 每一幀
// ============================================================

void audio_reset(void)
{
    memset(ch, 0, sizeof(ch));
    song.active = 0;
    out_len = 0;
}

void audio_frame(uint32_t frame)
{
    // 22050 ÷ 60 = 367.5：偶數幀 367 個、奇數幀 368 個，兩幀剛好 735 個 = 1/30 秒
    out_len = (frame % 2 == 0) ? 367 : 368;
    for (int i = 0; i < out_len; i++) {
        int sum = 0;
        for (int k = 0; k < AUDIO_CHANNELS; k++) sum += channel_sample(&ch[k]);
        song_step();
        // 4 個聲道最大加起來約 ±3556，乘 9 還在 16 位元（±32767）以內
        out[i] = (int16_t)(sum * 9);
    }
}

const int16_t *audio_samples(void) { return out; }
int audio_sample_count(void) { return out_len; }

// ============================================================
// Lua API
// ============================================================

// 從 Lua 的 SFX 表格讀一個音效（合併工具產生的：{ speed = 4, notes = "十六進位字串", loop = { 開始, 結束 } }）
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void read_sound(lua_State *L, const char *name, Sound *s)
{
    lua_getglobal(L, "SFX");
    if (!lua_istable(L, -1)) luaL_error(L, "沒有音效資料：在 comet.json 加上 \"sfx\": \"sfx.json\"");
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) luaL_error(L, "沒有叫「%s」的音效（sfx 檔裡找不到）", name);

    lua_getfield(L, -1, "speed");
    lua_Integer speed = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (speed < 1 || speed > 255) luaL_error(L, "音效「%s」的 speed 要是 1–255", name);
    s->speed = (uint8_t)speed;

    lua_getfield(L, -1, "notes");
    size_t len = 0;
    const char *hex = lua_tolstring(L, -1, &len);
    if (hex == NULL || len % 4 != 0 || len / 4 > SFX_NOTES) luaL_error(L, "音效「%s」的音符資料壞掉了（重新合併一次）", name);
    // 每個音符 4 個十六進位字 = 16 位元：音高（低 6 位）、波形（3 位）、音量（3 位）、效果（3 位）
    s->len = (uint8_t)(len / 4);
    for (int i = 0; i < s->len; i++) {
        int v = 0;
        for (int k = 0; k < 4; k++) {
            int h = hexval(hex[i * 4 + k]);
            if (h < 0) luaL_error(L, "音效「%s」的音符資料壞掉了（重新合併一次）", name);
            v = v * 16 + h;
        }
        Note *n = &s->notes[i];
        n->pitch = (uint8_t)(v & 63);
        n->wave = (uint8_t)((v >> 6) & 7);
        n->vol = (uint8_t)((v >> 9) & 7);
        n->fx = (uint8_t)((v >> 12) & 7);
        if (n->wave >= WAVE_COUNT) n->wave = WAVE_TRI;
        if (n->fx >= FX_COUNT) n->fx = FX_NONE;
    }
    lua_pop(L, 1);

    s->loop_start = s->loop_end = -1;
    lua_getfield(L, -1, "loop");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        lua_Integer a = lua_tointeger(L, -2), b = lua_tointeger(L, -1);
        lua_pop(L, 2);
        if (a >= 0 && b >= a && b < s->len) {
            s->loop_start = (int8_t)a;
            s->loop_end = (int8_t)b;
        }
    }
    lua_pop(L, 3);   // loop、這個音效的表格、SFX
}

// sfx(名字, 聲道)：回傳用了哪個聲道。sfx(nil, 聲道) 停一個聲道；sfx(nil) 停掉所有 sfx 播的（音樂不停）
static int l_sfx(lua_State *L)
{
    int want = -1;
    if (!lua_isnoneornil(L, 2)) {
        lua_Integer c = luaL_checkinteger(L, 2);
        luaL_argcheck(L, c >= 0 && c < AUDIO_CHANNELS, 2, "聲道只有 0–3");
        want = (int)c;
    }
    if (lua_isnoneornil(L, 1)) {
        for (int i = 0; i < AUDIO_CHANNELS; i++) {
            if (want == i || (want < 0 && !ch[i].from_music)) ch[i].active = 0;
        }
        return 0;
    }
    Sound s;
    read_sound(L, luaL_checkstring(L, 1), &s);
    int use = want;
    if (use < 0) {
        // 自動找聲道：先找空的；都在響的話，占用音樂的聲道（從 3 號往前找）；再不行就用 3 號
        for (int i = 0; i < AUDIO_CHANNELS && use < 0; i++) if (!ch[i].active) use = i;
        for (int i = AUDIO_CHANNELS - 1; i >= 0 && use < 0; i--) if (ch[i].from_music) use = i;
        if (use < 0) use = AUDIO_CHANNELS - 1;
    }
    channel_start(use, &s, 0);
    lua_pushinteger(L, use);
    return 1;
}

// 音樂池子裡找名字；沒有就讀進來。回傳在池子的第幾個
static int song_pool(lua_State *L, const char *name)
{
    for (int i = 0; i < song.pool_len; i++) if (strcmp(song.names[i], name) == 0) return i;
    if (song.pool_len >= AUDIO_SONG_SFX) luaL_error(L, "一首音樂最多用 %d 個不同的音效", AUDIO_SONG_SFX);
    if (strlen(name) >= sizeof(song.names[0])) luaL_error(L, "音效的名字太長（最多 %d 個英文字）：%s", (int)sizeof(song.names[0]) - 1, name);
    read_sound(L, name, &song.pool[song.pool_len]);
    strcpy(song.names[song.pool_len], name);
    return song.pool_len++;
}

// music(名字)：從頭播；music()：停掉
static int l_music(lua_State *L)
{
    // 先停掉音樂在用的聲道
    song.active = 0;
    for (int i = 0; i < AUDIO_CHANNELS; i++) if (ch[i].from_music) ch[i].active = 0;
    if (lua_isnoneornil(L, 1)) return 0;

    const char *name = luaL_checkstring(L, 1);
    lua_getglobal(L, "MUSIC");
    if (!lua_istable(L, -1)) luaL_error(L, "沒有音樂資料：在 sfx 檔加上 \"music\"");
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) luaL_error(L, "沒有叫「%s」的音樂", name);
    int song_idx = lua_gettop(L);

    song.pool_len = 0;
    lua_getfield(L, song_idx, "patterns");
    if (!lua_istable(L, -1)) luaL_error(L, "音樂「%s」沒有 patterns", name);
    int n = (int)lua_rawlen(L, -1);
    if (n < 1 || n > AUDIO_SONG_PATS) luaL_error(L, "音樂「%s」要有 1–%d 段", name, AUDIO_SONG_PATS);
    for (int p = 0; p < n; p++) {
        lua_rawgeti(L, -1, p + 1);   // 第 p 段：{ "聲道0的音效", "聲道1的音效", … }
        for (int i = 0; i < AUDIO_CHANNELS; i++) {
            lua_rawgeti(L, -1, i + 1);
            const char *s = lua_tostring(L, -1);
            // 注意：s 指向 Lua 的字串，pop 之後可能被回收，所以在 pop 之前用完
            song.pats[p][i] = (int8_t)((s && s[0]) ? song_pool(L, s) : -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   // patterns
    song.n_pats = n;

    lua_getfield(L, song_idx, "loop");
    song.loop_to = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : -1;
    if (song.loop_to >= n) song.loop_to = -1;
    lua_pop(L, 3);   // loop、這首音樂、MUSIC

    song.cur = 0;
    song.active = 1;
    song_start_pattern();
    return 0;
}

void audio_open_lua(lua_State *L)
{
    lua_register(L, "sfx", l_sfx);
    lua_register(L, "music", l_music);
}
