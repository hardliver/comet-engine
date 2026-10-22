// platform.h — 核心需要「平台」提供的功能
// 核心只宣告，每個平台各自實作：
//   Mac：platform/mac/main.c
//   網頁：platform/web/web.c

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

// 印出一行訊息（Lua 的 print、錯誤訊息）。
// Mac 印在終端機，網頁印在瀏覽器的主控台。
void platform_log(const char *msg);

// ---- 存檔（格式由核心決定，平台只負責保管 bytes）----
// game_id：遊戲的 id，例如 "hardliver/wildkin"；slot：0–7。
// 不同遊戲的存檔要分開放；同一個遊戲的同一個欄位只有一份。

// 讀：把存檔複製到 buf（最多 cap bytes），回傳長度。沒有存檔回傳 -1
int platform_save_read(const char *game_id, int slot, unsigned char *buf, int cap);

// 寫：data 是 NULL 代表刪除這個欄位。成功回傳 0，失敗（空間不足等）回傳 1
int platform_save_write(const char *game_id, int slot, const unsigned char *data, int len);

// ---- 現實時鐘（見 clock.h）----
// 本地時間：從 1970-01-01 00:00:00 起算的秒數（已經加上時區，所以「下午 2 點」除以 3600 的餘數就是 14）。
// 沒有時間來源回傳 -1。網頁：瀏覽器的時間；Mac：--clock 指定；主機：RTC 或 WiFi 對時
int64_t platform_clock(void);

#endif
