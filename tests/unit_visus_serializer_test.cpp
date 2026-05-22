/// VisusDesc ↔ JSON round-trip + ResourceRef encoding.

#include "pictor/visus/visus.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

namespace {

VisusDesc make_sample() {
    VisusDesc d;
    d.name = "kuzu_enemy_mob";
    d.geometry_kind = VisusGeometryKind::MODEL;

    d.asset.local_path   = "models/kuzu.fbx";
    d.asset.remote_url   = "https://cdn.example/kuzu.fbx";
    d.asset.sha256       = "deadbeef00";
    d.asset.size_bytes   = 12345678;
    d.asset.fetch_policy = ResourceRef::FetchPolicy::REVALIDATE;
    d.asset.headers.emplace_back("X-Auth", "${env:CDN_TOKEN}");

    d.mesh           = INVALID_MESH;
    d.model          = 5;
    d.shader         = INVALID_MESH;
    d.generic_handle = 0;

    // CUSTOM kind 用シェーダステージ (vert/frag/comp) も round-trip 対象。
    d.shader_stages.vert.local_path   = "shaders/custom_kuzu.vert.spv";
    d.shader_stages.frag.local_path   = "shaders/custom_kuzu.frag.spv";
    d.shader_stages.frag.remote_url   = "https://cdn.example/custom_kuzu.frag.spv";
    d.shader_stages.frag.fetch_policy = ResourceRef::FetchPolicy::CACHE_FIRST;

    VisusMaterialSlot body;
    body.slot_name = "body";
    body.material  = 12;
    body.material_resource.local_path = "materials/kuzu_body.mat.json";
    d.materials.push_back(body);

    VisusMaterialSlot armor;
    armor.slot_name = "armor";
    armor.material  = 13;
    d.materials.push_back(armor);

    VisusTextureSlot tex;
    tex.slot_name = "albedo_override";
    tex.texture   = 7;
    tex.texture_resource.local_path = "textures/kuzu_albedo.ktx2";
    d.textures.push_back(tex);

    d.default_flags = ObjectFlags::DYNAMIC | ObjectFlags::CAST_SHADOW;
    d.layer         = 2;
    d.pool_hint     = PoolType::GPU_DRIVEN;
    d.initial_lod   = 1;

    d.animation_default.kind  = VisusAnimationDefault::Kind::CLIP;
    d.animation_default.name  = "idle";
    d.animation_default.loop  = true;
    d.animation_default.speed = 1.25f;

    d.shader_key_override = 0xC0FFEE1234ULL;
    return d;
}

bool same_rr(const ResourceRef& a, const ResourceRef& b) {
    if (a.local_path   != b.local_path)   return false;
    if (a.remote_url   != b.remote_url)   return false;
    if (a.sha256       != b.sha256)       return false;
    if (a.size_bytes   != b.size_bytes)   return false;
    if (a.fetch_policy != b.fetch_policy) return false;
    if (a.headers.size() != b.headers.size()) return false;
    for (size_t i = 0; i < a.headers.size(); ++i) {
        if (a.headers[i] != b.headers[i]) return false;
    }
    return true;
}

bool same_desc(const VisusDesc& a, const VisusDesc& b) {
    if (a.name != b.name) return false;
    if (a.geometry_kind != b.geometry_kind) return false;
    if (!same_rr(a.asset, b.asset)) return false;
    if (a.rive_artboard != b.rive_artboard) return false;
    if (a.text_default  != b.text_default)  return false;
    if (a.mesh != b.mesh) return false;
    if (a.model != b.model) return false;
    if (a.shader != b.shader) return false;
    if (a.generic_handle != b.generic_handle) return false;

    if (!same_rr(a.shader_stages.vert, b.shader_stages.vert)) return false;
    if (!same_rr(a.shader_stages.frag, b.shader_stages.frag)) return false;
    if (!same_rr(a.shader_stages.comp, b.shader_stages.comp)) return false;

    if (a.materials.size() != b.materials.size()) return false;
    for (size_t i = 0; i < a.materials.size(); ++i) {
        if (a.materials[i].slot_name != b.materials[i].slot_name) return false;
        if (a.materials[i].material  != b.materials[i].material)  return false;
        if (!same_rr(a.materials[i].material_resource, b.materials[i].material_resource)) return false;
    }
    if (a.textures.size() != b.textures.size()) return false;
    for (size_t i = 0; i < a.textures.size(); ++i) {
        if (a.textures[i].slot_name != b.textures[i].slot_name) return false;
        if (a.textures[i].texture   != b.textures[i].texture)   return false;
        if (!same_rr(a.textures[i].texture_resource, b.textures[i].texture_resource)) return false;
    }

    if (a.default_flags != b.default_flags) return false;
    if (a.layer         != b.layer)         return false;
    if (a.pool_hint     != b.pool_hint)     return false;
    if (a.initial_lod   != b.initial_lod)   return false;

    if (a.animation_default.kind  != b.animation_default.kind)  return false;
    if (a.animation_default.name  != b.animation_default.name)  return false;
    if (a.animation_default.loop  != b.animation_default.loop)  return false;
    if (a.animation_default.speed != b.animation_default.speed) return false;

    if (a.shader_key_override != b.shader_key_override) return false;
    return true;
}

} // namespace

