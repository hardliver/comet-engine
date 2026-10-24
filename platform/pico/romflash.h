// romflash.h — 主機的唯讀區：Flash 上保留的一塊，放「安裝好的遊戲」
//               （遊戲的程式、圖片、凍結的表格，見 core/luarom.h）
//
// Flash 配置（Pico 2 W 共 4 MB）：
//   0       – 1 MB          韌體（目前約 350 KB；之後的 WiFi 韌體也放這裡）
//   1 MB    – 1 MB + 4 KB   安裝紀錄（RomHeader：裝的是哪個遊戲檔、主函式在哪裡）
//   1 MB + 4 KB – 4 MB      遊戲資料
//
// 概念：遊戲檔（.nvc，在 SD 卡；之後也可以從 GitHub 下載）要先「安裝」進 Flash 才能執行，
// 就像 Game Boy 的卡匣是一顆 ROM，CPU 直接從裡面讀。
//   - 開機時比對安裝紀錄：同一個遊戲檔、同一版韌體 → 已經裝好，不用再寫，馬上開始
//   - 不一樣 → 重新安裝：用 64 KB 的大區塊先擦乾淨，再從頭一路往後寫（Flash 最快的寫法）
//   - 安裝紀錄最後才寫：安裝到一半斷電，下次開機會發現沒有紀錄，重新安裝
// 安裝的來源只是一個「依序讀 bytes 的函式」（RomReader），所以 SD 卡、網路下載都能用同一套。

#ifndef ROMFLASH_H
#define ROMFLASH_H

#include <stddef.h>
#include <stdint.h>
#include "luarom.h"

#define ROMFLASH_OFFSET (1024u * 1024u)        // 從 Flash 的第 1 MB 開始
#define ROMFLASH_SIZE   (3u * 1024u * 1024u)   // 3 MB（含安裝紀錄）

// 遊戲檔的身分：用來判斷「Flash 上裝的是不是這個檔」
typedef struct {
    uint32_t size;       // 檔案大小
    uint32_t stamp;      // 檔案修改時間（FatFs 的日期、時間）
    uint32_t crc;        // 檔案開頭 64 KB 的 CRC32
    char firmware[32];   // 韌體版本（編譯時間）：換韌體後記憶體位址可能不同，要重新安裝
} RomId;

// 遊戲資料在 CPU 看到的位址（XIP：Flash 直接映射到記憶體位址，可以像讀 RAM 一樣讀）與大小
const char *romflash_base(void);
size_t romflash_capacity(void);

// 開機時呼叫：Flash 上已經裝好這個遊戲檔了嗎？
// 是：回傳 1，*root 是主函式的位址；之後的接在後面寫會從安裝時的位置開始比對（一樣就不寫）
// 否：回傳 0，要呼叫 romflash_begin_install 重新安裝
int romflash_installed(const RomId *id, const void **root);

// 開始安裝：清掉安裝紀錄，*w 是給 luarom_build 用的寫入器
void romflash_begin_install(RomWriter *w);

// luarom_build 完成之後呼叫：記下程式用到哪裡（之後圖片等接著放）
void romflash_build_done(size_t used);

// 遊戲載入完成（最上層程式跑完、freeze 都做完）之後呼叫：寫入安裝紀錄
void romflash_finish_install(const RomId *id, const void *root);

// 接在後面寫（圖片、字型、地圖、凍結的表格；gfx_set_rom_store、luarom_set_append 用）。
// 已經安裝過的遊戲，內容和 Flash 上一樣 → 不寫，直接回傳位址
void *romflash_append(const void *data, size_t len, size_t align);
const uint8_t *romflash_store(const uint8_t *data, size_t len);

// 每次真的寫入或擦除之後呼叫（主程式用來更新「安裝中」畫面）
void romflash_set_progress(void (*fn)(void));

// 統計：這次開機實際寫了幾頁、擦了幾次、失敗幾次（另一個核心沒回應）
unsigned romflash_pages_written(void);
unsigned romflash_erases(void);
unsigned romflash_errors(void);
size_t romflash_used(void);

#endif
