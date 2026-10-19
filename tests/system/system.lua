-- id: comet/system-test
-- 系統卡匣測試（make check 用）：一個最簡單的「大廳」
--   每款遊戲一條橫條，游標所在的是黃色；剛剛玩過的那款左邊有綠色方塊
--   左下角檢查方塊：綠色 = 通過，紅色 = 失敗

local list = {}
local cursor = 1
local last = nil
local checks = {}
local was_down, was_a = false, false

function _init()
    list = sys.games()
    last = sys.last_game()
    -- 回到 HOME 時，游標停在剛剛玩的那款
    for i, g in ipairs(list) do
        if g.id == last then cursor = i end
    end
    checks[1] = #list == 2 and list[1].id == "test/a" and list[2].id == "test/b"
    checks[2] = sys.launch("沒有這款") == false
end

function _update()
    local down, a = btn(0, 3), btn(0, 4)
    if down and not was_down then cursor = cursor % #list + 1 end
    if a and not was_a then sys.launch(list[cursor].id) end
    was_down, was_a = down, a
end

function _draw()
    cls(1)
    for i, g in ipairs(list) do
        rect(40, 20 + i * 30, 240, 20, i == cursor and 10 or 6)
        if g.id == last then rect(20, 24 + i * 30, 12, 12, 11) end
    end
    for i, ok in ipairs(checks) do
        rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8)
    end
end
