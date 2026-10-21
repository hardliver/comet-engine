// studio.js — 網頁版遊戲編輯器 Comet Studio
// 左邊寫 Lua，右邊立刻看到結果。引擎和遊玩頁是同一個（comet.js）。
//
// 兩種模式：
//   單檔模式：程式碼存在瀏覽器裡，適合寫小遊戲、試東西
//   資料夾模式：直接讀寫電腦上的專案資料夾（有 comet.json），適合大遊戲。
//              用 VS Code 等其他編輯器改檔案，這裡也會自動重新載入

import {
    EditorView, basicSetup, keymap, EditorState, StateEffect, StateField,
    Decoration, indentWithTab, StreamLanguage, indentUnit, lua, oneDark,
} from "./vendor/codemirror.js";
import { createEngine, startLoop } from "./comet.js";
import { setupKeyboard, readGamepad, merge } from "./input.js";
import { buildCart, buildImages, buildMaps, buildData, dataFiles, dataEntries, imageEntries, imageFiles, mapFiles, mapLine, browserInflate, fontTextFiles, fontTextCode } from "./cart.js";
import { createImageViewer } from "./imageview.js";
import { createMapEditor, newMapText } from "./mapedit.js";
import { createDataEditor } from "./dataedit.js";
import { buildFont, fontSummary } from "./font.js";
import { createDebugPanel } from "./debugpanel.js";

const STORAGE_KEY = "comet.studio.code";
const $ = (id) => document.getElementById(id);

// ============================================================
// 小工具
// ============================================================

let toastTimer = null;
function toast(text, ms = 2500) {
    $("toast").textContent = text;
    $("toast").classList.add("show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => $("toast").classList.remove("show"), ms);
}

// 瀏覽器儲存空間可能被關掉，失敗就算了
function saveLocal(code) { try { localStorage.setItem(STORAGE_KEY, code); } catch (e) {} }
function loadLocal() { try { return localStorage.getItem(STORAGE_KEY); } catch (e) { return null; } }

// 複製到剪貼簿。用區網 IP 打開時（不是 https），瀏覽器不給用新的剪貼簿 API，改用舊方法
async function copyText(text) {
    try {
        await navigator.clipboard.writeText(text);
        return true;
    } catch (e) {
        const ta = document.createElement("textarea");
        ta.value = text;
        document.body.appendChild(ta);
        ta.select();
        const ok = document.execCommand("copy");
        ta.remove();
        return ok;
    }
}

// ============================================================
// 主控台：print 的輸出和錯誤訊息
// ============================================================

function consoleClear() { $("console").innerHTML = ""; }

function consoleAdd(text, kind = "") {
    const div = document.createElement("div");
    div.className = `line ${kind}`;
    div.textContent = text;
    $("console").appendChild(div);
    $("console").scrollTop = $("console").scrollHeight;
    return div;
}

// 引擎印出的每一行都會到這裡。
// Lua 的錯誤訊息長這樣："game:12: attempt to call a nil value"。
// 資料夾模式下，遊戲檔是好幾個檔案合併的，要把行號換回原本的檔案和行號
let errorAt = null;    // { path, line }：執行時的錯誤位置（單檔模式 path 是 null）
let syntaxAt = null;   // { path, line }：打字時發現的語法錯誤位置

// 遊戲檔的第 n 行 → 原本是哪個檔案的第幾行
function locate(cartLine) {
    const where = { path: null, line: cartLine };
    return folder ? mapLine(folder.segments, cartLine) || where : where;
}

function label(where) {
    return where.path ? `${where.path} 第 ${where.line} 行` : `第 ${where.line} 行`;
}

// 拆開 Lua 的錯誤訊息 "game:12: 'end' expected (to close 'function' at line 5) near <eof>"
// 回傳 { where, text }；訊息中間的 "at line 5" 也換成原本的檔案和行號。不是這種格式回傳 null
function parseError(msg) {
    const m = msg.match(/^game:(\d+):/);
    if (!m) return null;
    const text = msg.slice(m[0].length).trim()
        .replace(/at line (\d+)/g, (_, n) => `at ${label(locate(Number(n)))}`);
    return { where: locate(Number(m[1])), text };
}

function onLog(msg) {
    const e = parseError(msg);
    if (!e) {
        consoleAdd(msg);
        return;
    }
    const where = e.where;
    const div = consoleAdd(`${label(where)}出錯：${e.text}`, "err");
    div.addEventListener("click", () => jumpTo(where));
    errorAt = where;
    showErrorMark();
    showTab("console");
}

// ============================================================
// 程式碼編輯器（CodeMirror）
// ============================================================

// 錯誤行的紅色背景
const setErrorLine = StateEffect.define();
const errorLineField = StateField.define({
    create: () => Decoration.none,
    update(deco, tr) {
        deco = deco.map(tr.changes);
        for (const e of tr.effects) {
            if (!e.is(setErrorLine)) continue;
            if (e.value === null) {
                deco = Decoration.none;
            } else {
                const n = Math.min(Math.max(e.value, 1), tr.state.doc.lines);
                const from = tr.state.doc.line(n).from;
                deco = Decoration.set([Decoration.line({ class: "cm-error-line" }).range(from)]);
            }
        }
        return deco;
    },
    provide: (f) => EditorView.decorations.from(f),
});

// 語法錯誤那一行：橘色波浪底線
const setSyntaxLine = StateEffect.define();
const syntaxLineField = StateField.define({
    create: () => Decoration.none,
    update(deco, tr) {
        deco = deco.map(tr.changes);
        for (const e of tr.effects) {
            if (!e.is(setSyntaxLine)) continue;
            if (e.value === null) {
                deco = Decoration.none;
            } else {
                const n = Math.min(Math.max(e.value, 1), tr.state.doc.lines);
                const from = tr.state.doc.line(n).from;
                deco = Decoration.set([Decoration.line({ class: "cm-syntax-line" }).range(from)]);
            }
        }
        return deco;
    },
    provide: (f) => EditorView.decorations.from(f),
});

// 目前畫面上的檔案如果就是出錯的檔案，標出錯誤行（執行錯誤、語法錯誤各自標）
function showErrorMark() {
    const here = folder ? folder.current : null;
    const line = errorAt && errorAt.path === here ? errorAt.line : null;
    const sline = syntaxAt && syntaxAt.path === here ? syntaxAt.line : null;
    editor.dispatch({ effects: [setErrorLine.of(line), setSyntaxLine.of(sline)] });
}

// 編輯器下方的語法錯誤提示。text = null 代表沒有錯誤
function showSyntax(where, text) {
    syntaxAt = where;
    const bar = $("syntax-msg");
    bar.hidden = text === null;
    if (text !== null) {
        bar.textContent = `${where ? label(where) : ""}語法錯誤：${text}（遊戲繼續執行上一個版本）`;
        bar.onclick = () => where && jumpTo(where);
    }
    showErrorMark();
}

async function jumpTo(where) {
    if (folder && where.path && where.path !== folder.current) await openFile(where.path);
    const n = Math.min(where.line, editor.state.doc.lines);
    editor.dispatch({ selection: { anchor: editor.state.doc.line(n).from }, scrollIntoView: true });
    editor.focus();
}

// 貼上 AI 的整段回覆時，自動只留下 ```lua ... ``` 裡的程式碼
function extractCodeBlock(text) {
    const m = text.match(/```(?:lua)?[ \t]*\r?\n([\s\S]*?)```/);
    return m ? m[1] : null;
}

let autoRunTimer = null;

// 每個檔案有自己的 EditorState（內容、游標、復原紀錄），切換檔案就是換 state
function makeState(doc) {
    return EditorState.create({
        doc,
        extensions: [
            basicSetup,
            StreamLanguage.define(lua),
            indentUnit.of("  "),
            oneDark,
            errorLineField,
            syntaxLineField,
            keymap.of([
                indentWithTab,
                { key: "Mod-Enter", run: () => { run(); return true; } },
                { key: "Mod-s", run: () => { saveNow(); return true; } },
            ]),
            EditorView.updateListener.of((u) => {
                if (!u.docChanged) return;
                onEdit(u.state.doc.toString());
            }),
            EditorView.domEventHandlers({
                paste(e, view) {
                    const text = e.clipboardData?.getData("text/plain") || "";
                    const code = extractCodeBlock(text);
                    if (code === null) return false;   // 一般的貼上
                    e.preventDefault();
                    view.dispatch(view.state.replaceSelection(code));
                    toast("已自動去掉 AI 回覆裡的說明文字，只貼上程式碼");
                    return true;
                },
            }),
        ],
    });
}

const editor = new EditorView({ parent: $("editor"), state: makeState("") });

function setDoc(code) {
    editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: code } });
}

