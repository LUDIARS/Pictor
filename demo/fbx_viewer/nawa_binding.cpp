#include "nawa_binding.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pictor_fbx_viewer {

using pictor::float3;

namespace {

constexpr float kPi    = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 add(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 sub(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 mul(float3 a, float s)  { return {a.x * s, a.y * s, a.z * s}; }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float  dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 cross(float3 a, float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float  length(float3 a) { return std::sqrt(dot(a, a)); }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 normalize(float3 a) { float l = length(a); return l > 1e-8f ? mul(a, 1.0f / l) : float3{0, 1, 0}; }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float  clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float  lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Radial basis (u = theta 0, v = theta 90deg) orthogonal to the frame axis.
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
void radial_basis(const BindingFrame& frame, float3& u, float3& v) {
    float3 axis = normalize(sub(frame.end, frame.start));
    float3 ref  = frame.theta_zero_dir;
    float3 tangential = sub(ref, mul(axis, dot(ref, axis)));
    if (dot(tangential, tangential) < 1e-8f) {
        float3 fallback = std::fabs(axis.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0};
        tangential = sub(fallback, mul(axis, dot(fallback, axis)));
    }
    u = normalize(tangential);
    v = cross(axis, u);
}

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
const char* binding_pattern_name(BindingPatternKind kind) {
    switch (kind) {
        case BindingPatternKind::Guruguru: return "guruguru";
        case BindingPatternKind::Kikkou:   return "kikkou";
        case BindingPatternKind::Tasuki:   return "tasuki";
        case BindingPatternKind::Obi:      return "obi";
    }
    return "?";
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool parse_binding_pattern(const char* text, BindingPatternKind& out) {
    if (!text) return false;
    if (std::strcmp(text, "guruguru") == 0) { out = BindingPatternKind::Guruguru; return true; }
    if (std::strcmp(text, "kikkou")   == 0) { out = BindingPatternKind::Kikkou;   return true; }
    if (std::strcmp(text, "tasuki")   == 0) { out = BindingPatternKind::Tasuki;   return true; }
    if (std::strcmp(text, "obi") == 0 || std::strcmp(text, "waist") == 0 || std::strcmp(text, "center") == 0) {
        out = BindingPatternKind::Obi; return true;
    }
    return false;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
BindingPatternSpec BindingPatternSpec::create_default(BindingPatternKind kind) {
    BindingPatternSpec spec;
    spec.kind  = kind;
    spec.turns = kind == BindingPatternKind::Kikkou ? 3.0f : 4.0f;
    if (kind == BindingPatternKind::Obi) {
        spec.turns = 1.0f;
        spec.body_start = spec.body_end = 0.5f;   // one loop at mid-body
    }
    return spec;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
BindingPatternSpec& BindingPatternSpec::limit_turns_for(const BindingFrame& frame,
                                                        const BindingBodyProfile& profile) {
    const float span = length(sub(frame.end, frame.start)) * std::fabs(body_end - body_start);
    const float footprint = profile.rope_radius + profile.groove_width;
    if (kind == BindingPatternKind::Obi) {
        turns = 1.0f;
        // Loops touch: centre-to-centre = 2 rope radii (+5% so tubes do not z-fight).
        const float axis_len = std::max(length(sub(frame.end, frame.start)), 1e-4f);
        strand_spacing_t = 2.1f * profile.rope_radius / axis_len;
        return *this;
    }
    const float k = binding_strand_count(kind) == 1 ? 1.5f : 2.2f;
    const float max_turns = span / std::max(k * footprint, 1e-4f);
    // Round down to half turns; never below 1.5 so a spiral still reads.
    const float limited = std::floor(max_turns * 2.0f) * 0.5f;
    turns = std::max(1.5f, std::min(turns, limited));
    return *this;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
BindingBodyProfile BindingBodyProfile::scaled_plush(float metres_to_units) {
    BindingBodyProfile p;   // defaults are a plush preset in metres
    p.sink_depth   *= metres_to_units;
    p.bulge_height *= metres_to_units;
    p.groove_width *= metres_to_units;
    p.bulge_width  *= metres_to_units;
    p.rope_radius  *= metres_to_units;
    return p;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
int binding_strand_count(BindingPatternKind kind) {
    switch (kind) {
        case BindingPatternKind::Guruguru: return 1;
        case BindingPatternKind::Obi:      return 3;
        default:                           return 2;
    }
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
BindingBodyProfile BindingBodyProfile::pinched_for(BindingPatternKind kind) const {
    BindingBodyProfile p = *this;
    if (kind == BindingPatternKind::Obi) {
        p.sink_depth   *= 1.8f;
        p.groove_width *= 1.6f;
        p.bulge_height *= 1.4f;
        p.bulge_width  *= 1.6f;
        p.rope_follows_dent = true;
    }
    return p;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
void sample_binding_strand(const BindingPatternSpec& spec, int strand, float s,
                           float& t_out, float& theta_out) {
    s = clamp01(s);
    t_out = lerp(spec.body_start, spec.body_end, s);
    float turns = std::max(spec.turns, 0.25f);
    switch (spec.kind) {
        case BindingPatternKind::Guruguru:
            theta_out = spec.theta_offset_rad + kTwoPi * turns * s;
            break;
        case BindingPatternKind::Kikkou: {
            // Counter-wound spirals: the crossings (matching theta) are where
            // the smooth-min in the shader deepens the groove.
            float winding = strand == 0 ? 1.0f : -1.0f;
            theta_out = spec.theta_offset_rad + winding * kTwoPi * turns * s;
            break;
        }
        case BindingPatternKind::Tasuki:
            theta_out = strand == 0 ? spec.theta_offset_rad + kPi * s
                                    : spec.theta_offset_rad + kPi * (1.0f - s);
            break;
        case BindingPatternKind::Obi:
            // Three closed rings stacked along the axis (strand 1 at the middle).
            t_out = clamp01(0.5f * (spec.body_start + spec.body_end)
                            + static_cast<float>(strand - 1) * spec.strand_spacing_t);
            theta_out = spec.theta_offset_rad + kTwoPi * s;
            break;
    }
}

namespace {

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 surface_point_with_radius(const BindingFrame& frame, float t, float theta,
                                 float radius, float radial_offset) {
    float3 u, v;
    radial_basis(frame, u, v);
    t = clamp01(t);
    float3 axis_point = add(frame.start, mul(sub(frame.end, frame.start), t));
    float3 radial = add(mul(u, std::cos(theta)), mul(v, std::sin(theta)));
    return add(axis_point, mul(radial, std::max(radius, 0.001f) + radial_offset));
}

// Rope centre sits rope_radius * (1 - embed) outside the body surface; when
// the rope follows the dent it is lowered by the sink depth so it touches
// the deformed skin (the groove under it stays saturated either way).
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float radial_offset_of(const BindingBodyProfile& profile) {
    float offset = profile.rope_radius * (1.0f - clamp01(profile.surface_embed_ratio));
    if (profile.rope_follows_dent) offset -= profile.sink_depth;
    return offset;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float frame_radius(const BindingFrame& frame, float t) {
    return lerp(std::max(frame.radius_start, 0.001f),
                std::max(frame.radius_end, 0.001f), clamp01(t));
}

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float3 binding_surface_point(const BindingFrame& frame, float t, float theta,
                             float radial_offset) {
    return surface_point_with_radius(frame, t, theta, frame_radius(frame, t), radial_offset);
}

namespace {

/// Moller-Trumbore, both faces. Returns the ray parameter or -1.
/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
float ray_triangle(float3 o, float3 d, float3 a, float3 b, float3 c) {
    float3 e1 = sub(b, a), e2 = sub(c, a);
    float3 p = cross(d, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-8f) return -1.0f;
    float inv = 1.0f / det;
    float3 s = sub(o, a);
    float u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    float3 q = cross(s, e1);
    float v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return dot(e2, q) * inv;
}

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
SurfaceRadiusFn make_mesh_surface_radius(const float3* positions, size_t vertex_count,
                                         const uint32_t* indices, size_t index_count,
                                         const BindingFrame& frame) {
    std::vector<float3>   pts(positions, positions + vertex_count);
    std::vector<uint32_t> idx(indices, indices + (index_count / 3) * 3);
    return [pts = std::move(pts), idx = std::move(idx), frame](float t, float theta) -> float {
        float3 u, v;
        radial_basis(frame, u, v);
        float3 dir    = add(mul(u, std::cos(theta)), mul(v, std::sin(theta)));
        float3 origin = add(frame.start, mul(sub(frame.end, frame.start), clamp01(t)));
        float nearest = -1.0f;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (idx[i] >= pts.size() || idx[i + 1] >= pts.size() || idx[i + 2] >= pts.size()) continue;
            float hit = ray_triangle(origin, dir, pts[idx[i]], pts[idx[i + 1]], pts[idx[i + 2]]);
            if (hit > 1e-4f && (nearest < 0.0f || hit < nearest)) nearest = hit;
        }
        return nearest > 0.0f ? nearest : frame_radius(frame, t);
    };
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
NawaCapsuleChain build_capsule_chain(const BindingPatternSpec& spec,
                                     const BindingFrame& frame,
                                     const BindingBodyProfile& profile,
                                     const SurfaceRadiusFn& surface_radius) {
    NawaCapsuleChain chain;
    auto point_at = [&](float t, float theta) {
        float radius = surface_radius ? surface_radius(t, theta) : frame_radius(frame, t);
        return surface_point_with_radius(frame, t, theta, radius, radial_offset_of(profile));
    };
    const int strand_count = binding_strand_count(spec.kind);
    const int segments_per_strand = static_cast<int>(kNawaMaxSegments) / strand_count;
    chain.strands.resize(static_cast<size_t>(strand_count));
    for (int strand = 0; strand < strand_count; ++strand) {
        auto& poly = chain.strands[static_cast<size_t>(strand)];
        poly.reserve(static_cast<size_t>(segments_per_strand) + 1);
        float t, theta;
        sample_binding_strand(spec, strand, 0.0f, t, theta);
        float3 prev = point_at(t, theta);
        poly.push_back(prev);
        for (int seg = 1; seg <= segments_per_strand; ++seg) {
            float s = static_cast<float>(seg) / static_cast<float>(segments_per_strand);
            sample_binding_strand(spec, strand, s, t, theta);
            float3 cur = point_at(t, theta);
            poly.push_back(cur);
            prev = cur;
        }
    }
    // Interleave strands by progress so a prefix of the segment array is the
    // same partially wrapped geometry that RopePass displays.
    for (int seg = 0; seg < segments_per_strand; ++seg) {
        for (int strand = 0; strand < strand_count; ++strand) {
            const auto& poly = chain.strands[static_cast<size_t>(strand)];
            chain.segments.push_back({poly[static_cast<size_t>(seg)],
                                      poly[static_cast<size_t>(seg + 1)],
                                      profile.rope_radius});
        }
    }
    return chain;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
NawaBindUBO compose_bind_ubo(const NawaCapsuleChain& chain,
                             const BindingBodyProfile& profile,
                             float tightness,
                             uint32_t visible_segments) {
    NawaBindUBO ubo{};
    tightness = clamp01(tightness);
    const uint32_t count = std::min<uint32_t>(
        {visible_segments, static_cast<uint32_t>(chain.segments.size()), kNawaMaxSegments});
    for (uint32_t i = 0; i < count; ++i) {
        const NawaCapsuleSegment& s = chain.segments[i];
        ubo.seg_start[i][0] = s.start.x; ubo.seg_start[i][1] = s.start.y;
        ubo.seg_start[i][2] = s.start.z; ubo.seg_start[i][3] = s.radius;
        ubo.seg_end[i][0] = s.end.x; ubo.seg_end[i][1] = s.end.y;
        ubo.seg_end[i][2] = s.end.z; ubo.seg_end[i][3] = 0.0f;
    }
    // Displacement amounts scale with tightness; widths do not (design §0.2).
    ubo.deform[0] = profile.sink_depth * tightness;
    ubo.deform[1] = profile.bulge_height * tightness;
    ubo.deform[2] = profile.groove_width;
    ubo.deform[3] = profile.bulge_width;
    ubo.surface[0] = profile.fur_crush * tightness;
    ubo.surface[1] = profile.seam_emphasis * tightness;
    ubo.surface[2] = 0.0f;
    // Smooth-min blend = half the groove width so crossings merge smoothly.
    ubo.surface[3] = profile.groove_width * 0.5f;
    ubo.meta[0] = static_cast<float>(count);
    return ubo;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
BindingFrame estimate_binding_frame(const float3* positions, size_t count,
                                    float body_start_ratio, float body_end_ratio) {
    BindingFrame frame;
    if (!positions || count == 0) return frame;

    float3 lo = positions[0], hi = positions[0];
    for (size_t i = 1; i < count; ++i) {
        const float3& p = positions[i];
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    const float  height = std::max(hi.y - lo.y, 1e-4f);
    const float  cx = 0.5f * (lo.x + hi.x), cz = 0.5f * (lo.z + hi.z);
    const float  y_start = lo.y + height * body_start_ratio;
    const float  y_end   = lo.y + height * body_end_ratio;

    // 60th percentile of horizontal distance within a band around `y`:
    // ignores wings / ears that stick out on a few vertices.
    auto radius_at = [&](float y) {
        std::vector<float> d;
        const float band = height * 0.06f;
        for (size_t i = 0; i < count; ++i) {
            const float3& p = positions[i];
            if (std::fabs(p.y - y) > band) continue;
            d.push_back(std::sqrt((p.x - cx) * (p.x - cx) + (p.z - cz) * (p.z - cz)));
        }
        if (d.empty()) return height * 0.25f;
        std::sort(d.begin(), d.end());
        return d[std::min(d.size() - 1, static_cast<size_t>(d.size() * 0.6))];
    };

    frame.start = {cx, y_start, cz};
    frame.end   = {cx, y_end, cz};
    frame.radius_start = radius_at(y_start);
    frame.radius_end   = radius_at(y_end);
    frame.theta_zero_dir = {0.0f, 0.0f, 1.0f};
    return frame;
}

} // namespace pictor_fbx_viewer
