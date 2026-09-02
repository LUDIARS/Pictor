#version 450

// Manga-style teardrop: flat sky-blue fill, dark outline, one white glint.
// The shape is a signed distance evaluated on the quad corner coordinates.

layout(location = 0) in vec2  fragCorner;
layout(location = 1) in float fragAge;

layout(location = 0) out vec4 outColor;

float teardrop(vec2 p) {
    // Round bottom centred at y=-0.3, tapering to a point at y=0.9.
    const vec2  bottom = vec2(0.0, -0.3);
    const float r = 0.55;
    if (p.y < bottom.y) return length(p - bottom) - r;
    float t = clamp((p.y - bottom.y) / 1.2, 0.0, 1.0);
    float halfWidth = r * (1.0 - t * t);
    return max(abs(p.x) - halfWidth, p.y - 0.9);
}

void main() {
    vec2 p = fragCorner;
    float d = teardrop(p);
    if (d > 0.0) discard;

    // Anti-aliased outline band just inside the silhouette.
    float aa = fwidth(d) * 1.5;
    float outline = 1.0 - smoothstep(-0.16 - aa, -0.16 + aa, d);

    vec3 fill    = vec3(0.60, 0.84, 1.00);
    vec3 shade   = vec3(0.38, 0.66, 0.96);
    vec3 edge    = vec3(0.12, 0.30, 0.62);
    // Simple two-tone: darker on the lower-right like a cel shadow.
    float tone = smoothstep(-0.1, 0.5, p.x * 0.6 - p.y * 0.4);
    vec3 color = mix(fill, shade, tone);
    // Glint: small ellipse upper-left.
    float glint = 1.0 - smoothstep(0.10, 0.16, length((p - vec2(-0.20, -0.32)) * vec2(1.0, 1.6)));
    color = mix(color, vec3(1.0), glint);
    color = mix(color, edge, outline);

    // Pop in near the eye, fade out at the far end of the line.
    float alpha = smoothstep(0.0, 0.10, fragAge) * (1.0 - smoothstep(0.80, 1.0, fragAge));
    outColor = vec4(color, alpha);
}