int main() {
    // 1. Round-trip: encode → decode → field-for-field equality.
    {
        VisusDesc src = make_sample();
        std::string j = to_visus_json(src);
        VisusDesc round;
        std::string err;
        bool ok = from_visus_json(j, round, &err);
        PT_ASSERT(ok, "parse round-trip succeeds");
        if (!ok) std::fprintf(stderr, "  parse error: %s\n", err.c_str());
        PT_ASSERT(same_desc(src, round), "round-trip preserves all fields");
    }

    // 2. Default VisusDesc encodes and decodes cleanly.
    {
        VisusDesc src;   // all defaults
        std::string j = to_visus_json(src);
        VisusDesc round;
        bool ok = from_visus_json(j, round, nullptr);
        PT_ASSERT(ok, "default VisusDesc round-trips");
        PT_ASSERT(same_desc(src, round), "default fields preserved");
    }

    // 3. Empty materials / textures arrays serialize as `[]`.
    {
        VisusDesc src;
        src.name = "bare";
        std::string j = to_visus_json(src);
        PT_ASSERT(j.find("\"materials\": []") != std::string::npos, "empty materials -> []");
        PT_ASSERT(j.find("\"textures\": []")  != std::string::npos, "empty textures  -> []");
    }

    // 4. Unknown keys are tolerated (forward compat).
    {
        const std::string j =
            "{ \"version\": 1, \"name\": \"x\", "
            "  \"unknown_root_key\": [1,2,3], "
            "  \"geometry\": { \"kind\": \"primitive\", \"future_field\": 42 } }";
        VisusDesc out;
        std::string err;
        bool ok = from_visus_json(j, out, &err);
        PT_ASSERT(ok, "unknown keys are skipped");
        PT_ASSERT(out.name == "x", "name parsed despite junk siblings");
        PT_ASSERT(out.geometry_kind == VisusGeometryKind::PRIMITIVE, "kind parsed despite junk siblings");
    }

    // 5. Syntax error → false + non-empty err.
    {
        VisusDesc out;
        std::string err;
        bool ok = from_visus_json("{ \"name\": ", out, &err);
        PT_ASSERT(!ok, "broken JSON returns false");
        PT_ASSERT(!err.empty(), "error message provided");
    }

    // 6. All geometry kind enum values round-trip.
    {
        const VisusGeometryKind kinds[] = {
            VisusGeometryKind::NONE,
            VisusGeometryKind::PRIMITIVE,
            VisusGeometryKind::MODEL,
            VisusGeometryKind::RIVE,
            VisusGeometryKind::UI,
            VisusGeometryKind::PARTICLE,
            VisusGeometryKind::TEXT,
            VisusGeometryKind::CUSTOM,
        };
        for (auto k : kinds) {
            VisusDesc src; src.geometry_kind = k;
            std::string j = to_visus_json(src);
            VisusDesc round;
            from_visus_json(j, round, nullptr);
            PT_ASSERT(round.geometry_kind == k, "geometry_kind round-trips");
        }
    }

    // 7. CUSTOM kind の shader_stages が round-trip し、 has_graphics_stages
    //    が正しく判定される。
    {
        VisusDesc src;
        src.name          = "custom_fx";
        src.geometry_kind = VisusGeometryKind::CUSTOM;
        src.shader        = 9;
        src.shader_stages.vert.local_path = "fx.vert.spv";
        src.shader_stages.frag.local_path = "fx.frag.spv";

        PT_ASSERT(src.shader_stages.has_graphics_stages(),
                  "vert+frag set -> has_graphics_stages true");
        PT_ASSERT(!src.shader_stages.empty(), "shader_stages not empty");

        std::string j = to_visus_json(src);
        VisusDesc round;
        std::string err;
        bool ok = from_visus_json(j, round, &err);
        PT_ASSERT(ok, "CUSTOM shader_stages round-trip parses");
        PT_ASSERT(round.shader_stages.vert.local_path == "fx.vert.spv",
                  "vert stage path preserved");
        PT_ASSERT(round.shader_stages.frag.local_path == "fx.frag.spv",
                  "frag stage path preserved");
        PT_ASSERT(round.shader == 9u, "CUSTOM shader handle preserved");
        PT_ASSERT(round.shader_stages.has_graphics_stages(),
                  "round-tripped shader_stages still graphics-capable");
    }

    // 8. 空の shader_stages は has_graphics_stages=false / empty=true。
    {
        VisusShaderStages s;
        PT_ASSERT(s.empty(), "default shader_stages empty");
        PT_ASSERT(!s.has_graphics_stages(),
                  "default shader_stages not graphics-capable");
    }

    return report("unit_visus_serializer_test");
}
