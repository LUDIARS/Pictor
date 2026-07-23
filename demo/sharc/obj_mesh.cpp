#include "obj_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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

/// MTL を読み、 名前 → マテリアルの表を返す (Kd / Ns のみ)。
std::unordered_map<std::string, PlyMaterial>
load_mtl(const std::string& path) {
    std::unordered_map<std::string, PlyMaterial> mats;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::fprintf(stderr, "[obj] mtl not found (clay render): %s\n",
                     path.c_str());
        return mats;
    }
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
            // テクスチャ前提の Kd=1.0 白は露出が暴れるのでクレイ灰へ抑える
            for (float& c : m.albedo) c = std::min(c, 0.8f);
        } else if (tok == "Ns" && !current.empty()) {
            float ns = 0.0f;
            ss >> ns;
            mats[current].roughness = ns_to_roughness(ns);
        }
    }
    std::fprintf(stderr, "[obj] mtl loaded: %zu materials\n", mats.size());
    return mats;
}

std::string dir_of(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
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

    const char* p   = text.data();
    const char* end = p + text.size();
    std::vector<int64_t> face;   // 一時 (負 index 対応)

    while (p < end) {
        p = skip_ws(p, end);
        if (p + 1 < end && p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            char* q = nullptr;
            const float x = std::strtof(p + 2, &q);
            const float y = std::strtof(q, &q);
            const float z = std::strtof(q, &q);
            mesh.positions.push_back({x, y, z});
        } else if (p + 1 < end && p[0] == 'f' &&
                   (p[1] == ' ' || p[1] == '\t')) {
            face.clear();
            const char* q = p + 2;
            while (q < end && *q != '\n' && *q != '\r') {
                q = skip_ws(q, end);
                if (q >= end || *q == '\n' || *q == '\r') break;
                char* r = nullptr;
                const long long vi = std::strtoll(q, &r, 10);
                if (r == q) break;
                face.push_back(vi);
                // v/vt/vn の残りを読み飛ばす
                q = r;
                while (q < end && *q != ' ' && *q != '\t' && *q != '\n' &&
                       *q != '\r') {
                    ++q;
                }
            }
            const auto vcount = static_cast<int64_t>(mesh.positions.size());
            auto resolve = [vcount](int64_t i) -> uint32_t {
                return static_cast<uint32_t>(i > 0 ? i - 1 : vcount + i);
            };
            for (size_t k = 2; k < face.size(); ++k) {
                mesh.triangles.push_back({resolve(face[0]),
                                          resolve(face[k - 1]),
                                          resolve(face[k])});
                mesh.tri_material.push_back(current_mat);
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
    finalize_mesh(mesh);
    std::fprintf(stderr, "[obj] loaded %s: %zu verts, %zu tris, %zu mats\n",
                 path.c_str(), mesh.positions.size(), mesh.triangles.size(),
                 mesh.materials.size());
    return mesh;
}

}  // namespace sharc_demo
