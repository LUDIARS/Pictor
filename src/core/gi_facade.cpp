#include "pictor/core/gi_facade.h"
#include "pictor/core/renderer_subsystem_manager.h"

namespace pictor {

void GIFacade::set_directional_light(const DirectionalLight& light) {
    if (auto* gi = subsystems_.gi_system()) gi->set_directional_light(light);
}

void GIFacade::upload_gi_probe_data(const float* sh_data, uint32_t probe_count) {
    if (auto* gi = subsystems_.gi_system()) gi->upload_probe_data(sh_data, probe_count);
}

void GIFacade::set_gi_config(const GIConfig& config) {
    if (auto* gi = subsystems_.gi_system()) gi->set_config(config);
}

GIBakeResult GIFacade::bake_static_gi() {
    auto* bake = subsystems_.bake_system();
    if (!bake) return GIBakeResult{};
    auto result = bake->bake();
    result.bake_timestamp = frame_number_;
    return result;
}

GIBakeResult GIFacade::bake_static_gi(BakeProgressCallback progress) {
    auto* bake = subsystems_.bake_system();
    if (!bake) return GIBakeResult{};
    auto result = bake->bake(std::move(progress));
    result.bake_timestamp = frame_number_;
    return result;
}

void GIFacade::apply_bake(const GIBakeResult& result) {
    auto* bake = subsystems_.bake_system();
    if (!bake) return;
    bake->apply(result);

    // Notify GI system how many static objects have baked data
    if (auto* gi = subsystems_.gi_system()) {
        gi->set_baked_static_count(static_cast<uint32_t>(result.object_ids.size()));
    }
}

void GIFacade::invalidate_bake() {
    if (auto* bake = subsystems_.bake_system()) bake->invalidate();
    if (auto* gi = subsystems_.gi_system()) gi->set_baked_static_count(0);
}

bool GIFacade::save_bake(const std::string& path, const GIBakeResult& result) {
    auto* bake = subsystems_.bake_system();
    return bake ? bake->save(path, result) : false;
}

GIBakeResult GIFacade::load_bake(const std::string& path) {
    auto* bake = subsystems_.bake_system();
    return bake ? bake->load(path) : GIBakeResult{};
}

void GIFacade::set_bake_data_provider(IBakeDataProvider* provider) {
    if (auto* bake = subsystems_.bake_system()) bake->set_bake_data_provider(provider);
}

} // namespace pictor
