# Pictor — Rendering Pipeline Module

## Technical Design Document — Data-Driven Edition

**Version 2.1** | 2026.03

---

# 1. Overview

## 1.1 Pictorとは

Pictorは、描画オブジェクトの一元管理と最適な描画パイプラインの自動選択を行う、独立したレンダリングパイプラインモジュールである。UnityのURP/HDRPのように複数のレンダリングプロファイルを提供し、プロジェクト要件に応じて切り替え可能な設計とする。

## 1.2 設計目標

- データ駆動設計（Data-Oriented Design）を基盤とし、CPUキャッシュとGPUメモリ帯域の理論的最大効率を追求
- 全描画データをSoA（Structure of Arrays）で管理し、処理パス毎に必要なストリームのみを走査

- オブジェクト分類結果を物理的に分離した連続配列で保持し、フレーム毎のフラグ分岐を排除
- 100万オブジェクト規模のデータ更新をGPU Compute Updateで完結させ、CPU→GPUアップロードを排除

- GPU Driven Renderingにおいても GPU側メモリをSoA化し、Compute Shaderのメモリ帯域を最大化
- フレーム一時データを全てリニアアロケータで確保し、ヒープアロケーションを排除

- 組込みプロファイラでFPS・各パスのGPU/CPU時間・メモリ統計をリアルタイム可視化
- 複数のデフォルトパイプラインプロファイルを用意し、ランタイムでも切替可能

## 1.3 用語定義

|                 |                                                                                      |
|-----------------|--------------------------------------------------------------------------------------|
| **用語**        | **説明**                                                                             |
| SoA Stream      | 同一属性のデータだけを連続配列として格納する構造。処理パスが必要なデータのみ走査可能 |
| Hot Data        | 毎フレームアクセスされるデータ（BoundingVolume, Transform等）                        |
| Cold Data       | 変更時のみアクセスされるデータ（MeshHandle, MaterialHandle等）                       |
| Object Pool     | Static / Dynamic / GPU Driven の分類毎に物理分離された連続配列群                     |
| Frame Allocator | フレーム開始時にポインタリセットするリニアバンプアロケータ                           |
| Compute Update  | GPU Compute Shaderでtransforms SSBOを直接書き換えるデータ更新方式                    |
| NT Store        | Non-Temporal Store。キャッシュ階層をバイパスしメモリに直接書き込むストア命令         |
| RenderBatch     | 同一シェーダ・同一属性のオブジェクトをまとめた描画単位                               |
| PipelineProfile | レンダリングパスの構成定義。URP/HDRPに相当する切替可能なプリセット                   |
| Flat BVH        | ポインタレスのノード配列によるBVH。Van Emde Boasレイアウトでキャッシュ最適化         |

# 2. Architecture

## 2.1 全体構成

Pictorは大きく3つのレイヤーで構成される。各レイヤー間のデータ受け渡しはSoAストリームの参照（ポインタ + count）で行い、データのコピーを最小限に抑える。

### Front-end Layer（オブジェクト管理層）

オブジェクトの登録・更新・削除を担当する。登録時にObjectClassifierが属性を判定し、対応するObject Pool（Static Pool / Dynamic Pool / GPU Driven Pool）に直接配置する。フラグ判定は登録・属性変更時のみ行い、毎フレームの走査では分岐が発生しない。

### Middle Layer（バッチング・ソーティング層）

各Object Poolから必要なSoAストリーム（shaderKeyストリーム等）だけを読み取り、RenderBatchを生成する。ソートはRadix Sortにより O(n) で実行し、sortKey + index のペア配列のみソートして実データは間接参照する。

### Back-end Layer（コマンド発行層）

PipelineProfileに基づきRenderPassの実行順序を決定し、各パスで対応するバッチからDrawCommandを生成・GPU発行する。フレーム一時データは全てFrame Allocatorから確保する。

## 2.2 モジュール構成

|                     |                                                                    |                                 |
|---------------------|--------------------------------------------------------------------|---------------------------------|
| **モジュール**      | **責務**                                                           | **依存先**                      |
| SceneRegistry       | Object Poolの管理、SoAストリームの確保・拡張                       | MemorySubsystem                 |
| ObjectClassifier    | 登録時のStatic/Dynamic/GPU Driven判定とPool振り分け                | SceneRegistry                   |
| UpdateScheduler     | データ更新戦略の選択と実行（CPU並列 / Compute Update）             | SceneRegistry, GPUBufferManager |
| BatchBuilder        | Pool内のshaderKeyストリームからバッチ生成（インデックス間接参照）  | SceneRegistry                   |
| SortSystem          | Radix Sortによるkey-index pairソート                               | BatchBuilder                    |
| GPUBufferManager    | メッシュプール、SoA SSBO、インスタンスバッファのサブアロケーション | SceneRegistry                   |
| CullingSystem       | Flat BVH走査、Hi-Z GPU Culling                                     | SceneRegistry, GPUBufferManager |
| RenderPassScheduler | PipelineProfileに基づくパス順序決定                                | PipelineProfile                 |
| CommandEncoder      | DrawCommand生成とCommandBuffer記録                                 | 全上位モジュール                |
| MemorySubsystem     | Frame Allocator、Pool Allocator、GPU Memory Allocator              | なし（最下位）                  |
| Profiler            | FPS計測、パス別GPU/CPUタイマー、メモリ統計、オーバーレイUI         | 全モジュール（計測対象）        |

# 3. SoA Data Model

## 3.1 設計原則: 構造体の廃止

全ての描画オブジェクトデータは、同一インデックスで紐付けられたSoAストリームとして管理する。objectId=N のオブジェクトのトランスフォームは transforms[N]、バウンディングボリュームは bounds[N] のように、各配列の同じスロットに格納される。

> **> **> **[Design Note]** **** *v1.0の RenderObject 構造体を廃止。全フィールドを用途別の独立した連続配列（SoAストリーム）に分離し、各処理パスが必要なストリームだけを走査する設計に変更。*

## 3.2 SoAストリーム定義

### Stream Group A: カリング用（Hot — 毎フレーム読取）

|                     |                                |                 |                                                |
|---------------------|--------------------------------|-----------------|------------------------------------------------|
| **ストリーム**      | **型**                         | **サイズ/要素** | **アクセスパターン**                           |
| bounds[]          | AABB (float3 min + float3 max) | 24 bytes        | 毎フレーム: Frustum Culling, BVH走査           |
| visibilityFlags[] | uint8                          | 1 byte          | 毎フレーム: カリング結果書込・バッチ構築時読取 |

### Stream Group B: ソート・バッチ用（Hot — 毎フレーム読取）

