# AtomScript — 作業の引き継ぎメモ

新しいセッションはまずこのファイルを読むこと。最終更新：2026-10-06。

## これは何か

LT（DS、2007年）のスクリプトマネージャ（`E:\usr\app\ds\LT\prog\source\scriptman*.cpp`）を参考に新規設計した、
エンジン非依存のイベントスクリプトシステム。C++17 のコア VM を C API で公開し、Unity・Unreal Engine の両方から使う。
スクリプトは当面テキスト（`.ats`）で書き、最終的にはノードエディタが同じ `.ats` を出力する。

- 仕様書の正本：Claude Docs「AtomScript 仕様書（ドラフト）」 https://claude.ai/code/artifact/f9a9efc7-d931-4d44-9332-b9f199bc9c02
  - `docs/spec.md` はその書き出し（図は Mermaid に描き直し済み）。仕様書を変えたら書き出し直す：
    Claude Docs の export（format: markdown）の結果を保存し、`python tools/scripts/export_spec.py <保存したファイル>` を実行する。
    仕様書に図を足したら、スクリプトの `DIAGRAMS` に Mermaid 版を追加する。
- 言語・マニフェスト・atsc のリファレンス：`docs/language.md`
- バイナリ形式・命令・セーブ形式：`docs/bytecode.md`
- GitHub：`git@github.com:pupooh15/atom.script.git`（main）

## ユーザーと決めたこと（変更するときは必ず確認する）

| 項目 | 決定 |
| --- | --- |
| 正式名称 | AtomScript |
| 接頭辞・拡張子・CLI | API `ats_` / `ATS_`、テキスト `.ats`、バイナリ `.atsb`（マジック `ATSB`）、マニフェスト `.atsmanifest.yaml`、CLI `atsc` |
| 対象 | PC、PS5、Xbox Series X\|S、Nintendo Switch、Nintendo Switch 2、iOS、Android |
| エンジン | Unity 6、UE 5.4 |
| 記述形式 | 当面はテキスト（`.ats`）＋ CLI。ノードエディタは後回し（技術選定も保留）。最終的にエディタが `.ats` を出力し CLI でバイナリ化 |
| ノードエディタのレイアウト | 左→右ではなく上→下（仕様書 §11） |
| goto | 持たない（テキスト↔ノードの往復変換のため。仕様書 §4） |
| 変数 | 共有変数（マニフェスト）／スクリプト変数 `var`（ファイル単位・セーブ対象、`transient var` は対象外、`@was` で改名）／`let`。「クエスト専用」のような変数は作らない |
| スクリプトID | ファイル先頭に `script "ID"` を手書き（エディタ移行後はエディタが振る） |
| セーブ | 変数と乱数の状態だけ。**実行中のファイバは保存しない**。再開位置はスクリプトが変数（LT のイベントランク相当）で判断する |
| スタック要素 | 64bit（仕様書の当初案 32bit から変更済み） |
| LT 資産 | 参考のみ。互換性は持たない |
| C# ラッパー（2026-10-07） | ① コマンドの実装は **interface**（`I<Project>Commands`。MonoBehaviour でも実装できる）。仕様書の partial クラス案から変更 ② 待機ありは `Awaitable` / `Awaitable<T>` ＋ `CancellationToken`。低レベル登録 `ScriptRuntime.RegisterCommand` も残す ③ handle は `AtsHandle` 構造体＋任意の `HandleTable<T>` ④ 共有変数は型付き `VarId<T>` ⑤ イベントは `Events.FireXxx` ⑥ 初期化系は例外（`AtsException`）、Update・IsFiberAlive・AbortFiber は例外なし ⑦ `IDisposable` ＋ `AtomScriptLoop`（PlayerLoop）。MonoBehaviour は提供しない ⑧ 名前空間 `AtomScript`、`ScriptRuntime` / `ScriptVM` / `ScriptProgram` |
| cancel コールバック | `ats_cancel_fn( vm, token, user )`。トークンは VM ごとに振られるため VM も渡す（2026-10-07 に変更） |

