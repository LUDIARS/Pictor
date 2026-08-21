#include "pictor/visus/visus_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pictor {

namespace {

void warn(std::vector<std::string>* w, std::string msg) {
    if (w) w->push_back(std::move(msg));
}

// `shader.key_override` — part metadata が visus metadata に優先。
// ShaderKey の上位 32bit は CUSTOM フラグ + ShaderHandle の予約領域なので、
// metadata が渡せるのは下位 32bit のみ。 `with_custom_shader` は shader が
// INVALID のとき base をそのまま返すため、 ここで刈らないと untrusted な
// visus ファイルが「解決済みカスタムシェーダ」を捏造できてしまう
// (visus_base_descriptor と同じ不変条件)。
uint64_t key_override(const VisusMetadata& part_meta, const VisusMetadata& visus_meta) {
    constexpr double kMax = static_cast<double>(std::numeric_limits<uint32_t>::max());
    for (const VisusMetadata* m : {&part_meta, &visus_meta}) {
        const auto v = m->get_number(visus_keys::kShaderKeyOverride);
        if (v && std::isfinite(*v) && *v >= 0.0 && *v <= kMax &&
            std::trunc(*v) == *v)
            return static_cast<uint64_t>(*v);
    }
    return 0;
}

// 解決済みパスに置き換えた STAGES 参照を作る。
VisusShaderRef resolve_stage_paths(const VisusCatalog& catalog, const VisusDesc& owner,
                                   const VisusShaderRef& ref) {
    VisusShaderRef out = ref;
    if (!ref.vert.empty()) out.vert = catalog.resolve_path(owner, ref.vert);
    if (!ref.frag.empty()) out.frag = catalog.resolve_path(owner, ref.frag);
    if (!ref.comp.empty()) out.comp = catalog.resolve_path(owner, ref.comp);
    return out;
}

bool contains_model_part(const std::vector<VisusModelPart>& parts, std::string_view name) {
    for (const VisusModelPart& part : parts) {
        if (part.name == name) return true;
    }
    return false;
}

} // namespace

void VisusRuntime::resolve_model_(const VisusCatalog& catalog,
                                  const VisusDesc& desc,
                                  const std::string& asset,
                                  IVisusResolver& resolver,
                                  VisusResolved& out,
                                  std::vector<std::string>* warnings,
                                  const VisusPackageCatalog* packages) {
    std::string err;
    if (asset.empty()) return;

    out.model = resolver.load_model(asset, &err);
    if (out.model == INVALID_MODEL) {
        if (!err.empty()) warn(warnings, desc.name + ": " + err);
        return;
    }

    err.clear();
    std::vector<VisusModelPart> model_parts = resolver.model_parts(out.model, &err);
    if (model_parts.empty()) {
        warn(warnings, desc.name + ": " +
             (err.empty() ? "model has no drawable parts" : err));
        return;
    }

    out.parts.reserve(model_parts.size());
    for (const VisusModelPart& model_part : model_parts) {
        if (model_part.mesh == INVALID_MESH) {
            warn(warnings, desc.name + "." + model_part.name + ": model part has no mesh");
            continue;
        }

        VisusResolvedPart rp;
        rp.part = model_part.name;
        rp.mesh = model_part.mesh;

        const VisusPart* configured = desc.find_part(model_part.name);
        const VisusMetadata empty_metadata;
        const VisusMetadata& metadata = configured ? configured->metadata : empty_metadata;
        const VisusShaderRef shader = configured ? configured->shader
                                                 : VisusShaderRef::builtin();
        rp.shader = resolve_shader_ref_(catalog, desc, shader, metadata,
                                        resolver, 0, warnings);
        if (const auto mat = metadata.get_string(visus_keys::kMaterial)) {
            err.clear();
            rp.material = resolver.load_material(catalog.resolve_path(desc, *mat), &err);
            if (rp.material == INVALID_MATERIAL && !err.empty())
                warn(warnings, desc.name + "." + model_part.name + ": " + err);
        }
        rp.shader_key = ShaderKey::with_custom_shader(
            key_override(metadata, desc.metadata), rp.shader);
        // base の上に重ね掛けする実効パッケージ列 (§2.5.3)。
        const std::vector<VisusPackageRef> empty_refs;
        resolve_packages_(catalog, desc,
                          configured ? configured->packages : empty_refs,
                          resolver, packages, rp.packages, warnings);
        out.parts.push_back(std::move(rp));
    }
    for (const VisusPart& configured : desc.parts) {
        if (!configured.is_wildcard() && !contains_model_part(model_parts, configured.part))
            warn(warnings, desc.name + "." + configured.part +
                           ": configured part not found in model");
    }
}

