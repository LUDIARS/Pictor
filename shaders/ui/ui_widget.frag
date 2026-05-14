// Pictor — UI Widget Fragment Shader
// mode に応じて 角丸矩形 / テクスチャ画像 / 9-slice を描く。
//   mode 0 = Rect (角丸、 純色)
//   mode 1 = Image (テクスチャ * tint)
//   mode 2 = NineSlice (9-slice UV リマップ後にサンプル)

#version 450

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec2 inLocalPx;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PC {
    vec4 rect;       // x, y, w, h
    vec4 color;      // rgba
    vec4 params;     // corner_radius, mode, opacity, _pad
    vec4 nine;       // 9-slice insets l, t, r, b (px)
    vec4 viewport;
};

// 角丸矩形の signed distance。
float rounded_box_sdf(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - half_size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

// 1 軸の 9-slice UV リマップ。 size = rect の該当軸長、 lo/hi = inset。
float nine_axis(float uv, float size, float lo, float hi) {
    float px = uv * size;
    if (px < lo)             return px;                       // 左/上の固定帯
    if (px > size - hi)      return px - (size - hi) + lo;     // 右/下の固定帯 (テクスチャ末尾基準は別途)
    // 中央帯: lo..(size-hi) を lo..(1-hi 相当) に伸縮 — ここでは簡易に
    // 中央を [lo, lo+ (mid)] でストレッチした値を返す。
    return lo + (px - lo);   // 呼び出し側で正規化する
}

void main() {
    float radius  = params.x;
    int   mode    = int(params.y + 0.5);
    float opacity = params.z;

    vec4 result;

    if (mode == 1) {
        // Image: テクスチャ * tint
        result = texture(tex, inUV) * color;
    } else if (mode == 2) {
        // NineSlice: 9-slice UV リマップ。 nine = l,t,r,b (px)。
        // テクスチャ・矩形ともに同じ inset で 3x3 に分割し、 各帯を対応付ける。
        vec2 sz = rect.zw;
        vec2 px = inUV * sz;
        // X
        float u;
        if (px.x < nine.x) {
            u = px.x / max(sz.x, 1.0);                    // 左帯 (素のまま近似)
        } else if (px.x > sz.x - nine.z) {
            u = 1.0 - (sz.x - px.x) / max(sz.x, 1.0);
        } else {
            float t = (px.x - nine.x) / max(sz.x - nine.x - nine.z, 1.0);
            u = mix(nine.x / max(sz.x, 1.0),
                    1.0 - nine.z / max(sz.x, 1.0), t);
        }
        // Y
        float v;
        if (px.y < nine.y) {
            v = px.y / max(sz.y, 1.0);
        } else if (px.y > sz.y - nine.w) {
            v = 1.0 - (sz.y - px.y) / max(sz.y, 1.0);
        } else {
            float t = (px.y - nine.y) / max(sz.y - nine.y - nine.w, 1.0);
            v = mix(nine.y / max(sz.y, 1.0),
                    1.0 - nine.w / max(sz.y, 1.0), t);
        }
        result = texture(tex, vec2(u, v)) * color;
    } else {
        // Rect: 角丸の SDF マスク。
        vec2 half_size = rect.zw * 0.5;
        vec2 p = inLocalPx - half_size;
        float d = rounded_box_sdf(p, half_size, min(radius, min(half_size.x, half_size.y)));
        float aa = fwidth(d) + 1e-4;
        float mask = 1.0 - smoothstep(-aa, aa, d);
        result = vec4(color.rgb, color.a * mask);
    }

    result.a *= opacity;
    if (result.a <= 0.0) discard;
    outColor = result;
}
