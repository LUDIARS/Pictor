#include "pictor/visus/visus_package_serializer.h"

#include "visus_json.h"

namespace pictor {

namespace {

constexpr double kPackageVersion = 1.0;

void warn(std::vector<std::string>* warnings, std::string msg) {
    if (warnings) warnings->push_back(std::move(msg));
}

} // namespace

// ---- VisusPackageRef ---------------------------------------------------------

bool visus_package_ref_from_value(const VisusValue&         value,
                                  VisusPackageRef&          out,
                                  std::vector<std::string>* warnings) {
    if (value.is_string()) {
        out = VisusPackageRef{};
        out.package = value.as_string();
        if (out.package.empty()) {
            warn(warnings, "shader_packages[]: empty package name ignored");
            return false;
        }
        return true;
    }
    if (!value.is_object()) {
        warn(warnings, "shader_packages[]: entry must be a string or an object");
        return false;
    }

    const VisusMetadata& o = value.as_object();
    out = VisusPackageRef{};
    out.package = o.get_string("package").value_or("");
    if (out.package.empty()) {
        warn(warnings, "shader_packages[]: entry without 'package' ignored");
        return false;
    }
    if (const auto enabled = o.get_bool("enabled")) out.enabled = *enabled;
    if (const VisusMetadata* p = o.get_object("params"))   out.params   = *p;
    if (const VisusMetadata* m = o.get_object("metadata")) out.metadata = *m;
    return true;
}

VisusValue visus_package_ref_to_value(const VisusPackageRef& ref) {
    const bool plain = ref.enabled && ref.params.empty() && ref.metadata.empty();
    if (plain) return VisusValue(ref.package);

    VisusMetadata o;
    o.set("package", ref.package);
    if (!ref.enabled) o.set("enabled", false);
    if (!ref.params.empty())   o.set("params", VisusValue(ref.params));
    if (!ref.metadata.empty()) o.set("metadata", VisusValue(ref.metadata));
    return VisusValue(std::move(o));
}

// ---- VisusShaderPackage ------------------------------------------------------

VisusValue visus_package_to_value(const VisusShaderPackage& pkg) {
    VisusMetadata root;
    root.set("version", kPackageVersion);
    root.set("name", pkg.name);
    root.set("shader", visus_shader_ref_to_value(pkg.shader));
    root.set("params", VisusValue(pkg.params));
    root.set("metadata", VisusValue(pkg.metadata));
    return VisusValue(std::move(root));
}

bool visus_package_from_value(const VisusValue&         value,
                              VisusShaderPackage&       out,
                              std::string*              error,
                              std::vector<std::string>* warnings) {
    if (!value.is_object()) {
        if (error) *error = "shader package root must be an object";
        return false;
    }
    const VisusMetadata& root = value.as_object();

    const VisusValue* version_value = root.find("version");
    if (version_value && !version_value->is_number()) {
        if (error) *error = "shader package version must be a number";
        return false;
    }
    const double version = version_value ? version_value->as_number() : kPackageVersion;
    if (version < 1.0 || version > kPackageVersion) {
        if (error) *error = "unsupported shader package version";
        return false;
    }

    out = VisusShaderPackage{};
    out.name = root.get_string("name").value_or("");
    if (const VisusValue* s = root.find("shader")) {
        if (!visus_shader_ref_from_value(*s, out.shader)) {
            warn(warnings, "shader: unrecognized shader reference, using builtin:pbr");
            out.shader = VisusShaderRef::builtin();
        }
    }
    if (const VisusMetadata* p = root.get_object("params"))   out.params   = *p;
    if (const VisusMetadata* m = root.get_object("metadata")) out.metadata = *m;
    return true;
}

std::string to_shader_package_json(const VisusShaderPackage& pkg) {
    return visus_json::emit(visus_package_to_value(pkg));
}

bool from_shader_package_json(const std::string&        json,
                              VisusShaderPackage&       out,
                              std::string*              error,
                              std::vector<std::string>* warnings) {
    VisusValue root;
    if (!visus_json::parse(json, root, error)) return false;
    return visus_package_from_value(root, out, error, warnings);
}

} // namespace pictor