// 每次修改：存檔（單檔存瀏覽器、資料夾寫回檔案），停止打字 0.7 秒後重新執行
let saveTimer = null;
function onEdit(text) {
    if (folder) {
        const f = folder.files.get(folder.current);
        f.text = text;
        f.dirty = f.text !== f.savedText;
        renderTabs();
        clearTimeout(saveTimer);
        saveTimer = setTimeout(saveNow, 800);
    } else {
        saveLocal(text);
    }
    if ($("auto-run").checked) {
        clearTimeout(autoRunTimer);
        autoRunTimer = setTimeout(autoRun, 700);
    }
}

// 自動重新執行（停止打字、其他編輯器存檔時）：先只檢查語法。
// 打到一半的程式通常有語法錯誤，這時不重新執行，遊戲繼續跑上一個版本，
// 只在編輯器標出錯誤的地方。語法正確才真的重新執行
function autoRun() {
    clearTimeout(autoRunTimer);
    let code;
    try {
        code = currentCart();
    } catch (e) {
        showSyntax(null, e.message);   // 例如 comet.json 列了不存在的檔案
        return;
    }
    const err = engine.check(code);
    if (err) {
        const e = parseError(err);
        showSyntax(e ? e.where : null, e ? e.text : err);
        return;
    }
    showSyntax(null, null);
    run();
}

// ============================================================
// 引擎
// ============================================================

const engine = await createEngine({ onLog });
const keyboard = setupKeyboard();
const canvas = $("screen");

function getInput(player) {
    if (player === 0) return merge(keyboard(), readGamepad(0));
    return readGamepad(player);
}
const loop = startLoop(engine, canvas, getInput);

// 狀態分頁：遊戲的 DEBUG_VIEW、DEBUG_COMMANDS、除錯指令（debugpanel.js）
let lastCart = "";   // 最後一次載入的遊戲（清除存檔要知道遊戲的 id）
const debugPanel = createDebugPanel({
    root: $("state"),
    engine,
    toast: (t) => toast(t),
    getSource: () => lastCart,
    isVisible: () => !$("state").hidden,
});

// 圖片檢視器（下方的「圖片」分頁）：點格子複製 spr(...)
const viewer = createImageViewer({
    select: $("img-select"),
    canvas: $("img-canvas"),
    info: $("img-info"),
    inflate: browserInflate,
    onPick: async (code) => {
        await copyText(code);
        toast(`已複製：${code}`);
    },
});

