// pak.mjs — 產生資源包（.pak）的小工具，格式見 core/pak.h
//
// 遊戲可以用任何語言自己產生 .pak（例如用 Python 從 Blender 的算圖結果產生），
// 這裡是 JavaScript 版，給測試和用 Node.js 寫工具的遊戲用：
//
//   import { buildPak, bgEntry, spritesEntry, palEntry, dataEntry } from "./pak.mjs";
//   const bytes = buildPak([
//       bgEntry("hall_a", { palette, pixels, depth }),
//       spritesEntry("hero", frames),
//       palEntry("actors", 16, rgb),
//       dataEntry("text", "……"),
//   ]);
//
// 所有整數都是 little-endian。

import crypto from "crypto";

export const PAK_BG = 1, PAK_SPRITES = 2, PAK_PAL = 3, PAK_DATA = 4;
const HEADER = 48, ENTRY = 48;

// 依序寫入 bytes 的小幫手
class Writer {
    constructor() { this.parts = []; this.length = 0; }
    bytes(b) { this.parts.push(Uint8Array.from(b)); this.length += b.length; }
    u8(v) { this.bytes([v & 255]); }
    u16(v) { this.bytes([v & 255, (v >> 8) & 255]); }
    s16(v) { this.u16(v < 0 ? v + 65536 : v); }
    u32(v) { this.bytes([v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]); }
    done() {
        const out = new Uint8Array(this.length);
        let p = 0;
        for (const b of this.parts) { out.set(b, p); p += b.length; }
        return out;
    }
}

// 背景：palette = [[r,g,b], …]（最多 128 色）；pixels = 320×240 個 0..n-1；
// depth = 160×120 個值（單位 8 公分，0 = 沒有遮擋物），或 null
export function bgEntry(name, { palette, pixels, depth = null, w = 320, h = 240 }) {
    if (palette.length < 1 || palette.length > 128) throw new Error(`${name}: background needs 1-128 colors`);
    if (pixels.length !== w * h) throw new Error(`${name}: expected ${w * h} pixels`);
    const out = new Writer();
    out.u16(w); out.u16(h); out.u16(palette.length);
    for (const [r, g, b] of palette) out.bytes([r, g, b]);
    out.bytes(pixels);
    if (depth == null) {
        out.u16(0); out.u16(0);
    } else {
        const dw = w / 2, dh = h / 2;
        if (depth.length !== dw * dh) throw new Error(`${name}: expected ${dw * dh} depth values`);
        out.u16(dw); out.u16(dh);
        // RLE：連續相同的值記成（個數、值），個數最多 255
        let i = 0;
        while (i < depth.length) {
            let n = 1;
            while (i + n < depth.length && n < 255 && depth[i + n] === depth[i]) n++;
            out.u8(n); out.u8(depth[i]);
            i += n;
        }
    }
    return { name, type: PAK_BG, data: out.done() };
}

// 一格角色的像素（w×h，0 = 透明）→ 一列一列壓縮的格式（見 core/gfx.h 的 gfx_sprite_draw）：
// h 個 u16（每一列的位置），每一列重複（透明幾點、有顏色幾點、那幾點的顏色）
export function encodeSpriteRows(pixels, w, h) {
    const rows = [];
    for (let y = 0; y < h; y++) {
        const out = [];
        let x = 0;
        while (x < w) {
            let skip = 0;
            while (x + skip < w && pixels[y * w + x + skip] === 0 && skip < 255) skip++;
            let run = 0;
            while (x + skip + run < w && pixels[y * w + x + skip + run] !== 0 && run < 255) run++;
            out.push(skip, run);
            for (let i = 0; i < run; i++) out.push(pixels[y * w + x + skip + i]);
            x += skip + run;
        }
        rows.push(out);
    }
    const data = new Writer();
    let off = h * 2;
    for (const r of rows) { data.u16(off); off += r.length; }
    if (off > 65535) throw new Error("sprite frame too large");
    for (const r of rows) data.bytes(r);
    return data.done();
}

// 角色動畫：frames = [{ w, h, ox, oy, pixels }]，pixels 是 w×h 個全域顏色編號（0 = 透明）
export function spritesEntry(name, frames) {
    const head = 2 + frames.length * 12;
    const enc = frames.map((f) => {
        if (f.pixels.length !== f.w * f.h) throw new Error(`${name}: frame size mismatch`);
        return encodeSpriteRows(f.pixels, f.w, f.h);
    });
    const out = new Writer();
    out.u16(frames.length);
    let off = head;
    frames.forEach((f, i) => {
        out.u16(f.w); out.u16(f.h); out.s16(f.ox); out.s16(f.oy); out.u32(off);
        off += enc[i].length;
    });
    for (const e of enc) out.bytes(e);
    return { name, type: PAK_SPRITES, data: out.done() };
}

// 調色盤：從第 first 色開始，rgb = [[r,g,b], …]
export function palEntry(name, first, rgb) {
    const out = new Writer();
    out.u16(first); out.u16(rgb.length);
    for (const [r, g, b] of rgb) out.bytes([r, g, b]);
    return { name, type: PAK_PAL, data: out.done() };
}

// 任意資料：字串（存成 UTF-8）或 bytes
export function dataEntry(name, content) {
    const data = typeof content === "string" ? new TextEncoder().encode(content) : Uint8Array.from(content);
    return { name, type: PAK_DATA, data };
}

export function buildPak(entries) {
    const dir = new Writer();
    let off = HEADER + entries.length * ENTRY;
    const names = new Set();
    for (const e of entries) {
        const nameBytes = new TextEncoder().encode(e.name);
        if (nameBytes.length < 1 || nameBytes.length > 32) throw new Error(`name must be 1-32 bytes: ${e.name}`);
        if (names.has(e.name)) throw new Error(`duplicate name: ${e.name}`);
        names.add(e.name);
        const nm = new Uint8Array(32);
        nm.set(nameBytes);
        dir.bytes(nm); dir.u32(e.type); dir.u32(off); dir.u32(e.data.length); dir.u32(0);
        off += e.data.length;
    }
    const body = new Writer();
    body.bytes(dir.done());
    for (const e of entries) body.bytes(e.data);
    const bodyBytes = body.done();
    // 內容指紋：檔頭以後的全部內容（目錄＋資料）
    const sha = crypto.createHash("sha256").update(bodyBytes).digest();

    const out = new Writer();
    out.bytes([0x43, 0x50, 0x41, 0x4b]);   // "CPAK"
    out.u32(1);
    out.u32(entries.length);
    out.bytes(sha);
    out.u32(0);
    out.bytes(bodyBytes);
    return out.done();
}
