#include "pictor/visus/visus_shader_package.h"

#include <unordered_map>

namespace pictor {

void visus_metadata_merge(VisusMetadata& base, const VisusMetadata& over) {
    base.merge_from(over);
}

void visus_package_ref_merge(VisusPackageRef& base, const VisusPackageRef& over) {
    base.enabled = over.enabled;
    visus_metadata_merge(base.params, over.params);
    visus_metadata_merge(base.metadata, over.metadata);
}

namespace {

/// `list` へ 1 件重ねる。 同名があればその位置でマージ、 無ければ末尾へ追加。
void overlay(std::vector<VisusPackageRef>& list,
             std::unordered_map<std::string, size_t>& positions,
             const VisusPackageRef& ref) {
    if (!ref.valid()) return;
    const auto found = positions.find(ref.package);
    if (found == positions.end()) {
        positions.emplace(ref.package, list.size());
        list.push_back(ref);
        return;
    }
    visus_package_ref_merge(list[found->second], ref);
}

} // namespace

std::vector<VisusPackageRef> visus_effective_packages(
    const std::vector<VisusPackageRef>& visus_level,
    const std::vector<VisusPackageRef>& part_level)
{
    std::vector<VisusPackageRef> merged;
    merged.reserve(visus_level.size() + part_level.size());
    std::unordered_map<std::string, size_t> positions;
    positions.reserve(visus_level.size() + part_level.size());
    for (const VisusPackageRef& r : visus_level) overlay(merged, positions, r);
    for (const VisusPackageRef& r : part_level)  overlay(merged, positions, r);

    std::vector<VisusPackageRef> out;
    out.reserve(merged.size());
    for (VisusPackageRef& r : merged) {
        if (r.enabled) out.push_back(std::move(r));
    }
    return out;
}

VisusMetadata visus_package_effective_params(const VisusShaderPackage& pkg,
                                             const VisusPackageRef&    ref) {
    VisusMetadata out = pkg.params;
    visus_metadata_merge(out, ref.params);
    return out;
}

VisusMetadata visus_package_effective_metadata(const VisusShaderPackage& pkg,
                                               const VisusPackageRef&    ref) {
    VisusMetadata out = pkg.metadata;
    visus_metadata_merge(out, ref.metadata);
    return out;
}

} // namespace pictor
