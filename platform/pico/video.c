// video.c — Pico 的畫面輸出（HSTX → HDMI，DVI 640×480 @ 60 Hz）
//
// 以 Pico SDK 的範例 hstx/dvi_out_hstx_encoder 為基礎，改了兩件事：
//   1. 不用 640×480 的大陣列（300 KB，Lua 會沒有記憶體可用），
//      改成每一列臨時換算：320 個調色盤編號 → 640 個 RGB332 像素（左右各重複一次）。
//      上下放大 2 倍：同一列連續送兩次。只需要兩條 640 bytes 的「列緩衝」。
//   2. 引擎一邊畫、HDMI 一邊掃描會撕裂（畫到一半被送出去）。
//      所以 HDMI 讀的是另一份「顯示用畫面」，main.c 在垂直空白期把引擎畫好的那一份複製過來。
//
// 接線（實測，2026-10-23）：
//   GP12 D0+  GP13 D0-   GP14 CK+  GP15 CK-   GP16 D2+  GP17 D2-   GP18 D1+  GP19 D1-
//   各串 220 Ω。clk_hstx 必須是 126 MHz（252 Mbps = 標準 640×480 @ 60 Hz）。

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/bus_ctrl.h"

#include "video.h"
#include "../../core/gfx.h"   // SCREEN_W、SCREEN_H

// ---- 畫面資料 ----

// HDMI 讀的那一份（320×240 調色盤編號）
static uint8_t display[SCREEN_W * SCREEN_H] __attribute__((aligned(4)));

// 調色盤編號 → RGB332。HSTX 的設定（下面 expand_tmds）讓一個 byte 代表 RRRGGGBB
static uint8_t lut[256];

// 兩條列緩衝：一條正在被 DMA 送出，另一條讓我們填下一列。
// 每條 = 這一列開頭的 HSTX 指令（遮沒期、同步訊號，9 個字）+ 640 個 RGB332 像素（160 個字）。
// 指令和像素放在一起，一列只要「一段」DMA。
// 為什麼（2026-10-24 實機紀錄）：本來指令、像素分兩段送，指令那段只有 9 個字，
// 中斷要在 1–2 微秒內幫下一段換好位址。Wildkin 這種大遊戲一直從 Flash 讀程式，
// CPU 等 Flash 的時候中斷進不來，常常超過 2 微秒 → 兩個 DMA 通道對不上（每秒上百次）。
// 合成一段後，中斷有一整列（約 30 微秒）的時間可以處理。
#define LINE_CMD_WORDS 9
#define LINE_PIX_WORDS (SCREEN_W * 2 / 4)
static uint32_t linebuf[2][LINE_CMD_WORDS + LINE_PIX_WORDS];
static inline uint8_t *line_pixels(int i) { return (uint8_t *)&linebuf[i][LINE_CMD_WORDS]; }

static volatile uint32_t frame_count;

// 換算工作交給另一個「軟體中斷」：DMA 中斷只負責換位址（幾十個週期就結束），
// 要換算的列號寫在這裡，再觸發軟體中斷。軟體中斷的優先權比 DMA 中斷低，可以被它打斷。
// 2026-10-23 第二版：換算直接寫在 DMA 中斷裡，中斷一長，兩個 DMA 通道的中斷擠在一起，
// 改到正在送資料的通道 → 畫面左右錯開、出現重影。
static uint convert_irq;           // 軟體中斷的編號（向 SDK 借一個沒用到的）
static volatile int convert_row;   // 下一個要換算的列

uint8_t *video_display_buffer(void) { return display; }
uint32_t video_frame_count(void) { return frame_count; }

void video_set_palette(const uint8_t *rgb)
{
    for (int i = 0; i < 256; ++i) {
        uint8_t r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        lut[i] = (uint8_t)((r & 0xe0) | ((g & 0xe0) >> 3) | (b >> 6));
    }
}

void video_set_color(int index, uint8_t rgb332)
{
    lut[index & 255] = rgb332;
}

// ---- DVI 的時序與控制符號（照範例） ----

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640
#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480
#define MODE_V_BLANK_LINES   (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH)
#define MODE_V_TOTAL_LINES   (MODE_V_BLANK_LINES + MODE_V_ACTIVE_LINES)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V1_H1,
    HSTX_CMD_NOP
};
static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V0_H1,
    HSTX_CMD_NOP
};
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

