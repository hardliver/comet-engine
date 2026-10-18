# Comet

Nova One 的遊戲引擎。同一份 C 核心，編譯到三個平台：Mac（開發測試）、網頁（WebAssembly）、Nova One 主機（Pico 2 W）。

遊戲是一個 Lua 文字檔，改遊戲不用重新編譯引擎。

**Comet Studio**（`make serve` 後開 `/studio.html`）：在瀏覽器裡寫遊戲，左邊程式碼、右邊立刻執行；可以一鍵把說明文件和程式碼複製給 AI，再把 AI 的回覆貼回來。

## 結構

```
core/                純 C 核心：不碰檔案、視窗、硬體
  engine             一幀的流程：呼叫遊戲的 _update、_draw
  script             Lua 與引擎的橋接、沙盒
  gfx                畫面陣列（320×240，8-bit 調色盤）與繪圖函式
  input              按鍵狀態：平台寫入、遊戲讀取
  platform.h         核心需要平台提供的功能（印訊息）
  comet_lua_user.h   對 Lua 的設定（固定雜湊種子、輸出導向）
third_party/lua/     Lua 5.4.9 原始碼（未修改，見該資料夾 README）
games/               範例遊戲（move_box、catch、blank）
docs/
  AI_GUIDE.md        寫遊戲的說明文件：給 AI 也給人看，編輯器的「API 說明」就是這份
platform/
  mac/               用預設按鍵跑 90 幀，最後一幀存成 PPM（測試用）
  web/
    comet.js         載入引擎、遊戲迴圈（遊玩頁和編輯器共用）
    input.js         鍵盤、手把、觸控
    index.html       遊玩頁（手機可用）
    studio.html      編輯器 Comet Studio
    vendor/          CodeMirror（編輯器元件，已打包）
tools/
  check_web.mjs      網頁版自動測試
  editor/            重新打包 CodeMirror 用（平常不需要）
```

## 執行

```
make                      # Mac 版
./build/mac games/xxx.lua # 跑指定遊戲，產生 out.ppm
make serve                # 網頁版：瀏覽器開 http://localhost:8420
make check                # 自動測試
```

網頁版需要 emscripten（`brew install emscripten`）。

## 寫遊戲

```lua
function _init()   end   -- 開始時一次
function _update() end   -- 每幀一次：讀按鍵、改狀態
function _draw()   end   -- 每幀一次：畫畫面
```

完整說明在 [docs/AI_GUIDE.md](docs/AI_GUIDE.md)。

| 函式 | 說明 |
|---|---|
| `cls(色)` | 清除畫面 |
| `pset(x, y, 色)` | 畫一個點 |
| `rect(x, y, 寬, 高, 色)` | 畫實心矩形 |
| `btn(玩家, 鍵)` | 按著回傳 `true`。玩家 0–3；鍵 0–11：左 右 上 下 A B Start Select X Y L R |
| `stick(玩家, n)` | 搖桿 x, y（-127～127）；n = 0 左、1 右 |
| `rnd(n)` | 0 到 n-1 的隨機整數（各平台結果相同） |
| `frame()` | 目前第幾幀 |
| `SCREEN_W`、`SCREEN_H` | 320、240 |

- 座標、大小、顏色都要用整數（`//` 是整數除法）
- 不能用：`io`、`os`、`require`、`dofile`、`loadfile`、`debug`、`math.random`

## 狀態

- 畫面格式 320×240、8-bit 調色盤、按鍵編號（目前 12 個，最多 16 個）、搖桿範圍（±127）皆為暫定，規格 v0.1 才凍結
- 同一串按鍵，Mac 版與網頁版輸出逐像素相同（`make check`）
- 網頁輸入：
  - 鍵盤：WASD = 左搖桿、方向鍵 = 右搖桿、K/L/J/I = A/B/X/Y（空白鍵也是 A）、Q/E = L/R、Enter = Start、Tab = Select
  - 手把（Gamepad API，最多 4 支）：按鍵、十字鍵、雙搖桿類比值
  - 手機觸控：現代（雙搖桿 + 十字鍵 + ABXY + L/R，Xbox 手把的位置）、紅白機（十字鍵 + B A）
- 手機全螢幕：Android 按「全螢幕」；iPhone 用 Safari「加入主畫面」
