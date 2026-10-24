// usb_pad.c — Pico 的 USB 手把（PIO-USB 主機，跑在第二個核心）
//
// 為什麼放在第二個核心：PIO-USB 用軟體即時解碼 USB 訊號，CPU 要隨時跟上每個位元。
// 遊戲（Lua）和畫面中斷都在第一個核心，互不干擾。
//
// 2026-10-23 實測出來、這個檔案必須處理的事：
//   1. 沛影 Specter 的 2.4G 接收器：一插上是「1A34:F517 Receiver Update」空殼，
//      手把開機連上後才斷線、重新以「413D:2131」XInput 身分出現。
//      要先插接收器、再開手把；還要像 Windows 一樣列舉（TinyUSB 補丁，見 patches/）
//      並在連上後送 Windows 會送的幾個請求（下面的 windows_probe）。
//   2. Specter 有線模式是 Switch Pro（057E:2009）：主機要先送一串握手，它才會持續送按鍵。
//   3. 拔插偵測靠 D+/D- 的外部 4.7 kΩ 下拉電阻（RP2350 Erratum E9）。

#include <math.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/structs/scb.h"
#include "tusb.h"
#include "pio_usb.h"
#include "xinput_host.h"
#include "usb_pad.h"

// ============================================================================
// 兩個核心共用的狀態
// ============================================================================
// volatile：告訴編譯器「別的核心會改它」，每次都要真的去記憶體讀，不能用暫存的舊值。
// 這裡沒有用鎖：最壞的情況是讀到「半新半舊」的一幀按鍵，下一幀就對了，對遊戲沒影響。

static volatile PadState pad;
static volatile uint8_t want_strong, want_weak;   // 遊戲要求的震動（第一個核心寫）
static volatile bool ready;

bool usb_pad_ready(void) { return ready; }

void usb_pad_get(PadState *out)
{
    out->connected = pad.connected;
    out->buttons = pad.buttons;
    out->lt = pad.lt;  out->rt = pad.rt;
    out->lx = pad.lx;  out->ly = pad.ly;
    out->rx = pad.rx;  out->ry = pad.ry;
}

void usb_pad_rumble(uint8_t strong, uint8_t weak)
{
    want_strong = strong;
    want_weak = weak;
}

static void pad_clear(void)
{
    pad.connected = false;
    pad.buttons = 0;
    pad.lt = pad.rt = 0;
    pad.lx = pad.ly = pad.rx = pad.ry = 0;
}

// ============================================================================
// 模仿 Windows：連上後要 0xEE 字串描述元，有回應就再要 Compat ID、BOS、Device Qualifier
// （Specter 接收器在這組請求之下，手把開機後會切換成 XInput）
// ============================================================================

static uint8_t ee_buf[64], ms_buf[40], bos_buf[16], qual_buf[10];
static tusb_control_request_t ms_req;   // static：傳輸進行中這塊記憶體必須一直存在

static void qual_done(tuh_xfer_t *x) { (void)x; }

static void bos_done(tuh_xfer_t *x)
{
    tuh_descriptor_get(x->daddr, TUSB_DESC_DEVICE_QUALIFIER, 0, qual_buf, sizeof(qual_buf), qual_done, 0);
}

static void ms_done(tuh_xfer_t *x)
{
    tuh_descriptor_get(x->daddr, TUSB_DESC_BOS, 0, bos_buf, sizeof(bos_buf), bos_done, 0);
}

static void ee_done(tuh_xfer_t *xfer)
{
    bool ok = xfer->result == XFER_RESULT_SUCCESS;
    // 0xEE 字串的第 16 個 byte 是廠商代碼，Windows 用它再問一次「你相容哪種驅動」
    if (ok && ee_buf[0] >= 18 && ee_buf[1] == TUSB_DESC_STRING) {
        ms_req = (tusb_control_request_t){
            .bmRequestType_bit = { .recipient = TUSB_REQ_RCPT_DEVICE,
                                   .type = TUSB_REQ_TYPE_VENDOR,
                                   .direction = TUSB_DIR_IN },
            .bRequest = ee_buf[16],
            .wValue = 0,
            .wIndex = 0x0004,   // Extended Compat ID
            .wLength = sizeof(ms_buf),
        };
        tuh_xfer_t x = {
            .daddr = xfer->daddr, .ep_addr = 0, .setup = &ms_req,
            .buffer = ms_buf, .complete_cb = ms_done, .user_data = 0,
        };
        tuh_control_xfer(&x);
    } else {
        tuh_descriptor_get(xfer->daddr, TUSB_DESC_BOS, 0, bos_buf, sizeof(bos_buf), bos_done, 0);
    }
}

