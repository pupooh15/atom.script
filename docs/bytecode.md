# AtomScript バイナリ形式（.atsb） v1.0

VM とコンパイラ（atsc）の取り決め。定義の実体は [`include/atomscript/ats_format.h`](../include/atomscript/ats_format.h)。

- すべてリトルエンディアン。構造体はパディングなし。
- 読み込み時に VM がすべての参照・ジャンプ先・スタック深さを検証する。壊れたデータでメモリを壊さない。
- major バージョンが違えば読み込みを拒否する。minor が新しい場合、未知のセクションは読み飛ばす。

## ファイル構造

```
FileHeader (32 bytes)
SectionEntry × section_count (16 bytes each)
各セクションの中身（8 バイト境界に揃える）
```

| フィールド | 型 | 内容 |
| --- | --- | --- |
| magic | u32 | `ATSB` |
| version_major / minor | u16 / u16 | 1 / 0 |
| flags | u32 | 予約 |
| file_size | u32 | ファイル全体のサイズ |
| manifest_hash | u64 | コンパイル時のマニフェストのハッシュ |
| script_id | u32 | スクリプトID（STRS のインデックス）。セーブデータのキー |
| section_count | u32 | セクション数 |

## セクション

| タグ | 必須 | 中身 |
| --- | --- | --- |
| `STRS` | ○ | u32 count, u32 offset[count]（セクション先頭から）, NUL 終端の UTF-8 文字列 |
| `CNST` | | u32 count, u64 value[count] |
| `IMPT` | | u32 count, ImportEntry[count]（24 bytes） |
| `VARS` | | u32 count, VarEntry[count]（24 bytes） |
| `ENTR` | ○ | u32 count, EntryEntry[count]（24 bytes） |
| `CODE` | ○ | 命令列 |
| `DBUG` | | u32 count, DebugEntry[count]（12 bytes, pc の昇順） |

### ImportEntry（コマンド・クエリ）

名前で参照し、読み込み時にランタイムの登録表と照合する。足りない名前はすべて列挙してエラーにする。
`sig_hash` が両方 0 以外で一致しなければエラー。引数は最大 8 個、戻り値は 0 か 1 個。クエリは必ず値を返す。

### VarEntry（変数）

| kind | 参照方法 | 備考 |
| --- | --- | --- |
| 0：共有変数 | `id`（マニフェストの安定ID） | ランタイムに `ats_define_var` で登録済みであること |
| 1：スクリプト変数 | `name` | `flags` bit0 = transient（セーブしない）。`was_name` = `@was` の旧名。`init` は初期値（STRING は STRS のインデックス） |

### EntryEntry（イベント・関数）

`localc` は引数を含むローカル数。`max_stack` は演算スタックの最大深さ（コンパイラが計算する）。
イベントは値を返せない。関数は `OP_CALL_FN` で呼ぶ。

## 実行モデル

- スタックの 1 要素は 64bit。int / bool / enum は下位 32bit、float はビット列、string は VM 内の文字列 ID、handle は 64bit 全体。
- フレームは `[ローカル][演算スタック]` の順に積む。ファイバ 1 本の上限は `ats_vm_desc::max_stack`（既定 1024 要素）。
- ジャンプのオフセットは「次の命令の先頭」からの相対値（i32）。

## 命令

1 バイトのオペコード + 固定長オペランド。

