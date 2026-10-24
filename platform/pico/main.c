// main.c — Nova One 主機（Pico 2 W）的平台層
//
// 和 platform/mac、platform/web 是同一個角色：核心（core/）只會畫在 320×240 的陣列、
// 讀按鍵陣列，這裡負責把它接到真正的硬體：
//   畫面 → video.c（HSTX → HDMI）
//   按鍵 → usb_pad.c（PIO-USB 手把，第二個核心）
//   卡匣 → 目前編譯時內嵌在韌體裡（cart_data.c，由 embed_cart.py 產生）；之後改成從 SD 卡讀
//
// 時脈（2026-10-23 實測決定）：
//   pll_sys 144 MHz → CPU、周邊（SPI、UART）、PIO-USB（要 12 MHz 的倍數），÷3 = 48 MHz 給 micro USB
//   pll_usb 126 MHz → 只給 HSTX（252 Mbps = 標準 640×480 @ 60 Hz；新版 SDK 預設的 150 MHz 會變 71 Hz，電視不收）

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"

#include "../../core/engine.h"
#include "../../core/gfx.h"
#include "../../core/input.h"
#include "../../core/platform.h"
#include "../../core/script.h"
#include "video.h"
#include "usb_pad.h"
#include "sd.h"
#include "romflash.h"
#include "../../core/luarom.h"
#include "lauxlib.h"
#include "tusb.h"
#include "xinput_host.h"   // XINPUT_GAMEPAD_* 按鍵位元（要先 include tusb.h）

// cart_data.c（編譯時產生）：卡匣的原始碼
extern const char cart_src[];
extern const unsigned cart_len;

// ============================================================================
// 時脈
// ============================================================================

static void nova_clocks(void)
{
    set_sys_clock_khz(144000, true);
    // 周邊時脈（SPI、UART 用它算速度）：set_sys_clock_khz 會把它接到 pll_usb 的 48 MHz。
    // 下面要把 pll_usb 改成 126 MHz，它就會跟著變快，但 SDK 還以為是 48 MHz，
    // 算出來的 SPI 速度會快 2.6 倍（2026-10-23：SD 卡因此讀不到）。所以先改接 pll_sys。
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, 144 * MHZ);
    // micro USB 和 ADC 需要 48 MHz，原本接在 pll_usb；先改接 pll_sys ÷ 3，再把 pll_usb 調成 126 MHz
    clock_configure_int_divider(clk_usb, 0, CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, 144 * MHZ, 3);
    clock_configure_int_divider(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, 144 * MHZ, 3);
    // 12 MHz 晶振 × 126 = 1512 MHz，÷ 6 ÷ 2 = 126 MHz
    pll_init(pll_usb, 1, 1512 * MHZ, 6, 2);
    clock_configure_undivided(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, 126 * MHZ);
}

// ============================================================================
// 核心要求平台提供的功能（core/platform.h）
// ============================================================================

// 訊息：主機沒有終端機，留下最近幾行，Lua 出錯時畫在畫面上
#define LOG_LINES 8
#define LOG_COLS  38   // 每行最多幾個字元（320 ÷ 8）
static char log_buf[LOG_LINES][LOG_COLS + 1];
static int  log_next;

void platform_log(const char *msg)
{
    // 一則訊息可能有好幾行（Lua 的錯誤訊息），拆開來放
    while (*msg) {
        char *line = log_buf[log_next];
        int n = 0;
        while (*msg && *msg != '\n' && n < LOG_COLS) line[n++] = *msg++;
        line[n] = 0;
        while (*msg && *msg != '\n') msg++;   // 太長的部分丟掉
        if (*msg == '\n') msg++;
        log_next = (log_next + 1) % LOG_LINES;
    }
}

// 存檔：還沒做（之後放 SD 卡）。讀取回報「沒有存檔」，寫入回報失敗
int platform_save_read(const char *game_id, int slot, unsigned char *buf, int cap)
{
    (void)game_id; (void)slot; (void)buf; (void)cap;
    return -1;
}

int platform_save_write(const char *game_id, int slot, const unsigned char *data, int len)
{
    (void)game_id; (void)slot; (void)data; (void)len;
    return 1;
}

// 現實時鐘：還沒有（之後用 WiFi 對時）
int64_t platform_clock(void) { return -1; }

