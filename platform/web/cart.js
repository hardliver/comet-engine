// cart.js — 把一個專案的多個原始碼檔合併成一個遊戲檔（cart）
//
// Comet 規定一款遊戲是一個 .lua 檔；大遊戲拆成好幾個檔案比較好開發。
// 專案資料夾裡放一個 comet.json，寫清楚要合併哪些檔案、依什麼順序：
//
//   {
//     "name": "Wildkin 野伴",
//     "id": "hardliver/wildkin",
//     "sources": ["src/util.lua", "src/battle.lua", "src/main.lua"],
//     "output": "build/wildkin.lua"
//   }
//
// 這個檔案在瀏覽器（Comet Studio）和命令列（tools/cart.mjs）共用，
// 兩邊合併出來的結果保證一模一樣。

// project：comet.json 的內容
// files：{ "src/main.lua": "原始碼文字", ... }
// 回傳 { code: 合併後的遊戲檔, segments: 每個來源檔在遊戲檔裡的行號範圍 }
export function buildCart(project, files) {
    const lines = [
        `-- ${project.name || "Comet game"}`,
        ...(project.id ? [`-- id: ${project.id}`] : []),
        "-- 這個檔案是自動合併產生的，請修改原始碼檔，不要直接改這裡",
    ];
    const segments = [];

    for (const path of project.sources) {
        if (!(path in files)) throw new Error(`comet.json 列了 ${path}，但找不到這個檔案`);
        lines.push("", `-- ==================== ${path} ====================`);
        const body = files[path].replace(/\r\n/g, "\n").replace(/\n$/, "").split("\n");
        const start = lines.length + 1;   // 這個檔案第 1 行在遊戲檔的第幾行（從 1 開始數）
        lines.push(...body);
        segments.push({ path, start, end: start + body.length - 1 });
    }
    return { code: lines.join("\n") + "\n", segments };
}

// 遊戲檔的第 n 行 → 原本是哪個檔案的第幾行（錯誤訊息用）
export function mapLine(segments, n) {
    for (const s of segments) {
        if (n >= s.start && n <= s.end) return { path: s.path, line: n - s.start + 1 };
    }
    return null;
}
