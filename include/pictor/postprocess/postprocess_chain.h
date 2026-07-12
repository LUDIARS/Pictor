#pragma once

/// PostProcessChain — 任意 post-process pass を挿入できる汎用チェーン記述.
///
/// 背景 (`spec/rendering-extensibility-design.md` §6.3, phase 2 項目1):
///   旧 `PostProcessPipeline` は固定 4-pass (extract → blur H → blur V →
///   grade)・3 render pass・3 ターゲット・2 descriptor レイアウトを丸ごと
///   ベタ書きしていた。 `build_post_process_config()` は
///   `PostProcessKind::UNKNOWN` (SSAO / TAA / FXAA 等) を黙って捨てており、
///   系統A → 系統B の断点になっていた。
///
///   本ヘッダは固定構造を解体した受け皿。 チェーンを `PostProcessPassDef`
///   の配列で記述し、 ターゲットを名前付きの `std::vector` で持つ。
///   組み込み 4 エフェクト (Bloom / blur / grade) は「組み込み pass
///   テンプレート」 として同じ汎用記述へ畳み込まれる
///   (`build_post_process_chain()`)。
///
/// スコープ (phase 2 項目1):
///   - 1 入力 / 複数入力の graphics pass。 各 pass は fullscreen triangle。
///   - ターゲットは全て HDR (RGBA16F) 中間 + 最終 swapchain 出力。
///   - history buffer (ライフタイム >1 フレーム、 TAA 用) は将来。
///   - subpass dependency は pass の挿入順から動的生成する。
///
/// `RenderPassDef` (`pipeline_profile.h`) が雛形 — pass 名 + 入出力ターゲット名。

#include "pictor/postprocess/postprocess_effect.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

struct PostProcessDef;

// ============================================================
// PostProcessPushField — push constant 1 フィールドの汎用記述
// ============================================================
/// push constant ブロックを「16-byte 境界に並んだ float / uint の列」 として
/// 汎用記述する。 組み込みエフェクトの `ExtractPC` / `BlurPC` / `GradePC`
/// は全て float / uint のフラット構造体なので、 これで十分に表現できる。
///
/// `record()` は pass ごとにこのレイアウトに従って push constant を組む。
enum class PushFieldType : uint8_t {
    FLOAT = 0,  ///< 4-byte float
    UINT  = 1,  ///< 4-byte uint32
};

/// push constant 1 スロット (4 byte)。 値は `PostProcessPassDef::push_data`
/// が確定値を持つ — レイアウトは型情報のみ (デバッグ / 検証用)。
struct PushFieldDesc {
    std::string   name;                       ///< フィールド名 (デバッグ用)
    PushFieldType type   = PushFieldType::FLOAT;
    uint32_t      offset = 0;                  ///< push range 内バイトオフセット
};

// ============================================================
// PostProcessPassDef — 1 個の汎用 post-process pass
// ============================================================
/// 1 pass = fullscreen triangle 1 枚の描画。
///
///   - `name`           : pass 識別子 (デバッグ / タイムライン用)。
///   - `vert_spv`       : 頂点シェーダ SPIR-V (通常 fullscreen_quad.vert.spv)。
///   - `frag_spv`       : フラグメントシェーダ SPIR-V。
///   - `inputs`         : サンプルする論理ターゲット名の列 (descriptor の
///                        binding 0,1,2… 順)。
///   - `output`         : 描画先の論理ターゲット名。 `kOutputTarget` のとき
///                        最終 swapchain 画像へ出力する。
///   - `push_layout`    : push constant フィールドのレイアウト記述。
///   - `push_data`      : push constant の確定バイト列 (`record()` がそのまま
///                        `vkCmdPushConstants`)。 サイズは `push_layout` の
///                        末尾 offset+4 を 16 の倍数へ切り上げた値。
///
/// `inputs` / `output` は `PostProcessChainBuilder` がターゲット index へ
/// 解決する。 ping-pong は論理名 ("pp_ping" / "pp_pong") の解決で表現する。
struct PostProcessPassDef {
    std::string name;
    std::string vert_spv;
    std::string frag_spv;

    std::vector<std::string>   inputs;
    std::string                output;

    std::vector<PushFieldDesc> push_layout;
    std::vector<uint8_t>       push_data;

