// Pictor — FXAA 3.11 (quality) fullscreen pass
// トーンマップ後の LDR カラーに対して走るスクリーンスペース AA。
// 入力 (binding 0) は grade pass の出力 (pp_ldr)。 grade が輝度を alpha に
// 書き込むため、 本シェーダの luma 参照はテクスチャ alpha 読みで済む。
//
// edge_threshold      : 相対輝度コントラスト閾値 (超えたエッジのみ AA)
// edge_threshold_min  : 暗部の絶対輝度閾値 (ノイズ AA 抑止)
// subpix_quality      : サブピクセル AA のブレンド強度 0..1
// 無効化は edge_threshold を巨大値にする (全ピクセル素通し = 恒等縮退)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ldrColor;

layout(push_constant) uniform FxaaPC {
    float texel_x;
    float texel_y;
    float edge_threshold;
    float edge_threshold_min;
    float subpix_quality;
    float _pad0;
    float _pad1;
    float _pad2;
};

float lumaAt(vec2 uv) { return texture(ldrColor, uv).a; }

// エッジ端探索の各ステップ幅 (FXAA 3.11 quality preset 相当)。
const float STEP_SIZES[8] = float[](1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0, 12.0);

void main() {
    vec2 texel = vec2(texel_x, texel_y);

    vec3  rgbM  = texture(ldrColor, inUV).rgb;
    float lumaM = lumaAt(inUV);
    float lumaN = lumaAt(inUV + vec2( 0.0, -texel.y));
    float lumaS = lumaAt(inUV + vec2( 0.0,  texel.y));
    float lumaW = lumaAt(inUV + vec2(-texel.x, 0.0));
    float lumaE = lumaAt(inUV + vec2( texel.x, 0.0));

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float range   = lumaMax - lumaMin;

    // コントラストが閾値未満 → AA 不要 (素通し)。
    if (range < max(edge_threshold_min, lumaMax * edge_threshold)) {
        outColor = vec4(rgbM, lumaM);
        return;
    }

    float lumaNW = lumaAt(inUV + vec2(-texel.x, -texel.y));
    float lumaNE = lumaAt(inUV + vec2( texel.x, -texel.y));
    float lumaSW = lumaAt(inUV + vec2(-texel.x,  texel.y));
    float lumaSE = lumaAt(inUV + vec2( texel.x,  texel.y));

    // エッジ方向判定 (水平 or 垂直)。
    float edgeH = abs(-2.0 * lumaW + lumaNW + lumaSW)
                + abs(-2.0 * lumaM + lumaN  + lumaS ) * 2.0
                + abs(-2.0 * lumaE + lumaNE + lumaSE);
    float edgeV = abs(-2.0 * lumaN + lumaNW + lumaNE)
                + abs(-2.0 * lumaM + lumaW  + lumaE ) * 2.0
                + abs(-2.0 * lumaS + lumaSW + lumaSE);
    bool isHorizontal = edgeH >= edgeV;

    // エッジの勾配が強い側 (エッジを挟む隣ピクセル) を選ぶ。
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float grad1 = luma1 - lumaM;
    float grad2 = luma2 - lumaM;
    bool  is1Steepest = abs(grad1) >= abs(grad2);
    float gradScaled  = 0.25 * max(abs(grad1), abs(grad2));

    float stepLen  = isHorizontal ? texel.y : texel.x;
    float lumaLocal;
    if (is1Steepest) {
        stepLen  = -stepLen;
        lumaLocal = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocal = 0.5 * (luma2 + lumaM);
    }

    // エッジに直交する半ピクセル分ずらした位置からエッジ端を両方向へ探索。
    vec2 currentUV = inUV;
    if (isHorizontal) currentUV.y += stepLen * 0.5;
    else              currentUV.x += stepLen * 0.5;

    vec2 offset = isHorizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uv1 = currentUV;
    vec2 uv2 = currentUV;
    float lumaEnd1 = 0.0;
    float lumaEnd2 = 0.0;
    bool reached1 = false;
    bool reached2 = false;

    for (int i = 0; i < 8; ++i) {
        if (!reached1) {
            uv1 -= offset * STEP_SIZES[i];
            lumaEnd1 = lumaAt(uv1) - lumaLocal;
            reached1 = abs(lumaEnd1) >= gradScaled;
        }
        if (!reached2) {
            uv2 += offset * STEP_SIZES[i];
            lumaEnd2 = lumaAt(uv2) - lumaLocal;
            reached2 = abs(lumaEnd2) >= gradScaled;
        }
        if (reached1 && reached2) break;
    }

    float dist1 = isHorizontal ? (inUV.x - uv1.x) : (inUV.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - inUV.x) : (uv2.y - inUV.y);
    float distMin  = min(dist1, dist2);
    float edgeLen  = dist1 + dist2;
    float pixelOff = -distMin / max(edgeLen, 1e-6) + 0.5;

    // エッジ端の輝度変化が中心と同符号なら段差の外側 — オフセットしない。
    bool isLumaMSmaller = lumaM < lumaLocal;
    bool correctVariation = ((dist1 < dist2 ? lumaEnd1 : lumaEnd2) < 0.0)
                            != isLumaMSmaller;
    float finalOff = correctVariation ? pixelOff : 0.0;

    // サブピクセル AA: 3x3 平均輝度との差からローパス量を求める。
    float lumaAvg = (2.0 * (lumaN + lumaS + lumaW + lumaE)
                     + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subpix  = clamp(abs(lumaAvg - lumaM) / max(range, 1e-6), 0.0, 1.0);
    subpix = smoothstep(0.0, 1.0, subpix);
    subpix = subpix * subpix * subpix_quality;

    finalOff = max(finalOff, subpix);

    vec2 finalUV = inUV;
    if (isHorizontal) finalUV.y += finalOff * stepLen;
    else              finalUV.x += finalOff * stepLen;

    vec4 result = texture(ldrColor, finalUV);
    outColor = vec4(result.rgb, result.a);
}
