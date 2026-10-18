// cart.js — 把一個專案的原始碼和圖片合併成一個遊戲檔（cart）
//
// Comet 規定一款遊戲是一個 .lua 檔；大遊戲拆成好幾個檔案比較好開發。
// 專案資料夾裡放一個 comet.json：
//
//   {
//     "name": "Wildkin 野伴",
//     "id": "hardliver/wildkin",
//     "sources": ["src/util.lua", "src/battle.lua", "src/main.lua"],
//     "images": {
//       "town": "assets/kenney/tiny-town/tilemap.png",
//       "monsters": { "file": "assets/art/monsters.png", "tile": 32 }
//     },
//     "output": "build/wildkin.lua"
//   }
//
// sources：依順序接起來。
// images：每張 PNG 轉成遊戲檔裡的圖片資料，遊戲裡用 IMG.town 取得圖片編號。
//         所有圖片共用一個調色盤：每種顏色自動分配一個編號（從 16 開始，0–15 保留給預設色）。
//
// 這個檔案在瀏覽器（Comet Studio）和命令列（tools/cart.mjs）共用，
// 兩邊合併出來的結果保證一模一樣。

import { decodePNG } from "./png.js";

const FIRST_IMAGE_COLOR = 16;   // 0–15 是預設的 16 色

// 圖片設定可以只寫檔名，也可以寫 { file, tile }
function imageEntries(project) {
    return Object.entries(project.images || {}).map(([name, v]) =>
        typeof v === "string" ? { name, file: v, tile: 16 } : { name, file: v.file, tile: v.tile || 16 });
}

// 專案用到的所有檔案（Studio 用來決定要讀哪些檔、偵測哪些檔被修改）
export function imageFiles(project) {
    return imageEntries(project).map((e) => e.file);
}

// 位元組 → base64 文字（瀏覽器和 Node.js 都能用的寫法）
function toBase64(bytes) {
    const CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let out = "";
    for (let i = 0; i < bytes.length; i += 3) {
        const n = (bytes[i] << 16) | ((bytes[i + 1] ?? 0) << 8) | (bytes[i + 2] ?? 0);
        out += CH[(n >> 18) & 63] + CH[(n >> 12) & 63] +
            (i + 1 < bytes.length ? CH[(n >> 6) & 63] : "=") +
            (i + 2 < bytes.length ? CH[n & 63] : "=");
    }
    return out;
}

// 把 comet.json 的 images 轉成一段 Lua 程式碼：先設定調色盤，再建立每張圖片。
// readBytes(path)：讀檔，回傳 Uint8Array；inflate(bytes)：zlib 解壓縮
export async function buildImages(project, readBytes, inflate) {
    const entries = imageEntries(project);
    if (entries.length === 0) return "";

    const colors = new Map();   // "r,g,b" → 顏色編號
    const lines = [
        "-- ==================== 圖片（comet.json 的 images，自動產生） ====================",
        "IMG = {}",
    ];
    const imageLines = [];

    for (const e of entries) {
        const png = await decodePNG(await readBytes(e.file), inflate);
        const idx = new Uint8Array(png.width * png.height);
        for (let i = 0; i < idx.length; i++) {
            const [r, g, b, a] = png.rgba.subarray(i * 4, i * 4 + 4);
            if (a < 128) continue;   // 透明 → 0
            const key = `${r},${g},${b}`;
            if (!colors.has(key)) {
                const n = FIRST_IMAGE_COLOR + colors.size;
                if (n > 255) throw new Error(`圖片的顏色超過 ${256 - FIRST_IMAGE_COLOR} 種（加上 ${e.file} 之後），請減少顏色`);
                colors.set(key, n);
            }
            idx[i] = colors.get(key);
        }
        imageLines.push(`IMG[${JSON.stringify(e.name)}] = newimage(${png.width}, ${png.height}, "${toBase64(idx)}", ${e.tile})`);
    }

    for (const [key, n] of colors) lines.push(`pal(${n}, ${key.replace(/,/g, ", ")})`);
    lines.push(...imageLines);
    return lines.join("\n");
}

// project：comet.json 的內容
// files：{ "src/main.lua": "原始碼文字", ... }
// imagesCode：buildImages() 產生的程式碼（沒有圖片就是空字串）
// 回傳 { code: 合併後的遊戲檔, segments: 每個來源檔在遊戲檔裡的行號範圍 }
export function buildCart(project, files, imagesCode = "") {
    const lines = [
        `-- ${project.name || "Comet game"}`,
        ...(project.id ? [`-- id: ${project.id}`] : []),
        "-- 這個檔案是自動合併產生的，請修改原始碼檔，不要直接改這裡",
    ];
    if (imagesCode) lines.push("", ...imagesCode.split("\n"));
    const segments = [];

    for (const path of project.sources) {
        if (!(path in files)) throw new Error(`comet.json 列了 ${path}，但找不到這個檔案`);
        lines.push("", `-- ==================== ${path} ====================`);
        const body = files[path].replace(/\r\n/g, "\n").replace(/\n$/, "").split("\n");
        const start = lines.length + 1;   // 這個檔案第 1 行在遊戲檔的第幾行（從 1 開始數）
        lines.push(...body);
        segments.push({ path, start, end: start + body.length - 1 });
    }
    return { code: lines.join("\n") + "\n", segments };
}

// 遊戲檔的第 n 行 → 原本是哪個檔案的第幾行（錯誤訊息用）
export function mapLine(segments, n) {
    for (const s of segments) {
        if (n >= s.start && n <= s.end) return { path: s.path, line: n - s.start + 1 };
    }
    return null;
}

// 瀏覽器的 zlib 解壓縮
export async function browserInflate(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}
