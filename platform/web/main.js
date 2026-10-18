// main.js — 網頁平台層
// 工作有三件：
//   1. 收集鍵盤狀態，交給引擎
//   2. 每 1/60 秒叫引擎跑一幀
//   3. 把引擎的畫面搬到 <canvas>

const SCREEN_W = 320;
const SCREEN_H = 240;
const FRAME_MS = 1000 / 60;   // 一幀幾毫秒（約 16.67）

// 按鍵編號，和 core/input.h 一致
const BTN = { LEFT: 0, RIGHT: 1, UP: 2, DOWN: 3, A: 4, B: 5, START: 6, SELECT: 7 };

// 鍵盤 → 按鍵編號
const KEYMAP = {
    ArrowLeft:  BTN.LEFT,
    ArrowRight: BTN.RIGHT,
    ArrowUp:    BTN.UP,
    ArrowDown:  BTN.DOWN,
    KeyZ:       BTN.A,
    KeyX:       BTN.B,
    Enter:      BTN.START,
    ShiftRight: BTN.SELECT,
};

// 目前按著哪些鍵，和 C 一樣用 bit 表示
let keys = 0;

window.addEventListener("keydown", (e) => {
    const b = KEYMAP[e.code];
    if (b === undefined) return;
    keys |= (1 << b);          // 把第 b 個 bit 設成 1
    e.preventDefault();        // 避免方向鍵捲動網頁
});

window.addEventListener("keyup", (e) => {
    const b = KEYMAP[e.code];
    if (b === undefined) return;
    keys &= ~(1 << b);         // 把第 b 個 bit 設成 0
});

// 切到別的視窗時，keyup 可能收不到，全部放開比較安全
window.addEventListener("blur", () => { keys = 0; });

// ---- 手把（Gamepad API）----
// 瀏覽器把認得的手把整理成「標準配置」，按鍵編號固定：
//   0 = 下面那顆（Xbox 的 A）、1 = 右邊那顆（Xbox 的 B）
//   8 = Back/Select、9 = Start、12–15 = 十字鍵 上下左右
const PAD_BUTTONS = [
    [12, BTN.UP], [13, BTN.DOWN], [14, BTN.LEFT], [15, BTN.RIGHT],
    [0, BTN.A], [1, BTN.B], [9, BTN.START], [8, BTN.SELECT],
];
const STICK_DEADZONE = 0.5;   // 搖桿推超過一半才算按方向鍵

// 讀第 index 支手把，回傳和鍵盤一樣格式的 bit 狀態
function readGamepad(index) {
    const pad = navigator.getGamepads()[index];
    if (!pad) return 0;

    let b = 0;
    for (const [padBtn, btn] of PAD_BUTTONS) {
        if (pad.buttons[padBtn] && pad.buttons[padBtn].pressed) {
            b |= (1 << btn);
        }
    }
    // 左搖桿也當方向鍵用
    const x = pad.axes[0] || 0;
    const y = pad.axes[1] || 0;
    if (x < -STICK_DEADZONE) b |= (1 << BTN.LEFT);
    if (x >  STICK_DEADZONE) b |= (1 << BTN.RIGHT);
    if (y < -STICK_DEADZONE) b |= (1 << BTN.UP);
    if (y >  STICK_DEADZONE) b |= (1 << BTN.DOWN);
    return b;
}

// ---- 觸控按鈕 ----
// 和鍵盤一樣，最後只是設定 bit，遊戲分不出來是哪一種輸入。
// 用 pointer 事件（同時支援手指和滑鼠），每根手指有自己的 pointerId，
// 所以可以一手按十字鍵、一手按 A。

let dpadBits = 0;                 // 十字鍵目前的方向
const buttonPointers = new Map(); // 哪根手指按著哪個按鍵

function touchKeys() {
    let b = dpadBits;
    for (const btn of buttonPointers.values()) b |= (1 << btn);
    return b;
}

// 目前的觸控配置："nes"（紅白機）或 "modern"（現代）
let layout = "nes";

