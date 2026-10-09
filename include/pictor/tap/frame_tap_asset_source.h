#pragma once

/// フレームタップがアセット名を引く口 (spec/feature/frame-tap.md §4)。
///
/// タップはハンドル番号を出さないので、 draw ごとに名前 (と名前が無いときの
/// 指紋の材料) をここから引く。 既定実装は FrameTapNameTable。

#include "pictor/core/types.h"

#include <cstdint>
#include <string_view>

namespace pictor {

/// メッシュ 1 つ分の名前と指紋の材料。 name が空なら「名前が取れない」。
struct FrameTapMeshInfo {
    std::string_view name;
    uint32_t         vertex_count = 0;
    uint32_t         index_count  = 0;
};

class IFrameTapAssetSource {
public:
    virtual ~IFrameTapAssetSource() = default;

    /// 未登録のハンドルは name 空・数 0。
    virtual FrameTapMeshInfo mesh_info(MeshHandle mesh) const = 0;

    /// 名前が無ければ空。
    virtual std::string_view material_name(MaterialHandle material) const = 0;

    /// UIRenderer の texture_id に対応するテクスチャ名。 無ければ空。
    virtual std::string_view ui_texture_name(uint32_t texture_id) const = 0;
};

} // namespace pictor