|                  |        |                 |                                    |
|------------------|--------|-----------------|------------------------------------|
| **ストリーム**   | **型** | **サイズ/要素** | **アクセスパターン**               |
| shaderKeys[]   | uint64 | 8 bytes         | 毎フレーム: バッチキー生成、ソート |
| sortKeys[]     | uint64 | 8 bytes         | 毎フレーム: Radix Sort対象         |
| materialKeys[] | uint32 | 4 bytes         | 毎フレーム: テクスチャバインド判定 |

### Stream Group C: トランスフォーム（Hot — Dynamicのみ毎フレーム書込）

|                    |          |                 |                                            |
|--------------------|----------|-----------------|--------------------------------------------|
| **ストリーム**     | **型**   | **サイズ/要素** | **アクセスパターン**                       |
| transforms[]     | float4x4 | 64 bytes        | Dynamic: 毎フレーム書込 / Static: 初回のみ |
| prevTransforms[] | float4x4 | 64 bytes        | モーションベクター用。前フレームコピー     |

### Stream Group D: メタデータ（Cold — 変更時のみ）

|                      |                         |                 |                      |
|----------------------|-------------------------|-----------------|----------------------|
| **ストリーム**       | **型**                  | **サイズ/要素** | **アクセスパターン** |
| meshHandles[]      | MeshHandle (uint32)     | 4 bytes         | 登録・変更時のみ     |
| materialHandles[]  | MaterialHandle (uint32) | 4 bytes         | 登録・変更時のみ     |
| lodLevels[]        | uint8                   | 1 byte          | LOD切替時のみ        |
| flags[]            | uint16 (bitfield)       | 2 bytes         | 登録・属性変更時のみ |
| lastFrameUpdated[] | uint32                  | 4 bytes         | 更新検知用           |

> **> **> **[Design Note]** **** *Cold Dataは Hot Data と異なるメモリページに配置する。毎フレームの走査時にCold Dataのキャッシュライン読込が発生せず、L1/L2キャッシュをHot Dataが独占できる。*

## 3.3 属性フラグ定義

|         |                |                                                    |
|---------|----------------|----------------------------------------------------|
| **Bit** | **名称**       | **説明**                                           |
| 0       | STATIC         | 静的オブジェクト（Pool判定用。走査時は参照しない） |
| 1       | DYNAMIC        | 動的オブジェクト（Pool判定用。走査時は参照しない） |
| 2       | GPU_DRIVEN     | GPU側処理対象（Pool判定用。走査時は参照しない）    |
| 3       | CAST_SHADOW    | シャドウマップ書込対象                             |
| 4       | RECEIVE_SHADOW | シャドウ受け対象                                   |
| 5       | TRANSPARENT    | 半透明。TransparentPassで描画                      |
| 6       | TWO_SIDED      | 両面描画                                           |
| 7       | INSTANCED      | インスタンシング描画対象                           |
| 8-9     | LAYER (2bit)   | レンダリングレイヤー（0-3）                        |
| 10-15   | RESERVED       | 将来拡張用                                         |

# 4. Memory Architecture

## 4.1 Object Pool: 分類による物理的配列分離

登録時にObjectClassifierが判定した結果に基づき、オブジェクトを物理的に異なる3つのObject Poolに配置する。各Poolは独立したSoAストリーム群を持ち、先頭から末尾まで分岐なしで走査できる。

|                 |                                  |                               |                                           |
|-----------------|----------------------------------|-------------------------------|-------------------------------------------|
| **Pool**        | **内容**                         | **走査方式**                  | **バッチ戦略**                            |
| Static Pool     | トランスフォーム不変オブジェクト | シーンロード時 + Dirty時のみ  | インデックス間接参照 + Base Vertex Offset |
| Dynamic Pool    | 毎フレーム更新オブジェクト       | 毎フレーム先頭から末尾まで    | インスタンスバッファ更新 → Instanced Draw |
| GPU Driven Pool | GPU側処理オブジェクト            | CPU: 初回のみSSBOアップロード | Compute Culling → Indirect Draw           |

### Pool間移動

属性変更が発生した場合、Swap-and-Pop方式で元Poolから削除し、新Poolの末尾に追加する。objectIdからPoolインデックスへの逆引きはハッシュマップで管理する。

## 4.2 ホット/コールド分離

- Hot Region: bounds[], shaderKeys[], sortKeys[], transforms[] 等。キャッシュライン64B境界アライメント。ラージページ（2MB）推奨
- Cold Region: meshHandles[], materialHandles[], flags[] 等。Hot Regionとは異なるページに配置

## 4.3 Frame Allocator（リニアバンプアロケータ）

|                |                                                                                           |
|----------------|-------------------------------------------------------------------------------------------|
| **プロパティ** | **説明**                                                                                  |
| 確保方式       | ポインタをsizeだけ進めるバンプアロケーション。O(1)、ロックフリー                          |
| 解放方式       | フレーム終了時にポインタを先頭にリセット。個別解放は不可（不要）                          |
| Flight管理     | ダブル/トリプルバッファリング対応。Frame N-2 のアロケータをFrame Nで再利用                |
| 使用対象       | カリング結果、ソート用key-index pair、Indirect Drawステージング、コマンド記録一時バッファ |
| 初期サイズ     | Standard: 16MB、Ultra: 64MB。不足時はページ追加                                           |

## 4.4 メモリ予算

|                       |                    |                     |                    |
|-----------------------|--------------------|---------------------|--------------------|
| **カテゴリ**          | **Pictor Lite**    | **Pictor Standard** | **Pictor Ultra**   |
| Object Pool (CPU)     | 5K x ~120B = 0.6MB | 50K x ~120B = 6MB   | 1M x ~120B = 120MB |
| Frame Allocator (CPU) | 4MB x 2 flight     | 16MB x 3 flight     | 64MB x 3 flight    |
| Mesh Pool (GPU)       | 128MB              | 256MB               | 1GB                |
| Instance/SSBO (GPU)   | 32MB               | 128MB               | 512MB              |
| Render Target (GPU)   | 64MB               | 256MB               | 512MB              |
| 合計 GPU              | ~256MB             | ~512MB              | ~2GB               |

# 5. Data Update Strategy

> **> **> **[Design Note]** **** *本章はv2.1で新設。100万オブジェクト規模で全オブジェクトが毎フレーム独立に移動するケースを前提に、データ更新の効率化戦略を3段階で規定する。*

## 5.1 課題: 大規模データ更新のボトルネック

