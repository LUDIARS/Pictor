#include "obj_mesh.h"

#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sharc_demo {

namespace {

/// ファイル全体を読み込む (300MB 級テキストを行ストリームで舐めると
/// 遅すぎるため、 メモリ上をポインタで歩く)。
std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return {};
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::string buf(static_cast<size_t>(size), '\0');
    in.read(buf.data(), size);
    return buf;
}

const char* skip_ws(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

const char* next_line(const char* p, const char* end) {
    while (p < end && *p != '\n') ++p;
    return (p < end) ? p + 1 : end;
}

/// Ns (Phong 指数) → GGX roughness の慣例近似 (α = sqrt(2/(Ns+2)))。
float ns_to_roughness(float ns) {
    const float alpha = std::sqrt(2.0f / (std::max(ns, 0.0f) + 2.0f));
    return std::clamp(std::sqrt(alpha), 0.05f, 1.0f);
}

std::string dir_of(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

std::string normalize_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

/// MTL を読み、 名前 → マテリアルの表を返す (Kd / Ns / map_Kd)。
std::unordered_map<std::string, PlyMaterial>
load_mtl(const std::string& path) {
    std::unordered_map<std::string, PlyMaterial> mats;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::fprintf(stderr, "[obj] mtl not found (clay render): %s\n",
                     path.c_str());
        return mats;
    }
    const std::string mtl_dir = dir_of(path);
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tok;
        ss >> tok;
        if (tok == "newmtl") {
            ss >> current;
            mats[current] = PlyMaterial{};
        } else if (tok == "Kd" && !current.empty()) {
            auto& m = mats[current];
            ss >> m.albedo[0] >> m.albedo[1] >> m.albedo[2];
        } else if (tok == "Ns" && !current.empty()) {
            float ns = 0.0f;
            ss >> ns;
            mats[current].roughness = ns_to_roughness(ns);
        } else if (tok == "map_Kd" && !current.empty()) {
            // 残り全体をパスとして取る (オプション引数は Bistro には無い)
            std::string rest;
            std::getline(ss, rest);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                rest.erase(rest.begin());
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' ||
                                     rest.back() == '\r'))
                rest.pop_back();
            if (!rest.empty()) {
                mats[current].texture = mtl_dir + normalize_slashes(rest);
            }
        }
    }
    // テクスチャなしマテリアルのみクレイ抑制 (白 Kd の露出暴れ防止)。
    // テクスチャありは Kd をそのまま乗数として使う。
    for (auto& [name, m] : mats) {
        if (m.texture.empty()) {
            for (float& c : m.albedo) c = std::min(c, 0.8f);
        }
    }
    std::fprintf(stderr, "[obj] mtl loaded: %zu materials\n", mats.size());
    return mats;
}

/// マテリアル名から植生 (葉・花) を推定して SSS を付ける。
/// テクスチャなしで Kd が白い葉はクレイ視認性のため緑へ寄せる。
void tag_foliage(const std::string& name, PlyMaterial& m) {
    std::string low = name;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool leafy =
        (low.find("leaf") != std::string::npos ||
         low.find("leaves") != std::string::npos ||
         low.find("foliage") != std::string::npos ||
         low.find("flower") != std::string::npos ||
         low.find("ivy") != std::string::npos ||
         low.find("hedge") != std::string::npos);
    const bool woody =
        (low.find("trunk") != std::string::npos ||
         low.find("branch") != std::string::npos);
    if (!leafy || woody) return;
    m.mfp = 0.03f;   // 葉の透過 (数 cm オーダー)
    if (m.texture.empty() && m.albedo[0] > 0.7f && m.albedo[1] > 0.7f &&
        m.albedo[2] > 0.7f) {
        m.albedo = {0.35f, 0.55f, 0.30f};
    }
}

