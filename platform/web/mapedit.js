// mapedit.js — Comet Studio 的地圖編輯器
//
// 編輯 comet.json 的 maps 裡的地圖檔（格式見 cart.js 的 buildMaps）。
// 和 Studio 的分工：Studio 決定打開哪張地圖、把結果存到哪裡；這個檔案只管「編輯」。
//
// 操作：
//   右邊的圖塊盤：點一格當畫筆，拖曳可以框選好幾格（一次蓋一整棟房子）
//   地圖：左鍵用目前的工具畫，右鍵擦掉
//   工具：畫筆 B、橡皮擦 E、填滿 F、吸管 I；復原 Ctrl+Z、重做 Ctrl+Shift+Z
//   名為 solid 的圖層是「碰撞層」：畫上去的格子是 1（走不過去），顯示成紅色
//   「物件」：NPC、告示牌、門、擋路物、起點、採集點。點空格放一個，點物件選取（右邊改欄位），
//            拖曳移動，右鍵或 Delete 刪除。存在地圖檔的 objects，遊戲裡用 MAPOBJ.地圖名 取得

import { decodePNG } from "./png.js";

const TOOLS = [
    { id: "pen",    label: "畫筆",   key: "b" },
    { id: "eraser", label: "橡皮擦", key: "e" },
    { id: "fill",   label: "填滿",   key: "f" },
    { id: "picker", label: "吸管",   key: "i" },
];
const LAYER_NAME = /^[A-Za-z_][A-Za-z0-9_]{0,14}$/;
const PALETTE_ZOOM = 2;

// solid（碰撞層）只存 0 / 1，不畫圖，用紅色表示
const isFlagLayer = (name) => name === "solid";

// 「物件」不是真的圖層，用一個不可能是圖層名稱的字串代表
const OBJ = "(物件)";

// 物件的種類和欄位。欄位型別：text 文字、textarea 多行文字、int 整數、
// tile 格子編號（可以用目前的畫筆）、map 地圖名稱
const OBJECT_TYPES = {
    npc:   { label: "NPC",   mark: "人", color: "#ffa300", fields: [
        ["name", "名字（程式裡用，英文）", "text"], ["tile", "圖", "tile"], ["talk", "對話編號", "text"]] },
    sign:  { label: "告示牌", mark: "牌", color: "#ffec27", fields: [["text", "內容", "textarea"]] },
    door:  { label: "門",    mark: "門", color: "#29adff", fields: [
        ["to", "通往哪張地圖", "map"], ["tx", "到達第幾欄", "int"], ["ty", "到達第幾列", "int"]] },
    block: { label: "擋路物", mark: "擋", color: "#ff004d", fields: [
        ["tile", "圖", "tile"], ["need", "需要的野外技能（例如 伐木）", "text"]] },
    start: { label: "起點",  mark: "起", color: "#00e436", fields: [] },
    gather: { label: "採集點", mark: "採", color: "#ab5236", fields: [
        ["item", "撿到什麼（資料檔的道具編號，例如 wood）", "text"], ["amount", "一次幾個", "int"], ["tile", "圖", "tile"]] },
};