// ============================================================================
// 疊在畫面上的文字（錯誤訊息、狀態）：3×5 的小字型，只有用到的字
// ============================================================================
// 畫在 video 的「顯示用畫面」上，不會影響遊戲自己的畫面，也不會被重播、測試看到。

static const struct { char ch; uint8_t rows[5]; } FONT[] = {
    {'0',{7,5,5,5,7}},{'1',{2,6,2,2,7}},{'2',{7,1,7,4,7}},{'3',{7,1,7,1,7}},
    {'4',{5,5,7,1,1}},{'5',{7,4,7,1,7}},{'6',{7,4,7,5,7}},{'7',{7,1,1,1,1}},
    {'8',{7,5,7,5,7}},{'9',{7,5,7,1,7}},{'A',{2,5,7,5,5}},{'B',{6,5,6,5,6}},
    {'C',{7,4,4,4,7}},{'D',{6,5,5,5,6}},{'E',{7,4,7,4,7}},{'F',{7,4,7,4,4}},
    {'G',{7,4,5,5,7}},{'H',{5,5,7,5,5}},{'I',{7,2,2,2,7}},{'J',{1,1,1,5,7}},
    {'K',{5,5,6,5,5}},{'L',{4,4,4,4,7}},{'M',{5,7,7,5,5}},{'N',{6,5,5,5,5}},
    {'O',{7,5,5,5,7}},{'P',{7,5,7,4,4}},{'Q',{7,5,5,7,1}},{'R',{6,5,6,5,5}},
    {'S',{7,4,7,1,7}},{'T',{7,2,2,2,2}},{'U',{5,5,5,5,7}},{'V',{5,5,5,5,2}},
    {'W',{5,5,7,7,5}},{'X',{5,5,2,5,5}},{'Y',{5,5,2,2,2}},{'Z',{7,1,2,4,7}},
    {'-',{0,0,7,0,0}},{':',{0,2,0,2,0}},{'.',{0,0,0,0,2}},{'%',{5,1,2,4,5}},
    {'/',{1,1,2,4,4}},{'(',{1,2,2,2,1}},{')',{4,2,2,2,4}},{'_',{0,0,0,0,7}},
    {'=',{0,7,0,7,0}},{'\'',{2,2,0,0,0}},{'"',{5,5,0,0,0}},{',',{0,0,0,2,4}},
};

// 疊字專用的顏色：調色盤最後一格強制當成白色（只影響送到 HDMI 的顏色，不改遊戲的調色盤）
#define OVERLAY_COLOR 255

static void overlay_char(int x, int y, char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');   // 只有大寫字型
    uint8_t *fb = video_display_buffer();
    for (unsigned i = 0; i < count_of(FONT); ++i) {
        if (FONT[i].ch != c) continue;
        for (int r = 0; r < 5; ++r)
            for (int b = 0; b < 3; ++b)
                if (FONT[i].rows[r] & (4 >> b)) {
                    int px = x + b, py = y + r;
                    if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                        fb[py * SCREEN_W + px] = OVERLAY_COLOR;
                }
        return;
    }
}

static void overlay_text(int x, int y, const char *s)
{
    for (; *s; ++s, x += 4) overlay_char(x, y, *s);
}

// ============================================================================
// 手把 → Comet 的按鍵
// ============================================================================

// 搖桿放開時不會剛好是 0（有一點雜訊），太小的值當成 0，畫面上的東西才不會自己慢慢飄
#define STICK_DEADZONE 4000

static int stick_to_comet(int16_t v)
{
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0;
    int x = v / 258;   // ±32767 → ±127
    if (x > STICK_MAX) x = STICK_MAX;
    if (x < -STICK_MAX) x = -STICK_MAX;
    return x;
}

