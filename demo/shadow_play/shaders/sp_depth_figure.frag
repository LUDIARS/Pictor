#version 450
#extension GL_GOOGLE_include_directive : require

#include "sp_shapes.glsl"

layout(location = 0) in vec2 in_uv;

void main() {
    if (!sp_figure_solid(in_uv)) discard;
}
