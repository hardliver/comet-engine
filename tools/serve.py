#!/usr/bin/env python3
"""serve.py：開發用的網頁伺服器（make serve 用）

用法：python3 tools/serve.py 埠號 資料夾

和 python3 -m http.server 一樣，但每個回應都加上「不要快取」。
內建的伺服器沒有說明能不能快取，瀏覽器就會自己決定沿用舊的 JS，
結果新的 studio.html 配上舊的 studio.js，頁面就壞了。
開發時檔案隨時在改，每次都拿最新的才不會被這種問題困擾。
"""

import functools
import http.server
import sys


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8420
    folder = sys.argv[2] if len(sys.argv) > 2 else "."
    handler = functools.partial(NoCacheHandler, directory=folder)
    with http.server.ThreadingHTTPServer(("0.0.0.0", port), handler) as server:
        print(f"Serving {folder} on port {port}（不快取）")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
