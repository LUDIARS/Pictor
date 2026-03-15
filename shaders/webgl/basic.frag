#version 300 es
precision highp float;

in vec3 v_world_normal;
in vec3 v_world_pos;

uniform vec4 u_color;

out vec4 frag_color;

const vec3 LIGHT_DIR   = normalize(vec3(0.5, 1.0, 0.3));
const vec3 LIGHT_COLOR = vec3(1.0, 0.98, 0.95);
const vec3 AMBIENT     = vec3(0.15, 0.17, 0.22);

void main() {
    vec3 N = normalize(v_world_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);
    float half_lambert = NdotL * 0.5 + 0.5;

    vec3 diffuse = u_color.rgb * LIGHT_COLOR * half_lambert;
    vec3 color   = AMBIENT * u_color.rgb + diffuse;

    frag_color = vec4(color, u_color.a);
}
