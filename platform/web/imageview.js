// imageview.js — Comet Studio 的圖片檢視器
//
// 把專案的圖片（comet.json 的 images）放大顯示，畫上格線和每一格的編號。
// spr(IMG.town, n, x, y) 的 n 就是這裡標的數字：由左到右、由上到下，從 0 開始數。
// 滑鼠移到格子上會顯示編號，點一下把 spr(...) 複製到剪貼簿。

import { decodePNG } from "./png.js";

const MAX_SCALE = 4;

// select：選圖片的下拉選單；canvas：畫圖的地方；info：顯示目前滑鼠指著哪一格
// inflate：zlib 解壓縮（解 PNG 用）；onPick(code)：點了格子，code 是 spr(...) 這行程式
export function createImageViewer({ select, canvas, info, inflate, onPick }) {
    const ctx = canvas.getContext("2d");
    let entries = [];          // [{ name, file, tile, bytes }]
    let current = null;        // 目前顯示的 { name, tile, png, sheet }
    let scale = 1;
    let hover = -1;            // 滑鼠指著第幾格（-1 = 沒有）
    const cache = new Map();   // PNG 的 bytes → 解開的圖（同一份 bytes 不重複解）

    // 圖片名稱是合法的 Lua 名字就寫 IMG.town，否則寫 IMG["my-town"]
    function imgRef(name) {
        return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name) ? `IMG.${name}` : `IMG[${JSON.stringify(name)}]`;
    }

    async function load(entry) {
        if (!cache.has(entry.bytes)) {
            const png = await decodePNG(entry.bytes, inflate);
            // 先畫到一張原尺寸的隱藏畫布，之後放大時直接整張貼上
            const sheet = document.createElement("canvas");
            sheet.width = png.width;
            sheet.height = png.height;
            // ImageData 要的是 Uint8ClampedArray；同一塊記憶體換個型別來看就好，不用複製
            const rgba = new Uint8ClampedArray(png.rgba.buffer, png.rgba.byteOffset, png.rgba.length);
            sheet.getContext("2d").putImageData(new ImageData(rgba, png.width, png.height), 0, 0);
            cache.set(entry.bytes, { png, sheet });
        }
        return { name: entry.name, tile: entry.tile, ...cache.get(entry.bytes) };
    }

    function cellAt(px, py) {
        const t = current.tile * scale;
        const col = Math.floor(px / t), row = Math.floor(py / t);
        const cols = Math.floor(current.png.width / current.tile);
        const rows = Math.floor(current.png.height / current.tile);
        if (col < 0 || row < 0 || col >= cols || row >= rows) return -1;
        return row * cols + col;
    }

    function draw() {
        if (!current) return;
        const { png, sheet, tile } = current;
        // 依分頁的寬度決定放大幾倍（整數倍，像素才會方正）
        const room = canvas.parentElement.clientWidth - 4;
        scale = Math.max(1, Math.min(MAX_SCALE, Math.floor(room / png.width)));
        canvas.width = png.width * scale;
        canvas.height = png.height * scale;

        // 透明的地方畫成棋盤格
        const c = 8;
        for (let y = 0; y < canvas.height; y += c) {
            for (let x = 0; x < canvas.width; x += c) {
                ctx.fillStyle = ((x + y) / c) % 2 ? "#2a2d34" : "#22252b";
                ctx.fillRect(x, y, c, c);
            }
        }
        ctx.imageSmoothingEnabled = false;   // 放大時不要模糊
        ctx.drawImage(sheet, 0, 0, canvas.width, canvas.height);

        // 格線
        const t = tile * scale;
        const cols = Math.floor(png.width / tile), rows = Math.floor(png.height / tile);
        ctx.strokeStyle = "rgba(255, 255, 255, 0.15)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (let i = 0; i <= cols; i++) { ctx.moveTo(i * t + 0.5, 0); ctx.lineTo(i * t + 0.5, rows * t); }
        for (let i = 0; i <= rows; i++) { ctx.moveTo(0, i * t + 0.5); ctx.lineTo(cols * t, i * t + 0.5); }
        ctx.stroke();

        // 每一格的編號（左上角，深色底比較好讀）
        ctx.font = "9px ui-monospace, monospace";
        ctx.textBaseline = "top";
        for (let n = 0; n < cols * rows; n++) {
            const x = (n % cols) * t, y = Math.floor(n / cols) * t;
            const label = String(n);
            const w = ctx.measureText(label).width + 3;
            ctx.fillStyle = "rgba(0, 0, 0, 0.55)";
            ctx.fillRect(x + 1, y + 1, w, 10);
            ctx.fillStyle = n === hover ? "#ffa300" : "#e6e6e6";
            ctx.fillText(label, x + 2, y + 2);
        }

        // 滑鼠指著的格子
        if (hover >= 0) {
            ctx.strokeStyle = "#ffa300";
            ctx.lineWidth = 2;
            ctx.strokeRect((hover % cols) * t + 1, Math.floor(hover / cols) * t + 1, t - 2, t - 2);
        }
    }

    function showInfo() {
        if (!current) return;
        const cols = Math.floor(current.png.width / current.tile);
        const rows = Math.floor(current.png.height / current.tile);
        const size = `${current.png.width}×${current.png.height}，每格 ${current.tile}×${current.tile}，共 ${cols * rows} 格`;
        info.textContent = hover >= 0
            ? `第 ${hover} 格（第 ${hover % cols} 欄、第 ${Math.floor(hover / cols)} 列）· 點一下複製 spr(${imgRef(current.name)}, ${hover}, x, y)`
            : `${size} · 滑鼠移到格子上看編號`;
    }

    async function choose(name) {
        const entry = entries.find((e) => e.name === name);
        if (!entry) return;
        try {
            current = await load(entry);
        } catch (e) {
            current = null;
            info.textContent = `${entry.file} 讀取失敗：${e.message}`;
            return;
        }
        hover = -1;
        draw();
        showInfo();
    }

    canvas.addEventListener("mousemove", (e) => {
        if (!current) return;
        const r = canvas.getBoundingClientRect();
        // 畫布可能被 CSS 縮放，換算回畫布本身的像素
        const n = cellAt((e.clientX - r.left) * canvas.width / r.width, (e.clientY - r.top) * canvas.height / r.height);
        if (n !== hover) { hover = n; draw(); showInfo(); }
    });
    canvas.addEventListener("mouseleave", () => { hover = -1; draw(); showInfo(); });
    canvas.addEventListener("click", () => {
        if (current && hover >= 0) onPick(`spr(${imgRef(current.name)}, ${hover}, x, y)`);
    });
    select.addEventListener("change", () => choose(select.value));
    new ResizeObserver(() => draw()).observe(canvas.parentElement);   // 分頁變寬、變窄時重新決定倍率

    return {
        // 換一組圖片（開啟資料夾、圖片被修改時）。list = [{ name, file, tile, bytes }]
        // 沒有圖片時顯示 emptyText
        async setImages(list, emptyText = "這個專案沒有圖片。在 comet.json 的 images 加上 PNG，這裡就會出現") {
            entries = list;
            const keep = select.value;
            select.innerHTML = "";
            for (const e of entries) select.add(new Option(`${e.name}（${e.file}）`, e.name));
            select.hidden = entries.length === 0;
            if (entries.length === 0) {
                current = null;
                canvas.width = canvas.height = 0;
                info.textContent = emptyText;
                return;
            }
            select.value = entries.some((e) => e.name === keep) ? keep : entries[0].name;
            await choose(select.value);
        },
    };
}
