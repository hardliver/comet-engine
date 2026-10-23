// sfxedit.js — Comet Studio 的音效編輯器（Studio 清單第 19 項）
//
// 編輯 comet.json 的 "sfx" 檔（格式見 cart.js 的 buildSfx、core/audio.h）：
//   上半：一個音效的 32 個音符。橫的是時間（第幾個音），直的是音高（越上面越高，橫線是每個 C）。
//         按住拖曳 = 畫音符（用上面選的波形、音量、效果）；右鍵 = 休止（音量 0）
//         下面一列是音量（點一下設定這個音的音量）；再下面一列點一下 = 把這個音換成上面選的波形和效果
//         點在最後一個音的右邊 = 加長；「縮短」刪掉最後一個音
//   下半：音樂。一段一段往下排，每段 4 個聲道各選一個音效（空白 = 不出聲），播完最後一段跳回「循環從第幾段」
// 試聽：叫正在跑的遊戲執行 sfx("名字")／music("名字")（改了之後會自動重新執行，聽到的就是新的）
//
// 和 Studio 的分工：Studio 決定打開哪個檔案、存到哪裡；這個檔案只管「編輯」，改了就呼叫 onChange(新的文字)。

import { SFX_LIMITS, buildSfx } from "./cart.js";

export const WAVES = ["三角", "方波", "窄方", "鋸齒", "雜訊"];
export const FXS = ["—", "滑音", "抖音", "漸弱"];
const WAVE_COLORS = ["#29adff", "#00e436", "#ffec27", "#ff77a8", "#c2c3c7"];
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

// 音高 → 「A4」這種名字（0 = C2）
export function pitchName(p) {
    return NOTE_NAMES[p % 12] + (Math.floor(p / 12) + 2);
}

// ============================================================
// 存檔格式（不碰畫面）：一個音效一行、一首音樂一行，用 git 看差異時一眼就知道改了哪一個
// ============================================================
export function formatSfx(data) {
    const line = (v) => JSON.stringify(v).replace(/,/g, ", ").replace(/:/g, ": ");
    const block = (obj) => {
        const ks = Object.keys(obj || {});
        if (ks.length === 0) return "{}";
        return "{\n" + ks.map((k) => `    ${JSON.stringify(k)}: ${line(obj[k])}`).join(",\n") + "\n  }";
    };
    return `{\n  "sfx": ${block(data.sfx)},\n  "music": ${block(data.music)}\n}\n`;
}

// 新的音效：一個 C4 的方波
export function newSound() {
    return { speed: 8, notes: [[24, 1, 5, 0]] };
}

// ============================================================
// 畫面
// ============================================================
const COL_W = 20, ROW_H = 5;                       // 一個音 20 像素寬、一個半音 5 像素高
const GRID_H = (SFX_LIMITS.pitch + 1) * ROW_H;     // 音高區 320
const VOL_H = 42, INFO_H = 30;
const CANVAS_W = SFX_LIMITS.notes * COL_W, CANVAS_H = GRID_H + VOL_H + INFO_H;

