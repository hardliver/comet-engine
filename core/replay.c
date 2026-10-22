// replay.c — 重播檔的實作（格式見 replay.h）

#include <stdlib.h>
#include <string.h>
#include "replay.h"
#include "engine.h"
#include "gfx.h"
#include "input.h"
#include "platform.h"
#include "sha256.h"

// 重播檔最大 4 MB：60 幀 × 3600 秒，每幀都在變也只要約 1.5 MB／小時，
// 實際上大部分幀沒有變化，遠小於這個數字。上限是為了 Pico 不要把記憶體吃光
#define REPLAY_MAX (4u * 1024 * 1024)

// 事件開頭那 1 byte 的各個 bit
#define EV_PLAYER_MASK 0x03
#define EV_BUTTONS     0x04
#define EV_LSTICK      0x08
#define EV_RSTICK      0x10
#define EV_TYPE_SHIFT  5
#define EV_TYPE_INPUT  0
#define EV_TYPE_SAVE   1
#define EV_TYPE_CLOCK  2
#define EV_SLOT_MASK   0x07   // 讀存檔事件：bit 0–2 是欄位

// 一位玩家一幀的輸入
typedef struct {
    uint16_t buttons;
    int8_t lx, ly, rx, ry;
} PadState;

static int state = REPLAY_OFF;

// 錄影、播放共用的緩衝區：前 112 bytes 是檔頭，後面是事件
static uint8_t *buf = NULL;
static size_t buf_len = 0;    // 用了幾 bytes
static size_t buf_cap = 0;    // 借了幾 bytes

static PadState pads[MAX_PLAYERS];   // 錄影：上一次記下的輸入；播放：目前的輸入
static uint32_t last_event_frame;    // 上一個事件在第幾幀

// 播放用
static size_t   read_pos;            // 讀到哪個位置
static uint32_t next_event_frame;    // 下一個事件在第幾幀
static uint32_t total_frames;
static int      matched;

// ============================================================
// 讀寫小工具
// ============================================================

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 確保緩衝區還有 n bytes 的空間；不夠就借兩倍大的。失敗回傳 1
static int reserve(size_t n)
{
    if (buf_len + n <= buf_cap) return 0;
    size_t cap = buf_cap ? buf_cap * 2 : 4096;
    while (cap < buf_len + n) cap *= 2;
    if (cap > REPLAY_MAX) return 1;
    uint8_t *p = realloc(buf, cap);
    if (p == NULL) return 1;
    buf = p;
    buf_cap = cap;
    return 0;
}

static void put_byte(uint8_t b) { buf[buf_len++] = b; }

// varint：小的數字用 1 byte，大的才用比較多 byte。
// 每個 byte 用低 7 bit 存數字，最高 bit 是 1 代表「後面還有」。
// 幀數差通常很小（輸入每幾幀才變一次），大多只要 1 byte
static void put_varint(uint32_t v)
{
    while (v >= 0x80) {
        put_byte((uint8_t)(v | 0x80));
        v >>= 7;
    }
    put_byte((uint8_t)v);
}

// 讀 varint；讀到檔尾或格式不對回傳 1
static int get_varint(uint32_t *out)
{
    uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (read_pos >= buf_len) return 1;
        uint8_t b = buf[read_pos++];
        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return 0; }
    }
    return 1;
}

// 畫面的指紋：畫面陣列 + 調色盤（只改調色盤，看起來也不一樣）
static void screen_sha(uint8_t out[32])
{
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, gfx_framebuffer(), SCREEN_W * SCREEN_H);
    sha256_update(&s, gfx_palette(), 256 * 3);
    sha256_final(&s, out);
}

static void release(void)
{
    free(buf);
    buf = NULL;
    buf_len = buf_cap = 0;
}

// ============================================================
// 錄影
// ============================================================

int replay_record_begin(void)
{
    release();
    state = REPLAY_OFF;
    if (reserve(REPLAY_HEADER_SIZE)) return 1;
    // 檔頭要用到遊戲指紋，等 engine_load() 呼叫 replay_on_load() 時才寫
    state = REPLAY_ARMED;
    return 0;
}