static void windows_probe(uint8_t dev_addr)
{
    tuh_descriptor_get_string(dev_addr, 0xEE, 0x0000, ee_buf, sizeof(ee_buf), ee_done, 0);
}

// ============================================================================
// Switch Pro（有線）
// ============================================================================
// 握手順序（同 Linux 的 hid-nintendo）：
//   80 02 握手、80 03 高速、80 02 握手、80 04 不逾時、子指令 03 30（完整按鍵報告）、子指令 30 01（1P 燈）

static volatile bool sp_active;
static uint8_t  sp_dev, sp_idx, sp_step, sp_counter;
static volatile bool sp_got_reply;
static uint32_t sp_sent_ms;
static uint8_t  sp_buf[63];

static const uint8_t SP_RUMBLE_OFF[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };

// HD 震動的編碼（dekuNukem 的 Switch 逆向筆記）：頻率和強度各自換成對數刻度。
// 一個馬達 4 bytes：高頻（320 Hz）和低頻（160 Hz）各一組。
static void sp_rumble_block(uint8_t out[4], float amp)
{
    if (amp <= 0.0f) { memcpy(out, SP_RUMBLE_OFF, 4); return; }
    if (amp > 1.0f) amp = 1.0f;
    int ehf = (int)lroundf(log2f(320.0f / 10.0f) * 32.0f);
    int elf = (int)lroundf(log2f(160.0f / 10.0f) * 32.0f);
    uint16_t hf = (uint16_t)((ehf - 0x60) * 4);
    uint8_t  lf = (uint8_t)(elf - 0x40);
    int ea;
    if (amp > 0.23f)      ea = (int)lroundf(log2f(amp * 8.7f) * 32.0f);
    else if (amp > 0.12f) ea = (int)lroundf(log2f(amp * 17.0f) * 16.0f);
    else                  ea = (int)lroundf(((log2f(amp) * 32.0f) - 96.0f) / (5.0f - amp * amp) - 1.0f);
    if (ea < 0) ea = 0;
    uint8_t  hf_amp = (uint8_t)(ea * 2);
    uint16_t lf_amp = (uint16_t)(ea / 2 + 64);
    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(hf_amp + ((hf >> 8) & 0xFF));
    out[2] = (uint8_t)(lf + ((lf_amp >> 8) & 0xFF));
    out[3] = (uint8_t)(lf_amp & 0xFF);
}

// Switch Pro 的報告一律補到 64 bytes（含 report id）；有些副廠手把收到短封包會不理
static bool sp_send(uint8_t report_id, const uint8_t *data, uint8_t len)
{
    memset(sp_buf, 0, sizeof(sp_buf));
    memcpy(sp_buf, data, len);
    return tuh_hid_send_report(sp_dev, sp_idx, report_id, sp_buf, sizeof(sp_buf));
}

static bool sp_subcmd(uint8_t cmd, uint8_t arg)
{
    uint8_t d[11];
    d[0] = (sp_counter++) & 0x0F;
    memcpy(&d[1], SP_RUMBLE_OFF, 8);
    d[9] = cmd;
    d[10] = arg;
    return sp_send(0x01, d, sizeof(d));
}

static bool sp_rumble(uint8_t strong, uint8_t weak)
{
    uint8_t d[9];
    d[0] = (sp_counter++) & 0x0F;
    float a = (strong > weak ? strong : weak) / 255.0f;
    sp_rumble_block(&d[1], a);   // 左馬達
    sp_rumble_block(&d[5], a);   // 右馬達
    return sp_send(0x10, d, sizeof(d));
}

static bool sp_do_step(uint8_t step)
{
    static const uint8_t c80[4] = { 0x02, 0x03, 0x02, 0x04 };
    if (step < 4) return sp_send(0x80, &c80[step], 1);
    if (step == 4) return sp_subcmd(0x03, 0x30);
    if (step == 5) return sp_subcmd(0x30, 0x01);
    return true;
}

