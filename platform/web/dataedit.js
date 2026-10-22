// dataedit.js — Comet Studio 的資料表編輯器（comet.json 的 data 裡的 JSON 檔）
//
// 和 Studio 的分工：Studio 決定打開哪個檔案、存到哪裡；這個檔案只管「編輯」。
// 編輯方式依檔案的形狀自動決定：
//   表格：最外層是「編號 → 一筆資料」（例如 species.json、moves.json）。
//         每一列是一筆、每一欄是一個欄位；整數欄只能填整數，"type" 欄是屬性的下拉選單，
//         "species" 欄是野伴的下拉選單（專案裡有 types、species 資料檔時），
//         表格、陣列類的欄位用一行 JSON 填。填錯的格子變紅，不會存檔
//   遭遇表：每一筆有 rate 和 list（[{ species, min, max, weight }]），例如 encounters.json。
//     每隻可以另外有 times（時段 → 改用的權重）、seasons（季節 → 權重乘幾倍），用一行 JSON 編輯；
//     有用到 times 的區域，下面多一張「各時段出現機率」（2026-10-22 Neil：原本看不到時段的設定）
//         專用畫面：遇敵率直接換算成每格的 %，每隻野伴旁邊算出出現的機率
//   文字：其他形狀（例如 types.json 的相剋表）用文字編輯，即時檢查 JSON 格式
//
// 存檔格式固定（formatData）：最外層一筆一行，用 git 看差異時一眼就知道改了哪一筆。

// ============================================================
// 存檔格式（不碰畫面，check_web.mjs 測試 13 也用）
// ============================================================

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);

// 一行的 JSON，逗號和冒號後面加空格：{ "hp": 45, "atk": 49 }
function oneLine(v) {
    if (Array.isArray(v)) return v.length ? "[ " + v.map(oneLine).join(", ") + " ]" : "[]";
    if (isObj(v)) {
        const ks = Object.keys(v);
        return ks.length ? "{ " + ks.map((k) => JSON.stringify(k) + ": " + oneLine(v[k])).join(", ") + " }" : "{}";
    }
    return JSON.stringify(v);
}

// 一筆資料：通常一行；如果某個欄位是「物件的陣列」（遭遇表的 list），那個陣列一個元素一行
function formatRow(row, indent) {
    if (!isObj(row)) return oneLine(row);
    const longKey = Object.keys(row).find((k) => Array.isArray(row[k]) && row[k].length > 0 && row[k].every(isObj));
    if (!longKey) return oneLine(row);
    const pad = " ".repeat(indent + 2);
    const parts = Object.keys(row).map((k) => {
        if (k !== longKey) return pad + JSON.stringify(k) + ": " + oneLine(row[k]);
        return pad + JSON.stringify(k) + ": [\n" + row[k].map((e) => pad + "  " + oneLine(e)).join(",\n") + "\n" + pad + "]";
    });
    return "{\n" + parts.join(",\n") + "\n" + " ".repeat(indent) + "}";
}

// 檔案最外層的編號，照檔案裡的順序。
// 為什麼不用 Object.keys：JavaScript 會把「像整數」的編號（"10"）自動排到最前面，
// 存回去順序就亂了（"01"～"09" 有前導零不算整數，所以 "10" 會跑到 "01" 前面）
export function topKeys(text) {
    const keys = [];
    let depth = 0, i = 0;
    while (i < text.length) {
        const c = text[i];
        if (c === '"') {
            let j = i + 1;
            while (j < text.length && text[j] !== '"') j += text[j] === "\\" ? 2 : 1;
            const str = text.slice(i, j + 1);
            let k = j + 1;
            while (k < text.length && /\s/.test(text[k])) k++;
            if (depth === 1 && text[k] === ":") keys.push(JSON.parse(str));
            i = j + 1;
            continue;
        }
        if (c === "{" || c === "[") depth++;
        else if (c === "}" || c === "]") depth--;
        i++;
    }
    return keys;
}

