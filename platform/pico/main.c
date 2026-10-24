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
#include "hardware/watchdog.h"
#include "hardware/structs/mpu.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"

// ============================================================================
// 當機紀錄（除錯用）
// ============================================================================
// 程式讀寫到不該碰的位址、執行到壞掉的指令時，CPU 會跳到 HardFault。
// HardFault 比所有中斷都優先，畫面的中斷也停了 → 電視沒訊號，什麼都看不到。
// 所以在 HardFault 裡把「當時執行到哪裡」記在一塊開機時不會清掉的 RAM，
// 然後重新開機；下一次開機發現有紀錄，就顯示在畫面上，也先不載入 game.nvc（不然又當）。
// PC 是當機那一行程式的位址，用 arm-none-eabi-addr2line 可以查出是哪個函式。

#define CRASH_MAGIC 0x4E4F5641u   // "NOVA"
typedef struct { uint32_t magic, pc, lr, cfsr, hfsr, bfar, r0, core, stack0, vtor, slot, ndiff, firstdiff, dma; } CrashInfo;
static uint32_t vt_snapshot[68];   // 開機登記完後的向量表（比對哪些格子被改）
static CrashInfo __uninitialized_ram(crash);   // 重新開機（看門狗）後內容還在

// 放在 RAM（__not_in_flash_func）：當機可能剛好發生在寫 Flash 的時候，那時不能從 Flash 讀程式
void __attribute__((used)) __not_in_flash_func(hardfault_record)(uint32_t *frame)
{
    // frame：CPU 進 HardFault 時自動存在堆疊上的 8 個暫存器：r0 r1 r2 r3 r12 lr pc xpsr
    crash.pc = frame[6];
    crash.lr = frame[5];
    crash.r0 = frame[0];          // 「沒有處理函式的中斷」時，r0 是那個中斷的編號
    crash.core = *(volatile uint32_t *)(SIO_BASE + 0x000);   // SIO CPUID：哪個核心（0 或 1）
    crash.vtor = scb_hw->vtor;                               // 這個核心當時的中斷向量表位址
    crash.slot = ((volatile uint32_t *)crash.vtor)[16 + (crash.r0 & 63)];   // 表裡那個中斷的處理函式
    // 向量表和開機時比：幾格不一樣、第一個不一樣的是哪格（全部都變 = 整張被重設；只有幾格 = 被蓋掉）
    crash.ndiff = 0; crash.firstdiff = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 68; ++i) {
        if (((volatile uint32_t *)0x20000000u)[i] != vt_snapshot[i]) {
            if (crash.ndiff == 0) crash.firstdiff = i;
            crash.ndiff++;
        }
    }
    // 有沒有 DMA 通道的寫入位址在向量表附近（DMA 寫記憶體不經過 MPU，抓不到）
    crash.dma = 0xFFFFFFFFu;
    for (uint32_t ch = 0; ch < 16; ++ch) {
        uint32_t wa = *(volatile uint32_t *)(0x50000000u + ch * 0x40u + 4u);   // DMA CHx_WRITE_ADDR
        if (wa >= 0x20000000u && wa < 0x20000400u) { crash.dma = (ch << 24) | (wa & 0xFFFFFFu); break; }
    }
    crash.cfsr = scb_hw->cfsr;   // 是哪一種錯（讀寫錯、指令錯、對齊錯…）
    crash.hfsr = scb_hw->hfsr;
    crash.bfar = *(volatile uint32_t *)0xE000ED34u;   // MMFAR：寫到受保護區域時，被寫的位址
    // 主核心的堆疊最多用到哪裡（從底部數還沒被改過的字；見 run_on_big_stack）
    {
        extern uint32_t big_stack_words[];
        uint32_t i = 0;
        while (i < 16 * 1024 / 4 && big_stack_words[i] == 0x5A5A5A5Au) ++i;
        crash.stack0 = i * 4;
    }
    crash.magic = CRASH_MAGIC;
    // 重新開機：寫 AIRCR 的 SYSRESETREQ（0x05FA 是規定的「鑰匙」，不對就不理）。RAM 的內容會保留
    __asm volatile("dsb");
    scb_hw->aircr = 0x05FA0004u;
    while (1) {}
}

