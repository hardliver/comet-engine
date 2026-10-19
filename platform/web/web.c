// web.c — 網頁平台層的 C 部分
// 大部分網頁平台層是 JavaScript（main.js），
// 但核心要求的 platform_log、存檔是 C 函式，所以在這裡轉交給 JavaScript。

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
