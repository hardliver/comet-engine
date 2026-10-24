// sd.c — Pico 的 microSD 卡
//
// 用 carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico（FatFs + SD 卡 SPI 驅動）。
// 這個函式庫要求我們提供 hw_config：告訴它卡接在哪些腳位（下面的 sd_get_num、sd_get_by_num）。
//
// 接線（01 規格 D）：GP2 SCK、GP3 MOSI、GP4 MISO、GP5 CS。
// 用杜邦線接，速度先保守設 12.5 MHz；之後焊在萬用板上可以再調高。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"
#include "sd.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// ---- 硬體設定（函式庫來呼叫） ----

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 2,
    .mosi_gpio = 3,
    .miso_gpio = 4,
    .baud_rate = 12500 * 1000,
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 5,   // CS
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num)
{
    return num == 0 ? &sd_card : NULL;
}

// ---- 我們用的函式 ----

static FATFS fs;               // FatFs 的工作區（掛載期間要一直存在，所以是 static）
static char err_msg[48] = "";

const char *sd_error(void) { return err_msg; }

int sd_mount(void)
{
    // 有些卡剛通電還沒準備好，第一次會失敗：最多試 5 次，每次間隔 200 ms
    FRESULT fr = FR_NOT_READY;
    for (int i = 0; i < 5; ++i) {
        fr = f_mount(&fs, "", 1);   // 1 = 現在就掛上（立刻知道有沒有卡）
        if (fr == FR_OK) return 0;
        sleep_ms(200);
    }
    snprintf(err_msg, sizeof err_msg, "SD: %s", FRESULT_str(fr));
    return 1;
}

// ---- 一段一段讀 ----
// luarom 讀位元組碼時常常一次只要 1 byte；每次都呼叫 FatFs 太慢，所以自己多一層 512 bytes 的緩衝

static FIL stream;
uint32_t sd_read_us;   // 診斷：讀 SD 累計花了多少微秒
static uint8_t sbuf[512];
static UINT sbuf_len, sbuf_pos;

int sd_stream_open(const char *path)
{
    FRESULT fr = f_open(&stream, path, FA_READ);
    if (fr != FR_OK) {
        snprintf(err_msg, sizeof err_msg, "SD %s: %s", path, FRESULT_str(fr));
        return 1;
    }
    sbuf_len = sbuf_pos = 0;
    return 0;
}

int sd_stream_read(void *ud, void *dst, size_t n)
{
    (void)ud;
    uint8_t *d = dst;
    while (n > 0) {
        if (sbuf_pos == sbuf_len) {
            uint32_t t0 = time_us_32();
            FRESULT fr = f_read(&stream, sbuf, sizeof sbuf, &sbuf_len);
            sd_read_us += time_us_32() - t0;
            if (fr != FR_OK || sbuf_len == 0) return 1;
            sbuf_pos = 0;
        }
        UINT k = sbuf_len - sbuf_pos;
        if (k > n) k = (UINT)n;
        memcpy(d, sbuf + sbuf_pos, k);
        sbuf_pos += k; d += k; n -= k;
    }
    return 0;
}

void sd_stream_close(void)
{
    f_close(&stream);
}

// ---- 紀錄檔 nova_log.txt ----
// 主機沒有螢幕可看（例如畫面出不來）的時候，在電腦上打開 SD 卡的這個檔案，就知道開機走到哪一步。
// 每一行寫完馬上 f_sync（真的寫到卡上）：之後當機或斷電，寫過的行也不會不見
static FIL logf;
static int log_open;

