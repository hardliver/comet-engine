# 用法（在 comet 資料夾裡）：
#   make         編譯 Mac 版，執行 ./build/mac [遊戲.lua] 產生 out.ppm
#   make web     編譯網頁版到 build/web/
#   make serve   在 http://localhost:8420 開啟網頁版（編輯器：/studio.html）
#                （換埠號：make serve PORT=9000）
#   make check   自動測試：網頁版與 Mac 版結果一致（含中文字、地圖、重播檔、存檔、系統卡匣、資源包）、Lua 錯誤處理正常
#   make clean   清除編譯結果
#
# -Wall -Wextra：開啟警告，讓編譯器幫你抓可疑的寫法

CFLAGS = -Wall -Wextra -std=c99 -O2

# 核心
CORE = core/gfx.c core/map.c core/input.c core/engine.c core/script.c core/sha256.c core/replay.c core/save.c core/system.c core/pak.c
HEADERS = $(wildcard core/*.h)

# Lua 5.4（third_party/lua，原始碼未修改）
# LUA_USER_H：讓 Lua 讀入 Comet 的設定（固定雜湊種子、輸出導向平台）
LUA_SRC = $(wildcard third_party/lua/*.c)
LUA_FLAGS = -Ithird_party/lua -Icore -DLUA_USER_H='"comet_lua_user.h"'

SHARED = $(CORE) $(LUA_SRC)

# 網頁版要讓 JavaScript 呼叫的 C 函式（名稱前面要加底線）
# malloc、free：JavaScript 要把 Lua 原始碼放進 wasm 的記憶體
WEB_EXPORTS = _engine_init,_engine_load,_engine_check,_engine_check_error,_engine_frame,_engine_frame_count,_input_set,_input_set_stick,_gfx_framebuffer,_gfx_palette,_engine_set_seed,_engine_seed,_engine_cart_sha,_replay_record_begin,_replay_snapshot,_replay_snapshot_size,_replay_play_begin,_replay_stop,_replay_state,_replay_frames,_replay_matched,_engine_load_system,_system_clear_games,_system_add_game,_system_set_info,_system_request,_system_request_game,_system_request_done,_pak_set,_malloc,_free

# 網頁伺服器的埠號；?= 代表指令列沒有指定時才用這個值
PORT ?= 8420

build/mac: $(SHARED) $(HEADERS) platform/mac/main.c
	mkdir -p build
	cc $(CFLAGS) $(LUA_FLAGS) $(SHARED) platform/mac/main.c -o build/mac -lm

# --no-entry：沒有 main()，由 JavaScript 決定呼叫哪個函式
# STANDALONE_WASM：只產生 .wasm，不產生 emscripten 的 JS 膠水程式
# SUPPORT_LONGJMP=wasm：Lua 處理錯誤時會「跳回」pcall 的位置（setjmp/longjmp），
#                       用 WebAssembly 內建的例外處理來做，不需要 JavaScript 幫忙
# ALLOW_MEMORY_GROWTH：記憶體不夠時自動變大（預設固定 16 MB；大遊戲的資源包放不下）。
#                       JavaScript 每次都重新取 memory.buffer，所以記憶體換一塊也沒關係
# ERROR_ON_UNDEFINED_SYMBOLS=0：js_log 由 JavaScript 提供，C 裡找不到是正常的。
#                       其他找不到的函式仍會列出警告，要看一下是不是打錯字
web: $(SHARED) $(HEADERS) $(wildcard platform/web/*) $(wildcard games/*.lua) docs/AI_GUIDE.md fonts/c11.bin
	mkdir -p build/web
	emcc $(CFLAGS) $(LUA_FLAGS) $(SHARED) platform/web/web.c -o build/web/comet.wasm \
		--no-entry -sSTANDALONE_WASM -sSUPPORT_LONGJMP=wasm \
		-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1gb \
		-sERROR_ON_UNDEFINED_SYMBOLS=0 -sWARN_ON_UNDEFINED_SYMBOLS=1 \
		-sEXPORTED_FUNCTIONS=$(WEB_EXPORTS)
	mkdir -p build/web/vendor build/web/games build/web/fonts
	cp platform/web/*.html platform/web/*.js platform/web/*.css platform/web/*.webmanifest build/web/
	cp platform/web/vendor/*.js build/web/vendor/
	cp docs/AI_GUIDE.md build/web/
	cp fonts/c11.bin build/web/fonts/
	cp games/*.lua build/web/games/
	cp games/move_box.lua build/web/game.lua

serve: web
	@echo "電腦：http://localhost:$(PORT)"
	@for i in en0 en1 en2; do ip=$$(ipconfig getifaddr $$i 2>/dev/null) && echo "手機：http://$$ip:$(PORT)"; done; true
	python3 tools/serve.py $(PORT) build/web

# 文字測試用的遊戲檔：要經過合併工具才會有字型
build/text_test.lua: tests/text/comet.json tests/text/main.lua fonts/c11.bin platform/web/font.js platform/web/cart.js
	node tools/cart.mjs tests/text

# 地圖測試用的遊戲檔
build/map_test.lua: tests/map/comet.json tests/map/main.lua tests/map/maps/test.json tests/map/town.png tests/map/dungeon.png platform/web/cart.js
	node tools/cart.mjs tests/map

# 資源包測試用的 .pak
build/pak_test.pak: tests/pak/make_pak.mjs tools/pak.mjs
	node tests/pak/make_pak.mjs

check: build/mac web build/text_test.lua build/map_test.lua build/pak_test.pak tests/save/main.lua tests/pak/main.lua $(wildcard tests/system/*.lua)
	./build/mac
	./build/mac games/move_box.lua build/record.ppm --record build/move_box.rep
	./build/mac games/move_box.lua build/replay.ppm --replay build/move_box.rep
	./build/mac build/text_test.lua build/text_test.ppm
	./build/mac build/map_test.lua build/map_test.ppm
	rm -rf build/saves && mkdir -p build/saves
	./build/mac tests/save/main.lua build/save1.ppm --saves build/saves
	./build/mac tests/save/main.lua build/save2.ppm --saves build/saves --record build/save.rep
	./build/mac tests/save/main.lua build/save3.ppm --replay build/save.rep
	./build/mac tests/system/system.lua build/system.ppm --system \
		--game test/a tests/system/game_a.lua --game test/b tests/system/game_b.lua
	./build/mac tests/pak/main.lua build/pak_test.ppm --pak build/pak_test.pak --record build/pak.rep
	./build/mac tests/pak/main.lua build/pak_replay.ppm --pak build/pak_test.pak --replay build/pak.rep
	node tools/check_web.mjs

clean:
	rm -rf build out.ppm

.PHONY: web serve check clean
