// sha256.h — SHA-256：把任意長度的資料變成 32 bytes 的「指紋」
// 資料只要改一個 byte，指紋就完全不同，所以拿來辨認「是不是同一個檔案」。
// Comet 用在：
//   - 辨認遊戲版本（重播檔記錄遊戲的指紋，播放時比對）
//   - 比對畫面（重播結束時的畫面指紋，確認各平台結果相同）
//   - 之後：商城下載的檔案有沒有損壞
//
// 用法：
//   sha256(資料, 長度, out);              一次算完
//   或 sha256_init → sha256_update（可以分好幾次）→ sha256_final

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];        // 目前的計算結果
    uint8_t  block[64];   // 還沒湊滿 64 bytes 的資料
    size_t   used;        // block 裡有幾 bytes
    uint64_t total;       // 總共餵了幾 bytes
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final(Sha256 *s, uint8_t out[32]);

void sha256(const void *data, size_t len, uint8_t out[32]);

#endif
