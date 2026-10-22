// clock.h — 現實時鐘：遊戲讀「現在幾年幾月幾日幾點」
//
// 遊戲這邊（Lua）：
//   clock()   回傳一個表格 { year, month, day, hour, min, sec, wday, time }；
//             這台機器沒有時間來源（例如主機沒裝 RTC、也沒連網對時）回傳 nil
//     wday：星期幾（0 = 星期日 … 6 = 星期六）
//     time：從 1970-01-01 00:00:00 起算的秒數（本地時間），用來算「過了多久」
//
// 分工：
//   平台：只回傳一個數字「本地時間的秒數」（platform.h 的 platform_clock），
//         沒有時間來源回傳 -1。時區由平台處理（網頁用瀏覽器的時區）
//   核心（這裡）：把秒數換算成年月日時分秒（純整數運算，各平台結果一樣）；和重播檔配合
//
// 和重播的關係（決定性）：
//   現實時間每次開遊戲都不一樣，所以它和按鍵、讀存檔一樣是「輸入」：
//   錄影時：讀到的時間記進重播檔（和上一次讀到的一樣就不記，省空間）
//   播放時：改從重播檔讀，不問平台——幾年後重播，遊戲看到的還是當時的時間
//   同一幀裡讀好幾次，拿到的都是同一個值（不會一幀裡秒數跳了一下）

#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

struct lua_State;

// engine_load() 時呼叫：忘掉上一款遊戲讀到的時間
void clock_on_load(void);

// 年月日時分秒 → 從 1970-01-01 00:00:00 起算的秒數（給平台把「2026-10-22 14:05」換成秒數用）
int64_t clock_from_date(int year, int month, int day, int hour, int min, int sec);

// 把 clock 註冊成 Lua 的全域函式（script.c 建立 Lua 環境時呼叫）
void clock_open_lua(struct lua_State *L);

#endif
