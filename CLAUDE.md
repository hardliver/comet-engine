# CLAUDE.md — Comet 引擎

給 Claude 的工作說明。每次開工先讀這份和 `README.md`。
**一律用中文（繁體）回覆和寫說明。**

## 這是什麼

開源遊戲主機 **Nova One** 的遊戲引擎。同一份 C 核心編譯到三個平台：
Mac（無頭模擬器，開發測試用）、網頁（WebAssembly）、Nova One 主機（Pico 2 W）。
遊戲是一個 Lua 檔，改遊戲不用重新編譯引擎。

目標是讓一般人（甚至只靠下 prompt 給 AI）幾小時到幾天就能做出遊戲，
支援 2D 和早期 DOOM 式的偽 3D。

作者 Neil：機械碩士、Python 強，**C 還在學，想透過這個專案學好 C**。
所以程式要寫得讓他看得懂，不要炫技。

## 架構原則（不要違反）

- **`core/` 是純 C 核心**：不碰檔案、視窗、硬體。平台相關的東西放 `platform/`，透過 `core/platform.h` 介面。
- **`third_party/lua/` 原則上不修改**。Lua 的設定放 `core/comet_lua_user.h`。
  唯一的例外（2026-10-23 作者同意）：`LUA_ROM`——讓程式和字串放在 Flash（見 `core/luarom.h`）。
  修改都用 `#if defined(LUA_ROM)` 包起來、加註「Comet/Nova」，目前只有 `lstring.h`、`lstring.c`、`ltable.c` 三處。
  升級 Lua 時要把這三處搬過去。
- **決定性（determinism）**：同一串按鍵，Mac 版與網頁版輸出**逐像素相同**；`rnd` 各平台結果相同；重播檔能完全重現。任何改動都不能破壞這點。
- **為主機設想**：Pico 2 W 記憶體很小。新功能要考慮記憶體用量，大資料走資源包（pak），不要假設有充足 RAM。
- **引擎是給所有遊戲用的**：Wildkin 需要的功能要做成通用 API，不要寫進只有 Wildkin 用得到的邏輯。
- 畫面 320×240、8-bit 調色盤、按鍵編號、搖桿範圍目前**暫定**，規格 v0.1 才凍結；要改先問。

## 改 API 時

- 同步更新 `docs/AI_GUIDE.md`——它同時是給 AI 寫遊戲的說明和 Studio 的「API 說明」，**內容要準確，範例要能跑**。
- README 的函式表一併更新。
- 新的 C 函式要給網頁呼叫時，記得加進 `Makefile` 的 `WEB_EXPORTS`。

## 建置與驗證

```
make             # Mac 版 → build/mac
make web         # 網頁版（需要 emscripten）
make serve       # http://localhost:8420，編輯器 /studio.html
make check       # 自動測試：Mac 與網頁一致、中文字、地圖、重播、存檔、系統卡匣、資源包
```

- 改完一定跑 `make check`，要過。
- **雲端工作階段**若沒有 emscripten：不要花太多時間安裝；至少跑 `make build/mac` 和 Mac 版測試，並在回報中**明確寫出哪些測試沒跑**。

## 寫 C 的風格（配合作者學習）

- 照現有檔案的風格：**中文註解說明「為什麼」**，不只是「做什麼」（參考 `Makefile` 的註解密度）。
- 避免巨集魔法、過度精簡的寫法；寧可多幾行清楚的程式。
- 用到記憶體配置、指標運算、`setjmp/longjmp` 等不直觀的地方，在註解說明原因和要注意的事。
- 編譯開著 `-Wall -Wextra`，不要留下警告。

## 工作方式

1. 任務來源：Wildkin 的 `docs/tasks.md`（標 **[引擎]** 的項目）和 Comet Studio 改進清單。一次只做一個。
2. Commit 訊息以任務編號開頭，例如 `W73：spr 支援旋轉`；Studio 的改進寫明是 Studio。
3. **寫完要教**：在最後的回報（雲端時寫在 PR 說明）用中文說明——
   - 做了什麼、改了哪些檔案
   - 關鍵的 C 程式為什麼這樣寫（特別是 C 特有的觀念）
   - 結構為什麼這樣設計、考慮過但沒用的做法
   - 怎麼驗證的

### Git 規則

- **作者電腦上**：直接 commit 到 `main`，**不要 push**（作者自己 push）。
- **雲端工作階段**：開分支工作，**不要動 `main`、不要合併**；推送該分支。
  和 Wildkin 一起改時，兩個 repo 用**同一個分支名稱**。作者看過後自己合併。

### 資料夾名稱

作者電腦上這個 repo 的資料夾叫 `comet`（Wildkin 的 Makefile 預設找 `../comet`）。
雲端可能被 clone 成 `comet-engine`，Wildkin 那邊用 `ENGINE=../comet-engine` 指定即可。