// 左手區：兩種配置共用同一塊，判斷方向的方法不同
//   紅白機：十字鍵。手指離中心往哪邊偏就按哪邊，橫、直各自判斷，
//           所以靠近角落時會斜按
//   現代：類比搖桿。看手指的「角度」切成 8 個方向，每個方向 45°，
//         手感比較平均；圓頭跟著手指移動
function setupDpad(el) {
    const knob = document.getElementById("knob");
    let activePointer = null;

    function update(e) {
        const r = el.getBoundingClientRect();
        const radius = r.width / 2;
        const dx = e.clientX - (r.left + radius);
        const dy = e.clientY - (r.top + radius);
        let b = 0;

        if (layout === "nes") {
            const dead = radius * 0.3;      // 中心一小圈不算任何方向
            if (dx < -dead) b |= (1 << BTN.LEFT);
            if (dx >  dead) b |= (1 << BTN.RIGHT);
            if (dy < -dead) b |= (1 << BTN.UP);
            if (dy >  dead) b |= (1 << BTN.DOWN);
        } else {
            const dist = Math.hypot(dx, dy);
            if (dist > radius * 0.25) {
                // 角度換算成 0–7：0 = 右，往順時針每 45° 加 1
                const sector = Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) & 7;
                const DIRS = [
                    [BTN.RIGHT], [BTN.RIGHT, BTN.DOWN], [BTN.DOWN], [BTN.DOWN, BTN.LEFT],
                    [BTN.LEFT], [BTN.LEFT, BTN.UP], [BTN.UP], [BTN.UP, BTN.RIGHT],
                ];
                for (const d of DIRS[sector]) b |= (1 << d);
            }
            // 圓頭最多移到底座邊緣
            const max = radius - 30;
            const k = dist > max ? max / dist : 1;
            knob.style.transform = `translate(${dx * k}px, ${dy * k}px)`;
        }
        dpadBits = b;
    }
    function release(e) {
        if (e.pointerId !== activePointer) return;
        activePointer = null;
        dpadBits = 0;
        knob.style.transform = "";
        el.classList.remove("active");
    }

    el.addEventListener("pointerdown", (e) => {
        activePointer = e.pointerId;
        el.setPointerCapture(e.pointerId);  // 手指滑出範圍也繼續追蹤
        el.classList.add("active");
        update(e);
    });
    el.addEventListener("pointermove", (e) => {
        if (e.pointerId === activePointer) update(e);
    });
    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
}

// A、B、Start、Select：按下就設定，放開就清除
function setupButton(el) {
    const btn = Number(el.dataset.btn);
    el.addEventListener("pointerdown", (e) => {
        buttonPointers.set(e.pointerId, btn);
        el.setPointerCapture(e.pointerId);
        el.classList.add("pressed");
    });
    const release = (e) => {
        buttonPointers.delete(e.pointerId);
        el.classList.remove("pressed");
    };
    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
}

setupDpad(document.getElementById("dpad"));
document.querySelectorAll("[data-btn]").forEach(setupButton);

// 長按不要跳出選單
window.addEventListener("contextmenu", (e) => e.preventDefault());

// ---- 工具列 ----

// 提示框：顯示幾秒後自動收起
let hintTimer = null;
function showHint(text, ms = 4000) {
    const el = document.getElementById("hint");
    el.textContent = text;
    el.classList.add("show");
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => el.classList.remove("show"), ms);
}

// 配置切換。選擇記在瀏覽器裡，下次打開還是同一種。
// 瀏覽器的儲存空間可能被關掉（例如私密瀏覽），所以用 try 包起來，失敗就算了
const LAYOUT_NAMES = { nes: "紅白機", modern: "現代" };

function applyLayout(name) {
    layout = name;
    document.body.classList.remove("layout-nes", "layout-modern");
    document.body.classList.add(`layout-${name}`);
    document.getElementById("btn-layout").textContent = `配置：${LAYOUT_NAMES[name]}`;
    try { localStorage.setItem("comet.layout", name); } catch (e) {}
}

