#!/usr/bin/env python3
"""Claude Docs から書き出した仕様書を docs/spec.md に保存する。

使い方:
  python tools/scripts/export_spec.py <export 結果> [出力先（既定 docs/spec.md）]

<export 結果> は次のどちらか:
  - Claude Docs の export（format: markdown）の結果を保存した JSON
    （[{type, text}] の配列。2 要素目の text の data.bytes_b64 に本文が入っている）
  - そのまま保存した Markdown ファイル（.md）

書き出した Markdown では埋め込み図が「embedded content」の跡になるので、
下の Mermaid 図に差し替える。図を増やしたら DIAGRAMS に追加すること。
"""
import base64
import datetime
import json
import sys

DIAGRAMS = {
    # キャプションに含まれる語 → Mermaid 図
    "全体構成": """```mermaid
flowchart TB
  subgraph tools["共通ツール（全プロジェクトで1つ）"]
    manifest["マニフェスト（YAML）<br/>コマンド・変数・enum の定義"] --> script["スクリプト（.ats）<br/>手書き／将来はノードエディタ"]
    script --> atsc["コンパイラ / atsc<br/>.ats → .atsb を出力"]
  end
  subgraph core["AtomScript コア（C++17・エンジン非依存）"]
    vm["VM<br/>ファイバと待機"]
    vars["変数バンク<br/>フラグ・セーブ／ロード"]
    strs["文字列プール<br/>キーのみ、本文はホスト"]
    dbg["デバッグサーバー<br/>開発ビルドのみ"]
    capi["C API（ats_api.h）：不透明ハンドルとコールバックだけを公開"]
  end
  atsc -- ".atsb を読み込み" --> core
  script -. "デバッグ接続・ホットリロード" .- dbg
  capi <-- "コマンド呼び出しと完了通知" --> unity["Unity パッケージ<br/>C# ラッパー（P/Invoke・SafeHandle）<br/>ScriptedImporter・ネイティブプラグイン"]
  capi <-- "コマンド呼び出しと完了通知" --> ue["Unreal Engine プラグイン<br/>Subsystem・Asset Factory・BP 基底クラス<br/>コアはソースのまま同梱"]
  subgraph game["ゲーム固有（プロジェクトごとに実装）"]
    gu["コマンド実装（C#）"]
    gue["コマンド実装（C++ / Blueprint）"]
  end
  unity --> gu
  ue --> gue
```""",
    "縦型": """```mermaid
flowchart TB
  fadein["フェードイン<br/>演出フェード小"] --> gamewait["ゲーム待機<br/>画面切り替え"]
  gamewait --> sw{"スイッチ"}
  retry1(["retry"]) -. 値 .-> sw
  sw -- "0" --> t1["会話<br/>エル救助開始１"]
  sw -- "1" --> t2["会話<br/>エル救助再開"]
  sw -- "その他" --> t3["会話<br/>エル救助再開２"]
  t1 --> m1(( ))
  t2 --> m1
  t3 --> m1
  m1 --> cam["カメラ解除<br/>固定を終了"]
  cam --> move["プレイヤー移動<br/>倉庫 レッド正面"]
  move --> wait["待機<br/>1.0 秒"]
  wait --> br{"分岐"}
  retry2(["retry <= 1"]) -. 値 .-> br
  br -- True --> t4["会話<br/>エル救助開始２"]
  t4 --> m2(( ))
  br -- False --> m2
  m2 --> battle["戦闘開始<br/>続けて BGM 再生"]
```""",
    "開発フェーズ": """```mermaid
flowchart LR
  p0["段階0<br/>仕様確定：本書のレビューと承認"] --> s1
  subgraph s1["段階1"]
    core["ランタイム：コアVM・C API<br/>単体テスト・ファジング"]
    comp["ツール：コンパイラ・CLI<br/>マニフェストとコード生成"]
  end
  s1 --> g1{{"形式凍結（.atsb・C API v1）"}}
  g1 --> s2
  subgraph s2["段階2"]
    vsc["ツール：VS Code 拡張<br/>補完・エラー表示・デバッガ"]
    uni["Unity 統合<br/>UPM パッケージとサンプル"]
    uei["UE 統合<br/>プラグインとサンプル"]
  end
  s2 --> g2{{"試験タイトルで評価"}}
  g2 --> s3
  subgraph s3["段階3"]
    plat["ランタイム：家庭用機・モバイル対応<br/>各 SDK でのビルドと CI"]
    node["ツール：ノードエディタ<br/>.ats を読み書き（後回し）"]
  end
```""",
}


def load_markdown(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if path.endswith(".md"):
        return text
    arr = json.loads(text)
    data = json.loads(arr[-1]["text"])["data"]
    return base64.b64decode(data["bytes_b64"]).decode("utf-8")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out_path = sys.argv[2] if len(sys.argv) > 2 else "docs/spec.md"
    md = load_markdown(sys.argv[1])

    lines = []
    missing = []
    for line in md.split("\n"):
        if line.startswith("&#91;embedded content:"):
            for key, diagram in DIAGRAMS.items():
                if key in line:
                    lines.append(diagram)
                    break
            else:
                missing.append(line)
                lines.append(line)
        else:
            lines.append(line)
    md = "\n".join(lines)

    today = datetime.date.today().isoformat()
    note = (f"> この文書は Claude Docs の「AtomScript 仕様書（ドラフト）」から書き出したもの（{today} 時点）。"
            "図は Mermaid で描き直している。\n")
    md = md.replace("\n\n", "\n\n" + note + "\n", 1)

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(md)
    print(f"wrote {out_path}")
    for m in missing:
        print(f"warning: 差し替える図がありません: {m}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