## リポジトリの構成

```
include/atomscript/ats_api.h     C API（エンジン・ゲームが使う唯一の入口）
include/atomscript/ats_format.h  .atsb の定義（VM とコンパイラで共有）
src/                             コア VM（例外・RTTI・STL 不使用。メモリはホストのアロケータ）
tools/writer/                    .atsb 書き出し（STL 可）。max_stack を自動計算
tools/compiler/                  コンパイラ：yaml → manifest → lexer → parser → compiler（意味検査＋コード生成）、disasm、
                                 gen（C++ / HTML）、json と lsp（言語サーバー）、atsc_main
editors/vscode/                  VS Code 拡張（TypeScript。解析は atsc lsp に任せ、拡張側に解析器は持たない）
unity/com.pupooh15.atomscript/   Unity パッケージ（パッケージ名は仮）。Runtime/ が C# ラッパー、Tests/Editor/ が EditMode テスト
                                 Tests/Editor/Generated/*.g.cs と Data/*.atsb.bytes は CMake のビルドで atsc から作られる（コミットする）
                                 Runtime/Plugins/Windows/x86_64/atomscript.dll もビルドでコピーされる（.dll はコミットしない。.meta はする）
samples/                         sample.atsmanifest.yaml、merchant.ats（仕様書 §4 の例）
tests/                           自前の簡易テスト（TEST / CHECK / REQUIRE）。Env が確保数を数えてリークを検出する
docs/                            spec.md / language.md / bytecode.md
```

## ビルドとテスト（Windows、VS2019 同梱の CMake）

```bash
CMAKE="/c/Program Files (x86)/Microsoft Visual Studio/2019/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -S . -B build -G "Visual Studio 16 2019" -A x64
"$CMAKE" --build build --config Debug      # Release も確認する
./build/Debug/atomscript_tests.exe         # 現在 58 件すべて成功
./build/Debug/atsc.exe compile samples/merchant.ats -m samples/sample.atsmanifest.yaml -o build/merchant.atsb
./build/Debug/atsc.exe disasm build/merchant.atsb

# VS Code 拡張（先に Release の atsc を作る）
cd editors/vscode && npm install && npm run compile
npm test                                   # 実際の atsc lsp と標準入出力でやり取りする疎通テスト
echo y | npx vsce package                  # LICENSE が無いので確認に y を渡す。先に npm run copy-atsc で bin/ に atsc を入れる
```

# Unity パッケージのテスト（先に上の CMake ビルドで DLL と生成物を作る。Unity 6000.0.67f1 で確認）
# スクラッチ領域などに空のプロジェクトを作り、Packages/manifest.json に
#   "com.pupooh15.atomscript": "file:E:/usr/app/github/atom.script/unity/com.pupooh15.atomscript",
#   "com.unity.test-framework": "1.6.0"  と  "testables": [ "com.pupooh15.atomscript" ] を書いて：
"/c/Program Files/Unity/Hub/Editor/6000.0.67f1/Editor/Unity.exe" -batchmode -nographics -projectPath <プロジェクト>     -runTests -testPlatform EditMode -testResults results.xml -logFile unity.log   # 現在 19 件すべて成功
```

- Unity は 8.3 形式の短いパス（`KAZUHI~1.HAG`）のプロジェクトだとプレイヤービルドが失敗する。長いパスで渡す。
- IL2CPP（StandaloneWindows64）で確かめるときは `-testPlatform StandaloneWindows64 -testSettingsFile <{"scriptingBackend":"IL2CPP"}>`。
  プロジェクトのパスが深いと出力が MAX_PATH（260 文字）を超え、プレイヤーが「Failed to initialize IL2CPP」で止まる（エディタは待ち続ける）。
  スクラッチ領域は深すぎるので、短いパス（例 `E:	mp\…`）に一時プロジェクトを作る。プレイヤー用のテストはパッケージに無いので、
  Assets に PlayMode テスト（生成コードと .atsb を Resources に置く）を一時的に作って動かした（2026-10-07 に 1 件成功）。
- Unity はパッケージに .meta を書き出す。新しいファイルを足したら .meta もコミットする。

```bash
- 構文ハイライトの正規表現は vscode-textmate + vscode-oniguruma で実際に字句分けして確かめた（スクラッチ領域で実施。リポジトリには入れていない）。

