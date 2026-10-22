// web.c — 網頁平台層的 C 部分
// 大部分網頁平台層是 JavaScript（main.js），
// 但核心要求的 platform_log、存檔、時鐘是 C 函式，所以在這裡轉交給 JavaScript。

#include "../../core/platform.h"

// 告訴編譯器：js_log 不在 C 裡，由 JavaScript 在載入 wasm 時提供
// （main.js 裡 imports.env.js_log）
__attribute__((import_module("env"), import_name("js_log")))
void js_log(const char *msg);

void platform_log(const char *msg)
{
    js_log(msg);
}

// 存檔：交給 JavaScript 存在瀏覽器裡（comet.js 的 js_save_read／js_save_write）
__attribute__((import_module("env"), import_name("js_save_read")))
int js_save_read(const char *game_id, int slot, unsigned char *buf, int cap);
__attribute__((import_module("env"), import_name("js_save_write")))
int js_save_write(const char *game_id, int slot, const unsigned char *data, int len);

int platform_save_read(const char *game_id, int slot, unsigned char *buf, int cap)
{
    return js_save_read(game_id, slot, buf, cap);
}

int platform_save_write(const char *game_id, int slot, const unsigned char *data, int len)
{
    return js_save_write(game_id, slot, data, len);
}

// 現實時鐘：JavaScript 回傳本地時間的秒數（comet.js 的 js_clock）。
// 用 double 而不是 int64_t：wasm 和 JavaScript 之間傳 64 位元整數要用 BigInt，比較麻煩；
// double 可以精確表示到 2^53，秒數用不完
__attribute__((import_module("env"), import_name("js_clock")))
double js_clock(void);

int64_t platform_clock(void)
{
    double t = js_clock();
    if (t < 0) return -1;
    return (int64_t)t;
}