// 專案的圖片：[{ name, file, tile, bytes }]（圖片檢視器、地圖編輯器用）
function projectImages() {
    return imageEntries(folder.project)
        .filter((e) => folder.images.has(e.file))
        .map((e) => ({ ...e, bytes: folder.images.get(e.file).bytes }));
}

function refreshImageViewer() {
    if (!folder) {
        viewer.setImages([], "單檔模式沒有圖片。用「開啟資料夾」打開有 comet.json 的專案，就能在這裡看圖片和格子編號");
        return;
    }
    viewer.setImages(projectImages());
}

// 地圖編輯器：點檔案分頁裡的地圖時，取代左邊的程式碼編輯器
const mapEditor = createMapEditor({ root: $("mapedit"), inflate: browserInflate, toast, onChange: mapChanged });
let mapSaveTimer = null;

const isMapOpen = () => folder && folder.maps.has(folder.current);

// 地圖被修改：馬上重新合併（遊戲畫面更新），0.5 秒後存檔
async function mapChanged(text) {
    const m = folder.maps.get(folder.current);
    m.text = text;
    m.dirty = true;
    renderTabs();
    await rebuildMaps();
    if ($("auto-run").checked) {
        clearTimeout(autoRunTimer);
        autoRunTimer = setTimeout(autoRun, 300);
    }
    clearTimeout(mapSaveTimer);
    mapSaveTimer = setTimeout(saveMaps, 500);
}

// 資料表編輯器：點檔案分頁裡的資料檔（comet.json 的 data）時，取代左邊的程式碼編輯器
const dataEditor = createDataEditor({ root: $("dataedit"), toast, onChange: dataChanged });
let dataSaveTimer = null;

const isDataOpen = () => folder && folder.data.has(folder.current);

// 表格裡顯示名稱用：專案裡有 types、species 資料檔的話，下拉選單會列出它們
function dataRefs() {
    const refs = {};
    for (const e of dataEntries(folder.project)) {
        const d = folder.data.get(e.file);
        if (!d) continue;
        try {
            const v = JSON.parse(d.text);
            if (e.name === "types" && v.names) refs.types = v.names;
            if (e.name === "species") refs.species = Object.fromEntries(Object.entries(v).map(([k, s]) => [k, s.name ?? k]));
        } catch (err) {}
    }
    return refs;
}

// 資料改了：馬上重新合併（遊戲用新的數值重新執行），0.5 秒後存檔
async function dataChanged(text) {
    const d = folder.data.get(folder.current);
    d.text = text;
    d.dirty = true;
    renderTabs();
    await rebuildData();
    if ($("auto-run").checked) {
        clearTimeout(autoRunTimer);
        autoRunTimer = setTimeout(autoRun, 300);
    }
    clearTimeout(dataSaveTimer);
    dataSaveTimer = setTimeout(saveData, 500);
}

async function saveData() {
    clearTimeout(dataSaveTimer);
    for (const [path, d] of folder.data) {
        if (!d.dirty) continue;
        d.lastModified = await writeText(folder.dir, path, d.text);
        d.dirty = false;
    }
    renderTabs();
}

async function saveMaps() {
    clearTimeout(mapSaveTimer);
    for (const [path, m] of folder.maps) {
        if (!m.dirty) continue;
        m.lastModified = await writeText(folder.dir, path, m.text);
        m.dirty = false;
    }
    renderTabs();
}

// 字型（c11.bin，330 KB）：開啟 Studio 時讀一次。讀不到的話，用 text() 的遊戲會出錯，其他照常
const fontBytes = await fetch("fonts/c11.bin")
    .then((r) => (r.ok ? r.arrayBuffer() : null))
    .then((b) => (b ? new Uint8Array(b) : null))
    .catch(() => null);
let fontInfo = "";   // 這次合併用了幾個字（run() 印在主控台）

function fontFor(files) {
    if (!fontBytes) return "";
    const font = buildFont(files, fontBytes);
    fontInfo = fontSummary(font.stats);
    return font.code;
}

// 目前要執行的遊戲檔：單檔模式就是編輯器的內容；資料夾模式是所有檔案合併的結果
function currentCart() {
    fontInfo = "";
    if (!folder) {
        // 單檔模式：字型接在最後面，原本的行號才不會跑掉。
        // 檔案最上層的程式碼會先全部執行完，引擎才呼叫 _init，所以放最後也來得及
        const doc = editor.state.doc.toString();
        const fontCode = fontFor({ "main.lua": doc });
        return fontCode ? `${doc}\n\n${fontCode}\n` : doc;
    }
    const texts = {};
    for (const [path, f] of folder.files) texts[path] = f.text;
    if (folder.mapsError) throw new Error(folder.mapsError);   // 地圖檔格式有錯：顯示原因，不執行
    if (folder.dataError) throw new Error(folder.dataError);   // 資料檔有錯（JSON 格式、小數……）：同上
    // 字型也要掃描地圖物件裡的字（例如告示牌的內容），以及 comet.json 的 fontText 列的檔案
    const fontCode = fontFor({ ...texts, "(maps)": folder.mapsCode, "(data)": folder.dataCode, "(fontText)": folder.fontText });
    const { code, segments } = buildCart(folder.project, texts, folder.imagesCode, fontCode, folder.mapsCode, folder.dataCode);
    folder.segments = segments;
    folder.cart = code;
    return code;
}