try {
    const saved = localStorage.getItem("comet.layout");
    if (saved in LAYOUT_NAMES) layout = saved;
} catch (e) {}
applyLayout(layout);

document.getElementById("btn-layout").addEventListener("click", () => {
    applyLayout(layout === "nes" ? "modern" : "nes");
});

// 全螢幕。
// Android：瀏覽器支援 Fullscreen API，按一下就全螢幕，並嘗試鎖定橫向。
// iPhone：Safari 不讓網頁全螢幕，唯一的方法是「加入主畫面」，
//        從主畫面的圖示打開時就沒有網址列。
const isStandalone =
    window.matchMedia("(display-mode: fullscreen)").matches ||
    window.matchMedia("(display-mode: standalone)").matches ||
    window.navigator.standalone === true;

const fsButton = document.getElementById("btn-fullscreen");
if (isStandalone) fsButton.style.visibility = "hidden";  // 已經是全螢幕了

fsButton.addEventListener("click", async () => {
    const root = document.documentElement;
    const request = root.requestFullscreen || root.webkitRequestFullscreen;

    if (document.fullscreenElement || document.webkitFullscreenElement) {
        (document.exitFullscreen || document.webkitExitFullscreen).call(document);
        return;
    }
    if (!request) {
        showHint("iPhone 請用 Safari 打開這個網頁，按下方的「分享」→「加入主畫面」，之後從主畫面的圖示打開就是全螢幕。", 8000);
        return;
    }
    try {
        await request.call(root);
        if (screen.orientation && screen.orientation.lock) {
            await screen.orientation.lock("landscape").catch(() => {});
        }
    } catch (e) {
        showHint("這個瀏覽器不允許全螢幕。");
    }
});

const onFullscreenChange = () => {
    const on = !!(document.fullscreenElement || document.webkitFullscreenElement);
    fsButton.textContent = on ? "離開全螢幕" : "全螢幕";
};
document.addEventListener("fullscreenchange", onFullscreenChange);
document.addEventListener("webkitfullscreenchange", onFullscreenChange);

// 在網頁上顯示偵測到的手把，方便除錯
const padInfo = () => document.getElementById("pad-info");
window.addEventListener("gamepadconnected", (e) => {
    padInfo().textContent =
        `手把 ${e.gamepad.index + 1}：${e.gamepad.id}（配置：${e.gamepad.mapping || "非標準"}）`;
});
window.addEventListener("gamepaddisconnected", () => {
    padInfo().textContent = "沒有偵測到手把（接上後要先按一個鍵，瀏覽器才會看到）";
});

// ---- 載入引擎 ----

// wasm 需要從 JavaScript 拿到的函式（imports）。
// js_log：C 的 platform_log 會呼叫它，把訊息印在瀏覽器的主控台。
// 其他是 C 標準函式庫順帶要求的系統功能（檔案、時間），遊戲用不到，
// 統一回傳「沒有這個檔案」的錯誤碼。用 Proxy 是為了不用一個一個列出名字。
function makeImports(getEngine) {
    const unsupported = new Proxy({}, { get: () => () => 8 });  // 8 = EBADF（沒有這個檔案）
    return {
        env: new Proxy({
            js_log(ptr) {
                const msg = readCString(getEngine(), ptr);
                console.log(msg);
                showHint(msg, 8000);   // 遊戲出錯時，錯誤訊息直接顯示在畫面上
            },
        }, { get: (target, name) => target[name] || (() => 0) }),
        wasi_snapshot_preview1: unsupported,
    };
}

// 從 wasm 記憶體讀出 C 字串：一直讀到值為 0 的 byte，再用 UTF-8 解碼
function readCString(engine, ptr) {
    const mem = new Uint8Array(engine.memory.buffer);
    let end = ptr;
    while (mem[end] !== 0) end++;
    return new TextDecoder().decode(mem.subarray(ptr, end));
}

