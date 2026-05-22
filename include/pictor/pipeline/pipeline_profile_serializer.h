#pragma once

#include "pictor/pipeline/pipeline_profile.h"

#include <string>

namespace pictor {

// ============================================================
// Pipeline profile serializer — PipelineProfileDef <-> JSON
// ============================================================
// visus_serializer.h / material_serializer.h と同じ手書きパーサ方針。
// 外部依存なし。 未知 key は黙ってスキップ、 構文エラーのみ false。
//
// このシリアライザは「系統A」 (PipelineProfileDef = 宣言的レンダリング
// プロファイル) を外部 JSON で完全に表現するためのもの。 後続の Ergo-web
// 編集ツールはここで定義される JSON スキーマに準拠する。
// 正本スキーマとサンプルは spec/pipeline-profile-config.md に明文化。
//
// 重要 (正直なスコープ): PipelineProfileDef は現状「系統B」 (実 VkRenderPass
// チェーン = src/surface/vulkan_context.cpp / src/postprocess/) と未接続。
// この JSON が今すぐ制御できる範囲 / 系統B でハードコードのまま残る範囲は
// spec/pipeline-profile-config.md の "Scope" 節を参照。
//
// JSON layout (schema version 1):
//   {
//     "version": 1,
//     "profile_name": "Standard",
//     "rendering_path": "FORWARD_PLUS",   // FORWARD|FORWARD_PLUS|DEFERRED|HYBRID
//     "max_lights": 256,
//     "msaa_samples": 4,                   // 0|2|4|8
//     "gpu_driven_enabled": true,
//     "compute_update_enabled": false,
//     "render_passes": [
//       {
//         "pass_name": "OpaquePass",
//         "pass_type": "OPAQUE",           // DEPTH_ONLY|OPAQUE|TRANSPARENT|
//                                          // SHADOW|POST_PROCESS|COMPUTE|CUSTOM
//         "shader_override": "none",       // "none" | "handle:<u32>"
//         "render_targets":  ["sceneColor"],
//         "input_textures":  ["shadowAtlas"],
//         "sort_mode": "FRONT_TO_BACK",    // FRONT_TO_BACK|BACK_TO_FRONT|NONE
//         "filter_mask": 65535,
//         "gpu_driven_pass": false,
//         "required_streams": ["transforms", "shaderKeys"]
//       }
//     ],
//     "post_process": [
//       { "name": "Bloom", "enabled": true, "kind": "Bloom",
//         "bloom": { "threshold": 0.75, "intensity": 0.8, "radius": 5.0,
//                    "mip_levels": 5, "scatter": 0.7 } },
//       { "name": "Tonemapping", "enabled": true, "kind": "ToneMapping",
//         "tone_mapping": { "op": "ACES_FILMIC", "exposure": 1.0,
//                           "gamma": 2.2, "saturation": 1.0 } }
//     ],
//     // `kind` ∈ Bloom|ToneMapping|Vignette|ColorGrading|DepthOfField|Unknown
//     // (省略時は name から推論)。 effect param ブロックは kind 一致時のみ
//     // 消費 (spec §3.4)。 Unknown (SSAO/TAA/FXAA…) は bridge が無視する。
//     "shadow": {
//       "cascade_count": 3, "resolution": 2048, "filter_mode": "PCF"
//     },
//     "gi": {
//       "shadow_enabled": true, "ssao_enabled": true, "gi_probes_enabled": false,
//       "shadow": { "cascade_count": 3, "resolution": 2048, "depth_bias": 0.005,
//                   "normal_bias": 0.02, "slope_scale_bias": 0.005,
//                   "cascade_lambda": 0.75, "max_shadow_dist": 200.0,
//                   "cascade_blend_width": 0.1, "filter_mode": "PCF",
//                   "shadow_strength": 1.0, "pcss_light_size": 0.04,
//                   "pcss_min_penumbra": 0.5, "pcss_max_penumbra": 16.0,
//                   "pcss_blocker_search_radius": 8.0 },
//       "ssao":   { "sample_count": 32, "radius": 0.5, "bias": 0.025,
//                   "intensity": 1.0, "falloff_start": 50.0,
//                   "falloff_end": 100.0, "blur_enabled": true },
//       "probes": { "grid_origin": [0,0,0], "grid_spacing": [4,4,4],
//                   "grid_x": 16, "grid_y": 8, "grid_z": 16,
//                   "gi_intensity": 1.0, "max_probe_distance": 16.0 }
//     },
//     "memory": {
//       "frame_allocator_size": 16777216, "flight_count": 3,
//       "pool_chunk_size": 65536, "use_large_pages": false,
//       "gpu": { "mesh_pool_size": 268435456, "ssbo_pool_size": 134217728,
//                "instance_buffer_size": 67108864,
//                "indirect_buffer_size": 16777216,
//                "staging_buffer_size": 67108864 }
//     },
//     "gpu_driven": {
//       "max_triangle_count": 50000, "min_instance_count": 32,
//       "workgroup_size": 256, "two_phase_culling": true, "compute_update": false
//     },
//     "update": {
//       "chunk_size": 16384, "worker_threads": 0,
//       "nt_store_enabled": true, "nt_store_threshold": 10000
//     },
//     "profiler": {
//       "enabled": true, "overlay_mode": "STANDARD", "max_queries": 64
//     }
//   }
//
// 全 byte サイズはバイト単位の整数 (memory.frame_allocator_size 等)。
// 後方互換のため、 欠落フィールドは PipelineProfileDef の C++ 既定値、
// もしくは from_pipeline_profile_json() に渡した preset の値を保持する。

/// Serialize `def` to canonical pretty-printed JSON (schema version 1).
std::string to_pipeline_profile_json(const PipelineProfileDef& def);

/// Parse JSON into a fresh PipelineProfileDef.
/// 構文エラー時のみ false、 欠落フィールドは PipelineProfileDef の既定値。
/// `error` (non-null) に短い理由を入れる。
bool from_pipeline_profile_json(const std::string& json,
                                PipelineProfileDef& out,
                                std::string*        error = nullptr);

/// Parse JSON seeded from an existing preset, so the JSON only needs to
/// specify overrides. Arrays (render_passes / post_process) replace the
/// preset's content entirely when present, scalars override per-field.
bool from_pipeline_profile_json(const std::string&        json,
                                const PipelineProfileDef& preset,
                                PipelineProfileDef&       out,
                                std::string*              error = nullptr);

/// Load a pipeline profile from a JSON file on disk.
/// I/O failure or syntax error -> false. `error` carries a short reason.
bool load_pipeline_profile_file(const std::string&  path,
                                PipelineProfileDef& out,
                                std::string*        error = nullptr);

/// Load a pipeline profile from a JSON file, seeded from `preset`.
bool load_pipeline_profile_file(const std::string&        path,
                                const PipelineProfileDef& preset,
                                PipelineProfileDef&       out,
                                std::string*              error = nullptr);

/// Write `def` to a JSON file on disk. Returns false on I/O failure.
bool save_pipeline_profile_file(const std::string&        path,
                                const PipelineProfileDef& def,
                                std::string*              error = nullptr);

} // namespace pictor
