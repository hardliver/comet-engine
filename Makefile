# 用法（在 comet 資料夾裡）：
#   make         編譯 Mac 版，執行 ./build/mac 產生 out.ppm
#   make web     編譯網頁版到 build/web/
#   make serve   在 http://localhost:8000 開啟網頁版
#   make clean   清除編譯結果
#
# -Wall -Wextra：開啟警告，讓編譯器幫你抓可疑的寫法

CFLAGS = -Wall -Wextra -std=c99 -O2

# 所有平台共用的 C 檔：核心 + 測試畫面
SHARED = core/gfx.c examples/test_scene.c
HEADERS = core/gfx.h examples/test_scene.h

# 網頁版要讓 JavaScript 呼叫的 C 函式（名稱前面要加底線）
WEB_EXPORTS = _test_scene_draw,_gfx_framebuffer,_gfx_palette

build/mac: $(SHARED) $(HEADERS) platform/mac/main.c
	mkdir -p build
	cc $(CFLAGS) $(SHARED) platform/mac/main.c -o build/mac

# --no-entry：沒有 main()，由 JavaScript 決定呼叫哪個函式
# STANDALONE_WASM：只產生 .wasm，不產生 emscripten 的 JS 膠水程式
web: $(SHARED) $(HEADERS) platform/web/index.html platform/web/main.js
	mkdir -p build/web
	emcc $(CFLAGS) $(SHARED) -o build/web/comet.wasm \
		--no-entry -sSTANDALONE_WASM \
		-sEXPORTED_FUNCTIONS=$(WEB_EXPORTS)
	cp platform/web/index.html platform/web/main.js build/web/

serve: web
	python3 -m http.server 8000 --directory build/web

clean:
	rm -rf build out.ppm

.PHONY: web serve clean
