#pragma once

/// Post-process config bridge — 系統A → 系統B の橋渡し.
///
/// `PipelineProfileDef::post_process_stack` (系統A: 宣言データ) を実描画が
/// 消費する `PostProcessConfig` (系統B: `PostProcessPipeline::set_config`)
/// へ畳み込む。 方針2 phase 1 の中核
/// (`spec/rendering-extensibility-design.md` §3)。
///
/// 動作:
///   - スタックを走査し、各 `PostProcessDef` の `kind` で対応する
///     `PostProcessConfig` のエフェクトスロットを上書きする。
///   - `enabled` はそのスロットの `.enabled` に転写される。
///   - スタックに現れないエフェクトは `enabled=false` に倒される
///     (= プロファイルが Bloom を載せなければ Bloom は切れる)。
///   - `PostProcessKind::UNKNOWN` (SSAO / TAA / FXAA 等、 host-driven な
///     `PostProcessPipeline` に実装の無いエフェクト) は黙って無視される。
///     これらは系統B phase 2 の領分。
///
/// `PostProcessPipeline` の固定 4-pass 構造はそのまま。 本ブリッジは
/// 「既存エフェクトのパラメータと on/off を設定駆動にする」 だけで、
/// 任意 pass の挿入は行わない (phase 2 / 設計書 §3 参照)。

#include "pictor/postprocess/postprocess_effect.h"

#include <vector>

namespace pictor {

struct PostProcessDef;
struct PipelineProfileDef;

/// Fold a post-process stack into a `PostProcessConfig`.
///
/// `base` seeds non-effect fields (notably `hdr` and `gaussian_blur`, which
/// have no `PostProcessDef` representation) so callers can keep their HDR /
/// blur setup. Every effect that *can* be expressed by a `PostProcessDef`
/// (bloom / tone-mapping / vignette / color-grading / depth-of-field) is
/// reset to disabled first, then re-enabled + parameterised by whatever the
/// stack contains. Pass a default-constructed `PostProcessConfig` for `base`
/// to drive the whole stack purely from the profile.
PostProcessConfig build_post_process_config(const std::vector<PostProcessDef>& stack,
                                            const PostProcessConfig& base = {});

/// Convenience overload — folds `profile.post_process_stack`.
PostProcessConfig build_post_process_config(const PipelineProfileDef& profile,
                                            const PostProcessConfig& base = {});

} // namespace pictor
