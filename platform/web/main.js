// main.js — 網頁平台層
// 載入編譯好的引擎（comet.wasm），請它畫圖，再把畫面搬到 <canvas>。
// 角色和 Mac 版的 main.c 一樣，只是輸出的地方從圖檔換成網頁。

const SCREEN_W = 320;
const SCREEN_H = 240;

async function start() {
    // 1. 下載並啟動 WebAssembly。
    //    引擎不需要網頁提供任何東西，所以第二個參數是空的 {}
    const response = await fetch("comet.wasm");
    const bytes = await response.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {});
    const engine = instance.exports;

    // 2. 呼叫 C 的函式：畫測試畫面
    engine.test_scene_draw();

    // 3. 找到畫面和調色盤在 WASM 記憶體裡的位置。
    //    C 回傳的「指標」到了 JavaScript 就只是一個數字：從記憶體開頭算起第幾個 byte
    const memory = new Uint8Array(engine.memory.buffer);
    const fbAddr  = engine.gfx_framebuffer();
    const palAddr = engine.gfx_palette();

    // 4. 查調色盤，把顏色編號換成 RGBA（canvas 每個像素要 4 bytes，多一個透明度）
    const canvas = document.getElementById("screen");
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(SCREEN_W, SCREEN_H);

    for (let i = 0; i < SCREEN_W * SCREEN_H; i++) {
        const c = memory[fbAddr + i];            // 這個像素的顏色編號
        image.data[i * 4 + 0] = memory[palAddr + c * 3 + 0];  // R
        image.data[i * 4 + 1] = memory[palAddr + c * 3 + 1];  // G
        image.data[i * 4 + 2] = memory[palAddr + c * 3 + 2];  // B
        image.data[i * 4 + 3] = 255;                          // 不透明
    }

    // 5. 畫到 canvas 上
    ctx.putImageData(image, 0, 0);
}

start();
