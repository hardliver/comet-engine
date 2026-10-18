// map.h — 地圖模組的「說明書」
//
// 一張地圖 = 寬 × 高 格，每一格存一個「格子編號」（0 = 空的）。
// 一張地圖可以有好幾個圖層（例如 ground、deco、solid），每層各自有名字。
// 一張地圖可以同時用好幾張圖（tileset），格子編號接續下去：
//   第 1 張圖的第 0 格 = 1，第 1 格 = 2 …；第 2 張圖接在第 1 張的最後一格後面。
// 例如 Tiny Town（132 格）＋ Tiny Dungeon（132 格）：1–132 是 Town，133–264 是 Dungeon。

#ifndef MAP_H
#define MAP_H

#include <stdint.h>

#define MAX_MAPS 32        // 最多同時載入幾張地圖
#define MAX_LAYERS 8       // 一張地圖最多幾個圖層
#define MAX_TILESETS 4     // 一張地圖最多用幾張圖
#define LAYER_NAME_MAX 16  // 圖層名稱最長幾個 byte（含結尾的 0）

// 換遊戲時呼叫：釋放所有地圖
void map_reset(void);

// 建立一張地圖：w × h 格，用 imgs 這幾張圖（圖片編號，見 gfx.h）。
// 所有圖的小格大小必須相同。成功回傳地圖編號（1 起算）；
// 失敗回傳 0（參數不對、地圖太多、記憶體不夠）或 -1（圖的小格大小不一樣）
int map_new(int w, int h, const int *imgs, int nimgs);

// 幫地圖加一個圖層。data 是 w × h 個格子編號，每個 2 byte（little-endian），由左到右、由上到下。
// 回傳 0 成功；-1 參數不對、圖層太多或記憶體不夠；-2 有格子編號超過這張地圖的圖能用的範圍
int map_add_layer(int m, const char *name, const uint8_t *data);

// 讀一格。回傳格子編號；超出地圖回傳 0；地圖不存在回傳 -2、圖層不存在回傳 -1
int map_get(int m, const char *layer, int cx, int cy);

// 改一格。回傳 0 成功（超出地圖就不做事，也算成功）；地圖、圖層不存在同 map_get
int map_set(int m, const char *layer, int cx, int cy, int tile);

// 畫一個圖層，地圖的左上角放在 (ox, oy)。受相機影響，只畫畫面看得到的格子。
// 回傳 0 成功；地圖、圖層不存在同 map_get
int map_draw(int m, const char *layer, int ox, int oy);

// 地圖幾格寬、幾格高、一格幾像素（地圖不存在回傳 0）
int map_w(int m);
int map_h(int m);
int map_tile(int m);

#endif
