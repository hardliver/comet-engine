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

// 兩條列緩衝（各 640 個 RGB332）：一條正在被 DMA 送出，另一條讓我們填下一列。
// aligned(4)：DMA 一次搬 4 bytes，起始位址要是 4 的倍數
static uint8_t linebuf[2][SCREEN_W * 2] __attribute__((aligned(4)));

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

// ---- DMA：兩個通道輪流（ping-pong），每完成一段就觸發中斷，準備下下段 ----

#define DMACH_PING 0
#define DMACH_PONG 1
static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

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

// __scratch_x：把中斷函式放進 RAM 的 scratch 區，不必從 Flash 讀程式，反應比較快
void __scratch_x("") dma_irq_handler(void)
{
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_BLANK_LINES) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
        // 空白期快結束時，先把畫面第 0 列換算好（這時兩條列緩衝都沒有在送）。
        // 不能太早：main.c 在空白期一開始才把新的一幀複製到 display（約 0.5 ms = 16 列）
        if (v_scanline == MODE_V_BLANK_LINES - 6) { convert_row = 0; irq_set_pending(convert_irq); }
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        // 第幾條輸出列 → 引擎畫面的第幾列（每列送兩次）
        int out_line = (int)v_scanline - MODE_V_BLANK_LINES;
        int src_y = out_line / 2;
        uint8_t *buf = linebuf[src_y & 1];
        // 提早一列換算：送第 src_y 列的時候，順便把第 src_y + 1 列換算到另一條緩衝。
        // 為什麼不在這裡才換算 src_y？DMA 送完上一段會「立刻」接著讀這一段，
        // 不會等中斷函式跑完，邊換算邊被讀就會出現殘影。
        // 另一條緩衝裝的是 src_y - 1，它的第二次也已經送完（剛剛觸發這次中斷的就是它），可以安全覆寫。
        if ((out_line & 1) == 0 && src_y + 1 < SCREEN_H) {
            convert_row = src_y + 1;
            irq_set_pending(convert_irq);   // 中斷結束後，軟體中斷才會跑
        }
        ch->read_addr = (uintptr_t)buf;
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        if (v_scanline == 0) frame_count++;   // 進入垂直空白期
    }
}

static void __scratch_x("") convert_irq_handler(void)
{
    int y = convert_row;
    convert_line(linebuf[y & 1], y);
}

void video_start(void)
{
    memset(display, 0, sizeof(display));
    memset(lut, 0, sizeof(lut));

    // TMDS 編碼器：一個 byte = RRRGGGBB（紅 3 位元、綠 3 位元、藍 2 位元）
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS | 5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

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
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);   // 數字越小越優先
    irq_set_enabled(DMA_IRQ_0, true);

    convert_irq = (uint)user_irq_claim_unused(true);
    irq_set_exclusive_handler(convert_irq, convert_irq_handler);
    irq_set_priority(convert_irq, PICO_DEFAULT_IRQ_PRIORITY);  // 比 DMA 低：DMA 隨時可以插隊
    irq_set_enabled(convert_irq, true);
    // DMA 在匯流排上最優先：Lua 大量讀寫記憶體時，HDMI 的資料也不能晚到（晚到畫面就會錯亂）
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);
}

// ---- 寫 Flash 時暫停畫面 ----
// 為什麼需要：寫 Flash 時（romflash.c 的 flash_safe_execute）這個核心的中斷全部關掉，
// 擦一個磁區要約 45 ms。這段時間 DMA 送完一段，中斷沒辦法幫它換下一段的位址，
// DMA 就從上一段的結尾「繼續往後讀」：每一列往後跑 2.5 KB，幾十毫秒就跑出 RAM 的範圍，
// 讀到不存在的位址 → DMA 出錯停下來，之後中斷恢復也救不回來，HDMI 永遠沒有訊號。
// （電腦上的 Flash 模擬器沒有 HDMI，所以測不出來；2026-10-24 實機發現）
// 做法：寫之前把兩個 DMA 通道停掉，寫完從垂直空白期的開頭重新開始送。
// 電視會短暫失去同步（閃一下或沒訊號），但寫完一定會恢復。

static void stop_channel(uint ch)
{
    // 先關掉「完成後觸發另一個通道」：不然停了這個，另一個又把它叫起來
    hw_clear_bits(&dma_hw->ch[ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
}

void video_pause(void)
{
    irq_set_enabled(DMA_IRQ_0, false);
    stop_channel(DMACH_PING);
    stop_channel(DMACH_PONG);
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);   // 清掉還沒處理的中斷
}

void video_resume(void)
{
    // HSTX 可能停在一個指令的中間：關掉再打開，從乾淨的狀態開始
    hw_clear_bits(&hstx_ctrl_hw->csr, HSTX_CTRL_CSR_EN_BITS);
    hw_set_bits(&hstx_ctrl_hw->csr, HSTX_CTRL_CSR_EN_BITS);

    // 和 video_start 一樣的起點：兩個通道都先送空白期的列，掃描線從 2 開始算
    dma_pong = false;
    v_scanline = 2;
    vactive_cmdlist_posted = false;
    for (uint ch = DMACH_PING; ch <= DMACH_PONG; ++ch) {
        dma_hw->ch[ch].read_addr = (uintptr_t)vblank_line_vsync_off;
        dma_hw->ch[ch].transfer_count = count_of(vblank_line_vsync_off);
        hw_set_bits(&dma_hw->ch[ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(DMACH_PING);
}
