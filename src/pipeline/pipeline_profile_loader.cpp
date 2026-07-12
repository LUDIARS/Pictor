#include "pictor/pipeline/pipeline_profile_loader.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace pictor {

namespace {

// Canonical preset name -> hardcoded C++ factory. This is the always-available
// fallback when the JSON file is missing or malformed.
PipelineProfileDef cpp_fallback(const std::string& name) {
    if (name == "Low")        return PipelineProfileManager::create_low_profile();
    if (name == "Mid")        return PipelineProfileManager::create_mid_profile();
    if (name == "High")       return PipelineProfileManager::create_high_profile();
    if (name == "Lite")       return PipelineProfileManager::create_lite_profile();
    if (name == "Standard")   return PipelineProfileManager::create_standard_profile();
    if (name == "Ultra")      return PipelineProfileManager::create_ultra_profile();
    if (name == "MobileLow")  return PipelineProfileManager::create_mobile_low_profile();
    if (name == "MobileHigh") return PipelineProfileManager::create_mobile_high_profile();
    // Unknown name — Standard is a safe, balanced default.
    return PipelineProfileManager::create_standard_profile();
}

std::string lowercased(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string preset_file_path(const std::string& dir, const std::string& name) {
    std::string path = dir;
    if (!path.empty()) {
        char last = path.back();
        if (last != '/' && last != '\\') path.push_back('/');
    }
    path += lowercased(name);
    path += ".profile.json";
    return path;
}

constexpr std::array<const char*, 8> kBuiltinPresetNames = {
    "Low", "Mid", "High", "Lite", "Standard", "Ultra", "MobileLow", "MobileHigh"};

} // namespace

PipelineProfileDef load_builtin_preset(const std::string& name,
                                       const std::string& profile_dir,
                                       PresetLoadResult*   out_result) {
    PresetLoadResult result;
    result.preset_name = name;

    // Seed from the C++ factory so that a partial JSON (overrides only)
    // still produces a complete, correct profile. The file's content
    // takes precedence; arrays replace, scalars override.
    PipelineProfileDef fallback = cpp_fallback(name);

    if (!profile_dir.empty()) {
        result.source_path = preset_file_path(profile_dir, name);
        PipelineProfileDef loaded;
        std::string err;
        if (load_pipeline_profile_file(result.source_path, fallback, loaded, &err)) {
            result.loaded_from_file = true;
            // The file may omit profile_name; force the canonical name so
            // PipelineProfileManager keys the profile correctly.
            if (loaded.profile_name.empty()) loaded.profile_name = name;
            if (out_result) *out_result = result;
            return loaded;
        }
        result.error = err;
    }

    // Fallback path: file missing, dir empty, or parse failure.
    if (out_result) *out_result = result;
    return fallback;
}

std::vector<PipelineProfileDef> load_builtin_presets(
    const std::string&             profile_dir,
    std::vector<PresetLoadResult>* out_results) {

    std::vector<PipelineProfileDef> profiles;
    profiles.reserve(kBuiltinPresetNames.size());
    if (out_results) {
        out_results->clear();
        out_results->reserve(kBuiltinPresetNames.size());
    }

    for (const char* name : kBuiltinPresetNames) {
        PresetLoadResult r;
        profiles.push_back(load_builtin_preset(name, profile_dir, &r));
        if (out_results) out_results->push_back(std::move(r));
    }
    return profiles;
}

void register_presets_from_dir(PipelineProfileManager&        mgr,
                               const std::string&             profile_dir,
                               std::vector<PresetLoadResult>* out_results) {
    auto profiles = load_builtin_presets(profile_dir, out_results);
    for (const auto& p : profiles) {
        mgr.register_profile(p);
    }
    mgr.set_profile("Standard");
}

} // namespace pictor
