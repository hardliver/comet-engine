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
            engine.input_set(0, keys);
            engine.engine_frame();
            acc -= FRAME_MS;
        }

        present();
        requestAnimationFrame(tick);
    }

    requestAnimationFrame(tick);
}

start();
