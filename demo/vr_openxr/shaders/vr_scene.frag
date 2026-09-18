#version 450

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    // @implements SPEC-PC-XR-DEMO
    // 固定の光源 1 つ + 環境光。 見る向きに依らないので再投影しても破綻しない。
    vec3  n       = normalize(v_normal);
    float diffuse = max(dot(n, normalize(vec3(0.4, 0.8, 0.45))), 0.0);
    out_color = vec4(v_color.rgb * (0.25 + 0.75 * diffuse), 1.0);
}
