#include "pictor/tap/frame_tap_asset_ids.h"

namespace pictor {

namespace {

constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime       = 0x100000001b3ULL;

uint64_t fnv1a_mix_u32(uint64_t hash, uint32_t value) {
    // エンディアンに依らないよう下位バイトから順に混ぜる (ビルド間・機種間で同じ値)。
    for (int i = 0; i < 4; ++i) {
        hash ^= static_cast<uint8_t>(value >> (i * 8));
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

uint64_t frame_tap_mesh_fingerprint(uint32_t vertex_count, uint32_t index_count) {
    uint64_t hash = kFnvOffsetBasis;
    hash = fnv1a_mix_u32(hash, vertex_count);
    hash = fnv1a_mix_u32(hash, index_count);
    return hash;
}

bool resolve_mesh_id(const FrameTapMeshInfo& info, std::string& out) {
    out.clear();
    if (!info.name.empty()) {
        out.assign(info.name.data(), info.name.size());
        return true;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    const uint64_t fp = frame_tap_mesh_fingerprint(info.vertex_count, info.index_count);
    out = "fp:";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(fp >> shift) & 0xF]);
    }
    return false;
}

const char* frame_tap_ui_mesh_id(FrameTapUiKind kind) {
    switch (kind) {
        case FrameTapUiKind::RECT:       return "ui:rect";
        case FrameTapUiKind::NINE_SLICE: return "ui:nine_slice";
        case FrameTapUiKind::IMAGE:      return "ui:image";
    }
    return "ui:rect";
}

} // namespace pictor