// ---- VisusResolved -----------------------------------------------------------

const VisusResolvedPart* VisusResolved::find_part(std::string_view part) const {
    for (const VisusResolvedPart& p : parts) {
        if (p.part == part) return &p;
    }
    return nullptr;
}

// ---- VisusRuntime ------------------------------------------------------------

const VisusResolved* VisusRuntime::get(std::string_view name) const {
    auto it = resolved_.find(name);
    return it == resolved_.end() ? nullptr : &it->second;
}

void VisusRuntime::set(VisusResolved resolved) {
    std::string key = resolved.name;
    resolved_.insert_or_assign(std::move(key), std::move(resolved));
}

bool VisusRuntime::invalidate(std::string_view name) {
    auto it = resolved_.find(name);
    if (it == resolved_.end()) return false;
    // Children and `visus:` shader entries can be shared by several roots.
    // Without a reverse dependency graph, erasing just this entry would let a
    // later root resolve reuse stale dependent handles. Clear atomically so
    // the next resolve rebuilds one coherent generation of the side-table.
    resolved_.clear();
    return true;
}

bool VisusRuntime::resolve(const VisusCatalog&        catalog,
                           std::string_view           name,
                           IVisusResolver&            resolver,
                           std::vector<std::string>*  warnings,
                           const VisusPackageCatalog* packages) {
    const VisusDesc* desc = catalog.find(name);
    if (!desc) {
        warn(warnings, "visus not found: " + std::string(name));
        return false;
    }
    std::vector<std::string> stack;
    std::map<std::string, uint16_t, std::less<>> visited_depths;
    return resolve_rec_(catalog, *desc, resolver, 0, stack, visited_depths, warnings, packages);
}

bool VisusRuntime::resolve_rec_(const VisusCatalog& catalog, const VisusDesc& desc,
                                IVisusResolver& resolver, int depth,
                                std::vector<std::string>& stack,
                                std::map<std::string, uint16_t, std::less<>>& visited_depths,
                                std::vector<std::string>* warnings,
                                const VisusPackageCatalog* packages) {
    for (const std::string& s : stack) {
        if (s == desc.name) {
            warn(warnings, "visus cycle at: " + desc.name);
            return false;
        }
    }

    // A catalog is a graph, not a tree: the same child may be referenced from
    // many parents. Skipping only `resolve_one_` still re-walks the subtree once
    // per path, so a shared subtree costs O(branching^depth) visits and emits the
    // same warnings that many times. Visit each (name, depth) state once, exactly
    // like VisusCatalog::validate_node_. Depth is bounded to kMaxChildDepth + 1,
    // so a uint16_t bitset is sufficient.
    const int bounded_depth = std::min(depth, VisusCatalog::kMaxChildDepth + 1);
    const uint16_t depth_bit = static_cast<uint16_t>(uint16_t{1} << bounded_depth);
    uint16_t& visited = visited_depths[desc.name];
    if ((visited & depth_bit) != 0) return is_resolved(desc.name);

    if (depth > VisusCatalog::kMaxChildDepth) {
        warn(warnings, "visus nesting too deep at: " + desc.name);
        visited = static_cast<uint16_t>(visited | depth_bit);
        return false;
    }

    bool ok = true;
    if (!is_resolved(desc.name)) {
        ok = resolve_one_(catalog, desc, resolver, warnings, packages);
    }

    stack.push_back(desc.name);
    for (const VisusChildRef& c : desc.children) {
        std::string err;
        const VisusDesc* child = catalog.resolve_child(desc, c, &err);
        if (!child) {
            warn(warnings, desc.name + ": " + err);
            continue;
        }
        resolve_rec_(catalog, *child, resolver, depth + 1, stack, visited_depths, warnings, packages);
    }
    stack.pop_back();
    visited = static_cast<uint16_t>(visited | depth_bit);
    return ok;
}

