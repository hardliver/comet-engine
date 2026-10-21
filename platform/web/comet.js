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

// ---- 存檔的保管處 ----
// 預設放在瀏覽器的 localStorage：同一個網址（例如 http://localhost:8420）的網頁共用，
// 關掉瀏覽器還在，清除網站資料才會不見。
// localStorage 只能存文字，所以 bytes 轉成 base64。
// 無痕模式或被封鎖時 localStorage 會出錯：改存在記憶體裡（關掉分頁就沒了），遊戲照樣能玩。
// 之後網頁版 Nova 可以換成別的保管處（OPFS、同步到 GitHub），只要提供同樣的 get／set／remove
export function browserSaveStore() {
    const memory = new Map();
    let ls = null;
    try {
        ls = window.localStorage;
        ls.getItem("comet:test");
    } catch {
        ls = null;
    }
    return {
        get(key) {
            try { if (ls) return ls.getItem(key); } catch { /* 改用記憶體 */ }
            return memory.has(key) ? memory.get(key) : null;
        },
        set(key, value) {   // 成功回傳 true；空間不足回傳 false
            try {
                if (ls) { ls.setItem(key, value); return true; }
            } catch {
                return false;
            }
            memory.set(key, value);
            return true;
        },
        remove(key) {
            try { if (ls) ls.removeItem(key); } catch { /* 忽略 */ }
            memory.delete(key);
        },
    };
}

function bytesToBase64(bytes) {
    let s = "";
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa(s);
}

function base64ToBytes(text) {
    const s = atob(text);
    const bytes = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i);
    return bytes;
}

// 一個遊戲的一個存檔欄位，在保管處裡的名字
export function saveKey(gameId, slot) {
    return `comet:save:${gameId}:${slot}`;
}

// 把 JavaScript 字串複製進 wasm 記憶體，變成 C 字串（結尾補 0）。用完要 free
function writeCString(exports, text) {
    const bytes = new TextEncoder().encode(text);
    const ptr = exports.malloc(bytes.length + 1);
    const mem = new Uint8Array(exports.memory.buffer);
    mem.set(bytes, ptr);
    mem[ptr + bytes.length] = 0;
    return ptr;
}