1,000,000オブジェクトの transforms[] を毎フレーム更新する場合、書込データ量は 1M x 64B = 64MB/frame となる。60fpsでは 3.84GB/s の書込帯域が必要であり、DDR4-3200 の理論帯域（25.6GB/s）の約15%を消費する。さらに、CPU → GPU アップロードが発生すると PCIe帯域（Gen3 x16: 15.75GB/s）も圧迫する。

この規模のデータ更新を効率的に処理するには、以下の3段階の戦略を組み合わせる。

## 5.2 Level 1: マルチスレッド並列 + SoAレンジ分割

transforms[] がSoAの連続配列であるため、配列をレンジ分割して各レンジを独立したジョブとして並列実行できる。SoA配列上の隣接レンジはキャッシュラインを共有しないよう、レンジ境界を64Bアライメントに揃える。

|                |                                         |                                |
|----------------|-----------------------------------------|--------------------------------|
| **パラメータ** | **説明**                                | **デフォルト値**               |
| chunkSize      | 1ジョブあたりのオブジェクト数           | 16,384 (64B x 16K = 1MB/chunk) |
| jobCount       | ceil(objectCount / chunkSize)           | 1M / 16K = 62 jobs             |
| workerThreads  | CPU論理コア数 - 1（メインスレッド除く） | 7 (8コアCPU想定)               |
| alignment      | レンジ境界のアライメント                | 64B（キャッシュライン）        |

各ジョブは UpdateJob(startIndex, endIndex) として発行される。ジョブ内ではユーザー提供の更新関数（IUpdateCallback::Update(transforms + start, bounds + start, count)）を呼び出し、transforms[] と bounds[] を一括更新する。

> **> **> **[Design Note]** **** *ジョブシステムはPictorが内蔵する。外部ジョブシステム（例: Enki, JobSystem等）への差替えはIJobDispatcherインターフェースで対応可能。*

## 5.3 Level 2: Non-Temporal Store（NTストア）

transforms[] の更新は「書いたデータを同フレーム内で再度読むことが少ない」パターンに該当する（書込 → GPUアップロード → 次フレームまで不要）。このため、通常のストア命令ではなくNTストア（\_mm256_stream_ps等）を使用してキャッシュ階層をバイパスし、Write-Combineバッファ経由でメモリに直接書き込む。

- 利点: transforms[] の書込がL1/L2キャッシュを汚染しない。カリング用の bounds[] がキャッシュに留まりやすくなる
- 利点: 書込帯域がキャッシュ→メモリのエビクション帯域と競合しない

- 制約: NTストアは64B（キャッシュライン）単位で書くと最大効率。float4x4（64B）がちょうど1キャッシュラインなので最適
- 制約: 書込先がページテーブルに載っている必要あり。ラージページ推奨

## 5.4 Level 3: GPU Compute Update（最重要）

オブジェクトの移動ロジックがGPUで計算可能な場合（sin波移動、パーティクル物理、フロッキング等）、Compute Shaderで gpu_transforms SSBO を直接書き換えることで、CPU側の transforms[] 更新と CPU→GPU アップロードを完全に排除する。

> **> **> **[Design Note]** **** *100万オブジェクト規模ではこれが最も効果的な戦略。CPU側の64MB書込 + PCIeアップロードが消滅し、GPUの内部メモリ帯域（例: RTX 4070: 504GB/s）で全て完結する。*

### Compute Update処理フロー

1.  CPU: 更新パラメータのみをUniformバッファにアップロード（deltaTime, フレーム番号等。~64B）
2.  GPU Compute [Update Pass]: gpu_transforms SSBO を直接書き換え。1 thread = 1 object

3.  GPU Compute [Bounds Update Pass]: 更新された transform から gpu_bounds を再計算
4.  GPU Compute [Cull Pass]: 更新済み gpu_bounds でカリング実行（以降は既存パイプライン）

Update PassとBounds Update Passは1つのCompute Shaderに統合可能。dispatch(ceil(objectCount / 256), 1, 1) で100万オブジェクトを約3,907ワークグループで処理する。

### Compute Update用SSBO追加

|                   |                     |                   |                                                    |
|-------------------|---------------------|-------------------|----------------------------------------------------|
| **SSBO名**        | **内容**            | **サイズ/要素**   | **説明**                                           |
| gpu_velocities    | float3 速度ベクトル | 12 bytes          | オブジェクト毎の移動速度。Compute Updateで読み書き |
| gpu_update_params | float4x4 + misc     | ~128 bytes (全体) | deltaTime, frameNumber, gravity等のフレーム定数    |

## 5.5 更新戦略の自動選択

UpdateSchedulerは以下の判定ロジックで最適な更新戦略を自動選択する。

|                                              |                                     |                                                              |
|----------------------------------------------|-------------------------------------|--------------------------------------------------------------|
| **条件**                                     | **選択される戦略**                  | **理由**                                                     |
| GPU Driven Pool + Compute Update対応シェーダ | Level 3 (GPU Compute Update)        | CPU帯域消費ゼロ。100万オブジェクトで最速                     |
| Dynamic Pool + objectCount > 10,000         | Level 1 + Level 2 (並列 + NTストア) | 大規模CPUバッチ。キャッシュ汚染防止が効果的                  |
| Dynamic Pool + objectCount <= 10,000        | Level 1 のみ (並列)                 | 小規模ではNTストアのレイテンシオーバーヘッドが相対的に大きい |
| Static Pool                                  | 更新なし                            | トランスフォーム不変                                         |

## 5.6 CPU → GPU アップロード戦略

Level 1/2で CPU側 transforms[] を更新した場合、GPU側への転送が必要になる。以下の方式を採用する。

- Staging Buffer（Persistent Mapped）にNTストアで書き込み、GPU側でコピー発行
- 100万オブジェクトの場合、Staging = 64MB。トリプルバッファリングで 64MB x 3 = 192MB

- Dirty Region Tracking: transforms[] を16K単位チャンクに分割し、更新のあったチャンクのみコピー
- 全オブジェクトが動く場合はDirty Trackingを省略し、全領域を一括コピー（Dirty判定のオーバーヘッド回避）

**ただし、100万オブジェクト規模で全オブジェクトが動くケースでは Level 3（GPU Compute Update）を強く推奨する。CPU→GPUの64MB転送自体を排除できるため。**

# 6. Batching System

## 6.1 バッチング戦略

Pictorのバッチングは各Object Poolのストリームを先頭から末尾まで走査し、shaderKey + materialKeyの一致するオブジェクト群をRenderBatchにまとめる。分類フラグの条件分岐は存在しない。

### Static Pool バッチ構築

メッシュデータはMesh Pool上に配置済みのまま触らない。バッチはインデックスバッファの参照リスト（Base Vertex Offset + Index Offset + Index Count の3-tuple配列）として表現し、Multi Draw Indirectで1回のAPI呼び出しで描画する。

