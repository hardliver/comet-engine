// gfx.c — 繪圖模組的實作
// 這個檔案只做「把顏色編號寫進陣列」，不知道畫面最後會出現在哪裡。

#include <stdlib.h>   // malloc、free（圖片的記憶體）
#include <string.h>   // memcpy

#include "gfx.h"

// 畫面：每個像素 1 byte，存的是顏色編號（0–255）
// static：只有這個檔案能直接碰它，別人只能透過函式
static uint8_t framebuffer[SCREEN_W * SCREEN_H];

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
    cam_x = 0;
    cam_y = 0;
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
}

void gfx_pset(int x, int y, uint8_t color)
{
    x -= cam_x;
    y -= cam_y;
    // 超出畫面就直接離開。C 不會幫你檢查陣列邊界，
    // 寫到陣列外面會默默蓋掉別的資料，這種 bug 最難找。
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        return;
    }
    framebuffer[y * SCREEN_W + x] = color;
}

void gfx_rect(int x, int y, int w, int h, uint8_t color)
{
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

        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= SCREEN_W) continue;

            int tx = x * sw / dw;
            if (flip_x) tx = sw - 1 - tx;
            int ix = sx + tx;
            if (ix < 0 || ix >= im->w) continue;

            uint8_t c = im->pixels[iy * im->w + ix];
            if (c != 0) {                  // 0 是透明，不畫
                framebuffer[py * SCREEN_W + px] = c;
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
