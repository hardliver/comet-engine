// png.js — 讀 PNG 圖片，取出每個像素的 RGBA
//
// 瀏覽器和 Node.js 都要用（編輯器、命令列工具），所以不依賴任何套件。
// PNG 裡的像素資料是用 zlib 壓縮的，解壓縮（inflate）由呼叫的一方提供：
//   瀏覽器：DecompressionStream("deflate")
//   Node.js：zlib.inflateSync
//
// 支援：8 位元的灰階、RGB、RGBA、灰階+透明，以及 1／2／4／8 位元的調色盤圖（Kenney 素材是這種）。
// 不支援交錯式（interlaced）PNG。

export async function decodePNG(bytes, inflate) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10];
    for (let i = 0; i < 8; i++) {
        if (bytes[i] !== SIGNATURE[i]) throw new Error("不是 PNG 檔");
    }

    // ---- 1. 讀出各個區塊（chunk）----
    let width = 0, height = 0, depth = 0, type = 0, interlace = 0;
    let plte = null, trns = null;
    const idat = [];
    for (let p = 8; p < bytes.length;) {
        const len = view.getUint32(p);
        const name = String.fromCharCode(...bytes.subarray(p + 4, p + 8));
        const data = bytes.subarray(p + 8, p + 8 + len);
        if (name === "IHDR") {
            width = view.getUint32(p + 8);
            height = view.getUint32(p + 12);
            depth = data[8];
            type = data[9];
            interlace = data[12];
        } else if (name === "PLTE") plte = data;
        else if (name === "tRNS") trns = data;
        else if (name === "IDAT") idat.push(data);
        else if (name === "IEND") break;
        p += 12 + len;   // 長度 4 + 名稱 4 + 資料 + CRC 4
    }
    if (interlace) throw new Error("不支援交錯式（interlaced）PNG，請另存為一般 PNG");

    // ---- 2. 解壓縮 ----
    const total = idat.reduce((n, d) => n + d.length, 0);
    const joined = new Uint8Array(total);
    let off = 0;
    for (const d of idat) { joined.set(d, off); off += d.length; }
    const raw = await inflate(joined);

    // ---- 3. 還原每一列（PNG 每列前面有一個「過濾方式」byte）----
    const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[type];
    if (!channels) throw new Error(`不支援的 PNG 顏色格式 ${type}`);
    const bitsPerPixel = channels * depth;
    const bpp = Math.max(1, bitsPerPixel >> 3);           // 過濾時「左邊那個像素」差幾個 byte
    const stride = Math.ceil(width * bitsPerPixel / 8);   // 每列幾個 byte
    const pixels = new Uint8Array(stride * height);
    let prev = new Uint8Array(stride);
    for (let y = 0; y < height; y++) {
        const filter = raw[y * (stride + 1)];
        const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
        const cur = pixels.subarray(y * stride, (y + 1) * stride);
        for (let x = 0; x < stride; x++) {
            const a = x >= bpp ? cur[x - bpp] : 0;            // 左
            const b = prev[x];                                // 上
            const c = x >= bpp ? prev[x - bpp] : 0;           // 左上
            let v = line[x];
            if (filter === 1) v += a;
            else if (filter === 2) v += b;
            else if (filter === 3) v += (a + b) >> 1;
            else if (filter === 4) {                          // Paeth
                const p = a + b - c;
                const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            cur[x] = v & 0xff;
        }
        prev = cur;
    }

    // ---- 4. 轉成 RGBA ----
    const rgba = new Uint8Array(width * height * 4);
    const sample = (row, x) => {
        if (depth === 8) return row[x * channels];
        const perByte = 8 / depth;                           // 1／2／4 位元：一個 byte 裝好幾個像素
        const byte = row[Math.floor(x / perByte)];
        const shift = 8 - depth * (x % perByte + 1);
        return (byte >> shift) & ((1 << depth) - 1);
    };
    for (let y = 0; y < height; y++) {
        const row = pixels.subarray(y * stride, (y + 1) * stride);
        for (let x = 0; x < width; x++) {
            const o = (y * width + x) * 4;
            if (type === 3) {                                // 調色盤
                const i = sample(row, x);
                rgba[o] = plte[i * 3]; rgba[o + 1] = plte[i * 3 + 1]; rgba[o + 2] = plte[i * 3 + 2];
                rgba[o + 3] = trns && i < trns.length ? trns[i] : 255;
            } else if (type === 0 || type === 4) {           // 灰階
                const g = sample(row, x);
                rgba[o] = rgba[o + 1] = rgba[o + 2] = g;
                rgba[o + 3] = type === 4 ? row[x * 2 + 1] : 255;
            } else {                                         // RGB、RGBA
                rgba[o] = row[x * channels]; rgba[o + 1] = row[x * channels + 1]; rgba[o + 2] = row[x * channels + 2];
                rgba[o + 3] = type === 6 ? row[x * 4 + 3] : 255;
            }
        }
    }
    return { width, height, rgba };
}