### Dynamic Pool バッチ構築

同一shaderKeyのオブジェクト群をInstanced Drawとしてバッチ化する。トランスフォームはSSBOまたはInstance Bufferにフレーム毎アップロード。

### GPU Driven Pool バッチ構築

CPU側ではバッチ構築を行わない。Compute Shaderがカリング・LOD選択・DrawCommand生成を全て行う。

## 6.2 ソートシステム: Radix Sort

sortKeyはuint64の合成キーであり、Radix Sort（LSB, 8-bit digit, 8パス）で安定ソートする。ソート対象はsortKey + objectIndex のペア配列（16B/要素）のみ。実データは一切移動しない。

|                |                        |                               |
|----------------|------------------------|-------------------------------|
| **ビット範囲** | **内容**               | **目的**                      |
| 63-60 (4bit)   | RenderPass ID          | パス順序グルーピング          |
| 59-56 (4bit)   | Transparency           | Opaque → Transparent順序制御  |
| 55-40 (16bit)  | Shader Key (上位16bit) | ステート切替最小化            |
| 39-24 (16bit)  | Material Key           | テクスチャバインド切替削減    |
| 23-0 (24bit)   | Depth                  | Front-to-Back / Back-to-Front |

## 6.3 バッチ無効化と再構築

- Static Pool変更 → インデックスリストの差分更新のみ
- シーン遷移 → 対象Pool領域の再構築

- PipelineProfile切替 → 全バッチ再構築

# 7. GPU Driven Rendering

## 7.1 概要

GPU Driven Poolに配置されたオブジェクトは、CPU側のカリングを省略し、GPUのCompute Shaderでカリング・LOD選択・DrawCommand生成を全て行う。Ch.5のCompute Updateと組み合わせることで、100万オブジェクトの更新から描画までをGPU内で完結させる。

## 7.2 処理フロー（Compute Update統合版）

5.  GPU Compute [Update Pass]: gpu_transforms / gpu_bounds をCompute Shaderで直接更新
6.  GPU Compute [Cull Pass]: gpu_bounds のみ読み取り、Frustum + Hi-Z Culling実行

7.  GPU Compute [LOD Pass]: gpu_transforms のみ読み取り、カメラ距離ベースLOD選択
8.  GPU Compute [Compact Pass]: 可視オブジェクトのDrawIndexedIndirectコマンド書込

9.  GPU Render: vkCmdDrawIndexedIndirectCountでIndirect Draw発行

ステップ1〜4は全てCompute Shaderであり、CPU↔GPU間のデータ転送はフレーム定数（~128B）のみ。

## 7.3 GPU側 SoAバッファレイアウト

|                    |                                         |                 |                        |
|--------------------|-----------------------------------------|-----------------|------------------------|
| **SSBO名**         | **内容**                                | **サイズ/要素** | **読取パス**           |
| gpu_bounds         | AABB                                    | 24 bytes        | Cull Pass のみ         |
| gpu_transforms     | float4x4                                | 64 bytes        | Update/LOD/Render Pass |
| gpu_velocities     | float3 速度                             | 12 bytes        | Update Pass のみ       |
| gpu_mesh_info      | MeshHandle+IndexOffset+Count+BaseVertex | 16 bytes        | Compact Pass のみ      |
| gpu_material_ids   | MaterialHandle (uint32)                 | 4 bytes         | Compact + Render Pass  |
| gpu_lod_info       | LODレベル数 + 距離閾値                  | 32 bytes        | LOD Pass のみ          |
| gpu_visibility     | 可視フラグ + 選択LOD                    | 4 bytes         | Cull/LOD → Compact     |
| IndirectDrawBuffer | VkDrawIndexedIndirectCommand            | 20 bytes        | Compact → GPU描画      |
| DrawCountBuffer    | atomic uint32                           | 4 bytes         | Compact → GPU描画      |

## 7.4 GPU Driven適用判定

- メッシュ 50,000tri以下（極大メッシュはCPU個別処理が効率的）
- 同一Mesh+Material インスタンス 32個以上

- シェーダがGPU Driven互換（Indirect Draw対応Vertex Input Layout）

# 8. Pipeline Profiles

## 8.1 デフォルトプロファイル一覧

|                  |                                   |                                                                                     |
|------------------|-----------------------------------|-------------------------------------------------------------------------------------|
| **プロファイル** | **ターゲット**                    | **主な特徴**                                                                        |
| Pictor Lite      | モバイル / ローエンドPC           | Forward, GPU Driven無効, シャドウ1カスケード, Frame Alloc 4MB                       |
| Pictor Standard  | コンシューマ / ミドルレンジPC     | Forward+, GPU Driven有効, シャドウ3カスケード, SSAO, Bloom, Frame Alloc 16MB        |
| Pictor Ultra     | ハイエンドPC / 次世代コンシューマ | Deferred+Forward Hybrid, Full GPU Driven, Compute Update, RT(opt), Frame Alloc 64MB |

## 8.2 プロファイル定義構造

|                      |                    |                                               |
|----------------------|--------------------|-----------------------------------------------|
| **プロパティ**       | **型**             | **説明**                                      |
| profileName          | string             | プロファイル識別名                            |
| renderingPath        | enum               | FORWARD / FORWARD_PLUS / DEFERRED / HYBRID    |
| renderPasses[]     | RenderPassDef[]  | パスの順序付きリスト                          |
| shadowConfig         | ShadowConfig       | カスケード数、解像度、フィルタリング          |
| postProcessStack[] | PostProcessDef[] | ポストプロセス定義リスト                      |
| gpuDrivenEnabled     | bool               | GPU Driven有効/無効                           |
| computeUpdateEnabled | bool               | GPU Compute Update有効/無効                   |
| gpuDrivenConfig      | GPUDrivenConfig    | 閾値・バッファサイズ・SoA分割設定             |
| memoryConfig         | MemoryConfig       | Frame Allocatorサイズ、Pool容量、ラージページ |
| updateConfig         | UpdateConfig       | chunkSize, NTストア有効化, workerThread数     |
| profilerConfig       | ProfilerConfig     | プロファイラ有効/無効、オーバーレイ設定       |
| maxLights            | uint32             | 最大ライト数                                  |
| msaaSamples          | uint8              | 0 / 2 / 4 / 8                                 |

## 8.3 RenderPass定義

