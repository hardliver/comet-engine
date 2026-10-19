// padmenu.js — 手機虛擬手把的設定選單：配置（Xbox／Switch／紅白機）、畫面比例、透明度、
// 調整按鍵位置、全螢幕。Comet 遊玩頁和網頁版 Nova 共用。
//
// 用法：const { menu } = setupPadMenu(setupTouchpad(), showHint);
// 網頁上要有這些元素（id）：btn-menu、menu、seg-preset、seg-screen、opacity、btn-edit、
// btn-fullscreen、btn-close-menu，以及 edit-bar 裡的 btn-smaller、btn-bigger、btn-reset、btn-done
// （HTML 照抄 index.html 的「選單」和「調整按鍵位置」兩段）

export function setupPadMenu(pad, showHint) {
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

    const fsButton = $("btn-fullscreen");
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

    return { menu };
}