    /// push constant の総バイト数 (16-byte アラインに切り上げ済み)。
    uint32_t push_size() const {
        return static_cast<uint32_t>(push_data.size());
    }
};

/// 最終 swapchain 画像を指す予約済みターゲット名。 `PostProcessPassDef::output`
/// がこの値のとき、 その pass は swapchain へ直接描く (チェーンの末尾)。
extern const char* const kPostProcessOutputTarget;   // "__output__"

/// HDR シーンカラー (ホストが 3D を描き込む入力) を指す予約済みターゲット名。
extern const char* const kPostProcessSceneTarget;    // "__scene__"

/// 色グレーディング LUT テクスチャを指す予約済みターゲット名。
/// `PostProcessPassDef::inputs` にこの名前を入れると、 その binding は
/// `PostProcessPipeline` が所有する LUT テクスチャに解決される。
extern const char* const kPostProcessLutTarget;      // "__lut__"

/// シーン深度 (D32_SFLOAT) を指す予約済みターゲット名。
/// `PostProcessPassDef::inputs` にこの名前を入れると、 その binding は
/// `PostProcessPipeline` の scene 深度ビューに解決される (NEAREST サンプラ)。
/// DoF / SSAO 等、 深度をパイプライン途中で要求する pass 用。
/// 入力専用 — `output` には使えない。
extern const char* const kPostProcessDepthTarget;    // "__depth__"

// ============================================================
// PostProcessChain — pass 列 + 中間ターゲット集合
// ============================================================
/// 汎用 post-process チェーン全体の宣言記述。
///
///   - `passes`            : 実行順の pass 列。
///   - `intermediate_names`: チェーンが要する HDR 中間ターゲットの論理名集合
///                           (scene / output を除く)。 `PostProcessPipeline`
///                           はこの数だけ `RenderTarget` を確保する。
///
/// `PostProcessPipeline` はこの記述から render pass / ターゲット / descriptor
/// を動的に構築する。 空の `passes` は「post-process 無し」 (scene を素通し)。
struct PostProcessChain {
    std::vector<PostProcessPassDef> passes;
    std::vector<std::string>        intermediate_names;

    bool empty() const { return passes.empty(); }
};

// ============================================================
// チェーン途中編集 API — anchor pass 名基準の挿入 / 削除
// ============================================================
/// パイプラインを「途中で」 変えるための編集操作。 `build_post_process_chain()`
/// が返した (あるいはホストが組んだ) チェーンに対して、 既存 pass 名を anchor
/// に指定して任意 pass を差し込む / 抜く。 編集後は
/// `rebuild_intermediate_targets()` が自動で走り、 新 pass が参照する中間
/// ターゲットが `intermediate_names` へ反映される。
///
/// 実行中のチェーンへ反映するには編集後に
/// `PostProcessPipeline::rebuild_chain()` を呼ぶ (構造変更はフレーム間のみ。
/// push constant だけの更新は `refresh_post_process_chain()` で足りる)。

/// pass の挿入位置 — anchor の直前か直後か。
enum class PassInsertWhere : uint8_t {
    BEFORE = 0,
    AFTER  = 1,
};

/// `name` を持つ pass の index を返す。 見つからなければ -1。
int find_post_process_pass(const PostProcessChain& chain, std::string_view name);

/// `anchor` の直前 / 直後へ `pass` を挿入する。 anchor が見つからない、
/// または同名 pass が既に存在する場合は false (チェーンは不変)。
bool insert_post_process_pass(PostProcessChain&  chain,
                              PostProcessPassDef pass,
                              std::string_view   anchor,
                              PassInsertWhere    where);

/// `name` を持つ pass を取り除く。 見つからなければ false。
bool remove_post_process_pass(PostProcessChain& chain, std::string_view name);

/// `pass_name` の入力のうち `old_target` を `new_target` へ張り替える。
/// 途中挿入した pass の出力を下流に読ませる配線替え用
/// (例: scene → dof 挿入後、 bloom の入力を dof 出力へ)。
/// pass か old_target が見つからなければ false。
bool rebind_post_process_input(PostProcessChain& chain,
                               std::string_view  pass_name,
                               std::string_view  old_target,
                               std::string_view  new_target);

