// clock.c — 現實時鐘的實作（用法見 clock.h）

#include "lua.h"
#include "lauxlib.h"

#include "clock.h"
#include "engine.h"
#include "platform.h"
#include "replay.h"

#define SECONDS_PER_DAY 86400

// 這一款遊戲上一次讀到的時間。用途有兩個：
//   1. 同一幀再讀一次，直接給同一個值
//   2. 錄影時和上一次一樣就不記（播放時重播檔裡沒有事件，就代表「和上一次一樣」）
static int     have_value = 0;   // 這款遊戲讀過時間了沒
static int64_t value;            // 讀到的秒數（-1 = 沒有時間來源）
static uint32_t value_frame;     // 在第幾幀讀的

void clock_on_load(void)
{
    have_value = 0;
}

// ============================================================
// 日期換算（Howard Hinnant 的公式，純整數運算）
// ============================================================
// 訣竅：把 3 月當成一年的第一個月。這樣 2 月（可能有 29 日）就是一年的最後一個月，
// 「這個月之前有幾天」可以用一條式子 (153 × 月 + 2) / 5 算出來（3 月起每個月 31、30、31、30、31…
// 有規律），閏年多出來的那一天剛好在年底，不會影響前面的月份。
// 400 年是一個完整的循環（剛好 146097 天），先算是第幾個 400 年（era），再算裡面的第幾年。
//
// 注意 C 的整數除法是「往 0 取整」：-1 / 400 = 0，不是 -1。
// 1970 年以前（負數）要往下取整，所以負數時先減掉 399（或 146096）再除。

// 年月日 → 從 1970-01-01 起算第幾天
static int64_t days_from_civil(int64_t y, int m, int d)
{
    if (m <= 2) y -= 1;                                // 1、2 月算前一年的最後兩個月
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                       // 400 年裡的第幾年（0–399）
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;   // 從 3 月 1 日算第幾天
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // 400 年裡的第幾天
    return era * 146097 + doe - 719468;                // 719468 = 0000-03-01 到 1970-01-01 的天數
}

// 從 1970-01-01 起算第幾天 → 年月日（上面那個函式反過來算）
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                           // 0–146096
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;      // 0–399
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                    // 從 3 月 1 日算第幾天
    int64_t mp = (5 * doy + 2) / 153;                                         // 3 月 = 0
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400;
    if (*m <= 2) *y += 1;                                                     // 1、2 月其實是下一年的
}

int64_t clock_from_date(int year, int month, int day, int hour, int min, int sec)
{
    return days_from_civil(year, month, day) * SECONDS_PER_DAY + hour * 3600 + min * 60 + sec;
}

// 往下取整的除法（負數也對）：floor_div(-1, 86400) = -1
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// ============================================================
// 讀時間：經過重播，再問平台
// ============================================================

static int64_t read_now(void)
{
    uint32_t frame = engine_frame_count();
    if (have_value && value_frame == frame) return value;   // 同一幀：同一個值

    int64_t t = 0;
    int got = 0;
    if (replay_state() == REPLAY_PLAYING) {
        int r = replay_play_clock(frame, &t);
        if (r == 1) {
            got = 1;                       // 重播檔裡有這一次讀到的時間
        } else if (r == 0 && have_value) {
            t = value;                     // 沒有事件 = 錄影時讀到的和上一次一樣
            got = 1;
        } else if (r == 0) {
            // 第一次讀卻沒有事件：錄影時沒在這裡讀時間，遊戲跑得和錄影時不一樣
            platform_log("重播和遊戲對不上（讀時鐘的時間不同），停止播放");
            replay_stop();
        }
        // r == -2：重播檔損壞，replay.c 已經停止播放。下面改問平台，玩家可以接著玩
    }
    if (!got) {
        t = platform_clock();
        if (t < 0) t = -1;
        if (!have_value || t != value) replay_record_clock(frame, t);   // 變了才記
    }
    have_value = 1;
    value = t;
    value_frame = frame;
    return t;
}

// ============================================================
// Lua API
// ============================================================

static void set_field(lua_State *L, const char *name, int64_t v)
{
    lua_pushinteger(L, (lua_Integer)v);
    lua_setfield(L, -2, name);
}

// clock()：{ year, month, day, hour, min, sec, wday, time }；沒有時間來源回傳 nil
static int l_clock(lua_State *L)
{
    int64_t t = read_now();
    if (t < 0) {
        lua_pushnil(L);
        return 1;
    }
    int64_t days = floor_div(t, SECONDS_PER_DAY);
    int64_t secs = t - days * SECONDS_PER_DAY;   // 今天過了幾秒（0–86399）
    int64_t year;
    int month, day;
    civil_from_days(days, &year, &month, &day);

    lua_createtable(L, 0, 8);
    set_field(L, "year", year);
    set_field(L, "month", month);
    set_field(L, "day", day);
    set_field(L, "hour", secs / 3600);
    set_field(L, "min", secs / 60 % 60);
    set_field(L, "sec", secs % 60);
    // 1970-01-01 是星期四：第 0 天 → 4。負數的天數也要落在 0–6，所以用往下取整的餘數
    set_field(L, "wday", days + 4 - floor_div(days + 4, 7) * 7);
    set_field(L, "time", t);
    return 1;
}

void clock_open_lua(lua_State *L)
{
    lua_register(L, "clock", l_clock);
}
