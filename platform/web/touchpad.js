// touchpad.js — 手機的虛擬手把：位置、外觀、自訂
//
// 按鈕「浮」在遊戲畫面上（半透明），所以可以照實體手把的比例排列。
// 每個按鈕的位置用「畫面的幾分之幾」記錄，大小用 vmin（螢幕短邊的百分之幾），
// 這樣換手機、換方向，按鈕的相對位置都一樣。
//
// 三種預設配置：
//   Xbox（預設）：左搖桿在左上、十字鍵在它右下；ABXY 在右上、右搖桿在它左下
//                A 在下、B 在右、X 在左、Y 在上
//   Switch：     位置和 Xbox 相同，但 A 在右、B 在下、X 在上、Y 在左（Switch 手把的排法）
//   紅白機：     只有十字鍵和 B A
//
// 使用者可以在選單裡「調整按鍵位置」：拖曳移動、放大縮小，存在瀏覽器裡。
// 直拿、橫拿各自有一套設定。
//
// 判斷按下哪一顆的程式在 input.js，這裡只管放在哪裡、長什麼樣子。

// 每個控制項：x、y 是中心點在畫面上的位置（0～1），s 是大小（vmin）
const LANDSCAPE = {
    lstick: { x: 0.13, y: 0.40, s: 34 },
    dpad:   { x: 0.27, y: 0.74, s: 28 },
    face:   { x: 0.86, y: 0.38, s: 46 },
    rstick: { x: 0.72, y: 0.74, s: 28 },
    L:      { x: 0.08, y: 0.08, s: 16 },
    R:      { x: 0.92, y: 0.08, s: 16 },
    select: { x: 0.40, y: 0.92, s: 15 },
    start:  { x: 0.60, y: 0.92, s: 15 },
};
const PORTRAIT = {
    lstick: { x: 0.22, y: 0.63, s: 34 },
    dpad:   { x: 0.36, y: 0.83, s: 26 },
    face:   { x: 0.77, y: 0.63, s: 42 },
    rstick: { x: 0.63, y: 0.84, s: 26 },
    L:      { x: 0.12, y: 0.50, s: 18 },
    R:      { x: 0.88, y: 0.50, s: 18 },
    select: { x: 0.38, y: 0.95, s: 17 },
    start:  { x: 0.62, y: 0.95, s: 17 },
};
// 紅白機：十字鍵放在左搖桿的位置，B A 放大
const NES_OVERRIDES = {
    landscape: { dpad: { x: 0.15, y: 0.62, s: 42 }, face: { x: 0.85, y: 0.62, s: 44 } },
    portrait:  { dpad: { x: 0.25, y: 0.70, s: 42 }, face: { x: 0.75, y: 0.70, s: 44 } },
};
// 紅白機沒有的控制項
const NES_HIDDEN = ["lstick", "rstick", "L", "R"];

export const PRESETS = { xbox: "Xbox", switch: "Switch", nes: "紅白機" };

const store = {
    get(key, fallback) {
        try { const v = localStorage.getItem(key); return v === null ? fallback : JSON.parse(v); }
        catch (e) { return fallback; }
    },
    set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {} },
    remove(key) { try { localStorage.removeItem(key); } catch (e) {} },
};

export function setupTouchpad() {
    const layer = document.getElementById("pad-layer");
    const controls = [...layer.querySelectorAll("[data-ctl]")];

    let preset = store.get("comet.pad.preset", "xbox");
    let screenMode = store.get("comet.pad.screen", "fit");     // fit：原比例　stretch：拉寬
    let opacity = store.get("comet.pad.opacity", 0.5);
    let editing = false;
    let selected = null;

    const orientation = () => (innerWidth > innerHeight ? "landscape" : "portrait");
    const key = () => `comet.pad.layout.${preset}.${orientation()}`;

    function defaults() {
        const base = orientation() === "landscape" ? LANDSCAPE : PORTRAIT;
        const layout = structuredClone(base);
        if (preset === "nes") Object.assign(layout, structuredClone(NES_OVERRIDES[orientation()]));
        return layout;
    }
    let layout = null;
    function load() { layout = { ...defaults(), ...store.get(key(), {}) }; }

    // 把每個控制項放到它的位置
    function render() {
        const vmin = Math.min(innerWidth, innerHeight) / 100;
        for (const el of controls) {
            const name = el.dataset.ctl;
            const c = layout[name];
            const hidden = preset === "nes" && NES_HIDDEN.includes(name);
            el.style.display = hidden ? "none" : "";
            if (hidden) continue;

            const size = c.s * vmin;
            const isPill = el.classList.contains("pill");
            const w = size;
            const h = isPill ? size * 0.42 : (name === "face" && preset === "nes" ? size * 0.55 : size);
            el.style.width = `${w}px`;
            el.style.height = `${h}px`;
            el.style.left = `${c.x * innerWidth - w / 2}px`;
            el.style.top = `${c.y * innerHeight - h / 2}px`;
            if (isPill) el.style.fontSize = `${h * 0.42}px`;
        }
        document.body.dataset.preset = preset;
        document.body.dataset.screen = screenMode;
        document.body.classList.toggle("editing", editing);
        layer.style.setProperty("--pad-opacity", editing ? 0.85 : opacity);
        for (const el of controls) el.classList.toggle("selected", el === selected);
    }

    load();
    render();
    addEventListener("resize", () => { load(); render(); });

    // ---- 調整按鍵位置 ----
    // 編輯中：在控制項上按下就選取並拖曳；其他輸入處理一律攔下來，不會傳給遊戲
    let drag = null;
    layer.addEventListener("pointerdown", (e) => {
        if (!editing) return;
        e.stopImmediatePropagation();
        e.preventDefault();
        const el = e.target.closest("[data-ctl]");
        selected = el || null;
        if (el) {
            const c = layout[el.dataset.ctl];
            drag = { id: e.pointerId, name: el.dataset.ctl, sx: e.clientX, sy: e.clientY, x: c.x, y: c.y };
            layer.setPointerCapture(e.pointerId);
        }
        render();
    }, true);
    layer.addEventListener("pointermove", (e) => {
        if (!editing || !drag || e.pointerId !== drag.id) return;
        e.stopImmediatePropagation();
        const c = layout[drag.name];
        c.x = Math.min(1, Math.max(0, drag.x + (e.clientX - drag.sx) / innerWidth));
        c.y = Math.min(1, Math.max(0, drag.y + (e.clientY - drag.sy) / innerHeight));
        render();
    }, true);
    const endDrag = (e) => { if (editing) { e.stopImmediatePropagation(); drag = null; } };
    layer.addEventListener("pointerup", endDrag, true);
    layer.addEventListener("pointercancel", endDrag, true);

    function resizeSelected(factor) {
        if (!selected) return;
        const c = layout[selected.dataset.ctl];
        c.s = Math.min(70, Math.max(8, Math.round(c.s * factor)));
        render();
    }

    return {
        isEditing: () => editing,
        preset: () => preset,
        screenMode: () => screenMode,
        opacity: () => opacity,

        setPreset(p) { preset = p; store.set("comet.pad.preset", p); load(); render(); },
        setScreenMode(m) { screenMode = m; store.set("comet.pad.screen", m); render(); },
        setOpacity(v) { opacity = v; store.set("comet.pad.opacity", v); render(); },

        startEditing() { editing = true; selected = null; render(); },
        finishEditing() { editing = false; selected = null; store.set(key(), layout); render(); },
        bigger() { resizeSelected(1.12); },
        smaller() { resizeSelected(1 / 1.12); },
        reset() { store.remove(key()); load(); selected = null; render(); },
    };
}
