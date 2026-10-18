// input.js — 網頁上的三種輸入：鍵盤、手把、觸控
//
// 每一種輸入都整理成同樣格式的「輸入狀態」：
//   { buttons: 按鍵 bit, lx, ly: 左搖桿, rx, ry: 右搖桿 }
// 搖桿是 -127～127 的整數，往右、往下是正的。
// 最後用 merge() 把幾種來源合在一起交給引擎，引擎分不出是哪一種。

// 按鍵編號，和 core/input.h 一致
export const BTN = {
    LEFT: 0, RIGHT: 1, UP: 2, DOWN: 3,
    A: 4, B: 5, START: 6, SELECT: 7,
    X: 8, Y: 9, L: 10, R: 11,
};
export const STICK_MAX = 127;

export function emptyState() {
    return { buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0 };
}

// 合併幾種輸入：按鍵用 OR（任一來源按著就算），
// 搖桿取推得最遠的那一個
export function merge(...states) {
    const out = emptyState();
    for (const s of states) {
        out.buttons |= s.buttons;
        if (Math.hypot(s.lx, s.ly) > Math.hypot(out.lx, out.ly)) { out.lx = s.lx; out.ly = s.ly; }
        if (Math.hypot(s.rx, s.ry) > Math.hypot(out.rx, out.ry)) { out.rx = s.rx; out.ry = s.ry; }
    }
    return out;
}

// 把 -1～1 的小數搖桿值換成 -127～127 的整數。
// deadzone：中間這一圈當作 0（實體搖桿放開時不會剛好在正中間）。
// 圓形死區：看整體推的距離，不是 x、y 各自判斷，斜推才不會卡卡的
function toStick(x, y, deadzone) {
    const len = Math.hypot(x, y);
    if (len === 0 || len < deadzone) return [0, 0];
    // 死區外重新縮放到 0～1，推一點點就有一點點，不會突然跳一大格
    const k = Math.min(1, (len - deadzone) / (1 - deadzone)) / len;
    return [Math.round(x * k * STICK_MAX), Math.round(y * k * STICK_MAX)];
}

// ============================================================
// 鍵盤（電腦）
// ============================================================
// 左手 WASD 移動，右手 IJKL 是 A B X Y（排成和手把一樣的菱形）：
//
//        I(Y)                  方向鍵 ↑↓←→ = 右搖桿（瞄準）
//   J(X)    L(B)               Q = L 肩鍵    E = R 肩鍵
//        K(A)                  空白鍵 = A    Enter = Start    Tab = Select

const KEY_BUTTONS = {
    KeyK: BTN.A, Space: BTN.A,
    KeyL: BTN.B,
    KeyJ: BTN.X,
    KeyI: BTN.Y,
    KeyQ: BTN.L,
    KeyE: BTN.R,
    Enter: BTN.START,
    Tab: BTN.SELECT,
};
const KEY_LSTICK = { KeyW: [0, -1], KeyS: [0, 1], KeyA: [-1, 0], KeyD: [1, 0] };
const KEY_RSTICK = { ArrowUp: [0, -1], ArrowDown: [0, 1], ArrowLeft: [-1, 0], ArrowRight: [1, 0] };

// 正在打字的地方（程式碼編輯器、輸入框）不要被當成遊戲按鍵
function isTyping(el) {
    return el && (el.isContentEditable ||
        el.tagName === "TEXTAREA" || el.tagName === "INPUT" ||
        el.closest?.(".cm-editor"));
}