export function createSfxEditor({ root, onChange, onPreview, toast }) {
    root.innerHTML = `
      <div class="se-bar">
        <span class="se-title"></span>
        <select class="se-list" title="這個檔案裡的音效"></select>
        <button class="se-add">＋ 音效</button>
        <button class="se-rename">改名</button>
        <button class="se-del">刪除</button>
        <button class="se-play primary">▶ 試聽</button>
      </div>
      <div class="se-bar">
        <label>速度 <input class="se-speed" type="number" min="1" max="255" title="一個音多長（1/120 秒）"></label>
        <label title="播到結束就跳回開始，一直重複（空白 = 不循環）">循環 <input class="se-ls" type="number" min="0"> ～ <input class="se-le" type="number" min="0"></label>
        <button class="se-trim">縮短</button>
        <span class="se-sep">│ 畫筆：</span>
        <label>波形 <select class="se-wave"></select></label>
        <label>音量 <input class="se-vol" type="number" min="1" max="7"></label>
        <label>效果 <select class="se-fx"></select></label>
      </div>
      <canvas class="se-grid" width="${CANVAS_W}" height="${CANVAS_H}"></canvas>
      <div class="se-status"></div>
      <div class="se-bar se-music-bar">
        <b>音樂</b>
        <select class="se-songs"></select>
        <button class="se-song-add">＋ 音樂</button>
        <button class="se-song-del">刪除</button>
        <button class="se-song-play">▶ 播放</button>
        <button class="se-stop">■ 停止</button>
        <label>循環從第 <input class="se-loop" type="number" min="0"> 段</label>
        <button class="se-pat-add">＋ 一段</button>
      </div>
      <table class="se-pats"></table>`;
    const $ = (sel) => root.querySelector(sel);
    const canvas = $(".se-grid");
    const ctx = canvas.getContext("2d");
    WAVES.forEach((w, i) => $(".se-wave").add(new Option(w, i)));
    FXS.forEach((f, i) => $(".se-fx").add(new Option(f, i)));
    $(".se-wave").value = 1;
    $(".se-vol").value = 5;
    $(".se-fx").value = 0;

    let data = { sfx: {}, music: {} };
    let cur = "";          // 正在編的音效
    let song = "";         // 正在編的音樂
    let broken = false;    // 檔案的 JSON 壞掉了：不准改（不然會把空的資料存回去，蓋掉原本的內容）

    function status(msg, bad = false) {
        $(".se-status").textContent = msg;
        $(".se-status").classList.toggle("bad", bad);
    }

    // 改了：檢查一次（和合併時同一套檢查），通知 Studio
    function changed() {
        if (broken) return;
        try {
            buildSfx(data, "音效檔");
            status("改完會自動重新執行、存檔。按住拖曳畫音符、右鍵是休止");
        } catch (e) {
            status(e.message, true);
        }
        onChange(formatSfx(data));
    }

    const brush = () => [Number($(".se-wave").value), Math.max(1, Math.min(7, Number($(".se-vol").value) || 5)), Number($(".se-fx").value)];
    const snd = () => data.sfx[cur];

    // ---- 畫音符格 ----
    function draw() {
        ctx.fillStyle = "#16181d";
        ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);
        const s = snd();
        if (!s) return;
        const len = s.notes.length;
        // 循環的範圍：淡淡的底色
        if (Array.isArray(s.loop)) {
            ctx.fillStyle = "#1f2a24";
            ctx.fillRect(s.loop[0] * COL_W, 0, (s.loop[1] - s.loop[0] + 1) * COL_W, GRID_H);
        }
        // 每個 C 一條橫線、每個音一條直線；超過長度的欄位比較暗
        for (let p = 0; p <= SFX_LIMITS.pitch; p += 12) {
            const y = GRID_H - (p + 1) * ROW_H;
            ctx.fillStyle = "#2c313a";
            ctx.fillRect(0, y + ROW_H - 1, CANVAS_W, 1);
            ctx.fillStyle = "#6b7280";
            ctx.font = "10px sans-serif";
            ctx.fillText(pitchName(p), 2, y + ROW_H - 3);
        }
        for (let i = 0; i <= SFX_LIMITS.notes; i++) {
            ctx.fillStyle = i % 4 === 0 ? "#2c313a" : "#21252c";
            ctx.fillRect(i * COL_W, 0, 1, CANVAS_H);
        }
        ctx.fillStyle = "rgba(0,0,0,0.45)";
        ctx.fillRect(len * COL_W, 0, CANVAS_W - len * COL_W, CANVAS_H);
        // 音符、音量、波形和效果
        s.notes.forEach(([pitch, wave, vol, fx], i) => {
            const x = i * COL_W;
            if (vol > 0) {
                ctx.fillStyle = WAVE_COLORS[wave];
                ctx.fillRect(x + 2, GRID_H - (pitch + 1) * ROW_H, COL_W - 4, ROW_H);
                ctx.globalAlpha = 0.25;
                ctx.fillRect(x + 2, GRID_H - (pitch + 1) * ROW_H + ROW_H, COL_W - 4, pitch * ROW_H);   // 下面淡淡的柱子，看得出高低
                ctx.globalAlpha = 1;
            }
            ctx.fillStyle = vol > 0 ? WAVE_COLORS[wave] : "#3a3f4b";
            const h = Math.round((VOL_H - 6) * vol / 7);
            ctx.fillRect(x + 4, GRID_H + VOL_H - 3 - h, COL_W - 8, h);
            ctx.fillStyle = "#9aa3b2";
            ctx.font = "10px sans-serif";
            ctx.fillText(vol > 0 ? WAVES[wave].slice(0, 1) : "休", x + 4, GRID_H + VOL_H + 12);
            ctx.fillText(fx > 0 ? FXS[fx].slice(0, 1) : "", x + 4, GRID_H + VOL_H + 25);
        });
        ctx.fillStyle = "#2c313a";
        ctx.fillRect(0, GRID_H, CANVAS_W, 1);
        ctx.fillRect(0, GRID_H + VOL_H, CANVAS_W, 1);
    }

    // ---- 滑鼠 ----
    let dragging = 0;   // 0 沒按、1 左鍵畫、2 右鍵休止
    function at(e) {
        const r = canvas.getBoundingClientRect();
        const x = (e.clientX - r.left) * CANVAS_W / r.width;
        const y = (e.clientY - r.top) * CANVAS_H / r.height;
        return { i: Math.max(0, Math.min(SFX_LIMITS.notes - 1, Math.floor(x / COL_W))), y };
    }
    // 第 i 個音還沒有：中間補休止，讓音效變長
    function ensure(i) {
        const s = snd();
        while (s.notes.length <= i) {
            const last = s.notes[s.notes.length - 1] || [24, 1, 0, 0];
            s.notes.push([last[0], last[1], 0, 0]);
        }
    }
    function apply(e) {
        const s = snd();
        if (!s) return;
        const { i, y } = at(e);
        ensure(i);
        const n = s.notes[i];
        if (y < GRID_H) {
            const pitch = Math.max(0, Math.min(SFX_LIMITS.pitch, Math.floor((GRID_H - y) / ROW_H)));
            if (dragging === 2) s.notes[i] = [n[0], n[1], 0, n[3]];
            else s.notes[i] = [pitch, ...brush()];
        } else if (y < GRID_H + VOL_H) {
            n[2] = Math.max(0, Math.min(7, Math.round((GRID_H + VOL_H - y) * 7 / (VOL_H - 6))));
        } else if (dragging === 1) {
            const b = brush();
            n[1] = b[0];
            n[3] = b[2];
        }
        draw();
    }
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    canvas.addEventListener("pointerdown", (e) => {
        if (!snd()) return;
        dragging = e.button === 2 ? 2 : 1;
        canvas.setPointerCapture(e.pointerId);
        apply(e);
    });
    canvas.addEventListener("pointermove", (e) => { if (dragging) apply(e); });
    canvas.addEventListener("pointerup", () => {
        if (!dragging) return;
        dragging = 0;
        changed();
    });

    // ---- 音效的欄位 ----
    function fillSound() {
        const list = $(".se-list");
        list.innerHTML = "";
        for (const k of Object.keys(data.sfx)) list.add(new Option(k, k));
        if (!(cur in data.sfx)) cur = Object.keys(data.sfx)[0] || "";
        list.value = cur;
        const s = snd();
        for (const sel of [".se-speed", ".se-ls", ".se-le", ".se-trim", ".se-play", ".se-del", ".se-rename"]) $(sel).disabled = !s;
        $(".se-speed").value = s ? s.speed : "";
        $(".se-ls").value = s && s.loop ? s.loop[0] : "";
        $(".se-le").value = s && s.loop ? s.loop[1] : "";
        draw();
    }
    $(".se-list").addEventListener("change", () => { cur = $(".se-list").value; fillSound(); });
    $(".se-speed").addEventListener("change", () => {
        snd().speed = Math.max(1, Math.min(255, Math.trunc(Number($(".se-speed").value)) || 1));
        fillSound();
        changed();
    });
    function setLoop() {
        const a = $(".se-ls").value, b = $(".se-le").value;
        if (a === "" || b === "") delete snd().loop;
        else snd().loop = [Math.trunc(Number(a)), Math.trunc(Number(b))];
        fillSound();
        changed();
    }
    $(".se-ls").addEventListener("change", setLoop);
    $(".se-le").addEventListener("change", setLoop);
    $(".se-trim").addEventListener("click", () => {
        const s = snd();
        if (s.notes.length > 1) s.notes.pop();
        if (s.loop && s.loop[1] >= s.notes.length) delete s.loop;
        fillSound();
        changed();
    });
    function askName(title, now) {
        const name = prompt(title, now || "");
        if (name === null) return null;
        if (!/^[A-Za-z0-9_]{1,23}$/.test(name)) {
            toast("名字只能用英文、數字、底線，最多 23 個字");
            return null;
        }
        return name;
    }
    $(".se-add").addEventListener("click", () => {
        const name = askName("新的音效叫什麼？（英文，例如 hit、jump）");
        if (!name) return;
        if (name in data.sfx) { toast("已經有這個名字了"); return; }
        data.sfx[name] = newSound();
        cur = name;
        fillSound();
        fillMusic();
        changed();
    });
    $(".se-rename").addEventListener("click", () => {
        const name = askName("改成什麼名字？", cur);
        if (!name || name === cur) return;
        if (name in data.sfx) { toast("已經有這個名字了"); return; }
        // 重建物件（保持原本的順序），音樂裡用到的名字一起改
        data.sfx = Object.fromEntries(Object.entries(data.sfx).map(([k, v]) => [k === cur ? name : k, v]));
        for (const m of Object.values(data.music)) m.patterns = m.patterns.map((p) => p.map((x) => (x === cur ? name : x)));
        cur = name;
        fillSound();
        fillMusic();
        changed();
    });
    $(".se-del").addEventListener("click", () => {
        const used = Object.entries(data.music).filter(([, m]) => m.patterns.some((p) => p.includes(cur))).map(([k]) => k);
        if (used.length) { toast(`音樂 ${used.join("、")} 還在用「${cur}」，先從音樂裡拿掉`); return; }
        if (!confirm(`刪除音效「${cur}」？`)) return;
        delete data.sfx[cur];
        cur = "";
        fillSound();
        fillMusic();
        changed();
    });
    $(".se-play").addEventListener("click", () => onPreview(`sfx(${JSON.stringify(cur)})`));

    // ---- 音樂 ----
    function fillMusic() {
        const list = $(".se-songs");
        list.innerHTML = "";
        for (const k of Object.keys(data.music)) list.add(new Option(k, k));
        if (!(song in data.music)) song = Object.keys(data.music)[0] || "";
        list.value = song;
        const m = data.music[song];
        for (const sel of [".se-song-del", ".se-song-play", ".se-loop", ".se-pat-add"]) $(sel).disabled = !m;
        $(".se-loop").value = m && m.loop !== undefined ? m.loop : "";
        const table = $(".se-pats");
        table.innerHTML = "";
        if (!m) return;
        const names = ["", ...Object.keys(data.sfx)];
        const head = table.insertRow();
        ["段", "聲道 0", "聲道 1", "聲道 2", "聲道 3", ""].forEach((t) => { const th = document.createElement("th"); th.textContent = t; head.appendChild(th); });
        m.patterns.forEach((p, i) => {
            const row = table.insertRow();
            row.insertCell().textContent = String(i);
            for (let c = 0; c < SFX_LIMITS.channels; c++) {
                const sel = document.createElement("select");
                for (const n of names) sel.add(new Option(n || "（不出聲）", n));
                sel.value = p[c] || "";
                sel.addEventListener("change", () => {
                    while (p.length < SFX_LIMITS.channels) p.push("");
                    p[c] = sel.value;
                    changed();
                });
                row.insertCell().appendChild(sel);
            }
            const del = document.createElement("button");
            del.textContent = "刪";
            del.disabled = m.patterns.length <= 1;
            del.addEventListener("click", () => {
                m.patterns.splice(i, 1);
                if (m.loop !== undefined && m.loop >= m.patterns.length) m.loop = m.patterns.length - 1;
                fillMusic();
                changed();
            });
            row.insertCell().appendChild(del);
        });
    }
    $(".se-songs").addEventListener("change", () => { song = $(".se-songs").value; fillMusic(); });
    $(".se-song-add").addEventListener("click", () => {
        const name = askName("新的音樂叫什麼？（英文，例如 battle、town）");
        if (!name) return;
        if (name in data.music) { toast("已經有這個名字了"); return; }
        data.music[name] = { patterns: [[cur || "", "", "", ""]], loop: 0 };
        song = name;
        fillMusic();
        changed();
    });
    $(".se-song-del").addEventListener("click", () => {
        if (!confirm(`刪除音樂「${song}」？`)) return;
        delete data.music[song];
        song = "";
        fillMusic();
        changed();
    });
    $(".se-pat-add").addEventListener("click", () => {
        const m = data.music[song];
        m.patterns.push([...(m.patterns[m.patterns.length - 1] || ["", "", "", ""])]);   // 複製上一段，改起來比較快
        fillMusic();
        changed();
    });
    $(".se-loop").addEventListener("change", () => {
        const m = data.music[song];
        const v = $(".se-loop").value;
        if (v === "") delete m.loop;
        else m.loop = Math.max(0, Math.min(m.patterns.length - 1, Math.trunc(Number(v)) || 0));
        fillMusic();
        changed();
    });
    $(".se-song-play").addEventListener("click", () => onPreview(`music(${JSON.stringify(song)})`));
    $(".se-stop").addEventListener("click", () => onPreview("music() sfx(nil)"));

    return {
        open(path, text) {
            $(".se-title").textContent = path;
            try {
                const v = JSON.parse(text);
                data = { sfx: v.sfx || {}, music: v.music || {} };
                broken = false;
                status("按住拖曳畫音符、右鍵是休止。改完會自動重新執行、存檔");
            } catch (e) {
                data = { sfx: {}, music: {} };
                broken = true;
                status("JSON 格式錯誤：" + e.message + "（用其他編輯器修好再打開）", true);
            }
            fillSound();
            fillMusic();
        },
    };
}