// order：最外層的順序（topKeys 的結果；新加的編號會在 order 裡）
export function formatData(data, order) {
    if (!isObj(data)) return JSON.stringify(data, null, 2) + "\n";
    const ks = order ? order.filter((k) => k in data) : Object.keys(data);
    for (const k of Object.keys(data)) if (!ks.includes(k)) ks.push(k);
    if (ks.length === 0) return "{}\n";
    return "{\n" + ks.map((k) => "  " + JSON.stringify(k) + ": " + formatRow(data[k], 2)).join(",\n") + "\n}\n";
}

// 這份資料用哪一種畫面編輯
export function dataKind(data) {
    if (!isObj(data)) return "text";
    const rows = Object.values(data);
    if (rows.length === 0 || !rows.every(isObj)) return "text";
    if (rows.every((r) => Number.isInteger(r.rate) && Array.isArray(r.list) && r.list.every((e) => isObj(e) && "species" in e && "weight" in e)))
        return "encounters";
    // 表格：每一筆的欄位大部分是簡單的值（不是「名稱 → 表格」那種，例如相剋表）
    const simple = rows.every((r) => Object.values(r).filter(isObj).length <= Math.max(1, Object.keys(r).length / 2));
    return simple ? "table" : "text";
}

// 一格的新值（文字 → 正確的型別）。回傳 { ok, value, error }
// sample：這一欄原本的值（決定要什麼型別）
export function parseCell(text, sample) {
    if (Number.isInteger(sample) || (typeof sample === "number")) {
        if (!/^-?\d+$/.test(text.trim())) return { ok: false, error: "要填整數（不能有小數）" };
        return { ok: true, value: parseInt(text, 10) };
    }
    if (typeof sample === "boolean") return { ok: true, value: text === "true" };
    if (typeof sample === "string") return { ok: true, value: text };
    // 物件、陣列：一行 JSON
    try {
        const v = JSON.parse(text);
        if (Array.isArray(sample) !== Array.isArray(v) || (isObj(sample) && !isObj(v))) return { ok: false, error: Array.isArray(sample) ? "要是陣列 [ … ]" : "要是 { … }" };
        return { ok: true, value: v };
    } catch (e) {
        return { ok: false, error: "JSON 格式錯誤：" + e.message };
    }
}

// ============================================================
// 編輯器
// ============================================================

const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
};