// ---- 保護中斷向量表（診斷）----
// 2026-10-24：USB 核心當機時，中斷向量表第 41 格被改回「沒有人處理」，但程式裡沒有地方會這樣改。
// 用 MPU（記憶體保護單元）把向量表設成唯讀：之後誰去寫它，CPU 會當場出錯，
// 當機紀錄的 PC 就是「寫的那一行程式」，MMFAR 是被寫的位址。
// 所有中斷都登記完才呼叫。每個核心有自己的 MPU，兩個核心都要設（usb_pad.c 也呼叫）
void protect_vector_table(void)
{
    if (get_core_num() == 0) memcpy(vt_snapshot, (const void *)0x20000000u, sizeof vt_snapshot);
    mpu_hw->ctrl = 0;
    __dsb(); __isb();
    mpu_hw->mair[0] = 0xFF;   // 屬性 0：一般記憶體（可快取）
    mpu_hw->rnr = 0;          // 第 0 個區域
    // 0x20000000–0x200000FF（向量表前 64 格，含第 41 格）；AP = 11：所有人唯讀
    mpu_hw->rbar = 0x20000000u | (3u << M33_MPU_RBAR_AP_LSB);
    mpu_hw->rlar = 0x200000E0u | M33_MPU_RLAR_EN_BITS;
    // PRIVDEFENA：其他沒設定的位址照原本的規則（不然整個記憶體都不能用）
    mpu_hw->ctrl = M33_MPU_CTRL_PRIVDEFENA_BITS | M33_MPU_CTRL_ENABLE_BITS;
    __dsb(); __isb();
}

// 主程式卡住（video.c 偵測到 5 秒沒有進到下一幀）：記下最後看到的位置，重新開機
#define HANG_MAGIC 0x48414E47u   // "HANG"
void __not_in_flash_func(hang_detected)(uint32_t pc, uint32_t lr)
{
    crash.pc = pc;
    crash.lr = lr;
    crash.core = 0;
    crash.r0 = -1;
    crash.cfsr = 0;
    crash.magic = HANG_MAGIC;
    __asm volatile("dsb");
    scb_hw->aircr = 0x05FA0004u;
    while (1) {}
}

// naked：編譯器不要自己加進出函式的程式。只做一件事：找出存暫存器的那個堆疊，交給上面的 C 函式。
// lr 的 bit 2 說明當機前用的是哪個堆疊（MSP 或 PSP）。
// 名字不能叫 isr_hardfault：SD 卡的函式庫已經有一個（它最後會停在斷點，沒有除錯器時就卡死），
// 所以開機時用 exception_set_exclusive_handler 把中斷向量表裡的 HardFault 換成這個
static void __attribute__((naked)) __not_in_flash_func(nova_hardfault)(void)
{
    __asm volatile(
        // 先關掉堆疊下限（MSPLIM）：如果是堆疊用完造成的，記錄的程式本身也要用堆疊，不關會再出錯、卡死
        "movs r1, #0\n"
        "msr msplim, r1\n"
        // 也關掉 MPU（見 protect_vector_table）：記錄當機資訊要寫 RAM
        "ldr r2, =0xE000ED94\n"
        "str r1, [r2]\n"
        "dsb\n"
        "isb\n"
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_record\n");
}

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

static int sd_ready;   // SD 卡掛好了：platform_log 也寫進紀錄檔

void platform_log(const char *msg)
{
    if (sd_ready) sd_log(msg);
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

// 存檔：放在 SD 卡的 saves 資料夾（sd.c）。沒有 SD 卡就讀不到、也存不了
int platform_save_read(const char *game_id, int slot, unsigned char *buf, int cap)
{
    if (!sd_ready) return -1;
    return sd_save_read(game_id, slot, buf, cap);
}

int platform_save_write(const char *game_id, int slot, const unsigned char *data, int len)
{
    if (!sd_ready) return 1;
    uint32_t t0 = time_us_32();
    int r = sd_save_write(game_id, slot, data, len);
    char s[96];
    snprintf(s, sizeof s, "save slot %d: %d bytes, %s, %u ms", slot, len, r == 0 ? "ok" : "FAILED",
             (unsigned)((time_us_32() - t0) / 1000));
    sd_log(s);
    return r;
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

// ---- 診斷：時脈和畫面速度 ----
// frequency_count_khz：用晶片內建的頻率計量測實際的時脈（不是設定值）
static void log_clocks(const char *when)
{
    uint32_t f0 = video_frame_count();
    uint32_t t0 = time_us_32();
    sleep_ms(100);
    uint32_t frames = video_frame_count() - f0, us = time_us_32() - t0;
    char s[160];
    snprintf(s, sizeof s, "%s: sys %u kHz, hstx %u kHz, peri %u kHz, ref %u kHz, video %u frames in %u ms, hstx csr %08X",
             when, (unsigned)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS),
             (unsigned)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX),
             (unsigned)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI),
             (unsigned)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_REF),
             (unsigned)frames, (unsigned)(us / 1000), (unsigned)hstx_ctrl_hw->csr);
    platform_log(s);
}

