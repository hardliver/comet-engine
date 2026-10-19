-- move_box.lua：移動方塊（測試所有輸入用）
-- id: comet/move-box
--
-- 引擎會呼叫這三個函式：
--   _init()    開始時一次
--   _update()  每幀一次（每秒 60 次）：讀輸入、改狀態
--   _draw()    每幀一次：把目前狀態畫出來
--
-- 左搖桿：任何角度移動，推越多走越快（十字鍵／方向也可以）
-- 右搖桿：方塊旁邊的小點指向那個方向
-- A 按著：方塊變黃
-- 畫面底部 12 格：左 右 上 下 A B Start Select X Y L R，按著會亮

local LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
local A, B, START, SELECT = 4, 5, 6, 7
local X, Y, L, R = 8, 9, 10, 11

local SIZE = 16
local MAX_SPEED = 32   -- 每幀最多移動 32/16 = 2 像素

-- 位置用「放大 16 倍的整數」存，才能表示 1.5 像素這種細微的移動
local x16, y16

function _init()
  x16 = (SCREEN_W - SIZE) // 2 * 16
  y16 = (SCREEN_H - SIZE) // 2 * 16
end

function _update()
  local lx, ly = stick(0, 0)   -- 左搖桿，-127～127

  -- 沒推搖桿時，看十字鍵／方向鍵
  if lx == 0 and ly == 0 then
    if btn(0, LEFT)  then lx = -127 end
    if btn(0, RIGHT) then lx =  127 end
    if btn(0, UP)    then ly = -127 end
    if btn(0, DOWN)  then ly =  127 end
  end

  -- 推越多走越快：搖桿值 × 最高速度 ÷ 127
  x16 = x16 + lx * MAX_SPEED // 127
  y16 = y16 + ly * MAX_SPEED // 127

  -- 不准離開畫面
  x16 = math.max(0, math.min(x16, (SCREEN_W - SIZE) * 16))
  y16 = math.max(0, math.min(y16, (SCREEN_H - SIZE) * 16))
end

function _draw()
  cls(1)                         -- 深藍背景
  rect(20, 20, 100, 60, 8)       -- 紅色方塊（裝飾）
  rect(200, 150, 60, 60, 11)     -- 綠色方塊（裝飾）

  local x, y = x16 // 16, y16 // 16

  -- A 鍵按著時方塊變黃色，平常是白色
  local color = btn(0, A) and 10 or 7
  rect(x, y, SIZE, SIZE, color)

  -- 右搖桿：在方塊中心旁邊畫一個小點，最遠離中心 16 像素
  local rx, ry = stick(0, 1)
  if rx ~= 0 or ry ~= 0 then
    local cx, cy = x + SIZE // 2, y + SIZE // 2
    rect(cx + rx * 16 // 127 - 2, cy + ry * 16 // 127 - 2, 4, 4, 14)
  end

  -- 畫面底部：12 個按鍵指示燈
  for b = 0, 11 do
    rect(8 + b * 14, SCREEN_H - 14, 10, 8, btn(0, b) and 9 or 5)
  end
end
