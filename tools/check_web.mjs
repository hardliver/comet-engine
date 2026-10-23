// check_web.mjs — 網頁版的自動測試（用 Node.js 執行，不用開瀏覽器）
//
// 用法：make check
//
// 測試 1：同一串按鍵跑 move_box 90 幀，網頁版的畫面必須和 Mac 版的 out.ppm 逐像素相同
// 測試 2：載入一個會出錯的遊戲，引擎要能接住錯誤、印出訊息，而不是整個當掉
// 測試 3：文字測試（tests/text）跑 90 幀，網頁版和 Mac 版的 build/text_test.ppm 逐像素相同
// 測試 4：畫出來的「野」和 fonts/c11.bin 裡的點陣逐點相同（確認畫得「對」，不只是兩邊「一樣」）
// 測試 5：engine_check 抓得到語法錯誤，而且檢查時正在跑的遊戲不受影響（Studio 打字時用）
// 測試 6：地圖測試（tests/map）跑 90 幀，網頁版和 Mac 版的 build/map_test.ppm 逐像素相同，
//         左下角三個檢查方塊（mget、地圖外、mapw/maph）都是綠色
// 測試 7：重播。Mac 版錄的重播檔，網頁版播放後最後的畫面一致；
//         網頁版用同樣的按鍵錄出來的重播檔，和 Mac 版的逐 byte 相同；
//         竄改過的重播檔會被發現（畫面不同）；不同的遊戲拒絕播放
// 測試 8：遊戲指紋（engine_cart_sha）和 Node.js 內建的 SHA-256 相同
// 測試 9：存檔（tests/save）。檢查方塊全綠；網頁版跑兩次（第二次讀到第一次的存檔），
//         畫面和 Mac 版的 build/save1.ppm、save2.ppm 相同，存檔內容和 Mac 版的檔案逐 byte 相同；
//         網頁版錄的重播檔和 Mac 版相同；播放 Mac 的重播檔時讀到的是重播檔裡的存檔，而且不會寫入
// 測試 10：系統卡匣（tests/system）。照 Mac 版同樣的按鍵：啟動、回 HOME、換一款、HOME 鍵，
//          切換的幀數和最後的畫面和 Mac 版的 build/system.ppm 相同；遊戲裡沒有 sys；
//          沒載入系統卡匣時，Start＋Select 不會觸發回 HOME
// 測試 11：資源包（tests/pak）。檢查方塊全綠；網頁版和 Mac 版的 build/pak_test.ppm 逐像素相同；
//          畫面上的點對（被背景擋住、沒被擋住、放大、翻轉、染色）；
//          遊戲指紋 = SHA-256(遊戲檔指紋 + 資源包內容指紋)
// 測試 12：資料檔（tests/data）。comet.json 的 data 合併成 DATA 表格：檢查方塊全綠、
//          和 Mac 版的 build/data_test.ppm 逐像素相同；小數、null、壞掉的 JSON 會清楚地報錯
// 測試 13：資料表編輯器（dataedit.js）的存檔格式。重新排版後內容不變、排兩次結果一樣、
//          編號的順序照檔案（"10" 不會跑到 "01" 前面）；依檔案形狀選對畫面（表格、遭遇表、文字）；
//          格子的輸入檢查（整數不能有小數、JSON 格式錯誤）
// 測試 17：現實時鐘（tests/clock）。和 Mac 版 --clock 2026-12-31T23:59:59 同樣的時間：檢查方塊全綠、
//          畫面和 build/clock1.ppm 相同、錄的重播檔和 Mac 版逐 byte 相同；
//          沒有時間來源時播放 Mac 的重播檔，讀到的是重播檔裡的時間；沒有時間來源時 clock() 是 nil
// 測試 18：聲音（tests/sound）。同樣跑 480 幀，網頁版每一幀的聲音取樣接起來，和 Mac 版的 build/sound.wav 逐 byte 相同；
//          檢查方塊全綠、畫面和 build/sound.ppm 相同

import fs from "fs";
import crypto from "crypto";
import { buildData } from "../platform/web/cart.js";
import { formatData, dataKind, parseCell, topKeys } from "../platform/web/dataedit.js";

const WASM = "build/web/comet.wasm";
const W = 320, H = 240;
const BTN_RIGHT = 1, BTN_DOWN = 3, BTN_A = 4;

let failures = 0;
const logs = [];