|                     |                                                                              |
|---------------------|------------------------------------------------------------------------------|
| **フィールド**      | **説明**                                                                     |
| passName            | パス識別名（例: DepthPrePass, ComputeUpdate）                                |
| passType            | DEPTH_ONLY / OPAQUE / TRANSPARENT / SHADOW / POST_PROCESS / COMPUTE / CUSTOM |
| shaderOverride      | 強制使用シェーダ（nullならオブジェクトのシェーダ）                           |
| renderTargets[]   | 出力先レンダーターゲット名                                                   |
| inputTextures[]   | 入力テクスチャ依存                                                           |
| sortMode            | FRONT_TO_BACK / BACK_TO_FRONT / NONE                                         |
| filterMask          | 処理対象フラグマスク                                                         |
| gpuDrivenPass       | GPU Driven Drawを使用するか                                                  |
| requiredStreams[] | 読み取るSoAストリーム名リスト（プリフェッチヒント）                          |

## 8.4 プロファイル切替

10. 新プロファイルのロードとバリデーション
11. Frame Allocatorサイズ変更

12. UpdateScheduler設定変更（Compute Update有効/無効切替）
13. 全RenderBatch無効化

14. RenderPassScheduler再構成
15. GPUリソース再確保

16. 全バッチ再構築
17. 次フレームから新パイプラインで描画開始

# 9. Standard Render Pass Reference

## 9.1 Pictor Standard パス構成例

|          |                 |                                     |                        |
|----------|-----------------|-------------------------------------|------------------------|
| **順序** | **パス名**      | **処理内容**                        | **読取ストリーム**     |
| 1        | ShadowPass      | カスケードシャドウマップ生成        | bounds, transforms     |
| 2        | DepthPrePass    | 不透明深度のみ描画                  | bounds, transforms     |
| 3        | Hi-Z Build      | Hi-Zミップマップ生成（Compute）     | （前パス出力）         |
| 4        | GPU Cull Pass   | Compute Culling + Indirect Draw生成 | gpu_bounds のみ        |
| 5        | OpaquePass      | Forward+ 不透明描画                 | transforms, shaderKeys |
| 6        | SkyboxPass      | スカイボックス描画                  | （固定リソース）       |
| 7        | TransparentPass | 半透明 Back-to-Front描画            | transforms, sortKeys   |
| 8        | PostProcess     | SSAO → Bloom → Tonemap → TAA        | （前パス出力）         |

## 9.2 Pictor Ultra パス構成例（Compute Update統合）

|          |                   |                                               |                                                   |
|----------|-------------------|-----------------------------------------------|---------------------------------------------------|
| **順序** | **パス名**        | **処理内容**                                  | **読取/書込**                                     |
| 0        | Compute Update    | gpu_transforms / gpu_bounds 更新              | gpu_velocities → gpu_transforms / gpu_bounds 書込 |
| 1        | ShadowPass        | カスケードシャドウマップ生成                  | gpu_transforms 読取                               |
| 2        | DepthPrePass      | 不透明深度のみ描画                            | gpu_transforms 読取                               |
| 3        | Hi-Z Build        | Hi-Zミップマップ生成                          | （前パス出力）                                    |
| 4        | GPU Cull Pass     | Frustum + Hi-Z Culling                        | gpu_bounds 読取                                   |
| 5        | GPU LOD + Compact | LOD選択 + Indirect Draw生成                   | gpu_transforms, gpu_mesh_info                     |
| 6        | GBuffer Pass      | Deferred GBuffer書込                          | gpu_transforms, gpu_material_ids                  |
| 7        | Lighting Pass     | Deferred Lighting（Compute）                  | GBuffer入力                                       |
| 8        | TransparentPass   | 半透明描画                                    | gpu_transforms, sortKeys                          |
| 9        | PostProcess       | SSAO → Bloom → Tonemap → TAA → Volumetric Fog | （前パス出力）                                    |

# 10. Culling System

## 10.1 カリング階層

|          |                |                            |                      |
|----------|----------------|----------------------------|----------------------|
| **段階** | **実行場所**   | **手法**                   | **読取データ**       |
| Level 1  | CPU            | Frustum Culling (Flat BVH) | bounds[] のみ      |
| Level 2  | CPU (optional) | Software Occlusion Culling | bounds[] のみ      |
| Level 3  | GPU (Compute)  | Hi-Z Occlusion Culling     | gpu_bounds SSBO のみ |

## 10.2 Flat BVH: キャッシュ最適化ノードレイアウト

BVHノードは32Bフラット構造体。ポインタなし、配列インデックスで子参照。2ノードが1キャッシュラインに収まる。

|                    |        |            |                                             |
|--------------------|--------|------------|---------------------------------------------|
| **フィールド**     | **型** | **サイズ** | **説明**                                    |
| aabbMin            | float3 | 12B        | ノードAABB最小点                            |
| childOrObjectIndex | uint32 | 4B         | 内部: 左子Index / リーフ: オブジェクトIndex |
| aabbMax            | float3 | 12B        | ノードAABB最大点                            |
| flags              | uint32 | 4B         | bit0: isLeaf, bit1-7: objectCount           |

### Van Emde Boas Layout

構築済みBVHノードをvEBレイアウトで再配置。空間的に近いノードがメモリ上でも近くなり、全深度で均一に良好なキャッシュ性能を示す。静的BVHはシーンロード時にSAH構築+vEB再配置。動的BVHはRefit+閾値超過時に非同期再構築。

## 10.3 Hi-Z Occlusion Culling

GPU Driven Poolに対してHi-Zによる高速オクルージョンカリングを実行。Two-Phase方式で前フレーム可視オブジェクトを先に描画しHi-Zを更新後、残りを再カリング。

# 11. Resource Management

## 11.1 CPU側メモリ管理

|                           |                  |                                                |
|---------------------------|------------------|------------------------------------------------|
| **カテゴリ**              | **アロケータ**   | **説明**                                       |
| Object Pool SoAストリーム | Pool Allocator   | 登録時確保。チャンク単位拡張                   |
| BVHノード配列             | Pool Allocator   | Static: ロード時 / Dynamic: 予約+Refit         |
| フレーム一時データ        | Frame Allocator  | カリング結果、ソート配列、コマンド一時バッファ |
| ハッシュマップ            | ヒープ（例外的） | objectId→Pool Index逆引き                      |

## 11.2 GPU側メモリ管理

|                      |                            |                                     |
|----------------------|----------------------------|-------------------------------------|
| **バッファ種別**     | **管理方式**               | **説明**                            |
| Mesh Pool (VB/IB)    | プール型サブアロケーション | メッシュデータ。Upload後Staging解放 |
| SoA SSBOs            | 固定確保+チャンク拡張      | GPU側SoAストリーム群                |
| Instance Buffer      | GPU Ring Buffer            | Dynamic Poolトランスフォーム        |
| Indirect Draw Buffer | 固定確保                   | GPU Driven描画コマンド              |
| Render Target        | Profile依存固定確保        | プロファイル切替時再確保            |