// ---- 看門狗：一幀跑太久時記下 Lua 執行到哪裡 ----
// （2026-10-24：Wildkin 切換畫面時卡住，紀錄檔看不出卡在哪裡）
static uint32_t frame_start_us;
static uint32_t watch_last_log_us;

static void frame_watch(const char *where)
{
    uint32_t now = time_us_32();
    if (now - frame_start_us < 1000000u) return;          // 這一幀還不到 1 秒：正常
    if (now - watch_last_log_us < 1000000u) return;       // 每秒最多記一行
    watch_last_log_us = now;
    char s[160];
    snprintf(s, sizeof s, "slow frame: %u ms so far, at %s, heap %uK, lua %uK",
             (unsigned)((now - frame_start_us) / 1000), where,
             (unsigned)(mallinfo().uordblks / 1024), (unsigned)(script_mem() / 1024));
    platform_log(s);
}

// ---- 安裝中畫面 ----
// 直接畫在 HDMI 的顯示畫面上（引擎還沒有遊戲可以畫）。
// 寫 Flash 時畫面的中斷照常在跑（romflash.c），所以畫面不會斷。每寫 32 次更新一次就夠了
static uint32_t loading_t0;
static unsigned loading_ops;

static void show_installing(void)
{
    uint8_t *fb = video_display_buffer();
    memset(fb, 0, SCREEN_W * SCREEN_H);   // 0 號色 = 黑
    video_set_color(0, 0x00);
    video_set_color(OVERLAY_COLOR, 0xFF);
    // 進度：遊戲檔讀到哪裡（安裝的最後還有一小段是遊戲自己的載入，停在 100% 一下是正常的）
    size_t total = sd_stream_size();
    unsigned pct = total ? (unsigned)((uint64_t)sd_stream_pos() * 100 / total) : 0;
    if (pct > 100) pct = 100;
    overlay_text(4, 4, "INSTALLING GAME.NVC");
    overlay_text(4, 12, "ONLY THE FIRST TIME. THE SCREEN WILL GO DARK");
    overlay_text(4, 18, "UNTIL IT IS DONE (ABOUT 30S). DO NOT POWER OFF.");
    // 進度條：外框 + 填滿
    int x0 = 4, y0 = 24, w = SCREEN_W - 8, h = 8;
    for (int x = x0; x < x0 + w; ++x) { fb[y0 * SCREEN_W + x] = OVERLAY_COLOR; fb[(y0 + h - 1) * SCREEN_W + x] = OVERLAY_COLOR; }
    for (int y = y0; y < y0 + h; ++y) { fb[y * SCREEN_W + x0] = OVERLAY_COLOR; fb[y * SCREEN_W + x0 + w - 1] = OVERLAY_COLOR; }
    int fill = (w - 4) * (int)pct / 100;
    for (int y = y0 + 2; y < y0 + h - 2; ++y)
        for (int x = x0 + 2; x < x0 + 2 + fill; ++x) fb[y * SCREEN_W + x] = OVERLAY_COLOR;
    char s[64];
    snprintf(s, sizeof s, "%u%%  WR %u ER %u FE %u  %uS", pct, romflash_pages_written(),
             romflash_erases(), romflash_errors(), (unsigned)((time_us_32() - loading_t0) / 1000000));
    overlay_text(4, 36, s);
}

