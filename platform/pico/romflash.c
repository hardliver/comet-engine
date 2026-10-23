// romflash.c — 主機的唯讀區：寫進 Flash（說明見 romflash.h）
//
// Flash 的規則（和 RAM 很不一樣）：
//   - 擦除（erase）：一次最少一個「磁區」4 KB，擦完全部變成 0xFF（所有位元都是 1）
//   - 寫入（program）：一次一「頁」256 bytes，只能把位元從 1 變成 0，不能從 0 變回 1
//   - 擦除、寫入的時候，CPU 不能讀 Flash（程式碼也在 Flash 上！）：
//     所以要關中斷、讓另一個核心暫停，寫入的程式本身放在 RAM（SDK 的 flash_safe_execute 處理這些）
//
// 這個檔案的做法：
//   1. 寫入先放在一頁（256 bytes）的緩衝區，累積滿一頁或換頁時才「沖」進 Flash
//   2. 沖的時候先和 Flash 上的比較：一樣就不寫（同一款遊戲第二次開，全部都一樣）
//   3. 不一樣：如果只需要把 1 變成 0，直接寫；否則把整個磁區讀出來、擦掉、再寫回需要的部分

#include <string.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "romflash.h"

#define BASE ((char *)(XIP_BASE + ROMFLASH_OFFSET))

static uint8_t page_buf[FLASH_PAGE_SIZE];
static size_t page_off = (size_t)-1;   // 緩衝區裡是哪一頁（唯讀區內的位移；-1 = 沒有）
static uint8_t sector_buf[FLASH_SECTOR_SIZE];
static size_t used;                    // 唯讀區用到哪裡
static unsigned pages_written, sectors_erased;

const char *romflash_base(void) { return BASE; }
size_t romflash_used(void) { return used; }
unsigned romflash_pages_written(void) { return pages_written; }
unsigned romflash_sectors_erased(void) { return sectors_erased; }

// ---- 真正動到 Flash 的地方：透過 flash_safe_execute ----
// flash_safe_execute 會：關掉這個核心的中斷、讓另一個核心（USB）在 RAM 裡等待，
// 然後才呼叫我們的函式。所以函式本身做的事要越短越好（畫面在這段時間也會停住）

typedef struct {
    int erase;            // 1 = 擦除磁區；0 = 寫入
    uint32_t flash_off;   // 從 Flash 開頭算的位移
    const uint8_t *data;
    size_t len;
} FlashOp;

static void do_flash_op(void *p)
{
    FlashOp *op = p;
    if (op->erase) flash_range_erase(op->flash_off, op->len);
    else flash_range_program(op->flash_off, op->data, op->len);
}

static void flash_op(int erase, size_t rom_off, const uint8_t *data, size_t len)
{
    FlashOp op = { erase, (uint32_t)(ROMFLASH_OFFSET + rom_off), data, len };
    flash_safe_execute(do_flash_op, &op, UINT32_MAX);
    if (erase) sectors_erased++;
    else pages_written += (unsigned)(len / FLASH_PAGE_SIZE);
}

