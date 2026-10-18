// engine.c — 引擎主體的實作

#include <string.h>
#include "engine.h"
#include "gfx.h"
#include "map.h"
#include "replay.h"
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

int engine_load(const char *src, size_t len)
{
    // 先算遊戲的指紋，讓重播檢查是不是同一個遊戲（不是就不載入）
    uint8_t sha[32];
    sha256(src, len, sha);
    if (replay_on_load(sha)) return 2;
    memcpy(cart_sha, sha, 32);

    frame_count = 0;
    script_set_seed(seed);
    gfx_reset();   // 上一款遊戲的調色盤、相機、圖片、字型都清掉
    map_reset();   // 地圖也是
    // "=game"：錯誤訊息裡的檔名，例如 "game:12: attempt to call a nil value"
    return script_load(src, len, "=game");
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
