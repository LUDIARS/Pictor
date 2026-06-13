#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/culling/culling_system.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/render_pass_scheduler.h"
#include "pictor/profiler/profiler.h"
#include "pictor/profiler/overlay_renderer.h"
#include "pictor/profiler/stats_overlay.h"
#include "pictor/profiler/data_exporter.h"
#include "pictor/data/data_handler.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"
#include "pictor/animation/animation_system.h"
#include <memory>

namespace pictor {

struct RendererConfig; // pictor_renderer.h

/// 全サブシステムの所有・構築順序・プロファイル再構成を一手に引き受ける
/// (review/2026-06-11 D-1: PictorRenderer God Class からの所有/初期化順序の切り出し)。
///
/// 初期化順序の依存 (旧 PictorRenderer::initialize のコメントでしか守られて
/// いなかった 18 段) を `initialize()` の手続きに閉じ込め、 破棄は逆順で行う。
/// PictorRenderer はこのマネージャを所有し、 本体コードからは各 accessor 越しに
/// サブシステムへアクセスする。
class RendererSubsystemManager {
public:
    RendererSubsystemManager();
    ~RendererSubsystemManager();

    RendererSubsystemManager(const RendererSubsystemManager&) = delete;
    RendererSubsystemManager& operator=(const RendererSubsystemManager&) = delete;

    /// 18 サブシステムを依存順に構築する (旧 initialize の手続きそのまま)。
    void initialize(const RendererConfig& config);

    /// 構築の逆順で破棄する。
    void shutdown();

    /// §8.4 プロファイル切替: 下流サブシステムを再構成する。
    /// gpu_pipeline / gi_system / bake_system を profile に応じて生成/破棄するため、
    /// 呼び出し後はこれらの accessor が指す実体が変わり得る (呼び出し側で再取得)。
    void apply_profile(const PipelineProfileDef& profile);

    // ---- accessors (非所有、 null 可なものは null を返し得る) ----
    MemorySubsystem*        memory()             { return memory_.get(); }
    SceneRegistry*          scene()              { return scene_.get(); }
    UpdateScheduler*        update_scheduler()   { return update_scheduler_.get(); }
    BatchBuilder*           batch_builder()      { return batch_builder_.get(); }
    CullingSystem*          culling()            { return culling_.get(); }
    GPUBufferManager*       gpu_buffer_manager() { return gpu_buffer_manager_.get(); }
    GPUDrivenPipeline*      gpu_pipeline()       { return gpu_pipeline_.get(); }
    PipelineProfileManager* profile_manager()    { return profile_manager_.get(); }
    RenderPassScheduler*    pass_scheduler()     { return pass_scheduler_.get(); }
    Profiler*               profiler()           { return profiler_.get(); }
    OverlayRenderer*        overlay()            { return overlay_.get(); }
    StatsOverlay*           stats_overlay()      { return stats_overlay_.get(); }
    DataExporter*           data_exporter()      { return data_exporter_.get(); }
    DataHandler*            data_handler()       { return data_handler_.get(); }
    GILightingSystem*       gi_system()          { return gi_system_.get(); }
    GIBakeSystem*           bake_system()        { return bake_system_.get(); }
    AnimationSystem*        animation_system()   { return animation_system_.get(); }

private:
    std::unique_ptr<MemorySubsystem>        memory_;
    std::unique_ptr<SceneRegistry>          scene_;
    std::unique_ptr<UpdateScheduler>        update_scheduler_;
    std::unique_ptr<BatchBuilder>           batch_builder_;
    std::unique_ptr<CullingSystem>          culling_;
    std::unique_ptr<GPUBufferManager>       gpu_buffer_manager_;
    std::unique_ptr<GPUDrivenPipeline>      gpu_pipeline_;
    std::unique_ptr<PipelineProfileManager> profile_manager_;
    std::unique_ptr<RenderPassScheduler>    pass_scheduler_;
    std::unique_ptr<Profiler>               profiler_;
    std::unique_ptr<OverlayRenderer>        overlay_;
    std::unique_ptr<StatsOverlay>           stats_overlay_;
    std::unique_ptr<DataExporter>           data_exporter_;
    std::unique_ptr<DataHandler>            data_handler_;
    std::unique_ptr<GILightingSystem>       gi_system_;
    std::unique_ptr<GIBakeSystem>           bake_system_;
    std::unique_ptr<AnimationSystem>        animation_system_;

    // apply_profile が screen_width/height を要するため保持。
    uint32_t screen_width_  = 0;
    uint32_t screen_height_ = 0;
};

} // namespace pictor
