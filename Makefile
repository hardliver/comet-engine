# 用法（在 comet 資料夾裡）：
#   make         編譯 Mac 版，執行 ./build/mac [遊戲.lua] 產生 out.ppm
#   make web     編譯網頁版到 build/web/
#   make serve   在 http://localhost:8420 開啟網頁版
#                （換埠號：make serve PORT=9000）
#   make check   自動測試：網頁版與 Mac 版結果一致、Lua 錯誤處理正常
#   make clean   清除編譯結果
#
# -Wall -Wextra：開啟警告，讓編譯器幫你抓可疑的寫法

CFLAGS = -Wall -Wextra -std=c99 -O2

# 核心
CORE = core/gfx.c core/input.c core/engine.c core/script.c
HEADERS = $(wildcard core/*.h)

# Lua 5.4（third_party/lua，原始碼未修改）
# LUA_USER_H：讓 Lua 讀入 Comet 的設定（固定雜湊種子、輸出導向平台）
LUA_SRC = $(wildcard third_party/lua/*.c)
LUA_FLAGS = -Ithird_party/lua -Icore -DLUA_USER_H='"comet_lua_user.h"'

SHARED = $(CORE) $(LUA_SRC)

# 網頁版要讓 JavaScript 呼叫的 C 函式（名稱前面要加底線）
# malloc、free：JavaScript 要把 Lua 原始碼放進 wasm 的記憶體
WEB_EXPORTS = _engine_init,_engine_load,_engine_frame,_input_set,_gfx_framebuffer,_gfx_palette,_malloc,_free

# 網頁伺服器的埠號；?= 代表指令列沒有指定時才用這個值
PORT ?= 8420

build/mac: $(SHARED) $(HEADERS) platform/mac/main.c
	mkdir -p build
	cc $(CFLAGS) $(LUA_FLAGS) $(SHARED) platform/mac/main.c -o build/mac -lm

# --no-entry：沒有 main()，由 JavaScript 決定呼叫哪個函式
# STANDALONE_WASM：只產生 .wasm，不產生 emscripten 的 JS 膠水程式
# SUPPORT_LONGJMP=wasm：Lua 處理錯誤時會「跳回」pcall 的位置（setjmp/longjmp），
#                       用 WebAssembly 內建的例外處理來做，不需要 JavaScript 幫忙
# ERROR_ON_UNDEFINED_SYMBOLS=0：js_log 由 JavaScript 提供，C 裡找不到是正常的。
#                       其他找不到的函式仍會列出警告，要看一下是不是打錯字
web: $(SHARED) $(HEADERS) platform/web/web.c $(wildcard platform/web/*) games/move_box.lua
	mkdir -p build/web
	emcc $(CFLAGS) $(LUA_FLAGS) $(SHARED) platform/web/web.c -o build/web/comet.wasm \
		--no-entry -sSTANDALONE_WASM -sSUPPORT_LONGJMP=wasm \
		-sERROR_ON_UNDEFINED_SYMBOLS=0 -sWARN_ON_UNDEFINED_SYMBOLS=1 \
		-sEXPORTED_FUNCTIONS=$(WEB_EXPORTS)
	cp platform/web/index.html platform/web/main.js platform/web/manifest.webmanifest build/web/
	cp games/move_box.lua build/web/game.lua

serve: web
	@echo "電腦：http://localhost:$(PORT)"
	@for i in en0 en1 en2; do ip=$$(ipconfig getifaddr $$i 2>/dev/null) && echo "手機：http://$$ip:$(PORT)"; done; true
	python3 -m http.server $(PORT) --bind 0.0.0.0 --directory build/web

check: build/mac web
	./build/mac
	node tools/check_web.mjs

clean:
	rm -rf build out.ppm

.PHONY: web serve check clean
