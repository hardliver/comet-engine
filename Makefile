# 用法（在 comet 資料夾裡）：
#   make         編譯 Mac 版，執行 ./build/mac 產生 out.ppm
#   make web     編譯網頁版到 build/web/
#   make serve   在 http://localhost:8420 開啟網頁版
#                （換埠號：make serve PORT=9000）
#   make clean   清除編譯結果
#
# -Wall -Wextra：開啟警告，讓編譯器幫你抓可疑的寫法

CFLAGS = -Wall -Wextra -std=c99 -O2

# 所有平台共用的 C 檔：核心 + 遊戲
CORE = core/gfx.c core/input.c core/engine.c
GAME = examples/move_box.c
SHARED = $(CORE) $(GAME)
HEADERS = core/gfx.h core/input.h core/engine.h

# 網頁版要讓 JavaScript 呼叫的 C 函式（名稱前面要加底線）
WEB_EXPORTS = _engine_init,_engine_frame,_input_set,_gfx_framebuffer,_gfx_palette

# 網頁伺服器的埠號；?= 代表指令列沒有指定時才用這個值
PORT ?= 8420

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
	@echo "電腦：http://localhost:$(PORT)"
	@for i in en0 en1 en2; do ip=$$(ipconfig getifaddr $$i 2>/dev/null) && echo "手機：http://$$ip:$(PORT)"; done; true
	python3 -m http.server $(PORT) --bind 0.0.0.0 --directory build/web

clean:
	rm -rf build out.ppm

.PHONY: web serve clean
