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

// 在網頁上顯示偵測到的手把，方便除錯
const padInfo = () => document.getElementById("pad-info");
window.addEventListener("gamepadconnected", (e) => {
    padInfo().textContent =
        `手把 ${e.gamepad.index + 1}：${e.gamepad.id}（配置：${e.gamepad.mapping || "非標準"}）`;
});
window.addEventListener("gamepaddisconnected", () => {
    padInfo().textContent = "沒有偵測到手把（接上後要先按一個鍵，瀏覽器才會看到）";
});

async function start() {
    const response = await fetch("comet.wasm");
    const bytes = await response.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {});
    const engine = instance.exports;

    const memory  = new Uint8Array(engine.memory.buffer);
    const fbAddr  = engine.gfx_framebuffer();
    const palAddr = engine.gfx_palette();

    const canvas = document.getElementById("screen");
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(SCREEN_W, SCREEN_H);

    // 把引擎的畫面（顏色編號）查調色盤，畫到 canvas
    function present() {
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
            // 1 號玩家：鍵盤和第 1 支手把都能控制
            engine.input_set(0, keys | readGamepad(0));
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
