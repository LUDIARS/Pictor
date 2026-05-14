# Animation Montage

UE Animation Montage の最小サブセットを Pictor に追加したファサード。
`AnimationSystem` の `play(handle, clip, weight, speed, blend)` を低レイヤと
して使い、 上に以下を被せる:

- **Notify トラック** — 時刻キーで gameplay 側にイベント発火
- **Section** — 1 Montage 内の論理区間 + `jump_to_section` / `set_next_section`
- **Blend in / out** — 開始/終了で weight を補間
- **on_finish コールバック** — ONCE 終了 or Section チェーン終了で呼ばれる
- **Curve** — 任意名 float カーブのサンプル取得
- **ボーン名リターゲット** — clip 作成時のボーン名 → 再生先 skeleton ボーン
  index への remap テーブル (rig が近いキャラ間でモーション流用)

## API

```cpp
#include "pictor/animation/montage.h"

pictor::AnimationSystem sys; sys.initialize({});
pictor::MontagePlayer  montages(sys);

// 1) 静的登録
pictor::MontageDescriptor d;
d.clip = clip_handle;
d.notifies = {
    { 0.20f, "hit_window_open",  "" },
    { 0.45f, "spawn_projectile", "Projectile/Knife" },
    { 0.55f, "hit_window_close", "" },
};
d.sections = {
    { "step1", 0.0f, 0.6f, "step2" },
    { "step2", 0.6f, 1.2f, "step3" },
    { "step3", 1.2f, 1.8f, "" },     // 空 next で終了
};
pictor::MontageHandle m_attack = montages.register_montage(d);

// 2) 再生
pictor::MontagePlayParams p;
p.weight        = 1.0f;
p.blend_in_time = 0.10f;
p.blend_out_time= 0.15f;
p.on_notify     = [](const auto& n){ kuzu::handle_notify(n.name, n.payload); };
p.on_finish     = []{ kuzu::on_attack_finished(); };
montages.play(inst_handle, m_attack, skel_handle, p);

// 3) 毎フレーム
sys.update(dt);
montages.update(dt);

// 4) Combo 連結 (現 section の次を上書き)
if (player_pressed_again) montages.set_next_section(inst_handle, "step2");
```

## ボーン名リターゲットの仕組み

1. `AnimationClipDescriptor::bone_names` は channels[i] が参照する source bone 名。
   FBX importer がこれを埋める (今は legacy clip 互換のため空でも許容)。
2. `MontagePlayer::play(inst, m, skel, params)` 呼び出し時、 `(clip, skel)` の
   組に対して remap テーブル `vector<int> remap` を `Skeleton::find_bone(name)`
   ベースで構築し、 `remap_cache_` にキャッシュ。
3. **適用** : `AnimationSystem::evaluate_instance` が remap を参照して channel の
   `target_index` を再解決する hook を入れる。
   - 現状の Pictor 実装ではこの hook は未配線 (follow-up)。 `MontagePlayer::get_bone_remap`
     から取得した remap を `AnimationSystem` に渡せるよう、 次の改修で
     `AnimationInstance` に `const std::vector<int>* channel_remap` フィールドを
     足し、 `evaluate_instance` がそれを参照して `channel.target_index` を差し替える。

## tick 順

```
gameplay tick (input, AI, ...)
  ↓
montages.update(dt)        // weight 補間、 Notify 発火、 Section 遷移、 on_finish
  ↓
sys.update(dt)             // 実際のクリップ評価、 IK、 スキニング行列計算
  ↓
WorldRenderer.sync         // ObjectId transform 反映
```

`montages.update` を先に呼ぶ理由: weight / 終了処理を反映してから sys が
実評価するため。 順序逆でも 1 フレーム遅延が出るだけで動作は破綻しない。

## Notify 命名規則 (KuzuSurvivors 側合意)

| name | 用途 |
|---|---|
| `hit_window_open`  | AttackHitbox.open() |
| `hit_window_close` | AttackHitbox.close() |
| `spawn_projectile` | payload で kind を識別 ("Projectile/Knife" 等) |
| `spawn_vfx:<key>`  | VfxCatalog.play_vfx(payload, ...) |
| `play_sfx:<key>`   | AudioService.play_sfx(payload) |
| `lock_movement`    | PlayerController で移動を禁止 |
| `unlock_movement`  | 解除 |

## 制限事項

- 1 instance / 1 active montage (重ね合わせは未対応; 必要なら layer index で拡張)
- bone-mask / slot 分離は未実装 (上半身 Montage + 下半身 locomotion の分離)
- curve は単軸 float のみ、 ベクトル / カラーは未対応

これらは AdventureCube / KuzuSurvivors の現用途では不要、 必要時に拡張する。
