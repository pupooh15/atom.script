# AtomScript 言語リファレンス（.ats） / マニフェスト（.atsmanifest.yaml）

コンパイラ `atsc` が受け付ける書き方。サンプルは [samples/](../samples)。

## ファイルの構成

```
script "golden_lore/el_rescue"     // 先頭に 1 つ。スクリプトID（セーブデータのキー。変えない）

@was("retry_count")                // 旧名からセーブ値を引き継ぐ（任意）
var retry: int = 0                 // スクリプト変数（このファイルだけ。セーブ対象）
transient var temp: int = 0        // セーブしないスクリプト変数

event OnTalk(target: handle) { … } // イベント（ゲームや fire から起動）
fn Helper(x: int) -> int { … }     // 関数（戻り値は省略可）
```

- 文は改行で区切る。式は括弧の中を除いて行をまたがない。
- コメントは `//` と `/* */`。文字列は UTF-8 の `"…"`（`\n` `\t` `\"` `\\`）。
- 識別子には日本語も使える。

## 型

| 型 | 内容 |
| --- | --- |
| `bool` | `true` / `false` |
| `int` | 32bit 整数。演算のオーバーフローは折り返す |
| `float` | 32bit 実数。int は必要に応じて float に昇格する |
| `string` | 文字列（比較 `==` `!=` のみ） |
| enum 名 | マニフェストの enum。値は `Face.Smile` |
| `handle` | ホストが意味を決める値（アクター参照など）。比較のみ。var にはできない |

## 変数

| 書き方 | 範囲 | セーブ |
| --- | --- | --- |
| `story.chapter` | マニフェストの共有変数（バンク名.変数名） | バンクの scope による |
| `var x: int = 0` | このファイル | 対象（`transient` は対象外） |
| `let x = 0` / `let x: float = 1` | ブロック | 対象外 |

var の初期値は定数（リテラル・enum の値・負の数）だけ。

## 文

```
let n = 3
n += 1                              // = += -= *= /= %= &= |= ^= <<= >>=
if (cond) { … } else if (cond) { … } else { … }
switch (rank) { case Rank.Start { … } case Rank.Lab { … } default { … } }
loop { … }                          // break / continue。待機も break もないとエラー
loop (3) { … }                      // 回数指定
return / return value
await ShowMessage("…", face: Face.Smile)
parallel { branch { … } branch { … } }   // 全部終わるまで待つ
race     { branch { … } branch { … } }   // どれかが終わったら残りを中断
wait 1.5                            // 秒
wait frames 2                       // フレーム
yield                               // 次のフレームまで
fire OnSignal(5)                    // 別スクリプトのイベントを発火（待たない）
reset vars                          // このファイルの var を初期値に戻す
@node("n0012") Report(1)            // ノードエディタ用の永続ID（任意）
```

- `switch` の case は定数。フォールスルーしない。値が重複するとエラー。
- `branch` の中から外側の `let` 変数には代入できない（branch ごとにコピーされるため）。var・共有変数は可。`branch` の中では `return` できず、外の loop への `break` / `continue` もできない。
- goto はない（理由は仕様書 §4）。

## 式

優先順位（低い順）：`||`、`&&`、`|`、`^`、`&`、`==` `!=`、`<` `<=` `>` `>=`、`<<` `>>`、`+` `-`、`*` `/` `%`、単項 `-` `!` `~`。
`&&` と `||` は短絡評価。

### 呼び出し

- 引数は位置指定と名前指定（`face: Face.Smile`）を混ぜられる（位置指定が先）。省略した引数はマニフェストの `default`。
- **待機ありのコマンド**（マニフェストで `latent: true`）と、**待機を含む関数**には `await` が必要。それ以外に `await` を付けるとエラー。
- 値を返す呼び出しは式の中で使える（`let a = await SelectWindow(SelectType.YesNo)`）。文として呼んだ場合、値は捨てる。

### 組み込み関数

| 関数 | 内容 |
| --- | --- |
| `int(x)` | float → int（切り捨て）、enum / bool → int |
| `float(x)` | int / enum → float |
| `rand(n)` | 0 以上 n 未満の乱数（VM のシード付き。セーブされる） |
| `min(a, b)` / `max(a, b)` / `abs(x)` | int / float |

## マニフェスト

```yaml
manifest: 1
project: sample_rpg
include:
  - common/core.atsmanifest.yaml      # このファイルからの相対パス

enums:
  Face:
    display: 顔絵
    values: { Normal: 0, Smile: 1 }

variable_banks:
  story:
    scope: persistent                 # persistent（セーブ）/ session（しない）
    vars:
      - { id: 1001, name: chapter, type: int, init: 1 }   # id は一度付けたら変えない

events:
  - name: OnTalk
    params: [ { name: target, type: handle } ]

commands:
  - name: ShowMessage
    display: メッセージ表示
    category: メッセージ
    latent: true                      # 待機あり（呼び出しに await が必要）
    channel: message                  # 同じチャンネルのコマンドは 1 つずつ
    returns: int                      # 省略すると値を返さない
    deprecated: false                 # true にすると使った箇所に警告
    params:
      - { name: text, type: string, display: 本文 }
      - { name: face, type: Face, default: Normal }

queries:                              # 待機しない問い合わせ。returns は必須
  - name: IsQuestCleared
    returns: bool
    params: [ { name: quest_id, type: int } ]
```

- YAML はサブセット（ブロックのマップ・シーケンス、フロー形式 `{}` `[]`、クォート付き文字列、`#` コメント）。アンカー・ブロックスカラー（`|` `>`）・タブによるインデントは使えない。
- イベントがマニフェストにあれば、スクリプトのイベントの引数はそれと一致させる。
- コマンドのシグネチャ（`名前(型,…)->戻り値の型`）のハッシュが `.atsb` に入り、ランタイムの登録内容と照合される。

## atsc

```
atsc compile  <file.ats>... -m <manifest> [-o <out.atsb | dir>] [--json] [--no-debug]
atsc validate <file.ats>... -m <manifest> [--json]
atsc disasm   <file.atsb>
```

- エラーは `file(line,col): error: …`（VS / VS Code で飛べる形式）。`--json` で JSON 配列。
- 複数ファイルを渡すと、スクリプトIDの重複も検査する。
- 終了コード：0 成功、1 エラーあり、2 使い方の誤り。