// store：存檔放在哪裡（Map：「id:欄位」→ bytes）。不給就每次都是空的
// clock：第 0 幀的本地時間（秒），和 Mac 版的 --clock 一樣每 60 幀走 1 秒；null = 沒有時間來源
async function loadEngine(store = new Map(), clock = null) {
    let engine = null;
    const cstr = (ptr) => {
        const mem = new Uint8Array(engine.memory.buffer);
        let end = ptr;
        while (mem[end] !== 0) end++;
        return new TextDecoder().decode(mem.subarray(ptr, end));
    };
    const env = new Proxy({
        js_log(ptr) {
            logs.push(cstr(ptr));
        },
        js_save_read(idPtr, slot, buf, cap) {
            const bytes = store.get(`${cstr(idPtr)}:${slot}`);
            if (!bytes || bytes.length > cap) return -1;
            new Uint8Array(engine.memory.buffer).set(bytes, buf);
            return bytes.length;
        },
        js_save_write(idPtr, slot, data, len) {
            const key = `${cstr(idPtr)}:${slot}`;
            if (data === 0) store.delete(key);
            else store.set(key, new Uint8Array(engine.memory.buffer, data, len).slice());
            return 0;
        },
        js_clock() {
            return clock === null ? -1 : clock + Math.floor(engine.engine_frame_count() / 60);
        },
    }, { get: (t, name) => t[name] || (() => 0) });
    const wasi = new Proxy({}, { get: () => () => 8 });   // 8 = EBADF
    const { instance } = await WebAssembly.instantiate(fs.readFileSync(WASM), { env, wasi_snapshot_preview1: wasi });
    engine = instance.exports;
    if (engine._initialize) engine._initialize();
    engine.engine_init();
    return engine;
}

function load(engine, source) {
    const bytes = new TextEncoder().encode(source);
    const ptr = engine.malloc(bytes.length);
    new Uint8Array(engine.memory.buffer).set(bytes, ptr);
    const err = engine.engine_load(ptr, bytes.length);
    engine.free(ptr);
    return err;
}

function framebufferRGB(engine) {
    const mem = new Uint8Array(engine.memory.buffer);
    const fb = engine.gfx_framebuffer(), pal = engine.gfx_palette();
    const rgb = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H; i++) {
        const c = mem[fb + i];
        for (let k = 0; k < 3; k++) rgb[i * 3 + k] = mem[pal + c * 3 + k];
    }
    return rgb;
}

// 和 platform/mac/main.c 的 scripted_input() 完全相同的輸入
function scriptedInput(engine, i) {
    let keys = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    if (i < 30) keys = 1 << BTN_RIGHT;
    else if (i < 60) keys = 1 << BTN_DOWN;
    else { keys = 1 << BTN_A; lx = 50; ly = -90; rx = -127; }
    engine.input_set(0, keys);
    engine.input_set_stick(0, 0, lx, ly);
    engine.input_set_stick(0, 1, rx, ry);
}

// 把 bytes 交給 replay_play_begin
function playBegin(engine, bytes) {
    const ptr = engine.malloc(bytes.length);
    new Uint8Array(engine.memory.buffer).set(bytes, ptr);
    const err = engine.replay_play_begin(ptr, bytes.length);
    engine.free(ptr);
    return err;
}

// 一直跑到播完（最多 10 萬幀，避免壞掉時跑不停）。回傳 replay_state
function runReplay(engine) {
    for (let n = 0; engine.replay_state() === 2 && n < 100000; n++) engine.engine_frame();
    return engine.replay_state();
}

function ok(name)            { console.log(`✓ ${name}`); }
function fail(name, detail)  { console.log(`✗ ${name}\n  ${detail}`); failures++; }

// ---- 測試 1 ----
{
    const name = "網頁版與 Mac 版輸出逐像素相同（move_box，90 幀）";
    try {
        const engine = await loadEngine();
        load(engine, fs.readFileSync("games/move_box.lua", "utf8"));
        // 和 platform/mac/main.c 完全相同的輸入
        for (let i = 0; i < 90; i++) {
            let keys = 0, lx = 0, ly = 0, rx = 0, ry = 0;
            if (i < 30) keys = 1 << BTN_RIGHT;
            else if (i < 60) keys = 1 << BTN_DOWN;
            else { keys = 1 << BTN_A; lx = 50; ly = -90; rx = -127; }
            engine.input_set(0, keys);
            engine.input_set_stick(0, 0, lx, ly);
            engine.input_set_stick(0, 1, rx, ry);
            engine.engine_frame();
        }
        const ppm = fs.readFileSync("out.ppm");
        const mac = ppm.subarray(ppm.length - W * H * 3);
        if (Buffer.compare(framebufferRGB(engine), mac) === 0) ok(name);
        else fail(name, "畫面不同。先執行 ./build/mac 產生 out.ppm 再測");
    } catch (e) {
        fail(name, String(e));
    }
}

// ---- 測試 2 ----
{
    const name = "Lua 執行錯誤會被接住，不會讓引擎當掉";
    try {
        const engine = await loadEngine();
        logs.length = 0;
        load(engine, "function _update() rect(1.5, 0, 4, 4, 7) end");
        engine.engine_frame();   // 這一幀遊戲會出錯
        engine.engine_frame();   // 出錯後引擎要繼續跑（畫面變紅）
        if (logs.some((m) => m.includes("no integer representation"))) ok(name);
        else fail(name, `沒有收到預期的錯誤訊息，收到：${JSON.stringify(logs)}`);
    } catch (e) {
        fail(name, `錯誤沒有被接住，直接丟到了 JavaScript：${e}\n` +
                   "  通常是 emscripten 版本太舊，不支援 -sSUPPORT_LONGJMP=wasm");
    }
}

