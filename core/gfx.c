// gfx.c — 繪圖模組的實作
// 這個檔案只做「把顏色編號寫進陣列」，不知道畫面最後會出現在哪裡。

#include <stdlib.h>   // malloc、free（圖片的記憶體）
#include <string.h>   // memcpy

#include "gfx.h"

// 畫面：每個像素 1 byte，存的是顏色編號（0–255）
// static：只有這個檔案能直接碰它，別人只能透過函式
static uint8_t framebuffer[SCREEN_W * SCREEN_H];
uint32_t gfx_pixels_drawn = 0;   // 運算預算用（engine.c 每幀歸零）

// 繪圖用的顏色替換（dpal）：之後畫的每個點，顏色 c 先換成 dmap[c] 再寫進畫面。
// 和 pal（改調色盤的顏色，畫面上「所有」用到這個編號的點都會變）不同：dpal 只影響之後畫的東西。
// 用途：異色野伴（同一張圖換幾個顏色）、受傷閃白（全部換成白）、暗處（換成比較暗的顏色）
static uint8_t dmap[256];

// 預設調色盤：前 16 色，其餘是 0（黑色）。正式調色盤留到規格 v0.1
static const uint8_t default_palette[16 * 3] = {
      0,   0,   0,   // 0  黑
     29,  43,  83,   // 1  深藍
    126,  37,  83,   // 2  深紫
      0, 135,  81,   // 3  深綠
    171,  82,  54,   // 4  棕
     95,  87,  79,   // 5  深灰
    194, 195, 199,   // 6  淺灰
    255, 241, 232,   // 7  白
    255,   0,  77,   // 8  紅
    255, 163,   0,   // 9  橘
    255, 236,  39,   // 10 黃
      0, 228,  54,   // 11 綠
     41, 173, 255,   // 12 藍
    131, 118, 156,   // 13 灰紫
    255, 119, 168,   // 14 粉紅
    255, 204, 170,   // 15 膚色
};

// 目前的調色盤：256 色，每色 R、G、B 各 1 byte。遊戲可以用 pal() 改
static uint8_t palette[256 * 3];

// 原本的顏色：pal() 設定的值。paltint() 從這裡算出染色後的顏色寫進 palette
static uint8_t base_palette[256 * 3];

// 背景與深度（第一次用到才配置）
static uint8_t *bg_pixels = NULL;
static uint8_t *bg_depth = NULL;
static int bg_loaded = 0;

// 相機位置
static int cam_x = 0;
static int cam_y = 0;

// 圖片：寬、高、小格大小、像素（顏色編號）
typedef struct {
    int w, h, tile;
    uint8_t *pixels;   // NULL 代表這個位置沒有圖片
} Image;

// 圖片編號從 1 開始，所以陣列多開一格，images[0] 不用
static Image images[MAX_IMAGES + 1];

// 字型（同一時間只有一套）
static uint8_t *font_data = NULL;   // 所有字的資料接在一起
static int font_count = 0;          // 幾個字
static int font_height = 0;         // 每個字幾列
static int glyph_bytes = 0;         // 每個字佔幾 byte：4 + 1 + height × 2

void gfx_reset(void)
{
    memset(palette, 0, sizeof(palette));
    memcpy(palette, default_palette, sizeof(default_palette));
    memcpy(base_palette, palette, sizeof(palette));
    free(bg_pixels);
    free(bg_depth);
    bg_pixels = NULL;
    bg_depth = NULL;
    bg_loaded = 0;
    cam_x = 0;
    cam_y = 0;
    gfx_dpal_reset();
    for (int i = 1; i <= MAX_IMAGES; i++) {
        free(images[i].pixels);   // free(NULL) 什麼都不做，是安全的
        images[i].pixels = NULL;
    }
    free(font_data);
    font_data = NULL;
    font_count = 0;
}

// ============================================================
// 基本繪圖
// ============================================================

void gfx_cls(uint8_t color)
{
    memset(framebuffer, color, sizeof(framebuffer));
    gfx_pixels_drawn += SCREEN_W * SCREEN_H;
}

void gfx_dpal_reset(void)
{
    for (int i = 0; i < 256; i++) dmap[i] = (uint8_t)i;
}

