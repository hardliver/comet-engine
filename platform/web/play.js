// play.js — 遊玩頁（index.html）
// 引擎和輸入的細節在 comet.js、input.js；這裡只負責遊玩頁的畫面和按鈕。

import { createEngine, startLoop, createAudio } from "./comet.js";
import { setupKeyboard, setupTouch, readGamepad, connectedPads, merge } from "./input.js";
import { setupTouchpad } from "./touchpad.js";
import { setupPadMenu } from "./padmenu.js";

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
// 位置、配置、透明度、自訂都在 touchpad.js；選單按鈕在 padmenu.js
const pad = setupTouchpad();
const $ = (id) => document.getElementById(id);

const { menu } = setupPadMenu(pad, showHint);   // 選單、調整按鍵位置、全螢幕（padmenu.js）

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

// ---- 聲音（core/audio.h）----
// 瀏覽器規定使用者點過、按過鍵之後才能出聲：第一次點畫面或按鍵時才真的打開
const audio = createAudio();
engine.setAudio(audio);
for (const ev of ["pointerdown", "keydown", "touchstart"]) window.addEventListener(ev, () => audio.start(), { capture: true });
function toggleSound() {
    audio.setMuted(!audio.muted);
    for (const id of ["btn-sound", "btn-sound-touch"]) document.getElementById(id).textContent = audio.muted ? "🔇 靜音" : "🔊 聲音";
    showHint(audio.muted ? "聲音關了（再按一次打開）" : "聲音開了");
}
document.getElementById("btn-sound").addEventListener("click", toggleSound);
document.getElementById("btn-sound-touch").addEventListener("click", toggleSound);
window.addEventListener("keydown", (e) => { if (e.key === "m" || e.key === "M") toggleSound(); });

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
// 資源包：遊戲旁邊有同名的 .pak 就一起載入（game.lua → game.pak），沒有也沒關係
const pakRes = await fetch(gameUrl.replace(/\.lua$/, "") + ".pak").catch(() => null);
if (pakRes && pakRes.ok) engine.setPak(new Uint8Array(await pakRes.arrayBuffer()));
startGame(await fetch(gameUrl).then((r) => r.text()), gameUrl.split("/").pop().replace(/\.lua$/, ""));

// 換遊戲：按「載入遊戲」選檔案，或在電腦上把 .lua 檔拖進網頁。
// .rep 是重播檔：用目前的遊戲播放。
// .pak 是資源包：可以和 .lua 一起選（或一起拖進來）；只給 .pak 就換掉目前遊戲的資源包、重新開始
async function loadFiles(files) {
    files = [...files];
    const rep = files.find((f) => f.name.endsWith(".rep"));
    const pak = files.find((f) => f.name.endsWith(".pak"));
    const lua = files.find((f) => f.name.endsWith(".lua"));
    if (rep && !lua && !pak) {
        playReplay(new Uint8Array(await rep.arrayBuffer()));
        return;
    }
    if (lua && !pak) engine.setPak(null);   // 換成另一個遊戲：上一個的資源包不能留著
    if (pak && engine.setPak(new Uint8Array(await pak.arrayBuffer()))) {
        showHint("資源包格式不對", 5000);
        return;
    }
    if (lua) {
        if (!startGame(await lua.text(), lua.name.replace(/\.lua$/, ""))) hint.classList.remove("show");
    } else if (pak && currentSource) {
        if (!startGame(currentSource, currentName)) hint.classList.remove("show");
    }
}
const picker = document.getElementById("file-picker");
document.getElementById("btn-load").addEventListener("click", () => picker.click());
document.getElementById("btn-load-touch").addEventListener("click", () => picker.click());
document.getElementById("btn-load-touch").addEventListener("click", () => menu.classList.remove("show"));
picker.addEventListener("change", () => {
    if (picker.files.length) loadFiles(picker.files);
    picker.value = "";
});
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
    e.preventDefault();
    if (e.dataTransfer.files.length) loadFiles(e.dataTransfer.files);
});

startLoop(engine, document.getElementById("screen"), getInput);