// 回傳一個函式，呼叫它就得到目前鍵盤的輸入狀態
export function setupKeyboard() {
    const down = new Set();   // 目前按著的按鍵

    window.addEventListener("keydown", (e) => {
        const known = e.code in KEY_BUTTONS || e.code in KEY_LSTICK || e.code in KEY_RSTICK;
        if (!known || isTyping(e.target) || e.metaKey || e.ctrlKey) return;
        down.add(e.code);
        e.preventDefault();   // 避免方向鍵捲動網頁、空白鍵按到按鈕、Tab 跳到別的地方
    });
    window.addEventListener("keyup", (e) => down.delete(e.code));
    // 切到別的視窗時，keyup 可能收不到，全部放開比較安全
    window.addEventListener("blur", () => down.clear());

    // 幾個方向鍵合成一個搖桿方向；斜向時長度縮回 1，不會比直走快
    function keysToStick(map) {
        let x = 0, y = 0;
        for (const code of down) {
            if (map[code]) { x += map[code][0]; y += map[code][1]; }
        }
        return toStick(x, y, 0);
    }

    return () => {
        const s = emptyState();
        for (const code of down) {
            if (code in KEY_BUTTONS) s.buttons |= (1 << KEY_BUTTONS[code]);
        }
        [s.lx, s.ly] = keysToStick(KEY_LSTICK);
        [s.rx, s.ry] = keysToStick(KEY_RSTICK);
        return s;
    };
}

// ============================================================
// 手把（Gamepad API）
// ============================================================
// 瀏覽器把認得的手把整理成「標準配置」，編號固定：
//   按鍵 0–3：A（下）B（右）X（左）Y（上）  4、5：LB、RB
//        8：Back/Select  9：Start  12–15：十字鍵 上下左右
//   軸   0、1：左搖桿 x、y   2、3：右搖桿 x、y（-1～1）

const PAD_BUTTONS = [
    [0, BTN.A], [1, BTN.B], [2, BTN.X], [3, BTN.Y],
    [4, BTN.L], [5, BTN.R], [8, BTN.SELECT], [9, BTN.START],
    [12, BTN.UP], [13, BTN.DOWN], [14, BTN.LEFT], [15, BTN.RIGHT],
];
const PAD_DEADZONE = 0.15;

export function readGamepad(index) {
    const s = emptyState();
    const pad = navigator.getGamepads?.()[index];
    if (!pad) return s;

    for (const [padBtn, btn] of PAD_BUTTONS) {
        if (pad.buttons[padBtn]?.pressed) s.buttons |= (1 << btn);
    }
    [s.lx, s.ly] = toStick(pad.axes[0] || 0, pad.axes[1] || 0, PAD_DEADZONE);
    [s.rx, s.ry] = toStick(pad.axes[2] || 0, pad.axes[3] || 0, PAD_DEADZONE);
    return s;
}

// ============================================================
// 觸控（手機）
// ============================================================
// 用 pointer 事件（同時支援手指和滑鼠），每根手指有自己的 pointerId，
// 所以可以同時按住好幾個地方。
//
//   十字鍵（#dpad）：手指離中心往哪邊偏就按哪邊，橫、直各自判斷，靠近角落時會斜按
//   搖桿（#lstick、#rstick）：手指離中心的距離和方向直接換成 -127～127，任何角度都可以
//   其他按鈕（有 data-btn 的元素）：按下就設定，放開就清除

// 讓一個元素追蹤「按在它上面的那根手指」，手指滑出範圍也繼續追蹤
function trackPointer(el, onMove, onRelease) {
    let active = null;
    el.addEventListener("pointerdown", (e) => {
        active = e.pointerId;
        el.setPointerCapture(e.pointerId);
        el.classList.add("active");
        onMove(e);
    });
    el.addEventListener("pointermove", (e) => { if (e.pointerId === active) onMove(e); });
    const release = (e) => {
        if (e.pointerId !== active) return;
        active = null;
        el.classList.remove("active");
        onRelease();
    };
    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
}

// 手指相對元素中心的位置，換算成 -1～1（中心是 0，邊緣是 1）
function offsetFromCenter(el, e) {
    const r = el.getBoundingClientRect();
    return [
        (e.clientX - (r.left + r.width / 2)) / (r.width / 2),
        (e.clientY - (r.top + r.height / 2)) / (r.height / 2),
    ];
}

