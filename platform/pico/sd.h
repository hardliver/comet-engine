// sd.h — Pico 的 microSD 卡（SPI0：GP2 SCK、GP3 MOSI、GP4 MISO、GP5 CS）
//
// 卡匣放在 SD 卡根目錄，檔名 game.lua（之後由 Nova 主選單選遊戲）。

#ifndef SD_H
#define SD_H

#include <stddef.h>
#include <stdint.h>

// 掛上 SD 卡。成功回傳 0；沒有卡、格式不對回傳非 0
int sd_mount(void);

// 把整個檔案讀進 malloc 來的記憶體。成功回傳 0，*data 用完要 free
// 失敗回傳非 0（找不到檔案、記憶體不夠…），*data 為 NULL
int sd_read_file(const char *path, char **data, size_t *len);

// 一段一段讀一個檔案（大檔案放不進 RAM 時用）。同一時間只能開一個
int sd_stream_open(const char *path);   // 成功回傳 0
int sd_stream_read(void *ud, void *dst, size_t n);   // 讀 n bytes，成功回傳 0（簽名配合 luarom 的 RomReader）
void sd_stream_close(void);
size_t sd_stream_pos(void);    // 已經讀了幾 bytes（安裝進度用）
size_t sd_stream_size(void);   // 檔案大小

// 檔案的身分（判斷 Flash 上裝的是不是這個檔）：大小、修改時間、開頭 64 KB 的 CRC32。成功回傳 0
int sd_fingerprint(const char *path, uint32_t *size, uint32_t *stamp, uint32_t *crc);

// 最後一次錯誤的說明（給畫面顯示）
const char *sd_error(void);

// 診斷：不經過函式庫，直接對卡片送 CMD0（重置），把卡片回的原始 bytes 寫成文字。
// 回 01 = 卡片有回應；全部 FF = 卡片沒說話；全部 00 = MISO 被拉到地。
// 要在 sd_mount 之前呼叫（之後腳位交給函式庫）
void sd_probe(char *out, int cap);

#endif
