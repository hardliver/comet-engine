// romflash.c — 主機的唯讀區：把遊戲安裝進 Flash（說明見 romflash.h）
//
// Flash 的規則（和 RAM 很不一樣）：
//   - 擦除（erase）：最少一個「磁區」4 KB；也可以一次擦 64 KB 的「區塊」（每 KB 快很多）。
//     擦完全部變成 0xFF（所有位元都是 1）
//   - 寫入（program）：一次最多一「頁」256 bytes，只能把位元從 1 變成 0
//   - 擦除、寫入的時候，CPU 不能讀 Flash（程式碼也在 Flash 上！），見下面的 flash_op
//
// 安裝時：要寫的地方先用 64 KB 區塊擦乾淨（寫到哪裡擦到哪裡），之後每一頁都只要「寫」。
// 已經安裝過：不建置；遊戲載入時的接在後面寫（freeze、圖片）和 Flash 上的比較，一樣就不寫。

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/structs/nvic.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "romflash.h"
#include "video.h"

#define REGION ((char *)(XIP_BASE + ROMFLASH_OFFSET))   // 整個唯讀區（安裝紀錄 + 遊戲資料）
#define HEADER_SIZE FLASH_SECTOR_SIZE                    // 安裝紀錄占第一個 4 KB 磁區
#define DATA (REGION + HEADER_SIZE)                      // 遊戲資料
#define DATA_SIZE (ROMFLASH_SIZE - HEADER_SIZE)
#define BLOCK_SIZE (64u * 1024u)                         // 大區塊擦除

// 安裝紀錄（放在唯讀區最前面）
#define HEADER_MAGIC "NOVAROM2"
typedef struct {
    char magic[8];
    RomId id;
    uint32_t root_off;    // 主函式在遊戲資料裡的位置
    uint32_t code_used;   // 程式用到哪裡（之後圖片等從這裡接著放）
} RomHeader;

static uint8_t page_buf[FLASH_PAGE_SIZE];
static size_t page_off = (size_t)-1;   // 緩衝區裡是哪一頁（遊戲資料內的位移；-1 = 沒有）
static uint8_t sector_buf[FLASH_SECTOR_SIZE];
static size_t used;                    // 遊戲資料用到哪裡
static size_t code_used;
static int installing;                 // 1 = 這次開機在安裝
static size_t erased_end;              // 安裝時：唯讀區從開頭擦到哪裡了（整個唯讀區的位移）
static unsigned pages_written, erases, flash_errors;
static void (*progress_fn)(void);

const char *romflash_base(void) { return DATA; }
size_t romflash_capacity(void) { return DATA_SIZE; }
size_t romflash_used(void) { return used; }
unsigned romflash_pages_written(void) { return pages_written; }
unsigned romflash_erases(void) { return erases; }
unsigned romflash_errors(void) { return flash_errors; }
void romflash_set_progress(void (*fn)(void)) { progress_fn = fn; }

// ---- 真正動到 Flash 的地方 ----
// 寫 Flash（擦除、寫入）時，Flash 暫時不能讀，而 CPU 平常的程式都在 Flash 上。所以：
//   1. 另一個核心（USB）要先停下來，在 RAM 裡等（multicore_lockout）
//   2. 這個核心的中斷要關掉，不然中斷函式在 Flash 上就會當機
//   3. 擦除、寫入的函式本身（SDK 的 flash_range_*）放在 RAM
//
// 第 2 點的例外：HDMI 畫面的兩個中斷。它們的程式放在 RAM（video.c 的 __scratch_x），
// 也只讀寫 RAM，所以 Flash 暫停時也能跑。保留它們，畫面就不會斷。
// （SDK 的 flash_safe_execute 會把中斷「全部」關掉：擦一個磁區約 45 ms，
//  畫面的 DMA 沒人換位址就一路讀出 RAM 範圍停住，HDMI 從此沒有訊號。2026-10-24 實機發現）

// RP2350 有 52 個中斷，NVIC 用兩個 32 位元的字記哪些開著
#define NVIC_WORDS 2