static void installing_progress(void)
{
    ++loading_ops;
    if (loading_ops % 32 == 0) show_installing();
    if (loading_ops % 64 == 0) {
        char s[128];
        snprintf(s, sizeof s, "installing: read %uK/%uK, pages %u, erases %u, fails %u",
                 (unsigned)(sd_stream_pos() / 1024), (unsigned)(sd_stream_size() / 1024),
                 romflash_pages_written(), romflash_erases(), romflash_errors());
        platform_log(s);
        // 診斷：時間花在哪裡（毫秒，從安裝開始累計）
        uint32_t lk, op, ul;
        romflash_timing(&lk, &op, &ul);
        snprintf(s, sizeof s, "  ms: lock %u, flash %u, unlock %u, sd %u, total %u | video irq %u, frames %u",
                 (unsigned)(lk / 1000), (unsigned)(op / 1000), (unsigned)(ul / 1000),
                 (unsigned)(sd_read_us / 1000), (unsigned)((time_us_32() - loading_t0) / 1000),
                 (unsigned)video_irq_count, (unsigned)video_frame_count());
        platform_log(s);
    }
}

// 韌體版本：用編譯的日期時間。換了韌體，記憶體位址可能不同（凍結的表格裡有指向韌體的指標），
// 已安裝的遊戲要重新安裝
static char FIRMWARE_ID[32];