ShaderHandle VisusRuntime::resolve_shader_ref_(const VisusCatalog& catalog,
                                               const VisusDesc& owner,
                                               const VisusShaderRef& ref,
                                               const VisusMetadata& metadata,
                                               IVisusResolver& resolver, int depth,
                                               std::vector<std::string>* warnings) {
    switch (ref.kind) {
        case VisusShaderRef::Kind::BUILTIN: {
            if (ref.is_default_builtin()) return INVALID_SHADER;
            const ShaderHandle h = resolver.builtin_shader(ref.name);
            if (h == INVALID_SHADER)
                warn(warnings, owner.name + ": builtin shader unavailable: " + ref.name);
            return h;
        }

        case VisusShaderRef::Kind::STAGES: {
            if (!ref.has_graphics_stages()) {
                warn(warnings, owner.name + ": shader stages need both vert and frag");
                return INVALID_SHADER;
            }
            const VisusShaderRef resolved = resolve_stage_paths(catalog, owner, ref);
            if (!resolved.has_graphics_stages()) {
                // 元は vert/frag が揃っていたが、 resolve_path が不正パスで
                // 空を返した。 空パスを resolver へ渡さない。
                warn(warnings, owner.name + ": shader stage path could not be resolved");
                return INVALID_SHADER;
            }
            std::string err;
            const ShaderHandle h = resolver.register_shader_stages(resolved, metadata, &err);
            if (h == INVALID_SHADER && !err.empty()) warn(warnings, owner.name + ": " + err);
            return h;
        }

        case VisusShaderRef::Kind::VISUS: {
            // 別 Visus (kind=custom) をシェーダ定義として共有。 1 段だけ辿る
            // (visus: → visus: の連鎖は許さない)。
            if (depth > 0) {
                warn(warnings, owner.name + ": nested visus: shader reference not allowed (" + ref.name + ")");
                return INVALID_SHADER;
            }
            // 解決済みでも kind=custom 以外はシェーダ定義として使えない。
            // cache hit を先に返すと、 参照先が先に解決された順序のときだけ
            // 診断が消えて INVALID_SHADER が黙って返る。
            if (const VisusResolved* cached = get(ref.name)) {
                if (cached->kind == VisusKind::CUSTOM) return cached->shader;
                warn(warnings, owner.name + ": shader visus '" + ref.name + "' is not kind=custom");
                return INVALID_SHADER;
            }
            const VisusDesc* target = catalog.find(ref.name);
            if (!target) {
                warn(warnings, owner.name + ": shader visus not found: " + ref.name);
                return INVALID_SHADER;
            }
            if (target->kind != VisusKind::CUSTOM) {
                warn(warnings, owner.name + ": shader visus '" + ref.name + "' is not kind=custom");
                return INVALID_SHADER;
            }
            // 参照先の metadata["shader"] (STAGES / builtin のみ) を登録し、
            // 参照先自身も side-table に入れて共有する。
            VisusShaderRef tref;
            const VisusValue* tsv = target->metadata.find(visus_keys::kShader);
            if (!tsv || !visus_shader_ref_from_value(*tsv, tref)) {
                warn(warnings, owner.name + ": shader visus '" + ref.name + "' has no metadata[\"shader\"]");
                return INVALID_SHADER;
            }
            const ShaderHandle h = resolve_shader_ref_(catalog, *target, tref, target->metadata,
                                                       resolver, depth + 1, warnings);
            VisusResolved tr;
            tr.name       = target->name;
            tr.kind       = VisusKind::CUSTOM;
            tr.shader     = h;
            tr.shader_key = ShaderKey::with_custom_shader(
                key_override(VisusMetadata{}, target->metadata), h);
            set(std::move(tr));
            return h;
        }
    }
    return INVALID_SHADER;
}

