# Visus task 3 — シェーダーパッケージ (シェーダ + 動的パラメータ)

- 起票: 2026-08-20 (neco 方針)
- 正本 spec: `spec/feature/visus-v2-design.md` §2.5
- 前提 stack: #723 (spec) → #725 (schema) → #729 (runtime) → #736 (hot reload) → 本タスク

## neco 方針

> メタデータは元来ある「マテリアル」の上位層にあたる。シェーダーとゲーム中動的に変える
> パラメータのペアをシェーダーパッケージとして Visus にアサインする。複数アサインできる。

複数アサインの意味は **重ね掛け (多パス) + パーツ別の両方** (2026-08-20 確認)。

## 完了範囲 (Pictor)

- [x] `VisusShaderPackage` (shader + params + metadata) と `VisusPackageRef` (name 参照 / enabled / 上書き)
- [x] 実効列のマージ規則 `visus_effective_packages` (§2.5.3: Visus 直下 → part append、同名は位置維持で上書き、`enabled:false` で除外)
- [x] `<name>.shaderpkg.json` のシリアライザ (version 1) と `VisusPackageCatalog` (ディレクトリ読込 / directory・direct load の symlink 拒否 / パッケージ起点のパス解決)
- [x] Visus v2 JSON の `shader_packages` (root / parts[]) 読み書き。アサインが無ければ従来と同一出力
- [x] `VisusRuntime::resolve(..., const VisusPackageCatalog*)` で `VisusResolvedPackage { shader, shader_key, params }` を part / visus に解決
- [x] `instantiate_visus` が base + パッケージ数の `ObjectDescriptor` を登録し、`VisusInstance::bindings` と `set_param` / `params_revision` で動的パラメータを扱う
- [x] 単体テスト 4 本 (新規 2 + runtime / instantiator 追記)

## 非ゴール

- uniform への実バインド / パイプライン state (blend・深度) の解釈 — ホスト (KS) 側 §4
- `params` のホットリロード — 値変更はプログラム経路 (#736 と同方針)
- パッケージのインライン定義 — 常に名前を持つ再利用資源とする

## 次 (KuzuSurvivors)

- `data/shaderpkg/` を作り `VisusPackageCatalog` を `SkinnedLayer` に配線
- `SkinnedRenderer` が `bindings` の ObjectId を重ね掛けパスとして描き、`params_revision` を見て uniform 更新
- kzs-web `/visus/` エディタにパッケージ表 (アサイン順 / enabled / params 上書き) を追加
