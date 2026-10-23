// sd.h — Pico 的 microSD 卡（SPI0：GP2 SCK、GP3 MOSI、GP4 MISO、GP5 CS）
//
// 卡匣放在 SD 卡根目錄，檔名 game.lua（之後由 Nova 主選單選遊戲）。

#ifndef SD_H
#define SD_H

#include <stddef.h>

// 掛上 SD 卡。成功回傳 0；沒有卡、格式不對回傳非 0
int sd_mount(void);

// 把整個檔案讀進 malloc 來的記憶體。成功回傳 0，*data 用完要 free
// 失敗回傳非 0（找不到檔案、記憶體不夠…），*data 為 NULL
int sd_read_file(const char *path, char **data, size_t *len);

// 最後一次錯誤的說明（給畫面顯示）
const char *sd_error(void);

// 診斷：不經過函式庫，直接對卡片送 CMD0（重置），把卡片回的原始 bytes 寫成文字。
// 回 01 = 卡片有回應；全部 FF = 卡片沒說話；全部 00 = MISO 被拉到地。
// 要在 sd_mount 之前呼叫（之後腳位交給函式庫）
void sd_probe(char *out, int cap);

#endif
