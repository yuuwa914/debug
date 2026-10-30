# ThreadProbe(診断用・読み取り専用)

何も変更せず、5秒ごとに logcat(タグ `ThreadProbe`)へ次を出力します。
- CPU を使っているスレッド上位6本: 名前・tid・直近に動いたコア・CPU%・nice値
- `wait`: 実行可能なのに CPU をもらえず待たされた時間(ms/秒)。高いスレッドは飢餓状態
- jitter: このプローブ自身の100msスリープの遅れ。大きいと、その瞬間 CPU 全体が混雑/制限されている
- CPU周波数・最高温度ゾーン(端末が読み取りを許す場合のみ)

## ビルド
GitHub に push → Actions の build → Artifacts `ThreadProbe-levipack` を取得し、中の `ThreadProbe.levipack` をインポート。