## 11.3 フレームリソースフロー

18. BeginFrame: フェンス待機 → Frame Allocator ポインタリセット
19. Update: UpdateSchedulerが更新戦略に従い transforms/bounds 更新

20. Culling: bounds[] のみ走査 → visibilityFlags 書込
21. Sort: key-index pair Radix Sort（Frame Allocatorから一時バッファ確保）

22. Batch: ソート済みindex経由でストリーム間接参照 → DrawCommand生成
23. Encode: CommandBuffer記録 → GPU提出

24. EndFrame: プレゼント

# 12. Public API Design

**初期化・終了**

PictorRenderer::Initialize(config) でバックエンド初期化、MemorySubsystem確保、Profiler初期化、デフォルトプロファイルロードを行う。PictorRenderer::Shutdown() で全リソース解放とGPU同期を行う。

**オブジェクト操作**

SceneRegistry::Register(desc) でObject Poolに配置しobjectId返却。SceneRegistry::Unregister(objectId) でSwap-and-Pop削除。SceneRegistry::UpdateTransform(objectId, matrix) でDynamic Poolのtransforms[]更新。

**Compute Update登録**

SceneRegistry::SetComputeUpdateShader(poolId, shaderHandle) でGPU Driven PoolにCompute Updateシェーダを関連付ける。以降、そのPoolのオブジェクトはCPU更新をスキップしGPU Compute Updateで処理される。

**プロファイル操作**

PipelineProfileManager::SetProfile(profileName) / RegisterCustomProfile(def) でプロファイル切替・追加。

**プロファイラ操作**

Profiler::SetEnabled(bool) / Profiler::SetOverlayMode(mode) でプロファイラの有効化とオーバーレイ表示モードを切替。Profiler::GetFrameStats() で直近フレームの統計データを取得。

**フレーム描画**

PictorRenderer::BeginFrame() → Render(camera, scene) → EndFrame() の3コール。BeginFrameでフェンス待機+Frame Allocatorリセット+Profilerフレーム開始。

## 12.2 拡張ポイント

- ICustomRenderPass: カスタムパス。requiredStreamsでプリフェッチ宣言可能
- ICullingProvider: カスタムカリングアルゴリズム差替え

- IBatchPolicy: バッチング戦略カスタマイズ
- IResourceAllocator: GPUメモリアロケーション戦略差替え

- IStreamLayout: カスタムSoAストリーム追加
- IUpdateCallback: CPU並列更新時のユーザーロジック注入

- IJobDispatcher: 外部ジョブシステムへの差替え

# 13. Built-in Profiler

> **> **> **[Design Note]** **** *本章はv2.1で新設。開発効率とパフォーマンスチューニングのため、Pictorにプロファイラを組込みコンポーネントとして統合する。*

## 13.1 設計目標

- フレームタイムとFPSのリアルタイム表示
- RenderPass毎のGPU時間・CPU時間の階層的計測

- メモリサブシステム（Frame Allocator、Pool、GPU SSBO）の使用量追跡
- 描画統計（DrawCall数、三角形数、バッチ数、カリング除外数）

- 計測オーバーヘッドを最小化し、リリースビルドでも有効化可能な低負荷設計

## 13.2 FPS / フレームタイム計測

|                      |                                          |                                          |
|----------------------|------------------------------------------|------------------------------------------|
| **指標**             | **計測方法**                             | **表示形式**                             |
| FPS                  | 直近N フレーム（デフォルト60）の移動平均 | 整数表示（例: 60 FPS）                   |
| Frame Time           | BeginFrame〜EndFrame間のCPUタイマー      | ms表示 + 折れ線グラフ（直近300フレーム） |
| GPU Frame Time       | フレーム全体のGPU Timestamp差分          | ms表示 + 折れ線グラフ                    |
| Frame Time Histogram | 直近1000フレームの分布                   | 0-33ms範囲のヒストグラム。スパイク検出用 |

## 13.3 階層プロファイラ（Pass別計測）

各RenderPassおよびデータ更新フェーズにGPU Timestamp QueryとCPU High-Resolution Timerを挿入し、階層的なタイムライン表示を行う。

### 計測ポイント

|                |                                                                            |             |
|----------------|----------------------------------------------------------------------------|-------------|
| **フェーズ**   | **計測区間**                                                               | **GPU/CPU** |
| Data Update    | UpdateScheduler全体 / Level1並列 / Level2 NTストア / Level3 Compute Update | CPU + GPU   |
| Culling        | BVH Frustum Cull / Hi-Z Build / GPU Cull Pass                              | CPU + GPU   |
| Sort           | Radix Sort 全体                                                            | CPU         |
| Batch Build    | BatchBuilder 全体 / Static再構築 / Dynamic更新                             | CPU         |
| Shadow         | ShadowPass (各カスケード個別)                                              | GPU         |
| DepthPrePass   | DepthPrePass                                                               | GPU         |
| Opaque         | OpaquePass                                                                 | GPU         |
| Transparent    | TransparentPass                                                            | GPU         |
| PostProcess    | SSAO / Bloom / Tonemap / TAA 個別                                          | GPU         |
| Command Encode | CommandBuffer記録全体                                                      | CPU         |

### GPU Timestamp Query実装

VulkanのvkCmdWriteTimestampを各パス開始・終了に挿入する。QueryPoolはフレーム毎に確保し（Flight数分）、前フレームの結果をvkGetQueryPoolResultsで非同期回収する。1クエリのオーバーヘッドは通常1-2µs以下。

計測ポイント数の上限はデフォルト64（32パス x 開始/終了）。プロファイル設定で拡張可能。

### CPU Timer実装

std::chrono::high_resolution_clock（またはプラットフォーム固有のQPF/rdtsc）を使用。各フェーズの開始・終了で計測し、結果をリングバッファに蓄積する。

## 13.4 メモリ統計

|                               |                  |                             |
|-------------------------------|------------------|-----------------------------|
| **統計項目**                  | **データソース** | **表示**                    |
| Frame Allocator 使用量/ピーク | MemorySubsystem  | バー表示（現在/最大）       |
| Object Pool 占有率            | SceneRegistry    | Pool毎の使用スロット数/容量 |
| GPU SSBO 使用量               | GPUBufferManager | SSBO毎のバイト数            |
| GPU Mesh Pool 使用量          | GPUBufferManager | 使用/空き/断片化率          |
| GPU Render Target             | GPUBufferManager | RT毎のサイズ                |

## 13.5 描画統計

