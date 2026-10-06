# AtomScript for VS Code

AtomScript（`.ats`）の編集支援。構文解析と検査はすべてコンパイラ `atsc`（言語サーバー `atsc lsp`）が行う。

## できること

- 構文ハイライト、スニペット（`event` `fn` `if` `switch` `loop` `parallel` `race` など）
- エラー・警告の表示（入力中に検査。マニフェストのエラーはマニフェスト側に表示）
- 補完：コマンド・クエリ（日本語の表示名でも絞り込める。待機ありのコマンドは `await` 付きで入る）、
  `Enum.` の後の値、`バンク.` の後の共有変数、var・関数・引数・let、`fire` の後のイベント
- ホバー：コマンドの書き方・表示名・説明、共有変数の ID とセーブの有無、enum の値
- 定義へ移動：コマンド・共有変数・enum はマニフェストの該当行へ、var・関数は同じファイルへ
- 引数ヒント、アウトライン
- コマンド：「AtomScript: このファイルをコンパイル」「AtomScript: コマンド一覧を表示」「AtomScript: 言語サーバーを再起動」

## 設定

| 設定 | 内容 |
| --- | --- |
| `atomscript.compilerPath` | `atsc` のパス。空なら拡張に同梱したもの、なければ PATH |
| `atomscript.manifest` | マニフェストのパス。空なら、スクリプトのフォルダから上へ探して最初の `*.atsmanifest.yaml` |

## ビルド

```bash
# リポジトリの直下で atsc を Release ビルドしてから
cd editors/vscode
npm install
npm run compile
npm test                 # atsc lsp との疎通テスト
npm run package          # atsc を bin/ に同梱して atomscript-<version>.vsix を作る
code --install-extension atomscript-0.1.0.vsix
```
