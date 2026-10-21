// debugpanel.js — Studio 的「狀態」分頁（Studio 清單 23）
//
// 開發遊戲時用，發布的遊戲沒有：
//   1. 指令列：在正在執行的遊戲裡跑一行 Lua（BAG.wood = 99、warp("route1", 9, 3)）。
//      先當算式算（顯示結果），不是算式再當一般敘述執行。↑↓ 叫出之前打過的指令
//   2. 快捷按鈕：遊戲自己定義的 DEBUG_COMMANDS = { { "按鈕上的字", "要執行的 Lua" }, … }
//   3. 清除這個遊戲的存檔（遊戲的 id 看原始碼開頭的 -- id:）
//   5. 工具：遊戲定義 DEBUG_TOOLS = { { name, fields = { { 欄位, 標題, "choice"／"int", 預設, 選項 } }, run = function(值) return 文字 end } }
//      這裡畫成表單，按「執行」呼叫遊戲的 run，結果顯示在下面（Wildkin 的戰鬥試算、平衡表）
//   4. 狀態樹：遊戲定義 DEBUG_VIEW = { "DAY", "party", "BAG" }（算式字串），這裡每半秒更新一次。
//      點表格展開／收合；點一個值，指令列會填好「路徑 = 值」，改一改按 Enter 就生效
//
// 都是透過 engine.eval（C 的 engine_eval）：在遊戲的 Lua 環境裡執行，所以看到的就是遊戲現在的樣子

import { saveKey, browserSaveStore } from "./comet.js";

const HISTORY_KEY = "comet.studio.debugHistory";
const REFRESH_MS = 500;

// Lua 的 %q 字串（"a\nb"）還原成 JavaScript 字串
function unquote(s) {
    if (!(s.startsWith("\"") && s.endsWith("\""))) return s;
    const bytes = [];
    const enc = new TextEncoder();
    for (let i = 1; i < s.length - 1; i++) {
        const c = s[i];
        if (c !== "\\") { bytes.push(...enc.encode(c)); continue; }
        const n = s[++i];
        if (n === "n") bytes.push(10);
        else if (/[0-9]/.test(n)) {
            let d = n;
            while (d.length < 3 && /[0-9]/.test(s[i + 1])) d += s[++i];
            bytes.push(Number(d));
        } else bytes.push(...enc.encode(n));
    }
    return new TextDecoder().decode(new Uint8Array(bytes));
}

// 原始碼開頭 10 行裡的「-- id: 作者/名稱」（和 C 的 save_on_load 一樣）；沒有是 "_"
function gameIdOf(source) {
    const lines = source.split("\n", 10);
    for (const l of lines) {
        const m = l.match(/^-- id:\s*(\S+)\s*$/);
        if (m) return m[1];
    }
    return "_";
}

