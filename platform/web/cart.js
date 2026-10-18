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
// maps：每張地圖一個 JSON 檔（格式見下方 buildMaps），遊戲裡用 MAP.camp 取得地圖編號。
//
// 字型不用寫在 comet.json：有呼叫 text() 的遊戲，會自動放進用到的字（見 font.js）。
//
// 這個檔案在瀏覽器（Comet Studio）和命令列（tools/cart.mjs）共用，
// 兩邊合併出來的結果保證一模一樣。

import { decodePNG } from "./png.js";

const FIRST_IMAGE_COLOR = 16;   // 0–15 是預設的 16 色

// 圖片設定可以只寫檔名，也可以寫 { file, tile }
export function imageEntries(project) {
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

// ============================================================
// 地圖
// ============================================================
// comet.json：  "maps": { "camp": "maps/camp.json" }
//
// 地圖檔（通常由 Comet Studio 的地圖編輯器產生，也可以手寫）：
//   {
//     "width": 40, "height": 30,
//     "tilesets": ["town", "dungeon"],        ← 用 comet.json images 裡的哪幾張圖
//     "objects": [                             ← 可以省略
//       {"type": "npc", "x": 19, "y": 11, "name": "elder", "tile": 217, "talk": "elder"},
//       {"type": "sign", "x": 5, "y": 3, "text": "新芽營地"}
//     ],
//     "layers": {
//       "ground": [[1, 1, 2, ...], ...],       ← height 列，每列 width 個格子編號
//       "deco":   [[0, 0, 17, ...], ...],
//       "solid":  [[0, 0, 1, ...], ...]
//     }
//   }
// 格子編號：0 = 空的；第 1 張圖的第 0 格 = 1，第 n 格 = n + 1；
// 第 2 張圖接在第 1 張的最後一格後面（Tiny Town 132 格 → Tiny Dungeon 的第 0 格是 133）。
// 每一列寫成一行，用 git 看差異時一眼就知道改了哪一列。
//
// objects：地圖上的物件（NPC、告示牌、門……）。每個都有 type、x、y（第幾欄、第幾列），
// 其他欄位自由，值只能是文字或整數。遊戲裡用 MAPOBJ.camp 取得（一般的 Lua 表格陣列）。

// 文字 → Lua 字串。JSON.stringify 的跳脫方式和 Lua 不完全一樣（例如 \u0001），自己處理：
// \ " 換行 Tab 用 Lua 的寫法，其他控制字元寫成 \ddd（十進位）
const LUA_ESCAPES = { "\\": "\\\\", '"': '\\"', "\n": "\\n", "\r": "\\r", "\t": "\\t" };
function luaString(s) {
    return '"' + s.replace(/[\\"\x00-\x1f]/g, (c) => LUA_ESCAPES[c] ?? "\\" + String(c.charCodeAt(0)).padStart(3, "0")) + '"';
}

export function mapEntries(project) {
    return Object.entries(project.maps || {}).map(([name, file]) => ({ name, file }));
}

export function mapFiles(project) {
    return mapEntries(project).map((e) => e.file);
}

// 檢查地圖檔的格式，有問題就丟出清楚的錯誤（哪個檔、哪一層、第幾列）
function checkMap(m, file, project) {
    const bad = (msg) => { throw new Error(`地圖 ${file}：${msg}`); };
    if (!Number.isInteger(m.width) || !Number.isInteger(m.height) || m.width < 1 || m.height < 1) bad("width、height 要是正整數");
    if (!Array.isArray(m.tilesets) || m.tilesets.length < 1 || m.tilesets.length > 4) bad("tilesets 要列出 1–4 張圖");
    for (const t of m.tilesets) {
        if (!(project.images && t in project.images)) bad(`tilesets 裡的「${t}」不在 comet.json 的 images 裡`);
    }
    if (!m.layers || typeof m.layers !== "object" || Object.keys(m.layers).length === 0) bad("至少要有一個圖層（layers）");
    if (m.objects !== undefined) {
        if (!Array.isArray(m.objects)) bad("objects 要是陣列");
        m.objects.forEach((o, i) => {
            const where = `第 ${i + 1} 個物件`;
            if (!o || typeof o.type !== "string" || !o.type) bad(`${where}沒有 type`);
            if (!Number.isInteger(o.x) || !Number.isInteger(o.y) || o.x < 0 || o.y < 0 || o.x >= m.width || o.y >= m.height) {
                bad(`${where}（${o.type}）的 x、y 要在地圖範圍內`);
            }
            for (const [k, v] of Object.entries(o)) {
                if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) bad(`${where}的欄位名稱「${k}」只能用英文、數字、底線`);
                if (typeof v !== "string" && !Number.isInteger(v)) bad(`${where}的 ${k} 只能是文字或整數`);
            }
        });
    }
    for (const [name, rows] of Object.entries(m.layers)) {
        if (!/^[A-Za-z_][A-Za-z0-9_]{0,14}$/.test(name)) bad(`圖層名稱「${name}」只能用英文、數字、底線，最多 15 個字`);
        if (!Array.isArray(rows) || rows.length !== m.height) bad(`圖層 ${name} 要有 ${m.height} 列`);
        rows.forEach((row, y) => {
            if (!Array.isArray(row) || row.length !== m.width) bad(`圖層 ${name} 第 ${y} 列要有 ${m.width} 個數字`);
            for (const v of row) {
                if (!Number.isInteger(v) || v < 0 || v > 65535) bad(`圖層 ${name} 第 ${y} 列有不是 0–65535 整數的值：${v}`);
            }
        });
    }
}

// 把 comet.json 的 maps 轉成一段 Lua 程式碼（要放在圖片後面，因為會用到 IMG）
// readText(path)：讀檔，回傳文字
export async function buildMaps(project, readText) {
    const entries = mapEntries(project);
    if (entries.length === 0) return "";
    const lines = [
        "-- ==================== 地圖（comet.json 的 maps，自動產生） ====================",
        "MAP = {}",
        "MAPOBJ = {}",
    ];
    for (const e of entries) {
        let m;
        try {
            m = JSON.parse(await readText(e.file));
        } catch (err) {
            throw new Error(`地圖 ${e.file} 讀取失敗：${err.message}`);
        }
        checkMap(m, e.file, project);

        // 每個圖層：w × h 個格子編號，每個 2 byte（little-endian）→ base64
        const layers = Object.entries(m.layers).map(([name, rows]) => {
            const bytes = new Uint8Array(m.width * m.height * 2);
            rows.flat().forEach((v, i) => { bytes[i * 2] = v & 0xff; bytes[i * 2 + 1] = v >> 8; });
            return `${name} = "${toBase64(bytes)}"`;
        });
        const sets = m.tilesets.map((t) => `IMG[${JSON.stringify(t)}]`).join(", ");
        lines.push(`MAP[${JSON.stringify(e.name)}] = newmap(${m.width}, ${m.height}, { ${sets} }, { ${layers.join(", ")} })`);
        // 物件：每個一行 Lua 表格，例如 { type = "npc", x = 19, y = 11, name = "elder" }
        const objs = (m.objects || []).map((o) =>
            "  { " + Object.entries(o).map(([k, v]) => `${k} = ${typeof v === "string" ? luaString(v) : v}`).join(", ") + " },");
        lines.push(`MAPOBJ[${JSON.stringify(e.name)}] = {`, ...objs, "}");
    }
    return lines.join("\n");
}

// project：comet.json 的內容
// files：{ "src/main.lua": "原始碼文字", ... }
// imagesCode：buildImages() 產生的程式碼（沒有圖片就是空字串）
// fontCode：buildFont() 產生的程式碼（沒用到 text() 就是空字串）
// mapsCode：buildMaps() 產生的程式碼（沒有地圖就是空字串）
// 回傳 { code: 合併後的遊戲檔, segments: 每個來源檔在遊戲檔裡的行號範圍 }
export function buildCart(project, files, imagesCode = "", fontCode = "", mapsCode = "") {
    const lines = [
        `-- ${project.name || "Comet game"}`,
        ...(project.id ? [`-- id: ${project.id}`] : []),
        "-- 這個檔案是自動合併產生的，請修改原始碼檔，不要直接改這裡",
    ];
    if (fontCode) lines.push("", ...fontCode.split("\n"));
    if (imagesCode) lines.push("", ...imagesCode.split("\n"));
    if (mapsCode) lines.push("", ...mapsCode.split("\n"));
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
