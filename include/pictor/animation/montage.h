#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/skeleton.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pictor {

class AnimationSystem;

// ============================================================
// Montage — single-shot motion data with notify / section / blend (UE inspired)
// ============================================================

/// 時刻キーで gameplay 側に発火するイベント。
struct MontageNotify {
    float       time = 0.0f;    // 秒
    std::string name;           // 例 "hit_window_open", "spawn_projectile"
    std::string payload;        // 自由文字列 (例 "Effect/Lightning")
};

/// Montage 内の論理区間。 1 Montage に複数 section を持たせて
/// `jump_to_section / set_next_section` で連結する (ComboAttack 連結用)。
struct MontageSection {
    std::string name;
    float       start  = 0.0f;  // 秒
    float       end    = 0.0f;  // 秒 (>= start)
    std::string next;           // 空文字列なら end で montage 終了
};

/// 任意の時間 → float カーブ (移動速度倍率、 ダメージ倍率等)
struct MontageCurveTrack {
    std::string                name;
    std::vector<float>         times;   // 単調増加
    std::vector<float>         values;  // times と同サイズ
};

/// 静的に登録される montage 定義。
struct MontageDescriptor {
    AnimationClipHandle             clip = INVALID_ANIMATION_CLIP;
    std::vector<MontageNotify>      notifies;
    std::vector<MontageSection>     sections;
    std::vector<MontageCurveTrack>  curves;

    /// 既定の wrap (ONCE = 1 回再生で終了)
    WrapMode wrap = WrapMode::ONCE;
};

struct MontageHandle { uint32_t id = 0; };
constexpr MontageHandle INVALID_MONTAGE{0};

struct MontagePlayParams {
    float weight        = 1.0f;
    float speed         = 1.0f;
    float blend_in_time  = 0.0f;
    float blend_out_time = 0.0f;
    AnimBlendMode blend  = AnimBlendMode::OVERRIDE;

    /// 開始 section (空文字列なら頭から)
    std::string start_section;

    /// 終了時 (ONCE で duration 到達 or section チェーン終了)
    std::function<void()> on_finish;

    /// Notify 発火コールバック (時刻 t で `montage.notifies` 上を跨いだ瞬間に呼ばれる)
    std::function<void(const MontageNotify&)> on_notify;
};

/// AnimationSystem に乗せる単発再生レイヤ。
///
/// 設計思想: UE Animation Montage の最小サブセット (notify + section + blend) +
/// クリップ作成時のボーン名と再生先スケルトンのボーン名で remap テーブルを
/// 構築する。 これにより「リグが近い」キャラ間でモーションを共通利用できる。
///
/// 実装メモ: 現状 Pictor の `AnimationSystem::evaluate_instance` はチャンネル
/// 評価結果を `channel.target_index` で直接ボーンに書き込む。 remap テーブルは
/// MontagePlayer に保持されるが、 実適用 (target_index の差し替え) は
/// `evaluate_instance` 側に hook を入れる follow-up が必要。 現実装では
/// テーブル準備 + ログ + データ伝達まで完結し、 evaluate_instance 側で対応
/// される予定の API を公開している。
class MontagePlayer {
public:
    explicit MontagePlayer(AnimationSystem& sys);
    ~MontagePlayer();

    /// 静的 montage を登録する。 戻り値の handle を `play` で渡す。
    MontageHandle register_montage(const MontageDescriptor& desc);

    /// 指定 instance で montage を開始する。 既存 layer は blend_out して終了する。
    void play(AnimationStateHandle inst, MontageHandle m,
              SkeletonHandle skel, const MontagePlayParams& params);

    /// 現フレーム経過分を反映する: 時刻進行 + notify 発火 + blend in/out +
    /// section 遷移 + ONCE 終了時の on_finish。
    /// AnimationSystem::update より後に呼ぶ。
    void update(float delta_time);

    /// 指定 section に即時ジャンプ
    void jump_to_section(AnimationStateHandle inst, const std::string& name);
    /// 現 section が終わった時点で遷移する section 名を上書き
    void set_next_section(AnimationStateHandle inst, const std::string& name);

    /// 強制停止 (blend_out を 0 にして即終了)
    void stop(AnimationStateHandle inst);

    /// 指定 instance で montage 再生中か?
    bool is_playing(AnimationStateHandle inst) const;

    /// 任意 curve の現時点 value を取得 (再生中のみ)
    bool sample_curve(AnimationStateHandle inst, const std::string& name, float& out) const;

    /// (clip × skeleton) のボーン名 remap テーブル。
    /// vector[channel_index] = target skeleton bone index (-1 で未一致)
    /// evaluate_instance のフック実装が ready になり次第参照される。
    const std::vector<int>* get_bone_remap(AnimationClipHandle clip, SkeletonHandle skel) const;

private:
    void rebuild_remap_(AnimationClipHandle clip, SkeletonHandle skel);

    struct Active {
        MontageHandle      m;
        SkeletonHandle     skel;
        uint32_t           layer = 0;          // AnimationInstance.layers の index
        float              local_time = 0.0f;   // section 開始からの相対時刻
        int                section_idx = -1;
        std::string        next_section_override;
        float              blend_in_remaining  = 0.0f;
        float              blend_in_initial    = 0.0f;
        float              blend_out_remaining = 0.0f;
        float              blend_out_initial   = 0.0f;
        bool               finishing = false;
        MontagePlayParams  params;
        // 跨ぎ判定のため前フレーム local_time を保持
        float              prev_local_time = 0.0f;
    };

    AnimationSystem&                                       sys_;
    std::vector<MontageDescriptor>                         montages_;
    std::unordered_map<uint32_t /*inst*/, Active>          actives_;

    // (clip_id, skel_id) → remap[channel] = bone_index or -1
    struct PairKey {
        uint32_t clip; uint32_t skel;
        bool operator==(const PairKey& o) const { return clip == o.clip && skel == o.skel; }
    };
    struct PairKeyHash { std::size_t operator()(const PairKey& k) const {
        return std::hash<uint64_t>()((uint64_t(k.clip) << 32) | k.skel); } };
    std::unordered_map<PairKey, std::vector<int>, PairKeyHash> remap_cache_;
};

} // namespace pictor