export function createDebugPanel({ root, engine, toast, getSource, isVisible }) {
    root.innerHTML = `
      <div class="dbg-cmd">
        <input class="dbg-input" spellcheck="false" placeholder="輸入一行 Lua，按 Enter 執行。例如：BAG.wood = 99　warp(&quot;route1&quot;, 9, 3)">
        <button class="dbg-run">執行</button>
      </div>
      <div class="dbg-buttons"></div>
      <div class="dbg-tools"></div>
      <div class="dbg-out"></div>
      <div class="dbg-tree"></div>`;
    const input = root.querySelector(".dbg-input");
    const out = root.querySelector(".dbg-out");
    const treeEl = root.querySelector(".dbg-tree");
    const buttonsEl = root.querySelector(".dbg-buttons");

    let history = [];
    try { history = JSON.parse(localStorage.getItem(HISTORY_KEY)) || []; } catch { history = []; }
    let histPos = history.length;

    function addOut(text, kind) {
        const div = document.createElement("div");
        div.className = "dbg-line " + (kind || "");
        div.textContent = text;
        out.appendChild(div);
        while (out.children.length > 30) out.firstChild.remove();
        out.scrollTop = out.scrollHeight;
    }

    // 執行一段 Lua，結果顯示在下面
    function exec(code, label) {
        code = code.trim();
        if (!code) return;
        const r = engine.eval(code, 0);
        addOut(`> ${label || code}`, "cmd");
        addOut(r.ok ? r.text : `錯誤：${r.text}`, r.ok ? "" : "err");
        refreshTree(true);
    }

    function run() {
        const code = input.value.trim();
        if (!code) return;
        if (history[history.length - 1] !== code) history.push(code);
        history = history.slice(-50);
        try { localStorage.setItem(HISTORY_KEY, JSON.stringify(history)); } catch { /* 不存也沒關係 */ }
        histPos = history.length;
        input.value = "";
        exec(code);
    }
    input.addEventListener("keydown", (e) => {
        e.stopPropagation();   // 不要讓遊戲收到這些按鍵
        if (e.key === "Enter") { run(); e.preventDefault(); }
        else if (e.key === "ArrowUp" && histPos > 0) { input.value = history[--histPos]; e.preventDefault(); }
        else if (e.key === "ArrowDown") {
            histPos = Math.min(history.length, histPos + 1);
            input.value = history[histPos] || "";
            e.preventDefault();
        }
    });
    root.querySelector(".dbg-run").addEventListener("click", run);

    // ---- 快捷按鈕（遊戲的 DEBUG_COMMANDS）＋ 清除存檔 ----
    const LIST_CMDS = "(function() local t = {} for _, c in ipairs(rawget(_G, 'DEBUG_COMMANDS') or {}) do " +
        "t[#t + 1] = tostring(c[1]) .. '\\t' .. tostring(c[2]) end return table.concat(t, '\\n') end)()";
    let lastButtons = null;
    function refreshButtons() {
        const r = engine.eval(LIST_CMDS, 2);
        const text = r.ok ? unquote(r.text) : "";
        if (text === lastButtons) return;
        lastButtons = text;
        buttonsEl.innerHTML = "";
        for (const line of text.split("\n")) {
            if (!line) continue;
            const [label, code] = line.split("\t");
            const b = document.createElement("button");
            b.textContent = label;
            b.title = code;
            b.addEventListener("click", () => exec(code, label));
            buttonsEl.appendChild(b);
        }
        const clear = document.createElement("button");
        clear.className = "dbg-danger";
        clear.textContent = "🗑 清除存檔";
        clear.title = "刪除這個遊戲的全部存檔（欄位 0–7），瀏覽器裡的，不能復原";
        clear.addEventListener("click", () => {
            const id = gameIdOf(getSource() || "");
            if (!confirm(`刪除「${id}」的全部存檔？不能復原。`)) return;
            const store = browserSaveStore();
            for (let slot = 0; slot < 8; slot++) store.remove(saveKey(id, slot));
            toast("存檔已清除（重新執行後生效）");
        });
        buttonsEl.appendChild(clear);
    }

    // ---- 工具（遊戲的 DEBUG_TOOLS）----
    const toolsEl = root.querySelector(".dbg-tools");
    const LIST_TOOLS = "(function() local t = {} for i, tool in ipairs(rawget(_G, 'DEBUG_TOOLS') or {}) do " +
        "t[#t + 1] = 'T\\t' .. i .. '\\t' .. tostring(tool.name) " +
        "for _, f in ipairs(tool.fields or {}) do local opts = f[5] if type(opts) == 'function' then opts = opts() end " +
        "local o = {} for _, p in ipairs(opts or {}) do o[#o + 1] = tostring(p[1]) .. '=' .. tostring(p[2]) end " +
        "t[#t + 1] = 'F\\t' .. f[1] .. '\\t' .. f[2] .. '\\t' .. f[3] .. '\\t' .. tostring(f[4]) .. '\\t' .. table.concat(o, '|') end end " +
        "return table.concat(t, '\\n') end)()";
    let lastTools = null;
    function refreshTools() {
        const r = engine.eval(LIST_TOOLS, 2);
        const text = r.ok ? unquote(r.text) : "";
        if (text === lastTools) return;
        lastTools = text;
        toolsEl.innerHTML = "";
        let tool = null;
        for (const line of text.split("\n")) {
            const p = line.split("\t");
            if (p[0] === "T") {
                tool = { index: p[1], fields: [] };
                const box = document.createElement("details");
                box.className = "dbg-tool";
                box.innerHTML = `<summary></summary><div class="dbg-form"></div><pre class="dbg-tool-out"></pre>`;
                box.querySelector("summary").textContent = "🧮 " + p[2];
                tool.box = box;
                const run = document.createElement("button");
                run.textContent = "執行";
                const t = tool;
                run.addEventListener("click", () => {
                    const parts = t.fields.map((f) => {
                        const v = f.input.value;
                        return `${f.key} = ${f.kind === "int" ? (parseInt(v, 10) || 0) : JSON.stringify(v)}`;
                    });
                    const out = box.querySelector(".dbg-tool-out");
                    out.textContent = "執行中……";
                    setTimeout(() => {
                        const r = engine.eval(`DEBUG_TOOLS[${t.index}].run({ ${parts.join(", ")} })`, 0);
                        out.textContent = r.ok ? unquote(r.text) : `錯誤：${r.text}`;
                    }, 10);
                });
                tool.run = run;
                toolsEl.appendChild(box);
            } else if (p[0] === "F" && tool) {
                const [, key, label, kind, def, opts] = p;
                const wrap = document.createElement("label");
                wrap.textContent = label + " ";
                let input;
                if (kind === "choice") {
                    input = document.createElement("select");
                    for (const o of (opts || "").split("|").filter(Boolean)) {
                        const i = o.indexOf("=");
                        const opt = document.createElement("option");
                        opt.value = o.slice(0, i);
                        opt.textContent = o.slice(i + 1);
                        input.appendChild(opt);
                    }
                    input.value = def;
                } else {
                    input = document.createElement("input");
                    input.type = "number";
                    input.value = def;
                    input.style.width = "60px";
                }
                input.addEventListener("keydown", (e) => e.stopPropagation());
                wrap.appendChild(input);
                tool.box.querySelector(".dbg-form").appendChild(wrap);
                tool.fields.push({ key, kind, input });
            }
            if (tool && tool.run) tool.box.querySelector(".dbg-form").appendChild(tool.run);
        }
    }

    // ---- 狀態樹（遊戲的 DEBUG_VIEW）----
    const open = new Set();   // 展開的表格（用路徑記）；第 0 層預設展開
    const closed = new Set(); // 第 0 層被收起來的
    let lastText = null;

    function isOpen(depth, path) {
        return depth === 0 ? !closed.has(path) : open.has(path);
    }

    function render(text) {
        treeEl.innerHTML = "";
        if (!text) {
            treeEl.innerHTML = `<div class="dbg-hint">遊戲裡定義 <code>DEBUG_VIEW = { "DAY", "party", "BAG" }</code>，這裡就會顯示這些值（每半秒更新）。</div>`;
            return;
        }
        let hideBelow = Infinity;   // 收起來的表格底下的行不顯示
        for (const line of text.split("\n")) {
            const [d, path, label, value] = line.split("\t");
            const depth = Number(d);
            if (depth > hideBelow) continue;
            hideBelow = Infinity;
            const row = document.createElement("div");
            row.className = "dbg-row";
            row.style.paddingLeft = `${depth * 14}px`;
            const isTable = /^\{\d+\}$/.test(value);
            if (isTable) {
                const o = isOpen(depth, path);
                if (!o) hideBelow = depth;
                row.innerHTML = `<span class="dbg-arrow">${o ? "▾" : "▸"}</span><span class="dbg-key"></span> <span class="dbg-count"></span>`;
                row.querySelector(".dbg-count").textContent = `${value.slice(1, -1)} 項`;
                row.classList.add("dbg-table");
                row.addEventListener("click", () => {
                    if (depth === 0) { if (closed.has(path)) closed.delete(path); else closed.add(path); }
                    else if (open.has(path)) open.delete(path); else open.add(path);
                    render(lastText);
                });
            } else {
                row.innerHTML = `<span class="dbg-arrow"></span><span class="dbg-key"></span> = <span class="dbg-val"></span>`;
                row.querySelector(".dbg-val").textContent = value;
                if (path) {
                    row.title = "點一下：在指令列改這個值";
                    row.addEventListener("click", () => {
                        input.value = `${path} = ${value}`;
                        input.focus();
                        input.setSelectionRange(path.length + 3, input.value.length);
                    });
                }
            }
            row.querySelector(".dbg-key").textContent = label;
            treeEl.appendChild(row);
        }
    }

    function refreshTree(force) {
        if (!force && !isVisible()) return;
        const r = engine.eval("", 1);
        const text = r.ok ? r.text : `0\t\t（錯誤）\t${r.text}`;
        if (text === lastText && !force) return;
        lastText = text;
        render(text);
    }

    setInterval(() => {
        if (!isVisible()) return;
        refreshButtons();
        refreshTools();
        refreshTree(false);
    }, REFRESH_MS);

    return {
        // 遊戲重新執行之後呼叫：按鈕可能變了
        reset() { lastButtons = null; lastText = null; lastTools = null; },
    };
}