// 重新執行：清掉主控台和錯誤標記，從頭載入
function run() {
    clearTimeout(autoRunTimer);
    consoleClear();
    errorAt = null;
    showSyntax(null, null);
    let code;
    try {
        code = currentCart();
    } catch (e) {
        consoleAdd(e.message, "err");
        return;
    }
    const err = engine.load(code);
    lastCart = code;
    debugPanel.reset();
    if (!err) consoleAdd("▶ 遊戲已重新開始", "info");
    if (fontInfo) consoleAdd(fontInfo, "info");
    if (folder && folder.project.output) writeOutput();
}

// 狀態列：目前第幾幀
setInterval(() => {
    const paused = loop.isPaused();
    const speed = loop.speed();
    $("frame-count").textContent = `${paused ? "⏸ " : ""}${speed > 1 ? `⏩ ×${speed}　` : ""}第 ${engine.frameCount()} 幀`;
    $("frame-count").className = paused ? "" : (speed > 1 ? "playing fast" : "playing");
}, 200);

// 點遊戲畫面 → 鍵盤控制遊戲；點程式碼 → 鍵盤打字
canvas.addEventListener("pointerdown", () => canvas.focus());

// ============================================================
// 資料夾模式
// ============================================================
// 用瀏覽器的 File System Access API：使用者選一個資料夾、同意讀寫之後，
// 網頁就能直接讀寫裡面的檔案（Chrome、Edge 支援；Safari、Firefox 目前不支援）。

let folder = null;
refreshImageViewer();   // 要在 folder 宣告之後才能呼叫（let 在宣告之前不能用）
// folder = {
//   dir:        資料夾的 handle
//   project:    comet.json 的內容
//   projectMod: comet.json 的修改時間（被改了就整個重新開啟）
//   files:      Map(路徑 → { state, text, savedText, dirty, lastModified })
//   images:     Map(路徑 → { bytes, lastModified })：comet.json 裡 images 的 PNG
//   imagesCode: 圖片轉成的 Lua 程式碼（圖片有變才重新轉換）
//   maps:       Map(路徑 → { text, lastModified })：comet.json 裡 maps 的 JSON
//   mapsCode:   地圖轉成的 Lua 程式碼；mapsError：地圖檔有錯時的訊息
//   data:       Map(路徑 → { text, lastModified })：comet.json 裡 data 的 JSON
//   dataCode:   資料檔轉成的 Lua 程式碼；dataError：資料檔有錯時的訊息
//   current:    目前編輯的檔案路徑
//   segments:   合併後每個檔案在遊戲檔裡的行號範圍
// }

// 依路徑取得檔案 handle，例如 "src/main.lua"。create = true 時順便建立資料夾和檔案
async function fileHandle(dir, path, create = false) {
    const parts = path.split("/");
    const name = parts.pop();
    for (const p of parts) dir = await dir.getDirectoryHandle(p, { create });
    return dir.getFileHandle(name, { create });
}

async function readText(dir, path) {
    const file = await (await fileHandle(dir, path)).getFile();
    return { text: await file.text(), lastModified: file.lastModified };
}

async function readBytes(dir, path) {
    const file = await (await fileHandle(dir, path)).getFile();
    return { bytes: new Uint8Array(await file.arrayBuffer()), lastModified: file.lastModified };
}

// 資源包（comet.json 的 "pak"，見 core/pak.h）：讀進來交給引擎。
// 回傳 true 代表資源包有變（要重新執行）
async function loadPak() {
    const path = folder.project.pak;
    if (!path) {
        engine.setPak(null);
        folder.pakMod = undefined;
        return false;
    }
    try {
        const file = await (await fileHandle(folder.dir, path)).getFile();
        if (file.lastModified === folder.pakMod) return false;
        folder.pakMod = file.lastModified;
        if (engine.setPak(new Uint8Array(await file.arrayBuffer()))) consoleAdd(`資源包 ${path} 格式不對`, "err");
        return true;
    } catch (e) {
        if (folder.pakMod !== null) toast(`找不到資源包 ${path}`, 5000);
        folder.pakMod = null;
        engine.setPak(null);
        return false;
    }
}

// 把 comet.json 的 images 轉成 Lua 程式碼（PNG → 顏色編號 → base64）
async function rebuildImages() {
    try {
        folder.imagesCode = await buildImages(folder.project, async (p) => {
            const img = folder.images.get(p);
            if (!img) throw new Error(`找不到圖片 ${p}`);
            return img.bytes;
        }, browserInflate);
    } catch (e) {
        folder.imagesCode = "";
        consoleAdd(`圖片轉換失敗：${e.message}`, "err");
    }
    refreshImageViewer();
    if (isMapOpen()) openFile(folder.current);
}

// 把 comet.json 的 data 轉成 Lua 程式碼。有錯時記下原因，執行時顯示
async function rebuildData() {
    try {
        folder.dataCode = await buildData(folder.project, async (p) => {
            const d = folder.data.get(p);
            if (!d) throw new Error(`找不到資料檔 ${p}`);
            return d.text;
        });
        folder.dataError = null;
    } catch (e) {
        folder.dataCode = "";
        folder.dataError = e.message;
    }
}

// 把 comet.json 的 maps 轉成 Lua 程式碼。格式有錯時記下原因，執行時顯示
async function rebuildMaps() {
    try {
        folder.mapsCode = await buildMaps(folder.project, async (p) => {
            const m = folder.maps.get(p);
            if (!m) throw new Error(`找不到地圖檔 ${p}`);
            return m.text;
        });
        folder.mapsError = null;
    } catch (e) {
        folder.mapsCode = "";
        folder.mapsError = e.message;
    }
}

