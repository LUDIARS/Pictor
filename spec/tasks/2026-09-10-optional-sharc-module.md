---
task: optional-sharc-module
project: Pictor
kind: 実装
status: implemented
created: 2026-09-10
---

# SHaRCを除外可能な描画モジュールに分離する

## 実装内容

PC-RULE-002の最初の実装として、SHaRCをcoreからbackend別targetへ分離。
DX12のdemo専用コンパイルを再利用可能なライブラリへ移し、共通値型をbackend headerから抽出する。
詳細とconsumer移行方法は`spec/feature/optional-sharc-module.md`。

## 受け入れ条件

- 全体OFFでSHaRC source、shader生成、専用demo/test targetを要求しない。
- Vulkan/DX12を個別選択でき、対応環境がない明示要求をエラーにする。
- coreにSHaRCの直接リンク依存を作らず、demoは公開module targetを使う。
- 共通値型にVulkan/DX12型やゲーム実装を持ち込まない。

## 検証

source/include/target/shader依存の読み取り確認と`git diff --check`のみ。
ユーザー指示に従いconfigure・ビルド・単体/統合/起動テストは実行していない。
既存testの登録条件のみ変更し、新規testは追加していない。

## 残作業

他機能の分割を継続する。各demoは固有パイプラインを個別のデフォルトとして整備し、共有・統合しない。
native backendは必要なモジュール単位で対応する。全機能統合は作業対象から除外する。
ルールPR #1649の後続。ローカルmain `5822f253`を基点とし、main更新は行わない。

## PR #1653の審査失敗への対応

Revisorの対象main `1380400ea95c`では、共通shader生成末尾にKuzuha demoの依存が追加されていた。
同じ箇所のSHaRC demo依存ブロック削除が競合したため、その既存ブロックを維持する。
共通shader一覧からのSHaRC除外と、ライブラリ専用shader targetの分離は維持される。
SHaRC全体OFF時はdemo target自体が存在しないため、復元した条件付き依存も発生しない。
mainへの取り込み操作やテストによる再現は行わず、両差分の読み取りで競合箇所を特定した。
