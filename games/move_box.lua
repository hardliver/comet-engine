-- move_box.lua：用方向鍵移動方塊
--
-- 引擎會呼叫這三個函式：
--   _init()    開始時一次
--   _update()  每幀一次（每秒 60 次）：讀按鍵、改狀態
--   _draw()    每幀一次：把目前狀態畫出來
--
-- 可用的函式：cls(色) pset(x,y,色) rect(x,y,寬,高,色) btn(玩家,鍵) frame()
-- 玩家從 0 開始；顏色 0–15；座標、大小都要用整數

-- 按鍵編號
local LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
local A, B, START, SELECT = 4, 5, 6, 7

local SIZE = 16    -- 方塊大小
local SPEED = 2    -- 每幀移動幾個像素

local x, y         -- 方塊左上角的位置

function _init()
  -- // 是整數除法；用 / 會得到小數
  x = (SCREEN_W - SIZE) // 2
  y = (SCREEN_H - SIZE) // 2
end

function _update()
  if btn(0, LEFT)  then x = x - SPEED end
  if btn(0, RIGHT) then x = x + SPEED end
  if btn(0, UP)    then y = y - SPEED end
  if btn(0, DOWN)  then y = y + SPEED end

  -- 不准離開畫面
  x = math.max(0, math.min(x, SCREEN_W - SIZE))
  y = math.max(0, math.min(y, SCREEN_H - SIZE))
end

function _draw()
  cls(1)                         -- 深藍背景
  rect(20, 20, 100, 60, 8)       -- 紅色方塊（裝飾）
  rect(200, 150, 60, 60, 11)     -- 綠色方塊（裝飾）

  -- A 鍵按著時方塊變黃色，平常是白色
  local color = btn(0, A) and 10 or 7
  rect(x, y, SIZE, SIZE, color)

  -- 畫面底部：8 個按鍵指示燈
  for b = 0, 7 do
    rect(8 + b * 14, SCREEN_H - 14, 10, 8, btn(0, b) and 9 or 5)
  end
end
