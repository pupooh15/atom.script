# AtomScript — 作業の引き継ぎメモ

新しいセッションはまずこのファイルを読むこと。最終更新：2026-10-07。

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
| ScriptedImporter（2026-10-07） | `.ats` を保存すると atsc を子プロセスで呼んで `AtsScriptAsset`（ScriptableObject、.atsb のバイト列）にする。マニフェストは Project Settings → AtomScript（`ProjectSettings/AtomScriptSettings.asset`）で指定、未設定なら Assets に 1 つだけあるもの。マニフェスト（include 先含む）と atsc が変わったらカスタム依存 `AtomScript/Manifest` で全 .ats を再インポート。エラーは `パス(行,列): error: …` でコンソールに出し、アセットは作らない |
| エディタからの atsc gen（2026-10-07） | マニフェスト（include 先含む）・atsc が変わったら自動で C# を作り直す（Project Settings でオフにできる）。手動は Assets → AtomScript → C# を生成、Project Settings の「今すぐ生成」。出力先の既定は `Assets/AtomScript/Generated/{Project}.g.cs`、名前空間の既定は project の PascalCase。中身が同じなら書き換えない。生成物はバージョン管理に入れる想定 |
| macOS の Unity エディタ（2026-10-07） | Apple Silicon 版だけ対応すればよい（Intel 版エディタは確認しない）。プラグインと atsc はユニバーサルのまま（Intel の Mac で動くプレイヤーのため） |
| macOS の署名・公証（2026-10-07） | 後回し。今はリンカーの ad-hoc 署名のみ。配布方法（仕様書 §14 の未決事項）が決まったら判断する。git（clone・UPM の git URL）で配るなら不要。ビルド済みバイナリを zip / tarball でダウンロードさせるなら必要（quarantine 属性が付き、Gatekeeper が bundle の読み込みと atsc の起動を止める。回避は `xattr -dr com.apple.quarantine`）。出荷するゲームはゲーム開発者が .app ごと署名・公証し直すので関係しない。必要になったら会社の Apple Developer Program（Developer ID 証明書）と codesign / notarytool の手順を足す |
| ネイティブの配り方（2026-10-08） | エディタ・デスクトップ（Windows / macOS）はビルド済みバイナリ（A）。Android・iOS・家庭用機は**コアのソースをパッケージに置き、ゲームのビルド時に IL2CPP がコンパイル**（B。機種ごとのバイナリを作らない。家庭用機の SDK ビルド環境を持たずに済む）。Mono での開発はしない（Android の Mono は armeabi-v7a のみで、arm64 は IL2CPP 専用） |
| ATS_API の既定（2026-10-08） | 何も定義しなければ静的リンク扱い（IL2CPP のソースプラグインと UE は define を渡せないため）。共有ライブラリを作るときは `ATS_BUILD_DLL`、Windows で DLL を使う C/C++ 側は任意で `ATS_USE_DLL`。`ATS_STATIC` は付けても同じ |
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
unity/com.pupooh15.atomscript/   Unity パッケージ（パッケージ名は仮）。Runtime/ が C# ラッパー、Editor/ がインポーターと設定、Tests/Editor/ が EditMode テスト、
                                 Tests/Runtime/ が PlayMode・プレイヤー用テスト（全プラットフォーム対象。エディタのテストからも参照する）
                                 Tests/*/Generated/*.g.cs と Data~/*.atsb.bytes は CMake のビルドで atsc から作られる（コミットする）。
                                 プレイヤーはファイルを読めないので、SampleRpg の .atsb は Tests/Runtime/Generated/SampleRpgBytecode.g.cs に埋め込む
                                 （tools/cmake/embed_csharp_bytes.cmake。Resources に置くと利用者のゲームに入ってしまうため）
                                 Runtime/Plugins/ と Editor/Tools~/ はビルドで作る（コミットしない）。Windows は Plugins/Windows/x86_64/atomscript.dll と Tools~/win-x64/atsc.exe、
                                 macOS は Plugins/macOS/atomscript.bundle と Tools~/osx/atsc（どちらもユニバーサル）、
                                 IL2CPP の機種は Plugins/IL2CPP/AtomScript/ にコアのソース（ホストのビルドで src・include から tools/cmake/copy_il2cpp_source.cmake でコピー）。
                                 IL2CPP はプラグインのソースを 1 つのフォルダーに平らにコピーしてコンパイルするので、サブフォルダーを作らず、
                                 #include "atomscript/xxx.h" を "xxx.h" に書き換えてある。.meta は「全プラットフォーム、ただしエディタ・Windows・macOS・Linux・WebGL を除く」
unity/plugin-meta/               プラグインの .meta の正本（GUID とプラットフォーム設定）。CMake がバイナリと一緒にパッケージへコピーする。
                                 パッケージ内に置くと、バイナリの無いプラットフォームで Unity が持ち主のいない .meta を消すため外に出した（2026-10-07）
                                 テスト用の .ats・マニフェストは Data~ に置く（~ 付きは Unity が読み込まない。パッケージ内の .ats がインポーターにかからないように）
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

# Unity パッケージのテスト（先に上の CMake ビルドで DLL と生成物を作る。Unity 6000.0.67f1 で確認）
# スクラッチ領域などに空のプロジェクトを作り、Packages/manifest.json に
#   "com.pupooh15.atomscript": "file:E:/usr/app/github/atom.script/unity/com.pupooh15.atomscript",
#   "com.unity.test-framework": "1.6.0"  と  "testables": [ "com.pupooh15.atomscript" ] を書いて：
"/c/Program Files/Unity/Hub/Editor/6000.0.67f1/Editor/Unity.exe" -batchmode -nographics -projectPath <プロジェクト>     -runTests -testPlatform EditMode -testResults results.xml -logFile unity.log   # 現在 28 件すべて成功（PlayMode は 2 件）
```

- Unity は 8.3 形式の短いパス（`KAZUHI~1.HAG`）のプロジェクトだとプレイヤービルドが失敗する。長いパスで渡す。
- IL2CPP（StandaloneWindows64）で確かめるときは `-testPlatform StandaloneWindows64 -testSettingsFile <{"scriptingBackend":"IL2CPP"}>`。
  プロジェクトのパスが深いと出力が MAX_PATH（260 文字）を超え、プレイヤーが「Failed to initialize IL2CPP」で止まる（エディタは待ち続ける）。
  スクラッチ領域は深すぎるので、短いパス（例 `E:\tmp\…`）に一時プロジェクトを作る。
  Mono のプレイヤーも同じで、深いパスだと「Unable to load mono library」で止まる。Mono も短いパスで動かす。
  2026-10-07 に Tests/Runtime の 2 件を Windows プレイヤー（x64）の Mono・IL2CPP とも成功。
- Unity はパッケージに .meta を書き出す。新しいファイルを足したら .meta もコミットする（プラグインの .meta だけは unity/plugin-meta に置く）。
- 構文ハイライトの正規表現は vscode-textmate + vscode-oniguruma で実際に字句分けして確かめた（スクラッチ領域で実施。リポジトリには入れていない）。

- PATH に cmake は無い。上のフルパスを使う。コンソール出力は cp932 なので、MSVC のメッセージを読むときは `iconv -f cp932 -t utf-8`。
- 警告ゼロを維持する（/W4）。MSVC の printf 系に日本語リテラルを渡すと C4819 が出るので、書式なしなら `fputs` を使う。

## Android（IL2CPP でソースからビルド）

- Unity のプレイヤーではコアをソースから IL2CPP がコンパイルするので、Android 用の .so は作らない。C# は `__Internal` から呼ぶ（`NativeMethods.Lib`）。
- 2026-10-08 に確認：Unity 6000.0.67f1 で AtomScript を呼ぶシーンを IL2CPP / ARM64 の APK にビルドでき、コアは libil2cpp.so に入る（libatomscript.so は無い）。
  確認は短いパスの一時プロジェクトで `-executeMethod` から BuildPipeline.BuildPlayer。IL2CPP のコンパイルで警告なし。
  **実機では未確認**（端末が無い。エミュレーターのイメージも入っていない）。端末をつないだら `-testPlatform Android` で Tests/Runtime の 2 件を動かす。
- コアが NDK でコンパイルできるかだけを見るなら、CMake でクロスビルドもできる（Unity には入れない）：

```bash
A="C:/Program Files/Unity/Hub/Editor/6000.0.67f1/Editor/Data/PlaybackEngines/AndroidPlayer"   # NDK r27c と CMake 3.22.1・Ninja が入っている
C="$A/SDK/cmake/3.22.1/bin"
"$C/cmake.exe" -S . -B build-android -G Ninja -DCMAKE_MAKE_PROGRAM="$C/ninja.exe"     -DCMAKE_TOOLCHAIN_FILE="$A/NDK/build/cmake/android.toolchain.cmake"     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
"$C/cmake.exe" --build build-android      # クロスビルドでは ATS_BUILD_TOOLS が既定で OFF（atsc・テストは作らない）
```

## ビルドとテスト（macOS）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j                    # arm64 + x86_64 のユニバーサル、最小 macOS 11.0（CMakeLists.txt の既定）
./build/atomscript_tests                   # arch -x86_64 を付けると Rosetta で x86_64 側も確かめられる
```

- この Mac は Xcode なし（Command Line Tools のみ）。CLT 26.6 のリンカーは SDK 27 を読めず（「unknown architecture arm64e.x1」）、
  CMake も Unity の IL2CPP ビルドも失敗していた。2026-10-07 に CLT 27.0 に更新して解消（SDK の指定も回避策も不要）。
  同じエラーが出たら CLT を更新する（`softwareupdate --list` で確認）。
- .bundle は arm64・x86_64 の両方で dlopen して `ats_get_api_version` が呼べることを確かめた（x86_64 は Rosetta）。

```bash
# Unity パッケージのテスト（Windows と同じ手順。manifest.json の file: は /Users/…/atom.script/unity/com.pupooh15.atomscript）
"/Applications/Unity/Hub/Editor/6000.0.67f1/Unity.app/Contents/MacOS/Unity" -batchmode -nographics -projectPath <プロジェクト> \
    -runTests -testPlatform EditMode -testResults results.xml -logFile unity.log   # 2026-10-07 に 28 件すべて成功（エディタは arm64）
# -testPlatform PlayMode でエディタ内、-testPlatform StandaloneOSX でプレイヤー（IL2CPP は -testSettingsFile に {"scriptingBackend":"IL2CPP"}）
```
- プレイヤー用テスト（Tests/Runtime/PlayerTests.cs、2 件）：AtomScriptLoop で数フレームかかる待機ありコマンドを動かす＋セーブ／ロード＋中断でキャンセル通知。
  2026-10-07 に PlayMode（エディタ内）と macOS プレイヤー arm64 / x64（Rosetta）× Mono / IL2CPP の 4 通りで成功。
  アーキテクチャはテスト用プロジェクトの [InitializeOnLoad] のエディタスクリプトで `UnityEditor.OSXStandalone.UserBuildSettings.architecture` を切り替えた。
  - 初回は「Error building Player because scripts are compiling」で失敗することがある。先に `-batchmode -quit` で一度開いておく。
- メモリの不具合は ASan で確かめられる：`-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`（リンカーフラグにも同じものを付ける）。

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
| 段階2：Unity 統合 | C# ラッパーと PlayerLoop 更新、ScriptedImporter、エディタからの atsc gen は実装済み（Windows x64。EditMode テスト 28 件成功、ラッパーは IL2CPP プレイヤーでも確認）。macOS 用の atsc とプラグイン（.bundle、ユニバーサル）も実装済み（EditMode テスト 28 件成功。macOS プレイヤーは arm64 / x64 × Mono / IL2CPP で確認）。Android・iOS・家庭用機は IL2CPP でコアをソースからビルドする方式にした（Android は APK のビルドまで確認、実機は未確認。iOS は未確認）。デバッガ接続は未着手 |
| 段階2：UE 統合 | 未着手 |
| 段階3：家庭用機・モバイル対応、ノードエディタ | 未着手 |

## 直近の作業（2026-10-07、macOS 対応）

- macOS 用の atsc と Unity プラグイン `atomscript.bundle` を追加（CMake の `atomscript_bundle`、arm64 + x86_64 のユニバーサル、最小 macOS 11.0）。
  Unity 6000.0.67f1（Apple Silicon）で EditMode テスト 28 件成功、コアのテスト 58 件は arm64・x86_64（Rosetta）とも成功。
  CLT 27.0 への更新後、SDK の指定・回避策なしでコア（Debug / Release）と Unity（EditMode・PlayMode・プレイヤー 4 通り）を確認し直した。
- ASan / UBSan で見つけた不具合を修正：
  - テストで `Env` を `Host` より先に宣言していたため、VM の破棄時のキャンセル通知が破棄済みの `Host` に書き込んでいた（MSVC ではたまたま表に出ていなかった）。
    VM の破棄時にホストへ通知が来るので、ホスト側のオブジェクトは `Env` より先に宣言する。
  - コンパイラが型エラーの印に `(ats_type)0xFF`（列挙型の範囲外＝未定義動作）を使っていた。`TypeRef::error` に変更。
- プレイヤー用テストをパッケージ（Tests/Runtime）に追加。
- プラグインの .meta を `unity/plugin-meta/` に移し、パッケージの `Runtime/Plugins/` をビルド生成物にした
  （バイナリの無いプラットフォームで Unity が .meta を消すため）。

## Windows での確認（2026-10-07、macOS 対応の後）

- コアのテスト 58 件（Debug / Release、/W4 で警告ゼロ）、Unity の EditMode 28 件・PlayMode 2 件・Windows プレイヤー（Mono / IL2CPP）各 2 件すべて成功。
  `unity/plugin-meta` から .meta がコピーされることも確認。
- `tools/cmake/embed_csharp_bytes.cmake` を Windows で動かすと生成物が変わっていたのを修正：
  `file( WRITE )` が CRLF で書く → `configure_file( … NEWLINE_STYLE UNIX )` で LF に揃える。
  `-DSOURCE=` の日本語が化ける（Visual Studio のビルドはカスタムコマンドを cp932 のバッチで実行する）→ 引数は ASCII にし、日本語はスクリプト側で組み立てる。
  -P で動かすスクリプトには `cmake_minimum_required` を書く（無いとポリシー警告が出る）。

## ユーザーへの確認待ち

- Unity パッケージ名 `com.pupooh15.atomscript` は仮（仕様書は `com.<company>.atomscript`）。決まったらフォルダー名・package.json・CMake・テストのパスを直す。

コミット・プッシュは毎回ユーザーの指示を待つこと。

（済）atsc の実装中に決めた文法・マニフェストの追加は仕様書 §4「文法の補足」・§8 に反映し、`docs/spec.md` も書き出し直した。

## 次の作業の候補

- Unity パッケージの続き：`.ats` のアイコン、
  macOS の配布用の署名・公証（後回しと決定済み。上の表を参照）、
  Android の実機確認（端末が要る）、iOS の確認（Unity で Xcode プロジェクトを書き出し、Mac でビルド）、家庭用機での確認（プレイヤー用テストは Tests/Runtime にある）。
  インポートは 1 ファイル 0.5〜0.8 秒（atsc の起動込み）。数が増えて遅ければ、まとめてコンパイルする方法を考える
- `gen` の改善候補：`Commands` をカテゴリごとに分割できるようにする、UE 向け（UObject / Blueprint）の生成
- `atsc fmt` / `strings` / `refs`
- VS Code 拡張の続き：デバッガ（DAP。VM 側のデバッグサーバー `ats_debug_server_start` が先に必要）、ホットリロード、Marketplace 以外での配布方法
- Unity パッケージ（Unity 6、P/Invoke、IL2CPP の MonoPInvokeCallback、`Awaitable`）
- UE 5.4 プラグイン（コアはソースのまま同梱）
- 仕様書 §14 の未決事項：構造体型（ベクトルなど）をコアの型に入れるか、メッセージ本文とローカライズの扱い、家庭用機の SDK ビルド環境、共通リポジトリの配布方法、段階2の試験タイトル
