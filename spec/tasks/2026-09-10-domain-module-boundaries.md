---
task: domain-module-boundaries
project: Pictor
kind: 実装
status: implemented
created: 2026-09-10
---

# ドメイン再編とポストプロセス・デカールの選択モジュール化

## 実装内容

UXを維持して8コア・23ビジネスへ再編し、機能単位のソース対応をAnatomia taxonomyへ登録する。
照明・影・遮蔽はビジネスドメイン、UIは汎用グラフィクス、計測は開発支援。
postprocessとdecalをcoreから独立targetへ分離し、不要時に実装・shader生成を除外する。

## 受け入れ条件

- ドメインのコア所属と、照明・影・遮蔽内の機能を明示する。
- VisusのDoDとパイプライン双方の所属を維持する。
- 利用側がpostprocess/decalを個別にリンクし、OFFでは専用実装・shaderを要求しない。
- 各demo固有の設計とモジュール単位のnative対応を守る。

## 確認と残作業

Anatomiaは専用worktreeの静的解析snapshotとして登録し、Pfからそのsnapshotに関連付ける。
本体mainは更新しない。解析対象を本体の反映済みコードと混同しない。
差分・依存の読取りとAnatomia静的解析のみ。configure・ビルド・テスト・起動は実行しない。
各効果のさらに細かい分割、他機能のモジュール化、各demoの個別デフォルト整備は残作業。