static void read_input(void)
{
    PadState p;
    usb_pad_get(&p);
    uint16_t b = 0;
    // 一個表格：XInput 的位元 → Comet 的按鍵編號
    static const struct { uint16_t xi; uint8_t btn; } MAP[] = {
        { XINPUT_GAMEPAD_DPAD_LEFT,      BTN_LEFT   },
        { XINPUT_GAMEPAD_DPAD_RIGHT,     BTN_RIGHT  },
        { XINPUT_GAMEPAD_DPAD_UP,        BTN_UP     },
        { XINPUT_GAMEPAD_DPAD_DOWN,      BTN_DOWN   },
        { XINPUT_GAMEPAD_A,              BTN_A      },
        { XINPUT_GAMEPAD_B,              BTN_B      },
        { XINPUT_GAMEPAD_X,              BTN_X      },
        { XINPUT_GAMEPAD_Y,              BTN_Y      },
        { XINPUT_GAMEPAD_START,          BTN_START  },
        { XINPUT_GAMEPAD_BACK,           BTN_SELECT },
        { XINPUT_GAMEPAD_LEFT_SHOULDER,  BTN_L      },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, BTN_R      },
        { XINPUT_GAMEPAD_GUIDE,          BTN_HOME   },
    };
    for (unsigned i = 0; i < count_of(MAP); ++i)
        if (p.buttons & MAP[i].xi) b |= (uint16_t)(1u << MAP[i].btn);
    input_set(0, b);
    // XInput 的上是正的，Comet 的上是負的（和畫面座標一致）
    input_set_stick(0, STICK_LEFT,  stick_to_comet(p.lx), -stick_to_comet(p.ly));
    input_set_stick(0, STICK_RIGHT, stick_to_comet(p.rx), -stick_to_comet(p.ry));
}

// ============================================================================
// 記憶體：Lua 用多少、還剩多少
// ============================================================================
// 連結器定義的位置：__bss_end__ 是靜態變數的結尾，堆積（malloc）從這裡開始，
// 長到 __StackLimit（堆疊的底）為止
extern char __bss_end__, __StackLimit;

static unsigned heap_total(void) { return (unsigned)(&__StackLimit - &__bss_end__); }
static unsigned heap_used(void)  { return (unsigned)mallinfo().uordblks; }

// ============================================================================
// 預先編譯的卡匣（.nvc）：程式放 Flash（core/luarom.h、romflash.h）
// ============================================================================
// .nvc 的格式（Mac 版 --compile 產生）：「NOVACART」+ 開頭原始碼長度（4 bytes）+ 開頭原始碼 + Lua 位元組碼

static const void *rom_root;

static int rom_loader(lua_State *L, void *ud)
{
    (void)ud;
    luarom_push_main(L, rom_root);
    return LUA_OK;
}

// ---- 載入中畫面 ----
// 直接畫在 HDMI 的顯示畫面上（引擎還沒有遊戲可以畫）。寫 Flash 時畫面會閃，
// 每寫 64 次更新一次數字就夠了，太常重畫只是浪費時間
static uint32_t loading_t0;
static unsigned loading_ops;

static void show_loading(void)
{
    memset(video_display_buffer(), 0, SCREEN_W * SCREEN_H);   // 0 號色 = 黑
    video_set_color(0, 0x00);
    video_set_color(OVERLAY_COLOR, 0xFF);
    overlay_text(4, 4, "LOADING GAME.NVC");
    overlay_text(4, 12, "FIRST TIME TAKES ABOUT 30S");
    char s[64];
    snprintf(s, sizeof s, "WR %u ER %u FE %u %uS", romflash_pages_written(),
             romflash_sectors_erased(), romflash_errors(),
             (unsigned)((time_us_32() - loading_t0) / 1000000));
    overlay_text(4, 24, s);
}

static void loading_progress(void)
{
    if (++loading_ops % 64 == 0) show_loading();
}

