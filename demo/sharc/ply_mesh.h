#pragma once

/// PLY メッシュ読み込み (SHaRC デモローカル)。
///
/// Stanford 3D Scanning Repository の vrip 再構成 PLY
/// (ascii / binary_little_endian、 位置 + 面リスト、 余分な property は
/// 読み飛ばし) を対象にした最小ローダ。 スムーズ法線 (面積重み) を計算し、
/// D2 逆光透過デモが SSS 評価に使う。
///
/// SRP: ファイル読み込みと法線計算のみ。 空間構造 (BVH) は mesh_bvh.h。

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sharc_demo {

/// メッシュのマテリアル (OBJ/MTL 由来)。
struct PlyMaterial {
    /// 自己発光強度 (0 = 非発光)。 街灯ガラス / 電飾玉 / 提灯など、
    /// 「光ってる見た目」 の表現用 (ライト配置とは独立)。
    float emissive = 0.0f;
    std::array<float, 3> albedo{0.75f, 0.75f, 0.75f};
    float roughness = 0.6f;
    float mfp = 0.0f;   ///< SSS 平均自由行程 (0 = 不透明)。 葉など半透明素材用
    std::string texture; ///< map_Kd の解決済みパス (空 = テクスチャなし)
};

/// OBJ の発光体グループ (街灯ガラス / 提灯) の抽出結果。
/// kind: 0 = 街灯 (StreetLight_Glass), 1 = 提灯 (Lantern / StringLights の玉)。
struct EmissiveGroup {
    std::array<float, 3> center{};
    int kind = 0;
};

struct PlyMesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;    ///< 頂点スムーズ法線
    std::vector<std::array<uint32_t, 3>> triangles;

    /// OBJ の街灯/提灯グループ中心 (ライト自動配置用、 OBJ 経路のみ)。
    /// scale_mesh / 平行移動の適用は呼び出し側の責務。
    std::vector<EmissiveGroup> emissive_groups;

    /// materials が空 = 呼び出し側の単一マテリアル (PLY 経路)。
    /// 非空なら tri_material が triangles と同数で対応する。
    std::vector<PlyMaterial> materials;
    std::vector<uint32_t>    tri_material;

    /// 三角形コーナーごとの UV (OBJ の vt)。 空 = UV なし (PLY 等)。
    /// GPU 側は texture2DArray + REPEAT サンプラでタイルする。
    std::vector<std::array<std::array<float, 2>, 3>> tri_corner_uvs;

    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};

    bool empty() const { return triangles.empty(); }
};

/// PLY を読み込み、 スムーズ法線と AABB を計算して返す。
/// 失敗時は empty() な PlyMesh (理由は stderr へ)。
PlyMesh load_ply(const std::string& path);

/// 位置 + 三角形が入ったメッシュのスムーズ法線と AABB を計算する
/// (各ローダの後処理共通部)。
void finalize_mesh(PlyMesh& mesh);

/// メッシュを一様スケール + 平行移動する (デモのワールド配置用)。
/// target_extent = 最大軸の長さ (m)、 底面 y=0 / x=z=0 中心に置く。
void fit_mesh(PlyMesh& mesh, float target_extent);

/// スケールせず平行移動のみ (実寸シーン用): 底面 y=0 / x=z 重心を原点へ。
void recenter_mesh(PlyMesh& mesh);

/// 一様スケールのみ (単位系変換用、 例: cm → m は 0.01)。
void scale_mesh(PlyMesh& mesh, float scale);

}  // namespace sharc_demo
