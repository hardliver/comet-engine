# Comet

Nova One 的遊戲引擎。同一份 C 核心，編譯到三個平台：Mac（開發測試）、網頁（WebAssembly）、Nova One 主機（Pico 2 W）。

## 結構

```
core/              純 C 核心：不碰檔案、視窗、硬體
  gfx.h / gfx.c    畫面陣列（320×240，8-bit 調色盤）與繪圖函式
examples/
  test_scene.c     測試畫面，各平台畫同一張，用來比對結果
platform/
  mac/             把畫面存成 PPM 圖檔
  web/             把畫面畫到瀏覽器的 <canvas>
```

## 執行

```
make            # Mac 版：再執行 ./build/mac，產生 out.ppm
make serve      # 網頁版：瀏覽器開 http://localhost:8000
```

網頁版需要 emscripten（`brew install emscripten`）。

## 狀態

- 畫面格式 320×240、8-bit 調色盤為暫定，規格 v0.1 才凍結
- Mac 版與網頁版輸出逐像素相同
