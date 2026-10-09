#pragma once

/// FrameTapNameTable — ホストが与えるアセット名の表 (IFrameTapAssetSource の既定実装)。
///
/// ハンドルは 0 起点の連番なので、 ハンドルを添字にした平坦な配列で持つ
/// (draw ごとの検索を O(1) にし、 map の pointer chase を避ける)。 書き込みは
/// 登録時だけで、 フレーム中は読むだけ。

#include "pictor/tap/frame_tap_asset_source.h"

#include <string>
#include <vector>

namespace pictor {

class FrameTapNameTable final : public IFrameTapAssetSource {
public:
    /// メッシュの名前と指紋の材料を登録する (名前は空でもよい — 指紋で代用される)。
    void set_mesh(MeshHandle mesh, std::string name,
                  uint32_t vertex_count, uint32_t index_count);
    /// 登録済みメッシュの名前だけを差し替える (未登録なら数 0 で登録)。
    void set_mesh_name(MeshHandle mesh, std::string name);
    void clear_mesh(MeshHandle mesh);

    void set_material_name(MaterialHandle material, std::string name);
    void clear_material(MaterialHandle material);

    void set_ui_texture_name(uint32_t texture_id, std::string name);
    void clear_ui_texture(uint32_t texture_id);

    FrameTapMeshInfo mesh_info(MeshHandle mesh) const override;
    std::string_view material_name(MaterialHandle material) const override;
    std::string_view ui_texture_name(uint32_t texture_id) const override;

private:
    struct MeshEntry {
        std::string name;
        uint32_t    vertex_count = 0;
        uint32_t    index_count  = 0;
    };

    std::vector<MeshEntry>   meshes_;
    std::vector<std::string> materials_;
    std::vector<std::string> ui_textures_;
};

} // namespace pictor
