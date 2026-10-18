# 用法：在 engine 資料夾裡輸入 make，再輸入 ./build/mac
# -Wall -Wextra：開啟警告，讓編譯器幫你抓可疑的寫法

CFLAGS = -Wall -Wextra -std=c99 -O2

build/mac: core/gfx.c platform/mac/main.c core/gfx.h
	mkdir -p build
	cc $(CFLAGS) core/gfx.c platform/mac/main.c -o build/mac

clean:
	rm -rf build out.ppm
