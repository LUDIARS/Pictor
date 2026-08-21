#include "pictor/visus/visus_serializer.h"

#include "pictor/visus/visus_package_serializer.h"

#include "visus_json.h"
#include "visus_v1_compat.h"

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pictor {

namespace {

constexpr double kCurrentVersion = 2.0;

// v1 にしか現れないトップレベル key。 `version` を欠いた文書がどちらの
// スキーマかを見分けるのに使う (§2.2 の移行表の左側)。
constexpr std::string_view kV1OnlyKeys[] = {
    "geometry", "materials", "textures", "flags",
    "animation_default", "shader_key_override",
};

void warn(std::vector<std::string>* warnings, std::string msg) {
    if (warnings) warnings->push_back(std::move(msg));
}

// ---- VisusDesc → value ------------------------------------------------------

VisusValue packages_to_value(const std::vector<VisusPackageRef>& packages) {
    VisusValue::Array out;
    out.reserve(packages.size());
    for (const VisusPackageRef& r : packages) out.push_back(visus_package_ref_to_value(r));
    return VisusValue(std::move(out));
}

/// `shader_packages` 配列を読む。 空 / 非配列なら何も積まない。 同名の重複は
/// 実効列マージ (§2.5.3) で先勝ちマージされるのでここでは落とさない。
void packages_from_value(const VisusMetadata& owner, const char* context,
                         std::vector<VisusPackageRef>& out,
                         std::vector<std::string>* warnings) {
    const VisusValue::Array* arr = owner.get_array(visus_keys::kShaderPackages);
    if (!arr) return;
    out.reserve(arr->size());
    for (const VisusValue& v : *arr) {
        VisusPackageRef ref;
        std::vector<std::string> local;
        if (!visus_package_ref_from_value(v, ref, &local)) {
            for (std::string& w : local) warn(warnings, std::string(context) + w);
            continue;
        }
        out.push_back(std::move(ref));
    }
}

VisusValue part_to_value(const VisusPart& p) {
    VisusMetadata o;
    o.set("part", p.part);
    o.set("shader", visus_shader_ref_to_value(p.shader));
    if (!p.metadata.empty()) o.set("metadata", VisusValue(p.metadata));
    if (!p.packages.empty())
        o.set(visus_keys::kShaderPackages, packages_to_value(p.packages));
    return VisusValue(std::move(o));
}

VisusValue attach_to_value(const VisusAttach& a) {
    VisusMetadata o;
    if (a.has_bone()) o.set("bone", a.bone);
    if (a.has_offset()) {
        VisusValue::Array off;
        off.emplace_back(static_cast<double>(a.offset[0]));
        off.emplace_back(static_cast<double>(a.offset[1]));
        off.emplace_back(static_cast<double>(a.offset[2]));
        o.set("offset", VisusValue(std::move(off)));
    }
    return VisusValue(std::move(o));
}

VisusValue child_to_value(const VisusChildRef& c) {
    VisusMetadata o;
    o.set("visus", c.visus);
    if (c.attach.has_bone() || c.attach.has_offset()) o.set("attach", attach_to_value(c.attach));
    if (!c.metadata.empty()) o.set("metadata", VisusValue(c.metadata));
    return VisusValue(std::move(o));
}

// ---- value → VisusDesc ------------------------------------------------------

bool part_from_value(const VisusValue& v, VisusPart& out,
                     std::vector<std::string>* warnings) {
    if (!v.is_object()) {
        warn(warnings, "parts[]: non-object entry ignored");
        return false;
    }
    const VisusMetadata& o = v.as_object();
    out.part = o.get_string("part").value_or("");
    if (out.part.empty()) {
        warn(warnings, "parts[]: entry without 'part' ignored");
        return false;
    }
    if (const VisusValue* s = o.find("shader")) {
        if (!visus_shader_ref_from_value(*s, out.shader)) {
            warn(warnings, "parts[" + out.part + "].shader: unrecognized shader reference, using builtin:pbr");
            out.shader = VisusShaderRef::builtin();
        }
    }
    if (const VisusMetadata* m = o.get_object("metadata")) out.metadata = *m;
    packages_from_value(o, "parts[].", out.packages, warnings);
    return true;
}

void attach_from_value(const VisusValue& v, VisusAttach& out,
                       std::vector<std::string>* warnings) {
    if (!v.is_object()) return;
    const VisusMetadata& o = v.as_object();
    out.bone = o.get_string("bone").value_or("");
    if (const VisusValue::Array* off = o.get_array("offset")) {
        for (size_t i = 0; i < 3 && i < off->size(); ++i) {
            if (!(*off)[i].is_number()) {
                warn(warnings, "children[].attach.offset contains a non-number; using 0");
                continue;
            }
            const double n = (*off)[i].as_number();
            if (!std::isfinite(n) ||
                std::fabs(n) > static_cast<double>(std::numeric_limits<float>::max())) {
                warn(warnings, "children[].attach.offset is out of float range; using 0");
                continue;
            }
            out.offset[i] = static_cast<float>(n);
        }
    }
}

void child_from_value(const VisusValue& v, VisusChildRef& out,
                      std::vector<std::string>* warnings) {
    if (!v.is_object()) return;
    const VisusMetadata& o = v.as_object();
    out.visus = o.get_string("visus").value_or("");
    if (const VisusValue* a = o.find("attach")) attach_from_value(*a, out.attach, warnings);
    if (const VisusMetadata* m = o.get_object("metadata")) out.metadata = *m;
}

} // namespace

// ---- public -----------------------------------------------------------------

VisusValue visus_desc_to_value(const VisusDesc& desc) {
    VisusMetadata root;
    root.set("version", kCurrentVersion);
    root.set("name", desc.name);
    root.set("kind", visus_kind_to_str(desc.kind));
    root.set("asset", desc.asset);

    VisusValue::Array parts;
    parts.reserve(desc.parts.size());
    for (const VisusPart& p : desc.parts) parts.push_back(part_to_value(p));
    root.set("parts", VisusValue(std::move(parts)));

    VisusValue::Array children;
    children.reserve(desc.children.size());
    for (const VisusChildRef& c : desc.children) children.push_back(child_to_value(c));
    root.set("children", VisusValue(std::move(children)));

    root.set("metadata", VisusValue(desc.metadata));
    if (!desc.packages.empty())
        root.set(visus_keys::kShaderPackages, packages_to_value(desc.packages));
    return VisusValue(std::move(root));
}

std::string to_visus_json(const VisusDesc& desc) {
    return visus_json::emit(visus_desc_to_value(desc));
}

bool visus_document_version(const VisusMetadata&      root,
                            double&                   out_version,
                            std::string*              error,
                            std::vector<std::string>* warnings) {
    const VisusValue* version_value = root.find("version");
    if (version_value && !version_value->is_number()) {
        if (error) *error = "visus version must be a number";
        return false;
    }
    // `version` は任意。 欠けているときに無条件で v2 と見なすと、 version を
    // 落とした v1 文書が「name だけ残して他は全部 default」という形で黙って
    // 成功してしまう。 v1 固有のトップレベル構造が残っていれば v1 として読む。
    if (version_value) {
        out_version = version_value->as_number();
        return true;
    }
    out_version = kCurrentVersion;
    for (std::string_view k : kV1OnlyKeys) {
        if (root.has(k)) {
            warn(warnings, "visus version missing; read as version 1");
            out_version = 1.0;
            break;
        }
    }
    return true;
}

bool visus_desc_from_value(const VisusValue&         value,
                           VisusDesc&                out,
                           std::string*              error,
                           std::vector<std::string>* warnings) {
    if (!value.is_object()) {
        if (error) *error = "visus root must be an object";
        return false;
    }
    const VisusMetadata& root = value.as_object();

    double version = kCurrentVersion;
    if (!visus_document_version(root, version, error, warnings)) return false;
    if (version == 1.0) {
        return visus_v1::convert(root, out, warnings);
    }
    if (version != kCurrentVersion) {
        if (error) *error = "unsupported visus version";
        return false;
    }

    out = VisusDesc{};
    out.name  = root.get_string("name").value_or("");
    const std::string kind = root.get_string("kind").value_or("none");
    out.kind  = visus_kind_from_str(kind);
    if (out.kind == VisusKind::NONE && kind != "none") {
        warn(warnings, "unknown visus kind; using none");
    }
    out.asset = root.get_string("asset").value_or("");

    if (const VisusValue::Array* parts = root.get_array("parts")) {
        out.parts.reserve(parts->size());
        // `parts` は信頼できない JSON 由来で、 要素数は kMaxJsonValues までしか
        // 縛られていない。 既出 part を線形走査すると数万要素で O(N^2) になるため
        // hash set で照合する (visus_json.cpp の kMaxObjectMembers と同じ理由)。
        std::unordered_set<std::string> seen_parts;
        for (const VisusValue& pv : *parts) {
            VisusPart p;
            if (!part_from_value(pv, p, warnings)) continue;
            if (!seen_parts.insert(p.part).second) {
                warn(warnings, "parts[]: duplicate part ignored");
                continue;
            }
            out.parts.push_back(std::move(p));
        }
    }
    if (const VisusValue::Array* children = root.get_array("children")) {
        out.children.reserve(children->size());
        for (const VisusValue& cv : *children) {
            VisusChildRef c;
            child_from_value(cv, c, warnings);
            if (c.visus.empty()) {
                warn(warnings, "children[]: entry without 'visus' ignored");
                continue;
            }
            out.children.push_back(std::move(c));
        }
    }
    if (const VisusMetadata* m = root.get_object("metadata")) out.metadata = *m;
    packages_from_value(root, "", out.packages, warnings);
    return true;
}

bool from_visus_json(const std::string&        json,
                     VisusDesc&                out,
                     std::string*              error,
                     std::vector<std::string>* warnings) {
    VisusValue root;
    if (!visus_json::parse(json, root, error)) return false;
    return visus_desc_from_value(root, out, error, warnings);
}

} // namespace pictor