// 垂直遮沒期（45 列）也整段送：分成 A（第 0–38 列，含垂直同步）和 B（第 39–44 列）兩段。
// 為什麼不一列一段：一列遮沒期只有 7 個字，DMA 幾微秒就塞進 HSTX 的 FIFO，
// 中斷又要在幾微秒內換好下一段（2026-10-24 實機：Wildkin 跑起來每秒還是會對不上幾次）。
// 分兩段是因為 B 開始送的時候要換算畫面第 0 列（和以前在第 39 列換算一樣）
#define VBLANK_A_LINES (MODE_V_BLANK_LINES - 6)
#define VBLANK_LINE_WORDS 7
static uint32_t vblank_a[VBLANK_A_LINES * VBLANK_LINE_WORDS];
static uint32_t vblank_b[6 * VBLANK_LINE_WORDS];

static void build_vblank(void)
{
    for (int line = 0; line < MODE_V_BLANK_LINES; ++line) {
        bool vsync = line >= MODE_V_FRONT_PORCH && line < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH;
        const uint32_t *src = vsync ? vblank_line_vsync_on : vblank_line_vsync_off;
        uint32_t *dst = line < VBLANK_A_LINES ? &vblank_a[line * VBLANK_LINE_WORDS]
                                              : &vblank_b[(line - VBLANK_A_LINES) * VBLANK_LINE_WORDS];
        memcpy(dst, src, VBLANK_LINE_WORDS * sizeof(uint32_t));
    }
}

// ---- DMA：兩個通道輪流（ping-pong），每完成一段就觸發中斷，準備下下段 ----

#define DMACH_PING 0
#define DMACH_PONG 1
static bool dma_pong = false;
static uint v_scanline = 2;

// 把引擎畫面的第 y 列換成 640 個 RGB332，放進 dst（每個來源像素寫兩次 = 左右放大）。
// 這段在中斷裡跑，一列只有 32 µs，越快越好：
//   一次讀 4 個來源像素（一個 32 位元字），查表後組成 2 個 32 位元字寫出去，
//   記憶體存取次數是一個一個處理的 1/4（2026-10-23 第一版一次一個像素，太慢，畫面會錯亂）。
// display、linebuf 都是 4 bytes 對齊，SCREEN_W 是 4 的倍數，所以可以用 uint32_t 讀寫
static void __scratch_x("") convert_line(uint8_t *dst, int y)
{
    const uint32_t *src = (const uint32_t *)&display[y * SCREEN_W];
    uint32_t *out = (uint32_t *)dst;
    for (int i = 0; i < SCREEN_W / 4; ++i) {
        uint32_t s = src[i];   // 4 個調色盤編號，最低的 byte 是最左邊的像素
        uint32_t c0 = lut[s & 0xff], c1 = lut[(s >> 8) & 0xff];
        uint32_t c2 = lut[(s >> 16) & 0xff], c3 = lut[s >> 24];
        out[i * 2]     = c0 | (c0 << 8) | (c1 << 16) | (c1 << 24);
        out[i * 2 + 1] = c2 | (c2 << 8) | (c3 << 16) | (c3 << 24);
    }
}

// 讓軟體中斷「待處理」。不用 SDK 的 irq_set_pending：它的程式放在 Flash，
// 寫 Flash 的時候畫面中斷還要繼續跑（見 romflash.c），那時候從 Flash 讀程式會當機。
// 這裡直接寫 NVIC 的「設定待處理」暫存器（每個 bit 是一個中斷），而且是 inline，程式就在呼叫的函式裡
static inline void pend_convert(void)
{
    nvic_hw->ispr[convert_irq / 32] = 1u << (convert_irq % 32);
}

// __scratch_x：把中斷函式放進 RAM 的 scratch 區，不必從 Flash 讀程式，反應比較快
volatile uint32_t video_irq_count;   // 診斷：DMA 中斷跑了幾次（正常每秒約 3.1 萬次）

volatile uint32_t video_resyncs;     // 診斷：DMA 對不上、重新同步了幾次

static void resync(void);

