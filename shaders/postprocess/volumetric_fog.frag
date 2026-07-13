// Pictor — Volumetric Fog (解析的 height fog + 太陽前方散乱) fullscreen pass
// 指数 height fog の視線積分を閉形式で解き、 Henyey-Greenstein 位相の
// 太陽 in-scatter を乗せる。 レイマーチ / シャドウボリューム評価は行わない
// カジュアル向け近似 (phase 3 — spec/feature/postprocess-effects-design.md)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = シーン深度。
// valid = 0 (カメラ未設定) または density 0 で素通し。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform FogPC {
    vec3  cam_pos;    float density;
    vec3  cam_fwd;    float height_falloff;
    vec3  cam_right;  float base_height;
    vec3  cam_up;     float start_dist;
    vec3  sun_dir;    float phase_g;
    vec3  sun_color;  float sun_scatter;
    vec3  fog_color;  float near_plane;
    float far_plane;  float tan_x; float tan_y;
    uint  valid;
};

float linearizeDepth(float d) {
    return near_plane * far_plane
         / max(far_plane - d * (far_plane - near_plane), 1e-6);
}

// 指数 height fog の光学的深さ (閉形式)。
//   σ(h) = density * exp(-height_falloff * (h - base_height))
//   ∫₀ᵗ σ(o.y + d.y s) ds = density * exp(-k(o.y-h0)) * (1-exp(-k d.y t)) / (k d.y)
float fogOpticalDepth(vec3 origin, vec3 dir, float t) {
    float k = height_falloff;
    float base = density * exp(-k * (origin.y - base_height));
    float kd = k * dir.y;
    if (abs(kd) < 1e-4) return base * t;             // 水平レイは一様
    return base * (1.0 - exp(-kd * t)) / kd;
}

void main() {
    vec3 scene = texture(sceneColor, inUV).rgb;

    if (valid == 0u || density <= 0.0) {
        outColor = vec4(scene, 1.0);
        return;
    }

    float depth = texture(sceneDepth, inUV).r;
    float viewDist = linearizeDepth(depth);

    // ピクセルの world 視線方向 (カメラ基底 + tan(fov/2))。
    vec2 ndc = inUV * 2.0 - 1.0;
    vec3 rayDir = normalize(cam_fwd
                          + cam_right * (ndc.x * tan_x)
                          + cam_up    * (-ndc.y * tan_y));   // Vulkan Y down

    // view-Z 距離 → レイ距離 (forward 成分で割る)。
    float rayLen = viewDist / max(dot(rayDir, cam_fwd), 1e-3);
    rayLen = max(rayLen - start_dist, 0.0);
    vec3 rayStart = cam_pos + rayDir * start_dist;

    float optical = fogOpticalDepth(rayStart, rayDir, rayLen);
    float transmittance = exp(-max(optical, 0.0));

    // 太陽の前方散乱 (Henyey-Greenstein 位相)。
    float cosT = dot(rayDir, -normalize(sun_dir));
    float g = clamp(phase_g, -0.99, 0.99);
    float phase = (1.0 - g * g)
                / (4.0 * 3.14159265 * pow(1.0 + g * g - 2.0 * g * cosT, 1.5));
    vec3 inscatter = fog_color + sun_color * (phase * sun_scatter);

    vec3 result = mix(inscatter, scene, transmittance);
    outColor = vec4(result, 1.0);
}
