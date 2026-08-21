#include "visus_v1_compat.h"

namespace pictor::visus_v1 {

namespace {

void warn(std::vector<std::string>* warnings, std::string msg) {
    if (warnings) warnings->push_back(std::move(msg));
}

// v1 ResourceRef object → local_path。 remote 設定は捨てる (警告)。
std::string resource_path(const VisusValue* v, const char* what,
                          std::vector<std::string>* warnings) {
    if (!v || !v->is_object()) return {};
    const VisusMetadata& r = v->as_object();
    const std::string local  = r.get_string("local_path").value_or("");
    const std::string remote = r.get_string("remote_url").value_or("");
    if (!remote.empty()) {
        // remote_url may contain a private endpoint or embedded credentials.
        // Report the lossy conversion without copying attacker-controlled data
        // into logs surfaced by callers.
        warn(warnings, std::string("v1 ") + what + ": remote_url dropped");
    }
    return local;
}

// v1 の "handle:N" 文字列は捨てる (同一性を表現できない)。
void drop_handle(const VisusMetadata& geo, const char* key,
                 std::vector<std::string>* warnings) {
    const auto s = geo.get_string(key);
    if (s && s->rfind("handle:", 0) == 0) {
        warn(warnings, std::string("v1 geometry.") + key + ": resolved handle dropped");
    }
}

void convert_geometry(const VisusMetadata& geo, VisusDesc& out,
                      std::vector<std::string>* warnings) {
    out.kind  = visus_kind_from_str(geo.get_string("kind").value_or("none"));
    out.asset = resource_path(geo.find("asset"), "geometry.asset", warnings);

    if (const auto s = geo.get_string("rive_artboard"); s && !s->empty())
        out.metadata.set(visus_keys::kRiveArtboard, *s);
    if (const auto s = geo.get_string("text_default"); s && !s->empty())
        out.metadata.set(visus_keys::kTextDefault, *s);

    if (const VisusMetadata* st = geo.get_object("shader_stages")) {
        const std::string vert = resource_path(st->find("vert"), "shader_stages.vert", warnings);
        const std::string frag = resource_path(st->find("frag"), "shader_stages.frag", warnings);
        const std::string comp = resource_path(st->find("comp"), "shader_stages.comp", warnings);
        if (!vert.empty() && !frag.empty()) {
            out.metadata.set(visus_keys::kShader,
                             visus_shader_ref_to_value(VisusShaderRef::stages(vert, frag, comp)));
        } else if (!vert.empty() || !frag.empty() || !comp.empty()) {
            warn(warnings, "v1 shader_stages: incomplete graphics stages dropped");
        }
        if (const VisusMetadata* vl = st->get_object("vertex_layout")) {
            const VisusValue::Array* attrs = vl->get_array("attributes");
            const double stride = vl->get_number("stride").value_or(0.0);
            if ((attrs && !attrs->empty()) || stride > 0.0) {
                out.metadata.set(visus_keys::kShaderVertexLayout, VisusValue(*vl));
            }
        }
    }

    drop_handle(geo, "mesh", warnings);
    drop_handle(geo, "model", warnings);
    drop_handle(geo, "shader", warnings);
}

// v1 model materials[] → parts[]。 Other kinds cannot use v2 parts, so keep
// their material paths in top-level metadata instead.
void convert_materials(const VisusValue::Array& mats, VisusDesc& out,
                       std::vector<std::string>* warnings) {
    for (const VisusValue& m : mats) {
        if (!m.is_object()) continue;
        const VisusMetadata& mo = m.as_object();
        std::string slot = mo.get_string("slot").value_or("");
        const std::string res = resource_path(mo.find("resource"), "materials.resource", warnings);
        const std::string label = slot.empty() ? "*" : slot;
        if (out.kind == VisusKind::MODEL) {
            VisusPart part;
            part.part = label;
            if (!res.empty()) part.metadata.set("material", res);
            out.parts.push_back(std::move(part));
        } else if (!res.empty()) {
            const std::string key = slot.empty()
                ? std::string(visus_keys::kMaterial)
                : std::string(visus_keys::kMaterialPrefix) + slot;
            out.metadata.set(key, res);
        }
        if (const auto h = mo.get_string("material"); h && h->rfind("handle:", 0) == 0)
            warn(warnings, "v1 materials." + label + ": resolved handle dropped");
    }
}

// v1 textures[] → metadata["texture.<slot>"] = path。
void convert_textures(const VisusValue::Array& texs, VisusDesc& out,
                      std::vector<std::string>* warnings) {
    for (const VisusValue& t : texs) {
        if (!t.is_object()) continue;
        const VisusMetadata& to = t.as_object();
        const std::string slot = to.get_string("slot").value_or("");
        const std::string res  = resource_path(to.find("resource"), "textures.resource", warnings);
        if (slot.empty() || res.empty()) continue;
        out.metadata.set(std::string(visus_keys::kTexturePrefix) + slot, res);
        if (const auto h = to.get_string("texture"); h && h->rfind("handle:", 0) == 0)
            warn(warnings, "v1 textures." + slot + ": resolved handle dropped");
    }
}

void convert_flags(const VisusMetadata& fl, VisusDesc& out) {
    if (const auto v = fl.get_number("default_flags")) out.metadata.set(visus_keys::kRenderFlags, *v);
    if (const auto v = fl.get_number("layer"))         out.metadata.set(visus_keys::kRenderLayer, *v);
    if (const auto v = fl.get_string("pool_hint"))     out.metadata.set(visus_keys::kRenderPool, *v);
    if (const auto v = fl.get_number("initial_lod"))   out.metadata.set(visus_keys::kRenderLod, *v);
}

void convert_animation(const VisusMetadata& an, VisusDesc& out) {
    const std::string kind = an.get_string("kind").value_or("none");
    if (kind == "none") return;
    out.metadata.set(visus_keys::kAnimationKind, kind);
    if (const auto n = an.get_string("name"); n && !n->empty())
        out.metadata.set(visus_keys::kAnimationDefault, *n);
    if (const auto l = an.get_bool("loop"))    out.metadata.set(visus_keys::kAnimationLoop, *l);
    if (const auto s = an.get_number("speed")) out.metadata.set(visus_keys::kAnimationSpeed, *s);
}

} // namespace

bool convert(const VisusMetadata& root, VisusDesc& out,
             std::vector<std::string>* warnings) {
    out = VisusDesc{};
    warn(warnings, "v1 converted");

    out.name = root.get_string("name").value_or("");

    if (const VisusMetadata* geo = root.get_object("geometry"))
        convert_geometry(*geo, out, warnings);
    if (const VisusValue::Array* mats = root.get_array("materials"))
        convert_materials(*mats, out, warnings);
    if (const VisusValue::Array* texs = root.get_array("textures"))
        convert_textures(*texs, out, warnings);
    if (const VisusMetadata* fl = root.get_object("flags"))
        convert_flags(*fl, out);
    if (const VisusMetadata* an = root.get_object("animation_default"))
        convert_animation(*an, out);
    if (const auto k = root.get_number("shader_key_override"); k && *k != 0.0)
        out.metadata.set(visus_keys::kShaderKeyOverride, *k);

    return true;
}

} // namespace pictor::visus_v1