|                         |                                                  |
|-------------------------|--------------------------------------------------|
| **統計項目**            | **計測方法**                                     |
| DrawCall数              | CommandEncoder発行カウント                       |
| 三角形数                | バッチ毎のindexCount合計 / 3                     |
| バッチ数                | BatchBuilder生成バッチ数                         |
| カリング除外数          | visibilityFlags[] の非可視カウント             |
| GPU Driven Object数     | GPU Driven Pool内オブジェクト数                  |
| Compute Update Object数 | Compute Update対象オブジェクト数                 |
| 可視オブジェクト数      | DrawCountBuffer値（GPU Driven）+ CPU可視カウント |

## 13.6 オーバーレイUI

プロファイラの結果はデバッグオーバーレイとして画面上に描画する。OverlayModeで表示内容を切替可能。

|            |                                                                  |
|------------|------------------------------------------------------------------|
| **モード** | **表示内容**                                                     |
| MINIMAL    | FPS + Frame Time（左上に小さく表示）                             |
| STANDARD   | FPS + Frame Time グラフ + Pass別GPU時間バー + DrawCall数         |
| DETAILED   | STANDARD + メモリ統計 + 描画統計 + GPU/CPUタイムライン           |
| TIMELINE   | フレーム全体のGPU/CPUタイムラインを横棒グラフで表示（GPUView風） |
| OFF        | オーバーレイ非表示（内部計測は継続）                             |

オーバーレイの描画自体はPostProcessパスの後に独自の描画パスとして実行し、レンダリング結果に影響を与えない。フォントレンダリングはSDF（Signed Distance Field）ベースのテキスト描画を使用し、任意解像度でシャープに表示する。

## 13.7 データエクスポート

- JSON Export: フレーム毎の全統計データをJSONファイルに出力。オフライン分析用
- Chrome Tracing: chrome://tracing 互換のJSON出力。GPU/CPUタイムラインを外部ツールで可視化

- CSV Export: 時系列データのCSV出力。スプレッドシートでのグラフ作成用

# 14. Benchmark: 1M Spheres

> **> **> **[Design Note]** **** *本章はv2.1で新設。Pictorの設計が100万オブジェクト規模で実際に機能することを検証するための標準ベンチマークサンプルを定義する。*

## 14.1 目的

1,000,000個の独立した球体を毎フレーム異なる位置に移動させ描画する。Pictorの全サブシステム（SoAデータ管理、Compute Update、GPU Driven Rendering、バッチング、カリング、プロファイラ）を統合的にストレステストし、性能特性を可視化する。

## 14.2 シーン仕様

|                |                                                                |
|----------------|----------------------------------------------------------------|
| **項目**       | **仕様**                                                       |
| オブジェクト数 | 1,000,000 球体                                                 |
| メッシュ       | 共通の低ポリ球体（ICO Sphere, 42頂点, 80三角形）               |
| マテリアル     | 単一シェーダ、オブジェクト毎にカラーパラメータをSSBO経由で渡す |
| 初期配置       | 100x100x100 の3Dグリッド。グリッド間隔 2.0 unit                |
| 空間範囲       | 200 x 200 x 200 unit の立方体空間                              |
| カメラ         | 空間中央を見下ろすオービットカメラ。ズームイン/アウト可能      |
| ライティング   | ディレクショナルライト1灯 + アンビエント                       |

## 14.3 移動ロジック（Compute Update Shader）

全オブジェクトの移動はGPU Compute Shaderで実行する。CPU→GPU転送はフレーム定数（deltaTime, totalTime）の~64Bのみ。

### 移動パターン

各オブジェクトは初期位置を中心に、固有の周波数と振幅で3D Lissajous曲線上を移動する。Compute Shaderの疑似コードは以下の通り。

*pos.x = initialPos.x + amplitude \* sin(totalTime \* freqX + phaseX)*

*pos.y = initialPos.y + amplitude \* sin(totalTime \* freqY + phaseY)*

*pos.z = initialPos.z + amplitude \* sin(totalTime \* freqZ + phaseZ)*

freqX/Y/Z と phaseX/Y/Z はオブジェクトインデックスから導出する（hash関数ベース）。これにより100万個の球体が全て異なる軌跡で独立に移動する。

### Compute Shader仕様

|                      |                                                                             |
|----------------------|-----------------------------------------------------------------------------|
| **パラメータ**       | **値**                                                                      |
| ワークグループサイズ | 256 threads                                                                 |
| dispatch             | ceil(1,000,000 / 256) = 3,907 groups                                        |
| 入力SSBO             | gpu_initial_positions (float3, 12B/obj), gpu_freq_phase (float3x2, 24B/obj) |
| 出力SSBO             | gpu_transforms (float4x4, 64B/obj), gpu_bounds (AABB, 24B/obj)              |
| Uniform              | totalTime (float), deltaTime (float)                                        |

1 threadあたり: 読取 36B + 書込 88B。理論メモリアクセス: 1M x 124B = 124MB/dispatch。RTX 4070（504GB/s）では ~0.25ms で完了する見込み。

## 14.4 描画パイプライン構成

Pictor Ultraプロファイルをベースとし、以下のパス構成で描画する。

|          |                |                                                             |
|----------|----------------|-------------------------------------------------------------|
| **順序** | **パス**       | **詳細**                                                    |
| 0        | Compute Update | 100万オブジェクトのtransform + bounds更新                   |
| 1        | Hi-Z Build     | 前フレーム深度からHi-Z生成                                  |
| 2        | GPU Cull Pass  | Frustum + Hi-Z Culling。gpu_boundsのみ走査                  |
| 3        | GPU Compact    | 可視オブジェクトのIndirect Draw生成                         |
| 4        | OpaquePass     | vkCmdDrawIndexedIndirectCount。単一メッシュのInstanced Draw |
| 5        | PostProcess    | Tonemapping + FXAA                                          |

単一メッシュ（42頂点 x 80三角形）x 100万インスタンスを単一のDrawCallで描画可能。カリングにより不可視オブジェクトを除外し、可視オブジェクト数に応じた三角形数のみをGPUが処理する。

## 14.5 期待性能目標

|                |            |                                                     |
|----------------|------------|-----------------------------------------------------|
| **指標**       | **目標値** | **計測条件**                                        |
| Compute Update | < 0.3ms   | RTX 4070, 1M objects, GPU Compute                   |
| Culling (GPU)  | < 0.2ms   | Frustum + Hi-Z, 1M objects                          |
| Compact + Draw | < 2.0ms   | 可視50万オブジェクト想定（80tri x 500K = 40M tri）  |
| PostProcess    | < 0.5ms   | Tonemap + FXAA, 1920x1080                           |
| Total Frame    | < 4.0ms   | 250fps target                                       |
| CPU Frame Time | < 0.5ms   | フレーム定数アップロード + Fence待機 + Present のみ |
| GPU Memory     | ~240MB     | transforms 64MB + bounds 24MB + mesh 3.2MB + misc   |
| CPU Memory     | < 10MB    | ID逆引きハッシュマップ + 設定データのみ             |

