# Lua 5.4.9

來源：https://github.com/lua/lua（tag `v5.4.9`），檔案未修改。

授權：MIT，版權聲明在 `lua.h` 檔尾（Copyright © 1994–2026 Lua.org, PUC-Rio）。

## 刻意沒有放進來的檔案

| 檔案 | 原因 |
|---|---|
| `liolib.c` | `io`：讀寫檔案。遊戲不准碰檔案 |
| `loslib.c` | `os`：系統時間、執行指令。會破壞同步，也不安全 |
| `loadlib.c` | `package`／`require`：從磁碟載入模組。遊戲是單一檔案 |
| `ldblib.c` | `debug`：可以繞過沙盒 |
| `linit.c` | 預設會開啟上面那些函式庫，改由 `core/script.c` 自己挑 |
| `lua.c`、`onelua.c`、`ltests.*` | 命令列直譯器與測試用，引擎用不到 |

Comet 對 Lua 的設定（固定雜湊種子、輸出導向平台等）在 `core/comet_lua_user.h`，透過 `LUA_USER_H` 帶入，不改 Lua 原始碼。