// 載入遊戲時真正開始錄：寫好檔頭。總幀數和最後畫面的指紋在 replay_snapshot() 時才填
static void record_start(const uint8_t cart_sha[32])
{
    memset(buf, 0, REPLAY_HEADER_SIZE);
    memcpy(buf, "CRPL", 4);
    put_u16(buf + 4, REPLAY_VERSION);
    put_u16(buf + 6, REPLAY_HEADER_SIZE);
    put_u32(buf + 8, engine_seed());
    memcpy(buf + 16, cart_sha, 32);
    // buf + 48：起始狀態，全部 0 = 從新遊戲開始
    buf_len = REPLAY_HEADER_SIZE;

    // 錄影開始前，當作所有輸入都是「放開」；第 0 幀有按著的鍵就會寫成事件
    memset(pads, 0, sizeof(pads));
    last_event_frame = 0;
    state = REPLAY_RECORDING;
}

// 記下一位玩家這一幀的輸入；和上次記的一樣就不寫
static void record_pad(int player, uint32_t frame)
{
    PadState now = {
        input_get_buttons(player),
        (int8_t)input_stick_x(player, STICK_LEFT),  (int8_t)input_stick_y(player, STICK_LEFT),
        (int8_t)input_stick_x(player, STICK_RIGHT), (int8_t)input_stick_y(player, STICK_RIGHT),
    };
    PadState *was = &pads[player];

    uint8_t head = (uint8_t)player | (EV_TYPE_INPUT << EV_TYPE_SHIFT);
    if (now.buttons != was->buttons)               head |= EV_BUTTONS;
    if (now.lx != was->lx || now.ly != was->ly)    head |= EV_LSTICK;
    if (now.rx != was->rx || now.ry != was->ry)    head |= EV_RSTICK;
    if ((head & (EV_BUTTONS | EV_LSTICK | EV_RSTICK)) == 0) return;   // 沒變化

    // 一個事件最多 5（varint）+ 1 + 2 + 2 + 2 = 12 bytes
    if (reserve(12)) {
        platform_log("重播檔太大，停止錄影");
        state = REPLAY_OFF;
        return;
    }
    put_varint(frame - last_event_frame);
    put_byte(head);
    if (head & EV_BUTTONS) { put_u16(buf + buf_len, now.buttons); buf_len += 2; }
    if (head & EV_LSTICK)  { put_byte((uint8_t)now.lx); put_byte((uint8_t)now.ly); }
    if (head & EV_RSTICK)  { put_byte((uint8_t)now.rx); put_byte((uint8_t)now.ry); }

    *was = now;
    last_event_frame = frame;
}

const uint8_t *replay_snapshot(void)
{
    if (state != REPLAY_RECORDING) return NULL;
    // 填上「到目前為止」的總幀數和畫面指紋。錄影繼續時，下一次取會再覆蓋
    put_u32(buf + 12, engine_frame_count());
    screen_sha(buf + 80);
    return buf;
}

size_t replay_snapshot_size(void)
{
    return state == REPLAY_RECORDING ? buf_len : 0;
}

// ============================================================
// 播放
// ============================================================

// 讀下一個事件的幀數，存進 next_event_frame。沒有事件了就設成「永遠不會到」
static void peek_next_event(void)
{
    uint32_t delta;
    if (read_pos >= buf_len || get_varint(&delta)) {
        next_event_frame = UINT32_MAX;
        return;
    }
    next_event_frame = last_event_frame + delta;
}

int replay_play_begin(const uint8_t *data, size_t len)
{
    replay_stop();
    if (len < 8 || memcmp(data, "CRPL", 4) != 0) {
        platform_log("這不是 Comet 的重播檔");
        return 1;
    }
    uint16_t version = get_u16(data + 4);
    uint16_t header = get_u16(data + 6);
    if (version > REPLAY_VERSION) {
        platform_log("重播檔的版本比引擎新，請更新引擎");
        return 1;
    }
    if (header < REPLAY_HEADER_SIZE || header > len) {
        platform_log("重播檔不完整或損壞");
        return 1;
    }
    if (reserve(len)) {
        platform_log("重播檔太大");
        return 1;
    }
    memcpy(buf, data, len);
    buf_len = len;

    total_frames = get_u32(buf + 12);
    engine_set_seed(get_u32(buf + 8));   // 接下來的 engine_load() 用錄影時的種子

    memset(pads, 0, sizeof(pads));
    last_event_frame = 0;
    read_pos = header;                   // 跳過檔頭（包括之後版本新增、這版看不懂的欄位）
    peek_next_event();
    matched = 0;
    state = REPLAY_PLAYING;
    return 0;
}

