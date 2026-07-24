/// Pictor SHaRC 拡張 — DirectX 12 移植のヘッドレス検証ドライバ
///
/// Vulkan 版 pictor_sharc_demo (demo/sharc/main.cpp) の Hero (Bistro) 経路を
/// ウィンドウ/入力なしで再現する: OBJ をロード → BVH → GPU シーンとして
/// SharcDx12Executor へ転送 → 固定カメラで 60 フレーム実行 → 出力バッファを
/// readback → sharc_dx12_out.ppm (P6) へ書き出し、 平均輝度 / NaN 数を stdout
/// へ出す。
///
/// 既存の Vulkan デモ資産 (obj_mesh/ply_mesh/mesh_bvh/texture_atlas) を
/// 無改変で再利用する。 GLSL/HLSL のアルゴリズムはここでは変更しない —
/// 本ファイルはシーン準備とドライバのみの責務 (SRP)。
///
/// Build target: pictor_sharc_dx12_test
/// 使い方: pictor_sharc_dx12_test <bistro.obj>

#include "pictor/gi/sharc_dx12_executor.h"
#include "mesh_bvh.h"
#include "obj_mesh.h"
#include "texture_atlas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifndef PICTOR_SHARC_HLSL_DIR
#define PICTOR_SHARC_HLSL_DIR "shaders/sharc/hlsl"
#endif

using namespace pictor;

namespace {

struct Vec3 { float x = 0, y = 0, z = 0; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 norm(Vec3 a) { float l = len(a); return (l > 1e-8f) ? a * (1.0f / l) : Vec3{0, 1, 0}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr int kRenderW  = 1280;
constexpr int kRenderH  = 720;
constexpr int kFrames   = 60;
constexpr float kExposure = 0.25f;   // sharc_present.frag と同じ Reinhard+gamma

Vec3 tonemap(Vec3 hdr, float exposure) {
    Vec3 v{std::max(hdr.x * exposure, 0.0f), std::max(hdr.y * exposure, 0.0f),
          std::max(hdr.z * exposure, 0.0f)};
    v = {v.x / (1.0f + v.x), v.y / (1.0f + v.y), v.z / (1.0f + v.z)};
    const float invGamma = 1.0f / 2.2f;
    return {std::pow(v.x, invGamma), std::pow(v.y, invGamma), std::pow(v.z, invGamma)};
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: pictor_sharc_dx12_test <bistro.obj>\n");
        return 1;
    }
    const std::string obj_path = argv[1];

    // ── シーンロード (Vulkan main.cpp の Hero/Bistro 経路と同一手順) ──
    sharc_demo::PlyMesh mesh = sharc_demo::load_obj(obj_path);
    if (mesh.empty()) {
        std::fprintf(stderr, "[init] FATAL: mesh load failed: %s\n", obj_path.c_str());
        return 1;
    }
    float extent = 0.0f;
    for (int a = 0; a < 3; ++a) {
        extent = std::max(extent, mesh.bounds_max[a] - mesh.bounds_min[a]);
    }
    if (extent > 1000.0f) {
        sharc_demo::scale_mesh(mesh, 0.01f);
        extent *= 0.01f;
        std::fprintf(stderr, "[scene] assumed cm units, scaled x0.01\n");
    }
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (const auto& p : mesh.positions) { cx += p[0]; cy += p[1]; cz += p[2]; }
    const double inv_n = 1.0 / static_cast<double>(mesh.positions.size());
    const Vec3 target{static_cast<float>(cx * inv_n), static_cast<float>(cy * inv_n),
                      static_cast<float>(cz * inv_n)};
    std::fprintf(stderr,
                "[scene] bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f) centroid "
                "(%.1f %.1f %.1f) extent %.1f\n",
                mesh.bounds_min[0], mesh.bounds_min[1], mesh.bounds_min[2],
                mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2],
                target.x, target.y, target.z, extent);
    const float ray_far = 400.0f;

    sharc_demo::MeshBvh bvh;
    bvh.build(mesh);

    // ── アルベドテクスチャ配列 + マテリアルへのレイヤ割当 ──
    const auto atlas = sharc_demo::build_texture_atlas(mesh.materials, 512, 192);
    std::vector<SharcMaterialGpu> gpu_mats(mesh.materials.size());
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        const auto& pm = mesh.materials[i];
        SharcMaterialGpu m{};
        m.albedo[0] = pm.albedo[0]; m.albedo[1] = pm.albedo[1]; m.albedo[2] = pm.albedo[2];
        m.roughness = pm.roughness;
        m.mfp       = pm.mfp;
        const auto it = pm.texture.empty() ? atlas.layer_of.end()
                                           : atlas.layer_of.find(pm.texture);
        m.atlas_layer_plus1 =
            (it != atlas.layer_of.end()) ? static_cast<float>(it->second + 1) : 0.0f;
        gpu_mats[i] = m;
    }

