// web.c — 網頁平台層的 C 部分
// 大部分網頁平台層是 JavaScript（main.js），
// 但核心要求的 platform_log 是 C 函式，所以在這裡轉交給 JavaScript。

#include "../../core/platform.h"

// 告訴編譯器：js_log 不在 C 裡，由 JavaScript 在載入 wasm 時提供
// （main.js 裡 imports.env.js_log）
__attribute__((import_module("env"), import_name("js_log")))
void js_log(const char *msg);

void platform_log(const char *msg)
{
    js_log(msg);
}
