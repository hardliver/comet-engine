# Comet 遊戲製作說明（給 AI 與創作者）

你要用 **Lua 5.4** 為 Comet 引擎寫一款遊戲。Comet 是開源遊戲主機 Nova One 的引擎，
同一個遊戲檔可以在網頁和實體主機上玩。請嚴格遵守本文件的規則，只使用這裡列出的函式。

## 遊戲的結構

整款遊戲是**一個 `.lua` 檔**。引擎會呼叫以下三個全域函式（都可以省略）：

```lua
function _init()    -- 開始時呼叫一次：設定初始狀態
end

function _update()  -- 每秒 60 次：讀按鍵、移動、判斷碰撞、計分
end

function _draw()    -- 每次 _update 之後：清畫面、畫出目前狀態
end
```

- 遊戲狀態放在檔案最上方的 `local` 變數裡，在 `_init` 設定初始值
- 邏輯只寫在 `_update`，`_draw` 只負責畫，不要在 `_draw` 裡改狀態
- 時間用「幀」計算：60 幀 = 1 秒。不要用系統時間

## 畫面

- 解析度 **320 × 240**，左上角是 (0, 0)，x 向右、y 向下
- 全域常數 `SCREEN_W` = 320、`SCREEN_H` = 240
- 超出畫面的部分會自動裁掉，不會出錯
- 每幀通常先 `cls()` 清畫面再畫

### 調色盤（顏色編號 0–15）

| 編號 | 顏色 | 編號 | 顏色 |
|---|---|---|---|
| 0 | 黑 | 8 | 紅 |
| 1 | 深藍 | 9 | 橘 |
| 2 | 深紫 | 10 | 黃 |
| 3 | 深綠 | 11 | 綠 |
| 4 | 棕 | 12 | 藍 |
| 5 | 深灰 | 13 | 灰紫 |
| 6 | 淺灰 | 14 | 粉紅 |
| 7 | 白 | 15 | 膚色 |

## 函式（API）

| 函式 | 說明 |
|---|---|
| `cls(色)` | 用一個顏色清除整個畫面。省略時為 0（黑） |
| `pset(x, y, 色)` | 畫一個點 |
| `rect(x, y, 寬, 高, 色)` | 畫實心矩形，(x, y) 是左上角 |
| `btn(玩家, 鍵)` | 該按鍵正按著回傳 `true`，否則 `false` |
| `rnd(n)` | 回傳 0 到 n-1 的隨機整數（n ≥ 1） |
| `frame()` | 遊戲開始後經過了幾幀（整數） |
| `print(...)` | 印出訊息到除錯主控台（不會顯示在遊戲畫面上） |

除此之外只能用 Lua 內建的：`math`（沒有 `random`）、`string`、`table`、`utf8`、`coroutine`，
以及 `pairs`、`ipairs`、`tostring`、`tonumber`、`type`、`select`、`pcall`、`error`、`setmetatable` 等基本函式。

### 按鍵

`btn(玩家, 鍵)` 的玩家編號 0–3（單人遊戲用 0）。鍵的編號：

| 鍵 | 編號 | 鍵 | 編號 |
|---|---|---|---|
| 左 | 0 | A | 4 |
| 右 | 1 | B | 5 |
| 上 | 2 | Start | 6 |
| 下 | 3 | Select | 7 |
| X | 8 | Y | 9 |

建議在檔案開頭定義名稱：

```lua
local LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
local A, B, START, SELECT, X, Y = 4, 5, 6, 7, 8, 9
```

- **主要動作用 A、B**：紅白機手把只有 A、B，用到 X、Y 的遊戲在紅白機手把上無法完整遊玩
- `btn` 回傳「目前是否按著」。要偵測「剛按下的那一幀」，自己記住上一幀的狀態：

```lua
local prev_a = false
function _update()
  local a = btn(0, A)
  if a and not prev_a then
    -- 剛按下 A
  end
  prev_a = a
end
```

## 必須遵守的規則

1. **座標、大小、顏色一律用整數**。除法用 `//`（整數除法），不要用 `/`。
   傳入有小數的數字（例如 `rect(1.5, 0, 4, 4, 7)`）會直接出錯
2. 位置、速度若需要比 1 像素更細的精度，用「放大 16 倍的整數」儲存，畫的時候再 `// 16`：

   ```lua
   local x16 = 100 * 16   -- 實際位置 100
   local vx16 = 24        -- 每幀移動 1.5 像素
   x16 = x16 + vx16
   rect(x16 // 16, y, 8, 8, 7)
   ```
3. 隨機數只能用 `rnd(n)`。**沒有** `math.random`
4. **沒有** `io`、`os`、`require`、`dofile`、`loadfile`、`debug`，不能讀寫檔案、不能取得系統時間
5. 不要依賴 `pairs()` 走訪表格的順序；需要固定順序時用陣列和 `ipairs()`
6. 目前**沒有**文字繪製、圖片（sprite）、音效。分數等數字要顯示時，用 `rect` 畫長條或方塊代替

## 完整範例：接住掉落的方塊

```lua
local LEFT, RIGHT, A = 0, 1, 4

local player_x, score, misses
local drops = {}            -- 每個元素 { x = , y = , speed = }

function _init()
  player_x = SCREEN_W // 2 - 16
  score, misses = 0, 0
  drops = {}
end

function _update()
  if btn(0, LEFT)  then player_x = player_x - 3 end
  if btn(0, RIGHT) then player_x = player_x + 3 end
  player_x = math.max(0, math.min(player_x, SCREEN_W - 32))

  -- 每 40 幀掉一個新方塊
  if frame() % 40 == 0 then
    drops[#drops + 1] = { x = rnd(SCREEN_W - 8), y = 0, speed = 1 + rnd(3) }
  end

  -- 從後面往前走，才能安全地刪除元素
  for i = #drops, 1, -1 do
    local d = drops[i]
    d.y = d.y + d.speed
    local caught = d.y + 8 >= 220 and d.y <= 226
                   and d.x + 8 > player_x and d.x < player_x + 32
    if caught then
      score = score + 1
      table.remove(drops, i)
    elseif d.y > SCREEN_H then
      misses = misses + 1
      table.remove(drops, i)
    end
  end

  if misses >= 5 and btn(0, A) then _init() end
end

function _draw()
  cls(1)
  for _, d in ipairs(drops) do rect(d.x, d.y, 8, 8, 10) end
  rect(player_x, 220, 32, 6, 7)

  -- 分數：每接到一個畫一小格（沒有文字功能）
  for i = 0, math.min(score, 60) - 1 do rect(4 + i * 5, 4, 4, 4, 11) end
  for i = 0, misses - 1 do rect(SCREEN_W - 10 - i * 6, 4, 4, 4, 8) end

  if misses >= 5 then rect(100, 100, 120, 40, 8) end   -- 遊戲結束，按 A 重來
end
```

## 回覆格式

請回覆**一個完整的 `.lua` 檔**，放在一個 ```` ```lua ```` 程式碼區塊裡，不要只給片段，
這樣使用者可以直接貼進編輯器執行。程式碼的註解請用繁體中文。