// region_off：整個唯讀區內的位移（0 = 安裝紀錄）。erase = 1 時 len 是擦除的大小
static void flash_op(int erase, size_t region_off, const uint8_t *data, size_t len)
{
    uint32_t flash_off = (uint32_t)(ROMFLASH_OFFSET + region_off);

    // 1. 請另一個核心停下來（最多等 1 秒；等不到就記成錯誤 FE，不寫）
    if (!multicore_lockout_start_timeout_us(1000 * 1000)) {
        flash_errors++;
        return;
    }

    // 2. 只留畫面的兩個中斷：先記下現在開著哪些，關掉其他的
    uint32_t keep[NVIC_WORDS] = {0};
    uint irqs[2] = { DMA_IRQ_0, video_convert_irq() };
    for (int i = 0; i < 2; ++i) keep[irqs[i] / 32] |= 1u << (irqs[i] % 32);
    uint32_t saved[NVIC_WORDS];
    for (int i = 0; i < NVIC_WORDS; ++i) {
        saved[i] = nvic_hw->iser[i];                // 讀 ISER = 目前開著的中斷
        nvic_hw->icer[i] = saved[i] & ~keep[i];     // 寫 ICER 的 bit = 關掉那個中斷
    }
    __dsb();   // 確定關掉了才往下（處理器可能還沒做完前面的寫入）
    __isb();

    // 3. 擦除或寫入（這兩個函式在 RAM）
    if (erase) flash_range_erase(flash_off, len);
    else flash_range_program(flash_off, data, len);

    // 4. 恢復
    for (int i = 0; i < NVIC_WORDS; ++i) nvic_hw->iser[i] = saved[i];   // 寫 ISER 的 bit = 打開
    multicore_lockout_end_timeout_us(1000 * 1000);

    if (erase) erases++;
    else pages_written++;
    if (progress_fn) progress_fn();
}

// 安裝時：要寫到 region_off 之前，確定那裡已經擦乾淨了（不夠就再擦一個 64 KB 區塊）
static void ensure_erased(size_t region_end)
{
    while (installing && erased_end < region_end && erased_end < ROMFLASH_SIZE) {
        flash_op(1, erased_end, NULL, BLOCK_SIZE);
        erased_end += BLOCK_SIZE;
    }
}

// 把一頁（遊戲資料的位移 off）放進 Flash：一樣就不寫；能直接寫就寫；不行就擦磁區重寫
static void put_page(size_t off, const uint8_t *data)
{
    size_t region_off = HEADER_SIZE + off;
    ensure_erased(region_off + FLASH_PAGE_SIZE);

    const uint8_t *cur = (const uint8_t *)DATA + off;   // Flash 上現在的內容（XIP 直接讀）
    if (memcmp(cur, data, FLASH_PAGE_SIZE) == 0) return;   // 一樣：不用寫

    // 只需要把位元從 1 變成 0 嗎？（cur & data == data）。安裝時擦乾淨了，一定是
    int compatible = 1;
    for (unsigned i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if ((cur[i] & data[i]) != data[i]) { compatible = 0; break; }
    }
    if (compatible) {
        // 寫入只能把 1 變 0：已經是 0 的位元不受影響，所以直接寫整頁就好
        // （寫入期間 CPU 不能讀 Flash，所以資料先複製到 RAM 的 sector_buf）
        memcpy(sector_buf, data, FLASH_PAGE_SIZE);
        flash_op(0, region_off, sector_buf, FLASH_PAGE_SIZE);
        return;
    }

    // 很少發生（已安裝的遊戲，載入時寫的東西和上次不一樣）：
    // 整個磁區讀出來，這一頁之前的保留，這一頁放新的，之後的丟掉，擦掉再寫回
    size_t sec = region_off / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
    size_t in = region_off - sec;
    memcpy(sector_buf, REGION + sec, in);
    memcpy(sector_buf + in, data, FLASH_PAGE_SIZE);
    memset(sector_buf + in + FLASH_PAGE_SIZE, 0xFF, FLASH_SECTOR_SIZE - in - FLASH_PAGE_SIZE);
    flash_op(1, sec, NULL, FLASH_SECTOR_SIZE);
    for (size_t p = 0; p < in + FLASH_PAGE_SIZE; p += FLASH_PAGE_SIZE)
        flash_op(0, sec + p, sector_buf + p, FLASH_PAGE_SIZE);
}