async function writeText(dir, path, text) {
    const h = await fileHandle(dir, path, true);
    const w = await h.createWritable();
    await w.write(text);
    await w.close();
    return (await h.getFile()).lastModified;
}

// 資料夾裡沒有 comet.json：找出 .lua 檔，自動建一個（main.lua 放最後）
async function createDefaultProject(dir) {
    const found = [];
    async function scan(d, prefix) {
        for await (const [name, h] of d.entries()) {
            if (h.kind === "file" && name.endsWith(".lua")) found.push(prefix + name);
            if (h.kind === "directory" && name === "src" && prefix === "") await scan(h, "src/");
        }
    }
    await scan(dir, "");
    found.sort((a, b) => (a.endsWith("main.lua") - b.endsWith("main.lua")) || a.localeCompare(b));
    if (found.length === 0) {
        found.push("main.lua");
        await writeText(dir, "main.lua", "function _init()\nend\n\nfunction _update()\nend\n\nfunction _draw()\n  cls(1)\nend\n");
    }
    const project = { name: dir.name, sources: found, output: "build/game.lua" };
    await writeText(dir, "comet.json", JSON.stringify(project, null, 2) + "\n");
    toast("這個資料夾沒有 comet.json，已經自動建立一個");
    return project;
}

async function openFolder(dir) {
    // 要求讀寫權限（第一次會跳出瀏覽器的確認視窗）
    if ((await dir.queryPermission({ mode: "readwrite" })) !== "granted" &&
        (await dir.requestPermission({ mode: "readwrite" })) !== "granted") {
        toast("沒有取得資料夾的讀寫權限");
        return;
    }

    let project, projectMod = 0;
    try {
        const r = await readText(dir, "comet.json");
        project = JSON.parse(r.text);
        projectMod = r.lastModified;
    } catch (e) {
        if (e instanceof SyntaxError) {
            toast(`comet.json 格式錯誤：${e.message}`, 6000);
            return;
        }
        project = await createDefaultProject(dir);
    }

    const files = new Map();
    for (const path of project.sources) {
        try {
            const { text, lastModified } = await readText(dir, path);
            files.set(path, { state: makeState(text), text, savedText: text, dirty: false, lastModified });
        } catch (e) {
            toast(`找不到 ${path}`, 5000);
        }
    }

    const images = new Map();
    for (const path of imageFiles(project)) {
        try {
            images.set(path, await readBytes(dir, path));
        } catch (e) {
            toast(`找不到圖片 ${path}`, 5000);
        }
    }

    const maps = new Map();
    for (const path of mapFiles(project)) {
        try {
            maps.set(path, await readText(dir, path));
        } catch (e) {
            toast(`找不到地圖 ${path}`, 5000);
        }
    }

    const data = new Map();
    for (const path of dataFiles(project)) {
        try {
            data.set(path, await readText(dir, path));
        } catch (e) {
            toast(`找不到資料檔 ${path}`, 5000);
        }
    }

    // fontText：只在打開資料夾時讀一次（改了要重新打開資料夾）
    const fontTexts = [];
    for (const path of fontTextFiles(project)) {
        try {
            fontTexts.push((await readText(dir, path)).text);
        } catch (e) {
            toast(`找不到 ${path}（comet.json 的 fontText）`, 5000);
        }
    }

    folder = { dir, project, projectMod, files, images, imagesCode: "", maps, mapsCode: "", mapsError: null,
               data, dataCode: "", dataError: null,
               fontText: fontTextCode(fontTexts), current: null, segments: [] };
    await rebuildImages();
    await rebuildMaps();
    await rebuildData();
    await loadPak();
    document.body.classList.add("folder-mode");
    $("files").hidden = false;
    $("project-name").textContent = project.name || dir.name;
    $("btn-reopen").hidden = true;
    rememberFolder(dir);

    // 預設打開 main.lua（通常是最後一個）
    await openFile(project.sources[project.sources.length - 1]);
    run();
}

async function openFile(path) {
    const isMap = folder.maps.has(path);
    const isData = folder.data.has(path);
    if (!folder.files.has(path) && !isMap && !isData) return;
    // 保留目前程式碼檔的游標、復原紀錄（目前是地圖就不用）
    if (folder.files.has(folder.current)) folder.files.get(folder.current).state = editor.state;
    folder.current = path;
    $("editor").hidden = isMap || isData;
    $("mapedit").hidden = !isMap;
    $("dataedit").hidden = !isData;
    if (isData) {
        dataEditor.open(path, folder.data.get(path).text, dataRefs());
    } else if (isMap) {
        try {
            await mapEditor.open(path, folder.maps.get(path).text, projectImages(), Object.keys(folder.project.maps || {}));
            mapEditor.focus();
        } catch (e) {
            toast(`地圖打不開：${e.message}`, 6000);
        }
    } else {
        editor.setState(folder.files.get(path).state);
    }
    renderTabs();
    showErrorMark();
}

