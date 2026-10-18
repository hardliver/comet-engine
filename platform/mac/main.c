// main.c — Mac 平台層
// 呼叫核心畫圖，再把畫面存成 PPM 圖檔。
// 只有平台層可以用 stdio（檔案、印字），核心不行。

#include <stdio.h>
#include "../../core/gfx.h"

// 把核心的畫面（顏色編號）查調色盤換成 RGB，寫成 PPM 檔
static int save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("無法建立檔案 %s\n", path);
        return 1;
    }

    const uint8_t *fb  = gfx_framebuffer();
    const uint8_t *pal = gfx_palette();

    // PPM 標頭：格式 P6、寬、高、每色最大值
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);

    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t c = fb[i];              // 這個像素的顏色編號
        fwrite(&pal[c * 3], 1, 3, f);   // 寫出對應的 R、G、B
    }

    fclose(f);
    return 0;
}

int main(void)
{
    gfx_cls(1);                        // 深藍背景
    gfx_rect(20, 20, 100, 60, 8);      // 紅色方塊
    gfx_rect(140, 100, 60, 60, 11);    // 綠色方塊
    gfx_rect(280, 200, 80, 80, 10);    // 黃色方塊，超出右下角，測試裁切
    gfx_rect(-30, 180, 60, 40, 12);    // 藍色方塊，超出左邊，測試裁切

    // 白色對角線，一點一點畫，測試 pset
    for (int i = 0; i < 240; i++) {
        gfx_pset(i, i, 7);
    }

    if (save_ppm("out.ppm") != 0) {
        return 1;
    }
    printf("已存成 out.ppm\n");
    return 0;
}