// 每一步：收到回應就做下一步；80 04 沒有回應，所以最多等 150 ms 就繼續
static void sp_tick(uint32_t now)
{
    if (!sp_active || sp_step >= 6) return;
    if (sp_sent_ms && !sp_got_reply && now - sp_sent_ms < 150) return;
    if (!tuh_hid_send_ready(sp_dev, sp_idx)) return;
    uint8_t next = sp_sent_ms ? (uint8_t)(sp_step + 1) : 0;
    if (next >= 6) { sp_step = 6; return; }
    if (sp_do_step(next)) {
        sp_step = next;
        sp_got_reply = false;
        sp_sent_ms = now ? now : 1;
    }
}

// 搖桿是 12 位元（0–4095，中間約 2048，行程約 ±1400）→ 換成 XInput 的 ±32767
static int16_t sp_axis(int v)
{
    int x = (v - 2048) * 24;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static void sp_parse(const uint8_t *r, uint16_t len)
{
    if (len < 12 || r[0] != 0x30) return;   // 0x30 = 完整按鍵報告
    uint8_t rb = r[3], sh = r[4], lb = r[5];
    uint16_t b = 0;
    if (rb & 0x04) b |= XINPUT_GAMEPAD_A;   // Switch 的 B 在下面
    if (rb & 0x08) b |= XINPUT_GAMEPAD_B;   // Switch 的 A 在右邊
    if (rb & 0x01) b |= XINPUT_GAMEPAD_X;   // Switch 的 Y 在左邊
    if (rb & 0x02) b |= XINPUT_GAMEPAD_Y;   // Switch 的 X 在上面
    if (rb & 0x40) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (lb & 0x40) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (sh & 0x01) b |= XINPUT_GAMEPAD_BACK;
    if (sh & 0x02) b |= XINPUT_GAMEPAD_START;
    if (sh & 0x04) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (sh & 0x08) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (sh & 0x10) b |= XINPUT_GAMEPAD_GUIDE;
    if (sh & 0x20) b |= XINPUT_GAMEPAD_SHARE;
    if (lb & 0x01) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (lb & 0x02) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (lb & 0x04) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (lb & 0x08) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    pad.buttons = b;
    pad.lt = (lb & 0x80) ? 255 : 0;   // ZL 只有按下／放開
    pad.rt = (rb & 0x80) ? 255 : 0;   // ZR
    pad.lx = sp_axis(r[6] | ((r[7] & 0x0F) << 8));
    pad.ly = sp_axis((r[7] >> 4) | (r[8] << 4));
    pad.rx = sp_axis(r[9] | ((r[10] & 0x0F) << 8));
    pad.ry = sp_axis((r[10] >> 4) | (r[11] << 4));
    pad.connected = true;
}

// ============================================================================
// TinyUSB 的回呼（都在第二個核心、tuh_task() 裡被呼叫）
// ============================================================================

// 把 XInput 驅動加進 TinyUSB（它不是內建的類別）
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &usbh_xinput_driver;
}

void tuh_mount_cb(uint8_t dev_addr)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    // 微軟、任天堂的裝置不需要；其他副廠接收器可能要看到 Windows 的請求才會切換
    if (vid != 0x045E && vid != 0x057E) windows_probe(dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) { (void)dev_addr; }

// ---- HID：目前只處理 Switch Pro ----

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc, uint16_t len)
{
    (void)desc; (void)len;
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    if (vid == 0x057E && pid == 0x2009) {
        sp_dev = dev_addr; sp_idx = idx;
        sp_step = 0; sp_sent_ms = 0; sp_counter = 0;
        sp_active = true;
    }
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)idx;
    if (sp_active && dev_addr == sp_dev) { sp_active = false; pad_clear(); }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    // 長度 0 = 這次沒有資料（或傳輸失敗），直接再要下一次
    if (len > 0 && sp_active && dev_addr == sp_dev) {
        if (report[0] == 0x81 || report[0] == 0x21) sp_got_reply = true;   // 握手／子指令的回應
        sp_parse(report, len);
    }
    tuh_hid_receive_report(dev_addr, idx);
}

// ---- XInput ----