// 把緩衝區沖進 Flash
static void flush_page(void)
{
    if (page_off == (size_t)-1) return;
    put_page(page_off, page_buf);
    page_off = (size_t)-1;
}

// 寫 len bytes 到遊戲資料的位移 off（只能往後寫，和 luarom 的要求一樣）
static int write_at(size_t off, const void *data, size_t len)
{
    const uint8_t *src = data;
    if (off + len > DATA_SIZE) return 1;
    while (len > 0) {
        size_t pg = off / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
        if (pg != page_off) {
            flush_page();
            // 新的一頁：先放 Flash 上現在的內容，沒寫到的部分就維持原樣（比較時才會一樣）。
            // 安裝時這一頁可能還沒擦：沒寫到的部分當成 0xFF（擦完的樣子）
            if (installing && HEADER_SIZE + pg >= erased_end) memset(page_buf, 0xFF, FLASH_PAGE_SIZE);
            else memcpy(page_buf, DATA + pg, FLASH_PAGE_SIZE);
            page_off = pg;
        }
        size_t in = off - pg;
        size_t n = FLASH_PAGE_SIZE - in < len ? FLASH_PAGE_SIZE - in : len;
        memcpy(page_buf + in, src, n);
        off += n; src += n; len -= n;
    }
    return 0;
}

// ---- 安裝紀錄 ----

static const RomHeader *header(void) { return (const RomHeader *)REGION; }

int romflash_installed(const RomId *id, const void **root)
{
    const RomHeader *h = header();
    installing = 0;
    page_off = (size_t)-1;
    pages_written = erases = flash_errors = 0;
    if (memcmp(h->magic, HEADER_MAGIC, 8) != 0) return 0;
    if (memcmp(&h->id, id, sizeof *id) != 0) return 0;
    if (h->code_used > DATA_SIZE || h->root_off >= h->code_used) return 0;
    *root = DATA + h->root_off;
    code_used = used = h->code_used;
    return 1;
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
            d[i] = (uint8_t)DATA[o];
    }
}

void romflash_begin_install(RomWriter *w)
{
    installing = 1;
    erased_end = 0;
    page_off = (size_t)-1;
    used = code_used = 0;
    pages_written = erases = flash_errors = 0;
    ensure_erased(HEADER_SIZE);   // 第一個區塊：安裝紀錄也跟著清掉（裝到一半斷電 = 沒裝）

    w->base = DATA;
    w->capacity = DATA_SIZE;
    w->used = 0;
    w->write = w_write;
    w->read = w_read;
    w->ud = NULL;
}

void romflash_build_done(size_t u)
{
    flush_page();
    code_used = used = u;
}

void romflash_finish_install(const RomId *id, const void *root)
{
    if (!installing) return;
    flush_page();
    // 安裝紀錄所在的磁區在開始安裝時就擦過了（是 0xFF），直接寫一頁
    static RomHeader h __attribute__((aligned(4)));
    memset(sector_buf, 0xFF, FLASH_PAGE_SIZE);
    memcpy(h.magic, HEADER_MAGIC, 8);
    h.id = *id;
    h.root_off = (uint32_t)((const char *)root - DATA);
    h.code_used = (uint32_t)code_used;
    memcpy(sector_buf, &h, sizeof h);
    flash_op(0, 0, sector_buf, FLASH_PAGE_SIZE);
}

// ---- 接在後面寫：圖片、字型、地圖、凍結的表格 ----
// 寫完馬上沖進 Flash：呼叫的人拿到位址就會開始讀

void *romflash_append(const void *data, size_t len, size_t align)
{
    size_t at = (used + align - 1) / align * align;
    if (at + len > DATA_SIZE) return NULL;
    // 對齊補的空位寫 0，和上次開機寫的一模一樣，才會被判斷成「不用寫」
    static const uint8_t zeros[16];
    if (at > used && write_at(used, zeros, at - used)) return NULL;
    if (write_at(at, data, len)) return NULL;
    flush_page();
    used = at + len;
    return DATA + at;
}

const uint8_t *romflash_store(const uint8_t *data, size_t len)
{
    return romflash_append(data, len, 4);
}
