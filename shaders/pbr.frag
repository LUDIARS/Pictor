// Pictor — PBR Fragment Shader (標準経路、 GI 無し)
// 本体は pbr_main.glsl (pbr_gi.frag と共有)。 既存ホストの descriptor
// layout (set 0/1/2 binding 0-1) はこの経路のまま変わらない。

#version 450
#extension GL_GOOGLE_include_directive : require

#include "pbr_main.glsl"
