// font.js — 從 c11.bin 挑出遊戲用到的字，產生遊戲檔裡的 newfont(...)
//
// 整套字型有 10,268 字（330 KB），主機記憶體放不下，也沒必要：
// 一款遊戲只會用到幾百個字。所以合併時先掃描原始碼裡的「字串」，
// 只把出現過的字放進遊戲檔。
//
// 和 cart.js 一樣，在瀏覽器（Comet Studio）和命令列（tools/cart.mjs）共用。
//
// c11.bin 的格式見 tools/font/c11_convert.py：
//   檔頭 8 byte："C11F"、版本、字高、字數
//   每個字 33 byte：字碼（4）、寬度（1）、點陣（28）

const HEADER = 8;
const RECORD = 33;

// 一定收錄的字：ASCII 的空白到 ~（95 個），
// 這樣 tostring(hp)、"Lv" .. lv 這種程式算出來的字一定畫得出來
const ALWAYS = Array.from({ length: 0x7f - 0x20 }, (_, i) => 0x20 + i);

// ------------------------------------------------------------
// 1. 掃描 Lua 原始碼
// ------------------------------------------------------------
// 回傳 { strings: [字串內容...], code: 去掉註解和字串之後的程式碼 }
//
// 為什麼只看字串、不看註解：Wildkin 的註解寫了很多中文說明，
// 那些字畫面上不會出現，收進來只是浪費記憶體。
//
// Lua 的字串有三種寫法："雙引號"、'單引號'、[[長字串]]（中間可以加 =，例如 [==[ ]==]）
// 註解有兩種：-- 到行尾、--[[ 多行 ]]
export function scanLua(text) {
    const strings = [];
    let code = "";
    let i = 0;

    // 從 text[at] 開始如果是 [[ 或 [==[，回傳中間有幾個 =；不是的話回傳 -1
    function longBracket(at) {
        if (text[at] !== "[") return -1;
        let j = at + 1;
        while (text[j] === "=") j++;
        return text[j] === "[" ? j - at - 1 : -1;
    }

    // 讀一段長字串或長註解，回傳 [內容, 結束後的位置]
    function readLong(at, level) {
        const close = "]" + "=".repeat(level) + "]";
        const start = at + level + 2;
        const end = text.indexOf(close, start);
        if (end < 0) return [text.slice(start), text.length];   // 沒有結尾：讀到檔案最後
        return [text.slice(start, end), end + close.length];
    }

    while (i < text.length) {
        const c = text[i];
        if (c === "-" && text[i + 1] === "-") {                 // 註解
            const level = longBracket(i + 2);
            if (level >= 0) {
                i = readLong(i + 2, level)[1];
            } else {
                const nl = text.indexOf("\n", i);
                i = nl < 0 ? text.length : nl;
            }
        } else if (c === '"' || c === "'") {                    // "字串" 或 '字串'
            let j = i + 1, s = "";
            while (j < text.length && text[j] !== c && text[j] !== "\n") {
                if (text[j] === "\\") { s += text[j + 1] ?? ""; j += 2; continue; }   // 跳脫字元：\" 不算結尾
                s += text[j++];
            }
            strings.push(s);
            code += " ";
            i = j + 1;
        } else if (longBracket(i) >= 0) {                       // [[長字串]]
            const [s, next] = readLong(i, longBracket(i));
            strings.push(s);
            code += " ";
            i = next;
        } else {
            code += c;
            i++;
        }
    }
    return { strings, code };
}

// ------------------------------------------------------------
// 2. 讀 c11.bin
// ------------------------------------------------------------
// 回傳 Map(字碼 → 這個字在檔案裡的位置)
function indexFont(bytes) {
    const magic = String.fromCharCode(...bytes.subarray(0, 4));
    if (magic !== "C11F" || bytes[4] !== 1) throw new Error("c11.bin 格式不對，請重新執行 c11_convert.py");
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const count = view.getUint16(6, true);
    const index = new Map();
    for (let n = 0; n < count; n++) {
        const at = HEADER + n * RECORD;
        index.set(view.getUint32(at, true), at);
    }
    return { height: bytes[5], index };
}

// 位元組 → base64（和 cart.js 相同的寫法）
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

// ------------------------------------------------------------
// 3. 挑字，產生 newfont(...)
// ------------------------------------------------------------
// files：{ "src/main.lua": "原始碼", ... }
// fontBytes：c11.bin 的內容（Uint8Array）
// 回傳 { code, stats }
//   code：一行 Lua，例如 newfont(14, 712, "...")；遊戲沒用到 text() 就是空字串
//   stats：{ glyphs: 字數, cjk: 中文字數, bytes: 字型資料大小, missing: [字型裡沒有的字] }
export function buildFont(files, fontBytes) {
    const scanned = Object.values(files).map(scanLua);

    // 沒有呼叫 text() 或 textw() 的遊戲不放字型
    if (!scanned.some((s) => /\btextw?\s*\(/.test(s.code))) {
        return { code: "", stats: null };
    }

    const wanted = new Set(ALWAYS);
    for (const s of scanned) {
        for (const str of s.strings) {
            for (const ch of str) {                 // for...of 一次取一個完整的字（不會把中文切半）
                const cp = ch.codePointAt(0);
                if (cp >= 0x20) wanted.add(cp);     // 換行、Tab 這些控制字元不用畫
            }
        }
    }

    const { height, index } = indexFont(fontBytes);
    const codes = [...wanted].sort((a, b) => a - b);    // 由小到大排，C 才能用二分搜尋
    const found = codes.filter((cp) => index.has(cp));
    const missing = codes.filter((cp) => !index.has(cp)).map((cp) => String.fromCodePoint(cp));

    // 把挑到的字照原本的格式（每字 33 byte）接起來
    const data = new Uint8Array(found.length * RECORD);
    found.forEach((cp, n) => {
        const at = index.get(cp);
        data.set(fontBytes.subarray(at, at + RECORD), n * RECORD);
    });

    const cjk = found.filter((cp) => cp >= 0x2e80).length;
    return {
        code: [
            "-- ==================== 字型（俐方體 11 號，OFL；自動產生，只含用到的字） ====================",
            `newfont(${height}, ${found.length}, "${toBase64(data)}")`,
        ].join("\n"),
        stats: { glyphs: found.length, cjk, bytes: data.length, missing },
    };
}

// 給人看的一行摘要，例如「字型：712 字（中文 617），23 KB」
export function fontSummary(stats) {
    if (!stats) return "";
    let s = `字型：${stats.glyphs} 字（中文 ${stats.cjk}），${Math.ceil(stats.bytes / 1024)} KB`;
    if (stats.missing.length) s += `；字型裡沒有：${stats.missing.join(" ")}`;
    return s;
}