export function setupTouch(root) {
    const state = emptyState();
    let dpadBits = 0;
    const buttonPointers = new Map();   // 哪根手指按著哪個按鍵（Select、Start、L、R）
    const facePointers = new Map();     // 哪根手指按著 A B X Y 的哪一顆

    // ---- 十字鍵 ----
    const dpad = root.querySelector("#dpad");
    if (dpad) {
        trackPointer(dpad, (e) => {
            const [x, y] = offsetFromCenter(dpad, e);
            const dead = 0.3;   // 中心一小圈不算任何方向
            let b = 0;
            if (x < -dead) b |= (1 << BTN.LEFT);
            if (x >  dead) b |= (1 << BTN.RIGHT);
            if (y < -dead) b |= (1 << BTN.UP);
            if (y >  dead) b |= (1 << BTN.DOWN);
            dpadBits = b;
        }, () => { dpadBits = 0; });
    }

    // 手指位置 → 搖桿值。center 是搖桿中心（螢幕座標），radius 是底座半徑（px）
    function stickValue(e, center, radius) {
        return toStick((e.clientX - center.x) / radius, (e.clientY - center.y) / radius, 0.12);
    }
    function moveKnob(knob, e, center, radius) {
        let dx = e.clientX - center.x;
        let dy = e.clientY - center.y;
        const len = Math.hypot(dx, dy);
        const max = radius * 0.6;   // 圓頭最多移到這麼遠
        if (len > max) { dx *= max / len; dy *= max / len; }
        knob.style.transform = `translate(${dx}px, ${dy}px)`;
    }

    // ---- 左、右搖桿 ----
    // 手指的位置相對搖桿中心，換算成 -127～127；圓頭跟著手指移動
    function setupStick(el, keyX, keyY) {
        if (!el) return;
        const knob = el.querySelector(".knob");
        trackPointer(el, (e) => {
            const r = el.getBoundingClientRect();
            const center = { x: r.left + r.width / 2, y: r.top + r.height / 2 };
            [state[keyX], state[keyY]] = stickValue(e, center, r.width / 2);
            moveKnob(knob, e, center, r.width / 2);
        }, () => {
            state[keyX] = state[keyY] = 0;
            knob.style.transform = "";
        });
    }
    setupStick(root.querySelector("#lstick"), "lx", "ly");
    setupStick(root.querySelector("#rstick"), "rx", "ry");

    // ---- A B X Y：離手指最近的那一顆 ----
    const face = root.querySelector("#face");
    if (face) {
        const rounds = [...face.querySelectorAll("[data-face]")];

        function nearest(e) {
            let best = null, bestDist = Infinity;
            for (const el of rounds) {
                if (el.offsetParent === null) continue;   // 隱藏的（紅白機沒有 X、Y）
                const r = el.getBoundingClientRect();
                const d = Math.hypot(e.clientX - (r.left + r.width / 2), e.clientY - (r.top + r.height / 2));
                if (d < bestDist && d < r.width) { best = el; bestDist = d; }
            }
            return best;
        }
        function refresh() {
            const on = new Set(facePointers.values());
            for (const el of rounds) el.classList.toggle("pressed", on.has(el));
        }
        face.addEventListener("pointerdown", (e) => {
            const el = nearest(e);
            if (!el) return;
            face.setPointerCapture(e.pointerId);
            facePointers.set(e.pointerId, el);
            refresh();
        });
        // 按著滑動：滑到別顆就換成那一顆
        face.addEventListener("pointermove", (e) => {
            if (!facePointers.has(e.pointerId)) return;
            const el = nearest(e);
            if (el && el !== facePointers.get(e.pointerId)) {
                facePointers.set(e.pointerId, el);
                refresh();
            }
        });
        const release = (e) => {
            facePointers.delete(e.pointerId);
            refresh();
        };
        face.addEventListener("pointerup", release);
        face.addEventListener("pointercancel", release);
    }

    // ---- 其他按鈕（L、R、Select、Start）----
    root.querySelectorAll("[data-btn]").forEach((el) => {
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
    });

    return () => {
        let b = dpadBits;
        for (const btn of buttonPointers.values()) b |= (1 << btn);
        for (const el of facePointers.values()) b |= (1 << Number(el.dataset.face));
        return { ...state, buttons: b };
    };
}
