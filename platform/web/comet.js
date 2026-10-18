// comet.js — 在網頁上執行 Comet 引擎
// 遊玩頁（index.html）和編輯器（studio.html）共用這個檔案。
//
// 用法：
//   const engine = await createEngine({ onLog: (msg) => ... });
//   engine.load(luaSource);
//   const loop = startLoop(engine, canvas, getInput);

export const SCREEN_W = 320;
export const SCREEN_H = 240;
const FRAME_MS = 1000 / 60;   // 一幀幾毫秒（約 16.67）

// ---- 載入引擎 ----

// 從 wasm 記憶體讀出 C 字串：一直讀到值為 0 的 byte，再用 UTF-8 解碼
function readCString(exports, ptr) {
    const mem = new Uint8Array(exports.memory.buffer);
    let end = ptr;
    while (mem[end] !== 0) end++;
    return new TextDecoder().decode(mem.subarray(ptr, end));
}

export async function createEngine({ wasmUrl = "comet.wasm", onLog = console.log } = {}) {
    let exports = null;

    // wasm 需要從 JavaScript 拿到的函式（imports）。
    // js_log：C 的 platform_log 會呼叫它。
    // 其他是 C 標準函式庫順帶要求的系統功能（檔案、時間），遊戲用不到，
    // 統一回傳「沒有這個檔案」的錯誤碼。用 Proxy 是為了不用一個一個列出名字。
    const env = new Proxy({
        js_log: (ptr) => onLog(readCString(exports, ptr)),
    }, { get: (target, name) => target[name] || (() => 0) });
    const wasi = new Proxy({}, { get: () => () => 8 });   // 8 = EBADF

    const bytes = await fetch(wasmUrl).then((r) => r.arrayBuffer());
    const { instance } = await WebAssembly.instantiate(bytes, { env, wasi_snapshot_preview1: wasi });
    exports = instance.exports;

    // emscripten 編出來的 wasm 有一個初始化函式，要先呼叫一次，
    // C 標準函式庫（malloc 等）才會準備好
    if (exports._initialize) exports._initialize();
    exports.engine_init();

    const fbAddr  = exports.gfx_framebuffer();
    const palAddr = exports.gfx_palette();

    return {
        exports,

        // 把 Lua 原始碼（文字）交給引擎，成功回傳 0：
        // 1. 用 C 的 malloc 在 wasm 記憶體裡借一塊空間
        // 2. 把文字的 UTF-8 bytes 複製進去
        // 3. 呼叫 engine_load(位置, 長度)
        // 4. 用完 free 還回去（Lua 已經把需要的東西複製走了）
        load(source) {
            const bytes = new TextEncoder().encode(source);
            const ptr = exports.malloc(bytes.length);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const err = exports.engine_load(ptr, bytes.length);
            exports.free(ptr);
            return err;
        },

        // 只檢查語法，不執行（正在跑的遊戲不受影響）。沒錯回傳 null，有錯回傳錯誤訊息
        check(source) {
            const bytes = new TextEncoder().encode(source);
            const ptr = exports.malloc(bytes.length);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const bad = exports.engine_check(ptr, bytes.length);
            exports.free(ptr);
            return bad ? readCString(exports, exports.engine_check_error()) : null;
        },

        // 一位玩家這一幀的輸入：{ buttons, lx, ly, rx, ry }（格式見 input.js）
        setInput(player, s) {
            exports.input_set(player, s.buttons);
            exports.input_set_stick(player, 0, s.lx, s.ly);
            exports.input_set_stick(player, 1, s.rx, s.ry);
        },
        frame() { exports.engine_frame(); },

        // ---- 亂數種子與重播（格式見 core/replay.h）----

        // 下一次 load() 用的亂數種子（0～4294967295）
        setSeed(seed) { exports.engine_set_seed(seed >>> 0); },

        // 開始錄影：load() 成功之後馬上呼叫
        recordBegin() { return exports.replay_record_begin(); },

        // 到目前為止的重播檔（Uint8Array）。沒有在錄影回傳 null。
        // slice() 複製一份出來：wasm 記憶體裡那一塊，下一幀就可能被改掉
        replaySnapshot() {
            const ptr = exports.replay_snapshot();
            if (!ptr) return null;
            const len = exports.replay_snapshot_size();
            return new Uint8Array(exports.memory.buffer, ptr, len).slice();
        },

        // 準備播放：之後要再 load() 同一個遊戲。成功回傳 0
        replayPlay(bytes) {
            const ptr = exports.malloc(bytes.length);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const err = exports.replay_play_begin(ptr, bytes.length);
            exports.free(ptr);   // 引擎已經複製一份了
            return err;
        },
        replayStop() { exports.replay_stop(); },
        // 0 沒有、1 錄影中、2 播放中、3 播完了
        replayState() { return exports.replay_state(); },
        replayFrames() { return exports.replay_frames(); },
        replayMatched() { return exports.replay_matched() === 1; },
        frameCount() { return exports.engine_frame_count(); },

        // 把引擎的畫面（顏色編號）查調色盤，寫進 ImageData。
        // 每次重新取得 memory.buffer：Lua 用到更多記憶體時 wasm 可能換一塊更大的，
        // 舊的 Uint8Array 就失效了
        present(ctx, image) {
            const memory = new Uint8Array(exports.memory.buffer);
            const d = image.data;
            for (let i = 0; i < SCREEN_W * SCREEN_H; i++) {
                const c = memory[fbAddr + i];
                d[i * 4 + 0] = memory[palAddr + c * 3 + 0];
                d[i * 4 + 1] = memory[palAddr + c * 3 + 1];
                d[i * 4 + 2] = memory[palAddr + c * 3 + 2];
                d[i * 4 + 3] = 255;
            }
            ctx.putImageData(image, 0, 0);
        },
    };
}

// ---- 遊戲迴圈 ----
// requestAnimationFrame 跟著螢幕更新：60Hz 螢幕每秒 60 次，120Hz 螢幕每秒 120 次。
// 所以不能「被叫一次就跑一幀」，而是累積經過的時間，每滿 1/60 秒才跑一幀。
//
// getInput(player) 回傳那位玩家這一幀的輸入狀態（格式見 input.js）。
export function startLoop(engine, canvas, getInput) {
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(SCREEN_W, SCREEN_H);
    let last = performance.now();
    let acc = 0;
    let paused = false;
    let running = true;

    function tick(now) {
        if (!running) return;
        acc += now - last;
        last = now;

        // 分頁被切走很久再回來，不要一口氣補跑幾千幀
        if (acc > 250) acc = 250;

        while (acc >= FRAME_MS) {
            if (!paused) {
                for (let p = 0; p < 4; p++) engine.setInput(p, getInput(p));
                engine.frame();
            }
            acc -= FRAME_MS;
        }
        engine.present(ctx, image);
        requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);

    return {
        stop() { running = false; },
        setPaused(p) { paused = p; },
        isPaused() { return paused; },
        // 暫停時一次只跑一幀（除錯用）
        step() {
            for (let p = 0; p < 4; p++) engine.setInput(p, getInput(p));
            engine.frame();
        },
    };
}