float srgb_to_linear(float v) {
    return (v <= 0.04045f) ? v / 12.92f
                           : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

uint32_t pack_rgb8(float r, float g, float b) {
    const auto q = [](float v) {
        return static_cast<uint32_t>(
            std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
    };
    return q(r) | (q(g) << 8) | (q(b) << 16);
}

/// bilinear サンプル (wrap)。 img は RGB8、 戻り値はリニア RGB。
void sample_bilinear(const unsigned char* img, int w, int h, float u, float v,
                     float out[3]) {
    // OBJ の vt は左下原点、 stb は上段から → v 反転
    u = u - std::floor(u);
    v = 1.0f - (v - std::floor(v));
    const float fx = u * static_cast<float>(w) - 0.5f;
    const float fy = v * static_cast<float>(h) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto wrap = [](int a, int n) { return ((a % n) + n) % n; };
    float acc[3] = {0, 0, 0};
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int x = wrap(x0 + dx, w);
            const int y = wrap(y0 + dy, h);
            const float wgt = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
            const unsigned char* px = img + (static_cast<size_t>(y) * w + x) * 3;
            acc[0] += wgt * static_cast<float>(px[0]) / 255.0f;
            acc[1] += wgt * static_cast<float>(px[1]) / 255.0f;
            acc[2] += wgt * static_cast<float>(px[2]) / 255.0f;
        }
    }
    out[0] = srgb_to_linear(acc[0]);
    out[1] = srgb_to_linear(acc[1]);
    out[2] = srgb_to_linear(acc[2]);
}

} // namespace

