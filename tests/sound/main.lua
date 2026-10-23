-- 聲音測試（make check 用）：Mac 版加 --wav 存成 WAV，tests/sound/check.mjs 檢查音高、網頁版比對逐 byte 相同
--   第 0 幀：a440（三角波 A4 = 440 Hz，整整 1 秒，聲道 0）
--   第 30 幀：blip（a440 還在響，自動找到聲道 1）　第 90 幀：boom（雜訊；a440 在第 60 幀剛好播完，聲道 0 空了）　第 120 幀：slide（鋸齒波滑音）
--   第 150 幀：hum（循環）→ 第 200 幀：sfx(nil, 聲道) 停掉　第 210 幀：music("song")　第 400 幀：music() 停掉
--   下方一排檢查方塊：綠色 = 通過，紅色 = 失敗

local checks = {}
local function check(ok) checks[#checks + 1] = ok end
local hum_ch

function _init()
  check(sfx("a440", 0) == 0)                          -- 指定聲道 0
  check(not pcall(sfx, "nothing"))                    -- 沒有這個音效：出錯
  check(not pcall(sfx, "blip", 4))                    -- 聲道只有 0–3
  check(not pcall(music, "nothing"))
end

function _update()
  local f = frame()
  if f == 30 then check(sfx("blip") == 1) end         -- 聲道 0 還在響（a440 一秒），自動用 1
  if f == 90 then check(sfx("boom") == 0) end         -- a440 在第 60 幀前剛好播完（22050 個取樣 = 60 幀），聲道 0 空了
  if f == 120 then sfx("slide") end
  if f == 150 then hum_ch = sfx("hum") end
  if f == 200 then sfx(nil, hum_ch) end
  if f == 210 then music("song") end
  if f == 300 then check(sfx("blip") ~= nil) end      -- 音樂進行中也能播音效
  if f == 400 then music() end
end

function _draw()
  cls(1)
  for i, ok in ipairs(checks) do rect(4 + (i - 1) * 8, 230, 6, 6, ok and 11 or 8) end
end
