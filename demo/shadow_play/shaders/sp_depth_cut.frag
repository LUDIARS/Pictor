#version 450
#extension GL_GOOGLE_include_directive : require

/// 影絵デモ — 切り絵シート用 depth-only pass。
/// シートは光を全部遮る黒紙として depth を書き、カットアウト (穴) だけ
/// discard して光を透過させる。ここで遮った光は shadow map 経由で
/// 「他の物へ一切影響しない」ことが保証される。

#include "sp_cutout.glsl"

layout(location = 0) in vec2 in_uv;

void main() {
    if (sp_cutout_hole(in_uv)) discard;
}