bool VisusRuntime::resolve_one_(const VisusCatalog& catalog, const VisusDesc& desc,
                                IVisusResolver& resolver,
                                std::vector<std::string>* warnings,
                                const VisusPackageCatalog* packages) {
    VisusResolved r;
    r.name = desc.name;
    r.kind = desc.kind;
    std::string err;

    const std::string asset = desc.asset.empty() ? std::string{}
                                                 : catalog.resolve_path(desc, desc.asset);
    if (!desc.asset.empty() && asset.empty()) {
        // resolve_path は不正パス (NUL / filesystem 非対応) で空を返す。
        // 黙って「アセット無し」に落とすと透明オブジェクトになるため警告。
        warn(warnings, desc.name + ": asset path could not be resolved: " + desc.asset);
    }

    switch (desc.kind) {
        case VisusKind::PRIMITIVE:
            if (!asset.empty()) {
                r.mesh = resolver.load_mesh(asset, &err);
                if (r.mesh == INVALID_MESH && !err.empty()) warn(warnings, desc.name + ": " + err);
            }
            // primitive は part を持たないので、 material は visus metadata から。
            // v1 変換は非 model の materials[] をここへ落とす (visus_v1_compat)。
            if (const auto mat = desc.metadata.get_string(visus_keys::kMaterial)) {
                err.clear();
                r.material = resolver.load_material(catalog.resolve_path(desc, *mat), &err);
                if (r.material == INVALID_MATERIAL && !err.empty())
                    warn(warnings, desc.name + ": " + err);
            }
            break;

        case VisusKind::MODEL: {
            resolve_model_(catalog, desc, asset, resolver, r, warnings, packages);
            break;
        }

        case VisusKind::CUSTOM: {
            VisusShaderRef ref;
            const VisusValue* sv = desc.metadata.find(visus_keys::kShader);
            if (!sv || !visus_shader_ref_from_value(*sv, ref)) {
                warn(warnings, desc.name + ": kind=custom without metadata[\"shader\"]");
            } else {
                r.shader = resolve_shader_ref_(catalog, desc, ref, desc.metadata,
                                               resolver, 0, warnings);
            }
            break;
        }

        case VisusKind::RIVE:
        case VisusKind::UI:
        case VisusKind::PARTICLE:
        case VisusKind::TEXT:
            if (!asset.empty()) {
                r.generic_handle = resolver.load_generic(desc.kind, asset, &err);
                if (r.generic_handle == 0 && !err.empty()) warn(warnings, desc.name + ": " + err);
            }
            break;

        case VisusKind::GROUP:
        case VisusKind::NONE:
            break;
    }

    r.shader_key = ShaderKey::with_custom_shader(key_override(VisusMetadata{}, desc.metadata),
                                                 r.shader);
    // MODEL は part ごとに実効列を持つ (resolve_model_)。 PRIMITIVE は
    // Visus 直下のアサインをそのまま重ね掛け列として持つ。 それ以外の
    // kind は instantiate 側 (register_packages) に重ね掛けパスの登録先が
    // 無いため、 黙って解決 (= shader handle 消費) せず警告して無視する。
    if (desc.kind == VisusKind::PRIMITIVE) {
        const std::vector<VisusPackageRef> no_part_level;
        resolve_packages_(catalog, desc, no_part_level, resolver, packages,
                          r.packages, warnings);
    } else if (desc.kind != VisusKind::MODEL && !desc.packages.empty()) {
        warn(warnings, desc.name +
                       ": shader_packages are only supported on kind=model/"
                       "primitive — ignored");
    }
    set(std::move(r));
    return true;
}

// ---- シェーダーパッケージ (§2.5) ---------------------------------------------