void gfx_dpal(int from, int to)
{
    if (from < 0 || from > 255 || to < 0 || to > 255) return;
    dmap[from] = (uint8_t)to;
}

void gfx_palget(int index, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (index < 0 || index > 255) { *r = *g = *b = 0; return; }
    *r = base_palette[index * 3 + 0];
    *g = base_palette[index * 3 + 1];
    *b = base_palette[index * 3 + 2];
}

void gfx_pset(int x, int y, uint8_t color)
{
    color = dmap[color];
    x -= cam_x;
    y -= cam_y;
    // 超出畫面就直接離開。C 不會幫你檢查陣列邊界，
    // 寫到陣列外面會默默蓋掉別的資料，這種 bug 最難找。
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        return;
    }
    framebuffer[y * SCREEN_W + x] = color;
    gfx_pixels_drawn++;
}

void gfx_rect(int x, int y, int w, int h, uint8_t color)
{
    color = dmap[color];
    x -= cam_x;
    y -= cam_y;

    // 先算出矩形的右下角（不含）
    int x1 = x + w;
    int y1 = y + h;

    // 裁切：把矩形縮到畫面範圍內
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;

    // 裁切後已在畫面內，可以直接寫陣列，不用每點再檢查
    if (x1 > x && y1 > y) gfx_pixels_drawn += (uint32_t)((x1 - x) * (y1 - y));
    for (int py = y; py < y1; py++) {
        for (int px = x; px < x1; px++) {
            framebuffer[py * SCREEN_W + px] = color;
        }
    }
}

// ============================================================
// 調色盤、相機
// ============================================================

void gfx_pal(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < 0 || index > 255) return;
    palette[index * 3 + 0] = r;
    palette[index * 3 + 1] = g;
    palette[index * 3 + 2] = b;
    base_palette[index * 3 + 0] = r;
    base_palette[index * 3 + 1] = g;
    base_palette[index * 3 + 2] = b;
}

// 乘完再除以 256（右移 8 位），超過 255 就停在 255（染成偏紅時紅色可以 > 256）
static uint8_t tint(uint8_t c, int m)
{
    int v = (c * m) >> 8;
    return (uint8_t)(v > 255 ? 255 : v);
}

void gfx_paltint(int first, int count, int r, int g, int b)
{
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    for (int i = first; i < first + count; i++) {
        if (i < 0 || i > 255) continue;
        palette[i * 3 + 0] = tint(base_palette[i * 3 + 0], r);
        palette[i * 3 + 1] = tint(base_palette[i * 3 + 1], g);
        palette[i * 3 + 2] = tint(base_palette[i * 3 + 2], b);
    }
}

void gfx_camera(int x, int y)
{
    cam_x = x;
    cam_y = y;
}

int gfx_camera_x(void) { return cam_x; }
int gfx_camera_y(void) { return cam_y; }

// ============================================================
// 圖片
// ============================================================

static Image *get_image(int img)
{
    if (img < 1 || img > MAX_IMAGES || images[img].pixels == NULL) return NULL;
    return &images[img];
}

int gfx_image_new(int w, int h, const uint8_t *pixels, int tile)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;

    // 找一個空位
    int id = 0;
    for (int i = 1; i <= MAX_IMAGES; i++) {
        if (images[i].pixels == NULL) { id = i; break; }
    }
    if (id == 0) return 0;   // 圖片太多了

    uint8_t *copy = malloc((size_t)w * (size_t)h);
    if (copy == NULL) return 0;   // 記憶體不夠
    memcpy(copy, pixels, (size_t)w * (size_t)h);

    images[id].w = w;
    images[id].h = h;
    images[id].tile = tile > 0 ? tile : 16;
    images[id].pixels = copy;
    return id;
}

int gfx_image_w(int img) { Image *im = get_image(img); return im ? im->w : 0; }
int gfx_image_h(int img) { Image *im = get_image(img); return im ? im->h : 0; }
int gfx_image_tile(int img) { Image *im = get_image(img); return im ? im->tile : 0; }