    // ── BVH ノード / 三角形 (葉順) を GPU レイアウトへ転写 ──
    const auto& nodes = bvh.nodes();
    const auto& order = bvh.tri_order();
    std::vector<SharcBvhNodeGpu> gpu_nodes(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::memcpy(gpu_nodes[i].bmin, nodes[i].bmin, sizeof(float) * 3);
        std::memcpy(gpu_nodes[i].bmax, nodes[i].bmax, sizeof(float) * 3);
        gpu_nodes[i].left  = nodes[i].left;
        gpu_nodes[i].count = nodes[i].count;
    }
    std::vector<SharcTriGpu> gpu_tris(order.size());
    std::vector<uint32_t> gpu_tri_mats(order.size());
    const bool has_uv = !mesh.tri_corner_uvs.empty();
    for (size_t i = 0; i < order.size(); ++i) {
        const auto& tri = mesh.triangles[order[i]];
        auto& g = gpu_tris[i];
        for (int c = 0; c < 3; ++c) {
            g.v0[c] = mesh.positions[tri[0]][c];
            g.v1[c] = mesh.positions[tri[1]][c];
            g.v2[c] = mesh.positions[tri[2]][c];
        }
        const auto& n0 = mesh.normals[tri[0]];
        const auto& n1 = mesh.normals[tri[1]];
        const auto& n2 = mesh.normals[tri[2]];
        g.n0 = sharc_oct32_encode(n0[0], n0[1], n0[2]);
        g.n1 = sharc_oct32_encode(n1[0], n1[1], n1[2]);
        g.n2 = sharc_oct32_encode(n2[0], n2[1], n2[2]);
        if (has_uv) {
            const auto& uv = mesh.tri_corner_uvs[order[i]];
            g.uv0[0] = uv[0][0]; g.uv0[1] = uv[0][1];
            g.uv1[0] = uv[1][0]; g.uv1[1] = uv[1][1];
            g.uv2[0] = uv[2][0]; g.uv2[1] = uv[2][1];
        }
        g.pad0 = 0; g.pad1 = 0;
        gpu_tri_mats[i] = mesh.tri_material[order[i]];
    }

    // ── D3D12 executor 初期化 ──
    SharcConfig cfg;
    cfg.max_rays   = kRenderW * kRenderH;
    cfg.table_size = 1u << 18;   // Bistro スケールは可視セル数が桁違い

    SharcDx12Executor sharc;
    if (!sharc.initialize(PICTOR_SHARC_HLSL_DIR, cfg)) {
        std::fprintf(stderr, "[init] FATAL: SHaRC DX12 executor init failed\n");
        return 1;
    }

    SharcSceneUpload up;
    up.nodes          = gpu_nodes.data();
    up.node_count     = static_cast<uint32_t>(gpu_nodes.size());
    up.tris           = gpu_tris.data();
    up.tri_count      = static_cast<uint32_t>(gpu_tris.size());
    up.tri_materials  = gpu_tri_mats.data();
    up.materials      = gpu_mats.data();
    up.material_count = static_cast<uint32_t>(gpu_mats.size());
    if (!atlas.empty()) {
        up.atlas_pixels = atlas.pixels.data();
        up.atlas_size   = atlas.size;
        up.atlas_layers = atlas.layers;
    }
    if (!sharc.upload_scene(up)) {
        std::fprintf(stderr, "[init] FATAL: GPU scene upload failed\n");
        return 1;
    }
    sharc.set_scene_floor(false, 0.0f);   // Bistro は実地面を持つ
    sharc.set_scene_far(ray_far);

    // ── 固定カメラ (Vulkan main.cpp のキー 'B' シーン俯瞰プリセットと同一:
    //    dist = 既定 30 * 1.5, pitch = 0.55 — street-level クレイレンダの
    //    確認に適した見下ろし視点) ──
    const float yaw = 0.35f, pitch = 0.55f, dist = 45.0f;
    const Vec3 eye{
        target.x + dist * std::cos(pitch) * std::sin(yaw),
        target.y + dist * std::sin(pitch),
        target.z + dist * std::cos(pitch) * std::cos(yaw)};
    const Vec3 fwd   = norm(target - eye);
    const Vec3 right = norm(cross(fwd, Vec3{0, 1, 0}));
    const Vec3 up_v  = cross(right, fwd);
    const float fov_scale = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = static_cast<float>(kRenderW) / static_cast<float>(kRenderH);
    std::fprintf(stderr, "[cam] eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f)\n",
                eye.x, eye.y, eye.z, target.x, target.y, target.z);

