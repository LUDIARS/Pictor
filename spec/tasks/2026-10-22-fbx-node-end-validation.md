# FBX バイナリのノード終端検証

関連タスク: `actio:81050b79-39b6-4d03-a095-c49cf3ab0bd0`

- C-1 read_node(end_offset): 非 sentinel の終端が入力外または読取済み位置より前なら既存の parse failure で拒否し、成功した反復は必ず前進する。
- C-2 parse_binary(version): 7400/7500 の正常な葉・子ノード・全ゼロ sentinel・sentinel 後の前進パディングを維持する。

`read_node` の既存 sentinel 判定と `set_err` を再利用し、終端の入力上限と読取済み位置を検証する。公開 API・依存関係・ゲーム非依存のフレームワーク境界は変更しない。

回帰テストは専用の `unit_fbx_binary_bounds_test` に置く。ノード開始と同じ終端、ヘッダ/名前/プロパティ/子/sentinel 内へ戻る終端、入力外終端を両バージョンで拒否し、正常系と対にする。異常時の無限反復に備えて CTest のタイムアウトを設定する。

Augur plan は現状の挙動を固定する unit test を提案したため、この異常系と正常系を具体化した。Augur の contract-wrap は TypeScript compiler API による TS/JS の注入であり、C++ の `read_node` には適用できない。架空の実行証跡や別言語の代理契約は追加しない。タスクの明示指示に従い、テスト・サービスは実行しない。C-1/C-2 の動作検証は未実施。
