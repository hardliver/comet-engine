// main.c — Mac 平台層（也是「無頭模擬器」：不開視窗，只跑遊戲）
// 讀取 Lua 遊戲檔，用「預先寫好的按鍵」代替真人操作，
// 跑固定幀數後把最後一幀存成圖檔。
// 同一串按鍵在網頁上跑，結果必須一模一樣：這就是之後的「測試向量」。
//
// 用法：
//   ./build/mac [遊戲檔] [輸出圖檔]                    用預先寫好的按鍵跑 90 幀
//   ./build/mac [遊戲檔] [輸出圖檔] --record 檔.rep     同上，並把輸入錄成重播檔
//   ./build/mac [遊戲檔] [輸出圖檔] --replay 檔.rep     播放重播檔，檢查最後的畫面是否一致
//   加上 --input 按鍵腳本.txt：用腳本裡的按鍵代替預先寫好的按鍵，跑完整個腳本
//     （一行一段：「幀數 按鍵…」，按鍵名稱 L R U D A B START SELECT X Y LB RB，# 後面是註解）
//   加上 --shots 幀,幀,…：在這幾幀結束時另外存圖（輸出圖檔名加上 _幀數）
//   加上 --pak 檔.pak：遊戲的資源包（背景、角色動畫，見 core/pak.h）
//   加上 --compile 檔.nvc：不執行，把遊戲編譯成主機用的卡匣（.nvc：開頭幾行原始碼 + Lua 位元組碼，見 core/luarom.h）
//   加上 --rom 檔.nvc：用主機的方式執行：程式放進一塊「唯讀」記憶體（模擬 Flash），
//     Lua 只要寫到它就會當掉，用來證明程式真的可以放在 Flash。遊戲檔參數仍要給（引擎讀 id）
//   加上 --saves 資料夾：存檔寫在那個資料夾（檔名「id.欄位.sav」，id 的 / 換成 _）。
//   沒加就只存在記憶體裡，程式結束就沒了（測試時不會留下檔案）
//   加上 --clock 時間：遊戲的 clock() 讀到的現實時間。「now」= 這台電腦現在的時間，
//     或寫成 2026-10-22T14:05（秒可省略）。時間從那一刻開始，每 60 幀走 1 秒（跟著幀數走，結果每次一樣）。
//     沒加就是「沒有時間來源」，clock() 回傳 nil（測試的結果不會因為哪天跑而不同）
//   加上 --wav 檔.wav：這次執行所有的聲音存成 WAV（22050 Hz、16 位元、單聲道），可以聽、也可以比對（見 core/audio.h）
//   加上 --profile：運算預算（Level 1）。跑完印出平均、最重的 5 幀（第幾幀、Lua 指令、像素、繪圖呼叫、預估 Pico 負載 %）
//   ./build/mac 系統卡匣.lua 輸出圖檔 --system --game id 遊戲.lua [--game id 遊戲.lua …]
//                                                    Nova 模式：載入系統卡匣，用另一組預先寫好的按鍵
//                                                    跑 150 幀（選遊戲、啟動、回 HOME），測試系統卡匣
// 沒給遊戲檔、圖檔就用 games/move_box.lua、out.ppm
//
// --replay 的結束代碼：0 = 一致，3 = 畫面不同，1 = 其他錯誤。
// 之後的成就驗證（GitHub Actions）就是跑這個模式。

// mmap 的 MAP_ANON 不在 C99 標準裡；這兩行讓 Linux（glibc）和 Mac 都提供它。
// 一定要放在所有 #include 之前才有效
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../core/engine.h"
#include "../../core/audio.h"
#include "../../core/clock.h"
#include "../../core/gfx.h"
#include "../../core/input.h"
#include "../../core/pak.h"
#include "../../core/platform.h"
#include "../../core/replay.h"
#include "../../core/save.h"
#include "../../core/system.h"
#include "../../core/script.h"
#include "../../core/luarom.h"
#include "lauxlib.h"
#include <sys/mman.h>
#include <unistd.h>   // sysconf：記憶體頁的大小