| 命令 | オペランド | スタック | 内容 |
| --- | --- | --- | --- |
| NOP | | | |
| PUSH_I32 / PUSH_F32 | i32 / f32 | → v | 即値 |
| PUSH_K | u32 | → v | 定数（CNST） |
| PUSH_STR | u32 | → s | 文字列（STRS） |
| POP / DUP | | v → / v → v v | |
| LD_LOCAL / ST_LOCAL | u16 | → v / v → | ローカル |
| LD_VAR / ST_VAR | u16 | → v / v → | 変数（VARS のインデックス） |
| ADD/SUB/MUL/DIV/MOD_I, NEG_I | | a b → r | 整数。オーバーフローは折り返し。0 除算はファイバを中断 |
| ADD/SUB/MUL/DIV_F, NEG_F | | a b → r | 実数 |
| BAND/BOR/BXOR/SHL/SHR, BNOT | | a b → r | ビット演算（SHR は算術シフト） |
| NOT | | a → r | 論理否定 |
| EQ/NE/LT/LE/GT/GE_I, …_F | | a b → bool | 比較（文字列の一致は EQ_I） |
| EQ_H / NE_H | | a b → bool | 64bit 比較（handle） |
| I2F / F2I | | a → r | 変換（F2I は範囲外を飽和、NaN は 0） |
| JMP / JZ / JNZ | i32 | / c → / c → | 分岐 |
| SWITCH | u16 n, i32 default, {i32 値, i32 先} × n | v → | 多分岐 |
| CALL_CMD | u16 import, u8 argc | args → [ret] | コマンド。待機する場合がある |
| CALL_QUERY | u16 import, u8 argc | args → ret | クエリ（その場で値を返す） |
| CALL_FN | u16 entry | args → [ret] | 関数呼び出し |
| RET | | [ret] → | 関数から戻る。最外のフレームならファイバ終了 |
| FORK | i32 | | 子ファイバを作る。子はローカルのコピーと空の演算スタックで開始 |
| JOIN | | | 子ファイバがすべて終わるまで待つ |
| RACE | | | 子ファイバのどれかが終わるまで待ち、残りを中断（待機中のコマンドにはキャンセル通知） |
| YIELD | | | 次のフレームまで待つ |
| SLEEP | | sec → | 秒数待つ |
| WAIT_FRAMES | | n → | フレーム数待つ |
| END | | | ファイバ終了 |
| FIRE | u32 event, u8 argc | args → | 結び付いている全スクリプトの同名イベントを発火（待たない） |
| RESET_VARS | | | このスクリプトの var を初期値に戻す |
| RAND | | n → r | [0, n) の乱数（VM のシード付き生成器。状態はセーブされる） |

### parallel / race の展開例

```
    FORK  branch1
    FORK  branch2
    JMP   join
branch1:
    …
    END
branch2:
    …
    END
join:
    JOIN        ; race なら RACE
```

## コマンド呼び出しの取り決め

ハンドラの戻り値：

| 戻り値 | 動き |
| --- | --- |
| `ATS_DONE` | 完了。結果（`ats_call_set_result`）を積んで次の命令へ |
| `ATS_PENDING` | 待機。ホストが後で `ats_call_complete` / `ats_call_fail` を呼ぶ |
| `ATS_RUNNING` | 待機。次の update で同じハンドラをもう一度呼ぶ（`ats_call_retry_count` が増える） |
| `ATS_FAIL` | 失敗。ファイバを中断する |

チャンネル付きのコマンドは、同じチャンネルのコマンドが完了するまで実行を待つ（先着順）。
ファイバが中断されたとき、待機中のコマンドに `cancel` コールバックでトークンを通知する。

## セーブデータ（ats_vm_save）

保存するのは共有変数（persistent）・スクリプト変数（transient 以外）・乱数の状態だけ。実行中のファイバは保存しない。

```
u32 'ATSS', u16 major(1), u16 minor(0), u64 乱数状態
u32 共有変数の数,   { u32 id, u8 type, 値 }
u32 スクリプト数,   { 文字列 script_id, u32 変数の数, { 文字列 name, u8 type, 値 } }
値：STRING なら文字列、それ以外は u64。文字列：u32 長さ + バイト列
```

- 共有変数は安定ID、スクリプト変数は「スクリプトID＋変数名」で照合する。追加・削除・並べ替えに強い。
- まだ結び付いていないスクリプトの値は「休眠データ」として保持し、結び付いた時点で適用する。次のセーブにもそのまま書き出す。
- 名前を変えた変数は `@was`（VarEntry の was_name）で旧名から引き継ぐ。
- 型が変わった変数は変換できれば変換し（int ↔ float ↔ bool）、できなければ初期値にして警告を出す。
- ロードは全体を読んで検証してから適用する。途中で失敗しても現在の状態は変わらない。
