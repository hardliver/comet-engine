-- id: comet/pak-test
-- 資源包測試（make check 用；資源包由 tests/pak/make_pak.mjs 產生）
-- 畫面上：條紋背景、幾個角色（被擋住、沒被擋、放大、翻轉），左下角一排檢查方塊：綠色 = 通過，紅色 = 失敗

local checks = {}
local man

local function check(ok)
    checks[#checks + 1] = ok
end

function _init()
    check(pakhas("room"))
    check(not pakhas("nothing"))
    check(pakdata("hello") == "事故報告")
    -- 種類不對、名稱不存在都要出錯（不是默默回傳）
    check(not pcall(bgload, "man"))
    check(not pcall(sprload, "nothing"))

    bgload("plain")
    bgload("room")              -- 第二張蓋掉第一張
    palload("actors")
    man = sprload("man")
    check(sprcount(man) == 2)
    local w, h, ox, oy = sprinfo(man, 0)
    check(w == 4 and h == 8 and ox == -2 and oy == -8)
    check(bgdepth(10, 10) == 200)
    check(bgdepth(200, 10) == 0)
    -- 染色：第 16 色變一半亮。重複呼叫不會越來越暗（每次都從原本的顏色算）
    paltint(16, 1, 128, 128, 128)
    paltint(16, 1, 128, 128, 128)
end

function _draw()
    bg()
    sprdraw(man, 0, 80, 100, 256, false, 300)    -- 在遮擋物後面：看不到
    sprdraw(man, 0, 90, 100, 256, false, 100)    -- 在遮擋物前面：看得到
    sprdraw(man, 0, 200, 100, 512, false, 300)   -- 右半邊沒有遮擋物，放大兩倍
    sprdraw(man, 1, 250, 150, 256, true)         -- 翻轉：第 17 色在腳底左邊一格
    sprdraw(man, 1, 260, 150)                    -- 不翻轉：第 17 色在腳底那一格
    for i, ok in ipairs(checks) do
        rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8)
    end
end
