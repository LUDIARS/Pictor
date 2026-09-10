# Pictor フレームワーク境界のルール

2026-09-10 neco 指示。以下は設計案ではなく、今後の実装・レビューに適用するルール。
既存コードの移行状況は末尾の「現状と移行」で区別する。

## 目的

UX は「DoDのモダンなレンダリングフレームワーク」。コアドメインは次の4つ。

- DoDによる高速描画
- Web/PC/Android/iOSのマルチプラットフォーム
- フレキシブルレンダーパイプライン
- 多様な描画機能

フレキシブルレンダーパイプラインには、レンダリングパイプラインの調整機構と
Visusが入る。VisusはDoDによる高速描画にも帰属する。

## PC-RULE-001: ゲーム非依存

Pictorはゲーム依存の実装を持たない。ゲーム開発では通常Ergoと組み合わせるが、
描画ツール、ビューア、可視化、UIなどの非ゲーム用途でも任意のhostと組み合わせられる。

- Pictorの公開API、資源管理、パイプライン、機能モジュールはErgoを必須にしない。
  依存方向は consumer → Pictor。ゲームルール、ゲーム状態、勝敗、戦闘、進行、
  プレイヤー操作の意味付け、特定ゲームのscene/entity型を下層へ持ち込まない。
- hostはwindow/layerとapplication loopを用意し、ライフサイクルと入力の意味付けを主導する。
  Pictorの任意のplatform adapterがwindow/layerを生成・所有してもよいが、共通coreの
  必須依存にしない。Pictorは描画データ、カメラ、描画資源、
  フレーム/描画コマンドの契約を受け取る。
- Ergoとのadapterやゲーム固有処理はErgoまたはconsumer側が所有する。
  汎用の骨格、アニメーション、空間分割、GUI描画などは、ゲーム状態に依存しない
  描画上の責務として提供してよい。
- Pictorのdemo/testへ上位ゲームライブラリを持ち込まない。汎用サンプルhostで
  利用経路を示す。特定ゲームのassetや保存形式を既定動作の前提にしない。
- Vulkan、Metal、DirectX等のbackend依存はそのbackend内へ閉じ込め、
  共通のモジュール契約へゲーム型や特定backendの必須型を漏らさない。

## PC-RULE-002: 必要な描画機能を組み合わせる

SHaRCを含む描画機能は責務ごとにモジュール化し、不要なものを外せるようにする。
SHaRCを全用途共通の必須描画経路にしない。

- モジュールは公開契約、依存モジュール、対応backend、必要なshader/asset、
  資源の所有者と解放手順を明示する。CPUの意味処理とbackend実装を区別する。
- ビルドから除外したモジュールは、実装コード、外部ライブラリ、shader生成、
  専用demoのビルドを要求しない。必須のcoreからその実装を直接参照しない。
- 実行時にパイプラインから外したモジュールは、専用GPU/CPU資源を確保せず、
  dispatch、draw、timer、worker、callback等を実行しない。既存資源は所有者が解放する。
  単に出力を隠すだけ、定数を0にするだけの実装で「切れる」と扱わない。
- 依存関係は閉包として検証する。使用すると指定されたモジュールが未ビルド、
  必須依存欠落、または非対応backendの場合は明示エラーにする。
  空の処理や別の描画へ黙って置き換えない。
- 選択可能という契約は動的pluginロードを必須としない。静的リンク、build option、
  registry等の具体方式は既存構成と責務に合わせて決め、方式自体を目的にしない。
- SHaRC、GI/影/AO、postprocess、文字/UI、vector/Rive、animation等を分割候補とし、
  module一覧と相互依存を正本で管理する。backendと機能の選択を混同しない。

## PC-RULE-003: demoパイプラインを再利用可能なデフォルトとして提供する

各描画demoで使用するレンダリングパイプラインを、consumerが選択できる既定値として
提供する。一つの万能なglobal defaultへ統合するという意味ではない。

- 各描画demoは共有された名前付きpresetを選択する。demoと配布用presetの双方に
  パス構成・設定を別々にハードコードして二重管理しない。
- presetは安定したID、版、必要モジュール、対応backend、パスと資源の構成、
  初期パラメータ、必要なshader/assetを明示する。
- demo以外のhostからも同じ公開loader/factory/registry経由で選択し、値を上書きできる。
  demo executableやdemo内部のmain関数を利用条件にしない。
- カメラ操作、モデル配置、ゲーム状態、private assetのパス、開発機の絶対パスは
  パイプラインpresetに含めない。描画契約とサンプルscene/host入力を分離する。
- backend差分や対応していないpassを明示する。VulkanのpresetがそのままMetal/DX12でも
  動くと見せない。対応外は読み込み・構築時に明示エラーにする。
- 既存の品質profileと実render-pass構成は区別する。品質profileを保存しただけでは、
  demoの実パイプラインを再利用できるようにしたことにはならない。
- shader/packageはconsumerへ配布できる構成にする。demoの作業ディレクトリに依存しない。
- 描画パイプラインを持たないimporter/変換/解析用demoは対象外と明記し、架空のpresetを作らない。

## レビュー時に確認すること

モジュールを外したconsumerが、そのモジュールのリンク・shader・初期化を要求しないか。
モジュールを要求したのに実装が無い構成を明示的に拒否するか。
同じpresetをdemoと非ゲームhostが利用できるか。
Ergoの型・ゲームルール・private assetが共通境界へ入っていないか。

テストや起動の実行はユーザーの明示許可がある場合のみ。ルールの記録を検証成功に読み替えない。

## 現状と移行

確認基準: ローカルmain `5822f253`。既存 `profiles/*.profile.json` は8種類の品質profile。
SHaRCのVulkan実装とdemoはCMakeの主要構成に含まれ、DX12 SHaRCは専用targetを持つ。
Riveは既存optionで選択できるが、全描画機能に統一的な除外契約が成立しているわけではない。
demoの実パイプラインを共有presetから選ぶ経路は全demoへ普及していない。

移行は次の単位で進める。未完了を既定対応として宣伝しない。

1. モジュール一覧と依存を棚卸し、SHaRCからbuild/link/shader/demoの選択境界を分離する。
2. coreが持つ具体機能への依存を狭い契約へ置き換え、他の描画機能も選択可能にする。
3. 描画demoの実パスを一覧化し、共有presetへ抽出して各demoとconsumerを接続する。
4. backend別の対応表と、非ゲームhost/Ergo側のconsumer例を整備する。

関連: `../feature/pipeline-profile-config.md`、`../feature/pipeline-system-b-config.md`、
`../feature/pipeline-hot-reload.md`、`../feature/visus-v2-design.md`。
