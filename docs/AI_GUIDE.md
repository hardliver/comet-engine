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
| `stick(玩家, n)` | 搖桿位置，回傳 `x, y` 兩個整數（-127～127）。n：0 = 左搖桿，1 = 右搖桿 |
| `rnd(n)` | 回傳 0 到 n-1 的隨機整數（n ≥ 1） |
| `frame()` | 遊戲開始後經過了幾幀（整數） |
| `pal(色, r, g, b)` | 設定第幾號顏色（0–255），r g b 是 0–255 |
| `camera(x, y)` | 之後的繪圖都往左上平移 (x, y)，用來捲動地圖；`camera()` 歸零。`cls` 不受影響 |
| `spr(圖, n, x, y, 左右翻, 上下翻)` | 畫圖片的第 n 格（小格通常 16×16，由左到右、由上到下從 0 數）。翻轉參數可省略 |
| `sspr(圖, sx, sy, sw, sh, dx, dy, dw, dh, 左右翻, 上下翻)` | 畫圖片的任意範圍；dw、dh 是畫出來的大小，可以放大（省略就是原尺寸） |
| `text(字串, x, y, 色)` | 在畫面上畫文字（中文、英數都可以），`\n` 換行，回傳寬度（像素）。色省略時是 7（白）。字串也可以是數字 |
| `textw(字串)` | 文字畫出來有多寬（像素），不畫。用來置中、排版 |
| `map(地圖, 圖層, ox, oy)` | 畫地圖的一個圖層，地圖左上角放在 (ox, oy)（省略是 0, 0）。受 `camera` 影響，只畫看得到的格子 |
| `mget(地圖, 圖層, cx, cy)` | 第 cx 欄、第 cy 列那一格的編號（0 = 空的；地圖外面也是 0） |
| `mset(地圖, 圖層, cx, cy, 編號)` | 改一格（例如砍掉擋路的樹） |
| `mapw(地圖)`、`maph(地圖)` | 地圖幾格寬、幾格高 |
| `save(欄位, 值)` | 存檔。欄位 0–7；值可以是表格、字串、整數、`true`/`false`（表格裡也只能放這些）。`save(欄位, nil)` 刪除 |
| `loadsave(欄位)` | 讀回存的值；沒有存檔回傳 `nil` |
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
| L（左肩鍵） | 10 | R（右肩鍵） | 11 |

建議在檔案開頭定義名稱：

```lua
local LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
local A, B, START, SELECT = 4, 5, 6, 7
local X, Y, L, R = 8, 9, 10, 11
```

- 方向鍵（0–3）：十字鍵按著，**或左搖桿推超過一半**，都會回傳 `true`
- **不要把「同時按住 Start＋Select」當成遊戲操作**：在 Nova 裡，按住 0.5 秒會回主選單（HOME）。分開按 Start、Select 沒有問題
- **主要動作用 A、B**：紅白機手把只有方向、A、B、Start、Select。用到 X、Y、L、R 或搖桿的遊戲，在紅白機手把上無法完整遊玩
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

### 搖桿

`stick(玩家, n)` 回傳 `x, y`：往右、往下是正的（和畫面座標方向相同），放開時是 `0, 0`。
任何角度都可以，推越多數值越大。

```lua
-- 用左搖桿移動，推越多走越快；位置用「放大 16 倍的整數」
local lx, ly = stick(0, 0)
x16 = x16 + lx * 32 // 127      -- 最快每幀 2 像素
y16 = y16 + ly * 32 // 127

-- 右搖桿：瞄準方向
local rx, ry = stick(0, 1)
if rx ~= 0 or ry ~= 0 then
  -- 朝 (rx, ry) 的方向射擊
end
```

沒有搖桿的手把（紅白機），`stick` 永遠回傳 `0, 0`。需要搖桿的遊戲，建議也讓方向鍵能移動：
搖桿是 `0, 0` 時，改看 `btn(0, LEFT)` 等方向鍵。

### 玩家在電腦上的鍵盤配置（參考用，遊戲不用處理）

| 鍵盤 | 對應 |
|---|---|
| W A S D | 左搖桿（也算方向鍵） |
| ↑ ↓ ← → | 右搖桿 |
| K／空白鍵、L、J、I | A、B、X、Y |
| Q、E | L、R |
| Enter、Tab | Start、Select |

### 文字

```lua
text("野生的 囤囤鼠 出現了！", 10, 200, 7)
text("HP " .. hp .. "/" .. max_hp, 10, 216, 7)
local w = textw("遊戲結束")
text("遊戲結束", (SCREEN_W - w) // 2, 110, 8)   -- 水平置中
```

- 字型是俐方體 11 號：**中文字寬 13、高 14 像素**；英數字寬 4–10（比例字型）。一行建議間隔 16 像素
- 一行最多約 23 個中文字（320 ÷ 13，扣掉邊框更少），太長要自己換行（`\n`）或分頁
- 文字和其他繪圖一樣受 `camera` 影響。畫在固定位置的介面（血條、對話框）先 `camera()` 歸零再畫
- 字型是**合併時自動加進遊戲檔的，只收原始碼「字串」裡出現過的字**（註解裡的字不算）。
  英數字和半形符號一定有。中文字只要在某個 `"字串"` 裡出現過就可以，不一定要在同一個地方。
  完全沒出現過的字（例如用程式拼出來的字碼）會畫成方框 □