int replay_on_load(const uint8_t cart_sha[32])
{
    if (state == REPLAY_ARMED) {
        record_start(cart_sha);
        return 0;
    }
    if (state == REPLAY_RECORDING || state == REPLAY_FINISHED) {
        replay_stop();        // 換了遊戲（或重新開始），之前的錄影／播放結果作廢
        return 0;
    }
    if (state != REPLAY_PLAYING) return 0;
    if (memcmp(buf + 16, cart_sha, 32) != 0) {
        platform_log("重播檔和這個遊戲對不上（不同的遊戲，或遊戲改過了）");
        replay_stop();
        return 1;
    }
    if (total_frames == 0) state = REPLAY_FINISHED;
    return 0;
}

// 下一個事件的種類（peek_next_event 之後，read_pos 指在事件開頭那 1 byte）
static int next_type(void)
{
    return read_pos < buf_len ? buf[read_pos] >> EV_TYPE_SHIFT : -1;
}

// 套用一個事件。格式不對回傳 1
static int apply_event(void)
{
    if (read_pos >= buf_len) return 1;
    uint8_t head = buf[read_pos++];
    if ((head >> EV_TYPE_SHIFT) != EV_TYPE_INPUT) return 1;   // 這版還沒有其他種類

    size_t need = (head & EV_BUTTONS ? 2 : 0) + (head & EV_LSTICK ? 2 : 0) + (head & EV_RSTICK ? 2 : 0);
    if (read_pos + need > buf_len) return 1;

    PadState *p = &pads[head & EV_PLAYER_MASK];
    if (head & EV_BUTTONS) { p->buttons = get_u16(buf + read_pos); read_pos += 2; }
    if (head & EV_LSTICK)  { p->lx = (int8_t)buf[read_pos]; p->ly = (int8_t)buf[read_pos + 1]; read_pos += 2; }
    if (head & EV_RSTICK)  { p->rx = (int8_t)buf[read_pos]; p->ry = (int8_t)buf[read_pos + 1]; read_pos += 2; }
    return 0;
}

void replay_before_frame(uint32_t frame)
{
    if (state == REPLAY_RECORDING) {
        for (int p = 0; p < MAX_PLAYERS; p++) record_pad(p, frame);
        return;
    }
    if (state != REPLAY_PLAYING) return;

    // 上一幀還有沒用掉的事件：錄影時遊戲在那一幀讀了存檔，這次卻沒讀
    if (next_event_frame < frame) {
        platform_log("重播和遊戲對不上（讀存檔或時鐘的次數不同），停止播放");
        replay_stop();
        return;
    }

    // 這一幀的輸入事件全部套用（同一幀可能有好幾位玩家的事件，幀數差是 0）。
    // 遇到讀存檔、讀時鐘事件就停：那要等遊戲執行到 loadsave、clock 時才用
    while (next_event_frame == frame && next_type() == EV_TYPE_INPUT) {
        if (apply_event()) {
            platform_log("重播檔損壞，停止播放");
            replay_stop();
            return;
        }
        last_event_frame = frame;
        peek_next_event();
    }
    // 蓋掉平台交進來的輸入：每位玩家每一幀都設定，不只是有事件的那幾位
    for (int p = 0; p < MAX_PLAYERS; p++) {
        input_set(p, pads[p].buttons);
        input_set_stick(p, STICK_LEFT, pads[p].lx, pads[p].ly);
        input_set_stick(p, STICK_RIGHT, pads[p].rx, pads[p].ry);
    }
}

