// Pictor — PBR Fragment Shader (GI 経路、 opt-in)
// pbr.frag + probe grid の間接光 (gi.glsl)。 追加バインディング:
//   set 2, binding 2 = GIProbeParams UBO   (GIGpuExecutor::params_buffer())
//   set 2, binding 3 = ProbeIrradiance SSBO (GIGpuExecutor::probe_sh_buffer())
// 移行手順は spec/setup/integration.md「GI 経路への移行」参照。

#version 450
#extension GL_GOOGLE_include_directive : require

#define PICTOR_GI 1
#include "pbr_main.glsl"
