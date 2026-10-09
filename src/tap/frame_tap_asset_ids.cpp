#include "pictor/tap/frame_tap_asset_ids.h"

#include <algorithm>

namespace pictor {

namespace {

constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime       = 0x100000001b3ULL;

void append_hex(std::string& out, uint64_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(value >> shift) & 0xF]);
    }
}

uint64_t fnv1a_mix_bytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t fnv1a_mix_u64(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= static_cast<uint8_t>(value >> (i * 8));
        hash *= kFnvPrime;
    }
    return hash;
}

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

bool frame_tap_mesh_content_hash(const void* vertex_data, size_t vertex_size,
                                 const void* index_data, size_t index_size,
                                 uint64_t& out) {
    if (!vertex_data) vertex_size = 0;
    if (!index_data) index_size = 0;
    if (vertex_size == 0 && index_size == 0) return false;
    // 長さを先に混ぜ、 頂点とインデックスの境目をずらした別データと区別する。
    uint64_t hash = kFnvOffsetBasis;
    hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(vertex_size));
    hash = fnv1a_mix_bytes(hash, vertex_data, vertex_size);
    hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(index_size));
    hash = fnv1a_mix_bytes(hash, index_data, index_size);
    out = hash;
    return true;
}

FrameTapIdentity resolve_mesh_id(const FrameTapMeshInfo& info, std::string& out) {
    out.clear();
    if (!info.name.empty()) {
        out.assign(info.name.data(), info.name.size());
        return FrameTapIdentity::ASSET_NAME;
    }
    if (info.has_content_hash) {
        out = "ch:";
        append_hex(out, info.content_hash);
        return FrameTapIdentity::CONTENT_HASH;
    }
    out = "fp:";
    append_hex(out, frame_tap_mesh_fingerprint(info.vertex_count, info.index_count));
    return FrameTapIdentity::COUNT_HASH;
}

uint64_t frame_tap_appearance_hash(std::string_view mesh,
                                   const std::vector<std::string>& materials,
                                   std::vector<uint64_t>& scratch) {
    scratch.clear();
    for (const std::string& material : materials) {
        scratch.push_back(fnv1a_mix_bytes(kFnvOffsetBasis, material.data(), material.size()));
    }
    std::sort(scratch.begin(), scratch.end());

    uint64_t hash = fnv1a_mix_u64(kFnvOffsetBasis, static_cast<uint64_t>(mesh.size()));
    hash = fnv1a_mix_bytes(hash, mesh.data(), mesh.size());
    hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(scratch.size()));
    for (const uint64_t material : scratch) hash = fnv1a_mix_u64(hash, material);
    return hash;
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
