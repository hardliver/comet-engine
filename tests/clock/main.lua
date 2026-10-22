-- id: comet/clock-test
-- 現實時鐘測試（make check 用）：
--   ./build/mac tests/clock/main.lua 圖 --clock 2026-12-31T23:59:59   有時間來源
--   ./build/mac tests/clock/main.lua 圖                                沒有時間來源（clock() 是 nil）
--   上方一個方塊：黃色 = 有時間來源，灰色 = 沒有
--   下方一排檢查方塊：綠色 = 通過，紅色 = 失敗

local checks = {}
local first = nil      -- 第 0 幀（_init）讀到的
local has_clock = false

local function check(ok)
    checks[#checks + 1] = ok
end

-- 用另一種寫法（一年一年、一個月一個月加）算秒數，和引擎的公式互相核對
local function is_leap(y) return y % 4 == 0 and (y % 100 ~= 0 or y % 400 == 0) end
local MDAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
local function to_days(c)
    local days = 0
    if c.year >= 1970 then
        for y = 1970, c.year - 1 do days = days + (is_leap(y) and 366 or 365) end
    else
        for y = c.year, 1969 do days = days - (is_leap(y) and 366 or 365) end
    end
    for m = 1, c.month - 1 do
        days = days + MDAYS[m] + ((m == 2 and is_leap(c.year)) and 1 or 0)
    end
    return days + c.day - 1
end

-- 表格的每一格都合理，而且和 time 對得上
local function consistent(c)
    if c.month < 1 or c.month > 12 or c.day < 1 or c.day > 31 then return false end
    if c.hour < 0 or c.hour > 23 or c.min < 0 or c.min > 59 or c.sec < 0 or c.sec > 59 then return false end
    local days = to_days(c)
    return c.time == days * 86400 + c.hour * 3600 + c.min * 60 + c.sec and c.wday == (days + 4) % 7
end

function _init()
    first = clock()
    has_clock = first ~= nil
    if not has_clock then return end
    check(consistent(first))
    -- --clock 2026-12-31T23:59:59：星期四
    check(first.year == 2026 and first.month == 12 and first.day == 31)
    check(first.hour == 23 and first.min == 59 and first.sec == 59 and first.wday == 4)
    -- 同一幀再讀一次：一樣的值（但是新的表格）
    local again = clock()
    check(again ~= first and again.time == first.time)
end

function _update()
    local c = clock()   -- 每一幀都讀：大部分的幀和上一次一樣，重播檔裡不會有事件
    if not has_clock then
        if frame() == 30 then check(c == nil) end
        return
    end
    if frame() == 59 then check(c.time == first.time) end   -- 還沒滿 1 秒
    if frame() == 60 then
        -- 過了 1 秒：跨年。2027-01-01 00:00:00 星期五
        check(c.time == first.time + 1 and consistent(c))
        check(c.year == 2027 and c.month == 1 and c.day == 1 and c.hour == 0 and c.min == 0 and c.sec == 0)
        check(c.wday == 5)
    end
end

function _draw()
    cls(1)
    rect(10, 10, 8, 8, has_clock and 10 or 6)
    for i, ok in ipairs(checks) do
        rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8)
    end
end
