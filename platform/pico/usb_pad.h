// usb_pad.h — Pico 的 USB 手把（PIO-USB 主機，跑在第二個核心）
//
// 接線：GP10 = D+、GP11 = D-，兩條各接 4.7 kΩ 到 GND（RP2350 Erratum E9，見 01 規格）。
// 支援：XInput（Xbox 360 有線協定，含沛影 Specter 的 2.4G 接收器）、Switch Pro（Specter 有線模式）。
// 按鍵一律依「位置」對應到 Xbox 配置：下面的鍵 = A。

#ifndef USB_PAD_H
#define USB_PAD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     connected;
    uint16_t buttons;      // XInput 的按鍵位元（見 xinput_host.h 的 XINPUT_GAMEPAD_*）
    uint8_t  lt, rt;       // 扳機 0–255
    int16_t  lx, ly;       // 左搖桿，-32768–32767，上 = 正
    int16_t  rx, ry;       // 右搖桿
} PadState;

// 在第二個核心啟動 USB 主機。呼叫前 clk_sys 必須是 12 MHz 的倍數（144 MHz）
void usb_pad_start(void);

// USB 主機準備好了沒（第二個核心初始化完成，DMA 通道也已經登記）。
// 其他也要用 DMA 的東西（SD 卡）要等它，才不會搶到同一個通道
bool usb_pad_ready(void);

// 讀目前的手把狀態（第一支手把）
void usb_pad_get(PadState *out);

// 震動：strong = 大馬達、weak = 小馬達，0–255。不支援震動的手把就不做事
void usb_pad_rumble(uint8_t strong, uint8_t weak);

#endif
