// make_pak.mjs — 產生資源包測試用的 build/pak_test.pak（make check 用）
//
// 背景：直條紋四色（每 80 像素換一色）。深度：畫面左半邊（x < 160）有一個 200 公分遠的遮擋物，右半邊沒有
// 角色：第 0 格 4×8 全部第 16 色，腳底在下緣中間；第 1 格 2×1，左邊第 17 色、右邊透明（測試翻轉）
// 調色盤：16、17 兩色
// 資料：一段中文字串

import fs from "fs";
import { buildPak, bgEntry, spritesEntry, palEntry, dataEntry } from "../../tools/pak.mjs";

const pixels = new Uint8Array(320 * 240);
for (let y = 0; y < 240; y++) for (let x = 0; x < 320; x++) pixels[y * 320 + x] = Math.floor(x / 80);
const depth = new Uint8Array(160 * 120);
for (let y = 0; y < 120; y++) for (let x = 0; x < 80; x++) depth[y * 160 + x] = 200 / 8;

const pak = buildPak([
    bgEntry("room", { palette: [[10, 20, 30], [40, 50, 60], [70, 80, 90], [100, 110, 120]], pixels, depth }),
    bgEntry("plain", { palette: [[200, 0, 0]], pixels: new Uint8Array(320 * 240) }),
    spritesEntry("man", [
        { w: 4, h: 8, ox: -2, oy: -8, pixels: new Uint8Array(32).fill(16) },
        { w: 2, h: 1, ox: 0, oy: -1, pixels: Uint8Array.from([17, 0]) },
    ]),
    palEntry("actors", 16, [[240, 200, 100], [0, 250, 250]]),
    dataEntry("hello", "事故報告"),
]);
fs.mkdirSync("build", { recursive: true });
fs.writeFileSync("build/pak_test.pak", pak);
console.log(`已產生 build/pak_test.pak（${pak.length} bytes）`);
