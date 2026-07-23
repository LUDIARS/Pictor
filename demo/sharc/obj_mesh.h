#pragma once

/// OBJ メッシュ読み込み (SHaRC デモローカル)。
///
/// Bistro (McGuire 版、 ~300MB テキスト / 284 万面) を想定した
/// 高速手書きパーサ。 対応: v / f (多角形 fan 分割、 v/vt/vn・負 index)、
/// mtllib + usemtl (Kd → albedo、 Ns → roughness 近似)。
/// テクスチャ (map_Kd) は未対応 — クレイレンダ用途。
///
/// SRP: OBJ/MTL の構文解析のみ。 メッシュ後処理は ply_mesh.h の
/// finalize_mesh、 空間構造は mesh_bvh.h。

#include "ply_mesh.h"

#include <string>

namespace sharc_demo {

/// OBJ を読み込み、 マテリアル表 + per-triangle マテリアル index 付きの
/// メッシュを返す。 失敗時は empty() (理由は stderr へ)。
PlyMesh load_obj(const std::string& path);

}  // namespace sharc_demo
