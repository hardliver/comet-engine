// main.c — Mac 平台層
// Mac 版沒有視窗，所以用「預先寫好的按鍵」代替真人操作，
// 跑固定幀數後把最後一幀存成圖檔。
// 同一串按鍵在網頁上跑，結果必須一模一樣：這就是之後的「測試向量」。

#include <stdio.h>
#include "../../core/engine.h"
#include "../../core/gfx.h"
#include "../../core/input.h"

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

int main(void)
{
    engine_init();

    // 第 0–29 幀：按住右鍵
    // 第 30–59 幀：按住下鍵
    // 第 60–89 幀：按住右鍵 + A
    for (int i = 0; i < 90; i++) {
        uint8_t keys;
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
    printf("跑了 %u 幀，已存成 out.ppm\n", engine_frame_count());
    return 0;
}