void gfx_sspr(int img, int sx, int sy, int sw, int sh,
              int dx, int dy, int dw, int dh, int flip_x, int flip_y)
{
    Image *im = get_image(img);
    if (im == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    dx -= cam_x;
    dy -= cam_y;

    // 逐一走過「畫面上」要畫的每個點，反推它對應圖片的哪個像素。
    // 這樣放大、縮小都是同一套算法（最近鄰取樣，像素保持方塊狀）
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < 0 || py >= SCREEN_H) continue;

        int ty = y * sh / dh;              // 對應圖片範圍內的第幾列
        if (flip_y) ty = sh - 1 - ty;
        int iy = sy + ty;
        if (iy < 0 || iy >= im->h) continue;
        gfx_pixels_drawn += (uint32_t)dw;   // 這一列每個點都要算一次（畫面外的也是）

        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= SCREEN_W) continue;

            int tx = x * sw / dw;
            if (flip_x) tx = sw - 1 - tx;
            int ix = sx + tx;
            if (ix < 0 || ix >= im->w) continue;

            uint8_t c = im->pixels[iy * im->w + ix];
            if (c != 0) {                  // 0 是透明，不畫（看的是原本的顏色，替換之前）
                framebuffer[py * SCREEN_W + px] = dmap[c];
            }
        }
    }
}

void gfx_spr(int img, int n, int dx, int dy, int flip_x, int flip_y)
{
    Image *im = get_image(img);
    if (im == NULL || n < 0) return;

    int t = im->tile;
    int cols = im->w / t;                  // 一列有幾格
    if (cols <= 0) return;
    int sx = (n % cols) * t;
    int sy = (n / cols) * t;
    gfx_sspr(img, sx, sy, t, t, dx, dy, t, t, flip_x, flip_y);
}

// ============================================================
// 背景與深度
// ============================================================

uint8_t *gfx_bg_pixels(void)
{
    if (bg_pixels == NULL) bg_pixels = calloc(SCREEN_W * SCREEN_H, 1);
    return bg_pixels;
}

uint8_t *gfx_bg_depth(void)
{
    if (bg_depth == NULL) bg_depth = calloc(BG_DEPTH_W * BG_DEPTH_H, 1);
    return bg_depth;
}

int gfx_bg_loaded(void) { return bg_loaded && bg_pixels != NULL; }
void gfx_bg_set_loaded(int loaded) { bg_loaded = loaded; }

void gfx_bg_draw(void)
{
    if (!gfx_bg_loaded()) return;
    // 一列一列複製。相機是 0 的時候整列直接 memcpy，最快
    for (int y = 0; y < SCREEN_H; y++) {
        int sy = y + cam_y;                 // 畫面第 y 列對應背景的第 sy 列
        if (sy < 0 || sy >= SCREEN_H) continue;
        int x0 = 0, x1 = SCREEN_W;          // 畫面上要畫的範圍
        if (cam_x > 0) x1 = SCREEN_W - cam_x;
        if (cam_x < 0) x0 = -cam_x;
        if (x1 <= x0) continue;
        gfx_pixels_drawn += (uint32_t)(x1 - x0);
        memcpy(&framebuffer[y * SCREEN_W + x0], &bg_pixels[sy * SCREEN_W + x0 + cam_x], (size_t)(x1 - x0));
    }
}

// 背景座標 (bx, by) 的遮擋物深度（公分）
static int depth_at_bg(int bx, int by)
{
    if (bg_depth == NULL || bx < 0 || by < 0 || bx >= SCREEN_W || by >= SCREEN_H) return 0;
    return bg_depth[(by >> 1) * BG_DEPTH_W + (bx >> 1)] * BG_DEPTH_UNIT;
}

int gfx_bg_depth_at(int x, int y)
{
    return depth_at_bg(x + cam_x, y + cam_y);
}

// 解開第 ty 列到 out（w 點）。資料在載入時檢查過（gfx_sprite_check），這裡不再檢查
static void sprite_row(const uint8_t *data, int ty, int w, uint8_t *out)
{
    const uint8_t *p = data + (data[ty * 2] | (data[ty * 2 + 1] << 8));
    int x = 0;
    while (x < w) {
        int skip = p[0], run = p[1];
        p += 2;
        memset(out + x, 0, (size_t)skip);
        x += skip;
        memcpy(out + x, p, (size_t)run);
        p += run;
        x += run;
    }
}