// ---- 卡住偵測（診斷）----
// 畫面中斷每秒跑幾萬次，順便記下「被打斷的時候主程式執行到哪裡」（PC、LR）。
// 主程式超過 5 秒沒有進到下一幀，就把最後記到的位置存成當機紀錄、重新開機（main.c 的 hang_detected）
volatile uint32_t video_sample_pc, video_sample_lr;
volatile uint32_t main_heartbeat;     // main.c 每跑一幀加 1
volatile bool hang_watch;             // main.c 開始跑遊戲後才打開
static uint32_t hb_last, hb_still;
extern void hang_detected(uint32_t pc, uint32_t lr);

void __scratch_x("") dma_irq_handler(void);

// 中斷的入口：先找出被打斷的程式存在堆疊上的暫存器（r0–r3、r12、lr、pc、xpsr），記下 PC、LR，
// 再跳到真正的處理函式。naked：編譯器不加任何進出的程式
void __attribute__((naked)) __scratch_x("") dma_irq_entry(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "ldr r1, [r0, #24]\n"        // 被打斷的 PC
        "ldr r2, =video_sample_pc\n"
        "str r1, [r2]\n"
        "ldr r1, [r0, #20]\n"        // 被打斷的 LR
        "ldr r2, =video_sample_lr\n"
        "str r1, [r2]\n"
        "b dma_irq_handler\n"
        ".ltorg\n");
}

void __scratch_x("") dma_irq_handler(void)
{
    video_irq_count++;
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    // 檢查兩個通道的順序還對不對：照理說每次只有「該輪到的那個」送完。
    // 如果兩個都送完了（中斷晚到，錯過一次），或送完的不是該輪到的那個，
    // 就代表已經對不上了：繼續下去會一直錯（2026-10-24 實測：畫面數變 5 倍、CPU 被中斷吃光）。
    // 這時整個停掉、從畫面開頭重新開始，電視最多閃一下
    uint32_t done = dma_hw->ints0 & ((1u << DMACH_PING) | (1u << DMACH_PONG));
    if (done != (1u << ch_num)) {
        resync();
        return;
    }
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    // v_scanline：現在要排的是第幾列（0 = 遮沒期開頭）
    if (v_scanline == 0) {
        ch->read_addr = (uintptr_t)vblank_a;
        ch->transfer_count = count_of(vblank_a);
        v_scanline = VBLANK_A_LINES;
        frame_count++;   // 進入垂直空白期：main.c 這時把新的一幀複製到 display
        if (hang_watch) {
            if (main_heartbeat != hb_last) { hb_last = main_heartbeat; hb_still = 0; }
            else if (++hb_still >= 300) hang_detected(video_sample_pc, video_sample_lr);   // 5 秒沒動
        }
    } else if (v_scanline == VBLANK_A_LINES) {
        ch->read_addr = (uintptr_t)vblank_b;
        ch->transfer_count = count_of(vblank_b);
        v_scanline = MODE_V_BLANK_LINES;
        // 遮沒期快結束，先把畫面第 0 列換算好（main.c 複製 display 約 0.5 ms，這時已經複製完）
        convert_row = 0;
        pend_convert();
    } else {
        // 第幾條輸出列 → 引擎畫面的第幾列（每列送兩次）
        int out_line = (int)v_scanline - MODE_V_BLANK_LINES;
        int src_y = out_line / 2;
        // 換算下一列的時機：現在要排的是第 out_line 列；剛送完的是 out_line - 2，
        // 另一個通道正在送 out_line - 1。
        // 排「第二次」（奇數列）的時候，另一條緩衝的兩次（src_y - 1）都已經送完，可以換算 src_y + 1 進去，
        // 而且離它要被送出還有將近兩列的時間
        if ((out_line & 1) == 1 && src_y + 1 < SCREEN_H) {
            convert_row = src_y + 1;
            pend_convert();   // 中斷結束後，軟體中斷才會跑
        }
        ch->read_addr = (uintptr_t)linebuf[src_y & 1];
        ch->transfer_count = LINE_CMD_WORDS + LINE_PIX_WORDS;
        v_scanline++;
        if (v_scanline == MODE_V_TOTAL_LINES) v_scanline = 0;
    }
}

static void __scratch_x("") convert_irq_handler(void)
{
    int y = convert_row;
    convert_line(line_pixels(y & 1), y);
}

static bool running;   // 畫面有沒有在送（video_stop／video_restart）

// HSTX 開始輸出：先整個關掉（清掉內部狀態），再用固定的設定打開
static void hstx_enable(void)
{
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS | 5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;
}