// 成功回傳 0（*err 是 engine_load 的結果）；找不到或格式不對回傳 1（改用別的卡匣）
static int load_nvc(const char *path, char *info, size_t info_size, int *err)
{
    if (sd_stream_open(path) != 0) return 1;
    char magic[8];
    uint32_t head_len;
    if (sd_stream_read(NULL, magic, 8) || memcmp(magic, "NOVACART", 8) != 0 ||
        sd_stream_read(NULL, &head_len, 4) || head_len > 4096) {
        sd_stream_close();
        snprintf(info, info_size, "GAME.NVC: BAD FILE");
        return 1;
    }
    char *head = malloc(head_len + 1);
    if (head == NULL || sd_stream_read(NULL, head, head_len)) {
        free(head);
        sd_stream_close();
        return 1;
    }

    // 「載入中」畫面：第一次裝一款遊戲要寫很久，讓玩家知道主機沒當機
    loading_t0 = time_us_32();
    show_loading();
    romflash_set_progress(loading_progress);

    // 建唯讀區：和 Flash 上一樣的部分不寫（同一款遊戲第二次開就很快）。
    // 第一次裝一款大遊戲要寫 1–2 MB，大約 30 秒，這段時間畫面會閃或沒有訊號
    uint32_t t0 = time_us_32();
    RomWriter w;
    romflash_writer(&w);
    lua_State *T = luaL_newstate();   // 只借它的字串雜湊種子
    char e[96];
    int r = luarom_build(T, sd_stream_read, NULL, &w, &rom_root, e, sizeof e);
    lua_close(T);
    sd_stream_close();
    if (r != 0) {
        platform_log(e);
        romflash_set_progress(NULL);
        free(head);
        snprintf(info, info_size, "GAME.NVC: BUILD FAILED");
        return 1;
    }
    romflash_finish_build(w.used);
    unsigned code_kb = (unsigned)(w.used / 1024);

    // 告訴引擎：程式在唯讀區；圖片、字型、地圖、凍結的表格也接著放在那裡
    luarom_activate(romflash_base(), ROMFLASH_SIZE);
    gfx_set_rom_store(romflash_store);
    luarom_set_append(romflash_append);
    script_set_loader(rom_loader, NULL);
    *err = engine_load(head, head_len);   // 開頭原始碼：引擎只用來讀遊戲 id
    // （遊戲最上層的程式在這裡執行：freeze、newimage 也會寫 Flash，所以載入中畫面到這裡才停）
    romflash_set_progress(NULL);
    free(head);

    snprintf(info, info_size, "CART GAME.NVC ROM %uK+%uK WR %u ER %u FE %u %uS",
             code_kb, (unsigned)((romflash_used() - w.used) / 1024),
             romflash_pages_written(), romflash_sectors_erased(), romflash_errors(),
             (unsigned)((time_us_32() - t0) / 1000000));
    return 0;
}

// ============================================================================
// 主程式
// ============================================================================

// ============================================================================
// 換一個比較大的堆疊
// ============================================================================
// Pico SDK 把堆疊放在 4 KB 的 scratch 記憶體（快，但小）。Lua 會一層層遞迴呼叫，
// 解析大卡匣、遊戲呼叫深一點就會用超過 4 KB，堆疊一爆就會蓋到別的資料，錯誤非常難找。
// 做法：在一般 RAM 準備 32 KB 的陣列，把 CPU 的堆疊指標（MSP）指到它的尾端，再呼叫 nova_main。
// 為什麼是尾端：ARM 的堆疊是「往下長」的，從高位址往低位址用。
// 中斷也用 MSP，所以中斷之後也在這塊新堆疊上執行，不會出問題。

static uint32_t big_stack[32 * 1024 / 4] __attribute__((aligned(8)));   // ARM 規定堆疊要 8 bytes 對齊

static void nova_main(void);

static void __attribute__((noreturn)) run_on_big_stack(void)
{
    void *top = &big_stack[count_of(big_stack)];
    // msr msp：設定堆疊指標；bx：跳去 nova_main（不會回來，所以舊的堆疊不再需要）
    __asm volatile ("msr msp, %0\n\tbx %1" : : "r"(top), "r"(nova_main) : "memory");
    __builtin_unreachable();
}

int main(void)
{
    run_on_big_stack();
}

