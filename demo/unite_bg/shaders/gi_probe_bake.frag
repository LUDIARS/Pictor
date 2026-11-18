#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// SkyOcclusion のベイク: 各プローブから全方向へレイを飛ばし、
// 「空が見える方向」だけを L1 の球面調和 (SH) 4 係数に畳み込む。
// 焼くのは "遮蔽の量" だけ。空の色は焼かない (実行時に差し替える)。
// テクスチャ: 幅 = X 方向のプローブ数, 高さ = Y 数 x Z 数
// 出力: (L0, L1x, L1y, L1z)
// =============================================================================
#include "gi_scene.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_sh;

void main() {
    ivec2 tc = ivec2(gl_FragCoord.xy);
    ivec3 idx = ivec3(tc.x, tc.y % PROBE_COUNT.y, tc.y / PROBE_COUNT.y);
    vec3 pos = PROBE_ORIGIN + vec3(idx) * PROBE_SPACING;

    const int N = 128;
    vec4 sh = vec4(0.0);
    for (int i = 0; i < N; ++i) {
        vec3 d = fibonacci_dir(i, N);
        int id;
        // 動く物体はベイクに入らない (dynamic = false)
        float t = gi_march(pos, d, 60.0, false, id);
        float vis = (t < 0.0 && d.y > 0.0) ? 1.0 : 0.0;
        sh += vis * vec4(0.282095, 0.488603 * d.x, 0.488603 * d.y, 0.488603 * d.z);
    }
    o_sh = sh * (4.0 * PI / float(N));
}
