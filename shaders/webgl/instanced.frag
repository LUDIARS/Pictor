#version 300 es
precision highp float;

in vec3 v_world_normal;
in vec3 v_world_pos;
in vec4 v_color;

out vec4 frag_color;

const vec3 LIGHT_DIR   = normalize(vec3(0.5, 1.0, 0.3));
const vec3 LIGHT_COLOR = vec3(1.0, 0.98, 0.95);
const vec3 AMBIENT     = vec3(0.15, 0.17, 0.22);

void main() {
    vec3 N = normalize(v_world_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);

    // Half-Lambert for softer shading
    float half_lambert = NdotL * 0.5 + 0.5;

    vec3 diffuse = v_color.rgb * LIGHT_COLOR * half_lambert;
    vec3 color   = AMBIENT * v_color.rgb + diffuse;

    // Rim light
    vec3 view_dir = normalize(-v_world_pos);
    float rim = 1.0 - max(dot(N, view_dir), 0.0);
    rim = smoothstep(0.6, 1.0, rim) * 0.3;
    color += vec3(rim);

    frag_color = vec4(color, v_color.a);
}