// ---- 測試 3、4 ----
{
    const engine = await loadEngine();
    load(engine, fs.readFileSync("build/text_test.lua", "utf8"));
    for (let i = 0; i < 90; i++) engine.engine_frame();

    const name3 = "網頁版與 Mac 版的中文字逐像素相同（tests/text，90 幀）";
    const ppm = fs.readFileSync("build/text_test.ppm");
    const mac = ppm.subarray(ppm.length - W * H * 3);
    if (Buffer.compare(framebufferRGB(engine), mac) === 0) ok(name3);
    else fail(name3, "畫面不同");

    // tests/text/main.lua 把「野」畫在 (10, 10)，顏色 7，背景 1
    const name4 = "「野」的點陣和 c11.bin 相同";
    const font = fs.readFileSync("fonts/c11.bin");
    const rows = font[5], size = 5 + rows * 2;
    let glyph = null;
    for (let at = 8; at < font.length; at += size) {
        if (font.readUInt32LE(at) === 0x91ce) { glyph = font.subarray(at, at + size); break; }
    }
    const mem = new Uint8Array(engine.memory.buffer);
    const fb = engine.gfx_framebuffer();
    let wrong = 0;
    for (let r = 0; r < rows; r++) {
        const bits = glyph.readUInt16BE(5 + r * 2);
        for (let c = 0; c < glyph[4]; c++) {
            const want = bits & (0x8000 >> c) ? 7 : 1;
            if (mem[fb + (10 + r) * W + 10 + c] !== want) wrong++;
        }
    }
    if (glyph && wrong === 0) ok(name4);
    else fail(name4, glyph ? `${wrong} 個點不對` : "c11.bin 裡找不到「野」");
}

// ---- 測試 5 ----
{
    const name = "只檢查語法：抓得到錯誤，正在跑的遊戲不受影響";
    const engine = await loadEngine();
    load(engine, "local n = 0\nfunction _update() n = n + 1 end\nfunction _draw() cls(n % 16) end\n");
    for (let i = 0; i < 5; i++) engine.engine_frame();
    const check = (src) => {
        const bytes = new TextEncoder().encode(src);
        const ptr = engine.malloc(bytes.length);
        new Uint8Array(engine.memory.buffer).set(bytes, ptr);
        const bad = engine.engine_check(ptr, bytes.length);
        engine.free(ptr);
        if (!bad) return null;
        const mem = new Uint8Array(engine.memory.buffer);
        let p = engine.engine_check_error(), end = p;
        while (mem[end]) end++;
        return new TextDecoder().decode(mem.subarray(p, end));
    };
    const bad = check("function _update()\n  local x = 1\n");
    const good = check("function _draw() cls(3) end\n");
    for (let i = 0; i < 5; i++) engine.engine_frame();
    const color = new Uint8Array(engine.memory.buffer)[engine.gfx_framebuffer()];
    if (bad && bad.includes("'end' expected") && good === null && color === 10) ok(name);
    else fail(name, `錯誤訊息：${bad}；正確程式的結果：${good}；畫面顏色 ${color}（應該是 10）`);
}

// ---- 測試 6 ----
{
    const name = "地圖：網頁版與 Mac 版逐像素相同，mget／mapw／maph 正確（tests/map，90 幀）";
    const engine = await loadEngine();
    logs.length = 0;
    load(engine, fs.readFileSync("build/map_test.lua", "utf8"));
    for (let i = 0; i < 90; i++) engine.engine_frame();
    const web = framebufferRGB(engine);
    const ppm = fs.readFileSync("build/map_test.ppm");
    const mac = ppm.subarray(ppm.length - W * H * 3);
    const green = [4, 12, 20].every((x) => {
        const i = (236 * W + x) * 3;
        return web[i] === 0 && web[i + 1] === 228 && web[i + 2] === 54;
    });
    if (Buffer.compare(web, mac) === 0 && green) ok(name);
    else fail(name, Buffer.compare(web, mac) !== 0 ? "畫面不同" : `檢查方塊不是綠色${logs.length ? "：" + logs.join(" ") : ""}`);
}

