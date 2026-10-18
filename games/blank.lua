-- 新遊戲
-- 說明文件：docs/AI_GUIDE.md（編輯器裡按「API」可以看）

local LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
local A, B, START, SELECT, X, Y = 4, 5, 6, 7, 8, 9

function _init()
  -- 開始時一次：設定初始狀態
end

function _update()
  -- 每秒 60 次：讀按鍵、改狀態
end

function _draw()
  cls(1)
  rect(150, 110, 20, 20, 7)
end
