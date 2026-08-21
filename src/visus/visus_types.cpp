#include "pictor/visus/visus.h"

namespace pictor {

// ---- VisusKind -------------------------------------------------------------

const char* visus_kind_to_str(VisusKind k) {
    switch (k) {
        case VisusKind::NONE:      return "none";
        case VisusKind::MODEL:     return "model";
        case VisusKind::RIVE:      return "rive";
        case VisusKind::PRIMITIVE: return "primitive";
        case VisusKind::CUSTOM:    return "custom";
        case VisusKind::UI:        return "ui";
        case VisusKind::PARTICLE:  return "particle";
        case VisusKind::TEXT:      return "text";
        case VisusKind::GROUP:     return "group";
    }
    return "none";
}

VisusKind visus_kind_from_str(std::string_view s) {
    if (s == "model")     return VisusKind::MODEL;
    if (s == "rive")      return VisusKind::RIVE;
    if (s == "primitive") return VisusKind::PRIMITIVE;
    if (s == "custom")    return VisusKind::CUSTOM;
    if (s == "ui")        return VisusKind::UI;
    if (s == "particle")  return VisusKind::PARTICLE;
    if (s == "text")      return VisusKind::TEXT;
    if (s == "group")     return VisusKind::GROUP;
    return VisusKind::NONE;
}

// ---- VisusShaderRef --------------------------------------------------------

VisusShaderRef VisusShaderRef::builtin(std::string name) {
    VisusShaderRef r;
    r.kind = Kind::BUILTIN;
    r.name = std::move(name);
    return r;
}

VisusShaderRef VisusShaderRef::stages(std::string vert, std::string frag, std::string comp) {
    VisusShaderRef r;
    r.kind = Kind::STAGES;
    r.name.clear();
    r.vert = std::move(vert);
    r.frag = std::move(frag);
    r.comp = std::move(comp);
    return r;
}

VisusShaderRef VisusShaderRef::visus(std::string name) {
    VisusShaderRef r;
    r.kind = Kind::VISUS;
    r.name = std::move(name);
    return r;
}

namespace {
constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kVisusPrefix   = "visus:";
} // namespace

bool visus_shader_ref_from_value(const VisusValue& v, VisusShaderRef& out) {
    if (v.is_string()) {
        std::string_view s = v.as_string();
        if (s.substr(0, kBuiltinPrefix.size()) == kBuiltinPrefix) {
            std::string name(s.substr(kBuiltinPrefix.size()));
            if (name.empty()) return false;
            out = VisusShaderRef::builtin(std::move(name));
            return true;
        }
        if (s.substr(0, kVisusPrefix.size()) == kVisusPrefix) {
            std::string name(s.substr(kVisusPrefix.size()));
            if (name.empty()) return false;
            out = VisusShaderRef::visus(std::move(name));
            return true;
        }
        return false;
    }
    if (v.is_object()) {
        const VisusMetadata& o = v.as_object();
        std::string vert = o.get_string("vert").value_or("");
        std::string frag = o.get_string("frag").value_or("");
        std::string comp = o.get_string("comp").value_or("");
        if (vert.empty() || frag.empty()) return false;
        out = VisusShaderRef::stages(std::move(vert), std::move(frag), std::move(comp));
        return true;
    }
    return false;
}

VisusValue visus_shader_ref_to_value(const VisusShaderRef& ref) {
    switch (ref.kind) {
        case VisusShaderRef::Kind::BUILTIN:
            return VisusValue(std::string(kBuiltinPrefix) + ref.name);
        case VisusShaderRef::Kind::VISUS:
            return VisusValue(std::string(kVisusPrefix) + ref.name);
        case VisusShaderRef::Kind::STAGES: {
            VisusMetadata o;
            o.set("vert", ref.vert);
            o.set("frag", ref.frag);
            if (!ref.comp.empty()) o.set("comp", ref.comp);
            return VisusValue(std::move(o));
        }
    }
    return VisusValue();
}

// ---- VisusChildRef / VisusDesc --------------------------------------------

bool VisusChildRef::is_path_ref() const {
    constexpr std::string_view suffix = ".visus.json";
    if (visus.size() >= suffix.size() &&
        std::string_view(visus).substr(visus.size() - suffix.size()) == suffix)
        return true;
    return visus.find('/') != std::string::npos || visus.find('\\') != std::string::npos;
}

const VisusPart* VisusDesc::find_part(std::string_view part) const {
    const VisusPart* wildcard = nullptr;
    for (const VisusPart& p : parts) {
        if (p.part == part) return &p;
        if (!wildcard && p.is_wildcard()) wildcard = &p;
    }
    return wildcard;
}

} // namespace pictor
