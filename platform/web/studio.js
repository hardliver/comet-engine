// studio.js — 網頁版遊戲編輯器
// 左邊寫 Lua，右邊立刻看到結果。引擎和遊玩頁是同一個（comet.js）。

import {
    EditorView, basicSetup, keymap, EditorState, StateEffect, StateField,
    Decoration, indentWithTab, StreamLanguage, indentUnit, lua, oneDark,
} from "./vendor/codemirror.js";
import { createEngine, startLoop } from "./comet.js";
import { setupKeyboard, readGamepad } from "./input.js";

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
function save(code) { try { localStorage.setItem(STORAGE_KEY, code); } catch (e) {} }
function loadSaved() { try { return localStorage.getItem(STORAGE_KEY); } catch (e) { return null; } }

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
// Lua 的錯誤訊息長這樣："game:12: attempt to call a nil value"，
// 抓出行號，在編輯器裡標紅，點一下訊息可以跳過去
function onLog(msg) {
    const m = msg.match(/^game:(\d+):/);
    if (!m) {
        consoleAdd(msg);
        return;
    }
    const line = Number(m[1]);
    const div = consoleAdd(`第 ${line} 行出錯：${msg.slice(m[0].length).trim()}`, "err");
    div.addEventListener("click", () => jumpToLine(line));
    markErrorLine(line);
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

function markErrorLine(n) { editor.dispatch({ effects: setErrorLine.of(n) }); }
function clearErrorLine() { editor.dispatch({ effects: setErrorLine.of(null) }); }

function jumpToLine(n) {
    const line = editor.state.doc.line(Math.min(n, editor.state.doc.lines));
    editor.dispatch({ selection: { anchor: line.from }, scrollIntoView: true });
    editor.focus();
}

// 貼上 AI 的整段回覆時，自動只留下 ```lua ... ``` 裡的程式碼
function extractCodeBlock(text) {
    const m = text.match(/```(?:lua)?[ \t]*\r?\n([\s\S]*?)```/);
    return m ? m[1] : null;
}

let autoRunTimer = null;

const editor = new EditorView({
    parent: $("editor"),
    state: EditorState.create({
        doc: "",
        extensions: [
            basicSetup,
            StreamLanguage.define(lua),
            indentUnit.of("  "),
            oneDark,
            errorLineField,
            keymap.of([
                indentWithTab,
                { key: "Mod-Enter", run: () => { run(); return true; } },
                { key: "Mod-s", run: () => { download(); return true; } },
            ]),
            EditorView.updateListener.of((u) => {
                if (!u.docChanged) return;
                save(u.state.doc.toString());
                if ($("auto-run").checked) {
                    clearTimeout(autoRunTimer);
                    autoRunTimer = setTimeout(run, 700);   // 停止打字 0.7 秒後才重跑
                }
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
    }),
});

function getCode() { return editor.state.doc.toString(); }

function setCode(code) {
    editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: code } });
}

// ============================================================
// 引擎
// ============================================================

const engine = await createEngine({ onLog });
const keyboard = setupKeyboard();
const canvas = $("screen");

function getInput(player) {
    if (player === 0) return keyboard() | readGamepad(0);
    return readGamepad(player);
}
const loop = startLoop(engine, canvas, getInput);

// 重新執行：清掉主控台和錯誤標記，從頭載入目前的程式碼
function run() {
    clearTimeout(autoRunTimer);
    consoleClear();
    clearErrorLine();
    const err = engine.load(getCode());
    if (!err) consoleAdd("▶ 遊戲已重新開始", "info");
}

// 狀態列：目前第幾幀
setInterval(() => {
    const paused = loop.isPaused();
    $("frame-count").textContent = `${paused ? "⏸ " : ""}第 ${engine.frameCount()} 幀`;
    $("frame-count").className = paused ? "" : "playing";
}, 200);

// 點遊戲畫面 → 鍵盤控制遊戲；點程式碼 → 鍵盤打字
canvas.addEventListener("pointerdown", () => canvas.focus());

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

$("template").addEventListener("change", async (e) => {
    const url = e.target.value;
    e.target.value = "";
    if (!url) return;
    if (getCode().trim() && !confirm("要用範例取代目前的程式碼嗎？\n目前的程式碼會被覆蓋（建議先「下載 .lua」備份）。")) return;
    setCode(await fetch(url).then((r) => r.text()));
    run();
});

const picker = $("file-picker");
$("btn-open").addEventListener("click", () => picker.click());
picker.addEventListener("change", async () => {
    const file = picker.files[0];
    picker.value = "";
    if (!file) return;
    setCode(await file.text());
    run();
});
// 把 .lua 檔拖進編輯器以外的地方也能開啟
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", async (e) => {
    if (e.target.closest?.(".cm-editor")) return;   // 拖進編輯器裡就照一般方式處理
    e.preventDefault();
    const file = e.dataTransfer.files[0];
    if (file) { setCode(await file.text()); run(); }
});

function download() {
    const blob = new Blob([getCode()], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "game.lua";
    a.click();
    URL.revokeObjectURL(a.href);
}
$("btn-download").addEventListener("click", download);

// 複製給 AI：說明文件 + 目前的程式碼 + 讓使用者填寫的需求
const guide = await fetch("AI_GUIDE.md").then((r) => r.text());

$("btn-ai").addEventListener("click", async () => {
    const prompt =
        `${guide}\n\n---\n\n## 目前的程式碼\n\n\`\`\`lua\n${getCode()}\n\`\`\`\n\n` +
        `## 我想要\n\n（在這裡寫你想做的遊戲，或想修改的地方）\n`;
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
    $("console").hidden = name !== "console";
    $("api").hidden = name !== "api";
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
        } else if (/^#{1,3} /.test(t)) {                     // 標題
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
            const para = [];
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
// 啟動：上次的程式碼，沒有的話用「移動方塊」範例
// ============================================================

setCode(loadSaved() ?? await fetch("games/move_box.lua").then((r) => r.text()));
run();