ShaderHandle VisusRuntime::resolve_package_shader_(const VisusCatalog& catalog,
                                                   const VisusDesc& owner,
                                                   const VisusPackageCatalog& packages,
                                                   const VisusShaderPackage& pkg,
                                                   const VisusMetadata& metadata,
                                                   IVisusResolver& resolver,
                                                   std::vector<std::string>* warnings) {
    switch (pkg.shader.kind) {
        case VisusShaderRef::Kind::BUILTIN:
            return pkg.shader.is_default_builtin() ? INVALID_SHADER
                                                   : resolver.builtin_shader(pkg.shader.name);

        case VisusShaderRef::Kind::STAGES: {
            if (!pkg.shader.has_graphics_stages()) {
                warn(warnings, owner.name + " (" + pkg.name +
                               "): shader package needs both vert and frag");
                return INVALID_SHADER;
            }
            // stage パスはパッケージファイル起点で解決する (§2.5.1)。
            VisusShaderRef ref = pkg.shader;
            if (!ref.vert.empty()) ref.vert = packages.resolve_path(pkg.name, ref.vert);
            if (!ref.frag.empty()) ref.frag = packages.resolve_path(pkg.name, ref.frag);
            if (!ref.comp.empty()) ref.comp = packages.resolve_path(pkg.name, ref.comp);
            if (!ref.has_graphics_stages()) {
                warn(warnings, owner.name + " (" + pkg.name +
                               "): shader stage path could not be resolved");
                return INVALID_SHADER;
            }
            std::string err;
            const ShaderHandle h = resolver.register_shader_stages(ref, metadata, &err);
            if (h == INVALID_SHADER && !err.empty())
                warn(warnings, owner.name + " (" + pkg.name + "): " + err);
            return h;
        }

        case VisusShaderRef::Kind::VISUS:
            // visus:<name> は Visus カタログ側の kind=custom を参照する。
            return resolve_shader_ref_(catalog, owner, pkg.shader, metadata,
                                       resolver, 0, warnings);
    }
    return INVALID_SHADER;
}

void VisusRuntime::resolve_packages_(const VisusCatalog& catalog, const VisusDesc& desc,
                                     const std::vector<VisusPackageRef>& part_level,
                                     IVisusResolver& resolver,
                                     const VisusPackageCatalog* packages,
                                     std::vector<VisusResolvedPackage>& out,
                                     std::vector<std::string>* warnings) {
    const std::vector<VisusPackageRef> effective =
        visus_effective_packages(desc.packages, part_level);
    if (effective.empty()) return;
    if (!packages) {
        warn(warnings, desc.name + ": shader packages assigned but no package catalog given");
        return;
    }

    out.reserve(effective.size());
    for (const VisusPackageRef& ref : effective) {
        const VisusShaderPackage* pkg = packages->find(ref.package);
        if (!pkg) {
            warn(warnings, desc.name + ": shader package not found: " + ref.package);
            continue;
        }
        VisusResolvedPackage rp;
        rp.package  = ref.package;
        rp.params   = visus_package_effective_params(*pkg, ref);
        rp.metadata = visus_package_effective_metadata(*pkg, ref);
        rp.shader   = resolve_package_shader_(catalog, desc, *packages, *pkg,
                                              rp.metadata, resolver, warnings);
        // シェーダが解決できなかったパッケージはパスごと落とす。 積むと
        // instantiate 側 (register_packages) が base と同一の
        // ObjectDescriptor を登録し、 同じ mesh が base pipeline で二重描画
        // される (深度/アルファが壊れる)。 既定 builtin (= 明示的に既定
        // pipeline を使うアサイン) は失敗ではないので通す。
        if (rp.shader == INVALID_SHADER && !pkg->shader.is_default_builtin()) {
            warn(warnings, desc.name + ": shader package shader unresolved, pass dropped: " +
                           ref.package);
            continue;
        }
        rp.shader_key = ShaderKey::with_custom_shader(
            key_override(rp.metadata, desc.metadata), rp.shader);
        out.push_back(std::move(rp));
    }
}

} // namespace pictor
