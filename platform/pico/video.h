// video.h — Pico 的畫面輸出（HSTX → HDMI，DVI 640×480 @ 60 Hz）
//
// 引擎畫的是 320×240、每個像素一個調色盤編號（gfx_framebuffer）。
// 這裡在 HDMI 掃描到每一列之前，把那一列換成 RGB332 並左右放大 2 倍，
// 上下放大 2 倍則是同一列送兩次。所以不需要 640×480 的大畫面陣列。

#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

// 啟動 HSTX 與 DMA。呼叫前 clk_hstx 必須是 126 MHz（見 main.c 的 nova_clocks）
void video_start(void);

// 每次進入垂直空白期就加 1（每秒 60 次）
uint32_t video_frame_count(void);

// 送到 HDMI 的畫面（320×240 調色盤編號）。只在垂直空白期間更新，畫面才不會撕裂
uint8_t *video_display_buffer(void);

// 由 RGB 調色盤（256 × 3 bytes）更新顏色對照表
void video_set_palette(const uint8_t *rgb);

// 直接指定某個編號送到 HDMI 的顏色（RRRGGGBB）。疊字用，不改遊戲的調色盤
void video_set_color(int index, uint8_t rgb332);

// 畫面用到的兩個中斷：DMA_IRQ_0 和這個軟體中斷。寫 Flash 時只留這兩個（romflash.c），
// 它們的程式都在 RAM，Flash 暫停時也能跑，所以畫面不會斷
#include "pico/types.h"
uint video_convert_irq(void);

#endif
