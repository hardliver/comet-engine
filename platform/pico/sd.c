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
            if (f_read(&stream, sbuf, sizeof sbuf, &sbuf_len) != FR_OK || sbuf_len == 0) return 1;
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