// 核心要求平台提供的功能：印出一行訊息
void platform_log(const char *msg)
{
    printf("%s\n", msg);
}

// ---- 現實時鐘（--clock）----
// clock_start：第 0 幀時的本地時間（秒）；-1 = 沒有時間來源
static int64_t clock_start = -1;

int64_t platform_clock(void)
{
    if (clock_start < 0) return -1;
    // 用幀數算經過幾秒，而不是讀電腦的時間：無頭模擬器跑得比 60 幀／秒快很多，
    // 而且這樣同一個指令跑幾次結果都一樣
    return clock_start + engine_frame_count() / FPS;
}

// 「now」或「2026-10-22T14:05[:30]」→ 設定 clock_start。格式不對回傳 1
static int parse_clock(const char *s)
{
    if (strcmp(s, "now") == 0) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);   // 依這台電腦的時區換成年月日時分秒
        if (lt == NULL) return 1;
        clock_start = clock_from_date(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                                      lt->tm_hour, lt->tm_min, lt->tm_sec);
        return 0;
    }
    int y, mo, d, h, mi, sec = 0;
    // sscanf 回傳成功讀到幾個數字：至少要有年月日時分 5 個
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5) return 1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return 1;
    clock_start = clock_from_date(y, mo, d, h, mi, sec);
    return 0;
}

// ---- 聲音（--wav）----
// 每跑完一幀，把這一幀的聲音接在後面；最後存成 WAV 檔。
// 記憶體用 realloc 慢慢變大：一分鐘的聲音 = 22050 × 60 × 2 bytes ≈ 2.6 MB，在電腦上沒問題
static const char *wav_path = NULL;
static int16_t *wav = NULL;
static size_t wav_len = 0, wav_cap = 0;

static void wav_collect(void)
{
    if (wav_path == NULL) return;
    int n = audio_sample_count();
    if (wav_len + (size_t)n > wav_cap) {
        size_t cap = wav_cap ? wav_cap * 2 : 65536;
        int16_t *p = realloc(wav, cap * sizeof(int16_t));
        if (p == NULL) return;
        wav = p;
        wav_cap = cap;
    }
    memcpy(wav + wav_len, audio_samples(), (size_t)n * sizeof(int16_t));
    wav_len += (size_t)n;
}

// WAV 檔頭：44 bytes，所有數字都是 little-endian（低位元組在前）
static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (i * 8)) & 255), f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