int gfx_sprite_check(const uint8_t *data, uint32_t size, int w, int h)
{
    if (w <= 0 || h <= 0 || w > SPRITE_MAX_W || (uint32_t)h * 2 > size) return 1;
    for (int y = 0; y < h; y++) {
        uint32_t off = (uint32_t)(data[y * 2] | (data[y * 2 + 1] << 8));
        int x = 0;
        while (x < w) {
            if (off + 2 > size) return 1;
            int skip = data[off], run = data[off + 1];
            off += 2;
            // 每一段至少要往前走一點，否則壞掉的資料會讓迴圈停不下來
            if (skip + run == 0 || x + skip + run > w || off + (uint32_t)run > size) return 1;
            x += skip + run;
            off += (uint32_t)run;
        }
    }
    return 0;
}

void gfx_sprite_draw(const uint8_t *data, int w, int h,
                     int dx, int dy, int dw, int dh, int flip_x, int depth)
{
    static uint8_t row[SPRITE_MAX_W];   // 解開的一列（static：Pico 的堆疊很小）
    if (data == NULL || w <= 0 || h <= 0 || w > SPRITE_MAX_W || dw <= 0 || dh <= 0) return;
    // 深度測試只看背景的深度圖：背景的遮擋物（柱子、桌子、門框）比角色近，就被擋住。
    // 角色之間誰擋誰，由遊戲照遠到近的順序畫（後畫的蓋掉先畫的）
    int test = depth > 0 && bg_depth != NULL && bg_loaded;
    // 深度圖的單位是 8 公分：先把角色的深度換成同樣的單位，迴圈裡只比 byte
    int dz = depth / BG_DEPTH_UNIT;
    if (dz > 255) dz = 255;

    // 背景座標 = 畫面座標 + 相機；畫面座標 = 角色座標 - 相機
    int bx0 = dx, by0 = dy;
    dx -= cam_x;
    dy -= cam_y;
    int last_ty = -1;
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < 0 || py >= SCREEN_H) continue;
        int ty = y * h / dh;
        if (ty != last_ty) {                // 放大時好幾列對到同一列，只解一次
            sprite_row(data, ty, w, row);
            last_ty = ty;
        }
        gfx_pixels_drawn += (uint32_t)dw;
        int by = by0 + y;
        const uint8_t *drow = (test && by >= 0 && by < SCREEN_H) ? bg_depth + (by >> 1) * BG_DEPTH_W : NULL;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= SCREEN_W) continue;
            int tx = x * w / dw;
            if (flip_x) tx = w - 1 - tx;
            uint8_t c = row[tx];
            if (c == 0) continue;           // 透明
            if (drow != NULL) {
                int bx = bx0 + x;
                if (bx >= 0 && bx < SCREEN_W) {
                    uint8_t d = drow[bx >> 1];
                    if (d != 0 && d < dz) continue;   // 被背景的東西擋住
                }
            }
            framebuffer[py * SCREEN_W + px] = dmap[c];
        }
    }
}

// ============================================================
// 文字
// ============================================================

#define LINE_GAP 2   // 行和行之間空幾像素：14 + 2 = 16，剛好一格地圖的高度

// 讀出第 n 個字的字碼。資料是 little-endian（低位的 byte 在前），
// 一個 byte 一個 byte 自己組起來，不管這台機器的 CPU 是哪種排法都會讀對
static uint32_t glyph_code(const uint8_t *g)
{
    return (uint32_t)g[0] | ((uint32_t)g[1] << 8) | ((uint32_t)g[2] << 16) | ((uint32_t)g[3] << 24);
}

int gfx_font_new(int height, int count, const uint8_t *data)
{
    if (height < 1 || height > 32 || count < 1) return -1;
    int size = 5 + height * 2;

    // 二分搜尋的前提是「由小到大排好」，載入時先檢查，否則之後會默默找不到字
    for (int n = 1; n < count; n++) {
        if (glyph_code(data + (size_t)n * size) <= glyph_code(data + (size_t)(n - 1) * size)) return -2;
    }

    uint8_t *copy = malloc((size_t)count * size);
    if (copy == NULL) return -1;
    memcpy(copy, data, (size_t)count * size);

    free(font_data);
    font_data = copy;
    font_count = count;
    font_height = height;
    glyph_bytes = size;
    return 0;
}

int gfx_font_loaded(void)
{
    return font_data != NULL;
}

