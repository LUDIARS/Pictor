---
task: kuzuha-stage-rendering
project: Pictor
kind: 実装
created: 2026-09-08
memory_links: []
---
# Fg 葛葉の C案ステージ動画を描画する

## 目的
neco が指定した Astra PN デモと加筆テクスチャを使い、動くステージ光で材質差が分かる10～30秒の映像を作る。

## 完了条件
- 固定録画時刻で動くキー光、読みやすいフィル、暗い背景を実装する。
- 顔と冠の加筆版を実モデルへ載せ、元UVとの適合とアップ映像を確認する。
- 元材質別の反射差を実装し、近似と未実装機能を提出資料に明記する。
- 本体フォルダから Excubitor 経由で制作録画し、Concordia claim/release を行う。
- 動画・採用画像・説明をセッションへ添付し配送記録を確認する。
- commit と Revisor local PR 提出まで。テストの追加実行、マージ、main 更新は行わない。

## スコープ (編集可ディレクトリ)
- demo/kuzuha、demo/fbx_viewer/main.cpp、CMakeLists.txt、関連 spec、Excubitor の専用録画定義。
- モデルとテクスチャ原本は読み取り参照。髪・服の物理と当たり判定は変更しない。