// ---- 測試 7 ----
{
    const name = "重播：跨平台播放一致、錄出來的檔案相同、竄改會被發現、不同遊戲拒絕播放";
    const src = fs.readFileSync("games/move_box.lua", "utf8");
    const macRep = fs.readFileSync("build/move_box.rep");
    const problems = [];

    // (1) 播放 Mac 錄的檔案
    {
        const engine = await loadEngine();
        if (playBegin(engine, macRep) !== 0) problems.push("Mac 的重播檔讀不進來");
        load(engine, src);
        const st = runReplay(engine);
        if (st !== 3 || engine.replay_matched() !== 1) problems.push(`播放結果：狀態 ${st}、一致 ${engine.replay_matched()}`);
        const ppm = fs.readFileSync("out.ppm");
        if (Buffer.compare(framebufferRGB(engine), ppm.subarray(ppm.length - W * H * 3)) !== 0) {
            problems.push("播放後的畫面和 out.ppm 不同");
        }
    }
    // (2) 網頁版自己錄
    {
        const engine = await loadEngine();
        engine.replay_record_begin();
        load(engine, src);
        for (let i = 0; i < 90; i++) { scriptedInput(engine, i); engine.engine_frame(); }
        const ptr = engine.replay_snapshot();
        const bytes = Buffer.from(new Uint8Array(engine.memory.buffer, ptr, engine.replay_snapshot_size()));
        if (Buffer.compare(bytes, macRep) !== 0) problems.push(`網頁版錄的檔案（${bytes.length} bytes）和 Mac 版（${macRep.length} bytes）不同`);
    }
    // (3) 竄改：第一個事件的「右」改成「下」
    {
        const bad = Buffer.from(macRep);
        bad[114] = 1 << BTN_DOWN;
        const engine = await loadEngine();
        playBegin(engine, bad);
        load(engine, src);
        if (runReplay(engine) !== 3 || engine.replay_matched() !== 0) problems.push("竄改過的重播檔沒有被發現");
    }
    // (4) 不同的遊戲
    {
        const engine = await loadEngine();
        playBegin(engine, macRep);
        if (load(engine, src + "\n-- 改了一行\n") !== 2) problems.push("遊戲改過了，卻沒有拒絕播放");
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 8 ----
{
    const name = "遊戲指紋（SHA-256）和 Node.js 算的相同";
    const src = fs.readFileSync("games/move_box.lua");
    const engine = await loadEngine();
    load(engine, src.toString("utf8"));
    const ours = Buffer.from(new Uint8Array(engine.memory.buffer, engine.engine_cart_sha(), 32)).toString("hex");
    const node = crypto.createHash("sha256").update(src).digest("hex");
    if (ours === node) ok(name);
    else fail(name, `引擎 ${ours}，Node.js ${node}`);
}

// ---- 測試 9 ----
{
    const name = "存檔：存讀正確、跨平台相同、重播時從重播檔讀且不寫入";
    const src = fs.readFileSync("tests/save/main.lua", "utf8");
    const problems = [];
    const macPPM = (f) => { const p = fs.readFileSync(f); return p.subarray(p.length - W * H * 3); };
    const run = (engine) => { for (let i = 0; i < 90; i++) { scriptedInput(engine, i); engine.engine_frame(); } };
    // 下方 12 個檢查方塊都是綠色、上方有 count 個黃色方塊
    const looksRight = (rgb, count) => {
        const px = (x, y) => { const i = (y * W + x) * 3; return [rgb[i], rgb[i + 1], rgb[i + 2]].join(","); };
        for (let i = 0; i < 12; i++) if (px(4 + i * 8, 230) !== "0,228,54") return false;
        for (let i = 0; i < count; i++) if (px(10 + i * 12, 10) !== px(10, 10) || px(10, 10) === px(0, 0)) return false;
        return px(10 + count * 12, 10) === px(0, 0);
    };
    if (!looksRight(macPPM("build/save1.ppm"), 1)) problems.push("Mac 版第一次執行的檢查方塊不是全綠");

    const store = new Map();
    // (1) 第一次執行：沒有存檔
    {
        const engine = await loadEngine(store);
        load(engine, src);
        run(engine);
        if (Buffer.compare(framebufferRGB(engine), macPPM("build/save1.ppm")) !== 0) problems.push("第一次執行的畫面和 Mac 版不同");
    }
    // (2) 第二次執行：讀到第一次的存檔（count = 2），同時錄影
    {
        const engine = await loadEngine(store);
        engine.replay_record_begin();
        load(engine, src);
        run(engine);
        const web = framebufferRGB(engine);
        if (Buffer.compare(web, macPPM("build/save2.ppm")) !== 0) problems.push("第二次執行的畫面和 Mac 版不同");
        if (!looksRight(web, 2)) problems.push("第二次執行沒有讀到第一次的存檔");
        for (const slot of [1, 2]) {
            const mac = fs.readFileSync(`build/saves/comet_save-test.${slot}.sav`);
            const ours = store.get(`comet/save-test:${slot}`);
            if (!ours || Buffer.compare(Buffer.from(ours), mac) !== 0) problems.push(`欄位 ${slot} 的存檔和 Mac 版不同`);
        }
        if (store.has("comet/save-test:3") || store.has("comet/save-test:4")) problems.push("刪除或出錯的欄位還留著");
        const ptr = engine.replay_snapshot();
        const rep = Buffer.from(new Uint8Array(engine.memory.buffer, ptr, engine.replay_snapshot_size()));
        if (Buffer.compare(rep, fs.readFileSync("build/save.rep")) !== 0) problems.push("網頁版錄的重播檔和 Mac 版不同");
    }
    // (3) 播放 Mac 的重播檔，存檔是空的：要讀到重播檔裡的存檔（畫面 count = 2），而且不寫入
    {
        const empty = new Map();
        const engine = await loadEngine(empty);
        playBegin(engine, fs.readFileSync("build/save.rep"));
        load(engine, src);
        const st = runReplay(engine);
        if (st !== 3 || engine.replay_matched() !== 1) problems.push(`播放結果：狀態 ${st}、一致 ${engine.replay_matched()}`);
        if (!looksRight(framebufferRGB(engine), 2)) problems.push("播放時沒有讀到重播檔裡的存檔");
        if (empty.size !== 0) problems.push("播放重播時寫入了存檔");
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 10 ----
{
    const name = "系統卡匣：啟動遊戲、回 HOME、遊戲碰不到 sys，和 Mac 版相同";
    const problems = [];
    const cstr = (engine, text) => {
        const b = new TextEncoder().encode(text);
        const p = engine.malloc(b.length + 1);
        const mem = new Uint8Array(engine.memory.buffer);
        mem.set(b, p);
        mem[p + b.length] = 0;
        return p;
    };
    const loadWith = (engine, fn, source) => {
        const bytes = new TextEncoder().encode(source);
        const ptr = engine.malloc(bytes.length);
        new Uint8Array(engine.memory.buffer).set(bytes, ptr);
        const err = engine[fn](ptr, bytes.length);
        engine.free(ptr);
        return err;
    };
    const px = (engine, x, y) => {
        const rgb = framebufferRGB(engine), i = (y * W + x) * 3;
        return [rgb[i], rgb[i + 1], rgb[i + 2]].join(",");
    };
    const GREEN = "0,228,54";
    const files = { "test/a": "tests/system/game_a.lua", "test/b": "tests/system/game_b.lua" };

    // 和 platform/mac/main.c 的 system_input() 完全相同
    const keysAt = (i) =>
        i >= 10 && i < 14 ? 1 << BTN_A :
        i >= 14 && i < 50 ? 1 << BTN_RIGHT :
        i >= 50 && i < 85 ? (1 << 6) | (1 << 7) :
        i >= 100 && i < 104 ? 1 << BTN_DOWN :
        i >= 110 && i < 114 ? 1 << BTN_A :
        i === 130 ? 1 << 12 : 0;

    const engine = await loadEngine();
    const sysSrc = fs.readFileSync("tests/system/system.lua", "utf8");
    for (const [id, file] of Object.entries(files)) {
        const a = cstr(engine, id), b = cstr(engine, file);
        engine.system_add_game(a, b);
        engine.system_set_info(a, a, Object.keys(files).indexOf(id) + 1);   // 和 Mac 版一樣
        engine.free(a); engine.free(b);
    }
    loadWith(engine, "engine_load_system", sysSrc);
    const events = [];
    for (let i = 0; i < 150; i++) {
        engine.input_set(0, keysAt(i));
        engine.input_set_stick(0, 0, 0, 0);
        engine.input_set_stick(0, 1, 0, 0);
        engine.engine_frame();
        if (i === 40 && px(engine, 6, 232) !== GREEN) problems.push("遊戲裡看得到 sys");
        const req = engine.system_request();
        if (req === 1) {
            const mem = new Uint8Array(engine.memory.buffer);
            let p = engine.system_request_game(), end = p;
            while (mem[end]) end++;
            const id = new TextDecoder().decode(mem.subarray(p, end));
            engine.system_request_done();
            events.push(`${i} 啟動 ${id}`);
            loadWith(engine, "engine_load", fs.readFileSync(files[id], "utf8"));
        } else if (req === 2) {
            engine.system_request_done();
            events.push(`${i} HOME`);
            loadWith(engine, "engine_load_system", sysSrc);
        }
    }
    const want = "10 啟動 test/a,79 HOME,110 啟動 test/b,130 HOME";
    if (events.join(",") !== want) problems.push(`切換順序：${events.join(",")}（應該是 ${want}）`);
    const ppm = fs.readFileSync("build/system.ppm");
    if (Buffer.compare(framebufferRGB(engine), ppm.subarray(ppm.length - W * H * 3)) !== 0) problems.push("最後的畫面和 Mac 版不同");
    if ([6, 14, 22].some((x) => px(engine, x, 232) !== GREEN)) problems.push("系統卡匣的檢查方塊不是綠色");
    if (px(engine, 25, 85) !== GREEN) problems.push("回 HOME 後，系統卡匣不知道剛剛玩的是 test/b");

    // 沒載入系統卡匣：Start＋Select 照常交給遊戲
    {
        const e2 = await loadEngine();
        loadWith(e2, "engine_load", fs.readFileSync(files["test/a"], "utf8"));
        for (let i = 0; i < 60; i++) { e2.input_set(0, (1 << 6) | (1 << 7)); e2.engine_frame(); }
        if (e2.system_request() !== 0) problems.push("單獨玩遊戲時，Start＋Select 也觸發了回 HOME");
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 11 ----
{
    const name = "資源包：背景、深度遮擋、角色、染色，網頁版與 Mac 版相同";
    const problems = [];
    try {
        const engine = await loadEngine();
        // 大遊戲的資源包有幾十 MB：wasm 的記憶體要能長大（Makefile 的 ALLOW_MEMORY_GROWTH）
        const big = engine.malloc(40 * 1024 * 1024);
        if (!big) problems.push("配置 40 MB 失敗：wasm 的記憶體不會長大");
        else engine.free(big);
        const pak = fs.readFileSync("build/pak_test.pak");
        // 資源包的記憶體由平台保管，遊戲執行期間不能 free
        const pp = engine.malloc(pak.length);
        new Uint8Array(engine.memory.buffer).set(pak, pp);
        if (engine.pak_set(pp, pak.length) !== 0) problems.push("pak_set 失敗");
        const src = fs.readFileSync("tests/pak/main.lua", "utf8");
        load(engine, src);
        for (let i = 0; i < 90; i++) { scriptedInput(engine, i); engine.engine_frame(); }
        const rgb = framebufferRGB(engine);
        const ppm = fs.readFileSync("build/pak_test.ppm");
        if (Buffer.compare(rgb, ppm.subarray(ppm.length - W * H * 3)) !== 0) problems.push("畫面和 Mac 版不同");
        const at = (x, y) => [...rgb.subarray((y * W + x) * 3, (y * W + x) * 3 + 3)].join(",");
        const want = {
            "80,96": "40,50,60",       // 被左半邊的遮擋物擋住：看到背景
            "90,96": "120,100,50",     // 在遮擋物前面：第 16 色染成一半亮
            "196,84": "120,100,50",    // 放大兩倍的左上角
            "203,99": "120,100,50",    // 放大兩倍的右下角
            "195,90": "70,80,90",      // 放大兩倍的範圍外
            "249,149": "0,250,250",    // 翻轉
            "248,149": "100,110,120",
            "260,149": "0,250,250",    // 不翻轉
            "261,149": "100,110,120",
        };
        for (const [k, v] of Object.entries(want)) {
            const [x, y] = k.split(",").map(Number);
            if (at(x, y) !== v) problems.push(`(${k}) 是 ${at(x, y)}，應該是 ${v}`);
        }
        for (let i = 0; i < 9; i++) if (at(6 + i * 8, 232) !== "0,228,54") problems.push(`第 ${i + 1} 個檢查方塊不是綠色`);
        const inner = crypto.createHash("sha256").update(Buffer.from(src)).digest();
        const expect = crypto.createHash("sha256").update(inner).update(pak.subarray(12, 44)).digest();
        const got = Buffer.from(new Uint8Array(engine.memory.buffer, engine.engine_cart_sha(), 32));
        if (Buffer.compare(expect, got) !== 0) problems.push("遊戲指紋沒有包含資源包");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 12 ----
{
    const name = "資料檔：JSON 合併成 DATA 表格，網頁版與 Mac 版相同，錯誤訊息清楚";
    const problems = [];
    try {
        const engine = await loadEngine();
        load(engine, fs.readFileSync("build/data_test.lua", "utf8"));
        for (let i = 0; i < 90; i++) engine.engine_frame();
        const rgb = framebufferRGB(engine);
        const ppm = fs.readFileSync("build/data_test.ppm");
        if (Buffer.compare(rgb, ppm.subarray(ppm.length - W * H * 3)) !== 0) problems.push("畫面和 Mac 版不同");
        for (let i = 0; i < 7; i++) {
            const p = ((232 * W) + 6 + i * 8) * 3;
            if ([rgb[p], rgb[p + 1], rgb[p + 2]].join(",") !== "0,228,54") problems.push(`第 ${i + 1} 個檢查方塊不是綠色`);
        }
        // 錯誤訊息：要指出哪個檔案、哪個位置
        const bad = async (text) => {
            try {
                await buildData({ data: { x: "x.json" } }, async () => text);
                return "沒有報錯";
            } catch (e) { return e.message; }
        };
        const m1 = await bad('[{ "hp": 1.5 }]');
        if (!m1.includes("x.json") || !m1.includes("[0].hp") || !m1.includes("小數")) problems.push(`小數的錯誤訊息：${m1}`);
        const m2 = await bad('{ "a": null }');
        if (!m2.includes("的 a 是 null")) problems.push(`null 的錯誤訊息：${m2}`);
        const m3 = await bad('{ "a": 1, }');
        if (!m3.includes("x.json")) problems.push(`壞掉的 JSON 的錯誤訊息：${m3}`);
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 13 ----
{
    const name = "資料表編輯器：存檔格式、順序、畫面種類、輸入檢查";
    const problems = [];
    try {
        const species = '{\n  "01": { "name": "芽", "base": { "hp": 45 }, "learn": [ [1, "a"] ] },\n' +
                        '  "02": { "name": "林", "base": { "hp": 75 }, "learn": [] },\n  "10": { "name": "燈", "base": { "hp": 40 }, "learn": [] }\n}\n';
        const d = JSON.parse(species);
        const out = formatData(d, topKeys(species));
        if (JSON.stringify(JSON.parse(out)) !== JSON.stringify(d)) problems.push("重新排版後內容變了");
        if (formatData(JSON.parse(out), topKeys(out)) !== out) problems.push("排兩次結果不一樣");
        if (topKeys(out).join(",") !== "01,02,10") problems.push(`順序亂了：${topKeys(out).join(",")}`);
        if (out.split("\n").length !== 6) problems.push("不是一筆一行");
        const enc = { route1: { rate: 20, list: [{ species: "07", min: 2, max: 4, weight: 50 }] } };
        if (dataKind(d) !== "table") problems.push("野伴應該用表格");
        if (dataKind(enc) !== "encounters") problems.push("遭遇表應該用遭遇表畫面");
        if (dataKind({ names: { a: "甲" }, chart: { a: { b: 1 }, b: { a: 2 } } }) !== "text") problems.push("相剋表應該用文字");
        if (dataKind({ lumber: "伐木" }) !== "text") problems.push("名稱對照表應該用文字");
        if (!formatData(enc).includes('\n      { "species": "07"')) problems.push("遭遇表的 list 要一筆一行");
        if (parseCell("12", 5).value !== 12) problems.push("整數");
        if (parseCell("1.5", 5).ok) problems.push("小數應該被擋");
        if (parseCell("[1,", [1]).ok) problems.push("壞掉的 JSON 應該被擋");
        if (parseCell("[1]", { a: 1 }).ok) problems.push("型別不對（陣列填在表格欄）應該被擋");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 14 ----
{
    const name = "除錯指令（Studio 的狀態分頁）：在執行中的遊戲裡跑 Lua、讀 DEBUG_VIEW";
    const problems = [];
    try {
        const engine = await loadEngine();
        const src = "BAG = { wood = 3 }\nDEBUG_VIEW = { 'BAG', 'nope.x' }\nfunction _update() end\n";
        load(engine, src);
        const ev = (code, mode) => {
            const bytes = new TextEncoder().encode(code);
            const ptr = engine.malloc(bytes.length + 1);
            new Uint8Array(engine.memory.buffer).set(bytes, ptr);
            const err = engine.engine_eval(ptr, bytes.length, mode);
            engine.free(ptr);
            const mem = new Uint8Array(engine.memory.buffer);
            let p = engine.engine_eval_result(), end = p;
            while (mem[end] !== 0) end++;
            return { ok: !err, text: new TextDecoder().decode(mem.subarray(p, end)) };
        };
        let r = ev("BAG.wood", 0);
        if (!r.ok || r.text !== "3") problems.push(`算式：${r.text}`);
        r = ev("BAG.wood = 99", 0);
        if (!r.ok || r.text !== "（完成）") problems.push(`敘述：${r.text}`);
        if (ev("BAG.wood", 2).text !== "99") problems.push("改了沒有生效");
        r = ev("error('boom')", 0);
        if (r.ok || !r.text.includes("boom")) problems.push(`錯誤：${r.text}`);
        r = ev("", 1);
        const lines = r.text.split("\n");
        if (lines[0] !== "0\tBAG\tBAG\t{1}" || lines[1] !== "1\tBAG.wood\twood\t99") problems.push(`狀態樹：${JSON.stringify(lines.slice(0, 2))}`);
        if (!lines[2] || !lines[2].includes("錯誤")) problems.push("DEBUG_VIEW 裡算錯的那一項要顯示錯誤，不能整個失敗");
        engine.engine_frame();   // 指令出錯不能讓遊戲停掉
        if (logs.some((l) => l.includes("boom"))) problems.push("指令的錯誤不該變成遊戲的錯誤");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 15 ----
{
    const name = "運算預算：Lua 指令數、像素、繪圖呼叫、預估負載";
    const problems = [];
    try {
        const engine = await loadEngine();
        engine.engine_profile(1);
        // 每幀：cls（76,800 像素）＋ 一個 10×10 的 rect（100 像素）＋ 1000 次空迴圈
        load(engine, "function _update() for i = 1, 1000 do end end\nfunction _draw() cls(1) rect(5, 5, 10, 10, 8) end\n");
        engine.engine_frame();
        const instr = engine.engine_prof_instr(), pixels = engine.engine_prof_pixels(), calls = engine.engine_prof_calls();
        if (pixels !== 76800 + 100) problems.push(`像素應該是 76900，得到 ${pixels}`);
        if (calls !== 2) problems.push(`繪圖呼叫應該是 2，得到 ${calls}`);
        if (instr < 1000 || instr > 10000) problems.push(`Lua 指令數不合理：${instr}`);
        const pct = engine.engine_prof_load();
        const expect = Math.floor((instr * 40 + pixels * 8 + calls * 300) * 1000 / 2500000);
        if (pct !== expect) problems.push(`負載算錯：${pct}，應該是 ${expect}`);
        engine.engine_profile(0);
        engine.engine_frame();
        if (engine.engine_prof_instr() !== 0) problems.push("關掉之後不該再算 Lua 指令");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 16 ----
{
    const name = "繪圖用的顏色替換 dpal、讀調色盤 palget、圖片用到的顏色 imgcolors";
    const problems = [];
    try {
        const engine = await loadEngine();
        load(engine, "function _draw() cls(0) dpal(8, 11) rect(0,0,4,4,8) dpal() rect(4,0,4,4,8) " +
                     "dpal({[8]=12,[7]=2}) rect(8,0,4,4,8) pset(12,0,7) dpal() local r,g,b = palget(8) print(r..','..g..','..b) end\n");
        engine.engine_frame();
        const mem = new Uint8Array(engine.memory.buffer);
        const fb = engine.gfx_framebuffer();
        const got = [mem[fb], mem[fb + 4], mem[fb + 8], mem[fb + 12]].join(",");
        if (got !== "11,8,12,2") problems.push(`畫出來的顏色 ${got}，應該是 11,8,12,2`);
        if (!logs.includes("255,0,77")) problems.push("palget(8) 應該是 255,0,77（PICO-8 的紅）");
        // imgcolors：2×2 的圖，用了 0（透明）、16、17
        load(engine, "local img = newimage(2, 2, \"EAAREA==\") print(table.concat(imgcolors(img), ',')) print(#imgcolors(img, 1, 0, 1, 1))\n");
        if (!logs.includes("16,17")) problems.push("imgcolors 應該是 16,17");
        if (!logs.includes("0")) problems.push("imgcolors 只看透明的那一格應該是空的");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 17 ----
{
    const name = "現實時鐘：和 Mac 版相同、記進重播檔、播放時讀重播檔裡的時間";
    const src = fs.readFileSync("tests/clock/main.lua", "utf8");
    const problems = [];
    const macPPM = (f) => { const p = fs.readFileSync(f); return p.subarray(p.length - W * H * 3); };
    const run = (engine) => { for (let i = 0; i < 90; i++) { scriptedInput(engine, i); engine.engine_frame(); } };
    // 下方 8 個檢查方塊都是綠色（有時間來源）
    const allGreen = (rgb, n) => {
        for (let i = 0; i < n; i++) {
            const k = (230 * W + 4 + i * 8) * 3;
            if ([rgb[k], rgb[k + 1], rgb[k + 2]].join(",") !== "0,228,54") return false;
        }
        return true;
    };
    try {
        if (!allGreen(macPPM("build/clock1.ppm"), 8)) problems.push("Mac 版的檢查方塊不是全綠");
        const start = Date.UTC(2026, 11, 31, 23, 59, 59) / 1000;   // 本地時間的秒數：當成 UTC 算就對了（不加時區）
        {
            const engine = await loadEngine(new Map(), start);
            engine.replay_record_begin();
            load(engine, src);
            run(engine);
            if (Buffer.compare(framebufferRGB(engine), macPPM("build/clock1.ppm")) !== 0) problems.push("有時間來源的畫面和 Mac 版不同");
            const ptr = engine.replay_snapshot();
            const rep = Buffer.from(new Uint8Array(engine.memory.buffer, ptr, engine.replay_snapshot_size()));
            if (Buffer.compare(rep, fs.readFileSync("build/clock.rep")) !== 0) problems.push("網頁版錄的重播檔和 Mac 版不同");
        }
        {
            const engine = await loadEngine(new Map(), null);   // 沒有時間來源：時間只能從重播檔來
            playBegin(engine, fs.readFileSync("build/clock.rep"));
            load(engine, src);
            const st = runReplay(engine);
            if (st !== 3 || engine.replay_matched() !== 1) problems.push(`播放結果：狀態 ${st}、一致 ${engine.replay_matched()}`);
            if (!allGreen(framebufferRGB(engine), 8)) problems.push("播放時沒有讀到重播檔裡的時間");
        }
        {
            const engine = await loadEngine(new Map(), null);
            load(engine, src);
            run(engine);
            if (Buffer.compare(framebufferRGB(engine), macPPM("build/clock3.ppm")) !== 0) problems.push("沒有時間來源的畫面和 Mac 版不同");
        }
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

// ---- 測試 18 ----
{
    const name = "聲音：網頁版和 Mac 版合成出來的聲音逐 byte 相同";
    const problems = [];
    try {
        const engine = await loadEngine();
        load(engine, fs.readFileSync("build/sound_test.lua", "utf8"));
        const chunks = [];
        for (let i = 0; i < 480; i++) {
            engine.input_set(0, 0);
            engine.engine_frame();
            const n = engine.audio_sample_count();
            // Int16Array 直接看 wasm 記憶體那一塊（位置要是 2 的倍數，int16_t 陣列本來就是）；slice() 複製出來
            chunks.push(new Int16Array(engine.memory.buffer, engine.audio_samples(), n).slice());
        }
        const total = chunks.reduce((a, c) => a + c.length, 0);
        const web = new Int16Array(total);
        let k = 0;
        for (const c of chunks) { web.set(c, k); k += c.length; }
        const wav = fs.readFileSync("build/sound.wav");
        const mac = new Int16Array(wav.buffer.slice(wav.byteOffset + 44, wav.byteOffset + wav.length));
        if (mac.length !== web.length) problems.push(`取樣數不同：Mac ${mac.length}、網頁 ${web.length}`);
        else {
            for (let i = 0; i < mac.length; i++) {
                if (mac[i] !== web[i]) { problems.push(`第 ${i} 個取樣不同：Mac ${mac[i]}、網頁 ${web[i]}`); break; }
            }
        }
        const macPPM = (() => { const p = fs.readFileSync("build/sound.ppm"); return p.subarray(p.length - W * H * 3); })();
        if (Buffer.compare(framebufferRGB(engine), macPPM) !== 0) problems.push("畫面和 Mac 版不同（檢查方塊）");
    } catch (e) {
        problems.push(String(e));
    }
    if (problems.length === 0) ok(name);
    else fail(name, problems.join("；"));
}

process.exit(failures ? 1 : 0);