### 地圖

地圖放在專案的 JSON 檔（通常用 Comet Studio 的地圖編輯器畫），在 `comet.json` 的 `maps` 列出來：

```json
"maps": { "camp": "maps/camp.json" }
```

遊戲裡用 `MAP.camp` 取得地圖。地圖有好幾個**圖層**，名字由地圖檔決定（例如 `ground` 地面、`deco` 裝飾、`solid` 碰撞）：

```lua
function _draw()
  cls()
  camera(cam_x, cam_y)          -- 捲動地圖：相機跟著主角
  map(MAP.camp, "ground")
  map(MAP.camp, "deco")
  -- 畫角色……
  camera()                      -- 介面（對話框、血條）固定在畫面上
end

-- 碰撞：主角要走到的那一格是不是牆。像素座標 // 16 = 第幾格
local function blocked(px, py)
  return mget(MAP.camp, "solid", px // 16, py // 16) ~= 0
end
```

- 格子編號：0 = 空的。地圖用的第 1 張圖，第 n 格的編號是 n + 1；第 2 張圖接在第 1 張的最後一格後面
- 一格的大小就是圖片的小格大小（通常 16 × 16）
- `mset` 只改記憶體裡的地圖，不會存檔；重新開始遊戲就恢復原狀

#### 地圖物件

地圖上的 NPC、告示牌、門等「物件」在地圖編輯器裡放，遊戲裡用 `MAPOBJ.地圖名` 取得，是一般的 Lua 表格陣列。
每個物件都有 `type`、`x`、`y`（第幾欄、第幾列，不是像素），其他欄位依種類而定：

```lua
for _, o in ipairs(MAPOBJ.camp) do
  if o.type == "npc" then
    -- o.name、o.tile（地圖的格子編號）、o.talk
  elseif o.type == "sign" then
    -- o.text
  elseif o.type == "door" then
    -- o.to（地圖名）、o.tx、o.ty（到達的第幾欄、第幾列）
  end
end
```

- 物件的種類：`npc`、`sign`（告示牌）、`door`（門）、`block`（擋路物：`tile`、`need`）、`start`（起點）
- 物件的文字（告示牌內容等）也會收進字型，可以直接 `text()` 畫出來
- 像素位置 = 第幾格 × 16

### 圖片

圖片放在專案資料夾，在 `comet.json` 的 `images` 列出來，合併時會自動轉換：

```json
"images": {
  "town": "assets/tilemap.png",
  "monsters": { "file": "assets/monsters.png", "tile": 32 }
}
```

遊戲裡用 `IMG.town`、`IMG.monsters` 取得圖片，再用 `spr`、`sspr` 畫：

```lua
spr(IMG.town, 0, x, y)                     -- 第 0 格（16×16）
spr(IMG.hero, 3, x, y, true)               -- 第 3 格，左右翻轉（角色往左走）
sspr(IMG.monsters, 0, 0, 32, 32, 40, 30, 64, 64)   -- 32×32 的範圍放大成 64×64
```

- PNG 裡透明的地方畫的時候會跳過（顏色編號 0 = 透明）
- 圖片的顏色會自動加進調色盤（從 16 號開始），所有圖片加起來最多 240 種顏色
- `tile` 是 `spr` 用的小格大小，預設 16

### 存檔

```lua
-- 存：把要留下來的狀態整理成一個表格
save(1, { name = "大志", level = 5, items = { 3, 3, 7 }, flags = { met_elder = true } })

-- 讀：通常在 _init，沒有存檔就從新遊戲開始
local s = loadsave(1)
if s then
    level = s.level
else
    level = 1
end
```

- 每個欄位最多 16 KB；太大、或放了小數、函式、表格裡放自己，`save` 會出錯
- 表格的鍵只能是整數或字串
- 存檔屬於哪個遊戲，看遊戲檔開頭的 `-- id: 作者/遊戲`（由 `comet.json` 的 `id` 自動寫入）。單一檔案的遊戲請自己在第一行寫上
- 讀回來的是新的表格，不是存進去的那一個；存檔版本升級時，記得處理舊存檔缺少的欄位（`s.gold or 0`）

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
3. 隨機數只能用 `rnd(n)`。**沒有** `math.random`。每次開局的亂數都不同（種子由引擎決定並記在重播檔裡），所以同一場遊戲可以完整重播
4. **沒有** `io`、`os`、`require`、`dofile`、`loadfile`、`debug`，不能讀寫檔案、不能取得系統時間。要留下資料用 `save`／`loadsave`
5. 不要依賴 `pairs()` 走訪表格的順序；需要固定順序時用陣列和 `ipairs()`
6. 目前**沒有**音效

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