// 兩個 DMA 通道的完整設定（每次都整個重寫，不依賴之前的狀態），以及從頭開始的掃描狀態
static void configure_channels(void)
{
    // 起點：PING 送遮沒期 A、PONG 送遮沒期 B，接著要排的是畫面第 0 列（先換算好）
    dma_pong = false;
    v_scanline = MODE_V_BLANK_LINES;
    convert_line(line_pixels(0), 0);
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_a, count_of(vblank_a), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_b, count_of(vblank_b), false);
}


void video_start(void)
{
    memset(display, 0, sizeof(display));
    memset(lut, 0, sizeof(lut));
    build_vblank();
    // 兩條列緩衝的開頭放好每一列都一樣的指令（之後只改後面的像素）
    _Static_assert(count_of(vactive_line) == LINE_CMD_WORDS, "LINE_CMD_WORDS 要等於 vactive_line 的長度");
    for (int i = 0; i < 2; ++i) {
        memcpy(linebuf[i], vactive_line, sizeof vactive_line);
        memset(line_pixels(i), 0, LINE_PIX_WORDS * 4);
    }

    // TMDS 編碼器：一個 byte = RRRGGGBB（紅 3 位元、綠 3 位元、藍 2 位元）
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_enable();

    // 時脈那一對在 GP14/15，三組資料在 12/13、16/17、18/19（第二支腳是反相）
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        uint32_t sel = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB | (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit]     = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }
    for (int i = 12; i <= 19; ++i) gpio_set_function(i, 0);   // 功能 0 = HSTX

    // DMA 0、1 給畫面用（PIO-USB 用 2，見 usb_pad.c）
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);
    configure_channels();

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_entry);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);   // 數字越小越優先
    irq_set_enabled(DMA_IRQ_0, true);

    convert_irq = (uint)user_irq_claim_unused(true);
    irq_set_exclusive_handler(convert_irq, convert_irq_handler);
    irq_set_priority(convert_irq, PICO_DEFAULT_IRQ_PRIORITY);  // 比 DMA 低：DMA 隨時可以插隊
    irq_set_enabled(convert_irq, true);
    // DMA 在匯流排上最優先：Lua 大量讀寫記憶體時，HDMI 的資料也不能晚到（晚到畫面就會錯亂）
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    running = true;
    dma_channel_start(DMACH_PING);
}

uint video_convert_irq(void) { return convert_irq; }

// ---- 寫 Flash 時把畫面整個停掉 ----
// 2026-10-24 實機紀錄（nova_log.txt）：寫 Flash 時讓畫面中斷繼續跑，結果畫面的 DMA 亂掉，
// 每秒送出 310 個畫面（正常 60），中斷多了 5 倍，CPU 幾乎全被中斷吃掉：安裝慢 200 倍、電視收不到訊號。
// 原因推測：寫 Flash 期間中斷晚了幾微秒，兩個輪流的 DMA 通道就對不上（中斷函式改到正在送的那個），
// 之後一直錯下去。所以最穩的做法：寫 Flash 之前整個停掉，寫完從頭開始（和開機一樣乾淨的狀態）。

bool video_running(void) { return running; }

void video_stop(void)
{
    if (!running) return;
    irq_set_enabled(DMA_IRQ_0, false);
    // 兩個通道的控制暫存器整個清成 0（關掉、也不再串接），再中止正在送的
    dma_hw->ch[DMACH_PING].al1_ctrl = 0;
    dma_hw->ch[DMACH_PONG].al1_ctrl = 0;
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);   // 清掉還沒處理的中斷
    hstx_ctrl_hw->csr = 0;                                        // HSTX 也停
    running = false;
}

// 中斷函式裡呼叫：停掉再從頭開始（和 video_stop + video_restart 一樣，但不動中斷的開關）
static void resync(void)
{
    video_resyncs++;
    dma_hw->ch[DMACH_PING].al1_ctrl = 0;
    dma_hw->ch[DMACH_PONG].al1_ctrl = 0;
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    hstx_enable();
    configure_channels();
    dma_channel_start(DMACH_PING);
}

void video_restart(void)
{
    if (running) return;
    // 和開機一樣：HSTX 重新打開、兩個通道整個重新設定、掃描從頭開始
    hstx_enable();
    configure_channels();
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_enabled(DMA_IRQ_0, true);
    running = true;
    dma_channel_start(DMACH_PING);
}
