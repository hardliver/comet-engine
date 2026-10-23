# embed_cart.py — 把卡匣（.lua）變成 C 陣列，編譯進韌體
# 用法：python3 embed_cart.py 卡匣.lua 輸出.c
# 之後卡匣改從 SD 卡讀，就不需要這個了
import sys

src = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('// 自動產生，不要手改（來源：%s）\n' % sys.argv[1])
    # const：陣列放在 Flash，不占 RAM
    f.write('const char cart_src[] = {\n')
    for i in range(0, len(src), 24):
        f.write(''.join('%d,' % b for b in src[i:i + 24]) + '\n')
    f.write('0};\n')
    f.write('const unsigned cart_len = %d;\n' % len(src))
