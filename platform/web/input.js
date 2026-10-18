// input.js — 網頁上的三種輸入：鍵盤、手把、觸控
// 三種最後都變成同樣格式的 bit，對引擎來說沒有差別。

// 按鍵編號，和 core/input.h 一致
export const BTN = {
    LEFT: 0, RIGHT: 1, UP: 2, DOWN: 3,
    A: 4, B: 5, START: 6, SELECT: 7,
    X: 8, Y: 9,
};

// ============================================================
// 鍵盤
// ============================================================

const KEYMAP = {
    ArrowLeft:  BTN.LEFT,
    ArrowRight: BTN.RIGHT,
    ArrowUp:    BTN.UP,
    ArrowDown:  BTN.DOWN,
    KeyZ:       BTN.A,
    KeyX:       BTN.B,
    KeyA:       BTN.X,
    KeyS:       BTN.Y,
    Enter:      BTN.START,
    ShiftRight: BTN.SELECT,
};

// 正在打字的地方（程式碼編輯器、輸入框）不要被當成遊戲按鍵
function isTyping(el) {
    return el && (el.isContentEditable ||
        el.tagName === "TEXTAREA" || el.tagName === "INPUT" ||
        el.closest?.(".cm-editor"));
}

// 回傳一個函式，呼叫它就得到目前鍵盤的按鍵 bit
export function setupKeyboard() {
    let keys = 0;

    window.addEventListener("keydown", (e) => {
        const b = KEYMAP[e.code];
        if (b === undefined || isTyping(e.target)) return;
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

    return () => keys;
}

// ============================================================
// 手把（Gamepad API）
// ============================================================
// 瀏覽器把認得的手把整理成「標準配置」，按鍵編號固定：
//   0 = 下面那顆（Xbox 的 A）  1 = 右邊（B）  2 = 左邊（X）  3 = 上面（Y）
//   8 = Back/Select  9 = Start  12–15 = 十字鍵 上下左右

const PAD_BUTTONS = [
    [12, BTN.UP], [13, BTN.DOWN], [14, BTN.LEFT], [15, BTN.RIGHT],
    [0, BTN.A], [1, BTN.B], [2, BTN.X], [3, BTN.Y],
    [9, BTN.START], [8, BTN.SELECT],
];
const STICK_DEADZONE = 0.5;   // 搖桿推超過一半才算按方向鍵

export function readGamepad(index) {
    const pad = navigator.getGamepads?.()[index];
    if (!pad) return 0;

    let b = 0;
    for (const [padBtn, btn] of PAD_BUTTONS) {
        if (pad.buttons[padBtn]?.pressed) b |= (1 << btn);
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

// ============================================================
// 觸控
// ============================================================
// 用 pointer 事件（同時支援手指和滑鼠），每根手指有自己的 pointerId，
// 所以可以一手按十字鍵、一手按 A。
//
// getLayout() 回傳 "nes" 或 "modern"，決定左手區怎麼判斷方向：
//   紅白機：十字鍵。手指離中心往哪邊偏就按哪邊，橫、直各自判斷，靠近角落時會斜按
//   現代：類比搖桿。看手指的「角度」切成 8 個方向，每個 45°；圓頭跟著手指移動
export function setupTouch(root, getLayout) {
    let dpadBits = 0;
    const buttonPointers = new Map();   // 哪根手指按著哪個按鍵

    const dpad = root.querySelector("#dpad");
    const knob = root.querySelector("#knob");
    let activePointer = null;

    function updateDpad(e) {
        const r = dpad.getBoundingClientRect();
        const radius = r.width / 2;
        const dx = e.clientX - (r.left + radius);
        const dy = e.clientY - (r.top + r.height / 2);
        let b = 0;

        if (getLayout() === "nes") {
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
            // 圓頭最多移到底座邊緣（圓頭半徑是底座的 0.2 倍）
            const max = radius * 0.6;
            const k = dist > max ? max / dist : 1;
            knob.style.transform = `translate(${dx * k}px, ${dy * k}px)`;
        }
        dpadBits = b;
    }
    function releaseDpad(e) {
        if (e.pointerId !== activePointer) return;
        activePointer = null;
        dpadBits = 0;
        knob.style.transform = "";
        dpad.classList.remove("active");
    }
    dpad.addEventListener("pointerdown", (e) => {
        activePointer = e.pointerId;
        dpad.setPointerCapture(e.pointerId);   // 手指滑出範圍也繼續追蹤
        dpad.classList.add("active");
        updateDpad(e);
    });
    dpad.addEventListener("pointermove", (e) => {
        if (e.pointerId === activePointer) updateDpad(e);
    });
    dpad.addEventListener("pointerup", releaseDpad);
    dpad.addEventListener("pointercancel", releaseDpad);

    // A、B、X、Y、Start、Select：按下就設定，放開就清除
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
        return b;
    };
}
