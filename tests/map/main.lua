-- 地圖測試：make check 用這個畫面比對網頁版和 Mac 版
-- 地圖 30×20 格（比畫面大），同時用 Tiny Town 和 Tiny Dungeon；不要隨便改

function _init()
  -- mset：(3, 3) 改成 Tiny Dungeon 第 84 格（老人）。第二張圖的編號接在第一張的 132 格後面
  mset(MAP.test, "deco", 3, 3, 133 + 84)
end

function _draw()
  cls(0)
  camera(40, 24)            -- 不是 16 的倍數：畫面邊緣的格子只露出一部分
  map(MAP.test, "ground")
  map(MAP.test, "deco")
  camera()

  -- 左下角三個方塊：mget、mapw/maph 的結果對就是綠色（11），錯就是紅色（8）
  rect(0, 232, 8, 8, mget(MAP.test, "solid", 5, 5) == 1 and 11 or 8)
  rect(8, 232, 8, 8, mget(MAP.test, "solid", -1, 0) == 0 and 11 or 8)   -- 地圖外面是 0
  rect(16, 232, 8, 8, (mapw(MAP.test) == 30 and maph(MAP.test) == 20) and 11 or 8)
end
