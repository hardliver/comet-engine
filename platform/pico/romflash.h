// romflash.h — 主機的唯讀區：Flash 上保留的一塊（放遊戲的程式、圖片、凍結的表格，見 core/luarom.h）
//
// Flash 配置（Pico 2 W 共 4 MB）：
//   0       – 1 MB   韌體（目前約 350 KB；之後的 WiFi 韌體也放這裡）
//   1 MB    – 4 MB   唯讀區（3 MB）
//
// 寫入方式：「先比較，不同才寫」。每次開機重新建一次唯讀區，但內容和 Flash 上已經有的一樣
// （同一款遊戲）就完全不寫，所以第二次開同一款遊戲很快、也不會磨損 Flash。
// 換遊戲時只重寫不一樣的部分。

#ifndef ROMFLASH_H
#define ROMFLASH_H

#include <stddef.h>
#include "luarom.h"

#define ROMFLASH_OFFSET (1024u * 1024u)        // 從 Flash 的第 1 MB 開始
#define ROMFLASH_SIZE   (3u * 1024u * 1024u)   // 3 MB

// 唯讀區在 CPU 看到的位址（XIP：Flash 直接映射到記憶體位址，可以像讀 RAM 一樣讀）
const char *romflash_base(void);

// 給 luarom_build 用的寫入器（從唯讀區開頭寫起）
void romflash_writer(RomWriter *w);

// 建完之後呼叫：把還在緩衝區的最後一頁寫進 Flash。from_used：建好的程式用到哪裡（之後接著放圖片等）
void romflash_finish_build(size_t used);

// 接在後面寫（圖片、字型、地圖、凍結的表格；gfx_set_rom_store、luarom_set_append 用）
void *romflash_append(const void *data, size_t len, size_t align);
const uint8_t *romflash_store(const uint8_t *data, size_t len);

// 統計：這次開機實際寫了幾頁、擦了幾個磁區（0 = 全部和上次一樣）
unsigned romflash_pages_written(void);
unsigned romflash_sectors_erased(void);
size_t romflash_used(void);

#endif
