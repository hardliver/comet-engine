// play.js — 遊玩頁（index.html）
// 引擎和輸入的細節在 comet.js、input.js；這裡只負責遊玩頁的畫面和按鈕。

import { createEngine, startLoop } from "./comet.js";
import { setupKeyboard, setupTouch, readGamepad, merge } from "./input.js";

// ---- 提示框：顯示幾秒後自動收起 ----
const hint = document.getElementById("hint");
let hintTimer = null;
function showHint(text, ms = 4000) {
    hint.textContent = text;
    hint.classList.add("show");
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => hint.classList.remove("show"), ms);
}

// ---- 觸控配置 ----
// 選擇記在瀏覽器裡，下次打開還是同一種。
// 瀏覽器的儲存空間可能被關掉（例如私密瀏覽），所以用 try 包起來，失敗就算了
const LAYOUT_NAMES = { nes: "紅白機", modern: "現代" };
let layout = "modern";   // 預設：現代（雙搖桿 + 十字鍵 + ABXY）

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

// ---- 全螢幕 ----
// Android：瀏覽器支援 Fullscreen API，按一下就全螢幕，並嘗試鎖定橫向。
// iPhone：Safari 不讓網頁全螢幕，唯一的方法是「加入主畫面」。
const isStandalone =
    window.matchMedia("(display-mode: fullscreen)").matches ||
    window.matchMedia("(display-mode: standalone)").matches ||
    window.navigator.standalone === true;

const fsButton = document.getElementById("btn-fullscreen");
if (isStandalone) fsButton.remove();   // 已經是全螢幕了

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
        await screen.orientation?.lock?.("landscape").catch(() => {});
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

// 長按不要跳出選單
window.addEventListener("contextmenu", (e) => e.preventDefault());

// ---- 手把資訊（除錯用）----
const padInfo = document.getElementById("pad-info");
window.addEventListener("gamepadconnected", (e) => {
    padInfo.textContent =
        `手把 ${e.gamepad.index + 1}：${e.gamepad.id}（配置：${e.gamepad.mapping || "非標準"}）`;
});
window.addEventListener("gamepaddisconnected", () => {
    padInfo.textContent = "沒有偵測到手把（接上後要先按一個鍵，瀏覽器才會看到）";
});

// ---- 啟動 ----
const keyboard = setupKeyboard();
const touch = setupTouch(document);

// 1 號玩家：鍵盤、觸控、第 1 支手把都能控制；2–4 號玩家：第 2–4 支手把
function getInput(player) {
    if (player === 0) return merge(keyboard(), touch(), readGamepad(0));
    return readGamepad(player);
}

const engine = await createEngine({
    onLog: (msg) => { console.log(msg); showHint(msg, 8000); },
});

// 要玩哪一款：網址有 ?game=xxx.lua 就載那個，否則載 game.lua
const params = new URLSearchParams(location.search);
const gameUrl = params.get("game") || "game.lua";
engine.load(await fetch(gameUrl).then((r) => r.text()));

// 換遊戲：按「載入遊戲」選檔案，或在電腦上把 .lua 檔拖進網頁
function loadFile(file) {
    file.text().then((src) => {
        if (!engine.load(src)) hint.classList.remove("show");
    });
}
const picker = document.getElementById("file-picker");
document.getElementById("btn-load").addEventListener("click", () => picker.click());
picker.addEventListener("change", () => {
    if (picker.files[0]) loadFile(picker.files[0]);
    picker.value = "";
});
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
    e.preventDefault();
    if (e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]);
});

startLoop(engine, document.getElementById("screen"), getInput);
