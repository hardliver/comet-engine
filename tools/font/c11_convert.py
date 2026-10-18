#!/usr/bin/env python3
"""c11_convert.py：把俐方體 11 號（TTF）轉成 Comet 的點陣字型檔 c11.bin

用法（在 comet 資料夾裡）：
    pip install fonttools
    python3 tools/font/c11_convert.py 俐方體的.ttf fonts/c11.bin

只需要執行一次，產生的 c11.bin 放進 repo。之後合併工具（cart.js）從 c11.bin
挑出遊戲用到的字，不用再碰 TTF。

原理
----
俐方體雖然是 TTF 格式，其實是點陣字：
  - 1 個字寬 1200 單位，所有輪廓的點都落在 100 的倍數上 → 100 單位 = 1 像素
  - 輪廓只有直線，沒有曲線
所以只要看「每個像素的中心點在不在輪廓裡」，就能完全精準地還原點陣。

c11.bin 的格式（數字都是 little-endian，也就是低位的 byte 在前）
----------------------------------------------------------------
  檔頭 8 byte：
    "C11F"          4 byte  檔案標記，用來確認讀到的是對的檔案
    版本            1 byte  目前是 1
    字高            1 byte  14（每個字存幾列）
    字數            2 byte
  接著每個字 33 byte，依字碼由小到大排列（之後才能用二分搜尋）：
    字碼            4 byte  Unicode 編號，例如「野」是 0x91CE
    寬度            1 byte  畫完這個字後游標往右移幾像素（中文 13、英數 4–10）
    點陣            28 byte 14 列，每列 2 byte（big-endian，最高位元 = 最左邊的像素）
"""

import struct
import sys

from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib import TTFont

UNIT = 100          # 100 單位 = 1 像素
TOP = 1000          # 第 0 列的上緣（字型座標 y 往上是正的）
ROWS = 14           # 量測：中文、英數、標點的墨跡都在 y = 1000 到 -400 之間，共 14 列
                    # （全部 10,268 字中，只有 6 個帶重音的拉丁字母如 Ń、Ŏ 頂端被切掉 1 像素）
MAX_WIDTH = 16      # 每列用 2 byte 存，所以字最寬 16 像素


def contours(glyph_set, name):
    """取出一個字的輪廓：每個輪廓是一串 (x, y) 頂點"""
    pen = RecordingPen()
    glyph_set[name].draw(pen)
    result, current = [], []
    for op, args in pen.value:
        if op == "moveTo":
            current = [args[0]]
        elif op == "lineTo":
            current.append(args[0])
        elif op == "closePath":
            result.append(current)
        else:
            raise ValueError(f"{name} 有曲線（{op}），不是點陣字")
    return result


def inside(shapes, x, y):
    """點 (x, y) 在不在輪廓裡（nonzero 規則）

    從這個點往右畫一條水平線，看它穿過幾條邊：
    由下往上穿過的邊 +1、由上往下 −1，加起來不是 0 就是在裡面。
    （字的外框和中間的洞方向相反，洞裡的點會正負抵消成 0。）
    """
    winding = 0
    for pts in shapes:
        for i in range(len(pts)):
            x1, y1 = pts[i]
            x2, y2 = pts[(i + 1) % len(pts)]
            if (y1 <= y) != (y2 <= y):                      # 這條邊跨過水平線
                cross_x = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if cross_x > x:                             # 交點在右邊
                    winding += 1 if y2 > y1 else -1
    return winding != 0


def rasterize(glyph_set, name, width):
    """回傳 14 個整數，每個代表一列；最左邊的像素是第 15 位元（0x8000）"""
    shapes = contours(glyph_set, name)
    rows = []
    for r in range(ROWS):
        y = TOP - r * UNIT - UNIT // 2          # 這一列像素中心的 y
        bits = 0
        for c in range(width):
            x = c * UNIT + UNIT // 2            # 這一格像素中心的 x
            if inside(shapes, x, y):
                bits |= 0x8000 >> c
        rows.append(bits)
    return rows


def main():
    if len(sys.argv) != 3:
        sys.exit("用法：python3 c11_convert.py 輸入.ttf 輸出.bin")
    font = TTFont(sys.argv[1])
    glyph_set = font.getGlyphSet()
    cmap = font.getBestCmap()                   # 字碼 → 字的名稱
    hmtx = font["hmtx"]                         # 字的名稱 → (寬度, 左邊距)

    glyphs = []
    for code in sorted(cmap):
        name = cmap[code]
        advance = hmtx[name][0]
        if advance % UNIT:
            sys.exit(f"U+{code:04X} 的寬度 {advance} 不是 {UNIT} 的倍數")
        width = advance // UNIT
        if width > MAX_WIDTH:
            continue                            # 太寬的特殊符號，遊戲用不到
        glyphs.append((code, width, rasterize(glyph_set, name, width)))

    with open(sys.argv[2], "wb") as f:
        f.write(b"C11F" + struct.pack("<BBH", 1, ROWS, len(glyphs)))
        for code, width, rows in glyphs:
            f.write(struct.pack("<IB", code, width))
            f.write(struct.pack(f">{ROWS}H", *rows))

    size = 8 + len(glyphs) * (5 + ROWS * 2)
    print(f"已產生 {sys.argv[2]}：{len(glyphs)} 個字，{size // 1024} KB")


if __name__ == "__main__":
    main()