static int wav_save(void)
{
    if (wav_path == NULL) return 0;
    FILE *f = fopen(wav_path, "wb");
    if (f == NULL) {
        printf("無法建立檔案 %s\n", wav_path);
        return 1;
    }
    uint32_t bytes = (uint32_t)(wav_len * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);   // PCM、單聲道
    put32(f, AUDIO_RATE); put32(f, AUDIO_RATE * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    for (size_t i = 0; i < wav_len; i++) put16(f, (uint16_t)wav[i]);
    fclose(f);
    printf("聲音 %s：%.1f 秒\n", wav_path, (double)wav_len / AUDIO_RATE);
    return 0;
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

// ---- 存檔 ----
// 有 --saves 就存成檔案，否則存在記憶體裡（只在這次執行有效）
static const char *save_dir = NULL;
static unsigned char *mem_save[SAVE_SLOTS];
static int mem_len[SAVE_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

// 存檔的檔名：資料夾/id.欄位.sav（"hardliver/wildkin" → "hardliver_wildkin.1.sav"）
static void save_path(char *out, size_t size, const char *game_id, int slot)
{
    char id[64];
    snprintf(id, sizeof(id), "%s", game_id);
    for (char *c = id; *c; c++) {
        if (*c == '/') *c = '_';
    }
    snprintf(out, size, "%s/%s.%d.sav", save_dir, id, slot);
}

int platform_save_read(const char *game_id, int slot, unsigned char *buf, int cap)
{
    if (save_dir == NULL) {
        if (mem_len[slot] < 0 || mem_len[slot] > cap) return -1;
        memcpy(buf, mem_save[slot], (size_t)mem_len[slot]);
        return mem_len[slot];
    }
    char path[512];
    save_path(path, sizeof(path), game_id, slot);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;   // 沒有存檔
    int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

int platform_save_write(const char *game_id, int slot, const unsigned char *data, int len)
{
    if (save_dir == NULL) {
        free(mem_save[slot]);
        mem_save[slot] = NULL;
        mem_len[slot] = -1;
        if (data == NULL) return 0;
        mem_save[slot] = malloc((size_t)len);
        if (mem_save[slot] == NULL) return 1;
        memcpy(mem_save[slot], data, (size_t)len);
        mem_len[slot] = len;
        return 0;
    }
    char path[512];
    save_path(path, sizeof(path), game_id, slot);
    if (data == NULL) {
        remove(path);
        return 0;
    }
    // 先寫到暫存檔再改名：寫到一半當機（或斷電），舊的存檔還在
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return 1;
    size_t n = fwrite(data, 1, (size_t)len, f);
    if (fclose(f) != 0 || n != (size_t)len) return 1;
    return rename(tmp, path) != 0;
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

// ---- 按鍵腳本（--input）----
// 每一段：幾幀、按哪些鍵。讀進來之後展開成「第幾幀按什麼」
#define MAX_SEGMENTS 4096
static int seg_frames[MAX_SEGMENTS];
static uint16_t seg_keys[MAX_SEGMENTS];
static int nseg = 0;

static int key_code(const char *name)
{
    static const char *names[] = { "L", "R", "U", "D", "A", "B", "START", "SELECT", "X", "Y", "LB", "RB" };
    for (int i = 0; i < 12; i++) {
        if (strcmp(name, names[i]) == 0) return i;
    }
    return -1;
}

// 讀按鍵腳本，回傳總幀數；格式錯誤回傳 -1
static int load_input_script(const char *path)
{
    size_t len = 0;
    char *src = read_file(path, &len);
    if (src == NULL) {
        printf("讀不到按鍵腳本 %s\n", path);
        return -1;
    }
    int total = 0;
    size_t pos = 0;
    int line_no = 0;
    while (pos < len) {
        char line[256];
        size_t n = 0;
        while (pos < len && src[pos] != '\n') {
            if (n < sizeof(line) - 1) line[n++] = src[pos];
            pos++;
        }
        pos++;
        line[n] = '\0';
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *tok = strtok(line, " \t\r");
        if (tok == NULL) continue;           // 空白行
        int frames = atoi(tok);
        uint16_t keys = 0;
        while ((tok = strtok(NULL, " \t\r")) != NULL) {
            int k = key_code(tok);
            if (k < 0) {
                printf("%s 第 %d 行：不認得的按鍵 %s\n", path, line_no, tok);
                free(src);
                return -1;
            }
            keys |= (uint16_t)(1 << k);
        }
        if (frames <= 0 || nseg >= MAX_SEGMENTS) {
            printf("%s 第 %d 行：幀數不對，或段數太多\n", path, line_no);
            free(src);
            return -1;
        }
        seg_frames[nseg] = frames;
        seg_keys[nseg] = keys;
        nseg++;
        total += frames;
    }
    free(src);
    return total;
}

// 第 i 幀要按的鍵
static uint16_t script_keys(int i)
{
    for (int s = 0; s < nseg; s++) {
        if (i < seg_frames[s]) return seg_keys[s];
        i -= seg_frames[s];
    }
    return 0;
}

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

// ---- Nova 模式（--system）----
// 預先寫好的按鍵：
//   第 10–13 幀：A（系統卡匣啟動游標所在的第 1 款）
//   第 14–49 幀：右（遊戲裡移動）
//   第 50–84 幀：Start＋Select（按住 30 幀 → 第 79 幀之後回 HOME）
//   第 100–103 幀：下（游標移到第 2 款）；第 110–113 幀：A（啟動第 2 款）
//   第 130 幀：HOME 鍵（馬上回 HOME）
#define SYSTEM_FRAMES 150
#define MAX_GAME_ARGS 8

static void system_input(int i)
{
    uint16_t keys = 0;
    if (i >= 10 && i < 14) keys = 1 << BTN_A;
    else if (i >= 14 && i < 50) keys = 1 << BTN_RIGHT;
    else if (i >= 50 && i < 85) keys = (1 << BTN_START) | (1 << BTN_SELECT);
    else if (i >= 100 && i < 104) keys = 1 << BTN_DOWN;
    else if (i >= 110 && i < 114) keys = 1 << BTN_A;
    else if (i == 130) keys = 1 << BTN_HOME;
    input_set(0, keys);
    input_set_stick(0, STICK_LEFT, 0, 0);
    input_set_stick(0, STICK_RIGHT, 0, 0);
}

// 這就是「平台」在 Nova 模式要做的事：每一幀之後看引擎有沒有請求，有就去載入
static int run_system(const char *sys_path, const char *out,
                      const char **ids, const char **paths, int ngames)
{
    size_t sys_len = 0;
    char *sys_src = read_file(sys_path, &sys_len);
    if (sys_src == NULL) {
        printf("讀不到系統卡匣 %s\n", sys_path);
        return 1;
    }
    for (int g = 0; g < ngames; g++) {
        // 名稱用檔名代替（真正的網頁版 Nova 會從 comet.json 的 name 來）；
        // 介紹、人數也沒有來源：介紹用 id、人數用排第幾（make check 會檢查這兩項有傳到 Lua）
        system_add_game(ids[g], paths[g]);
        system_set_info(ids[g], ids[g], g + 1);
    }

    engine_init();
    if (engine_load_system(sys_src, sys_len)) return 1;

    for (int i = 0; i < SYSTEM_FRAMES; i++) {
        system_input(i);
        engine_frame();

        int req = system_request();
        if (req == SYS_REQ_LAUNCH) {
            const char *id = system_request_game();
            const char *path = NULL;
            for (int g = 0; g < ngames; g++) {
                if (strcmp(ids[g], id) == 0) path = paths[g];
            }
            size_t len = 0;
            char *src = path ? read_file(path, &len) : NULL;
            system_request_done();
            if (src == NULL) {
                printf("第 %d 幀：讀不到遊戲 %s，留在 HOME\n", i, id);
                continue;
            }
            printf("第 %d 幀：啟動 %s\n", i, id);
            engine_load(src, len);
            free(src);
        } else if (req == SYS_REQ_HOME) {
            system_request_done();
            printf("第 %d 幀：回 HOME\n", i);
            engine_load_system(sys_src, sys_len);
        }
    }
    free(sys_src);

    if (save_ppm(out) != 0) return 1;
    printf("%s：Nova 模式跑了 %d 幀，已存成 %s\n", sys_path, SYSTEM_FRAMES, out);
    return 0;
}

// ============================================================
// 主機卡匣（.nvc）：「NOVACART」+ 開頭原始碼的長度（4 bytes）+ 開頭原始碼 + Lua 位元組碼
// 開頭原始碼只是給引擎讀遊戲 id（-- id: 那一行），程式本身在位元組碼裡
// ============================================================

#define NVC_MAGIC "NOVACART"
#define NVC_HEAD_LINES 10
#define NVC_HEAD_MAX   4096   // 主機那邊也檢查同一個上限（platform/pico/main.c 的 load_nvc）

static int write_out(void *ud, const void *p, size_t n)
{
    return fwrite(p, 1, n, (FILE *)ud) == n ? 0 : 1;
}

static int compile_nvc(const char *src, size_t len, const char *out_path)
{
    FILE *f = fopen(out_path, "wb");
    if (f == NULL) { printf("寫不出 %s\n", out_path); return 1; }
    // 開頭幾行（最多 NVC_HEAD_LINES 行、NVC_HEAD_MAX bytes）。
    // 一次加一整行，加了會超過上限就停：大遊戲開頭常有一行很長的資料（例如 Wildkin 的字型約 60 KB），
    // 主機要把開頭整段放進 RAM，不能讓它變大
    size_t head = 0;
    for (int line = 0; line < NVC_HEAD_LINES && head < len; ++line) {
        size_t end = head;
        while (end < len && src[end] != '\n') end++;
        if (end < len) end++;   // 包含換行
        if (end > NVC_HEAD_MAX) break;
        head = end;
    }
    uint32_t h32 = (uint32_t)head;
    fwrite(NVC_MAGIC, 1, 8, f);
    fwrite(&h32, 4, 1, f);
    fwrite(src, 1, head, f);
    // 位元組碼
    lua_State *L = luaL_newstate();
    char err[512];
    int r = luarom_compile(L, src, len, "=game", write_out, f, err, sizeof err);
    lua_close(L);
    fclose(f);
    if (r) { printf("編譯失敗：%s\n", err); return 1; }
    printf("已產生 %s\n", out_path);
    return 0;
}

// --rom：模擬 Flash 的唯讀區
static char *rom_base;
static size_t rom_cap = 8 * 1024 * 1024;
static const void *rom_root;
static size_t rom_used;   // 唯讀區用到哪裡（程式之後接著放圖片、字型）

// 接在唯讀區後面寫（圖片、字型、地圖、凍結的表格都用這個），寫完立刻改回唯讀
static void *rom_append(const void *data, size_t len, size_t align)
{
    size_t at = (rom_used + align - 1) / align * align;
    if (at + len > rom_cap) return NULL;
    // mprotect 以「頁」為單位（通常 4 KB 或 16 KB），範圍從頁的開頭算
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t start = at / page * page;
    mprotect(rom_base + start, at + len - start, PROT_READ | PROT_WRITE);
    memcpy(rom_base + at, data, len);
    mprotect(rom_base + start, at + len - start, PROT_READ);
    rom_used = at + len;
    return rom_base + at;
}

// 給 gfx（gfx_set_rom_store）
static const uint8_t *rom_store(const uint8_t *data, size_t len)
{
    return rom_append(data, len, 4);
}

static int rom_write(void *ud, size_t off, const void *data, size_t n)
{
    (void)ud;
    memcpy(rom_base + off, data, n);
    return 0;
}

static void rom_read(void *ud, size_t off, void *dst, size_t n)
{
    (void)ud;
    memcpy(dst, rom_base + off, n);
}

typedef struct { const char *p; size_t left; } MemReader;

static int mem_read(void *ud, void *buf, size_t n)
{
    MemReader *m = ud;
    if (n > m->left) return 1;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return 0;
}

static int rom_loader(lua_State *L, void *ud)
{
    (void)ud;
    luarom_push_main(L, rom_root);
    return LUA_OK;
}

// 讀 .nvc，把程式建進唯讀區，設好 script_load 的 loader。
// *head、*head_len：開頭原始碼（交給 engine_load 讀 id）
static int setup_rom(const char *nvc, size_t nlen, const char **head, size_t *head_len)
{
    if (nlen < 12 || memcmp(nvc, NVC_MAGIC, 8) != 0) { printf("不是 .nvc 檔\n"); return 1; }
    uint32_t h32;
    memcpy(&h32, nvc + 8, 4);
    if (12 + (size_t)h32 > nlen) { printf(".nvc 檔損壞\n"); return 1; }
    *head = nvc + 12;
    *head_len = h32;

    // mmap：向作業系統要一整塊記憶體，之後可以用 mprotect 改成唯讀
    rom_base = mmap(NULL, rom_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rom_base == MAP_FAILED) { printf("mmap 失敗\n"); return 1; }
    RomWriter w = { rom_base, rom_cap, 0, rom_write, rom_read, NULL };
    MemReader m = { nvc + 12 + h32, nlen - 12 - h32 };
    lua_State *T = luaL_newstate();   // 只是借它的設定（字串雜湊種子）
    char err[256];
    int r = luarom_build(T, mem_read, &m, &w, &rom_root, err, sizeof err);
    lua_close(T);
    if (r) { printf("%s\n", err); return 1; }
    // 改成唯讀：之後 Lua 只要寫進這塊記憶體，程式就會立刻當掉（Segmentation fault）
    mprotect(rom_base, rom_cap, PROT_READ);
    // Lua 要知道的唯讀區範圍：整塊（之後放的圖片、字型也在裡面，但 Lua 不會碰到它們）
    luarom_activate(rom_base, rom_cap);
    script_set_loader(rom_loader, NULL);
    rom_used = w.used;
    gfx_set_rom_store(rom_store);
    luarom_set_append(rom_append);
    printf("唯讀區：程式 %zu KB\n", w.used / 1024);
    return 0;
}

int main(int argc, char **argv)
{
    // 先挑出 --record／--replay，剩下的依序是遊戲檔、輸出圖檔
    const char *record_path = NULL, *replay_path = NULL, *pak_path = NULL;
    const char *input_path = NULL, *shots = NULL;
    const char *compile_path = NULL, *rom_path = NULL;
    int profile = 0;
    int system_mode = 0;
    const char *game_ids[MAX_GAME_ARGS], *game_paths[MAX_GAME_ARGS];
    int ngames = 0;
    const char *positional[2] = { "games/move_box.lua", "out.ppm" };
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
            shots = argv[++i];
        } else if (strcmp(argv[i], "--compile") == 0 && i + 1 < argc) {
            compile_path = argv[++i];
        } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--pak") == 0 && i + 1 < argc) {
            pak_path = argv[++i];
        } else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) {
            save_dir = argv[++i];
        } else if (strcmp(argv[i], "--clock") == 0 && i + 1 < argc) {
            if (parse_clock(argv[++i])) {
                printf("--clock 的格式：now 或 2026-10-22T14:05\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(argv[i], "--profile") == 0) {
            profile = 1;
        } else if (strcmp(argv[i], "--system") == 0) {
            system_mode = 1;
        } else if (strcmp(argv[i], "--game") == 0 && i + 2 < argc && ngames < MAX_GAME_ARGS) {
            game_ids[ngames] = argv[++i];
            game_paths[ngames] = argv[++i];
            ngames++;
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        }
    }
    const char *path = positional[0];
    const char *out = positional[1];

    if (system_mode) {
        return run_system(path, out, game_ids, game_paths, ngames);
    }

    size_t len = 0;
    char *src = read_file(path, &len);
    if (src == NULL) {
        printf("讀不到遊戲檔 %s\n", path);
        return 1;
    }

    if (compile_path != NULL) {
        int r = compile_nvc(src, len, compile_path);
        free(src);
        return r;
    }

    // --rom：遊戲改從 .nvc 的唯讀區執行；交給引擎的原始碼換成 .nvc 開頭那幾行
    char *nvc = NULL;
    if (rom_path != NULL) {
        size_t nlen = 0;
        nvc = read_file(rom_path, &nlen);
        if (nvc == NULL) { printf("讀不到 %s\n", rom_path); return 1; }
        const char *head;
        size_t head_len;
        if (setup_rom(nvc, nlen, &head, &head_len)) return 1;
        free(src);
        src = malloc(head_len + 1);
        memcpy(src, head, head_len);
        len = head_len;
        free(nvc);
    }

    engine_init();

    // 資源包：要在載入遊戲之前交給引擎（遊戲的指紋包含資源包）。
    // 整個程式執行期間都要用，所以不 free
    if (pak_path != NULL) {
        size_t plen = 0;
        char *pk = read_file(pak_path, &plen);
        if (pk == NULL) {
            printf("讀不到資源包 %s\n", pak_path);
            return 1;
        }
        if (pak_set((const uint8_t *)pk, plen)) return 1;
    }

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

    // 錄影：要在載入遊戲「之前」開始，_init 裡讀的存檔才會記進去
    if (record_path != NULL) {
        replay_record_begin();
    }

    if (profile) engine_profile(1);
    int err = engine_load(src, len);
    free(src);
    if (err) {
        return 1;
    }

    if (replay_path != NULL) {
        // 一直跑到播完。平台不交輸入：輸入全部來自重播檔
        while (replay_state() == REPLAY_PLAYING) {
            engine_frame();
            wav_collect();
        }
        if (replay_state() != REPLAY_FINISHED) {
            return 1;   // 播放途中發現重播檔損壞
        }
        if (save_ppm(out) != 0) {
            return 1;
        }
        if (wav_save() != 0) return 1;
        int same = replay_matched();
        printf("%s：重播 %u 幀，最後的畫面%s，已存成 %s\n",
               path, replay_frames(), same ? "一致" : "不同", out);
        return same ? 0 : 3;
    }

    int frames = SCRIPTED_FRAMES;
    if (input_path != NULL) {
        frames = load_input_script(input_path);
        if (frames < 0) return 1;
    }
    // --profile：每幀的負載，最後排出最重的 5 幀
    enum { TOP = 5 };
    uint32_t top_load[TOP] = { 0 }, top_frame[TOP] = { 0 }, top_instr[TOP] = { 0 }, top_pix[TOP] = { 0 }, top_calls[TOP] = { 0 };
    uint64_t sum_load = 0;
    size_t mem_peak = 0;
    for (int i = 0; i < frames; i++) {
        if (input_path != NULL) {
            input_set(0, script_keys(i));
            input_set_stick(0, STICK_LEFT, 0, 0);
            input_set_stick(0, STICK_RIGHT, 0, 0);
        } else {
            scripted_input(i);
        }
        engine_frame();
        wav_collect();
        if (profile) {
            size_t mem = script_mem();
            if (mem > mem_peak) mem_peak = mem;
            uint32_t ld = engine_prof_load();
            sum_load += ld;
            // 插進「最重的 5 幀」（由大到小）
            for (int k = 0; k < TOP; k++) {
                if (ld > top_load[k]) {
                    for (int m = TOP - 1; m > k; m--) {
                        top_load[m] = top_load[m - 1]; top_frame[m] = top_frame[m - 1];
                        top_instr[m] = top_instr[m - 1]; top_pix[m] = top_pix[m - 1]; top_calls[m] = top_calls[m - 1];
                    }
                    top_load[k] = ld; top_frame[k] = (uint32_t)(i + 1);
                    top_instr[k] = engine_prof_instr(); top_pix[k] = engine_prof_pixels(); top_calls[k] = engine_prof_calls();
                    break;
                }
            }
        }
        // --shots：這一幀要不要另外存圖（逗號分隔的幀數，幀數從 1 算：跑完第 N 幀）
        if (shots != NULL) {
            const char *p = shots;
            while (*p) {
                if (atoi(p) == i + 1) {
                    char shot[512];
                    const char *dot = strrchr(out, '.');
                    int base = dot ? (int)(dot - out) : (int)strlen(out);
                    snprintf(shot, sizeof(shot), "%.*s_%d.ppm", base, out, i + 1);
                    if (save_ppm(shot) != 0) return 1;
                }
                p = strchr(p, ',');
                if (p == NULL) break;
                p++;
            }
        }
    }
    if (profile && frames > 0) {
        // 記憶體：這台電腦是 64 位元，指標是 8 bytes；Pico 是 32 位元，實際會少一些（約 6–7 成）
        printf("Lua 記憶體（64 位元）：最高 %zu KB，結束時 %zu KB\n", mem_peak / 1024, script_mem() / 1024);
        printf("運算預算（Level 1 預估，100%% = Pico 一幀的時間剛好用完）：平均 %llu.%llu%%\n",
               (unsigned long long)(sum_load / frames / 10), (unsigned long long)(sum_load / frames % 10));
        for (int k = 0; k < TOP && top_load[k] > 0; k++) {
            printf("  第 %u 幀：%u.%u%%（Lua %u 指令、%u 像素、%u 次繪圖呼叫）\n", top_frame[k],
                   top_load[k] / 10, top_load[k] % 10, top_instr[k], top_pix[k], top_calls[k]);
        }
    }

    if (save_ppm(out) != 0) {
        return 1;
    }
    printf("%s：跑了 %u 幀，已存成 %s\n", path, engine_frame_count(), out);
    if (rom_base != NULL) printf("唯讀區：共 %zu KB（程式 + 圖片、字型）\n", rom_used / 1024);
    if (wav_save() != 0) return 1;

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
