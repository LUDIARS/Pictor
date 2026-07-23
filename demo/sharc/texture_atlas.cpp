#include "texture_atlas.h"

#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace sharc_demo {

namespace {

/// bilinear リサンプル (RGB8 入力 → RGBA8 出力、 sRGB バイトのまま)。
void resample_to(const unsigned char* src, int sw, int sh, uint8_t* dst,
                 uint32_t size) {
    for (uint32_t y = 0; y < size; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) /
                             static_cast<float>(size) *
                             static_cast<float>(sh) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (uint32_t x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) /
                                 static_cast<float>(size) *
                                 static_cast<float>(sw) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0,
                                      sw - 1);
            const int x1 = std::min(x0 + 1, sw - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f,
                                        1.0f);
            uint8_t* out = dst + (static_cast<size_t>(y) * size + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float v00 = src[(static_cast<size_t>(y0) * sw + x0) * 3 + c];
                const float v10 = src[(static_cast<size_t>(y0) * sw + x1) * 3 + c];
                const float v01 = src[(static_cast<size_t>(y1) * sw + x0) * 3 + c];
                const float v11 = src[(static_cast<size_t>(y1) * sw + x1) * 3 + c];
                const float v = (v00 * (1 - tx) + v10 * tx) * (1 - ty) +
                                (v01 * (1 - tx) + v11 * tx) * ty;
                out[c] = static_cast<uint8_t>(
                    std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
            out[3] = 255;
        }
    }
}

} // namespace

TextureAtlas build_texture_atlas(const std::vector<PlyMaterial>& materials,
                                 uint32_t size, uint32_t max_layers) {
    TextureAtlas atlas;
    atlas.size = size;

    // パス重複を共有しつつレイヤを確定 (順序決定的)
    std::vector<std::string> paths;
    for (const auto& m : materials) {
        if (m.texture.empty()) continue;
        if (atlas.layer_of.count(m.texture) != 0) continue;
        if (paths.size() >= max_layers) {
            std::fprintf(stderr,
                         "[atlas] layer cap %u reached — dropping %s\n",
                         max_layers, m.texture.c_str());
            continue;
        }
        atlas.layer_of.emplace(m.texture,
                               static_cast<uint32_t>(paths.size()));
        paths.push_back(m.texture);
    }
    atlas.layers = static_cast<uint32_t>(paths.size());
    if (atlas.layers == 0) return atlas;

    const size_t layer_bytes = static_cast<size_t>(size) * size * 4;
    atlas.pixels.assign(layer_bytes * atlas.layers, 0);

    std::atomic<size_t> next{0};
    std::atomic<int> failed{0};
    auto worker = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= paths.size()) return;
            int w = 0, h = 0, n = 0;
            unsigned char* img = stbi_load(paths[i].c_str(), &w, &h, &n, 3);
            if (img == nullptr || w <= 0 || h <= 0) {
                failed.fetch_add(1);
                if (img) stbi_image_free(img);
                // 失敗レイヤは中間グレーで埋める
                std::memset(atlas.pixels.data() + layer_bytes * i, 128,
                            layer_bytes);
                continue;
            }
            resample_to(img, w, h, atlas.pixels.data() + layer_bytes * i,
                        size);
            stbi_image_free(img);
        }
    };
    const unsigned n_threads =
        std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    pool.reserve(n_threads);
    for (unsigned i = 0; i < n_threads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    std::fprintf(stderr,
                 "[atlas] %u layers @ %ux%u (%.1f MB, %d decode failures)\n",
                 atlas.layers, size, size,
                 static_cast<double>(atlas.pixels.size()) / (1024.0 * 1024.0),
                 failed.load());
    return atlas;
}

}  // namespace sharc_demo
