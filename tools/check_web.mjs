// check_web.mjs — 網頁版的自動測試（用 Node.js 執行，不用開瀏覽器）
//
// 用法：make check
//
// 測試 1：同一串按鍵跑 move_box 90 幀，網頁版的畫面必須和 Mac 版的 out.ppm 逐像素相同
// 測試 2：載入一個會出錯的遊戲，引擎要能接住錯誤、印出訊息，而不是整個當掉

import fs from "fs";

const WASM = "build/web/comet.wasm";
const W = 320, H = 240;
const BTN_RIGHT = 1, BTN_DOWN = 3, BTN_A = 4;

let failures = 0;
const logs = [];

async function loadEngine() {
    let engine = null;
    const env = new Proxy({
        js_log(ptr) {
            const mem = new Uint8Array(engine.memory.buffer);
            let end = ptr;
            while (mem[end] !== 0) end++;
            logs.push(new TextDecoder().decode(mem.subarray(ptr, end)));
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

process.exit(failures ? 1 : 0);