// root：放編輯器的元素；inflate：解 PNG 用；onChange(text)：地圖改了，text 是新的 JSON；
// toast(msg)：顯示提示
export function createMapEditor({ root, inflate, onChange, toast }) {
    root.innerHTML = `
      <div class="me-bar">
        <span class="me-group me-layers"></span>
        <button class="me-add-layer" title="新增圖層">＋ 圖層</button>
        <span class="me-sep"></span>
        <span class="me-group me-tools"></span>
        <span class="me-sep"></span>
        <button class="me-undo" title="Ctrl+Z">↶ 復原</button>
        <button class="me-redo" title="Ctrl+Shift+Z">↷ 重做</button>
        <span class="me-sep"></span>
        <label>放大 <select class="me-zoom"><option>1</option><option selected>2</option><option>3</option><option>4</option></select></label>
        <label><input type="checkbox" class="me-grid" checked> 格線</label>
        <label><input type="checkbox" class="me-dim" checked> 其他圖層變淡</label>
        <span class="me-sep"></span>
        <label>大小 <input class="me-w" type="number" min="1" max="1024"> × <input class="me-h" type="number" min="1" max="1024"></label>
        <button class="me-resize">套用</button>
      </div>
      <div class="me-main">
        <div class="me-map-wrap"><canvas class="me-map"></canvas></div>
        <div class="me-side">
          <div class="me-obj" hidden></div>
          <div class="me-side-bar">
            <select class="me-set"></select>
            <select class="me-add-set"><option value="">＋ 加入圖片…</option></select>
          </div>
          <div class="me-pal-wrap"><canvas class="me-pal"></canvas></div>
          <div class="me-status"></div>
        </div>
      </div>`;
    const $ = (sel) => root.querySelector(sel);
    const mapCanvas = $(".me-map"), mctx = mapCanvas.getContext("2d");
    const palCanvas = $(".me-pal"), pctx = palCanvas.getContext("2d");

    // ---- 狀態 ----
    let images = [];       // 專案所有圖片 [{ name, tile, sheet, cols, rows }]
    let map = null;        // { width, height, tilesets: [名稱], layers: Map(名稱 → Uint16Array), extra: 其他欄位 }
    let sets = [];         // 這張地圖用的圖 [{ name, tile, sheet, cols, rows, first, count }]
    let tile = 16;         // 一格幾像素
    let layer = null;      // 目前的圖層名稱
    let tool = "pen";
    let zoom = 2;
    let palSet = 0;        // 圖塊盤顯示第幾張圖
    let brush = { w: 1, h: 1, tiles: [1] };   // 畫筆：w × h 格，tiles 是格子編號
    let palSel = null;     // 圖塊盤上框選的範圍 { c0, r0, c1, r1 }
    let hover = null;      // 滑鼠在地圖的哪一格 { cx, cy }
    let undoStack = [], redoStack = [];
    let stroke = null;     // 正在畫的一筆：{ layer, changes: Map(位置 → [舊值, 新值]), last }
    let dirty = true;      // 需要重畫
    let mapNames = [];     // 專案所有地圖的名字（門的「通往哪張地圖」用）
    let objType = "npc";   // 物件模式下，點空格要放哪一種
    let selected = -1;     // 選取的物件是第幾個（-1 = 沒有）
    let drag = null;       // 正在拖曳物件：{ before: 拖曳前的地圖文字, moved }

    // ============================================================
    // 格子編號 ↔ 哪張圖的第幾格
    // ============================================================
    // 0 = 空的；第 1 張圖的第 n 格 = n + 1；第 2 張圖接在第 1 張的最後一格後面

    function rebuildSets() {
        let next = 1;
        sets = map.tilesets.map((name) => {
            const img = images.find((i) => i.name === name);
            if (!img) throw new Error(`地圖用到的圖片「${name}」不在 comet.json 的 images 裡`);
            const s = { ...img, first: next, count: img.cols * img.rows };
            next += s.count;
            return s;
        });
        tile = sets[0].tile;
    }

    function setOf(id) {
        for (const s of sets) if (id >= s.first && id < s.first + s.count) return s;
        return null;
    }

    function drawTile(ctx, id, x, y, size) {
        const s = setOf(id);
        if (!s) return;
        const n = id - s.first;
        ctx.drawImage(s.sheet, (n % s.cols) * s.tile, Math.floor(n / s.cols) * s.tile, s.tile, s.tile, x, y, size, size);
    }

    // ============================================================
    // 讀寫地圖檔
    // ============================================================

    function parse(text) {
        const j = JSON.parse(text);
        const { width, height, tilesets, layers, objects, ...extra } = j;
        if (!Number.isInteger(width) || !Number.isInteger(height)) throw new Error("width、height 要是整數");
        const m = { width, height, tilesets: [...tilesets], layers: new Map(), extra,
                    objects: Array.isArray(objects) ? objects.map((o) => ({ ...o })) : [] };
        for (const [name, rows] of Object.entries(layers)) {
            const a = new Uint16Array(width * height);
            rows.forEach((row, y) => row.forEach((v, x) => { a[y * width + x] = v; }));
            m.layers.set(name, a);
        }
        return m;
    }

    // 每一列寫成一行，用 git 看差異時一眼就知道改了哪一列
    function serialize() {
        const w = map.width;
        const layerText = [...map.layers].map(([name, a]) => {
            const rows = [];
            for (let y = 0; y < map.height; y++) rows.push(`      [${Array.from(a.subarray(y * w, y * w + w)).join(",")}]`);
            return `    ${JSON.stringify(name)}: [\n${rows.join(",\n")}\n    ]`;
        });
        const extra = Object.entries(map.extra).map(([k, v]) => `  ${JSON.stringify(k)}: ${JSON.stringify(v)},\n`).join("");
        // 物件：每個一行（沒有物件就不寫，舊的地圖檔不會多出這個欄位）
        const objects = map.objects.length
            ? `  "objects": [\n${map.objects.map((o) => `    ${JSON.stringify(o)}`).join(",\n")}\n  ],\n` : "";
        return `{\n  "width": ${map.width},\n  "height": ${map.height},\n` +
            `  "tilesets": [${map.tilesets.map((t) => JSON.stringify(t)).join(", ")}],\n` + extra + objects +
            `  "layers": {\n${layerText.join(",\n")}\n  }\n}\n`;
    }

    function changed() {
        dirty = true;
        onChange(serialize());
    }

    // ============================================================
    // 畫面
    // ============================================================

    function renderBar() {
        const layers = $(".me-layers");
        layers.innerHTML = "圖層 ";
        for (const name of map.layers.keys()) {
            const b = document.createElement("button");
            b.textContent = isFlagLayer(name) ? `${name}（碰撞）` : name;
            b.className = name === layer ? "on" : "";
            b.addEventListener("click", () => { layer = name; renderBar(); dirty = true; });
            layers.appendChild(b);
        }
        const ob = document.createElement("button");
        ob.textContent = `物件（${map.objects.length}）`;
        ob.className = layer === OBJ ? "on" : "";
        ob.addEventListener("click", () => { layer = OBJ; renderBar(); dirty = true; });
        layers.appendChild(ob);

        const tools = $(".me-tools");
        if (layer === OBJ) {
            // 物件模式：選要放哪一種
            tools.innerHTML = "放置 ";
            for (const [id, t] of Object.entries(OBJECT_TYPES)) {
                const b = document.createElement("button");
                b.textContent = t.label;
                b.className = id === objType ? "on" : "";
                b.addEventListener("click", () => { objType = id; renderBar(); });
                tools.appendChild(b);
            }
            renderObjPanel();
            $(".me-undo").disabled = undoStack.length === 0;
            $(".me-redo").disabled = redoStack.length === 0;
            $(".me-w").value = map.width;
            $(".me-h").value = map.height;
            renderSetSelect();
            return;
        }
        $(".me-obj").hidden = true;
        tools.innerHTML = "工具 ";
        for (const t of TOOLS) {
            const b = document.createElement("button");
            b.textContent = t.label;
            b.title = `快捷鍵 ${t.key.toUpperCase()}`;
            b.className = t.id === tool ? "on" : "";
            b.addEventListener("click", () => { tool = t.id; renderBar(); });
            tools.appendChild(b);
        }
        $(".me-undo").disabled = undoStack.length === 0;
        $(".me-redo").disabled = redoStack.length === 0;
        $(".me-w").value = map.width;
        $(".me-h").value = map.height;
        renderSetSelect();
    }

    function renderSetSelect() {
        const setSel = $(".me-set");
        setSel.innerHTML = "";
        sets.forEach((s, i) => setSel.add(new Option(`${s.name}（${s.first}–${s.first + s.count - 1}）`, i)));
        setSel.value = palSet;
        const add = $(".me-add-set");
        add.innerHTML = `<option value="">＋ 加入圖片…</option>`;
        for (const img of images) {
            if (!map.tilesets.includes(img.name) && img.tile === tile) add.add(new Option(img.name, img.name));
        }
        add.hidden = add.options.length <= 1 || map.tilesets.length >= 4;
    }

    // 右邊的物件面板：選取的物件有哪些欄位
    function renderObjPanel() {
        const panel = $(".me-obj");
        panel.hidden = false;
        const o = map.objects[selected];
        if (!o) {
            panel.innerHTML = `<div class="me-obj-hint">點地圖上的空格放一個「${OBJECT_TYPES[objType].label}」；點已經有的物件來選取、拖曳移動；右鍵或 Delete 刪除</div>`;
            return;
        }
        const type = OBJECT_TYPES[o.type] || { label: o.type, fields: [] };
        panel.innerHTML = `<div class="me-obj-title">${type.label}（第 ${o.x} 欄、第 ${o.y} 列）
            <button class="me-obj-del">刪除</button></div>`;
        panel.querySelector(".me-obj-del").addEventListener("click", deleteSelected);
        for (const [key, label, kind] of type.fields) {
            const row = document.createElement("label");
            row.className = "me-obj-row";
            row.append(label);
            let input;
            if (kind === "textarea") {
                input = document.createElement("textarea");
                input.rows = 3;
            } else if (kind === "map") {
                input = document.createElement("select");
                for (const n of mapNames) input.add(new Option(n, n));
            } else {
                input = document.createElement("input");
                input.type = kind === "text" ? "text" : "number";
            }
            input.value = o[key] ?? "";
            // change：輸入完（按 Enter 或離開欄位）才記錄，不會每打一個字就存一次
            input.addEventListener("change", () => {
                const v = kind === "int" || kind === "tile" ? Math.max(0, Math.trunc(Number(input.value) || 0)) : input.value;
                snapshotEdit(() => { o[key] = v; });
            });
            row.appendChild(input);
            if (kind === "tile") {
                const use = document.createElement("button");
                use.textContent = "用目前的畫筆";
                use.title = "右邊圖塊盤選的那一格";
                use.addEventListener("click", (e) => { e.preventDefault(); snapshotEdit(() => { o[key] = brush.tiles[0]; }); });
                row.appendChild(use);
            }
            panel.appendChild(row);
        }
    }

    function deleteSelected() {
        if (!map.objects[selected]) return;
        const i = selected;
        selected = -1;
        snapshotEdit(() => { map.objects.splice(i, 1); });
    }

    // (cx, cy) 那一格上面的物件是第幾個（同一格有好幾個時取最後放的）；沒有回傳 -1
    function objectAt(cx, cy) {
        for (let i = map.objects.length - 1; i >= 0; i--) {
            if (map.objects[i].x === cx && map.objects[i].y === cy) return i;
        }
        return -1;
    }

    // 新物件的預設欄位
    function newObject(type, cx, cy) {
        const o = { type, x: cx, y: cy };
        if (type === "npc") {
            let n = 1;
            while (map.objects.some((x) => x.name === `npc${n}`)) n++;
            Object.assign(o, { name: `npc${n}`, tile: brush.tiles[0], talk: "" });
        } else if (type === "sign") {
            o.text = "";
        } else if (type === "door") {
            Object.assign(o, { to: mapNames[0] || "", tx: 0, ty: 0 });
        } else if (type === "block") {
            Object.assign(o, { tile: brush.tiles[0], need: "" });
        }
        return o;
    }

    function drawObjects() {
        const t = tile * zoom;
        mctx.font = `bold ${Math.max(9, 5 * zoom)}px sans-serif`;
        mctx.textBaseline = "top";
        map.objects.forEach((o, i) => {
            const type = OBJECT_TYPES[o.type] || { mark: "?", color: "#ffffff" };
            const x = o.x * t, y = o.y * t;
            mctx.globalAlpha = layer === OBJ ? 1 : 0.6;
            if (o.tile) drawTile(mctx, o.tile, x, y, t);
            mctx.strokeStyle = type.color;
            mctx.lineWidth = i === selected && layer === OBJ ? 3 : 1.5;
            mctx.strokeRect(x + 1, y + 1, t - 2, t - 2);
            // 左上角的小標籤：種類
            const w = mctx.measureText(type.mark).width + 4;
            mctx.fillStyle = type.color;
            mctx.fillRect(x + 1, y + 1, w, Math.max(10, 5 * zoom) + 2);
            mctx.fillStyle = "#111";
            mctx.fillText(type.mark, x + 3, y + 2);
            mctx.globalAlpha = 1;
        });
    }

    function drawMap() {
        const t = tile * zoom;
        mapCanvas.width = map.width * t;
        mapCanvas.height = map.height * t;
        mctx.imageSmoothingEnabled = false;
        mctx.fillStyle = "#1b1d22";
        mctx.fillRect(0, 0, mapCanvas.width, mapCanvas.height);

        const dim = $(".me-dim").checked;
        for (const [name, a] of map.layers) {
            if (isFlagLayer(name)) continue;   // 碰撞層最後才畫
            mctx.globalAlpha = dim && layer !== OBJ && name !== layer ? 0.35 : 1;   // 物件模式不淡化，才看得清楚要放哪
            for (let i = 0; i < a.length; i++) {
                if (a[i]) drawTile(mctx, a[i], (i % map.width) * t, Math.floor(i / map.width) * t, t);
            }
        }
        // 碰撞層：選到它的時候才顯示，紅色半透明
        if (isFlagLayer(layer)) {
            mctx.globalAlpha = 0.45;
            mctx.fillStyle = "#ff3355";
            const a = map.layers.get(layer);
            for (let i = 0; i < a.length; i++) {
                if (a[i]) mctx.fillRect((i % map.width) * t, Math.floor(i / map.width) * t, t, t);
            }
        }
        mctx.globalAlpha = 1;
        drawObjects();

        if ($(".me-grid").checked) {
            mctx.strokeStyle = "rgba(255, 255, 255, 0.12)";
            mctx.lineWidth = 1;
            mctx.beginPath();
            for (let x = 0; x <= map.width; x++) { mctx.moveTo(x * t + 0.5, 0); mctx.lineTo(x * t + 0.5, map.height * t); }
            for (let y = 0; y <= map.height; y++) { mctx.moveTo(0, y * t + 0.5); mctx.lineTo(map.width * t, y * t + 0.5); }
            mctx.stroke();
            // 每 20 × 15 格（一個畫面的大小）畫一條比較明顯的線，方便估計捲動範圍
            mctx.strokeStyle = "rgba(255, 163, 0, 0.35)";
            mctx.beginPath();
            for (let x = 20; x < map.width; x += 20) { mctx.moveTo(x * t + 0.5, 0); mctx.lineTo(x * t + 0.5, map.height * t); }
            for (let y = 15; y < map.height; y += 15) { mctx.moveTo(0, y * t + 0.5); mctx.lineTo(map.width * t, y * t + 0.5); }
            mctx.stroke();
        }

        // 滑鼠所在的格子：預覽畫筆
        if (hover) {
            // 畫筆在一般圖層是整個畫筆的大小；碰撞層一次只畫 1 格
            const big = tool === "pen" && !isFlagLayer(layer) && layer !== OBJ;
            const w = big ? brush.w : 1, h = big ? brush.h : 1;
            if (big) {
                mctx.globalAlpha = 0.6;
                for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
                    drawTile(mctx, brush.tiles[j * w + i], (hover.cx + i) * t, (hover.cy + j) * t, t);
                }
                mctx.globalAlpha = 1;
            }
            mctx.strokeStyle = tool === "eraser" ? "#ff5c6c" : "#ffa300";
            mctx.lineWidth = 2;
            mctx.strokeRect(hover.cx * t + 1, hover.cy * t + 1, w * t - 2, h * t - 2);
        }
    }

    function drawPalette() {
        const s = sets[palSet];
        const t = s.tile * PALETTE_ZOOM;
        palCanvas.width = s.cols * t;
        palCanvas.height = s.rows * t;
        pctx.imageSmoothingEnabled = false;
        pctx.fillStyle = "#22252b";
        pctx.fillRect(0, 0, palCanvas.width, palCanvas.height);
        pctx.drawImage(s.sheet, 0, 0, s.cols * s.tile, s.rows * s.tile, 0, 0, palCanvas.width, palCanvas.height);
        pctx.strokeStyle = "rgba(255, 255, 255, 0.12)";
        pctx.beginPath();
        for (let x = 0; x <= s.cols; x++) { pctx.moveTo(x * t + 0.5, 0); pctx.lineTo(x * t + 0.5, s.rows * t); }
        for (let y = 0; y <= s.rows; y++) { pctx.moveTo(0, y * t + 0.5); pctx.lineTo(s.cols * t, y * t + 0.5); }
        pctx.stroke();
        if (palSel) {
            const c0 = Math.min(palSel.c0, palSel.c1), c1 = Math.max(palSel.c0, palSel.c1);
            const r0 = Math.min(palSel.r0, palSel.r1), r1 = Math.max(palSel.r0, palSel.r1);
            pctx.strokeStyle = "#ffa300";
            pctx.lineWidth = 2;
            pctx.strokeRect(c0 * t + 1, r0 * t + 1, (c1 - c0 + 1) * t - 2, (r1 - r0 + 1) * t - 2);
        }
    }

    function status(text) { $(".me-status").textContent = text; }

    function loop() {
        if (map && dirty) {
            dirty = false;
            drawMap();
        }
        requestAnimationFrame(loop);
    }
    requestAnimationFrame(loop);

    // ============================================================
    // 修改格子（全部經過 setCell，才能記錄復原）
    // ============================================================

    function setCell(name, cx, cy, value) {
        if (cx < 0 || cy < 0 || cx >= map.width || cy >= map.height) return;
        const a = map.layers.get(name);
        const i = cy * map.width + cx;
        if (a[i] === value) return;
        if (stroke) {
            const old = stroke.changes.has(i) ? stroke.changes.get(i)[0] : a[i];
            stroke.changes.set(i, [old, value]);
        }
        a[i] = value;
        dirty = true;
    }

    // 在 (cx, cy) 用目前的工具做一次
    function apply(cx, cy, erase) {
        const flag = isFlagLayer(layer);
        if (erase || tool === "eraser") {
            setCell(layer, cx, cy, 0);
        } else if (tool === "pen") {
            if (flag) { setCell(layer, cx, cy, 1); return; }
            for (let j = 0; j < brush.h; j++) for (let i = 0; i < brush.w; i++) {
                setCell(layer, cx + i, cy + j, brush.tiles[j * brush.w + i]);
            }
        } else if (tool === "fill") {
            fill(cx, cy, flag);
        } else if (tool === "picker") {
            pick(cx, cy);
        }
    }

    // 填滿：和起點相同編號、上下左右相連的整塊區域。
    // 畫筆有好幾格時，用畫筆的圖案重複鋪滿（像磁磚一樣，從地圖左上角對齊）
    function fill(cx, cy, flag) {
        const a = map.layers.get(layer);
        const w = map.width, h = map.height;
        const target = a[cy * w + cx];
        const valueAt = (x, y) => (flag ? 1 : brush.tiles[(y % brush.h) * brush.w + (x % brush.w)]);
        if (brush.w === 1 && brush.h === 1 && valueAt(cx, cy) === target) return;   // 填一樣的東西，不用做
        const seen = new Uint8Array(w * h);
        const queue = [cy * w + cx];
        seen[cy * w + cx] = 1;
        // 用「待處理清單」一格一格往外擴散（不用遞迴：地圖很大時遞迴會太深）
        while (queue.length) {
            const i = queue.pop();
            const x = i % w, y = Math.floor(i / w);
            setCell(layer, x, y, valueAt(x, y));
            for (const [nx, ny] of [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]]) {
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const n = ny * w + nx;
                if (!seen[n] && a[n] === target) { seen[n] = 1; queue.push(n); }
            }
        }
    }

    // 吸管：拿地圖上的這一格當畫筆，圖塊盤也跳到那一格
    function pick(cx, cy) {
        const id = map.layers.get(layer)[cy * map.width + cx];
        if (isFlagLayer(layer) || id === 0) return;
        brush = { w: 1, h: 1, tiles: [id] };
        const s = setOf(id);
        palSet = sets.indexOf(s);
        const n = id - s.first;
        palSel = { c0: n % s.cols, r0: Math.floor(n / s.cols), c1: n % s.cols, r1: Math.floor(n / s.cols) };
        tool = "pen";
        renderBar();
        drawPalette();
        status(`吸管：${s.name} 第 ${n} 格（編號 ${id}）`);
    }

    // 拖曳很快時，滑鼠事件之間會跳過幾格：用直線把中間的格子補起來（Bresenham 演算法）
    function line(x0, y0, x1, y1, fn) {
        const dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0);
        const sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        let err = dx + dy;
        for (;;) {
            fn(x0, y0);
            if (x0 === x1 && y0 === y1) break;
            const e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    // ---- 復原、重做 ----
    // 一筆畫（按下到放開）記成一步：{ layer, changes: Map(位置 → [舊值, 新值]) }
    // 改大小、加圖層這種整張地圖的修改，記成「整張的快照」：{ before, after }

    function commitStroke() {
        if (stroke && stroke.changes.size) {
            undoStack.push({ layer: stroke.layer, changes: stroke.changes });
            redoStack = [];
            changed();
        }
        stroke = null;
        renderBar();
    }

    function snapshotEdit(fn) {
        const before = serialize();
        fn();
        undoStack.push({ before, after: serialize() });
        redoStack = [];
        rebuildSets();
        renderBar();
        drawPalette();
        changed();
    }

    function undoRedo(from, to, useOld) {
        const step = from.pop();
        if (!step) return;
        if (step.changes) {
            const a = map.layers.get(step.layer);
            for (const [i, [oldV, newV]] of step.changes) a[i] = useOld ? oldV : newV;
        } else {
            const keepLayer = layer;
            map = parse(useOld ? step.before : step.after);
            rebuildSets();
            layer = keepLayer === OBJ || map.layers.has(keepLayer) ? keepLayer : map.layers.keys().next().value;
            if (!map.objects[selected]) selected = -1;
            palSet = Math.min(palSet, sets.length - 1);
            drawPalette();
        }
        to.push(step);
        renderBar();
        changed();
    }
    const undo = () => undoRedo(undoStack, redoStack, true);
    const redo = () => undoRedo(redoStack, undoStack, false);

    // ============================================================
    // 滑鼠、鍵盤
    // ============================================================

    function cellOf(e) {
        const r = mapCanvas.getBoundingClientRect();
        const t = tile * zoom;
        return { cx: Math.floor((e.clientX - r.left) / t), cy: Math.floor((e.clientY - r.top) / t) };
    }

    mapCanvas.addEventListener("contextmenu", (e) => e.preventDefault());   // 右鍵用來擦掉，不要跳出選單
    mapCanvas.addEventListener("pointerdown", (e) => {
        if (!map) return;
        root.focus();
        mapCanvas.setPointerCapture(e.pointerId);   // 拖到畫布外面也繼續收到滑鼠事件
        const { cx, cy } = cellOf(e);
        if (layer === OBJ) {
            if (cx < 0 || cy < 0 || cx >= map.width || cy >= map.height) return;
            const i = objectAt(cx, cy);
            if (e.button === 2) {                       // 右鍵：刪除
                if (i >= 0) { selected = i; deleteSelected(); }
                return;
            }
            if (i >= 0) {                               // 點到物件：選取，準備拖曳
                selected = i;
                drag = { before: serialize(), moved: false };
                renderBar();
                dirty = true;
            } else {                                    // 點到空格：放一個新的
                snapshotEdit(() => { map.objects.push(newObject(objType, cx, cy)); });
                selected = map.objects.length - 1;
                renderBar();
            }
            return;
        }
        stroke = { layer, changes: new Map(), last: { cx, cy }, erase: e.button === 2 };
        apply(cx, cy, stroke.erase);
        if (tool === "fill" || tool === "picker") commitStroke();   // 這兩個工具點一下就結束
    });
    mapCanvas.addEventListener("pointermove", (e) => {
        if (!map) return;
        const c = cellOf(e);
        if (!hover || hover.cx !== c.cx || hover.cy !== c.cy) {
            hover = c;
            dirty = true;
            const inside = c.cx >= 0 && c.cy >= 0 && c.cx < map.width && c.cy < map.height;
            const id = inside && layer !== OBJ ? map.layers.get(layer)[c.cy * map.width + c.cx] : null;
            if (inside && layer === OBJ) status(`第 ${c.cx} 欄、第 ${c.cy} 列`);
            if (id !== null) status(`第 ${c.cx} 欄、第 ${c.cy} 列 · ${layer} = ${id}`);
        }
        if (drag) {
            const o = map.objects[selected];
            const cx = Math.max(0, Math.min(map.width - 1, c.cx)), cy = Math.max(0, Math.min(map.height - 1, c.cy));
            if (o && (o.x !== cx || o.y !== cy)) { o.x = cx; o.y = cy; drag.moved = true; dirty = true; }
            return;
        }
        if (stroke && (tool === "pen" || tool === "eraser" || stroke.erase)) {
            line(stroke.last.cx, stroke.last.cy, c.cx, c.cy, (x, y) => apply(x, y, stroke.erase));
            stroke.last = c;
        }
    });
    mapCanvas.addEventListener("pointerup", () => {
        if (stroke) commitStroke();
        if (drag) {
            // 拖曳結束：整個拖曳算一步復原
            if (drag.moved) {
                undoStack.push({ before: drag.before, after: serialize() });
                redoStack = [];
                changed();
            }
            drag = null;
            renderBar();
        }
    });
    mapCanvas.addEventListener("pointerleave", () => { hover = null; dirty = true; });

    // 圖塊盤：點一格或拖曳框選
    let palDrag = false;
    function palCell(e) {
        const s = sets[palSet];
        const r = palCanvas.getBoundingClientRect();
        const t = s.tile * PALETTE_ZOOM;
        return {
            c: Math.max(0, Math.min(s.cols - 1, Math.floor((e.clientX - r.left) / t))),
            r: Math.max(0, Math.min(s.rows - 1, Math.floor((e.clientY - r.top) / t))),
        };
    }
    function useSelection() {
        const s = sets[palSet];
        const c0 = Math.min(palSel.c0, palSel.c1), c1 = Math.max(palSel.c0, palSel.c1);
        const r0 = Math.min(palSel.r0, palSel.r1), r1 = Math.max(palSel.r0, palSel.r1);
        const tiles = [];
        for (let r = r0; r <= r1; r++) for (let c = c0; c <= c1; c++) tiles.push(s.first + r * s.cols + c);
        brush = { w: c1 - c0 + 1, h: r1 - r0 + 1, tiles };
        if (tool !== "fill") tool = "pen";
        renderBar();
        const n = r0 * s.cols + c0;
        status(brush.w * brush.h === 1
            ? `畫筆：${s.name} 第 ${n} 格（地圖編號 ${s.first + n}）`
            : `畫筆：${s.name} ${brush.w} × ${brush.h} 格`);
    }
    palCanvas.addEventListener("pointerdown", (e) => {
        if (!map) return;
        palCanvas.setPointerCapture(e.pointerId);
        const { c, r } = palCell(e);
        palSel = { c0: c, r0: r, c1: c, r1: r };
        palDrag = true;
        drawPalette();
    });
    palCanvas.addEventListener("pointermove", (e) => {
        if (!palDrag) return;
        const { c, r } = palCell(e);
        palSel.c1 = c;
        palSel.r1 = r;
        drawPalette();
    });
    palCanvas.addEventListener("pointerup", () => {
        if (!palDrag) return;
        palDrag = false;
        useSelection();
    });

    root.tabIndex = 0;
    root.addEventListener("keydown", (e) => {
        if (!map || ["INPUT", "SELECT", "TEXTAREA"].includes(e.target.tagName)) return;
        const mod = e.ctrlKey || e.metaKey;
        if (mod && e.key.toLowerCase() === "z") {
            e.preventDefault();
            if (e.shiftKey) redo(); else undo();
            return;
        }
        if (mod) return;
        if (layer === OBJ) {
            if (e.key === "Delete" || e.key === "Backspace") { e.preventDefault(); deleteSelected(); }
            return;
        }
        const t = TOOLS.find((x) => x.key === e.key.toLowerCase());
        if (t) { tool = t.id; renderBar(); dirty = true; }
    });

    $(".me-undo").addEventListener("click", undo);
    $(".me-redo").addEventListener("click", redo);
    $(".me-zoom").addEventListener("change", (e) => { zoom = Number(e.target.value); dirty = true; });
    $(".me-grid").addEventListener("change", () => { dirty = true; });
    $(".me-dim").addEventListener("change", () => { dirty = true; });
    $(".me-set").addEventListener("change", (e) => { palSet = Number(e.target.value); palSel = null; drawPalette(); });
    $(".me-add-set").addEventListener("change", (e) => {
        const name = e.target.value;
        if (!name) return;
        // 新的圖接在最後面，原本格子的編號都不會變
        snapshotEdit(() => { map.tilesets.push(name); });
        palSet = sets.length - 1;
        drawPalette();
        toast(`已加入圖片 ${name}`);
    });
    $(".me-add-layer").addEventListener("click", () => {
        const name = (prompt("新圖層的名稱（英文、數字、底線，例如 top；叫 solid 就是碰撞層）") || "").trim();
        if (!name) return;
        if (!LAYER_NAME.test(name)) { toast("圖層名稱只能用英文、數字、底線，最多 15 個字"); return; }
        if (map.layers.has(name)) { toast(`已經有 ${name} 圖層了`); return; }
        if (map.layers.size >= 8) { toast("最多 8 個圖層"); return; }
        snapshotEdit(() => { map.layers.set(name, new Uint16Array(map.width * map.height)); layer = name; });
    });
    $(".me-resize").addEventListener("click", () => {
        const w = Number($(".me-w").value), h = Number($(".me-h").value);
        if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1 || w > 1024 || h > 1024) {
            toast("大小要是 1–1024 的整數");
            return;
        }
        if (w === map.width && h === map.height) return;
        // 從左上角對齊：變大的地方是空的，變小就切掉右邊、下面
        snapshotEdit(() => {
            for (const [name, a] of map.layers) {
                const b = new Uint16Array(w * h);
                for (let y = 0; y < Math.min(h, map.height); y++) {
                    for (let x = 0; x < Math.min(w, map.width); x++) b[y * w + x] = a[y * map.width + x];
                }
                map.layers.set(name, b);
            }
            map.width = w;
            map.height = h;
        });
    });

    // ============================================================
    // 對外的介面
    // ============================================================

    // 把專案的圖片解成可以畫的樣子（同一份 bytes 只解一次）
    const decoded = new Map();
    async function loadImages(list) {
        const out = [];
        for (const e of list) {
            if (!decoded.has(e.bytes)) {
                const png = await decodePNG(e.bytes, inflate);
                const sheet = document.createElement("canvas");
                sheet.width = png.width;
                sheet.height = png.height;
                const rgba = new Uint8ClampedArray(png.rgba.buffer, png.rgba.byteOffset, png.rgba.length);
                sheet.getContext("2d").putImageData(new ImageData(rgba, png.width, png.height), 0, 0);
                decoded.set(e.bytes, sheet);
            }
            const sheet = decoded.get(e.bytes);
            out.push({ name: e.name, tile: e.tile, sheet, cols: Math.floor(sheet.width / e.tile), rows: Math.floor(sheet.height / e.tile) });
        }
        return out;
    }

    return {
        // 打開一張地圖。text：地圖檔的內容；imageList：專案所有圖片 [{ name, tile, bytes }]
        // 同一張地圖重新打開（例如圖片改了）會保留目前的圖層、工具和復原紀錄
        // maps：專案所有地圖的名字（門要通往哪張地圖）
        async open(key, text, imageList, maps = []) {
            mapNames = maps;
            images = await loadImages(imageList);
            const same = root.dataset.key === key;
            map = parse(text);
            rebuildSets();
            if (!same) {
                root.dataset.key = key;
                layer = map.layers.keys().next().value;
                palSet = 0;
                palSel = { c0: 0, r0: 0, c1: 0, r1: 0 };
                brush = { w: 1, h: 1, tiles: [sets[0].first] };
                undoStack = [];
                redoStack = [];
                selected = -1;
                status("左鍵畫、右鍵擦掉；右邊的圖塊盤點一格當畫筆，拖曳可以一次選好幾格");
            } else if (layer !== OBJ && !map.layers.has(layer)) {
                layer = map.layers.keys().next().value;
            }
            if (!map.objects[selected]) selected = -1;
            palSet = Math.min(palSet, sets.length - 1);
            renderBar();
            drawPalette();
            dirty = true;
        },
        focus() { root.focus(); },
    };
}

// 新地圖的內容：w × h 格，ground、deco、solid 三個空的圖層，地面先鋪滿 groundTile
export function newMapText(w, h, tilesets, groundTile = 1) {
    const layer = (v) => Array.from({ length: h }, () => Array(w).fill(v));
    const rows = (r) => r.map((row) => `      [${row.join(",")}]`).join(",\n");
    return `{\n  "width": ${w},\n  "height": ${h},\n  "tilesets": [${tilesets.map((t) => JSON.stringify(t)).join(", ")}],\n  "layers": {\n` +
        `    "ground": [\n${rows(layer(groundTile))}\n    ],\n` +
        `    "deco": [\n${rows(layer(0))}\n    ],\n` +
        `    "solid": [\n${rows(layer(0))}\n    ]\n  }\n}\n`;
}
