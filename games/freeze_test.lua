-- freeze_test.lua：freeze() 的自動測試（make check 用）
-- id: comet/freeze-test
-- 結果用 print 印出；最後一行是 "FREEZE OK" 或 "FREEZE FAIL"

local fails = 0
local function check(name, ok)
  if not ok then
    fails = fails + 1
    print("FAIL " .. name)
  end
end

-- 會出錯的動作：要出錯，而且訊息要說是凍結的表格
local function must_fail(name, fn, word)
  local ok, err = pcall(fn)
  check(name, not ok and tostring(err):find(word or "frozen") ~= nil)
end

function _init()
  local src = {
    name = "wildkin",
    list = { 10, 20, 30, 40 },
    kin = {
      { id = "sprout", hp = 12, moves = { "tackle", "absorb" } },
      { id = "ember",  hp = 10, moves = { "scratch", "ember" } },
    },
    by_id = {},
    [7] = "seven",
    [2.5] = "half",
    flag = true,
    long = string.rep("x", 100),   -- 長字串
  }
  src.by_id.sprout = src.kin[1]      -- 同一個子表格被參照兩次
  src.by_id.ember = src.kin[2]

  -- 凍結前記下 pairs 的順序
  local before = {}
  for k in pairs(src) do before[#before + 1] = tostring(k) end

  local t = freeze(src)

  -- 讀取
  check("string field", t.name == "wildkin")
  check("array", t.list[3] == 30 and #t.list == 4)
  check("nested", t.kin[2].moves[2] == "ember" and t.kin[1].hp == 12)
  check("integer key", t[7] == "seven")
  check("float key", t[2.5] == "half")
  check("bool", t.flag == true)
  check("long string", t.long == string.rep("x", 100))
  check("shared subtable", t.by_id.sprout == t.kin[1])
  -- 用執行時才組出來的字串（在 RAM）查凍結的表格（字串在唯讀區）
  local key = "na" .. "me"
  check("runtime key", t[key] == "wildkin")
  local id = string.format("%s", "ember")
  check("runtime key 2", t.by_id[id].hp == 10)
  -- 凍結的字串當成一般表格的 key
  local m = {}
  m[t.name] = 1
  check("frozen string as key", m["wild" .. "kin"] == 1)

  -- pairs 順序不變、ipairs
  local after = {}
  for k in pairs(t) do after[#after + 1] = tostring(k) end
  check("pairs order", table.concat(before, ",") == table.concat(after, ","))
  local sum = 0
  for _, v in ipairs(t.list) do sum = sum + v end
  check("ipairs", sum == 100)

  -- 寫入一律出錯
  must_fail("set field", function() t.name = "x" end)
  must_fail("new field", function() t.newkey = 1 end)
  must_fail("set index", function() t.list[1] = 0 end)
  must_fail("append", function() t.list[5] = 50 end)
  must_fail("nested set", function() t.kin[1].hp = 99 end)
  must_fail("rawset", function() rawset(t, "name", "x") end)
  must_fail("table.insert", function() table.insert(t.list, 5) end)
  must_fail("table.remove", function() table.remove(t.list) end)
  must_fail("table.sort", function() table.sort(t.list, function(a, b) return a > b end) end)
  must_fail("setmetatable", function() setmetatable(t, {}) end)
  -- 讀取沒有被弄壞
  check("still intact", t.list[1] == 10 and #t.list == 4 and t.name == "wildkin")

  -- 凍結的表格當 metatable（__index）
  local proto = freeze({ __index = { greet = "hi" } })
  local obj = setmetatable({}, proto)
  check("frozen metatable", obj.greet == "hi")

  -- 不能凍結的東西
  must_fail("function inside", function() freeze({ f = function() end }) end, "freeze")
  local cyc = {}
  cyc.self = cyc
  must_fail("cycle", function() freeze(cyc) end, "freeze")
  must_fail("metatable", function() freeze(setmetatable({}, {})) end, "freeze")

  -- 已經凍結的表格再凍結：回傳同一個
  check("refreeze", freeze(t) == t)

  -- 原本的表格不受影響，還可以改
  src.name = "changed"
  check("source still writable", src.name == "changed" and t.name == "wildkin")

  collectgarbage()   -- GC 不能碰凍結的表格
  check("after gc", t.kin[1].moves[1] == "tackle")

  print(fails == 0 and "FREEZE OK" or "FREEZE FAIL")
end

function _draw()
  cls(fails == 0 and 3 or 8)   -- 綠色 = 通過，紅色 = 失敗
end