function renderTabs() {
    const bar = $("files");
    bar.innerHTML = "";
    for (const [path, f] of folder.files) {
        const b = document.createElement("button");
        b.textContent = path.replace(/^src\//, "");
        b.title = path;
        b.className = (path === folder.current ? "on " : "") + (f.dirty ? "dirty" : "");
        b.addEventListener("click", () => openFile(path));
        bar.appendChild(b);
    }
    for (const [path, m] of folder.maps) {
        const b = document.createElement("button");
        b.textContent = "🗺 " + path.replace(/^maps\//, "");
        b.title = path;
        b.className = (path === folder.current ? "on " : "") + (m.dirty ? "dirty" : "");
        b.addEventListener("click", () => openFile(path));
        bar.appendChild(b);
    }
    for (const [path, d] of folder.data) {
        const b = document.createElement("button");
        b.textContent = "📋 " + path.replace(/^data\//, "");
        b.title = path + "（資料表）";
        b.className = (path === folder.current ? "on " : "") + (d.dirty ? "dirty" : "");
        b.addEventListener("click", () => openFile(path));
        bar.appendChild(b);
    }
    const add = document.createElement("button");
    add.textContent = "＋ 新檔案";
    add.className = "add";
    add.addEventListener("click", newFile);
    bar.appendChild(add);
    const addMap = document.createElement("button");
    addMap.textContent = "＋ 新地圖";
    addMap.className = "add";
    addMap.addEventListener("click", newMap);
    bar.appendChild(addMap);
}

// 新地圖：maps/名稱.json，20 × 15 格（剛好一個畫面），地面鋪滿第一張圖的第 0 格，
// 自動加進 comet.json 的 maps
async function newMap() {
    const imgs = imageEntries(folder.project);
    if (imgs.length === 0) { toast("先在 comet.json 的 images 加上圖片，才能畫地圖"); return; }
    let name = (prompt("新地圖的名稱（英文、數字、底線，例如 camp；遊戲裡用 MAP.camp 取得）") || "").trim();
    if (!name) return;
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) { toast("地圖名稱只能用英文、數字、底線"); return; }
    const path = `maps/${name}.json`;
    if (folder.maps.has(path)) { openFile(path); return; }

    // 用得上的圖：和第一張圖的小格一樣大的，最多 4 張
    const tilesets = imgs.filter((e) => e.tile === imgs[0].tile).slice(0, 4).map((e) => e.name);
    const text = newMapText(20, 15, tilesets, 1);
    const lastModified = await writeText(folder.dir, path, text);
    folder.maps.set(path, { text, lastModified });
    folder.project.maps = { ...(folder.project.maps || {}), [name]: path };
    folder.projectMod = await writeText(folder.dir, "comet.json", JSON.stringify(folder.project, null, 2) + "\n");
    await rebuildMaps();
    await openFile(path);
    toast(`已建立 ${path}，遊戲裡用 MAP.${name} 取得`);
}

// 把有修改的檔案寫回資料夾
async function saveNow() {
    if (!folder) { download(); return; }
    clearTimeout(saveTimer);
    for (const [path, f] of folder.files) {
        if (!f.dirty) continue;
        f.lastModified = await writeText(folder.dir, path, f.text);
        f.savedText = f.text;
        f.dirty = false;
    }
    await saveMaps();
    await saveData();
}

// 合併後的遊戲檔也寫一份到 comet.json 的 output（例如 build/wildkin.lua），
// 不用另外下指令建置
async function writeOutput() {
    try {
        await writeText(folder.dir, folder.project.output, folder.cart);
    } catch (e) { /* 寫不進去不影響執行 */ }
}

// 新增原始碼檔：放進 src/，加到 comet.json 的清單裡（main.lua 前面）
async function newFile() {
    let name = prompt("新檔案的名稱（例如 battle.lua，會放在 src/ 裡）");
    if (!name) return;
    name = name.trim();
    if (!name.endsWith(".lua")) name += ".lua";
    const path = name.includes("/") ? name : `src/${name}`;
    if (folder.files.has(path)) { openFile(path); return; }

    const text = `-- ${name}\n`;
    const lastModified = await writeText(folder.dir, path, text);
    folder.files.set(path, { state: makeState(text), text, savedText: text, dirty: false, lastModified });

    const src = folder.project.sources;
    const mainAt = src.findIndex((p) => p.endsWith("main.lua"));
    src.splice(mainAt >= 0 ? mainAt : src.length, 0, path);
    folder.projectMod = await writeText(folder.dir, "comet.json", JSON.stringify(folder.project, null, 2) + "\n");

    // Map 的順序要和 sources 一樣，分頁才會照合併順序排
    folder.files = new Map(src.filter((p) => folder.files.has(p)).map((p) => [p, folder.files.get(p)]));
    await openFile(path);
}

// 每 1.5 秒檢查一次：檔案被別的程式（例如 VS Code）改過，就重新載入並重新執行
setInterval(async () => {
    if (!folder) return;
    let changed = false;
    for (const [path, f] of folder.files) {
        if (f.dirty) continue;   // 這裡有還沒存的修改，不要覆蓋
        try {
            const file = await (await fileHandle(folder.dir, path)).getFile();
            if (file.lastModified === f.lastModified) continue;
            const text = await file.text();
            f.lastModified = file.lastModified;
            if (text === f.text) continue;
            f.text = f.savedText = text;
            if (path === folder.current) {
                setDoc(text);
                f.dirty = false;
            } else {
                f.state = makeState(text);
            }
            changed = true;
        } catch (e) { /* 檔案暫時讀不到，下次再試 */ }
    }
    // 圖片被修改（例如用繪圖軟體存檔）：重新轉換
    for (const [path, img] of folder.images) {
        try {
            const file = await (await fileHandle(folder.dir, path)).getFile();
            if (file.lastModified === img.lastModified) continue;
            folder.images.set(path, { bytes: new Uint8Array(await file.arrayBuffer()), lastModified: file.lastModified });
            await rebuildImages();
            changed = true;
        } catch (e) {}
    }
    // 地圖檔被修改（地圖編輯器存檔、手動改 JSON）：重新轉換
    for (const [path, m] of folder.maps) {
        try {
            const file = await (await fileHandle(folder.dir, path)).getFile();
            if (file.lastModified === m.lastModified || m.dirty) continue;
            folder.maps.set(path, { text: await file.text(), lastModified: file.lastModified });
            await rebuildMaps();
            if (path === folder.current) await openFile(path);
            changed = true;
        } catch (e) {}
    }

    // 資料檔被修改（用 VS Code 改數字、之後的資料表編輯器存檔）：重新轉換
    for (const [path, d] of folder.data) {
        try {
            const file = await (await fileHandle(folder.dir, path)).getFile();
            if (file.lastModified === d.lastModified || d.dirty) continue;
            folder.data.set(path, { text: await file.text(), lastModified: file.lastModified });
            await rebuildData();
            if (path === folder.current) await openFile(path);
            changed = true;
        } catch (e) {}
    }

    // 資源包被重新產生（例如跑了打包程式）
    if (await loadPak()) changed = true;

    // comet.json 被修改（例如加了新圖片、新檔案）：整個重新開啟
    try {
        const file = await (await fileHandle(folder.dir, "comet.json")).getFile();
        if (folder.projectMod && file.lastModified !== folder.projectMod) {
            folder.projectMod = file.lastModified;
            await saveNow();
            await openFolder(folder.dir);
            toast("comet.json 改變了，已重新開啟專案");
            return;
        }
    } catch (e) {}

    if (changed) {
        renderTabs();
        autoRun();
        toast("偵測到檔案被修改，已重新載入");
    }
}, 1500);

// 記住上次開的資料夾（存在瀏覽器的 IndexedDB），下次打開編輯器可以一鍵重新開啟
function idb() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open("comet-studio", 1);
        req.onupgradeneeded = () => req.result.createObjectStore("handles");
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}
async function rememberFolder(dir) {
    try {
        const db = await idb();
        db.transaction("handles", "readwrite").objectStore("handles").put(dir, "last");
    } catch (e) {}
}
async function lastFolder() {
    try {
        const db = await idb();
        return await new Promise((resolve) => {
            const req = db.transaction("handles").objectStore("handles").get("last");
            req.onsuccess = () => resolve(req.result || null);
            req.onerror = () => resolve(null);
        });
    } catch (e) { return null; }
}

$("btn-folder").addEventListener("click", async () => {
    if (!window.showDirectoryPicker) {
        toast("這個瀏覽器不支援開啟資料夾，請用 Chrome 或 Edge", 5000);
        return;
    }
    try {
        const dir = await window.showDirectoryPicker({ mode: "readwrite" });
        await openFolder(dir);
    } catch (e) {
        if (e.name !== "AbortError") toast(`無法開啟資料夾：${e.message}`, 5000);
    }
});

// ============================================================
// 工具列
// ============================================================

$("btn-run").addEventListener("click", () => { run(); canvas.focus(); });

$("btn-pause").addEventListener("click", () => {
    const p = !loop.isPaused();
    loop.setPaused(p);
    $("btn-pause").textContent = p ? "繼續" : "暫停";
    $("btn-step").disabled = !p;
});
$("btn-step").addEventListener("click", () => loop.step());

// ---- 加速（開發用）----
// ×1／×2／×4：每 1/60 秒跑幾幀（comet.js 的 setSpeed）。
// 在遊戲畫面上按住 `（數字 1 左邊那個鍵）：放開之前暫時 ×4，放開就回到選好的速度。
// 只有 Studio 有，遊玩頁和網頁版 Nova 沒有，所以不會影響玩家
let chosenSpeed = 1;
let turbo = false;
function applySpeed() {
    loop.setSpeed(turbo ? 4 : chosenSpeed);
    for (const b of $("speed").children) b.classList.toggle("on", Number(b.dataset.speed) === chosenSpeed);
}
for (const b of $("speed").children) {
    b.addEventListener("click", () => { chosenSpeed = Number(b.dataset.speed); applySpeed(); canvas.focus(); });
}
canvas.addEventListener("keydown", (e) => {
    if (e.code === "Backquote" && !turbo) { turbo = true; applySpeed(); }
});
canvas.addEventListener("keyup", (e) => {
    if (e.code === "Backquote") { turbo = false; applySpeed(); }
});
canvas.addEventListener("blur", () => { if (turbo) { turbo = false; applySpeed(); } });   // 按著時點到別的地方

$("template").addEventListener("change", async (e) => {
    const url = e.target.value;
    e.target.value = "";
    if (!url) return;
    if (editor.state.doc.toString().trim() &&
        !confirm("要用範例取代目前的程式碼嗎？\n目前的程式碼會被覆蓋（建議先「下載 .lua」備份）。")) return;
    setDoc(await fetch(url).then((r) => r.text()));
    run();
});

const picker = $("file-picker");
$("btn-open").addEventListener("click", () => picker.click());
picker.addEventListener("change", async () => {
    const file = picker.files[0];
    picker.value = "";
    if (!file) return;
    setDoc(await file.text());
    run();
});
// 單檔模式：把 .lua 檔拖進編輯器以外的地方也能開啟
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", async (e) => {
    if (folder || e.target.closest?.(".cm-editor")) return;
    e.preventDefault();
    const file = e.dataTransfer.files[0];
    if (file) { setDoc(await file.text()); run(); }
});

