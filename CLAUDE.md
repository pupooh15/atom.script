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

## リポジトリの構成

```
include/atomscript/ats_api.h     C API（エンジン・ゲームが使う唯一の入口）
include/atomscript/ats_format.h  .atsb の定義（VM とコンパイラで共有）
src/                             コア VM（例外・RTTI・STL 不使用。メモリはホストのアロケータ）
tools/writer/                    .atsb 書き出し（STL 可）。max_stack を自動計算
tools/compiler/                  コンパイラ：yaml → manifest → lexer → parser → compiler（意味検査＋コード生成）、disasm、atsc_main
samples/                         sample.atsmanifest.yaml、merchant.ats（仕様書 §4 の例）
tests/                           自前の簡易テスト（TEST / CHECK / REQUIRE）。Env が確保数を数えてリークを検出する
docs/                            spec.md / language.md / bytecode.md
```

## ビルドとテスト（Windows、VS2019 同梱の CMake）

```bash
CMAKE="/c/Program Files (x86)/Microsoft Visual Studio/2019/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -S . -B build -G "Visual Studio 16 2019" -A x64
"$CMAKE" --build build --config Debug      # Release も確認する
./build/Debug/atomscript_tests.exe         # 現在 48 件すべて成功
./build/Debug/atsc.exe compile samples/merchant.ats -m samples/sample.atsmanifest.yaml -o build/merchant.atsb
./build/Debug/atsc.exe disasm build/merchant.atsb
```

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
| 段階2：VS Code 拡張、Unity 統合、UE 統合 | 未着手 |
| 段階3：家庭用機・モバイル対応、ノードエディタ | 未着手 |

## ユーザーへの確認待ち

現在なし。コミット・プッシュは毎回ユーザーの指示を待つこと。

（済）atsc の実装中に決めた文法・マニフェストの追加は仕様書 §4「文法の補足」・§8 に反映し、`docs/spec.md` も書き出し直した。

## 次の作業の候補

- `atsc gen`：マニフェストから C# / C++ の登録コード（`ats_register_command` / `ats_define_var`、シグネチャハッシュは `MCommand::SignatureHash()` と同じ規則）を生成
- `atsc fmt` / `strings` / `refs`
- VS Code 拡張（構文ハイライト、コンパイラを言語サーバーとして使う補完・エラー表示）
- Unity パッケージ（Unity 6、P/Invoke、IL2CPP の MonoPInvokeCallback、`Awaitable`）
- UE 5.4 プラグイン（コアはソースのまま同梱）
- 仕様書 §14 の未決事項：構造体型（ベクトルなど）をコアの型に入れるか、メッセージ本文とローカライズの扱い、家庭用機の SDK ビルド環境、共通リポジトリの配布方法、段階2の試験タイトル