- PATH に cmake は無い。上のフルパスを使う。コンソール出力は cp932 なので、MSVC のメッセージを読むときは `iconv -f cp932 -t utf-8`。
- 警告ゼロを維持する（/W4）。MSVC の printf 系に日本語リテラルを渡すと C4819 が出るので、書式なしなら `fputs` を使う。

## コードの書き方

- コメントは日本語。インデントはタブ。既存ファイルの書式（`func( a, b )` の空白、`//===` の区切り）に合わせる。
- コア（`src/`）は例外・RTTI・STL を使わない（`Array<T>` と `StrMap` は `ats_internal.h`）。ツール側（writer / compiler）は STL 可、例外は使っていない。
- コンパイラのエラーメッセージは日本語。
- テストを足したら Debug / Release の両方で回す。リークは Env の破棄時に自動検出される。

## 現在の状態

| 段階（仕様書 §14） | 状態 |
| --- | --- |
| 段階1：コア VM・C API | 完了（push 済み、コミット 7135e08） |
| 段階1：コンパイラ・CLI | 完了（push 済み） |
| 段階1：`atsc gen`（C++ / HTML / C#） | 完了 |
| 段階2：VS Code 拡張 | 完了（push 済み、テスト 58 件）。デバッガ（DAP）は VM 側のデバッグサーバーと一緒に作る |
| 段階2：Unity 統合 | C# ラッパーと PlayerLoop 更新は実装済み（Windows x64。EditMode テスト 19 件成功、IL2CPP プレイヤーでの動作も確認）。ScriptedImporter・他プラットフォームのプラグイン・デバッガ接続は未着手 |
| 段階2：UE 統合 | 未着手 |
| 段階3：家庭用機・モバイル対応、ノードエディタ | 未着手 |

## ユーザーへの確認待ち

- Unity パッケージ名 `com.pupooh15.atomscript` は仮（仕様書は `com.<company>.atomscript`）。決まったらフォルダー名・package.json・CMake・テストのパスを直す。

コミット・プッシュは毎回ユーザーの指示を待つこと。

（済）atsc の実装中に決めた文法・マニフェストの追加は仕様書 §4「文法の補足」・§8 に反映し、`docs/spec.md` も書き出し直した。

## 次の作業の候補

- Unity パッケージの続き：`.ats` の ScriptedImporter（atsc を呼んで .atsb のバイト列を持つ ScriptableObject を作る）、
  エディタからの `atsc gen` 実行、macOS / Android / iOS / 家庭用機のプラグイン、PlayMode（実機）で動くテスト
- `gen` の改善候補：`Commands` をカテゴリごとに分割できるようにする、UE 向け（UObject / Blueprint）の生成
- `atsc fmt` / `strings` / `refs`
- VS Code 拡張の続き：デバッガ（DAP。VM 側のデバッグサーバー `ats_debug_server_start` が先に必要）、ホットリロード、Marketplace 以外での配布方法
- Unity パッケージ（Unity 6、P/Invoke、IL2CPP の MonoPInvokeCallback、`Awaitable`）
- UE 5.4 プラグイン（コアはソースのまま同梱）
- 仕様書 §14 の未決事項：構造体型（ベクトルなど）をコアの型に入れるか、メッセージ本文とローカライズの扱い、家庭用機の SDK ビルド環境、共通リポジトリの配布方法、段階2の試験タイトル
