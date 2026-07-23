#pragma once

/// アルベドテクスチャ配列の構築 (SHaRC デモローカル)。
///
/// MTL の map_Kd (PNG/JPG) を一律 N×N RGBA8 (sRGB バイト列) へ
/// リサンプルし、 texture2DArray の 1 レイヤ = 1 テクスチャで詰める。
/// UV は sampler の REPEAT でタイルするため、 atlas 内パッキングではなく
/// 「配列」 を使う (Bistro は UV が [0,1] を大きく超える)。
///
/// SRP: 画像デコードとレイヤ割当のみ。 GPU 資源化は SharcGpuExecutor。

#include "ply_mesh.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sharc_demo {

struct TextureAtlas {
    std::vector<uint8_t> pixels;   ///< [layer][size*size*4] 連結 (sRGB)
    uint32_t size   = 0;           ///< 1 辺 (px)
    uint32_t layers = 0;
    /// テクスチャパス → レイヤ index
    std::unordered_map<std::string, uint32_t> layer_of;

    bool empty() const { return layers == 0; }
};

/// materials の texture パスを集めて配列を構築する (パス重複は共有、
/// デコードはスレッド並列)。 max_layers 超過分は切り捨て (stderr へ)。
TextureAtlas build_texture_atlas(const std::vector<PlyMaterial>& materials,
                                 uint32_t size, uint32_t max_layers);

}  // namespace sharc_demo