// 把 Lua 原始碼（文字）交給引擎：
// 1. 用 C 的 malloc 在 wasm 記憶體裡借一塊空間
// 2. 把文字的 UTF-8 bytes 複製進去
// 3. 呼叫 engine_load(位置, 長度)
// 4. 用完 free 還回去（Lua 已經把需要的東西複製走了）
function loadGame(engine, source) {
    const bytes = new TextEncoder().encode(source);
    const ptr = engine.malloc(bytes.length);
    new Uint8Array(engine.memory.buffer).set(bytes, ptr);
    const err = engine.engine_load(ptr, bytes.length);
    engine.free(ptr);
    if (!err) document.getElementById("hint").classList.remove("show");
    return err;
}

async function start() {
    let engine = null;
    const response = await fetch("comet.wasm");
    const bytes = await response.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, makeImports(() => engine));
    engine = instance.exports;

    // emscripten 編出來的 wasm 有一個初始化函式，要先呼叫一次，
    // C 標準函式庫（malloc 等）才會準備好
    if (engine._initialize) engine._initialize();

    const fbAddr  = engine.gfx_framebuffer();
    const palAddr = engine.gfx_palette();

    const canvas = document.getElementById("screen");
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(SCREEN_W, SCREEN_H);

    // 把引擎的畫面（顏色編號）查調色盤，畫到 canvas。
    // 每次重新取得 memory.buffer：Lua 用到更多記憶體時 wasm 可能換一塊更大的，
    // 舊的 Uint8Array 就失效了
    function present() {
        const memory = new Uint8Array(engine.memory.buffer);
        for (let i = 0; i < SCREEN_W * SCREEN_H; i++) {
            const c = memory[fbAddr + i];
            image.data[i * 4 + 0] = memory[palAddr + c * 3 + 0];
            image.data[i * 4 + 1] = memory[palAddr + c * 3 + 1];
            image.data[i * 4 + 2] = memory[palAddr + c * 3 + 2];
            image.data[i * 4 + 3] = 255;
        }
        ctx.putImageData(image, 0, 0);
    }

    engine.engine_init();

    // 預設遊戲
    const game = await fetch("game.lua").then((r) => r.text());
    loadGame(engine, game);

    // 換遊戲：按「載入遊戲」選檔案，或在電腦上把 .lua 檔拖進網頁
    const picker = document.getElementById("file-picker");
    document.getElementById("btn-load").addEventListener("click", () => picker.click());
    picker.addEventListener("change", async () => {
        if (picker.files[0]) loadGame(engine, await picker.files[0].text());
        picker.value = "";
    });
    window.addEventListener("dragover", (e) => e.preventDefault());
    window.addEventListener("drop", async (e) => {
        e.preventDefault();
        const file = e.dataTransfer.files[0];
        if (file) loadGame(engine, await file.text());
    });

    // 遊戲迴圈。
    // requestAnimationFrame 跟著螢幕更新：60Hz 螢幕每秒 60 次，
    // 120Hz 螢幕每秒 120 次。所以不能「被叫一次就跑一幀」，
    // 而是累積經過的時間，每滿 1/60 秒才跑一幀。
    let last = performance.now();
    let acc = 0;   // 累積了多少還沒跑掉的時間

    function tick(now) {
        acc += now - last;
        last = now;

        // 分頁被切走很久再回來，不要一口氣補跑幾千幀
        if (acc > 250) acc = 250;

        while (acc >= FRAME_MS) {
            // 1 號玩家：鍵盤、觸控、第 1 支手把都能控制
            engine.input_set(0, keys | touchKeys() | readGamepad(0));
            // 2–4 號玩家：第 2–4 支手把
            for (let p = 1; p < 4; p++) {
                engine.input_set(p, readGamepad(p));
            }
            engine.engine_frame();
            acc -= FRAME_MS;
        }

        present();
        requestAnimationFrame(tick);
    }

    requestAnimationFrame(tick);
}

start();
