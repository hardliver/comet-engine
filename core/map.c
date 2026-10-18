// map.c — 地圖模組的實作

#include <stdlib.h>   // malloc、free
#include <string.h>   // strcmp、strncpy、memset

#include "map.h"
#include "gfx.h"

typedef struct {
    char name[LAYER_NAME_MAX];
    uint16_t *cells;              // w × h 個格子編號
} Layer;

typedef struct {
    int used;                     // 0 代表這個位置沒有地圖
    int w, h, tile;               // 幾格寬、幾格高、一格幾像素
    int nsets;                    // 用了幾張圖
    int set_img[MAX_TILESETS];    // 每張圖的圖片編號
    int set_first[MAX_TILESETS];  // 每張圖的第 0 格，在這張地圖裡是幾號
    int set_count[MAX_TILESETS];  // 每張圖有幾格
    int total;                    // 能用的最大格子編號（所有圖的格數加起來）
    int nlayers;
    Layer layers[MAX_LAYERS];
} Map;

// 地圖編號從 1 開始，maps[0] 不用
static Map maps[MAX_MAPS + 1];

void map_reset(void)
{
    for (int i = 1; i <= MAX_MAPS; i++) {
        for (int k = 0; k < maps[i].nlayers; k++) free(maps[i].layers[k].cells);
        memset(&maps[i], 0, sizeof(Map));
    }
}

static Map *get_map(int m)
{
    if (m < 1 || m > MAX_MAPS || !maps[m].used) return NULL;
    return &maps[m];
}

static Layer *get_layer(Map *map, const char *name)
{
    for (int k = 0; k < map->nlayers; k++) {
        if (strcmp(map->layers[k].name, name) == 0) return &map->layers[k];
    }
    return NULL;
}

int map_new(int w, int h, const int *imgs, int nimgs)
{
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || nimgs < 1 || nimgs > MAX_TILESETS) return 0;

    int id = 0;
    for (int i = 1; i <= MAX_MAPS; i++) {
        if (!maps[i].used) { id = i; break; }
    }
    if (id == 0) return 0;   // 地圖太多了

    Map *map = &maps[id];
    memset(map, 0, sizeof(Map));
    map->w = w;
    map->h = h;
    map->tile = gfx_image_tile(imgs[0]);
    if (map->tile <= 0) return 0;   // 圖片不存在

    int next = 1;   // 下一張圖的第 0 格是幾號（0 保留給「空的」）
    for (int k = 0; k < nimgs; k++) {
        int t = gfx_image_tile(imgs[k]);
        if (t <= 0) return 0;
        if (t != map->tile) return -1;
        map->set_img[k] = imgs[k];
        map->set_first[k] = next;
        map->set_count[k] = (gfx_image_w(imgs[k]) / t) * (gfx_image_h(imgs[k]) / t);
        next += map->set_count[k];
    }
    map->nsets = nimgs;
    map->total = next - 1;
    map->used = 1;
    return id;
}

int map_add_layer(int m, const char *name, const uint8_t *data)
{
    Map *map = get_map(m);
    if (map == NULL || map->nlayers >= MAX_LAYERS || strlen(name) >= LAYER_NAME_MAX) return -1;
    if (get_layer(map, name) != NULL) return -1;   // 同名的圖層已經有了

    size_t n = (size_t)map->w * (size_t)map->h;
    uint16_t *cells = malloc(n * sizeof(uint16_t));
    if (cells == NULL) return -1;
    for (size_t i = 0; i < n; i++) {
        // 2 個 byte 組回一個數字（little-endian：低位在前），和字型一樣自己組，不管 CPU 怎麼排
        int tile = data[i * 2] | (data[i * 2 + 1] << 8);
        if (tile > map->total) { free(cells); return -2; }
        cells[i] = (uint16_t)tile;
    }

    Layer *layer = &map->layers[map->nlayers++];
    strncpy(layer->name, name, LAYER_NAME_MAX - 1);
    layer->name[LAYER_NAME_MAX - 1] = '\0';
    layer->cells = cells;
    return 0;
}

// 找到地圖和圖層；找不到時 *err 是 -2（沒有地圖）或 -1（沒有圖層）
static Layer *find(int m, const char *name, Map **out, int *err)
{
    Map *map = get_map(m);
    if (map == NULL) { *err = -2; return NULL; }
    Layer *layer = get_layer(map, name);
    if (layer == NULL) { *err = -1; return NULL; }
    *out = map;
    return layer;
}

int map_get(int m, const char *name, int cx, int cy)
{
    Map *map; int err;
    Layer *layer = find(m, name, &map, &err);
    if (layer == NULL) return err;
    if (cx < 0 || cy < 0 || cx >= map->w || cy >= map->h) return 0;   // 地圖外面當成空的
    return layer->cells[cy * map->w + cx];
}

int map_set(int m, const char *name, int cx, int cy, int tile)
{
    Map *map; int err;
    Layer *layer = find(m, name, &map, &err);
    if (layer == NULL) return err;
    if (cx < 0 || cy < 0 || cx >= map->w || cy >= map->h) return 0;
    if (tile < 0 || tile > map->total) tile = 0;
    layer->cells[cy * map->w + cx] = (uint16_t)tile;
    return 0;
}

// 無條件捨去的整數除法。C 的 / 是「往 0 取整」：-5 / 16 = 0，
// 但我們要的是 -1（第 -1 格），所以負數要另外處理
static int floor_div(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int map_draw(int m, const char *name, int ox, int oy)
{
    Map *map; int err;
    Layer *layer = find(m, name, &map, &err);
    if (layer == NULL) return err;

    // 畫面看得到的範圍（世界座標）是 相機 到 相機 + 畫面大小，
    // 換算成地圖的第幾格到第幾格，只畫這些。40×30 的地圖一次只畫約 21×16 格
    int t = map->tile;
    int cx0 = floor_div(gfx_camera_x() - ox, t);
    int cy0 = floor_div(gfx_camera_y() - oy, t);
    int cx1 = floor_div(gfx_camera_x() - ox + SCREEN_W - 1, t);
    int cy1 = floor_div(gfx_camera_y() - oy + SCREEN_H - 1, t);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= map->w) cx1 = map->w - 1;
    if (cy1 >= map->h) cy1 = map->h - 1;

    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            int tile = layer->cells[cy * map->w + cx];
            if (tile == 0) continue;   // 空的
            // 找出這個編號屬於哪張圖、是那張圖的第幾格
            for (int k = 0; k < map->nsets; k++) {
                int n = tile - map->set_first[k];
                if (n >= 0 && n < map->set_count[k]) {
                    gfx_spr(map->set_img[k], n, ox + cx * t, oy + cy * t, 0, 0);
                    break;
                }
            }
        }
    }
    return 0;
}

int map_w(int m)    { Map *map = get_map(m); return map ? map->w : 0; }
int map_h(int m)    { Map *map = get_map(m); return map ? map->h : 0; }
int map_tile(int m) { Map *map = get_map(m); return map ? map->tile : 0; }
