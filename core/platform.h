// platform.h — 核心需要「平台」提供的功能
// 核心只宣告，每個平台各自實作：
//   Mac：platform/mac/main.c
//   網頁：platform/web/web.c

#ifndef PLATFORM_H
#define PLATFORM_H

// 印出一行訊息（Lua 的 print、錯誤訊息）。
// Mac 印在終端機，網頁印在瀏覽器的主控台。
void platform_log(const char *msg);

#endif
