-- catch.lua：接住掉落的方塊（docs/AI_GUIDE.md 裡的範例）
-- 左右移動接方塊；漏接 5 個就結束，按 A 重來
-- id: comet/catch

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
