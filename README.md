# Comet

Nova One 的遊戲引擎。同一份 C 核心，編譯到三個平台：Mac（開發測試）、網頁（WebAssembly）、Nova One 主機（Pico 2 W）。

## 結構

```
core/              純 C 核心：不碰檔案、視窗、硬體
  engine           一幀的流程：呼叫遊戲的 update、draw
  gfx              畫面陣列（320×240，8-bit 調色盤）與繪圖函式
  input            按鍵狀態：平台寫入、遊戲讀取
examples/
  move_box.c       用方向鍵移動方塊（目前的測試遊戲）
platform/
  mac/             用預設按鍵跑 90 幀，最後一幀存成 PPM（測試用）
  web/             60 fps 遊戲迴圈、鍵盤輸入、畫到 <canvas>
```

## 執行

```
make            # Mac 版：再執行 ./build/mac，產生 out.ppm
make serve      # 網頁版：瀏覽器開 http://localhost:8420
```

網頁版需要 emscripten（`brew install emscripten`）。

## 狀態

- 畫面格式 320×240、8-bit 調色盤為暫定，規格 v0.1 才凍結
- 同一串按鍵，Mac 版與網頁版輸出逐像素相同
- 網頁輸入：鍵盤（方向鍵、Z = A、X = B、Enter = Start、右 Shift = Select）、Gamepad API 手把（最多 4 支）、手機觸控按鈕（直拿、橫拿都支援）
