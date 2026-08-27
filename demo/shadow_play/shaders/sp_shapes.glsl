/// Shadow-play silhouette cutouts. UV uses generate_screen_quad's convention:
/// uv.y = 0 is the upper edge, so every shape first converts to y-up coordinates.

bool sp_shape_edge_clear(vec2 uv) {
    return uv.x > 0.05 && uv.x < 0.95 && uv.y > 0.05 && uv.y < 0.95;
}

bool sp_shape_ellipse(vec2 p, vec2 center, vec2 radius) {
    return dot((p - center) / radius, (p - center) / radius) < 1.0;
}

bool sp_shape_band(vec2 p, vec2 a, vec2 b, float half_width) {
    vec2 ab = b - a;
    float h = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - (a + ab * h)) < half_width;
}

bool sp_trunk_solid(vec2 uv) {
    if (!sp_shape_edge_clear(uv)) return false;
    vec2 p = vec2(uv.x, 1.0 - uv.y);
    float width = mix(0.34, 0.16, clamp((p.y - 0.14) / 0.72, 0.0, 1.0));
    float edge_wobble = sin(p.y * 7.0 + 0.5) * 0.006
                      + sin(p.y * 3.5 + 1.2) * 0.002;
    bool trunk = abs(p.x - 0.50) < width + edge_wobble;
    bool left_branch = sp_shape_band(p, vec2(0.48, 0.68), vec2(0.20, 0.91), 0.042);
    bool right_branch = sp_shape_band(p, vec2(0.53, 0.66), vec2(0.79, 0.88), 0.044);
    bool high_branch = sp_shape_band(p, vec2(0.49, 0.77), vec2(0.43, 0.94), 0.033);
    return trunk || left_branch || right_branch || high_branch;
}

bool sp_figure_solid(vec2 uv) {
    if (!sp_shape_edge_clear(uv)) return false;
    vec2 p = vec2(uv.x, 1.0 - uv.y);
    bool head = sp_shape_ellipse(p, vec2(0.42, 0.73), vec2(0.075, 0.070));
    bool body = sp_shape_ellipse(p, vec2(0.43, 0.55), vec2(0.095, 0.165));
    bool cello = sp_shape_ellipse(p, vec2(0.58, 0.42), vec2(0.120, 0.220));
    bool neck = sp_shape_band(p, vec2(0.61, 0.57), vec2(0.72, 0.80), 0.027);
    bool bow_arm = sp_shape_band(p, vec2(0.45, 0.60), vec2(0.66, 0.54), 0.032);
    bool chair_and_legs = p.y > 0.17 && p.y < 0.30 && abs(p.x - 0.50) < mix(0.17, 0.09, (p.y - 0.17) / 0.13);
    return head || body || cello || neck || bow_arm || chair_and_legs;
}
