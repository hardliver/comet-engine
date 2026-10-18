// test_scene.c — 測試畫面的內容
// 和核心一樣，不碰任何平台的東西，所以兩個平台都能用。

#include "test_scene.h"
#include "../core/gfx.h"

void test_scene_draw(void)
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
}
