// Pictor — Screen-Space Reflections (深度再構築版) fullscreen pass
// 法線バッファを要求しない: view 空間位置を深度から再構築し、 法線は
// 深度勾配 (dFdx/dFdy) の面法線で代用する。 エッジ品質は G-buffer 法線に
// 劣るが、 床・壁など平面の反射がカジュアル品質で出る (spec §3.4 の (b))。
//
// 前提: 標準的な透視投影 (Vulkan z ∈ [0,1]、 非 reversed-Z)。
// proj_xx / proj_yy / near / far はホストがカメラと一致させて供給する。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = シーン深度。
// intensity 0 で素通し (恒等縮退)。 ヒットしないレイは反射無し (fade out)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform SsrPC {
    float proj_xx;      // 投影行列 [0][0]
    float proj_yy;      // 投影行列 [1][1]
    float near_plane;
    float far_plane;
    float intensity;
    float stride_px;    // 1 ステップのスクリーン距離 (px)
    float thickness;    // ヒット判定の view-space 厚み
    uint  max_steps;
    float texel_x;
    float texel_y;
    float _pad0;
    float _pad1;
};

// 深度 (0..1) → view-space Z (正の距離)。 標準透視投影の逆算。
float linearizeDepth(float d) {
    return near_plane * far_plane
         / max(far_plane - d * (far_plane - near_plane), 1e-6);
}

// UV + 深度 → view 空間位置 (右手系、 +Z が奥)。
vec3 reconstructViewPos(vec2 uv, float d) {
    float z = linearizeDepth(d);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / proj_xx, ndc.y * z / proj_yy, z);
}

// view 空間位置 → UV。
vec2 projectToUV(vec3 vpos) {
    vec2 ndc = vec2(vpos.x * proj_xx, vpos.y * proj_yy) / max(vpos.z, 1e-6);
    return ndc * 0.5 + 0.5;
}

void main() {
    vec3 color = texture(sceneColor, inUV).rgb;

    float depth = texture(sceneDepth, inUV).r;
    if (intensity <= 0.0 || depth >= 1.0) {   // 素通し / 空
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 vpos = reconstructViewPos(inUV, depth);

    // 深度勾配から面法線を復元 (fullscreen pass では dFdx が UV 方向の
    // view 位置差になる)。
    vec3 dx = dFdx(vpos);
    vec3 dy = dFdy(vpos);
    vec3 normal = normalize(cross(dy, dx));
    // カメラへ向く側へ揃える (視線は +Z 奥向き → 法線は -Z 成分を持つはず)。
    if (normal.z > 0.0) normal = -normal;

    // 反射レイ (view 空間)。 視線方向 = normalize(vpos)。
    vec3 viewDir = normalize(vpos);
    vec3 rayDir  = normalize(reflect(viewDir, normal));

    // 手前 (カメラ側) へ向かうレイは画面内でヒットしにくい — 減衰させる。
    float towardCamera = clamp(rayDir.z * 4.0, 0.0, 1.0);
    if (towardCamera <= 0.0) {
        outColor = vec4(color, 1.0);
        return;
    }

    // スクリーンスペース レイマーチ。 stride は view 距離に比例させる
    // (画面上でおおよそ stride_px 進む)。
    float stepLen = stride_px * texel_x * vpos.z / proj_xx;
    vec3  pos = vpos;
    vec2  hitUV = vec2(-1.0);
    uint  steps = min(max_steps, 64u);

    for (uint i = 0u; i < steps; ++i) {
        pos += rayDir * stepLen;
        if (pos.z <= near_plane) break;
        vec2 uv = projectToUV(pos);
        if (uv != clamp(uv, vec2(0.0), vec2(1.0))) break;

        float sceneZ = linearizeDepth(texture(sceneDepth, uv).r);
        float diff   = pos.z - sceneZ;
        if (diff > 0.0 && diff < thickness) {
            hitUV = uv;
            break;
        }
        // 大きく潜ったら打ち切り (裏面貫通の誤ヒット防止)。
        if (diff > thickness * 8.0) break;
    }

    if (hitUV.x < 0.0) {
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 reflected = texture(sceneColor, hitUV).rgb;

    // フレネル近似 (Schlick) + 画面端フェード。
    float fresnel = pow(1.0 - clamp(dot(-viewDir, normal), 0.0, 1.0), 5.0);
    fresnel = mix(0.04, 1.0, fresnel);
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float edgeFade = clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0);

    float blend = intensity * fresnel * edgeFade * towardCamera;
    outColor = vec4(mix(color, reflected, clamp(blend, 0.0, 1.0)), 1.0);
}
