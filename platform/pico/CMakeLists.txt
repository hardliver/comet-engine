# Nova One 主機韌體：Comet 引擎 + Pico 平台層
#
# 需要：
#   PICO_SDK_PATH      Pico SDK 2.x（套用 patches/tinyusb-windows-enum.patch，見 README）
#   PICO_PIO_USB_PATH  Pico-PIO-USB（github.com/sekigon-gonnoc/Pico-PIO-USB）
#   FATFS_SD_PATH      no-OS-FatFS-SD-SDIO-SPI-RPi-Pico（github.com/carlk3/…，SD 卡）
#
# 編譯：
#   mkdir build && cd build
#   cmake .. -DPICO_BOARD=pico2_w [-DCART=/path/to/game.lua]
#   make
# 產生 nova.uf2：按住 BOOTSEL 接上電腦，拖進 RP2350 磁碟

cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(nova C CXX ASM)

if (NOT PICO_PIO_USB_PATH)
    set(PICO_PIO_USB_PATH $ENV{PICO_PIO_USB_PATH})
endif()
# SD 卡：github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
if (NOT FATFS_SD_PATH)
    set(FATFS_SD_PATH $ENV{FATFS_SD_PATH})
endif()
pico_sdk_init()
add_subdirectory(${FATFS_SD_PATH}/src fatfs_sd)

set(COMET ${CMAKE_CURRENT_LIST_DIR}/../..)

# 要內嵌的卡匣（預設：引擎附的 move_box）
if (NOT CART)
    set(CART ${COMET}/games/move_box.lua)
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cart_data.c
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/embed_cart.py ${CART} ${CMAKE_CURRENT_BINARY_DIR}/cart_data.c
    DEPENDS ${CART} ${CMAKE_CURRENT_LIST_DIR}/embed_cart.py)

# 核心：和 Mac、網頁版同一份
file(GLOB CORE_SRC ${COMET}/core/*.c)
# Lua：不需要的檔案不編（lua.c、luac.c 是命令列程式；io、os、loadlib 會碰檔案系統，引擎也沒有開）
file(GLOB LUA_SRC ${COMET}/third_party/lua/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|liolib|loslib|loadlib|linit)\\.c$")

add_executable(nova
    main.c video.c usb_pad.c sd.c romflash.c xinput_host.c
    ${CORE_SRC} ${LUA_SRC}
    ${CMAKE_CURRENT_BINARY_DIR}/cart_data.c)

target_include_directories(nova PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # tusb_config.h
    ${COMET}/core
    ${COMET}/third_party/lua)

target_compile_definitions(nova PRIVATE
    LUA_USER_H="comet_lua_user.h"
    PICO_USE_MALLOC_MUTEX=1              # 兩個核心都可能 malloc
    COMET_GC_PAUSE=110                   # RAM 少：記憶體長到 1.1 倍就回收垃圾（Lua 預設 2 倍）。
                                         # 2026-10-24 Wildkin 量測（32 位元）：120 → 110 最高用量少約 20 KB，速度幾乎不變；100 會慢 3 倍
    COMET_EVAL_MAX=256                   # 主機沒有 Studio 除錯，結果緩衝不用 16 KB
    SYSTEM_MAX_GAMES=16)                 # 主選單之後做；先不用放 64 款遊戲的清單

target_compile_options(nova PRIVATE -Wall -Wextra)

target_link_libraries(nova
    pico_stdlib pico_multicore
    hardware_dma hardware_pll hardware_clocks hardware_flash pico_flash
    tinyusb_host tinyusb_board tinyusb_pico_pio_usb
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

pico_add_extra_outputs(nova)
