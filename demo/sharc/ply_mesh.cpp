#include "ply_mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sharc_demo {

namespace {

struct PlyProperty {
    std::string name;
    std::string type;        // float / uchar / int / ...
    bool        is_list = false;
    std::string list_count_type;
    std::string list_item_type;
};

struct PlyElement {
    std::string name;
    size_t      count = 0;
    std::vector<PlyProperty> props;
};

size_t type_size(const std::string& t) {
    if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
    if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
    if (t == "int" || t == "uint" || t == "int32" || t == "uint32" ||
        t == "float" || t == "float32") return 4;
    if (t == "double" || t == "float64") return 8;
    return 0;
}

double read_binary_scalar(std::ifstream& in, const std::string& t) {
    unsigned char buf[8]{};
    const size_t n = type_size(t);
    in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
    if (t == "float" || t == "float32") {
        float v; std::memcpy(&v, buf, 4); return v;
    }
    if (t == "double" || t == "float64") {
        double v; std::memcpy(&v, buf, 8); return v;
    }
    if (t == "uchar" || t == "uint8") return buf[0];
    if (t == "char" || t == "int8") return static_cast<int8_t>(buf[0]);
    if (t == "ushort" || t == "uint16") {
        uint16_t v; std::memcpy(&v, buf, 2); return v;
    }
    if (t == "short" || t == "int16") {
        int16_t v; std::memcpy(&v, buf, 2); return v;
    }
    if (t == "uint" || t == "uint32") {
        uint32_t v; std::memcpy(&v, buf, 4); return v;
    }
    int32_t v; std::memcpy(&v, buf, 4); return v;
}

void accumulate_normals(PlyMesh& mesh) {
    mesh.normals.assign(mesh.positions.size(), {0.0f, 0.0f, 0.0f});
    for (const auto& tri : mesh.triangles) {
        const auto& a = mesh.positions[tri[0]];
        const auto& b = mesh.positions[tri[1]];
        const auto& c = mesh.positions[tri[2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        // 外積の大きさ = 面積×2 — 面積重みスムーズ法線
        const float nx = e1[1] * e2[2] - e1[2] * e2[1];
        const float ny = e1[2] * e2[0] - e1[0] * e2[2];
        const float nz = e1[0] * e2[1] - e1[1] * e2[0];
        for (uint32_t vi : tri) {
            mesh.normals[vi][0] += nx;
            mesh.normals[vi][1] += ny;
            mesh.normals[vi][2] += nz;
        }
    }
    for (auto& n : mesh.normals) {
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) {
            n[0] /= l; n[1] /= l; n[2] /= l;
        } else {
            n = {0.0f, 1.0f, 0.0f};
        }
    }
}

void compute_bounds(PlyMesh& mesh) {
    mesh.bounds_min = {1e30f, 1e30f, 1e30f};
    mesh.bounds_max = {-1e30f, -1e30f, -1e30f};
    for (const auto& p : mesh.positions) {
        for (int a = 0; a < 3; ++a) {
            mesh.bounds_min[a] = std::fmin(mesh.bounds_min[a], p[a]);
            mesh.bounds_max[a] = std::fmax(mesh.bounds_max[a], p[a]);
        }
    }
}

} // namespace

PlyMesh load_ply(const std::string& path) {
    PlyMesh mesh;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "[ply] cannot open: %s\n", path.c_str());
        return mesh;
    }

    // ── ヘッダ ──
    std::string line;
    std::getline(in, line);
    if (line.rfind("ply", 0) != 0) {
        std::fprintf(stderr, "[ply] not a PLY file: %s\n", path.c_str());
        return mesh;
    }
    bool binary = false;
    bool big_endian = false;
    std::vector<PlyElement> elements;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ss(line);
        std::string tok;
        ss >> tok;
        if (tok == "format") {
            std::string fmt; ss >> fmt;
            binary = (fmt.rfind("binary", 0) == 0);
            big_endian = (fmt == "binary_big_endian");
        } else if (tok == "element") {
            PlyElement e;
            ss >> e.name >> e.count;
            elements.push_back(e);
        } else if (tok == "property") {
            if (elements.empty()) continue;
            PlyProperty p;
            std::string t; ss >> t;
            if (t == "list") {
                p.is_list = true;
                ss >> p.list_count_type >> p.list_item_type >> p.name;
            } else {
                p.type = t;
                ss >> p.name;
            }
            elements.back().props.push_back(p);
        } else if (tok == "end_header") {
            break;
        }
    }
    if (big_endian) {
        std::fprintf(stderr, "[ply] big endian unsupported: %s\n", path.c_str());
        return mesh;
    }

    // ── 本体 ──
    for (const auto& elem : elements) {
        const bool is_vertex = (elem.name == "vertex");
        const bool is_face   = (elem.name == "face");
        if (is_vertex) mesh.positions.reserve(elem.count);
        if (is_face)   mesh.triangles.reserve(elem.count);

        for (size_t i = 0; i < elem.count; ++i) {
            float xyz[3] = {0, 0, 0};
            std::vector<uint32_t> face;
            if (binary) {
                for (const auto& p : elem.props) {
                    if (p.is_list) {
                        const auto n = static_cast<size_t>(
                            read_binary_scalar(in, p.list_count_type));
                        face.clear();
                        for (size_t k = 0; k < n; ++k) {
                            face.push_back(static_cast<uint32_t>(
                                read_binary_scalar(in, p.list_item_type)));
                        }
                    } else {
                        const double v = read_binary_scalar(in, p.type);
                        if (p.name == "x") xyz[0] = static_cast<float>(v);
                        else if (p.name == "y") xyz[1] = static_cast<float>(v);
                        else if (p.name == "z") xyz[2] = static_cast<float>(v);
                    }
                }
            } else {
                if (!std::getline(in, line)) break;
                std::istringstream ss(line);
                for (const auto& p : elem.props) {
                    if (p.is_list) {
                        size_t n = 0; ss >> n;
                        face.clear();
                        for (size_t k = 0; k < n; ++k) {
                            uint32_t v = 0; ss >> v;
                            face.push_back(v);
                        }
                    } else {
                        double v = 0; ss >> v;
                        if (p.name == "x") xyz[0] = static_cast<float>(v);
                        else if (p.name == "y") xyz[1] = static_cast<float>(v);
                        else if (p.name == "z") xyz[2] = static_cast<float>(v);
                    }
                }
            }
            if (is_vertex) {
                mesh.positions.push_back({xyz[0], xyz[1], xyz[2]});
            } else if (is_face && face.size() >= 3) {
                // 多角形は fan 分割
                for (size_t k = 2; k < face.size(); ++k) {
                    mesh.triangles.push_back({face[0],
                                              face[k - 1],
                                              face[k]});
                }
            }
        }
    }

    if (mesh.positions.empty() || mesh.triangles.empty()) {
        std::fprintf(stderr, "[ply] no geometry: %s\n", path.c_str());
        mesh = PlyMesh{};
        return mesh;
    }
    accumulate_normals(mesh);
    compute_bounds(mesh);
    std::fprintf(stderr, "[ply] loaded %s: %zu verts, %zu tris\n",
                 path.c_str(), mesh.positions.size(), mesh.triangles.size());
    return mesh;
}

void fit_mesh(PlyMesh& mesh, float target_extent) {
    if (mesh.empty()) return;
    float extent = 0.0f;
    for (int a = 0; a < 3; ++a) {
        extent = std::fmax(extent, mesh.bounds_max[a] - mesh.bounds_min[a]);
    }
    if (extent < 1e-12f) return;
    const float s = target_extent / extent;
    const float cx = 0.5f * (mesh.bounds_min[0] + mesh.bounds_max[0]);
    const float cz = 0.5f * (mesh.bounds_min[2] + mesh.bounds_max[2]);
    const float y0 = mesh.bounds_min[1];
    for (auto& p : mesh.positions) {
        p[0] = (p[0] - cx) * s;
        p[1] = (p[1] - y0) * s;
        p[2] = (p[2] - cz) * s;
    }
    compute_bounds(mesh);
}

}  // namespace sharc_demo
