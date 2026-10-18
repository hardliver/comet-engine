# vendor

`codemirror.js`：編輯器用的 [CodeMirror 6](https://codemirror.net/)（MIT 授權），打包成單一檔案，
這樣編輯器不需要網路、也不需要安裝 Node 套件就能用。授權聲明在檔案最後。

重新打包（升級 CodeMirror 時才需要）：

```
cd tools/editor
npm install
npm run build
```
