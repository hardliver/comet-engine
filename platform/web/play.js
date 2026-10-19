// play.js — 遊玩頁（index.html）
// 引擎和輸入的細節在 comet.js、input.js；這裡只負責遊玩頁的畫面和按鈕。

import { createEngine, startLoop } from "./comet.js";
import { setupKeyboard, setupTouch, readGamepad, connectedPads, merge } from "./input.js";
import { setupTouchpad } from "./touchpad.js";

// ---- 提示框：顯示幾秒後自動收起 ----
const hint = document.getElementById("hint");
let hintTimer = null;
function showHint(text, ms = 4000) {
    hint.textContent = text;
    hint.classList.add("show");
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => hint.classList.remove("show"), ms);
}

// ---- 虛擬手把（手機）----
// 位置、配置、透明度、自訂都在 touchpad.js；這裡只接選單按鈕
const pad = setupTouchpad();
const $ = (id) => document.getElementById(id);

const menu = $("menu");
function refreshMenu() {
    for (const b of $("seg-preset").children) b.classList.toggle("on", b.dataset.preset === pad.preset());
    for (const b of $("seg-screen").children) b.classList.toggle("on", b.dataset.screen === pad.screenMode());
    $("opacity").value = pad.opacity();
}
$("btn-menu").addEventListener("click", () => { refreshMenu(); menu.classList.toggle("show"); });
$("btn-close-menu").addEventListener("click", () => menu.classList.remove("show"));

for (const b of $("seg-preset").children) {
    b.addEventListener("click", () => { pad.setPreset(b.dataset.preset); refreshMenu(); });
}
for (const b of $("seg-screen").children) {
    b.addEventListener("click", () => { pad.setScreenMode(b.dataset.screen); refreshMenu(); });
}
$("opacity").addEventListener("input", (e) => pad.setOpacity(Number(e.target.value)));

// 調整按鍵位置：選單收起來，上方出現「縮小／放大／重設／完成」
$("btn-edit").addEventListener("click", () => {
    menu.classList.remove("show");
    pad.startEditing();
    showHint("拖曳按鍵可以移動；點一下選取後，可以按上方的放大、縮小", 4000);
});
$("btn-smaller").addEventListener("click", () => pad.smaller());
$("btn-bigger").addEventListener("click", () => pad.bigger());
$("btn-reset").addEventListener("click", () => pad.reset());
$("btn-done").addEventListener("click", () => pad.finishEditing());

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
    menu.classList.remove("show");
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
// 列出所有連著的手把，標出各是幾號玩家。
// mapping 不是 "standard" 時，按鍵位置可能和上面的對照表不同，要另外處理
const padInfo = document.getElementById("pad-info");
function showPads() {
    if (!window.isSecureContext) {
        padInfo.textContent = "這個網址不是 https 或 localhost，瀏覽器不開放手把（請用 http://localhost:8420）";
        return;
    }
    const pads = connectedPads();
    padInfo.textContent = pads.length === 0
        ? "沒有偵測到手把（接上後要先按一個鍵，瀏覽器才會看到）"
        : pads.map((p, i) => `${i + 1}P：${p.id}（${p.mapping || "非標準配置"}）`).join("　");
}
window.addEventListener("gamepadconnected", showPads);
window.addEventListener("gamepaddisconnected", showPads);
showPads();

// ---- 啟動 ----
const keyboard = setupKeyboard();
const touch = setupTouch(document);

// 1 號玩家：鍵盤、觸控、第 1 支連著的手把都能控制；2–4 號玩家：第 2–4 支
function getInput(player) {
    if (player === 0) return merge(keyboard(), touch(), readGamepad(0));
    return readGamepad(player);
}

const engine = await createEngine({
    onLog: (msg) => { console.log(msg); showHint(msg, 8000); },
});

