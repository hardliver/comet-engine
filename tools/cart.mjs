#!/usr/bin/env node
// cart.mjs — 命令列版的遊戲檔合併工具
//
// 用法：node tools/cart.mjs <專案資料夾>
// 讀取專案資料夾裡的 comet.json，把列出的原始碼和圖片合併成一個遊戲檔，
// 寫到 comet.json 的 "output"（沒寫的話是 build/game.lua）。
// 和 Comet Studio 用的是同一份程式（platform/web/cart.js）。

import fs from "fs";
import path from "path";
import zlib from "zlib";
import { buildCart, buildImages } from "../platform/web/cart.js";

const dir = process.argv[2] || ".";
const project = JSON.parse(fs.readFileSync(path.join(dir, "comet.json"), "utf8"));

const files = {};
for (const src of project.sources) {
    files[src] = fs.readFileSync(path.join(dir, src), "utf8");
}

const imagesCode = await buildImages(
    project,
    async (p) => new Uint8Array(fs.readFileSync(path.join(dir, p))),
    async (bytes) => new Uint8Array(zlib.inflateSync(bytes)),
);

const { code, segments } = buildCart(project, files, imagesCode);
const out = path.join(dir, project.output || "build/game.lua");
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, code);
const images = Object.keys(project.images || {}).length;
console.log(`已產生 ${path.relative(process.cwd(), out)}（${segments.length} 個原始碼檔、${images} 張圖片，${(code.length / 1024).toFixed(0)} KB）`);
