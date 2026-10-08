# AtomScript

エンジン非依存のイベントスクリプトシステム。C++17 のコア VM を C API（`ats_api.h`）で公開し、Unity・Unreal Engine の両方から使う。
LT（DS）のスクリプトマネージャを参考に新規設計したもの。仕様書は [docs/spec.md](docs/spec.md)（Claude Docs の「AtomScript 仕様書（ドラフト）」から書き出したもの）。

## 現在の状態

| 部分 | 状態 |
| --- | --- |
| コア VM・C API | 実装済み（本リポジトリ） |
| バイナリ形式 `.atsb` | v1.0（[docs/bytecode.md](docs/bytecode.md)） |
| 書き出しライブラリ（`tools/writer`） | 実装済み |
| コンパイラ `atsc`（`.ats` → `.atsb`） | 実装済み（[docs/language.md](docs/language.md)）。`gen` は C++・C#（Unity）・HTML に対応。`fmt` / `strings` / `refs` は未実装 |
| VS Code 拡張（`editors/vscode`） | 実装済み。構文ハイライト・スニペット・言語サーバー（`atsc lsp`）によるエラー表示・補完・ホバー・定義へ移動・引数ヒント・アウトライン |
| Unity パッケージ（`unity/com.pupooh15.atomscript`） | C# ラッパー（P/Invoke・SafeHandle・`Awaitable`）、PlayerLoop での更新、`.ats` の ScriptedImporter、エディタからの C# 生成を実装済み。Windows x64・macOS（ユニバーサル）はビルド済みバイナリ、Android・iOS・家庭用機は IL2CPP でソースからビルド |
| UE 統合、デバッガ | 未着手 |

## ディレクトリ

```
include/atomscript/ats_api.h     C API（エンジン・ゲームが使う唯一の入口）
include/atomscript/ats_format.h  .atsb の定義（VM とコンパイラで共有）
src/                             コア VM（例外・RTTI・STL を使わない）
tools/writer/                    .atsb の書き出し（ツール側。STL 可）
tools/compiler/                  コンパイラ（マニフェスト・字句／構文解析・意味検査・コード生成）と atsc
editors/vscode/                  VS Code 拡張（TypeScript。解析は atsc lsp に任せる）
unity/com.pupooh15.atomscript/   Unity パッケージ（C# ラッパーとテスト）
unity/plugin-meta/               Unity 用ネイティブプラグインの .meta（ビルドでパッケージにコピーする）
samples/                         サンプルのマニフェストとスクリプト
tests/                           テスト
docs/spec.md                     仕様書
docs/language.md                 言語・マニフェスト・atsc のリファレンス
docs/bytecode.md                 バイナリ形式・命令・セーブ形式の詳細
```

## ビルド

CMake 3.16 以降と C++17 コンパイラ。

```bash
# Windows
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
build/Release/atomscript_tests.exe

# macOS（arm64 と x86_64 のユニバーサル、macOS 11.0 以上）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/atomscript_tests

# Android（arm64-v8a、API 23 以上）。コアが NDK でコンパイルできるかの確認用（Unity には入れない。下を参照）。
# NDK と CMake・Ninja は Unity の Android モジュールに入っているものを使える
#   NDK   = <Unity>/Editor/Data/PlaybackEngines/AndroidPlayer/NDK
#   CMAKE = <Unity>/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/cmake/3.22.1/bin
"$CMAKE/cmake" -S . -B build-android -G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE/ninja"     -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
"$CMAKE/cmake" --build build-android
```

クロスビルド（Android・iOS）では、コアのライブラリだけを作る（atsc とテストはホスト用なので作らない。`ATS_BUILD_TOOLS`）。

ビルドすると Unity パッケージにネイティブプラグインと atsc がコピーされる。

| 対象 | Unity パッケージに入るもの |
| --- | --- |
| エディタ・デスクトップ（Windows） | ビルド済み `Runtime/Plugins/Windows/x86_64/atomscript.dll`、`Editor/Tools~/win-x64/atsc.exe` |
| エディタ・デスクトップ（macOS） | ビルド済み `Runtime/Plugins/macOS/atomscript.bundle`、`Editor/Tools~/osx/atsc` |
| IL2CPP の機種（Android・iOS・家庭用機） | コアのソース `Runtime/Plugins/IL2CPP/AtomScript/`。ゲームのビルド時に IL2CPP が一緒にコンパイルする（機種ごとのバイナリは作らない。Mono には対応しない） |

プラグインの `.meta` は `unity/plugin-meta` からコピーする。

| オプション | 既定 | 内容 |
| --- | --- | --- |
| `ATS_BUILD_SHARED` | ON | 共有ライブラリ `atomscript`（.dll / .so / .dylib）を作る |
| `ATS_BUILD_TESTS` | ON | テストを作る |
| `ATS_ENABLE_DEBUG` | ON | デバッグ用 API を含める。製品ビルドでは OFF |
| `ATS_BUILD_TOOLS` | ON（クロスビルドでは OFF） | writer・コンパイラ・atsc を作る。OFF ならテストも作らない |

成果物：

- `atomscript_static`：静的ライブラリ（テスト・C++ から静的にリンクする用）
- `atomscript`：共有ライブラリ（Windows・macOS・Android・Linux）
- `atomscript_bundle`：macOS の Unity 用プラグイン `atomscript.bundle`（macOS のみ）
- `atomscript_writer`：書き出しライブラリ
- `atomscript_compiler`：コンパイラ本体（ライブラリ）
- `atsc`：コンパイラ CLI

```bash
build/Release/atsc.exe compile samples/merchant.ats -m samples/sample.atsmanifest.yaml
build/Release/atsc.exe disasm samples/merchant.atsb
```

## 使い方（概要）

```c
ats_runtime* rt;
ats_runtime_create( NULL, &rt );

ats_command_desc cmd = { sizeof(cmd) };
cmd.name    = "ShowMessage";
cmd.channel = "message";          // 同じチャンネルのコマンドは 1 つずつ
cmd.fn      = ShowMessageHandler; // ATS_DONE / ATS_PENDING / ATS_RUNNING / ATS_FAIL を返す
ats_register_command( rt, &cmd );

ats_program* prog;
ats_program_load( rt, bytes, size, &prog );   // 検証と名前解決

ats_vm* vm;
ats_vm_create( rt, NULL, &vm );
ats_vm_fire_event( vm, prog, "OnTalk", args, 1, NULL );

// 毎フレーム
ats_vm_update( vm, dt );

// 待機中のコマンドが終わったら
ats_call_complete( vm, token, &result );
```

## コアの方針

- メモリはすべてホストのアロケータ（`ats_runtime_desc`）から取る。
- 1 つの VM は 1 スレッドから使う。別の VM は別スレッドで動かしてよい。
- 1 回の update で実行する命令数に上限がある（無限ループでもゲームは止まらない）。
- セーブに入るのは変数と乱数の状態だけ。実行中のファイバは保存しない。
