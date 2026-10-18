// main.c — Mac 平台層（也是「無頭模擬器」：不開視窗，只跑遊戲）
// 讀取 Lua 遊戲檔，用「預先寫好的按鍵」代替真人操作，
// 跑固定幀數後把最後一幀存成圖檔。
// 同一串按鍵在網頁上跑，結果必須一模一樣：這就是之後的「測試向量」。
//
// 用法：
//   ./build/mac [遊戲檔] [輸出圖檔]                    用預先寫好的按鍵跑 90 幀
//   ./build/mac [遊戲檔] [輸出圖檔] --record 檔.rep     同上，並把輸入錄成重播檔
//   ./build/mac [遊戲檔] [輸出圖檔] --replay 檔.rep     播放重播檔，檢查最後的畫面是否一致
// 沒給遊戲檔、圖檔就用 games/move_box.lua、out.ppm
//
// --replay 的結束代碼：0 = 一致，3 = 畫面不同，1 = 其他錯誤。
// 之後的成就驗證（GitHub Actions）就是跑這個模式。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/engine.h"
#include "../../core/gfx.h"
#include "../../core/input.h"
#include "../../core/platform.h"
#include "../../core/replay.h"

// 核心要求平台提供的功能：印出一行訊息
void platform_log(const char *msg)
{
    printf("%s\n", msg);
}

// 把整個檔案讀進記憶體。回傳的記憶體用完要 free()
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);       // 跳到檔尾，看檔案多大
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);       // 跳回開頭

    char *buf = malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    *len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    return buf;
}

static int save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("無法建立檔案 %s\n", path);
        return 1;
    }

    const uint8_t *fb  = gfx_framebuffer();
    const uint8_t *pal = gfx_palette();

    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t c = fb[i];
        fwrite(&pal[c * 3], 1, 3, f);
    }

    fclose(f);
    return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("無法建立檔案 %s\n", path);
        return 1;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

// 預先寫好的按鍵（測試向量）：
// 第 0–29 幀：按住右鍵
// 第 30–59 幀：按住下鍵
// 第 60–89 幀：按住 A，左搖桿推向右上 (50, -90)，右搖桿推向左 (-127, 0)
#define SCRIPTED_FRAMES 90

static void scripted_input(int i)
{
    uint16_t keys = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    if (i < 30) {
        keys = 1 << BTN_RIGHT;
    } else if (i < 60) {
        keys = 1 << BTN_DOWN;
    } else {
        keys = 1 << BTN_A;
        lx = 50;  ly = -90;
        rx = -127;
    }
    input_set(0, keys);
    input_set_stick(0, STICK_LEFT, lx, ly);
    input_set_stick(0, STICK_RIGHT, rx, ry);
}

int main(int argc, char **argv)
{
    // 先挑出 --record／--replay，剩下的依序是遊戲檔、輸出圖檔
    const char *record_path = NULL, *replay_path = NULL;
    const char *positional[2] = { "games/move_box.lua", "out.ppm" };
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        }
    }
    const char *path = positional[0];
    const char *out = positional[1];

    size_t len = 0;
    char *src = read_file(path, &len);
    if (src == NULL) {
        printf("讀不到遊戲檔 %s\n", path);
        return 1;
    }

    engine_init();

    // 播放：要在載入遊戲「之前」把重播檔交給引擎（引擎要先知道種子）
    if (replay_path != NULL) {
        size_t rlen = 0;
        char *rep = read_file(replay_path, &rlen);
        if (rep == NULL) {
            printf("讀不到重播檔 %s\n", replay_path);
            return 1;
        }
        int bad = replay_play_begin((const uint8_t *)rep, rlen);
        free(rep);   // 引擎已經複製一份了
        if (bad) {
            return 1;
        }
    }

    int err = engine_load(src, len);
    free(src);
    if (err) {
        return 1;
    }

    if (replay_path != NULL) {
        // 一直跑到播完。平台不交輸入：輸入全部來自重播檔
        while (replay_state() == REPLAY_PLAYING) {
            engine_frame();
        }
        if (replay_state() != REPLAY_FINISHED) {
            return 1;   // 播放途中發現重播檔損壞
        }
        if (save_ppm(out) != 0) {
            return 1;
        }
        int same = replay_matched();
        printf("%s：重播 %u 幀，最後的畫面%s，已存成 %s\n",
               path, replay_frames(), same ? "一致" : "不同", out);
        return same ? 0 : 3;
    }

    if (record_path != NULL) {
        replay_record_begin();
    }
    for (int i = 0; i < SCRIPTED_FRAMES; i++) {
        scripted_input(i);
        engine_frame();
    }

    if (save_ppm(out) != 0) {
        return 1;
    }
    printf("%s：跑了 %u 幀，已存成 %s\n", path, engine_frame_count(), out);

    if (record_path != NULL) {
        const uint8_t *rep = replay_snapshot();
        size_t rlen = replay_snapshot_size();
        if (rep == NULL || write_file(record_path, rep, rlen) != 0) {
            return 1;
        }
        printf("重播檔 %s：%zu bytes\n", record_path, rlen);
    }
    return 0;
}