/// 全 pass の inputs / output を走査し、 予約名 (__scene__ / __output__ /
/// __lut__ / __depth__) 以外を出現順に `intermediate_names` へ収集し直す。
/// 挿入 / 削除 API は内部で自動的に呼ぶ — 手で passes を編集した場合のみ
/// 明示的に呼ぶこと。
void rebuild_intermediate_targets(PostProcessChain& chain);

// ============================================================
// build_post_process_chain — 組み込みエフェクト → 汎用チェーン
// ============================================================
/// `PostProcessConfig` (= `build_post_process_config()` の出力) を汎用
/// `PostProcessChain` へ畳み込む。 `build_post_process_config()` と並列の
/// 第二経路 (`spec/rendering-extensibility-design.md` §6.3)。
///
/// 組み込み 4 エフェクト (Bloom / ToneMapping / Vignette / ColorGrading) は、
/// 旧 `PostProcessPipeline::record()` と「ビット単位で同一」 の 4 pass
/// (extract → blur H → blur V → grade) を生成する:
///   - extract : scene HDR → ping、 push = {threshold, soft_threshold,…}
///   - blur H  : ping → pong、 push = {1/w, 0, radius,…}
///   - blur V  : pong → ping、 push = {0, 1/h, radius,…}
///   - grade   : scene + ping → output、 push = GradePC レイアウト
/// disabled なエフェクトは旧実装と同じ「push constant で恒等へ縮退」 する
/// (Bloom 無効 → threshold 1e9、 tonemap 無効 → LINEAR_CLAMP 等)。
///
/// enabled なエフェクトに応じて任意 pass を途中挿入する
/// (`spec/feature/postprocess-effects-design.md` §2.1)。 フル構成:
///   scene → [dof] → [ssao_apply] → [motion_blur] →
///     extract → blur H → blur V → grade → [fxaa] → __output__
///   - dof         : scene + __depth__ → pp_dof (DofPC)
///   - ssao_apply  : 前段 + __depth__ → pp_ssao (SsaoPC、 深度差 AO を乗算)
///   - motion_blur : 前段 + __depth__ → pp_mblur (MotionBlurPC、 カメラ再投影)
///   - fxaa        : pp_ldr → __output__ (FxaaPC、 LDR。 有効時は grade の
///                   出力が pp_ldr へ差し替わる)
/// 後段の extract / grade は直前の挿入 pass の出力を読む (配線替え)。
/// 全て無効時のチェーンは従来どおりの 4 pass 構造で、 各 push 値は恒等へ
/// 縮退する (grade の push は CA / grain フィールド分だけ旧版より長い)。
/// 各エフェクトの有効 / 無効切替は構造変更なので
/// `PostProcessPipeline::rebuild_chain()` を要する
/// (値だけの変更は `refresh_post_process_chain()`)。
///
/// `shader_dir`        : 組み込みシェーダ SPIR-V のディレクトリ。
/// `extent_w/extent_h` : blur の texel ステップ計算に使う描画解像度。
/// `output_is_srgb`    : sRGB swapchain なら grade の gamma を 1.0 に倒す
///                       (旧実装の二重ガンマ回避をそのまま踏襲)。
/// `lut_loaded`        : 実 LUT が読めたか (false なら lut_intensity=0)。
///
/// 戻り値の `passes` には組み込み 4 pass のあと、 将来の任意 pass
/// (`extra` 引数) が続く。 `extra` の各 pass は inputs/output を論理名で
/// 指定でき、 SSAO / FXAA 等の挿入に使う (現状 KS は空で呼ぶ)。
PostProcessChain build_post_process_chain(const PostProcessConfig& cfg,
                                          const std::string&       shader_dir,
                                          uint32_t                 extent_w,
                                          uint32_t                 extent_h,
                                          bool                     output_is_srgb,
                                          bool                     lut_loaded,
                                          const std::vector<PostProcessPassDef>& extra = {});

/// `PostProcessChain` の push_data を現フレームの解像度 / config で更新する。
/// 解像度依存 (blur の texel step) と config 依存 (各エフェクトの値) を
/// 毎フレーム / resize 時に詰め直すための軽量経路。 pass 構造 (render pass /
/// pipeline) は変えない。
void refresh_post_process_chain(PostProcessChain&        chain,
                                const PostProcessConfig& cfg,
                                uint32_t                 extent_w,
                                uint32_t                 extent_h,
                                bool                     output_is_srgb,
                                bool                     lut_loaded);

} // namespace pictor