static void nova_main(void)
{
    nova_clocks();
    video_start();
    usb_pad_start();

    // SD 卡的驅動會向 SDK 借 DMA 通道；等 USB 先登記好它的通道 2，才不會搶到同一個
    absolute_time_t until = make_timeout_time_ms(1000);
    while (!usb_pad_ready() && !time_reached(until)) tight_loop_contents();

    engine_init();
    engine_set_seed(time_us_32());   // 每次開機不同的亂數種子（同一場遊戲內仍然是同步的）

    // 卡匣，依序找：
    //   SD 卡的 game.nvc（預先編譯好的，程式放 Flash：大遊戲用這個）
    //   SD 卡的 game.lua（原始碼，整個放 RAM：小遊戲）
    //   韌體內建的卡匣
    char cart_info[64] = "";
    int err;
    char *sd_src = NULL;
    size_t sd_len = 0;
    int sd_ok = sd_mount() == 0;
    // game.nvc 失敗的原因先記下來：後面試 game.lua 時 sd_error() 會被蓋掉，
    // 只顯示「找不到 game.lua」就看不出 game.nvc 是哪裡出問題
    char nvc_why[64] = "";
    if (sd_ok && load_nvc("game.nvc", cart_info, sizeof cart_info, &err) == 0) {
        // 已經載入
    } else {
        if (sd_ok) {
            // load_nvc 失敗時，cart_info 裡是原因（BAD FILE 等）；打不開檔案時原因在 sd_error()
            snprintf(nvc_why, sizeof nvc_why, "%s", cart_info[0] ? cart_info : sd_error());
            platform_log(nvc_why);
        }
        if (sd_ok && sd_read_file("game.lua", &sd_src, &sd_len) == 0) {
            err = engine_load(sd_src, sd_len);
            free(sd_src);   // 引擎已經把需要的東西複製走了（編譯好的程式、遊戲 id），原始碼可以丟掉
            snprintf(cart_info, sizeof cart_info, "CART SD GAME.LUA %uK", (unsigned)(sd_len / 1024));
        } else {
            err = engine_load(cart_src, cart_len);
            snprintf(cart_info, sizeof cart_info, "CART BUILT-IN (%s)", sd_error());
        }
    }
    unsigned heap_after_load = heap_used();

    bool show_stats = true;              // 開發中先預設顯示，之後改成預設關閉
    uint32_t combo_frames = 0;           // 「SELECT + L + R」按住了幾幀
    uint32_t last = video_frame_count();
    uint32_t frame_us = 0;

    while (true) {
        // 等 HDMI 進入垂直空白期（每秒 60 次）
        while (video_frame_count() == last) __wfe();
        last = video_frame_count();

        // 1. 把上一幀畫好的畫面交給 HDMI。空白期約 1.4 ms，複製 76 KB 約 0.5 ms，來得及
        memcpy(video_display_buffer(), gfx_framebuffer(), SCREEN_W * SCREEN_H);
        video_set_palette(gfx_palette());
        bool overlay = err || script_failed() || show_stats;
        if (overlay) video_set_color(OVERLAY_COLOR, 0xFF);   // 白色

        // 2. 疊字：出錯時顯示錯誤訊息；按住 SELECT + L + R 一秒切換狀態顯示
        if (err || script_failed()) {
            overlay_text(4, 4, err ? "LOAD ERROR" : "LUA ERROR");
            for (int i = 0; i < LOG_LINES; ++i)
                overlay_text(4, 14 + i * 7, log_buf[(log_next + i) % LOG_LINES]);
        }
        if (show_stats) {
            char s[96];
            snprintf(s, sizeof s, "HEAP %uK/%uK LUA %uK LOAD %uK FRAME %u.%uMS",
                     heap_used() / 1024, heap_total() / 1024, (unsigned)(script_mem() / 1024),
                     heap_after_load / 1024,
                     (unsigned)(frame_us / 1000), (unsigned)(frame_us % 1000 / 100));
            overlay_text(4, SCREEN_H - 8, s);
            PadState p;
            usb_pad_get(&p);
            snprintf(s, sizeof s, "PAD %s %04X LX %d LY %d", p.connected ? "OK" : "--",
                     p.buttons, p.lx / 258, p.ly / 258);
            overlay_text(4, SCREEN_H - 15, s);
            overlay_text(4, SCREEN_H - 22, cart_info);
        }

        // 3. 讀手把，跑一幀遊戲（_update、_draw）。超過 16.6 ms 的話會晚一個空白期，遊戲變慢但不出錯
        read_input();
        uint32_t t0 = time_us_32();
        engine_frame();
        frame_us = time_us_32() - t0;

        uint16_t held = input_get_buttons(0);
        uint16_t combo = (1u << BTN_SELECT) | (1u << BTN_L) | (1u << BTN_R);
        combo_frames = ((held & combo) == combo) ? combo_frames + 1 : 0;
        if (combo_frames == 60) show_stats = !show_stats;
    }
}
