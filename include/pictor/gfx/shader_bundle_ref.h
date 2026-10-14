#pragma once

#include <string>

namespace pictor::gfx {

/// Name of a shader bundle (one source compiled for every enabled backend,
/// spec/architecture/multi-rhi.md §4). The backend picks its own binary from
/// the bundle; consumers only ever name the bundle.
///
///   ShaderBundleRef{"my_game/avatar_sdf"}
struct ShaderBundleRef {
    std::string name;

    bool empty() const noexcept { return name.empty(); }

    friend bool operator==(const ShaderBundleRef& a, const ShaderBundleRef& b) noexcept {
        return a.name == b.name;
    }
    friend bool operator!=(const ShaderBundleRef& a, const ShaderBundleRef& b) noexcept {
        return !(a == b);
    }
};

} // namespace pictor::gfx
