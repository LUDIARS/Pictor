#pragma once

#include "pictor/pipeline/pipeline_profile.h"

#include <string>
#include <vector>

namespace pictor {

// ============================================================
// Pipeline profile loader — built-in presets from external JSON
// ============================================================
// The five built-in presets (Lite / Standard / Ultra / MobileLow /
// MobileHigh) are described declaratively in profiles/*.profile.json.
// This loader reads those files at startup and falls back to the
// hardcoded C++ factories in PipelineProfileManager when a file is
// missing or fails to parse — so the engine is always bootable.
//
// 依存ルール: ここは Pictor 下層レイヤー。 Ergo / ゲーム には依存しない。
// JSON 解釈は pipeline_profile_serializer.h (外部依存なし) に委譲する。

/// Result of one preset-load attempt — useful for diagnostics / logging.
struct PresetLoadResult {
    std::string preset_name;     ///< canonical preset name (e.g. "Standard")
    std::string source_path;     ///< file that was attempted
    bool        loaded_from_file = false; ///< false => C++ fallback used
    std::string error;           ///< parse/IO error if the file failed
};

/// Resolve a built-in preset by canonical name.
/// Tries `<profile_dir>/<lowercased-name>.profile.json` first; on any
/// failure returns the hardcoded C++ factory result instead.
/// `name` is one of: Lite, Standard, Ultra, MobileLow, MobileHigh.
/// `out_result` (optional) receives load diagnostics.
PipelineProfileDef load_builtin_preset(const std::string& name,
                                       const std::string& profile_dir,
                                       PresetLoadResult*   out_result = nullptr);

/// Load all five built-in presets, preferring JSON files under
/// `profile_dir`, falling back to C++ factories per-preset.
/// `out_results` (optional) receives one PresetLoadResult per preset.
std::vector<PipelineProfileDef> load_builtin_presets(
    const std::string&              profile_dir,
    std::vector<PresetLoadResult>*  out_results = nullptr);

/// Register the built-in presets into `mgr`, loading from `profile_dir`
/// where possible. Equivalent to PipelineProfileManager::register_defaults()
/// but data-driven. The active profile is set to "Standard".
/// `out_results` (optional) receives per-preset load diagnostics.
void register_presets_from_dir(PipelineProfileManager&        mgr,
                               const std::string&             profile_dir,
                               std::vector<PresetLoadResult>* out_results = nullptr);

} // namespace pictor