// ---- 開始一款遊戲（每次都錄影）----
// 每次開局給一個新的亂數種子，遊戲才不會每次都遇到一樣的事；
// 種子記在重播檔裡，所以重播時還是一模一樣。
// 從開機就一直錄：成就驗證需要「從新遊戲開始」的完整紀錄，
// 玩家任何時候按「下載重播」，拿到的都是從頭到現在的檔案（錄影繼續）。
let currentSource = null;
let currentName = "game";

function randomSeed() {
    return crypto.getRandomValues(new Uint32Array(1))[0] || 1;
}

function startGame(source, name) {
    engine.replayStop();
    engine.setSeed(randomSeed());
    engine.recordBegin();   // 先開始錄再載入：遊戲在 _init 讀存檔也會記下來
    const err = engine.load(source);
    if (err) {
        engine.replayStop();
    } else {
        currentSource = source;
        currentName = name;
    }
    return err;
}

// 檔名：遊戲名稱-日期時間.rep
function saveReplay() {
    const data = engine.replaySnapshot();
    if (!data) {
        showHint("目前沒有在錄影（播放重播時不會錄）");
        return;
    }
    const stamp = new Date().toISOString().slice(0, 19).replace(/[-:]/g, "").replace("T", "-");
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
    a.download = `${currentName}-${stamp}.rep`;
    a.click();
    URL.revokeObjectURL(a.href);
    const sec = Math.round(engine.frameCount() / 60);
    showHint(`已下載重播：${sec} 秒，${(data.length / 1024).toFixed(1)} KB`);
}
$("btn-save-replay").addEventListener("click", saveReplay);
$("btn-save-replay-touch").addEventListener("click", () => { menu.classList.remove("show"); saveReplay(); });

// 播放重播：重新載入同一個遊戲，輸入改由重播檔提供。播完後可以接著玩
let replayWatch = null;
function playReplay(bytes) {
    if (engine.replayPlay(bytes)) return;   // 格式不對：引擎會透過 onLog 顯示原因
    const err = engine.load(currentSource);
    if (err === 2) {
        showHint("這個重播檔是別的遊戲，或遊戲改過了。請先載入錄影時的那個遊戲", 8000);
        return;
    }
    const sec = Math.round(engine.replayFrames() / 60);
    showHint(`重播中（${sec} 秒）……`, sec * 1000 + 1000);
    clearInterval(replayWatch);
    replayWatch = setInterval(() => {
        if (engine.replayState() === 2) return;   // 還在播
        clearInterval(replayWatch);
        if (engine.replayState() === 3) {
            showHint(engine.replayMatched()
                ? "重播結束：最後的畫面和錄影時完全相同 ✓（現在可以接著玩）"
                : "重播結束：最後的畫面和錄影時不同（遊戲或引擎版本可能不一樣）", 8000);
        }
    }, 250);
}

// 要玩哪一款：網址有 ?game=xxx.lua 就載那個，否則載 game.lua
const params = new URLSearchParams(location.search);
const gameUrl = params.get("game") || "game.lua";
startGame(await fetch(gameUrl).then((r) => r.text()), gameUrl.split("/").pop().replace(/\.lua$/, ""));

// 換遊戲：按「載入遊戲」選檔案，或在電腦上把 .lua 檔拖進網頁。
// .rep 是重播檔：用目前的遊戲播放
function loadFile(file) {
    if (file.name.endsWith(".rep")) {
        file.arrayBuffer().then((buf) => playReplay(new Uint8Array(buf)));
        return;
    }
    file.text().then((src) => {
        if (!startGame(src, file.name.replace(/\.lua$/, ""))) hint.classList.remove("show");
    });
}
const picker = document.getElementById("file-picker");
document.getElementById("btn-load").addEventListener("click", () => picker.click());
document.getElementById("btn-load-touch").addEventListener("click", () => picker.click());
document.getElementById("btn-load-touch").addEventListener("click", () => menu.classList.remove("show"));
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
