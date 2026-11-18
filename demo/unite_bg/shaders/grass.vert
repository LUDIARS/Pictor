#version 450
#extension GL_GOOGLE_include_directive : require
// =============================================================================
// 草の頂点シェーダー: メッシュは持たず gl_VertexIndex から板ポリの草を作る。
//   UV0.x = 左右 (-1..1)  → 法線を左右に振って板ポリに丸みを持たせる
//   UV0.y = 根元 0 → 先端 1 → 風の変位・ねじれ・踏み分け・AO・色 の重み
//   p0: x=Fade Start y=Fade End z=LOD0 Dist w=Twist
//   p1: x=Weak Wind TimeScale y=Weak Sway z=Wind Angle(deg) w=Direction Lean
//   p2: x=Trample Radius y=Trample Strength z=Roundness w=AO
//   p3: x=Direction TimeScale
//   objs[2] = 踏み分けるプレイヤーの位置
// =============================================================================
#include "common.glsl"
#include "grass_common.glsl"

layout(push_constant) uniform PC { uint lod; uint capacity; } pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec3 v_tint;

void main() {
    uint idx = g_visible[pc.lod * pc.capacity + uint(gl_InstanceIndex)];
    GrassInstance g = g_inst[idx];

    int segs   = pc.lod == 0u ? LOD0_SEGS : LOD1_SEGS;
    int vpb    = (segs * 2 + 1) * 3;                 // 1 枚あたりの頂点数
    int blade  = gl_VertexIndex / vpb;
    int local  = gl_VertexIndex % vpb;
    int tri    = local / 3, corner = local % 3;

    // 三角形の角 → (段, 左右)
    float row, side;
    if (tri < segs * 2) {
        int q = tri / 2;
        bool second = (tri & 1) == 1;
        ivec2 c0 = second ? ivec2(q + 1, -1) : ivec2(q, -1);
        ivec2 c1 = second ? ivec2(q, 1)      : ivec2(q, 1);
        ivec2 c2 = second ? ivec2(q + 1, 1)  : ivec2(q + 1, -1);
        ivec2 c = corner == 0 ? c0 : corner == 1 ? c1 : c2;
        row = float(c.x); side = float(c.y);
    } else {
        row = corner == 2 ? float(segs) + 1.0 : float(segs);
        side = corner == 0 ? -1.0 : corner == 1 ? 1.0 : 0.0;
    }
    float h01 = min(row / float(segs + 1), 1.0);     // UV0.y
    vec2 uv0 = vec2(side, h01);

    // 株の中の 1 枚ごとのばらつき
    float hb = hash12(vec2(float(idx), float(blade)));
    float hb2 = hash12(vec2(float(blade) * 3.1, float(idx) * 0.7));
    float ang = g.params.x + float(blade) * 2.4 + hb * 0.8;
    vec2 off = vec2(cos(ang * 1.7), sin(ang * 1.3)) * 0.12 * hb2;
    float height = g.params.w * mix(0.7, 1.15, hb);
    float width  = (g.params.y > 0.5 ? 0.09 : 0.06) * g.pos_scale.w;

    vec3 root = g.pos_scale.xyz + vec3(off.x, 0.0, off.y);
    float dist = length(root - F.cam_pos.xyz);
    float fade = 1.0 - smoothstep(F.p[0].x, F.p[0].y, dist);   // 遠いほど縮めて消す
    height *= fade;

    // ねじれ: 先端ほど向きが回る
    float twist = ang + F.p[0].w * h01 * 2.0;
    vec3 right = vec3(cos(twist), 0.0, sin(twist));
    vec3 facing = vec3(-right.z, 0.0, right.x);

    vec3 pos = root + right * side * width * (1.0 - h01 * 0.85) + vec3(0.0, h01 * height, 0.0);
    pos += facing * h01 * h01 * height * 0.25 * (hb - 0.3);           // 自然な反り

    // ---- 風: UV0.y の 2 乗で、根元は動かさず先端ほど大きく ----
    float t = F.cam_pos.w;
    float w2 = h01 * h01;
    vec2 wn = vec2(value_noise2(root.xz * 0.15 + vec2(t * F.p[1].x, 0.0)),
                   value_noise2(root.xz * 0.15 + vec2(0.0, t * F.p[1].x) + 7.0)) - 0.5;
    vec2 disp = wn * 2.0 * F.p[1].y;                                   // 弱い風 (揺れ)
    float wa = radians(F.p[1].z);
    float gust = 0.6 + 0.4 * sin(t * F.p[3].x * 6.0 + dot(root.xz, vec2(cos(wa), sin(wa))) * 0.2);
    disp += vec2(cos(wa), sin(wa)) * F.p[1].w * gust;                  // 強い風 (向き)

    // ---- 踏み分け: プレイヤーから押しのける ----
    if (flag(3)) {
        vec2 away = root.xz - F.objs[2].xz;
        float d = length(away);
        float push = (1.0 - smoothstep(0.0, F.p[2].x, d)) * F.p[2].y;
        disp += (d > 1e-3 ? away / d : vec2(0.0)) * push * 0.8;
        pos.y = root.y + (pos.y - root.y) * (1.0 - push * 0.6 * h01);
    }
    pos.xz += disp * w2 * height;

    // 法線: 板ポリの向きを左右に振って丸みを出す
    vec3 n = facing;
    if (flag(2)) n = normalize(facing + right * side * F.p[2].z);
    n = normalize(n + vec3(0.0, 0.3, 0.0));

    v_world = pos;
    v_normal = n;
    v_uv0 = uv0;
    v_tint = vec3(g.params.z, g.params.y, float(idx));
    gl_Position = F.view_proj * vec4(pos, 1.0);
}