void replay_after_frame(uint32_t frames_done)
{
    if (state != REPLAY_PLAYING || frames_done < total_frames) return;
    uint8_t sha[32];
    screen_sha(sha);
    matched = memcmp(sha, buf + 80, 32) == 0;
    state = REPLAY_FINISHED;
}

// ============================================================
// 讀存檔
// ============================================================

void replay_record_save(uint32_t frame, int slot, const uint8_t *data, int len)
{
    if (state != REPLAY_RECORDING) return;
    // 最多 5（幀數差）+ 1 + 5（長度）+ 存檔
    if (reserve(11 + (size_t)(len > 0 ? len : 0))) {
        platform_log("重播檔太大，停止錄影");
        state = REPLAY_OFF;
        return;
    }
    put_varint(frame - last_event_frame);
    put_byte((uint8_t)((slot & EV_SLOT_MASK) | (EV_TYPE_SAVE << EV_TYPE_SHIFT)));
    put_varint((uint32_t)(len + 1));   // -1（沒有存檔）→ 0
    if (len > 0) {
        memcpy(buf + buf_len, data, (size_t)len);
        buf_len += (size_t)len;
    }
    last_event_frame = frame;
}

int replay_play_save(uint32_t frame, int slot, uint8_t *out, int cap)
{
    // 下一個事件必須是「這一幀、這個欄位」的讀存檔
    if (next_event_frame != frame || next_type() != EV_TYPE_SAVE ||
        (buf[read_pos] & EV_SLOT_MASK) != slot) {
        platform_log("重播和遊戲對不上（讀存檔的時間或欄位不同），停止播放");
        replay_stop();
        return -2;
    }
    read_pos++;
    uint32_t n1;
    if (get_varint(&n1) || n1 > (uint32_t)cap + 1 || read_pos + (n1 ? n1 - 1 : 0) > buf_len) {
        platform_log("重播檔損壞，停止播放");
        replay_stop();
        return -2;
    }
    int len = (int)n1 - 1;
    if (len > 0) {
        memcpy(out, buf + read_pos, (size_t)len);
        read_pos += (size_t)len;
    }
    last_event_frame = frame;
    peek_next_event();
    return len;
}

// ============================================================
// 讀時鐘
// ============================================================

// 時間用 8 bytes 固定長度（有號整數轉成無號再一個一個 byte 寫）：
// 秒數很大（2026 年約 17 億），varint 也省不了多少，而且只有時間變了才寫一次
void replay_record_clock(uint32_t frame, int64_t t)
{
    if (state != REPLAY_RECORDING) return;
    if (reserve(5 + 1 + 8)) {   // 幀數差 + 開頭 + 時間
        platform_log("重播檔太大，停止錄影");
        state = REPLAY_OFF;
        return;
    }
    put_varint(frame - last_event_frame);
    put_byte((uint8_t)(EV_TYPE_CLOCK << EV_TYPE_SHIFT));
    uint64_t u = (uint64_t)t;   // -1 變成 0xFFFF…FF，讀回來轉回 int64_t 又是 -1
    for (int i = 0; i < 8; i++) put_byte((uint8_t)(u >> (i * 8)));
    last_event_frame = frame;
}

int replay_play_clock(uint32_t frame, int64_t *t)
{
    // 下一個事件不是「這一幀的讀時鐘」：錄影時這次讀到的和上一次一樣
    if (next_event_frame != frame || next_type() != EV_TYPE_CLOCK) return 0;
    read_pos++;
    if (read_pos + 8 > buf_len) {
        platform_log("重播檔損壞，停止播放");
        replay_stop();
        return -2;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)buf[read_pos + i] << (i * 8);
    read_pos += 8;
    *t = (int64_t)u;
    last_event_frame = frame;
    peek_next_event();
    return 1;
}

// ============================================================
// 其他
// ============================================================

void replay_stop(void)
{
    state = REPLAY_OFF;
    release();
}

int replay_state(void) { return state; }
uint32_t replay_frames(void) { return state == REPLAY_PLAYING || state == REPLAY_FINISHED ? total_frames : 0; }
int replay_matched(void) { return state == REPLAY_FINISHED && matched; }