static uint8_t xi_dev, xi_inst;
static volatile bool xi_active;

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *itf)
{
    // 360 無線接收器有 4 個欄位，沒接手把的欄位先不管，等它送資料再說
    if (itf->type == XBOX360_WIRELESS && !itf->connected) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }
    xi_dev = dev_addr; xi_inst = instance; xi_active = true;
    tuh_xinput_set_led(dev_addr, instance, 1, false);   // 1P 燈
    pad.connected = true;
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    if (xi_active && dev_addr == xi_dev && instance == xi_inst) { xi_active = false; pad_clear(); }
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                   xinputh_interface_t const *itf, uint16_t len)
{
    (void)len;
    if (itf->connected && itf->new_pad_data) {
        const xinput_gamepad_t *p = &itf->pad;
        pad.buttons = p->wButtons;
        pad.lt = p->bLeftTrigger;  pad.rt = p->bRightTrigger;
        pad.lx = p->sThumbLX;      pad.ly = p->sThumbLY;
        pad.rx = p->sThumbRX;      pad.ry = p->sThumbRY;
        if (!xi_active) { xi_dev = dev_addr; xi_inst = instance; xi_active = true; }
        pad.connected = true;
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

// ============================================================================
// 第二個核心
// ============================================================================

// 第二個核心的堆疊：8 KB，放在一般 RAM。
// SDK 預設只給 2 KB、放在 scratch 記憶體，旁邊就是 HDMI 的中斷程式。
// 2026-10-23：USB 這邊呼叫一深（例如算 Switch 震動用的 log2f），堆疊溢出蓋掉 HDMI 的程式，
// 結果畫面全黑、手把一直震。
static uint32_t core1_stack[4 * 1024 / 4];   // 4 KB：2026-10-24 實測最多用約 600 bytes

unsigned usb_pad_stack_free(void)
{
    size_t i = 0;
    while (i < count_of(core1_stack) && core1_stack[i] == 0x5A5A5A5Au) ++i;
    return (unsigned)(i * 4);
}

uint32_t usb_core_vtor;              // 診斷：這個核心的中斷向量表在哪裡

static void core1_main(void)
{
    usb_core_vtor = scb_hw->vtor;
    // 讓另一個核心寫 Flash 時可以叫這個核心暫停（romflash.c 的 flash_safe_execute）：
    // 寫 Flash 的時候不能從 Flash 讀程式，這個核心的程式也在 Flash 上
    flash_safe_execute_core_init();
    // 堆疊下限：用超過 core1_stack 的底部就立刻出錯（見 main.c 的 run_on_big_stack）
    __asm volatile ("msr msplim, %0" : : "r"(&core1_stack[0]));


    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = 10;   // GP10 = D+，GP11 = D-（PIO-USB 規定 D- = D+ 的下一支）
    cfg.tx_ch  = 2;    // DMA 0、1 是畫面在用
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
    tuh_init(BOARD_TUH_RHPORT);
    ready = true;   // PIO-USB 已經登記了 DMA 通道 2
    // 這個核心的中斷都登記好了：把向量表設成唯讀（診斷，見 main.c 的 protect_vector_table）
    extern void protect_vector_table(void);
    protect_vector_table();

    uint8_t sent_s = 0, sent_w = 0;
    uint32_t last_send = 0;
    while (true) {
        tuh_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        sp_tick(now);

        // 震動：有變化就送；Switch Pro 震動中要每 50 ms 再送一次，不然會自己停
        uint8_t s = want_strong, w = want_weak;
        bool changed = (s != sent_s || w != sent_w);
        if (sp_active && sp_step >= 6) {
            bool on = s || w;
            if ((changed || (on && now - last_send >= 50)) && tuh_hid_send_ready(sp_dev, sp_idx)) {
                if (sp_rumble(s, w)) { sent_s = s; sent_w = w; last_send = now; }
            }
        } else if (xi_active) {
            if (changed && now - last_send >= 20) {
                last_send = now;
                if (tuh_xinput_set_rumble(xi_dev, xi_inst, s, w, false)) { sent_s = s; sent_w = w; }
            }
        } else {
            sent_s = sent_w = 0;
        }
    }
}

void usb_pad_start(void)
{
    pad_clear();
    for (size_t i = 0; i < count_of(core1_stack); ++i) core1_stack[i] = 0x5A5A5A5Au;   // 量堆疊用量用
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof(core1_stack));
}