// root：放編輯器的元素；onChange(text)：資料改了（text 是新的檔案內容）；toast(msg)：提示
export function createDataEditor({ root, onChange, toast }) {
    let data = null;     // 目前的資料（JS 物件）
    let order = [];      // 最外層的編號，照檔案裡的順序（見 topKeys）
    let path = "";
    let refs = {};       // { types: { grass: "草", … }, species: { "01": "芽角羊", … } }
    let sortCol = null, sortDir = 1;
    let errors = 0;      // 目前有幾格填錯

    root.innerHTML = `
      <div class="de-bar">
        <span class="de-title"></span>
        <span class="de-kind"></span>
        <span class="de-spacer"></span>
        <span class="de-add-wrap"><input class="de-new-id" placeholder="新編號" size="8"><button class="de-add">＋ 新增一筆</button></span>
      </div>
      <div class="de-main"></div>
      <div class="de-status"></div>`;
    const $ = (s) => root.querySelector(s);
    const main = $(".de-main");

    function status(msg, bad) {
        $(".de-status").textContent = msg;
        $(".de-status").classList.toggle("bad", !!bad);
    }

    function commit() {
        if (errors > 0) { status(`有 ${errors} 格填錯（紅色），改好之前不會存檔`, true); return; }
        onChange(formatData(data, order));
        status("已更新（遊戲會用新的數值重新執行，0.5 秒後存檔）");
    }

    // 一格輸入框；sample 決定型別；set(value) 寫回資料
    function cellInput(sample, set, opts = {}) {
        let input;
        if (opts.choices) {
            input = el("select");
            const keys = Object.keys(opts.choices);
            if (!keys.includes(sample)) keys.unshift(sample);
            for (const k of keys) {
                const o = el("option", null, opts.choices[k] !== undefined ? `${opts.choices[k]}（${k}）` : `${k}（不存在）`);
                o.value = k;
                input.appendChild(o);
            }
            input.value = sample;
            input.addEventListener("change", () => { set(input.value); commit(); });
            return input;
        }
        if (typeof sample === "boolean") {
            input = el("input");
            input.type = "checkbox";
            input.checked = sample;
            input.addEventListener("change", () => { set(input.checked); commit(); });
            return input;
        }
        input = el("input");
        const isNum = typeof sample === "number";
        input.value = (typeof sample === "string" || isNum) ? String(sample) : oneLine(sample);
        input.className = isNum ? "de-num" : (typeof sample === "string" ? "de-text" : "de-json");
        if (typeof sample === "string" && sample.length > 16) input.classList.add("de-long");
        let bad = false;
        input.addEventListener("change", () => {
            const r = parseCell(input.value, sample);
            if (!r.ok) {
                if (!bad) errors++;
                bad = true;
                input.classList.add("bad");
                input.title = r.error;
                status(r.error, true);
                return;
            }
            if (bad) errors--;
            bad = false;
            input.classList.remove("bad");
            input.title = "";
            sample = r.value;
            set(r.value);
            commit();
        });
        return input;
    }

    // ---------- 表格 ----------
    function renderTable() {
        const ids = order.filter((k) => k in data);
        const cols = [];
        for (const id of ids) for (const k of Object.keys(data[id])) if (!cols.includes(k)) cols.push(k);
        const shown = ids.slice();   // 畫面上的順序（排序只影響這個，不影響檔案）
        if (sortCol) {
            const val = (id) => (sortCol === "(編號)" ? id : data[id][sortCol]);
            shown.sort((a, b) => {
                const x = val(a), y = val(b);
                if (typeof x === "number" && typeof y === "number") return (x - y) * sortDir;
                return String(x ?? "").localeCompare(String(y ?? "")) * sortDir;
            });
        }
        const table = el("table", "de-table");
        const head = el("tr");
        for (const c of ["(編號)", ...cols, ""]) {
            const th = el("th", null, c === "(編號)" ? "編號" : c);
            if (c) {
                th.title = "點一下排序（只影響顯示）";
                if (c === sortCol) th.textContent += sortDir > 0 ? " ▲" : " ▼";
                th.addEventListener("click", () => {
                    if (sortCol === c) sortDir = -sortDir; else { sortCol = c; sortDir = 1; }
                    render();
                });
            }
            head.appendChild(th);
        }
        table.appendChild(head);
        for (const id of shown) {
            const tr = el("tr");
            tr.appendChild(el("td", "de-id", id));
            for (const c of cols) {
                const td = el("td");
                if (c in data[id]) {
                    const opts = {};
                    if (c === "type" && refs.types) opts.choices = refs.types;
                    if (c === "species" && refs.species) opts.choices = refs.species;
                    td.appendChild(cellInput(data[id][c], (v) => { data[id][c] = v; }, opts));
                }
                tr.appendChild(td);
            }
            const tools = el("td", "de-row-tools");
            const copy = el("button", null, "複製");
            copy.title = "用這一筆當範本新增（填上方的新編號）";
            copy.addEventListener("click", () => addRow(id));
            const del = el("button", null, "✕");
            del.title = "刪除這一筆";
            del.addEventListener("click", () => {
                if (!confirm(`刪除 ${id}？`)) return;
                delete data[id];
                order = order.filter((k) => k !== id);
                render();
                commit();
            });
            tools.append(copy, del);
            tr.appendChild(tools);
            table.appendChild(tr);
        }
        main.appendChild(table);
    }

    // 新增一筆：用 template（或最後一筆）當範本，編號用上方輸入框的
    function addRow(template) {
        const id = $(".de-new-id").value.trim();
        if (!id) { toast("先在上方填新的編號（例如 11、thunder）"); $(".de-new-id").focus(); return; }
        if (id in data) { toast(`編號 ${id} 已經有了`); return; }
        const src = template || order[order.length - 1];
        data[id] = src ? JSON.parse(JSON.stringify(data[src])) : {};
        order.push(id);
        $(".de-new-id").value = "";
        render();
        commit();
        toast(`已新增 ${id}（從 ${src} 複製，記得改內容）`);
    }

    // ---------- 遭遇表 ----------
    // 常見的時段代號顯示成中文（其他的照原樣顯示）
    const PERIOD_NAMES = { morning: "早上", day: "白天", evening: "傍晚", night: "晚上" };
    function renderEncounters() {
        for (const area of order.filter((k) => k in data)) {
            const t = data[area];
            const box = el("div", "de-area");
            const h = el("div", "de-area-head");
            h.appendChild(el("b", null, area));
            h.appendChild(el("span", null, "遇敵率"));
            const rate = cellInput(t.rate, (v) => { t.rate = v; renderPct(); });
            h.appendChild(rate);
            const pct = el("span", "de-muted");
            h.appendChild(pct);
            box.appendChild(h);
            const table = el("table", "de-table");
            table.innerHTML = "<tr><th>野伴</th><th>最低等級</th><th>最高等級</th><th>權重</th><th>出現機率</th>" +
                "<th title='某個時段改用的權重，例如 {\"night\":60,\"day\":15}；0 = 那個時段不出現；空白 = 整天都用「權重」'>時段（改用的權重）</th>" +
                "<th title='某一季權重乘幾倍，例如 {\"autumn\":2}；空白 = 每季一樣'>季節（倍數）</th><th></th></tr>";
            const pctCells = [];
            const timeBox = el("div");
            function renderPct() {
                pct.textContent = `/256，每走一格 ${(t.rate / 2.56).toFixed(1)}%（大約每 ${t.rate > 0 ? Math.round(256 / t.rate) : "∞"} 格遇一次）`;
                const total = t.list.reduce((s, e) => s + (e.weight || 0), 0);
                t.list.forEach((e, i) => { if (pctCells[i]) pctCells[i].textContent = total ? ((e.weight / total) * 100).toFixed(1) + "%" : "—"; });
                renderTimes();
            }
            // 各時段出現機率：只有這個區域有人設 times 才畫
            function renderTimes() {
                timeBox.innerHTML = "";
                const keys = [];
                for (const e of t.list) for (const k of Object.keys(e.times || {})) if (!keys.includes(k)) keys.push(k);
                if (keys.length === 0) return;
                const known = Object.keys(PERIOD_NAMES);
                const periods = [...known, ...keys.filter((k) => !known.includes(k))];
                const tt = el("table", "de-table");
                tt.style.marginTop = "6px";
                tt.innerHTML = "<tr><th>各時段出現機率</th>" + periods.map((k) => `<th>${PERIOD_NAMES[k] || k}</th>`).join("") + "</tr>";
                const w = (e, k) => (e.times && k in e.times) ? e.times[k] : (e.weight || 0);
                const totals = periods.map((k) => t.list.reduce((s, e) => s + w(e, k), 0));
                for (const e of t.list) {
                    const tr = el("tr");
                    tr.appendChild(el("td", null, (refs.species && refs.species[e.species]) || e.species));
                    periods.forEach((k, j) => {
                        const v = totals[j] ? (w(e, k) / totals[j]) * 100 : 0;
                        const td = el("td", v === 0 ? "de-muted" : null, v === 0 ? "—" : v.toFixed(1) + "%");
                        if (e.times && k in e.times) td.style.color = "var(--accent)";
                        tr.appendChild(td);
                    });
                    tt.appendChild(tr);
                }
                timeBox.appendChild(tt);
                timeBox.appendChild(el("div", "de-muted", "橘色 = 這個時段有特別設定。季節倍數另外再乘。"));
            }
            // 可以省略的一行 JSON 欄位（空白 = 刪掉這個欄位）
            function optJson(e, key) {
                const input = el("input", "de-json");
                input.value = e[key] ? oneLine(e[key]) : "";
                input.placeholder = "—";
                input.addEventListener("change", () => {
                    const v = input.value.trim();
                    if (v === "") { delete e[key]; input.classList.remove("bad"); renderPct(); commit(); return; }
                    try {
                        const o = JSON.parse(v);
                        if (!isObj(o) || !Object.values(o).every((x) => Number.isFinite(x) && x >= 0)) throw new Error("要寫成 {\"night\": 60} 這樣：名稱對到 0 以上的數字");
                        e[key] = o;
                        input.classList.remove("bad");
                        status("");
                        renderPct();
                        commit();
                    } catch (err) {
                        input.classList.add("bad");
                        status(`${key}：${err.message}`, true);
                    }
                });
                return input;
            }
            t.list.forEach((e, i) => {
                const tr = el("tr");
                const cell = (node) => { const td = el("td"); td.appendChild(node); tr.appendChild(td); };
                cell(cellInput(e.species, (v) => { e.species = v; }, { choices: refs.species }));
                cell(cellInput(e.min, (v) => { e.min = v; }));
                cell(cellInput(e.max, (v) => { e.max = v; }));
                cell(cellInput(e.weight, (v) => { e.weight = v; renderPct(); }));
                const p = el("td", "de-muted");
                pctCells[i] = p;
                tr.appendChild(p);
                cell(optJson(e, "times"));
                cell(optJson(e, "seasons"));
                const del = el("button", null, "✕");
                del.addEventListener("click", () => { t.list.splice(i, 1); render(); commit(); });
                const td = el("td"); td.appendChild(del); tr.appendChild(td);
                table.appendChild(tr);
            });
            box.appendChild(table);
            const add = el("button", null, "＋ 加一隻");
            add.addEventListener("click", () => {
                const last = t.list[t.list.length - 1];
                t.list.push(last ? { ...last } : { species: Object.keys(refs.species || { "01": 1 })[0], min: 2, max: 4, weight: 10 });
                render();
                commit();
            });
            box.appendChild(add);
            box.appendChild(timeBox);
            main.appendChild(box);
            renderPct();
        }
    }

    // ---------- 文字 ----------
    function renderText(text) {
        const area = el("textarea", "de-textarea");
        area.value = text;
        area.spellcheck = false;
        area.addEventListener("input", () => {
            try {
                data = JSON.parse(area.value);
                area.classList.remove("bad");
                status("格式正確");
                onChange(area.value.endsWith("\n") ? area.value : area.value + "\n");   // 文字模式照原樣存，不重排
            } catch (e) {
                area.classList.add("bad");
                status("JSON 格式錯誤：" + e.message + "（改好之前不會存檔）", true);
            }
        });
        main.appendChild(area);
    }

    let textMode = "";
    function render() {
        main.innerHTML = "";
        errors = 0;
        const kind = textMode ? "text" : dataKind(data);
        $(".de-kind").textContent = { table: "表格", encounters: "遭遇表", text: "文字（JSON）" }[kind];
        $(".de-add-wrap").hidden = kind !== "table";
        if (kind === "table") renderTable();
        else if (kind === "encounters") renderEncounters();
        else renderText(textMode || JSON.stringify(data, null, 2));
    }
    $(".de-add").addEventListener("click", () => addRow(null));

    return {
        // text：檔案內容；r：參照用的資料（屬性名稱、野伴名稱）
        open(p, text, r) {
            path = p;
            refs = r || {};
            $(".de-title").textContent = p;
            if (p !== root.dataset.path) { sortCol = null; sortDir = 1; }
            root.dataset.path = p;
            try {
                data = JSON.parse(text);
                order = topKeys(text);
                textMode = dataKind(data) === "text" ? text : "";
                status(textMode ? "這個檔案的形狀不適合表格，用文字編輯" : "改完會自動重新執行、存檔");
            } catch (e) {
                data = null;
                textMode = text;
                status("JSON 格式錯誤：" + e.message, true);
            }
            render();
        },
    };
}
