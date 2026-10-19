-- id: test/a
-- 系統卡匣測試用的遊戲 a：背景色 3，方塊可以左右移動
-- 左下角：碰不到系統 API（沒有 sys）就是綠色

local x = 100

function _update()
    if btn(0, 0) then x = x - 2 end
    if btn(0, 1) then x = x + 2 end
end

function _draw()
    cls(3)
    rect(x, 110, 20, 20, 7)
    rect(4, 230, 6, 6, sys == nil and 11 or 8)
end
