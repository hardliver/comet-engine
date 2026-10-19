-- id: comet/save-test
-- 存檔測試（make check 用）：
--   上方一排黃色方塊 = 這是第幾次執行（每次執行，欄位 1 的 count 加 1）
--   下方一排檢查方塊：綠色 = 通過，紅色 = 失敗

local count = 0
local checks = {}

local function same(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= "table" then return a == b end
    for k, v in pairs(a) do
        if not same(v, b[k]) then return false end
    end
    for k in pairs(b) do
        if a[k] == nil then return false end
    end
    return true
end

local function check(ok)
    checks[#checks + 1] = ok
end

function _init()
    -- 執行次數：讀上一次存的，加 1 再存回去
    local s = loadsave(1)
    count = (s and s.count or 0) + 1
    save(1, { count = count, name = "野伴" })

    -- 1. 表格存進去、讀回來要一模一樣（巢狀、負數、大整數、中文、true/false、整數鍵）
    local t = {
        list = { 1, 2, -3, 1 << 40, -(1 << 62) },
        name = "大志",
        ok = true, no = false,
        nested = { a = { b = { c = "深" } } },
        [5] = "five", [-7] = "neg",
    }
    save(2, t)
    check(same(loadsave(2), t))

    -- 2. 字串、整數、true 也能直接存；存 nil = 刪除
    save(3, "純文字")
    check(loadsave(3) == "純文字")
    save(3, 42)
    check(loadsave(3) == 42)
    save(3, nil)
    check(loadsave(3) == nil)
    check(loadsave(7) == nil)   -- 從來沒存過

    -- 3. 不能存的東西要出錯
    check(not pcall(save, 4, { 1.5 }))
    check(not pcall(save, 4, function() end))
    local loop = {}
    loop.self = loop
    check(not pcall(save, 4, loop))
    check(not pcall(save, 8, 1))           -- 欄位只有 0–7
    check(not pcall(save, 4, { [true] = 1 }))
    check(loadsave(4) == nil)              -- 出錯的都沒有寫進去
end

function _update()
    -- 4. 遊戲進行中（不是 _init）讀存檔：重播時也要讀到一樣的東西
    if frame() == 30 then
        local s = loadsave(1)
        check(s ~= nil and s.count == count and s.name == "野伴")
    end
end

function _draw()
    cls(1)
    for i = 0, count - 1 do
        rect(10 + i * 12, 10, 8, 8, 10)
    end
    for i, ok in ipairs(checks) do
        rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8)
    end
end