// 下載遊戲檔（資料夾模式下是合併後的整個遊戲）
function download() {
    const code = currentCart();
    const blob = new Blob([code], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = folder ? (folder.project.output || "game.lua").split("/").pop() : "game.lua";
    a.click();
    URL.revokeObjectURL(a.href);
}
$("btn-download").addEventListener("click", download);

// 複製給 AI：說明文件 + 程式碼 + 讓使用者填寫的需求。
// 資料夾模式會附上每個檔案，並請 AI 回覆時標明改了哪個檔案
const guide = await fetch("AI_GUIDE.md").then((r) => r.text());

$("btn-ai").addEventListener("click", async () => {
    let codePart;
    if (folder) {
        codePart = "## 目前的程式碼\n\n這個專案分成好幾個檔案，依下面的順序合併成一個遊戲檔執行。\n" +
            "修改時請標明檔名，並給出那個檔案的完整內容。\n\n" +
            folder.project.sources.map((p) => `### ${p}\n\n\`\`\`lua\n${folder.files.get(p)?.text ?? ""}\n\`\`\``).join("\n\n");
    } else {
        codePart = `## 目前的程式碼\n\n\`\`\`lua\n${editor.state.doc.toString()}\n\`\`\``;
    }
    const prompt = `${guide}\n\n---\n\n${codePart}\n\n## 我想要\n\n（在這裡寫你想做的遊戲，或想修改的地方）\n`;
    const ok = await copyText(prompt);
    toast(ok
        ? "已複製！貼到 ChatGPT、Claude 等 AI，在最後寫下你想要的，再把 AI 回覆的程式碼貼回來"
        : "複製失敗，請改用 localhost 打開編輯器", 6000);
});

// ============================================================
// 分頁：主控台／API 說明
// ============================================================

function showTab(name) {
    document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
    document.querySelectorAll(".tab-body").forEach((el) => { el.hidden = el.id !== name; });
}
document.querySelectorAll(".tabs button").forEach((b) => {
    b.addEventListener("click", () => showTab(b.dataset.tab));
});

// API 說明直接用 AI_GUIDE.md，人和 AI 看的是同一份
$("api").innerHTML = markdownToHtml(guide);

// 很簡單的 Markdown 轉換：只處理說明文件用到的語法（標題、表格、清單、程式碼）
function markdownToHtml(md) {
    const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const inline = (s) => esc(s)
        .replace(/`([^`]+)`/g, "<code>$1</code>")
        .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");

    const out = [];
    const lines = md.split("\n");
    let i = 0;
    while (i < lines.length) {
        const line = lines[i];
        const t = line.trim();
        if (t.startsWith("```")) {                           // 程式碼區塊
            const indent = line.indexOf("`");
            const body = [];
            i++;
            while (i < lines.length && !lines[i].trim().startsWith("```")) {
                body.push(lines[i].slice(Math.min(indent, lines[i].search(/\S|$/))));
                i++;
            }
            out.push(`<pre>${esc(body.join("\n"))}</pre>`);
            i++;
        } else if (/^#{1,6} /.test(t)) {                     // 標題（# 到 ######）
            const level = t.indexOf(" ");
            out.push(`<h${level}>${inline(t.slice(level + 1))}</h${level}>`);
            i++;
        } else if (t.startsWith("|")) {                      // 表格
            const rows = [];
            while (i < lines.length && lines[i].trim().startsWith("|")) {
                const cells = lines[i].trim().slice(1, -1).split("|").map((c) => c.trim());
                if (!cells.every((c) => /^-+$/.test(c))) rows.push(cells);
                i++;
            }
            out.push("<table>" + rows.map((r, n) =>
                "<tr>" + r.map((c) => n === 0 ? `<th>${inline(c)}</th>` : `<td>${inline(c)}</td>`).join("") + "</tr>"
            ).join("") + "</table>");
        } else if (/^(- |\d+\. )/.test(t)) {                 // 清單
            const items = [];
            while (i < lines.length && /^(- |\d+\. )/.test(lines[i].trim())) {
                items.push(`<li>${inline(lines[i].trim().replace(/^(- |\d+\. )/, ""))}</li>`);
                i++;
            }
            out.push(`<ul>${items.join("")}</ul>`);
        } else if (t === "" || t === "---") {
            i++;
        } else {                                             // 段落
            // 第一行一定收（不管它長怎樣），之後遇到其他格式的開頭才停。
            // 少了這一步，一行以 # 開頭、但又不是標題的字（例如 ####### 或 #標籤）
            // 會讓這裡一行都不收、i 也不前進，外面的迴圈就永遠卡在同一行
            const para = [t];
            i++;
            while (i < lines.length && lines[i].trim() !== "" && !/^(```|#|\||- |\d+\. )/.test(lines[i].trim())) {
                para.push(lines[i].trim());
                i++;
            }
            out.push(`<p>${inline(para.join(" "))}</p>`);
        }
    }
    return out.join("\n");
}

// ============================================================
// 啟動
// ============================================================

// 上次開過資料夾：顯示「重新開啟」按鈕（瀏覽器規定要使用者按一下才能重新取得權限）
const last = await lastFolder();
if (last) {
    $("btn-reopen").hidden = false;
    $("btn-reopen").textContent = `↻ 重新開啟 ${last.name}`;
    $("btn-reopen").addEventListener("click", () => openFolder(last));
}

// 單檔模式：上次的程式碼，沒有的話用「移動方塊」範例
setDoc(loadLocal() ?? await fetch("games/move_box.lua").then((r) => r.text()));
run();