    // ── コマンドリスト (フレームループ用) ──
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
    if (FAILED(sharc.device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(sharc.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&cmd)))) {
        std::fprintf(stderr, "[init] FATAL: command list creation failed\n");
        return 1;
    }
    cmd->Close();

    std::printf("=== Pictor SHaRC DX12 Test - Bistro (%dx%d, %d frames) ===\n",
               kRenderW, kRenderH, kFrames);

    double total_ms = 0.0;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto t0 = std::chrono::steady_clock::now();

        sharc.begin_frame(float3{eye.x, eye.y, eye.z});
        sharc.set_camera(float3{fwd.x, fwd.y, fwd.z}, float3{right.x, right.y, right.z},
                         float3{up_v.x, up_v.y, up_v.z}, fov_scale, aspect,
                         static_cast<uint32_t>(kRenderW));

        // ── Bistro 夕暮れライティング (Vulkan main.cpp と同一配置) ──
        auto* lights = sharc.lights_mapped();
        uint32_t li = 0;
        lights[li++] = SharcLightGpu{
            {target.x + 90.0f, target.y + 45.0f, target.z - 60.0f, 1.0f},
            {1.0f, 0.55f, 0.3f, 7000.0f}};
        lights[li++] = SharcLightGpu{
            {target.x - 40.0f, target.y + 70.0f, target.z + 50.0f, 2.0f},
            {0.4f, 0.5f, 0.9f, 2500.0f}};
        for (int i = 0; i < 8; ++i) {
            const float along = (static_cast<float>(i) - 3.5f) * 9.0f;
            const float side  = (i & 1) ? 6.0f : -6.0f;
            lights[li++] = SharcLightGpu{
                {target.x + along, target.y - 1.0f, target.z + side, 0.2f},
                {1.0f, 0.72f, 0.42f, 120.0f}};
        }
        lights[li++] = SharcLightGpu{
            {target.x + 6.0f, target.y + 1.0f, target.z - 8.0f, 0.2f},
            {0.2f, 0.85f, 0.8f, 80.0f}};
        lights[li++] = SharcLightGpu{
            {target.x - 10.0f, target.y + 2.0f, target.z + 3.0f, 0.2f},
            {0.9f, 0.25f, 0.55f, 80.0f}};
        lights[li++] = SharcLightGpu{
            {target.x, target.y + 4.0f, target.z + 5.0f, 0.3f},
            {1.0f, 0.7f, 0.4f, 200.0f}};
        const uint32_t n_lights = li;

        sharc.set_counts(static_cast<uint32_t>(kRenderW * kRenderH), n_lights);
        sharc.record(cmd.Get());
        sharc.execute_and_wait(cmd.Get(), allocator.Get());

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;
        if ((frame + 1) % 15 == 0) {
            std::printf("[loop] frame %d: %u requested cells, %.1f ms\n", frame + 1,
                       sharc.request_count(), ms);
        }
    }

    // ── 出力 readback ──
    sharc.copy_output_to_readback(cmd.Get());
    sharc.execute_and_wait(cmd.Get(), allocator.Get());
    const auto* out = static_cast<const float*>(sharc.map_readback());
    if (!out) {
        std::fprintf(stderr, "[readback] FATAL: map failed\n");
        return 1;
    }

    const size_t n_pixels = static_cast<size_t>(kRenderW) * kRenderH;
    double luma_sum = 0.0;
    uint64_t nan_pixels = 0;
    uint64_t nan_components = 0;
    std::vector<uint8_t> ppm(n_pixels * 3);
    for (size_t i = 0; i < n_pixels; ++i) {
        const float r = out[i * 4 + 0];
        const float g = out[i * 4 + 1];
        const float b = out[i * 4 + 2];
        const float a = out[i * 4 + 3];
        bool any_nan = false;
        for (float v : {r, g, b, a}) {
            if (std::isnan(v)) { ++nan_components; any_nan = true; }
        }
        if (any_nan) ++nan_pixels;
        const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        if (!std::isnan(luma)) luma_sum += luma;

        const Vec3 ldr = tonemap(Vec3{r, g, b}, kExposure);
        const auto clamp8 = [](float v) {
            v = std::isnan(v) ? 0.0f : std::max(0.0f, std::min(1.0f, v));
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        };
        ppm[i * 3 + 0] = clamp8(ldr.x);
        ppm[i * 3 + 1] = clamp8(ldr.y);
        ppm[i * 3 + 2] = clamp8(ldr.z);
    }
    sharc.unmap_readback();

    const double avg_luma = luma_sum / static_cast<double>(n_pixels);
    std::printf("[result] avg_luminance=%.6f nan_pixels=%llu nan_components=%llu "
               "avg_frame_ms=%.2f\n",
               avg_luma, static_cast<unsigned long long>(nan_pixels),
               static_cast<unsigned long long>(nan_components),
               total_ms / kFrames);

    std::ofstream ppm_file("sharc_dx12_out.ppm", std::ios::binary);
    if (!ppm_file) {
        std::fprintf(stderr, "[output] FATAL: failed to open sharc_dx12_out.ppm\n");
        return 1;
    }
    ppm_file << "P6\n" << kRenderW << " " << kRenderH << "\n255\n";
    ppm_file.write(reinterpret_cast<const char*>(ppm.data()),
                   static_cast<std::streamsize>(ppm.size()));
    ppm_file.close();
    std::printf("[output] wrote sharc_dx12_out.ppm (%dx%d)\n", kRenderW, kRenderH);

    sharc.shutdown();
    std::printf("[exit] done (%d frames)\n", kFrames);
    return 0;
}