// 二分搜尋：每次看中間那個字，太小就往右半邊找、太大就往左半邊找。
// 700 個字最多比 10 次（2^10 = 1024 > 700）
static const uint8_t *find_glyph(uint32_t cp)
{
    int lo = 0, hi = font_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *g = font_data + (size_t)mid * glyph_bytes;
        uint32_t code = glyph_code(g);
        if (code == cp) return g;
        if (code < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// 從 s[*i] 讀出一個 UTF-8 字，回傳字碼，並把 *i 移到下一個字。
// UTF-8 用 1–4 個 byte 表示一個字，看第一個 byte 的開頭就知道有幾個：
//   0xxxxxxx                       1 byte：ASCII
//   110xxxxx 10xxxxxx              2 byte
//   1110xxxx 10xxxxxx 10xxxxxx     3 byte：中文幾乎都在這裡
//   11110xxx 10xxxxxx × 3          4 byte：emoji 等
// x 的部分接起來就是字碼。格式不對的 byte 當成 U+FFFD（無法辨識的字）
static uint32_t utf8_next(const char *s, size_t len, size_t *i)
{
    uint8_t c = (uint8_t)s[*i];
    int extra;
    uint32_t cp;
    if (c < 0x80)                { *i += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { *i += 1; return 0xFFFD; }

    if (*i + extra >= len) { *i = len; return 0xFFFD; }   // 字串在一個字的中間就結束了
    for (int k = 1; k <= extra; k++) {
        uint8_t b = (uint8_t)s[*i + k];
        if ((b & 0xC0) != 0x80) { *i += 1; return 0xFFFD; }   // 後面的 byte 必須是 10xxxxxx
        cp = (cp << 6) | (b & 0x3F);
    }
    *i += extra + 1;
    return cp;
}

// 畫一個字：一列一列看點陣，位元是 1 的地方畫點。
// 最左邊的像素是最高位元（0x8000），每往右一格就把遮罩右移一位
static void draw_glyph(const uint8_t *g, int x, int y, uint8_t color)
{
    int w = g[4];
    for (int r = 0; r < font_height; r++) {
        unsigned bits = ((unsigned)g[5 + r * 2] << 8) | g[6 + r * 2];   // big-endian
        for (int c = 0; c < w; c++) {
            if (bits & (0x8000u >> c)) gfx_pset(x + c, y + r, color);
        }
    }
}

// 字型裡沒有的字：畫一個方框，讓人一眼看出少了字（而不是默默跳過）
static void draw_missing(int x, int y, int w, uint8_t color)
{
    int h = font_height - 3;
    gfx_rect(x + 1, y + 1, w - 2, 1, color);
    gfx_rect(x + 1, y + h, w - 2, 1, color);
    gfx_rect(x + 1, y + 1, 1, h, color);
    gfx_rect(x + w - 2, y + 1, 1, h, color);
}

// text 和 textw 共用的排版：draw = 0 時只算寬度。
// 共用同一段程式，量出來的寬度就保證和畫出來的一樣
static int layout(const char *s, size_t len, int x, int y, uint8_t color, int draw)
{
    int missing_w = font_height - 1;   // 方框的寬度：c11 是 13，和中文字一樣寬
    int cx = x, widest = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = utf8_next(s, len, &i);
        if (cp == '\n') {
            if (cx - x > widest) widest = cx - x;
            cx = x;
            y += font_height + LINE_GAP;
            continue;
        }
        if (cp < 0x20) continue;       // 其他控制字元（Tab 等）不畫
        const uint8_t *g = find_glyph(cp);
        if (g != NULL) {
            if (draw) draw_glyph(g, cx, y, color);
            cx += g[4];
        } else {
            if (draw) draw_missing(cx, y, missing_w, color);
            cx += missing_w;
        }
    }
    if (cx - x > widest) widest = cx - x;
    return widest;
}

int gfx_text(const char *s, size_t len, int x, int y, uint8_t color)
{
    if (font_data == NULL) return 0;
    return layout(s, len, x, y, color, 1);
}

int gfx_text_width(const char *s, size_t len)
{
    if (font_data == NULL) return 0;
    return layout(s, len, 0, 0, 0, 0);
}

// ============================================================
// 給平台層讀取用
// ============================================================

const uint8_t *gfx_framebuffer(void)
{
    return framebuffer;
}

const uint8_t *gfx_palette(void)
{
    return palette;
}