PlyMesh load_obj(const std::string& path) {
    PlyMesh mesh;
    const std::string text = read_all(path);
    if (text.empty()) {
        std::fprintf(stderr, "[obj] cannot open: %s\n", path.c_str());
        return mesh;
    }

    std::unordered_map<std::string, PlyMaterial> mtl_table;
    std::unordered_map<std::string, uint32_t> mat_index;
    uint32_t current_mat = 0;
    mesh.materials.push_back(PlyMaterial{});   // 既定 (usemtl 前)

    std::vector<std::array<float, 2>> uvs;
    // 三角形コーナーごとの UV (焼き込み後に破棄するローカル)
    std::vector<std::array<std::array<float, 2>, 3>> tri_uvs;

    const char* p   = text.data();
    const char* end = p + text.size();
    std::vector<int64_t> face;
    std::vector<int64_t> face_uv;

    while (p < end) {
        p = skip_ws(p, end);
        if (p + 1 < end && p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            char* q = nullptr;
            const float x = std::strtof(p + 2, &q);
            const float y = std::strtof(q, &q);
            const float z = std::strtof(q, &q);
            mesh.positions.push_back({x, y, z});
        } else if (p + 2 < end && p[0] == 'v' && p[1] == 't' &&
                   (p[2] == ' ' || p[2] == '\t')) {
            char* q = nullptr;
            const float u = std::strtof(p + 3, &q);
            const float v = std::strtof(q, &q);
            uvs.push_back({u, v});
        } else if (p + 1 < end && p[0] == 'f' &&
                   (p[1] == ' ' || p[1] == '\t')) {
            face.clear();
            face_uv.clear();
            const char* q = p + 2;
            while (q < end && *q != '\n' && *q != '\r') {
                q = skip_ws(q, end);
                if (q >= end || *q == '\n' || *q == '\r') break;
                char* r = nullptr;
                const long long vi = std::strtoll(q, &r, 10);
                if (r == q) break;
                long long ti = 0;
                q = r;
                if (q < end && *q == '/') {
                    ++q;
                    if (q < end && *q != '/') {
                        ti = std::strtoll(q, &r, 10);
                        q = r;
                    }
                }
                face.push_back(vi);
                face_uv.push_back(ti);
                // 残り (法線 index 等) を読み飛ばす
                while (q < end && *q != ' ' && *q != '\t' && *q != '\n' &&
                       *q != '\r') {
                    ++q;
                }
            }
            const auto vcount = static_cast<int64_t>(mesh.positions.size());
            const auto tcount = static_cast<int64_t>(uvs.size());
            auto resolve = [vcount](int64_t i) -> uint32_t {
                return static_cast<uint32_t>(i > 0 ? i - 1 : vcount + i);
            };
            auto uv_of = [&](int64_t i) -> std::array<float, 2> {
                if (i == 0 || uvs.empty()) return {0.0f, 0.0f};
                const auto idx =
                    static_cast<size_t>(i > 0 ? i - 1 : tcount + i);
                return (idx < uvs.size()) ? uvs[idx]
                                          : std::array<float, 2>{0.0f, 0.0f};
            };
            for (size_t k = 2; k < face.size(); ++k) {
                mesh.triangles.push_back({resolve(face[0]),
                                          resolve(face[k - 1]),
                                          resolve(face[k])});
                mesh.tri_material.push_back(current_mat);
                tri_uvs.push_back({uv_of(face_uv[0]), uv_of(face_uv[k - 1]),
                                   uv_of(face_uv[k])});
            }
        } else if (p + 6 < end && std::memcmp(p, "usemtl", 6) == 0) {
            const char* q = skip_ws(p + 6, end);
            const char* r = q;
            while (r < end && *r != '\n' && *r != '\r' && *r != ' ') ++r;
            const std::string name(q, r);
            auto it = mat_index.find(name);
            if (it != mat_index.end()) {
                current_mat = it->second;
            } else {
                PlyMaterial m;
                auto mt = mtl_table.find(name);
                if (mt != mtl_table.end()) m = mt->second;
                tag_foliage(name, m);
                current_mat = static_cast<uint32_t>(mesh.materials.size());
                mesh.materials.push_back(m);
                mat_index.emplace(name, current_mat);
            }
        } else if (p + 6 < end && std::memcmp(p, "mtllib", 6) == 0) {
            const char* q = skip_ws(p + 6, end);
            const char* r = q;
            while (r < end && *r != '\n' && *r != '\r') ++r;
            std::string name(q, r);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();
            mtl_table = load_mtl(dir_of(path) + name);
        }
        p = next_line(p, end);
    }

    if (mesh.positions.empty() || mesh.triangles.empty()) {
        std::fprintf(stderr, "[obj] no geometry: %s\n", path.c_str());
        mesh = PlyMesh{};
        return mesh;
    }

    // ── テクスチャ焼き込み: map_Kd をコーナー UV でサンプルして RGB8 ──
    //    (テクスチャなしはマテリアル albedo 一色)。 マテリアル単位で並列。
    const size_t tri_count = mesh.triangles.size();
    mesh.tri_corner_colors.resize(tri_count);
    for (size_t t = 0; t < tri_count; ++t) {
        const auto& m = mesh.materials[mesh.tri_material[t]];
        const uint32_t c = pack_rgb8(m.albedo[0], m.albedo[1], m.albedo[2]);
        mesh.tri_corner_colors[t] = {c, c, c};
    }

    std::vector<std::vector<uint32_t>> tris_by_mat(mesh.materials.size());
    for (size_t t = 0; t < tri_count; ++t) {
        tris_by_mat[mesh.tri_material[t]].push_back(
            static_cast<uint32_t>(t));
    }
    std::vector<uint32_t> textured;
    for (uint32_t mi = 0; mi < mesh.materials.size(); ++mi) {
        if (!mesh.materials[mi].texture.empty() &&
            !tris_by_mat[mi].empty()) {
            textured.push_back(mi);
        }
    }
    if (!textured.empty()) {
        std::atomic<size_t> next{0};
        std::atomic<int> failed{0};
        const unsigned n_threads =
            std::max(1u, std::thread::hardware_concurrency());
        auto worker = [&]() {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= textured.size()) return;
                const uint32_t mi = textured[i];
                const auto& mat = mesh.materials[mi];
                int w = 0, h = 0, n = 0;
                unsigned char* img =
                    stbi_load(mat.texture.c_str(), &w, &h, &n, 3);
                if (img == nullptr || w <= 0 || h <= 0) {
                    failed.fetch_add(1);
                    if (img) stbi_image_free(img);
                    continue;
                }
                for (const uint32_t t : tris_by_mat[mi]) {
                    for (int c = 0; c < 3; ++c) {
                        float rgb[3];
                        sample_bilinear(img, w, h, tri_uvs[t][c][0],
                                        tri_uvs[t][c][1], rgb);
                        // Kd はテクスチャの乗数 (Bistro はほぼ 1.0)
                        mesh.tri_corner_colors[t][c] = pack_rgb8(
                            rgb[0] * mat.albedo[0], rgb[1] * mat.albedo[1],
                            rgb[2] * mat.albedo[2]);
                    }
                }
                stbi_image_free(img);
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(n_threads);
        for (unsigned i = 0; i < n_threads; ++i) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
        std::fprintf(stderr,
                     "[obj] baked %zu textured materials (%d load failures)\n",
                     textured.size(), failed.load());
    }

    finalize_mesh(mesh);
    std::fprintf(stderr, "[obj] loaded %s: %zu verts, %zu tris, %zu mats\n",
                 path.c_str(), mesh.positions.size(), mesh.triangles.size(),
                 mesh.materials.size());
    return mesh;
}

}  // namespace sharc_demo