export async function createEngine({ wasmUrl = "comet.wasm", onLog = console.log, saveStore = browserSaveStore() } = {}) {
    let exports = null;

    // wasm 需要從 JavaScript 拿到的函式（imports）。
    // js_log：C 的 platform_log 會呼叫它。
    // js_save_read／js_save_write：C 的 platform_save_read／platform_save_write（web.c）會呼叫。
    //   C 給的是 wasm 記憶體裡的位置（指標），從那裡讀寫 bytes
    // 其他是 C 標準函式庫順帶要求的系統功能（檔案、時間），遊戲用不到，
    // 統一回傳「沒有這個檔案」的錯誤碼。用 Proxy 是為了不用一個一個列出名字。
    const env = new Proxy({
        js_log: (ptr) => onLog(readCString(exports, ptr)),

        // 回傳長度；沒有存檔回傳 -1
        js_save_read: (idPtr, slot, buf, cap) => {
            const text = saveStore.get(saveKey(readCString(exports, idPtr), slot));
            if (text == null) return -1;
            let bytes;
            try { bytes = base64ToBytes(text); } catch { return -1; }   // 被改壞了
            if (bytes.length > cap) return -1;
            new Uint8Array(exports.memory.buffer).set(bytes, buf);
            return bytes.length;
        },
        // data 是 0（C 的 NULL）代表刪除。成功回傳 0
        js_save_write: (idPtr, slot, data, len) => {
            const key = saveKey(readCString(exports, idPtr), slot);
            if (data === 0) {
                saveStore.remove(key);
                return 0;
            }
            const bytes = new Uint8Array(exports.memory.buffer, data, len);
            return saveStore.set(key, bytesToBase64(bytes)) ? 0 : 1;
        },
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

    let pakPtr = 0;   // 目前資源包在 wasm 記憶體裡的位置（0 = 沒有）

    return {
        exports,

        // 資源包（見 core/pak.h）：要在 load() 之前設定。bytes 是 Uint8Array，null = 沒有。
        // 和 load() 不同，這塊記憶體要一直留著（引擎需要時才去讀），換資源包時才 free。
        // 成功回傳 0；格式不對回傳 1（原因透過 onLog 顯示）
        setPak(bytes) {
            exports.pak_set(0, 0);
            if (pakPtr) exports.free(pakPtr);
            pakPtr = 0;
            if (!bytes) return 0;
            pakPtr = exports.malloc(bytes.length);
            if (!pakPtr) {
                onLog(`資源包太大，記憶體不夠（${(bytes.length / 1048576).toFixed(1)} MB）。請用新版的 comet.wasm（make web）`);
                return 1;
            }
            new Uint8Array(exports.memory.buffer).set(bytes, pakPtr);
            return exports.pak_set(pakPtr, bytes.length);
        },

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

        // ---- 系統卡匣（Nova 模式，說明見 core/system.h）----

        // 載入系統卡匣：和 load() 一樣，但 Lua 裡多一個 sys 表格
        loadSystem(source) {
            const bytes = new TextEncoder().encode(source);
            const ptr = exports.malloc(bytes.length);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const err = exports.engine_load_system(ptr, bytes.length);
            exports.free(ptr);
            return err;
        },
        // 遊戲清單（系統卡匣用 sys.games() 看到的）
        clearGames() { exports.system_clear_games(); },
        // info（可省略）：{ desc: 一句話介紹, players: 幾人遊玩 }，大廳顯示用
        addGame(id, name, info = {}) {
            const a = writeCString(exports, id), b = writeCString(exports, name);
            let err = exports.system_add_game(a, b);
            if (err === 0 && (info.desc || info.players)) {
                const c = writeCString(exports, info.desc || "");
                exports.system_set_info(a, c, info.players | 0);
                exports.free(c);
            }
            exports.free(a);
            exports.free(b);
            return err;
        },
        // 每一幀之後問：0 沒事、1 要啟動 requestGame() 這款、2 要回 HOME。做完呼叫 requestDone()
        request() { return exports.system_request(); },
        requestGame() { return readCString(exports, exports.system_request_game()); },
        requestDone() { exports.system_request_done(); },

        // 只檢查語法，不執行（正在跑的遊戲不受影響）。沒錯回傳 null，有錯回傳錯誤訊息
        check(source) {
            const bytes = new TextEncoder().encode(source);
            const ptr = exports.malloc(bytes.length);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const bad = exports.engine_check(ptr, bytes.length);
            exports.free(ptr);
            return bad ? readCString(exports, exports.engine_check_error()) : null;
        },

        // 開發工具用（Studio 的「狀態」分頁）：在正在執行的遊戲裡跑一段 Lua。
        // mode 0 = 指令（會改遊戲狀態）、1 = 讀 DEBUG_VIEW 攤成一行一行、2 = 只讀一個值。
        // 回傳 { ok, text }：結果的文字，或錯誤訊息
        eval(source, mode = 0) {
            const bytes = new TextEncoder().encode(source);
            const ptr = exports.malloc(bytes.length + 1);
            new Uint8Array(exports.memory.buffer).set(bytes, ptr);
            const err = exports.engine_eval(ptr, bytes.length, mode);
            exports.free(ptr);
            return { ok: !err, text: readCString(exports, exports.engine_eval_result()) };
        },

        // 運算預算（Level 1）：開了之後每幀記錄 Lua 指令數、像素、繪圖呼叫次數（C 的 engine_profile）
        profile(on) { exports.engine_profile(on ? 1 : 0); },
        // 上一幀的數字；load 是預估 Pico 負載（千分比，1000 = 一幀的時間剛好用完）
        prof() {
            return {
                instr: exports.engine_prof_instr() >>> 0,
                pixels: exports.engine_prof_pixels() >>> 0,
                calls: exports.engine_prof_calls() >>> 0,
                load: exports.engine_prof_load() >>> 0,
            };
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

        // 開始錄影：load() 之前呼叫（遊戲在 _init 讀的存檔也要記下來）；load() 失敗時呼叫 replayStop()
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
// afterFrame()：每跑完一幀呼叫一次（網頁版 Nova 用來處理「啟動遊戲／回 HOME」的請求）
export function startLoop(engine, canvas, getInput, { afterFrame = null } = {}) {
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(SCREEN_W, SCREEN_H);
    let last = performance.now();
    let acc = 0;
    let paused = false;
    let running = true;
    // 速度（開發用，Studio 的「×2」「×4」）：每 1/60 秒跑 speed 幀，畫面只顯示最後一幀。
    // 遊戲收到的還是一幀一幀同樣的輸入，所以結果和原速完全相同，只是比較快跑完
    let speed = 1;

    function tick(now) {
        if (!running) return;
        acc += now - last;
        last = now;

        // 分頁被切走很久再回來，不要一口氣補跑幾千幀
        if (acc > 250) acc = 250;

        while (acc >= FRAME_MS) {
            for (let k = 0; k < speed && !paused; k++) {
                for (let p = 0; p < 4; p++) engine.setInput(p, getInput(p));
                engine.frame();
                if (afterFrame) afterFrame();
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
        setSpeed(n) { speed = Math.max(1, Math.floor(n)); },
        speed() { return speed; },
        isPaused() { return paused; },
        // 暫停時一次只跑一幀（除錯用）
        step() {
            for (let p = 0; p < 4; p++) engine.setInput(p, getInput(p));
            engine.frame();
        },
    };
}