// 韌體本身（Flash 開頭到 __flash_binary_end）的 CRC32：韌體有任何改變，數字就不同。
// 2026-10-24：原本用編譯時間，但只有 main.c 重新編譯才會變，改了別的檔案卻沒變 → 沒有重新安裝
extern char __flash_binary_end;
static void compute_firmware_id(void)
{
    const uint8_t *p = (const uint8_t *)XIP_BASE;
    size_t n = (size_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    snprintf(FIRMWARE_ID, sizeof FIRMWARE_ID, "%08X %uK", (unsigned)~crc, (unsigned)(n / 1024));
}

// 成功回傳 0（*err 是 engine_load 的結果）；找不到或格式不對回傳 1（改用別的卡匣）
//
// 流程：
//   1. 讀遊戲檔的身分（大小、修改時間、開頭的 CRC），和 Flash 上的安裝紀錄比對
//   2. 已安裝 → 跳過建置，直接執行（約 1 秒）
//      沒安裝 → 安裝：一邊從 SD 讀位元組碼，一邊建進 Flash（luarom_build）
//   3. 執行遊戲的最上層程式（freeze、newimage 會接在後面寫；已安裝時內容一樣，不會寫）
//   4. 剛安裝的：最後寫安裝紀錄，下次開機就不用再裝
// 之後從網路（GitHub）下載安裝也是同一套，只是第 2 步的來源換成下載的資料
static int load_nvc(const char *path, char *info, size_t info_size, int *err)
{
    RomId id;
    memset(&id, 0, sizeof id);   // 整塊清 0：比對時用 memcmp，沒用到的 bytes 也要一樣
    if (sd_fingerprint(path, &id.size, &id.stamp, &id.crc) != 0) return 1;
    snprintf(id.firmware, sizeof id.firmware, "%s", FIRMWARE_ID);

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
        snprintf(info, info_size, "GAME.NVC: READ ERROR");
        return 1;
    }

    uint32_t t0 = time_us_32();
    int installed = romflash_installed(&id, &rom_root);
    {
        char s[96];
        snprintf(s, sizeof s, "game.nvc: size %u, crc %08X, %s", (unsigned)id.size, (unsigned)id.crc,
                 installed ? "already installed" : "not installed -> install");
        platform_log(s);
    }
    if (!installed) {
        loading_t0 = t0;
        sd_read_us = 0;
        {
            char s[96];
            snprintf(s, sizeof s, "before install: video irq %u, frames %u", (unsigned)video_irq_count,
                     (unsigned)video_frame_count());
            platform_log(s);
        }
        show_installing();
        // 讓電視先同步、顯示這個畫面 5 秒，再把畫面停掉開始安裝（寫 Flash 時畫面不能開著，見 video.c）。
        // 電視收到訊號後要 3 秒左右才會顯示（2026-10-24 實測 2.5 秒看不到）
        sleep_ms(5000);
        video_stop();
        romflash_set_progress(installing_progress);
        RomWriter w;
        romflash_begin_install(&w);
        lua_State *T = luaL_newstate();   // 只借它的字串雜湊種子
        char e[96];
        int r = luarom_build(T, sd_stream_read, NULL, &w, &rom_root, e, sizeof e);
        lua_close(T);
        if (r != 0) {
            sd_stream_close();
            platform_log(e);
            romflash_set_progress(NULL);
            video_restart();
            free(head);
            snprintf(info, info_size, "GAME.NVC: INSTALL FAILED");
            return 1;
        }
        romflash_build_done(w.used);
        char s[96];
        snprintf(s, sizeof s, "install: code built, %uK, %u s", (unsigned)(w.used / 1024),
                 (unsigned)((time_us_32() - t0) / 1000000));
        platform_log(s);
    }
    sd_stream_close();
    size_t code_used = romflash_used();

    // 告訴引擎：程式在唯讀區；圖片、字型、地圖、凍結的表格也接著放在那裡
    luarom_activate(romflash_base(), romflash_capacity());
    gfx_set_rom_store(romflash_store);
    luarom_set_append(romflash_append);
    script_set_loader(rom_loader, NULL);
    platform_log("engine_load start");
    *err = engine_load(head, head_len);   // 開頭原始碼：引擎只用來讀遊戲 id
    romflash_set_progress(NULL);
    {
        char s[96];
        snprintf(s, sizeof s, "engine_load done: err %d, rom used %uK, lua %uK, %u s", *err,
                 (unsigned)(romflash_used() / 1024), (unsigned)(script_mem() / 1024),
                 (unsigned)((time_us_32() - t0) / 1000000));
        platform_log(s);
    }
    free(head);

    // 載入成功才寫安裝紀錄（失敗的話下次重新安裝）
    if (!installed && *err == 0) {
        romflash_finish_install(&id, rom_root);
        platform_log("install record written");
    }
    romflash_lock();   // 開始玩之後不再寫 Flash（原因見 romflash.c）
    video_restart();   // 安裝完：畫面打開（已安裝的話本來就開著，什麼都不做）
    log_clocks("after load");
    {
        // 診斷：畫面真的有在跑嗎（等 0.1 秒，畫面數應該增加 6 左右）
        uint32_t f0 = video_frame_count(), i0 = video_irq_count;
        sleep_ms(100);
        char s[96];
        snprintf(s, sizeof s, "video after load: frames +%u, irq +%u in 0.1 s",
                 (unsigned)(video_frame_count() - f0), (unsigned)(video_irq_count - i0));
        platform_log(s);
    }

    snprintf(info, info_size, "CART %s ROM %uK+%uK WR %u ER %u FE %u %uS",
             installed ? "INSTALLED" : "NEW", (unsigned)(code_used / 1024),
             (unsigned)((romflash_used() - code_used) / 1024),
             romflash_pages_written(), romflash_erases(), romflash_errors(),
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
// 做法：在一般 RAM 準備一個陣列（大小見下面），把 CPU 的堆疊指標（MSP）指到它的尾端，再呼叫 nova_main。
// 為什麼是尾端：ARM 的堆疊是「往下長」的，從高位址往低位址用。
// 中斷也用 MSP，所以中斷之後也在這塊新堆疊上執行，不會出問題。

// 16 KB：2026-10-24 實測 Wildkin 最多只用到約 3 KB（stack free 29.6 KB／32 KB），
// 省下的 16 KB 給 Lua。用超過會被 MSPLIM 擋下來、當機紀錄會寫 STKOF，不會默默蓋到別的資料
uint32_t big_stack_words[16 * 1024 / 4] __attribute__((aligned(8)));
#define big_stack big_stack_words   // ARM 規定堆疊要 8 bytes 對齊

static void nova_main(void);

// 堆疊用了多少：開機時整塊填上一個特別的數字，之後從底部數「還沒被改過」的有幾個字
#define STACK_FILL 0x5A5A5A5Au
static void stack_fill(uint32_t *p, size_t words) { for (size_t i = 0; i < words; ++i) p[i] = STACK_FILL; }
static unsigned stack_free_bytes(const uint32_t *p, size_t words)
{
    size_t i = 0;
    while (i < words && p[i] == STACK_FILL) ++i;
    return (unsigned)(i * 4);
}
unsigned main_stack_free(void) { return stack_free_bytes(big_stack, count_of(big_stack)); }

static void __attribute__((noreturn)) run_on_big_stack(void)
{
    stack_fill(big_stack, count_of(big_stack));
    void *top = &big_stack[count_of(big_stack)];
    // 堆疊下限（MSPLIM，ARMv8-M 的功能）：堆疊用超過 big_stack 的底部，CPU 立刻出錯（CFSR 的 STKOF），
    // 而不是默默蓋掉下面的資料（2026-10-24：懷疑有東西被蓋掉，造成中斷向量表錯亂）
    __asm volatile ("msr msplim, %0" : : "r"(&big_stack[0]));
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
    // 把中斷向量表（在 RAM，SCB 的 VTOR 指著它）的第 3 格 HardFault 換成我們的。
    // 不用 SDK 的 exception_set_exclusive_handler：SD 卡函式庫已經放了自己的，SDK 會判斷「已經有人設定」而停機
    ((void (**)(void))scb_hw->vtor)[3] = nova_hardfault;
    __dsb();
    nova_clocks();
    compute_firmware_id();
    video_start();
    usb_pad_start();

    // SD 卡的驅動會向 SDK 借 DMA 通道；等 USB 先登記好它的通道 2，才不會搶到同一個
    absolute_time_t until = make_timeout_time_ms(1000);
    while (!usb_pad_ready() && !time_reached(until)) tight_loop_contents();

    // SD 卡先掛上：之後每一步都寫進 nova_log.txt，畫面出不來時也能在電腦上看到走到哪裡
    int sd_ok = sd_mount() == 0;
    sd_ready = sd_ok;
    if (sd_ok) {
        char s[96];
        snprintf(s, sizeof s, "==== NOVA BOOT (firmware %s) ====", FIRMWARE_ID);
        sd_log(s);
        snprintf(s, sizeof s, "video ok, usb pad %s, heap %uK free", usb_pad_ready() ? "ready" : "not ready",
                 heap_total() / 1024);
        sd_log(s);
        // 診斷：兩個核心的中斷向量表位址、第 41 格（核心間信箱中斷）裡放的是什麼
        snprintf(s, sizeof s, "vtor core0 %08X core1 %08X, slot41 %08X", (unsigned)scb_hw->vtor,
                 (unsigned)usb_core_vtor, (unsigned)((uint32_t *)scb_hw->vtor)[41]);
        sd_log(s);
        log_clocks("boot");
        protect_vector_table();
    }

    engine_init();
    // 載入時用固定的亂數種子：遊戲載入時如果用到 rnd（例如產生地圖），放進 Flash 的內容才會每次一樣
    // （2026-10-24 實機：用開機時間當種子，已安裝的遊戲每次開機還要重寫 1284 頁）。
    // 載入完才換成隨機的（下面的 script_reseed）
    engine_set_seed(12345);

    // 卡匣，依序找：
    //   SD 卡的 game.nvc（預先編譯好的，程式放 Flash：大遊戲用這個）
    //   SD 卡的 game.lua（原始碼，整個放 RAM：小遊戲）
    //   韌體內建的卡匣
    char cart_info[64] = "";
    int err;
    char *sd_src = NULL;
    size_t sd_len = 0;
    // 上次當機了：顯示紀錄，這次不載入 game.nvc
    bool hung = crash.magic == HANG_MAGIC;
    bool crashed = crash.magic == CRASH_MAGIC || hung;
    char crash_text[2][64];
    if (crashed) {
        crash.magic = 0;
        snprintf(crash_text[0], 64, "%s PC %08X LR %08X", hung ? "HANG" : "CRASH",
                 (unsigned)crash.pc, (unsigned)crash.lr);
        snprintf(crash_text[1], 64, "CORE %u R0 %d CFSR %08X MMFAR %08X",
                 (unsigned)crash.core, (int)crash.r0, (unsigned)crash.cfsr, (unsigned)crash.bfar);
        platform_log(crash_text[0]);
        platform_log(crash_text[1]);
        char t[64];
        snprintf(t, sizeof t, "SLOT %08X DIFF %u FIRST %d DMA %08X", (unsigned)crash.slot,
                 (unsigned)crash.ndiff, (int)crash.firstdiff, (unsigned)crash.dma);
        platform_log(t);
    }

    if (crashed) sd_ok = 0;   // 上次當機：這次不載入 SD 卡的遊戲（紀錄檔照寫）
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
    script_reseed(time_us_32());   // 載入完：之後的亂數每次開機不同
    unsigned heap_after_load = heap_used();
    platform_log(cart_info);

    bool show_stats = true;              // 開發中先預設顯示，之後改成預設關閉
    uint32_t combo_frames = 0;           // 「SELECT + L + R」按住了幾幀
    uint32_t last = video_frame_count();
    uint32_t frame_us = 0;

    platform_log("main loop start");
    uint64_t sum_update = 0, sum_draw = 0, sum_audio = 0;
    uint32_t logic_vb = 0, skipped_total = 0, skipped_last = 0;
    uint32_t sum_frames = 0;
    hang_watch = true;
    // （Lua 看門狗 script_set_watch 關掉：它讓 Lua 每 100 個指令就呼叫一次 C，會拖慢遊戲；
    //   卡住偵測改由畫面中斷負責，見 video.c）
    (void)frame_watch;
    engine_set_timer(time_us_32);   // 量 _update、_draw、聲音各花多久
    unsigned loops = 0;
    while (true) {
        if (loops < 5) {   // 診斷：前幾幀有沒有跑、花多久
            char s[128];
            snprintf(s, sizeof s, "frame loop %u: video frame %u, irq %u, resync %u, flash wr %u, last frame %u ms",
                     loops, (unsigned)video_frame_count(), (unsigned)video_irq_count, (unsigned)video_resyncs,
                     romflash_pages_written(), (unsigned)(frame_us / 1000));
            platform_log(s);
        }
        loops++;
        main_heartbeat++;
        // 等 HDMI 進入垂直空白期（每秒 60 次）
        while (video_frame_count() == last) __wfe();
        last = video_frame_count();

        // 1. 把上一幀畫好的畫面交給 HDMI。空白期約 1.4 ms，複製 76 KB 約 0.5 ms，來得及
        memcpy(video_display_buffer(), gfx_framebuffer(), SCREEN_W * SCREEN_H);
        video_set_palette(gfx_palette());
        bool overlay = err || script_failed() || show_stats || crashed;
        if (overlay) video_set_color(OVERLAY_COLOR, 0xFF);   // 白色

        // 2. 疊字：出錯時顯示錯誤訊息；按住 SELECT + L + R 一秒切換狀態顯示
        if (err || script_failed() || crashed) {
            overlay_text(4, 4, crashed ? "LAST BOOT CRASHED" : err ? "LOAD ERROR" : "LUA ERROR");
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

        // 3. 讀手把，跑一幀遊戲（_update、_draw）。
        // 畫一幀超過 16.6 ms 時，上一輪會錯過幾個空白期。錯過幾個，就先「只更新不畫」補幾次（最多 3 次），
        // 遊戲邏輯維持每秒 60 次：移動速度和網頁版一樣，只是畫面張數少一點（像早期遊戲機的「跳格」）
        uint32_t now_vb = video_frame_count();
        int32_t behind = (int32_t)(now_vb - logic_vb) - 1;   // 上一次跑遊戲之後，多過了幾個空白期
        if (logic_vb == 0 || behind < 0) behind = 0;            // （用有號數：剛好同一個空白期時是 -1）
        if (behind > 3) behind = 3;                // 落後太多就算了（例如載入時），不要一次補一大堆
        logic_vb = now_vb;
        read_input();
        uint32_t t0 = time_us_32();
        frame_start_us = t0;
        for (int32_t k = 0; k < behind; ++k) { engine_frame_nodraw(); skipped_total++; }
        engine_frame();
        frame_us = time_us_32() - t0;
        {   // 累計這 10 秒內各部分花的時間（紀錄檔的 alive 那一行）
            uint32_t u, d, a;
            engine_last_times(&u, &d, &a);
            sum_update += u; sum_draw += d; sum_audio += a; sum_frames++;
        }
        if (frame_us > 500000u && sd_ready) {
            char s[96];
            snprintf(s, sizeof s, "long frame done: %u ms, heap %uK, lua %uK", (unsigned)(frame_us / 1000),
                     heap_used() / 1024, (unsigned)(script_mem() / 1024));
            sd_log(s);
        }

        uint16_t held = input_get_buttons(0);
        uint16_t combo = (1u << BTN_SELECT) | (1u << BTN_L) | (1u << BTN_R);
        combo_frames = ((held & combo) == combo) ? combo_frames + 1 : 0;
        if (combo_frames == 60) show_stats = !show_stats;

        // USB 事件（手把插上、拔掉、看門狗）寫進紀錄檔：一幀最多 2 筆，不拖慢遊戲
        if (sd_ready) {
            char s[64];
            for (int k = 0; k < 2 && usb_pad_next_log(s, sizeof s); ++k) sd_log(s);
        }

        // 每 10 秒在紀錄檔寫一行：電腦上看得出主機還活著、跑得多快
        static uint32_t next_alive_ms = 10000;
        if (sd_ready && to_ms_since_boot(get_absolute_time()) >= next_alive_ms) {
            next_alive_ms += 10000;
            char s[160];
            unsigned nf = sum_frames ? sum_frames : 1;
            snprintf(s, sizeof s, "alive: %u fps drawn + %u skipped, avg ms: update %u.%u draw %u.%u audio %u.%u, stack %u/%u, heap %uK, lua %uK%s",
                     (unsigned)(sum_frames / 10), (unsigned)((skipped_total - skipped_last) / 10),
                     (unsigned)(sum_update / nf / 1000), (unsigned)(sum_update / nf % 1000 / 100),
                     (unsigned)(sum_draw / nf / 1000), (unsigned)(sum_draw / nf % 1000 / 100),
                     (unsigned)(sum_audio / nf / 1000), (unsigned)(sum_audio / nf % 1000 / 100),
                     main_stack_free(), usb_pad_stack_free(),
                     heap_used() / 1024, (unsigned)(script_mem() / 1024), script_failed() ? ", LUA ERROR" : "");
            sd_log(s);
            sum_update = sum_draw = sum_audio = 0;
            sum_frames = 0;
            skipped_last = skipped_total;
        }
    }
}