> **> **> **[Design Note]** **** *CPU Frame Time < 0.5ms は、100万オブジェクトの更新・カリング・描画が全てGPU側で完結するため。CPUは毎フレーム~64Bのフレーム定数アップロードとvkQueueSubmitのみ。*

## 14.6 プロファイラ表示例

ベンチマーク実行時、組込みプロファイラは以下の情報をオーバーレイ表示する。

- FPS: 250 \| Frame: 4.0ms (CPU: 0.5ms / GPU: 3.5ms)
- Compute Update: 0.28ms \| Cull: 0.18ms \| Compact: 0.12ms

- OpaquePass: 1.82ms (40M tri, 1 DrawCall) \| PostProcess: 0.45ms
- Visible: 523,412 / 1,000,000 (52.3%) \| Culled: 476,588

- GPU Memory: 237MB / 2048MB \| Frame Alloc: 0.1MB / 64MB

TIMELINEモードでは各Compute/Renderパスの時間を横棒グラフで表示し、ボトルネック箇所を視覚的に特定できる。

## 14.7 バリエーションテスト

1M Spheresベンチマークを基に、以下のバリエーションで性能特性の変化を計測する。

|                    |                                      |                                    |
|--------------------|--------------------------------------|------------------------------------|
| **バリエーション** | **変更点**                           | **計測目的**                       |
| Static 1M          | 全オブジェクトStatic（移動なし）     | Compute Update省略時のベースライン |
| Mixed 500K/500K    | 50万Static + 50万Dynamic             | Pool混在時のバッチ効率             |
| High-Poly 1M       | 球体を642頂点/1280tri に変更         | 三角形数増加のGPU負荷影響          |
| Multi-Material 1M  | 100種類のマテリアルをランダム割当    | マテリアル分散時のバッチ分割影響   |
| Zoomed-In          | カメラを近づけて可視率90%以上        | カリング除外率低下時の負荷         |
| CPU Update 1M      | Compute Update無効、CPU並列+NTストア | CPU更新 vs GPU更新の性能比較       |

# 15. Performance Targets

## 15.1 目標指標

|                      |                 |                     |                                           |
|----------------------|-----------------|---------------------|-------------------------------------------|
| **指標**             | **Pictor Lite** | **Pictor Standard** | **Pictor Ultra**                          |
| DrawCall上限         | 200 / frame     | 500 / frame         | 1000+ (GPU Driven)                        |
| オブジェクト数上限   | 5,000           | 50,000              | 1,000,000+                                |
| バッチ構築時間       | < 0.3ms        | < 0.5ms            | < 1.0ms (GPU含む)                        |
| カリング時間         | < 0.2ms (CPU)  | < 0.3ms (CPU+GPU)  | < 0.5ms (GPU主体)                        |
| Data Update (1M obj) | —               | —                   | < 0.3ms (Compute Update)                 |
| Radix Sort (50K obj) | —               | < 0.15ms           | < 0.15ms                                 |
| Frame Alloc確保      | < 1µs/alloc    | < 1µs/alloc        | < 1µs/alloc                              |
| フレームバジェット   | 16.6ms (60fps)  | 16.6ms (60fps)      | 8.3ms (120fps) / 4.0ms (250fps benchmark) |
| ヒープalloc/frame    | 0               | 0                   | 0                                         |
| メモリ上限 (GPU)     | 256MB           | 512MB               | 2GB                                       |

## 15.2 キャッシュライン効率比較

|                             |                        |                     |                        |
|-----------------------------|------------------------|---------------------|------------------------|
| **方式**                    | **読取データ**         | **サイズ (1M obj)** | **キャッシュライン数** |
| v1.0 AoS (RenderObject[]) | 構造体全体 (~160B/obj) | 160 MB              | 2,500,000              |
| v2.0 SoA (bounds[] のみ)  | AABB のみ (24B/obj)    | 24 MB               | 375,000                |

100万オブジェクトのFrustum Cullingにおいて、SoAではキャッシュライン数が6.67倍少ない。bounds配列24MBはL3キャッシュ（典型16-32MB）に大部分が収まる。

# 16. Implementation Roadmap

## 16.1 フェーズ計画

|                            |          |                                                                                  |
|----------------------------|----------|----------------------------------------------------------------------------------|
| **フェーズ**               | **期間** | **内容**                                                                         |
| Phase 0: Memory Foundation | 3週間    | MemorySubsystem、SoAストリーム基盤、Object Pool構造                              |
| Phase 1: Scene Registry    | 3週間    | Object Pool登録・削除、ID逆引き、Hot/Cold分離配置                                |
| Phase 2: Data Update       | 3週間    | UpdateScheduler、CPU並列更新+NTストア、Compute Update Shader基盤                 |
| Phase 3: Batching + Sort   | 4週間    | BatchBuilder（インデックス間接参照）、Radix Sort、Multi Draw Indirect            |
| Phase 4: Culling           | 4週間    | Flat BVH（SAH+vEB）、Frustum Culling、Hi-Z、GPU Occlusion Culling                |
| Phase 5: GPU Driven        | 4週間    | GPU側SoA SSBO、Compute Cull/LOD/Compact、Indirect Draw                           |
| Phase 6: Profiles          | 3週間    | PipelineProfileManager、Lite/Standard/Ultra、ランタイム切替                      |
| Phase 7: Post Process      | 3週間    | SSAO、Bloom、Tonemapping、TAA、Volumetric Fog                                    |
| Phase 8: Profiler          | 3週間    | FPS/FrameTime計測、Pass別GPU/CPU Timer、メモリ統計、オーバーレイUI、エクスポート |
| Phase 9: Benchmark         | 2週間    | 1M Spheresベンチマーク実装、バリエーションテスト、性能チューニング               |
| Phase 10: Polish           | 3週間    | ドキュメント、残課題修正、CI統合、リリース準備                                   |

## 16.2 将来拡張

- Ray Tracing統合（Vulkan RT Extension）
- Mesh Shader対応

- Nanite風仮想ジオメトリ（SoA設計との親和性が高い）
- DirectX 12バックエンド

- Bindless Resource対応
- NUMA対応: マルチソケットでのPool配置最適化

- マルチビューレンダリング: VR/AR向けステレオ最適化
- 10M+ Objectsベンチマーク: Multi-GPU / Mesh Shader統合検証
