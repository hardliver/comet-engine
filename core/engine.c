// engine.c — 引擎主體的實作

#include <string.h>
#include "engine.h"
#include "gfx.h"
#include "map.h"
#include "pak.h"
#include "replay.h"
#include "save.h"
#include "system.h"
#include "script.h"
#include "sha256.h"

static uint32_t frame_count = 0;
static char check_error[512];   // 上一次 engine_check 的錯誤訊息
static uint32_t seed = 1;
static uint8_t cart_sha[32];

void engine_set_seed(uint32_t s)
{
    seed = s ? s : 1;
}

uint32_t engine_seed(void)
{
    return seed;
}

const uint8_t *engine_cart_sha(void)
{
    return cart_sha;
}

void engine_init(void)
{
    frame_count = 0;
    gfx_reset();
    map_reset();
    gfx_cls(0);
}

static int load(const char *src, size_t len, int is_system)
{
    // 先算遊戲的指紋，讓重播檢查是不是同一個遊戲（不是就不載入）
    uint8_t sha[32];
    sha256(src, len, sha);
    // 有資源包：指紋再加上資源包的內容指紋（資源包換了，舊的重播就對不上）
    if (pak_sha() != NULL) {
        Sha256 s;
        sha256_init(&s);
        sha256_update(&s, sha, 32);
        sha256_update(&s, pak_sha(), 32);
        sha256_final(&s, sha);
    }
    if (replay_on_load(sha)) return 2;
    memcpy(cart_sha, sha, 32);
    save_on_load(src, len);   // 遊戲的 id：存檔放在哪裡
    system_on_load(is_system);

    frame_count = 0;
    script_set_seed(seed);
    gfx_reset();   // 上一款遊戲的調色盤、相機、圖片、字型都清掉
    map_reset();   // 地圖也是
    pak_reset();   // 角色動畫也是
    // "=game"：錯誤訊息裡的檔名，例如 "game:12: attempt to call a nil value"
    return script_load(src, len, "=game", is_system);
}

int engine_load(const char *src, size_t len)
{
    return load(src, len, 0);
}

int engine_load_system(const char *src, size_t len)
{
    replay_stop();   // 系統卡匣不錄、不播
    return load(src, len, 1);
}

int engine_check(const char *src, size_t len)
{
    return script_check(src, len, "=game", check_error, sizeof(check_error));
}

const char *engine_check_error(void)
{
    return check_error;
}

void engine_frame(void)
{
    // 回 HOME 的按鍵：看的是玩家真正按的鍵，所以在重播蓋掉輸入之前
    system_before_frame();

    // 播放重播：輸入改成重播檔裡的；錄影：記下這一幀的輸入
    replay_before_frame(frame_count);

    if (script_failed()) {
        gfx_cls(8);   // 遊戲出錯：整個畫面變紅，錯誤訊息看平台的輸出
    } else {
        script_call("_update");   // 先更新狀態
        script_call("_draw");     // 再畫出來
    }
    frame_count++;
    replay_after_frame(frame_count);
}

uint32_t engine_frame_count(void)
{
    return frame_count;
}
