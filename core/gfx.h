// gfx.h — 繪圖模組的「說明書」
// 別的檔案只要 #include "gfx.h"，就知道有哪些函式可以用。

#ifndef GFX_H   // 防止同一個檔案被 include 兩次
#define GFX_H

#include <stddef.h>  // 提供 size_t（記憶體大小、字串長度用的整數）
#include <stdint.h>  // 提供 uint8_t（0–255 的整數，剛好 1 byte）

// 遊戲畫面大小（暫定，規格 v0.1 才凍結）
#define SCREEN_W 320
#define SCREEN_H 240

// 最多同時載入幾張圖片
#define MAX_IMAGES 64

// 換遊戲時呼叫：調色盤回到預設、相機歸零、釋放所有圖片和字型
void gfx_reset(void);

// ---- 基本繪圖 ----

// 清除整個畫面，填滿 color（不受相機影響）
// 運算預算（engine.h 的 engine_profile）：這一幀畫了幾個像素（每一個「處理過」的點都算，透明的也算）
extern uint32_t gfx_pixels_drawn;

void gfx_cls(uint8_t color);

// 繪圖用的顏色替換（見 gfx.c 的 dmap）：之後畫的顏色 from 換成 to；reset 全部還原。不影響 cls、bg
void gfx_dpal(int from, int to);
void gfx_dpal_reset(void);
// 調色盤第 index 號的顏色（pal 設的原色，不含 paltint 的染色）
void gfx_palget(int index, uint8_t *r, uint8_t *g, uint8_t *b);

// 在 (x, y) 畫一個點；超出畫面就不畫
void gfx_pset(int x, int y, uint8_t color);

// 畫實心矩形，左上角 (x, y)，寬 w、高 h；超出畫面的部分不畫
void gfx_rect(int x, int y, int w, int h, uint8_t color);

// ---- 調色盤 ----

// 設定第 index 號顏色。0–15 是預設的 16 色，16 以後通常給圖片用
void gfx_pal(int index, uint8_t r, uint8_t g, uint8_t b);

// 調色盤染色：把 first 起 count 個顏色乘上 (r, g, b) / 256（256 = 不變，128 = 一半亮）。
// 每次都從「原本的顏色」（pal() 最後設定的值）算起，所以重複呼叫不會越染越深；
// paltint(first, count, 256, 256, 256) 就是還原。
// 用途：角色走進暗的房間，整組角色顏色一起變暗、偏色，不用每個像素去算
void gfx_paltint(int first, int count, int r, int g, int b);

// ---- 相機 ----
// 設定之後，所有繪圖（cls 除外）都會往左上平移 (x, y)。
// 地圖比畫面大的時候，移動相機就是捲動畫面
void gfx_camera(int x, int y);

// 目前的相機位置（地圖模組用來算出畫面看得到哪些格子）
int gfx_camera_x(void);
int gfx_camera_y(void);

// ---- 圖片 ----
// 圖片的每個像素是一個顏色編號；編號 0 是「透明」，畫的時候會跳過。

// 建立一張圖片，複製 pixels（w × h 個顏色編號）。
// tile：切成幾 × 幾的小格（spr 用），通常是 16。
// 成功回傳圖片編號（1 起算），失敗回傳 0
int gfx_image_new(int w, int h, const uint8_t *pixels, int tile);

// 畫圖片的一部分：從圖片 (sx, sy) 取 sw × sh 的範圍，畫到畫面 (dx, dy)，
// 放大成 dw × dh（和 sw、sh 相同就是原尺寸）。flip_x、flip_y：左右、上下翻轉
void gfx_sspr(int img, int sx, int sy, int sw, int sh,
              int dx, int dy, int dw, int dh, int flip_x, int flip_y);

// 畫圖片的第 n 格（由左到右、由上到下，從 0 開始數）
void gfx_spr(int img, int n, int dx, int dy, int flip_x, int flip_y);

// 圖片的寬、高、小格大小（不存在回傳 0）
int gfx_image_w(int img);
int gfx_image_h(int img);
int gfx_image_tile(int img);

// ---- 背景與深度（固定視角遊戲用，見 pak.h）----
// 背景：320 × 240 個顏色編號，畫面最底層。深度：160 × 120（每 2 × 2 像素一格），
// 每格是「這個位置最前面的遮擋物離鏡頭多遠」，單位 8 公分，0 = 沒有東西擋。
// 第一次用到才配置記憶體（不用背景的遊戲不佔記憶體），換遊戲時釋放。

#define BG_DEPTH_W (SCREEN_W / 2)
#define BG_DEPTH_H (SCREEN_H / 2)
#define BG_DEPTH_UNIT 8           // 深度的單位：8 公分

// 背景、深度的陣列（給資源包模組寫入）；記憶體不夠回傳 NULL
uint8_t *gfx_bg_pixels(void);
uint8_t *gfx_bg_depth(void);

// 有沒有載入背景
int gfx_bg_loaded(void);
void gfx_bg_set_loaded(int loaded);

// 把背景畫到畫面上（受相機影響：相機往右 3，背景就往左 3，用來做畫面震動）
void gfx_bg_draw(void);

// 背景在畫面 (x, y) 這一點的深度（公分，0 = 沒有遮擋物）；除錯和遊戲判斷用
int gfx_bg_depth_at(int x, int y);

// 畫一格角色。data 是「一列一列壓縮」的格式（資源包的角色動畫用，見 pak.h）：
//   開頭 h 個 u16：每一列的資料從哪裡開始（從 data 算起）
//   每一列：重複（透明幾點 u8、接著有顏色的幾點 u8、那幾點的顏色編號），直到湊滿 w 點
// 角色周圍大多是透明的，這樣存大約只要一半的空間。
// 畫到 (dx, dy)，放大成 dw × dh；flip_x：左右翻轉。
// depth：角色離鏡頭幾公分；背景那一點的遮擋物比角色近，就不畫那一點。0 = 不做深度測試（介面、特效）
#define SPRITE_MAX_W 1024
void gfx_sprite_draw(const uint8_t *data, int w, int h,
                     int dx, int dy, int dw, int dh, int flip_x, int depth);

// 檢查一格壓縮的角色資料是否完整（size：這格資料最多幾 bytes）。正確回傳 0
int gfx_sprite_check(const uint8_t *data, uint32_t size, int w, int h);

// ---- 文字 ----
// 字型資料的格式和 fonts/c11.bin 相同（見 tools/font/c11_convert.py）：
// 每個字 = 字碼 4 byte（little-endian）＋ 寬度 1 byte ＋ height 列點陣（每列 2 byte，big-endian），
// 依字碼由小到大排列。

// 載入字型（複製 data），取代原本的字型。
// 回傳 0 成功；-1 參數不對或記憶體不夠；-2 字碼沒有由小到大排列
int gfx_font_new(int height, int count, const uint8_t *data);

// 有沒有載入字型
int gfx_font_loaded(void);

// 從 (x, y) 開始畫 UTF-8 字串 s（長度 len byte），遇到 \n 換行。
// 字型裡沒有的字畫成一個方框。回傳最寬那一行的寬度（像素）
int gfx_text(const char *s, size_t len, int x, int y, uint8_t color);

// 和 gfx_text 一樣排版，但不畫，只回傳寬度
int gfx_text_width(const char *s, size_t len);

// ---- 給平台層讀取用 ----

// 畫面陣列（SCREEN_W * SCREEN_H 個顏色編號）
const uint8_t *gfx_framebuffer(void);

// 調色盤（256 色 × RGB 3 bytes）
const uint8_t *gfx_palette(void);

#endif
