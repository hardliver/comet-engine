// main.c — Mac 平台層
// 讀取 Lua 遊戲檔，用「預先寫好的按鍵」代替真人操作，
// 跑固定幀數後把最後一幀存成圖檔。
// 同一串按鍵在網頁上跑，結果必須一模一樣：這就是之後的「測試向量」。
//
// 用法：./build/mac [遊戲檔]，沒給就用 games/move_box.lua

#include <stdio.h>
#include <stdlib.h>
#include "../../core/engine.h"
#include "../../core/gfx.h"
#include "../../core/input.h"
#include "../../core/platform.h"

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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "games/move_box.lua";

    size_t len = 0;
    char *src = read_file(path, &len);
    if (src == NULL) {
        printf("讀不到遊戲檔 %s\n", path);
        return 1;
    }

    engine_init();
    int err = engine_load(src, len);
    free(src);
    if (err) {
        return 1;
    }

    // 第 0–29 幀：按住右鍵
    // 第 30–59 幀：按住下鍵
    // 第 60–89 幀：按住右鍵 + A
    for (int i = 0; i < 90; i++) {
        uint16_t keys;
        if (i < 30) {
            keys = 1 << BTN_RIGHT;
        } else if (i < 60) {
            keys = 1 << BTN_DOWN;
        } else {
            keys = (1 << BTN_RIGHT) | (1 << BTN_A);
        }
        input_set(0, keys);
        engine_frame();
    }

    if (save_ppm("out.ppm") != 0) {
        return 1;
    }
    printf("%s：跑了 %u 幀，已存成 out.ppm\n", path, engine_frame_count());
    return 0;
}
