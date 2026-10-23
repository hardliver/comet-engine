// tusb_config.h — TinyUSB 的設定（Nova One 主機）
// 只當 USB 主機，接在 PIO-USB 做的第二個 USB 埠（GP10/GP11）。
// Pico 原本的 micro USB 保留給燒錄和供電。

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_RP2040   // RP2350 也用這個值
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0

#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1                // 主機埠是 PIO-USB
#define BOARD_TUH_RHPORT        1
#define CFG_TUSB_RHPORT1_MODE   OPT_MODE_HOST

#define CFG_TUH_ENUMERATION_BUFSIZE 256          // 要能放下 255 bytes 的設定描述元（模仿 Windows）
#define CFG_TUH_HUB             1                // 之後本機多人：接 USB Hub
#define CFG_TUH_DEVICE_MAX      4
#define CFG_TUH_HID             4                // Switch Pro（HID）、接收器的其他介面
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0

#define CFG_TUH_XINPUT          4                // 360 無線接收器有 4 個欄位

#endif
