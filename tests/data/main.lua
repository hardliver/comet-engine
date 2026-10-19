-- 資料檔測試（make check 用）：comet.json 的 data 合併成 DATA 表格
-- 左下角檢查方塊：綠色 = 通過，紅色 = 失敗；上方畫出野伴名稱（確認資料檔裡的字有收進字型）

local checks = {}
local function check(ok) checks[#checks + 1] = ok end

function _init()
  local s = DATA.species
  check(#s == 2)
  check(s[1].name == "芽角羊" and s[1].hp == 45 and s[1].type == "grass")
  check(s[1].moves[2] == "藤鞭" and #s[2].moves == 1)
  check(s[2].work.lumber == 1 and s[1].work.carry == nil)
  check(math.type(s[2].id) == "integer")
  local c = DATA.config
  check(c.start_level == 5 and c.debug == false)
  check(c["end"] == "保留字也可以當 key" and c["with space"] == -3)
end

function _draw()
  cls(1)
  for i, sp in ipairs(DATA.species) do
    text(sp.name .. " HP " .. sp.hp, 10, 10 + (i - 1) * 16, 7)
  end
  for i, ok in ipairs(checks) do
    rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8)
  end
end