void sd_log(const char *line)
{
    if (!log_open) {
        if (f_open(&logf, "nova_log.txt", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
        // 太大（超過 64 KB）：舊的改名成 nova_log_old.txt，從新檔開始寫（上一次開機的紀錄還留著）
        if (f_size(&logf) > 64 * 1024) {
            f_close(&logf);
            f_unlink("nova_log_old.txt");
            f_rename("nova_log.txt", "nova_log_old.txt");
            if (f_open(&logf, "nova_log.txt", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
        }
        log_open = 1;
    }
    char stamp[16];
    unsigned ms = to_ms_since_boot(get_absolute_time());
    snprintf(stamp, sizeof stamp, "%6u.%03u ", ms / 1000, ms % 1000);   // 開機後幾秒
    f_puts(stamp, &logf);
    f_puts(line, &logf);
    f_puts("\n", &logf);
    f_sync(&logf);
}

size_t sd_stream_pos(void)
{
    // 檔案讀到哪裡 - 緩衝區裡還沒交出去的 = 呼叫的人實際讀到哪裡
    return (size_t)f_tell(&stream) - (sbuf_len - sbuf_pos);
}

size_t sd_stream_size(void)
{
    return (size_t)f_size(&stream);
}

// CRC32（和 zip、PNG 用的同一種）：一個 bit 一個 bit 算，慢但很短，64 KB 約幾毫秒
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int sd_fingerprint(const char *path, uint32_t *size, uint32_t *stamp, uint32_t *crc)
{
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr != FR_OK) {
        snprintf(err_msg, sizeof err_msg, "SD %s: %s", path, FRESULT_str(fr));
        return 1;
    }
    *size = (uint32_t)fno.fsize;
    *stamp = ((uint32_t)fno.fdate << 16) | fno.ftime;   // FatFs 的日期、時間各 16 bits
    // 開頭 64 KB 的 CRC：同名同大小但內容不同的檔案（例如重新編譯）也分得出來
    if (sd_stream_open(path) != 0) return 1;
    uint32_t c = 0;
    size_t left = 64 * 1024;
    while (left > 0) {
        if (sbuf_pos == sbuf_len) {
            if (f_read(&stream, sbuf, sizeof sbuf, &sbuf_len) != FR_OK || sbuf_len == 0) break;
            sbuf_pos = 0;
        }
        size_t k = sbuf_len - sbuf_pos;
        if (k > left) k = left;
        c = crc32_update(c, sbuf + sbuf_pos, k);
        sbuf_pos += (UINT)k;
        left -= k;
    }
    sd_stream_close();
    *crc = c;
    return 0;
}

// ---- 診斷：自己送 CMD0 ----
// SD 卡的 SPI 模式開機流程（SD 規格 7.2.1）：
//   1. CS 拉高，送至少 74 個時脈（這裡送 80 個 = 10 bytes 的 0xFF），讓卡片進入準備狀態
//   2. CS 拉低，送 CMD0：0x40 00 00 00 00 95（0x95 是這個指令固定的 CRC）
//   3. 卡片會在 1–8 bytes 內回一個 0x01（「我在閒置狀態」）
void sd_probe(char *out, int cap)
{
    spi_init(spi0, 400 * 1000);   // 開機流程規定 400 kHz 以下
    gpio_set_function(2, GPIO_FUNC_SPI);
    gpio_set_function(3, GPIO_FUNC_SPI);
    gpio_set_function(4, GPIO_FUNC_SPI);
    gpio_pull_up(4);              // MISO：卡片不說話時讀到 1（0xFF）
    gpio_init(5);
    gpio_set_dir(5, GPIO_OUT);
    gpio_put(5, 1);               // CS 先拉高
    sleep_ms(10);

    uint8_t ff[10];
    for (int i = 0; i < 10; ++i) ff[i] = 0xFF;
    spi_write_blocking(spi0, ff, sizeof ff);

    gpio_put(5, 0);
    const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
    spi_write_blocking(spi0, cmd0, sizeof cmd0);
    uint8_t resp[8];
    spi_read_blocking(spi0, 0xFF, resp, sizeof resp);
    gpio_put(5, 1);
    spi_write_blocking(spi0, ff, 1);   // 再送 8 個時脈，讓卡片放開 MISO

    int n = snprintf(out, (size_t)cap, "SD CMD0:");
    for (int i = 0; i < 8 && n < cap - 3; ++i) n += snprintf(out + n, (size_t)(cap - n), " %02X", resp[i]);
    spi_deinit(spi0);   // 交還給函式庫
}

int sd_read_file(const char *path, char **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        snprintf(err_msg, sizeof err_msg, "SD %s: %s", path, FRESULT_str(fr));
        return 1;
    }
    FSIZE_t size = f_size(&f);
    // +1：最後補一個 0，讀進來的內容也能當成 C 字串用
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        snprintf(err_msg, sizeof err_msg, "SD %s: NO MEMORY (%uK)", path, (unsigned)(size / 1024));
        f_close(&f);
        return 1;
    }
    UINT got = 0;
    fr = f_read(&f, buf, (UINT)size, &got);
    f_close(&f);
    if (fr != FR_OK || got != size) {
        snprintf(err_msg, sizeof err_msg, "SD %s: READ %s", path, FRESULT_str(fr));
        free(buf);
        return 1;
    }
    buf[size] = 0;
    *data = buf;
    *len = (size_t)size;
    return 0;
}

// ============================================================================
// 存檔：SD 卡的 saves 資料夾，一個欄位一個檔案
// ============================================================================
// 檔名和 Mac 版一樣：遊戲 id 的 / 換成 _，例如 saves/hardliver_wildkin.1.sav
// （把 SD 卡插到電腦，可以直接複製給 Mac 版用，反過來也可以）

static void save_path(char *out, size_t size, const char *game_id, int slot, const char *ext)
{
    char id[64];
    snprintf(id, sizeof id, "%s", game_id);
    for (char *c = id; *c; c++) {
        if (*c == '/' || *c == '\\' || *c == ':') *c = '_';   // 這些字元不能出現在檔名裡
    }
    snprintf(out, size, "saves/%s.%d.%s", id, slot, ext);
}

int sd_save_read(const char *game_id, int slot, unsigned char *buf, int cap)
{
    char path[96];
    save_path(path, sizeof path, game_id, slot, "sav");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;   // 沒有存檔
    UINT got = 0;
    FRESULT fr = f_read(&f, buf, (UINT)cap, &got);
    f_close(&f);
    return fr == FR_OK ? (int)got : -1;
}

int sd_save_write(const char *game_id, int slot, const unsigned char *data, int len)
{
    char path[96], tmp[96];
    save_path(path, sizeof path, game_id, slot, "sav");
    if (data == NULL) {                       // 刪除這個欄位
        FRESULT fr = f_unlink(path);
        return (fr == FR_OK || fr == FR_NO_FILE) ? 0 : 1;
    }
    f_mkdir("saves");                         // 已經有了會回傳錯誤，不用管
    // 先寫到暫存檔、確定寫進卡片（f_sync），再換掉舊的：寫到一半斷電，舊的存檔還在
    save_path(tmp, sizeof tmp, game_id, slot, "tmp");
    FIL f;
    if (f_open(&f, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return 1;
    UINT put = 0;
    FRESULT fr = f_write(&f, data, (UINT)len, &put);
    if (fr == FR_OK) fr = f_sync(&f);
    f_close(&f);
    if (fr != FR_OK || put != (UINT)len) return 1;
    f_unlink(path);                           // FatFs 的改名不能蓋掉已經存在的檔案，所以先刪舊的
    return f_rename(tmp, path) == FR_OK ? 0 : 1;
}