// 把一頁（page_buf，位移 off）放進 Flash：一樣就不寫；能直接寫就寫；不行就擦磁區重寫
static void put_page(size_t off, const uint8_t *data)
{
    const uint8_t *cur = (const uint8_t *)BASE + off;   // Flash 上現在的內容（XIP 直接讀）
    if (memcmp(cur, data, FLASH_PAGE_SIZE) == 0) return;   // 一樣：不用寫

    // 只需要把位元從 1 變成 0 嗎？（cur & data == data）
    int compatible = 1;
    for (unsigned i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if ((cur[i] & data[i]) != data[i]) { compatible = 0; break; }
    }
    if (compatible) {
        // 寫入只能把 1 變 0：已經是 0 的位元不受影響，所以直接寫整頁就好
        // （data 在 RAM 的 page_buf 裡，寫入期間 CPU 不能讀 Flash，資料不能放在 Flash）
        memcpy(sector_buf, data, FLASH_PAGE_SIZE);
        flash_op(0, off, sector_buf, FLASH_PAGE_SIZE);
        return;
    }

    // 要擦除：整個磁區讀出來，這一頁之前的保留（這次已經寫好的），這一頁放新的，
    // 之後的丟掉（那是上一款遊戲留下的，這次還會再寫）
    size_t sec = off / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
    size_t in = off - sec;
    memcpy(sector_buf, BASE + sec, in);
    memcpy(sector_buf + in, data, FLASH_PAGE_SIZE);
    memset(sector_buf + in + FLASH_PAGE_SIZE, 0xFF, FLASH_SECTOR_SIZE - in - FLASH_PAGE_SIZE);
    flash_op(1, sec, NULL, FLASH_SECTOR_SIZE);
    flash_op(0, sec, sector_buf, in + FLASH_PAGE_SIZE);
}

// 把緩衝區沖進 Flash
static void flush_page(void)
{
    if (page_off == (size_t)-1) return;
    put_page(page_off, page_buf);
    page_off = (size_t)-1;
}

// 寫 len bytes 到唯讀區位移 off（只能往後寫，和 luarom 的要求一樣）
static int write_at(size_t off, const void *data, size_t len)
{
    const uint8_t *src = data;
    if (off + len > ROMFLASH_SIZE) return 1;
    while (len > 0) {
        size_t pg = off / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
        if (pg != page_off) {
            flush_page();
            // 新的一頁：先放 Flash 上現在的內容，沒寫到的部分就維持原樣（比較時才會一樣）
            memcpy(page_buf, BASE + pg, FLASH_PAGE_SIZE);
            page_off = pg;
        }
        size_t in = off - pg;
        size_t n = FLASH_PAGE_SIZE - in < len ? FLASH_PAGE_SIZE - in : len;
        memcpy(page_buf + in, src, n);
        off += n; src += n; len -= n;
    }
    return 0;
}

// ---- luarom_build 用 ----

static int w_write(void *ud, size_t off, const void *data, size_t len)
{
    (void)ud;
    return write_at(off, data, len);
}

// 讀回剛寫的：還在緩衝區的那一頁從緩衝區讀，其他的直接讀 Flash
static void w_read(void *ud, size_t off, void *dst, size_t len)
{
    (void)ud;
    uint8_t *d = dst;
    for (size_t i = 0; i < len; ++i) {
        size_t o = off + i;
        if (page_off != (size_t)-1 && o >= page_off && o < page_off + FLASH_PAGE_SIZE)
            d[i] = page_buf[o - page_off];
        else
            d[i] = (uint8_t)BASE[o];
    }
}

void romflash_writer(RomWriter *w)
{
    w->base = BASE;
    w->capacity = ROMFLASH_SIZE;
    w->used = 0;
    w->write = w_write;
    w->read = w_read;
    w->ud = NULL;
    page_off = (size_t)-1;
    pages_written = sectors_erased = 0;
}

void romflash_finish_build(size_t u)
{
    flush_page();
    used = u;
}

// ---- 接在後面寫：圖片、字型、地圖、凍結的表格 ----
// 寫完馬上沖進 Flash：呼叫的人拿到位址就會開始讀

void *romflash_append(const void *data, size_t len, size_t align)
{
    size_t at = (used + align - 1) / align * align;
    if (at + len > ROMFLASH_SIZE) return NULL;
    // 對齊補的空位寫 0，和上次開機寫的一模一樣，才會被判斷成「不用寫」
    static const uint8_t zeros[16];
    if (at > used && write_at(used, zeros, at - used)) return NULL;
    if (write_at(at, data, len)) return NULL;
    flush_page();
    used = at + len;
    return BASE + at;
}

const uint8_t *romflash_store(const uint8_t *data, size_t len)
{
    return romflash_append(data, len, 4);
}
