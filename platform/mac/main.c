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
//   加上 --saves 資料夾：存檔寫在那個資料夾（檔名「id.欄位.sav」，id 的 / 換成 _）。
//   沒加就只存在記憶體裡，程式結束就沒了（測試時不會留下檔案）
//   ./build/mac 系統卡匣.lua 輸出圖檔 --system --game id 遊戲.lua [--game id 遊戲.lua …]
//                                                    Nova 模式：載入系統卡匣，用另一組預先寫好的按鍵
//                                                    跑 150 幀（選遊戲、啟動、回 HOME），測試系統卡匣
// 沒給遊戲檔、圖檔就用 games/move_box.lua、out.ppm
//
// --replay 的結束代碼：0 = 一致，3 = 畫面不同，1 = 其他錯誤。
// 之後的成就驗證（GitHub Actions）就是跑這個模式。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/engine.h"
#include "../../core/gfx.h"
#include "../../core/input.h"
#include "../../core/pak.h"
#include "../../core/platform.h"
#include "../../core/replay.h"
#include "../../core/save.h"
#include "../../core/system.h"

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

int main(int argc, char **argv)
{
    // 先挑出 --record／--replay，剩下的依序是遊戲檔、輸出圖檔
    const char *record_path = NULL, *replay_path = NULL, *pak_path = NULL;
    const char *input_path = NULL, *shots = NULL;
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
        } else if (strcmp(argv[i], "--pak") == 0 && i + 1 < argc) {
            pak_path = argv[++i];
        } else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) {
            save_dir = argv[++i];
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

    int err = engine_load(src, len);
    free(src);
    if (err) {
        return 1;
    }

    if (replay_path != NULL) {
        // 一直跑到播完。平台不交輸入：輸入全部來自重播檔
        while (replay_state() == REPLAY_PLAYING) {
            engine_frame();
        }
        if (replay_state() != REPLAY_FINISHED) {
            return 1;   // 播放途中發現重播檔損壞
        }
        if (save_ppm(out) != 0) {
            return 1;
        }
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
    for (int i = 0; i < frames; i++) {
        if (input_path != NULL) {
            input_set(0, script_keys(i));
            input_set_stick(0, STICK_LEFT, 0, 0);
            input_set_stick(0, STICK_RIGHT, 0, 0);
        } else {
            scripted_input(i);
        }
        engine_frame();
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

    if (save_ppm(out) != 0) {
        return 1;
    }
    printf("%s：跑了 %u 幀，已存成 %s\n", path, engine_frame_count(), out);

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
