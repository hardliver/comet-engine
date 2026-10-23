// luarom.h — 把預先編譯好的 Lua 程式放進「唯讀記憶體」（主機上就是 Flash）
//
// 為什麼要這樣做：
//   Lua 載入遊戲時，會把整個程式（指令、常數、字串、除錯資訊）建在 RAM 裡。
//   Wildkin 這樣的大遊戲光是這些就要約 1 MB，Pico 2 W 只有 520 KB RAM。
//   但這些東西載入之後**不會再改變**，所以可以放在 Flash（4 MB，CPU 可以直接讀）。
//   RAM 只留給遊戲執行時真的會變的東西（變數、表格、物件）。
//
// 怎麼做到的：
//   1. 電腦先把卡匣編譯成 Lua 位元組碼（luarom_compile，Mac 版的 --compile）
//   2. 主機讀位元組碼，把每個函式（Proto）、指令、常數、字串「依序」寫進唯讀區，
//      指標直接指向唯讀區裡的最終位址（Flash 只能從頭依序寫，不能回頭改）
//   3. 唯讀區裡的物件，GC 標記欄位設成 0（「灰色」）：GC 只會處理白色物件，
//      所以永遠不會去標記、回收或寫入它們，就像不存在一樣
//   4. Lua 的短字串原本靠「同一個位址」判斷相等；唯讀區的字串不在 Lua 的字串表裡，
//      同樣內容可能在 RAM 也有一份，所以改成比內容（third_party/lua 的 LUA_ROM 修改）
//
// 寫入方式由平台決定（RomWriter）：主機寫 Flash，Mac 測試時寫一塊記憶體再設成唯讀。

#ifndef LUAROM_H
#define LUAROM_H

#include <stddef.h>
#include "lua.h"

// 平台提供的「依序寫入」：把 data 接在已寫入內容的後面（offset 一定等於目前已寫的長度）
typedef struct {
    char  *base;       // 唯讀區的位址（CPU 讀得到的位址；主機上是 Flash 映射的位址）
    size_t capacity;   // 唯讀區大小
    size_t used;       // 已經寫了多少（luarom 自己更新）
    // 寫入：成功回傳 0
    int  (*write)(void *ud, size_t offset, const void *data, size_t len);
    // 讀回剛寫的內容（比對重複字串用）；主機上還沒寫進 Flash 的部分在緩衝區裡
    void (*read)(void *ud, size_t offset, void *dst, size_t len);
    void *ud;
} RomWriter;

// 讀位元組碼：從來源讀 n bytes 到 buf，成功回傳 0
typedef int (*RomReader)(void *ud, void *buf, size_t n);

// 把位元組碼（luarom_compile 的輸出）寫進唯讀區。
// 成功回傳 0，root 是主函式在唯讀區的位址；失敗回傳非 0，err 裡有原因
int luarom_build(lua_State *L, RomReader read, void *read_ud, RomWriter *w,
                 const void **root, char *err, size_t err_size);

// 開始使用唯讀區：告訴 Lua 哪一段位址是唯讀區（字串比對要用）。
// build 之後、執行遊戲之前呼叫；不同遊戲的唯讀區可以共用同一塊
void luarom_activate(const char *base, size_t size);

// 在 L 的堆疊上放一個可以呼叫的主函式（RAM 裡的閉包，程式本體在唯讀區）。
// 效果和 luaL_loadbuffer 成功時一樣
void luarom_push_main(lua_State *L, const void *root);

// 把原始碼編譯成位元組碼，寫到 out（用 lua_dump，保留除錯資訊：錯誤訊息才有行號）。
// 成功回傳 0
int luarom_compile(lua_State *L, const char *src, size_t len, const char *name,
                   int (*out)(void *ud, const void *p, size_t n), void *out_ud,
                   char *err, size_t err_size);

#endif
