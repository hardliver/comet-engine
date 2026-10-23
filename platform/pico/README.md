# platform/pico — Nova One 主機（Pico 2 W）

和 `platform/mac`、`platform/web` 一樣，是 Comet 核心的平台層。核心（`core/`）完全不用改。

| 檔案 | 做什麼 |
|---|---|
| `main.c` | 時脈、`core/platform.h` 的函式、60 fps 主迴圈、手把 → Comet 按鍵、錯誤與狀態疊字 |
| `video.c` | HDMI（HSTX，DVI 640×480 @ 60 Hz）。每一列送出前才把 320 個調色盤編號換成 RGB332 並左右放大，不需要 640×480 的大陣列 |
| `usb_pad.c` | USB 手把（PIO-USB 主機，第二個核心）：XInput、Switch Pro、副廠 2.4G 接收器 |
| `sd.c` | microSD（SPI0）：開機時讀根目錄的 `game.lua` |
| `xinput_host.c/h` | XInput 驅動（Ryzee119/tusb_xinput，MIT，改成相容新版 TinyUSB） |
| `embed_cart.py` | 把一個卡匣編進韌體（SD 卡沒有 `game.lua` 時用） |
| `patches/` | TinyUSB 的補丁：列舉時模仿 Windows（沛影 Specter 接收器需要） |

## 接線（2026-10-23 實測）

| 用途 | Pico 腳位 |
|---|---|
| HDMI | GP12–19（各串 220–270 Ω），地線 → GND，HDMI 腳 18 → VBUS |
| USB 母座 | GP10 = D+、GP11 = D−、VBUS、GND；**D+、D− 各加 4.7 kΩ 到 GND**（RP2350 Erratum E9，沒有的話拔插偵測不到） |
| microSD 模組 | GP2 SCK、GP3 MOSI、GP4 MISO、GP5 CS、VCC → VBUS、GND |

## 時脈

| 來源 | 頻率 | 給誰 |
|---|---|---|
| pll_sys | 144 MHz | CPU、周邊（SPI、UART）、PIO-USB（要 12 MHz 的倍數）；÷3 = 48 MHz 給 micro USB |
| pll_usb | 126 MHz | 只給 HSTX（252 Mbps = 標準 640×480 @ 60 Hz） |

注意：`set_sys_clock_khz` 會把周邊時脈接到 pll_usb；改 pll_usb 之前一定要先把周邊時脈改接 pll_sys，否則 SPI 速度會算錯（SD 卡讀不到）。

## 編譯

需要：Pico SDK 2.x（含 TinyUSB）、[Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)、[no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)、`arm-none-eabi-gcc`。

```
cd $PICO_SDK_PATH/lib/tinyusb && git apply /path/to/comet/platform/pico/patches/tinyusb-windows-enum.patch
export PICO_PIO_USB_PATH=… FATFS_SD_PATH=…
mkdir build && cd build
cmake /path/to/comet/platform/pico -DPICO_BOARD=pico2_w [-DCART=遊戲.lua]
make
```

產生 `nova.uf2`：按住 BOOTSEL 接上電腦，拖進 RP2350 磁碟。

## 使用

- SD 卡（FAT32）根目錄放 `game.lua`；沒有就跑內建卡匣
- 2.4G 接收器：**先插接收器、再開手把**
- 按住 SELECT + L + R 一秒：切換狀態列（記憶體、每幀毫秒、手把、卡匣來源）

## 還沒做

存檔（`platform_save_*` 目前回報失敗）、現實時鐘、聲音、資源包（pak）、主選單選遊戲。
大卡匣（Wildkin 編譯後約 1.4 MB）放不進 520 KB RAM，記憶體方案待決定。
