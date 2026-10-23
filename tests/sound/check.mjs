// check.mjs：檢查 Mac 版存的 build/sound.wav（make check 用；不需要網頁版）
//   1. 格式：22050 Hz、16 位元、單聲道，480 幀 = 176400 個取樣（每兩幀 735 個）
//   2. 前半秒（第 0～30 幀，blip 還沒開始）只有 A4 三角波：過零 440 次 = 每秒 880 次 = 440 Hz（音高表算得對）
//   3. sfx(nil, 聲道) 停掉之後、music() 停掉之後，真的沒有聲音
//   4. 每一段都有聲音（音效、雜訊、滑音、循環、音樂）
import fs from "fs";

const buf = fs.readFileSync("build/sound.wav");
const problems = [];
const rate = buf.readUInt32LE(24), bits = buf.readUInt16LE(34), chans = buf.readUInt16LE(22);
if (rate !== 22050 || bits !== 16 || chans !== 1) problems.push(`格式不對：${rate} Hz、${bits} 位元、${chans} 聲道`);
const n = buf.readUInt32LE(40) / 2;
if (n !== 176400) problems.push(`取樣數 ${n}，應該是 176400（480 幀）`);
const s = (i) => buf.readInt16LE(44 + i * 2);
const at = (f) => Math.floor(f * 367.5);   // 第幾幀開始的取樣位置

let zc = 0;
for (let i = 1; i < 11025; i++) if ((s(i - 1) < 0) !== (s(i) < 0)) zc++;
if (zc !== 440) problems.push(`前半秒過零 ${zc} 次，應該是 440（440 Hz）`);

const rms = (a, b) => {
    let sum = 0;
    for (let i = at(a); i < at(b); i++) sum += s(i) * s(i);
    return Math.sqrt(sum / (at(b) - at(a)));
};
if (rms(202, 210) !== 0) problems.push("sfx(nil, 聲道) 停掉之後還有聲音");
if (rms(405, 480) !== 0) problems.push("music() 停掉之後還有聲音");
for (const [name, a, b] of [["a440", 0, 30], ["blip", 30, 36], ["boom", 90, 100], ["slide", 120, 150], ["hum", 150, 200], ["music", 220, 400]]) {
    if (rms(a, b) < 300) problems.push(`${name}（第 ${a}～${b} 幀）沒有聲音`);
}

if (problems.length) {
    console.log("✗ 聲音：" + problems.join("；"));
    process.exit(1);
}
console.log("✓ 聲音：A4 = 440 Hz、停止後安靜、每段都有聲音");
