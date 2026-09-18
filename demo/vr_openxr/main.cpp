// Pictor VR demo — 部屋の椅子に座って周囲を見渡す。
//
//   pictor_vr_openxr_demo [preset]
//     preset: vr.multiview / vr.two_pass / vr.eye_delta / vr.film_delta (既定)
//
// 流れ: OpenXR ランタイムへ接続 → その要求どおりに Vulkan を初期化 → セッション開始
//       → 毎フレーム: 姿勢取得 → 差分描画の計画 → 計画どおりに描画 → 提出。
// 鏡表示 (PC 画面への表示) はしない。 ウィンドウは Vulkan の初期化に必要なだけ。
// GPU の完了をフレームごとに待つ単純な同期にしてある (demo の読みやすさ優先)。

#include "vr_eye_targets.h"
#include "vr_scene_renderer.h"

#include "pictor/core/transform_math.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/xr/delta_render_planner.h"
#include "pictor/xr/openxr_runtime.h"
#include "pictor/xr/reprojection.h"
#include "pictor/xr/reprojection_pass.h"
#include "pictor/xr/stereo_camera.h"
#include "pictor/xr/stereo_presets.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace pictor;
using namespace pictor::xr;
using vr_demo::VrSceneRenderer;

namespace {

constexpr float kSeatedHeadHeight = 1.2f;  // 椅子に座った頭の高さ (床から、 メートル)
constexpr uint32_t kStatsInterval = 300;   // 何フレームごとに穴の比率を出すか

/// 1 フレームぶんのコマンドと同期。 フレームごとに GPU の完了を待つ。
struct FrameCommands {
    VkCommandPool   pool   = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkFence         fence  = VK_NULL_HANDLE;
    VkQueryPool     holes  = VK_NULL_HANDLE;  // 眼ごとの「穴埋めで描いた画素数」

    bool create(VkDevice device, uint32_t queue_family) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        if (vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &alloc, &buffer) != VK_SUCCESS) return false;

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) return false;

        VkQueryPoolCreateInfo query_info{};
        query_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType  = VK_QUERY_TYPE_OCCLUSION;
        query_info.queryCount = kEyeCount;
        return vkCreateQueryPool(device, &query_info, nullptr, &holes) == VK_SUCCESS;
    }

    void destroy(VkDevice device) {
        if (holes) vkDestroyQueryPool(device, holes, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool)  vkDestroyCommandPool(device, pool, nullptr);  // buffer も一緒に解放される
        *this = FrameCommands{};
    }
};

void begin_pass(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, VkExtent2D extent) {
    VkClearValue clears[2]{};
    clears[0].color        = {{0.02f, 0.02f, 0.03f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo info{};
    info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass        = pass;
    info.framebuffer       = fb;
    info.renderArea.extent = extent;
    info.clearValueCount   = 2;
    info.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
}

/// demo の描画一式。 計画 (FramePlan) を Vulkan のコマンドへ落とす。
struct VrFrameRenderer {
    vr_demo::VrEyeTargets& targets;
    VrSceneRenderer&       scene;
    ReprojectionPass&      reprojection;
    uint32_t               keyframe_slot[kEyeCount];
    /// 画素数を正確に数えられるデバイスなら PRECISE。 そうでなければ 0 (数は信用しない)。
    VkQueryControlFlags    query_flags;

    void reproject(VkCommandBuffer cmd, uint32_t source_eye, const Camera& source,
                   const Camera& destination) const {
        float4x4 matrix;
        // 再投影元が壊れていたら写さない。 続く静的層の描画が全面を埋める。
        if (!build_reprojection(source, destination, matrix)) return;
        ReprojectionParams params;
        params.dst_width  = targets.extent().width;
        params.dst_height = targets.extent().height;
        reprojection.record(cmd, keyframe_slot[source_eye], matrix, params);
    }

    /// 静的層をキーフレームへ描く (取り直し)。
    void capture_keyframe(VkCommandBuffer cmd, uint32_t eye, const FramePlan& plan,
                          const Camera& camera, const float4x4& view_proj) const {
        begin_pass(cmd, targets.keyframe_pass(), targets.keyframe_framebuffer(eye), targets.extent());
        if (plan.eyes[eye].source == StaticLayerSource::OtherEyeKeyframe) {
            reproject(cmd, 1 - eye, plan.source_camera[eye], camera);
        }
        scene.draw(cmd, VrSceneRenderer::Layer::Static, view_proj, targets.extent());
        vkCmdEndRenderPass(cmd);
    }

    void draw_eye(VkCommandBuffer cmd, VkQueryPool holes, uint32_t image_index, uint32_t eye,
                  const FramePlan& plan, const Camera& camera) const {
        const float4x4 view_proj = transform_math::multiply(camera.view, camera.projection);
        const EyePlan& eye_plan  = plan.eyes[eye];
        if (eye_plan.capture_keyframe) capture_keyframe(cmd, eye, plan, camera, view_proj);

        begin_pass(cmd, targets.eye_pass(), targets.eye_framebuffer(image_index, eye),
                   targets.extent());
        if (eye_plan.capture_keyframe) {
            // 取り直したばかりのキーフレームは今の視点そのものなので、 そのまま写る。
            reproject(cmd, eye, camera, camera);
        } else if (eye_plan.source == StaticLayerSource::OwnKeyframe) {
            reproject(cmd, eye, plan.source_camera[eye], camera);
        } else if (eye_plan.source == StaticLayerSource::OtherEyeKeyframe) {
            reproject(cmd, 1 - eye, plan.source_camera[eye], camera);
        }
        // 再投影した眼では、 ここで描かれるのは穴の画素だけ。 その数を数える。
        vkCmdBeginQuery(cmd, holes, eye, query_flags);
        scene.draw(cmd, VrSceneRenderer::Layer::Static, view_proj, targets.extent());
        vkCmdEndQuery(cmd, holes, eye);
        scene.draw(cmd, VrSceneRenderer::Layer::Dynamic, view_proj, targets.extent());
        vkCmdEndRenderPass(cmd);
    }

    void draw_multiview(VkCommandBuffer cmd, uint32_t image_index, const StereoCamera& camera) const {
        const float4x4 view_proj[kEyeCount] = {
            transform_math::multiply(camera.eye(Eye::Left).view, camera.eye(Eye::Left).projection),
            transform_math::multiply(camera.eye(Eye::Right).view, camera.eye(Eye::Right).projection),
        };
        begin_pass(cmd, targets.multiview_pass(), targets.multiview_framebuffer(image_index),
                   targets.extent());
        scene.draw_multiview(cmd, VrSceneRenderer::Layer::Static, view_proj, targets.extent());
        scene.draw_multiview(cmd, VrSceneRenderer::Layer::Dynamic, view_proj, targets.extent());
        vkCmdEndRenderPass(cmd);
    }
};

bool submit_and_wait(VkDevice device, VkQueue queue, FrameCommands& frame) {
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &frame.buffer;
    if (vkQueueSubmit(queue, 1, &submit, frame.fence) != VK_SUCCESS) return false;
    if (vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    return vkResetFences(device, 1, &frame.fence) == VK_SUCCESS;
}

int run(const StereoPreset& preset) {
    std::string error;
    std::unique_ptr<OpenXrRuntime> runtime = OpenXrRuntime::create({"Pictor VR demo", 1}, error);
    if (!runtime) {
        std::fprintf(stderr, "[vr_demo] %s\n", error.c_str());
        return 1;
    }

    GlfwSurfaceProvider window;
    GlfwWindowConfig window_config;
    window_config.width  = 640;
    window_config.height = 360;
    window_config.title  = "Pictor VR demo (HMD に表示中)";
    if (!window.create(window_config)) {
        std::fprintf(stderr, "[vr_demo] failed to create the window\n");
        return 1;
    }

    VulkanContext vk;
    VulkanContextConfig vk_config;
    vk_config.app_name            = "Pictor VR demo";
    vk_config.device_requirements = runtime.get();
    vk_config.require_multiview   = preset.requires_multiview;
    if (!vk.initialize(&window, vk_config)) {
        std::fprintf(stderr, "[vr_demo] failed to initialize Vulkan for the HMD\n");
        return 1;
    }

    int exit_code = 1;
    FrameCommands frame;
    {
        // このブロックの資源は VkDevice より先に壊す (宣言の逆順に解放される)。
        vr_demo::VrEyeTargets targets;
        VrSceneRenderer       scene;
        ReprojectionPass      reprojection;

        OpenXrGraphicsBinding binding;
        binding.instance        = vk.instance();
        binding.physical_device = vk.physical_device();
        binding.device          = vk.device();
        binding.queue_family    = vk.queue_family();

        const std::string shader_dir = "shaders/xr";
        ReprojectionPassDesc reprojection_desc;
        reprojection_desc.device     = vk.device();
        reprojection_desc.shader_dir = shader_dir;

        const bool is_ready =
            runtime->start_session(binding, error) &&
            targets.initialize(vk.physical_device(), vk.device(), *runtime->surface(),
                               preset.requires_multiview) &&
            scene.initialize(vk.physical_device(), vk.device(), targets.eye_pass(),
                             targets.multiview_pass(), "shaders/vr_demo") &&
            (reprojection_desc.render_pass = targets.eye_pass(),
             reprojection.initialize(reprojection_desc)) &&
            frame.create(vk.device(), vk.queue_family());

        // 画素数を正確に数えられないデバイスでは、 穴の比率を planner へ戻さない
        // (不正確な数で全面描画へ戻す判断をさせないため)。 描画自体は成り立つ。
        VkPhysicalDeviceFeatures device_features;
        vkGetPhysicalDeviceFeatures(vk.physical_device(), &device_features);
        const bool can_count_pixels = device_features.occlusionQueryPrecise == VK_TRUE;
        if (!can_count_pixels) {
            std::printf("[vr_demo] precise occlusion queries are unavailable; "
                        "hole ratio feedback is off\n");
        }

        VrFrameRenderer renderer{targets, scene, reprojection, {0, 0},
                                 can_count_pixels ? VK_QUERY_CONTROL_PRECISE_BIT
                                                  : VkQueryControlFlags{0}};
        bool has_slots = is_ready;
        for (uint32_t eye = 0; eye < kEyeCount && has_slots; ++eye) {
            renderer.keyframe_slot[eye] = reprojection.register_source(
                {targets.keyframe_color(eye), targets.keyframe_depth(eye),
                 targets.extent().width, targets.extent().height});
            has_slots = renderer.keyframe_slot[eye] != ReprojectionPass::kInvalidSlot;
        }

        if (!has_slots) {
            std::fprintf(stderr, "[vr_demo] setup failed: %s\n", error.c_str());
        } else {
            IXrSession&           session = *runtime->session();
            IStereoOutputSurface& surface = *runtime->surface();

            StereoCamera camera;
            CameraRig rig;
            rig.world_from_tracking.set_translation(0.0f, kSeatedHeadHeight, 0.0f);
            camera.set_rig(rig);
            DeltaRenderPlanner planner(preset.config);

            const auto start = std::chrono::steady_clock::now();
            const uint64_t eye_pixels =
                static_cast<uint64_t>(targets.extent().width) * targets.extent().height;
            uint32_t frame_number = 0;
            exit_code = 0;

            while (session.poll_events() && !window.should_close()) {
                window.poll_events();
                if (session.state() == SessionState::Lost) {
                    std::fprintf(stderr, "[vr_demo] the XR session was lost\n");
                    exit_code = 1;
                    break;
                }

                FrameTiming timing;
                if (!session.begin_frame(timing)) continue;

                ViewState views;
                InputSnapshot input;
                session.locate_views(timing, views);
                session.sample_input(timing, input);  // demo は値を使わない (意味付けは host)
                const bool can_draw = timing.should_render && camera.update(views);

                bool has_layer = false;
                uint32_t image_index = 0;
                if (can_draw && surface.acquire(image_index)) {
                    const float t = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - start).count();
                    scene.update(t);

                    const FramePlan plan =
                        planner.plan(camera.eye(Eye::Left), camera.eye(Eye::Right));

                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkResetCommandBuffer(frame.buffer, 0);
                    vkBeginCommandBuffer(frame.buffer, &begin);
                    const bool is_multiview = plan.stereo_mode == StereoMode::Multiview;
                    if (is_multiview) {
                        renderer.draw_multiview(frame.buffer, image_index, camera);
                    } else {
                        vkCmdResetQueryPool(frame.buffer, frame.holes, 0, kEyeCount);
                        for (uint32_t eye = 0; eye < kEyeCount; ++eye) {
                            renderer.draw_eye(frame.buffer, frame.holes, image_index, eye, plan,
                                              camera.eye(static_cast<Eye>(eye)));
                        }
                    }
                    vkEndCommandBuffer(frame.buffer);

                    const bool is_submitted =
                        submit_and_wait(vk.device(), vk.graphics_queue(), frame);
                    has_layer = surface.release() && is_submitted;

                    if (is_submitted && !is_multiview && can_count_pixels) {
                        uint64_t hole_pixels[kEyeCount] = {0, 0};
                        if (vkGetQueryPoolResults(vk.device(), frame.holes, 0, kEyeCount,
                                                  sizeof(hole_pixels), hole_pixels, sizeof(uint64_t),
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                            VK_SUCCESS) {
                            for (uint32_t eye = 0; eye < kEyeCount; ++eye) {
                                // 全面描画した眼の値は「穴」ではないので報告しない。
                                if (plan.eyes[eye].source == StaticLayerSource::FullRedraw) continue;
                                planner.report_hole_pixels(static_cast<Eye>(eye), hole_pixels[eye],
                                                           eye_pixels);
                            }
                        }
                        if (++frame_number % kStatsInterval == 0) {
                            std::printf("[vr_demo] hole ratio L=%.3f R=%.3f\n",
                                        planner.last_hole_ratio(Eye::Left),
                                        planner.last_hole_ratio(Eye::Right));
                        }
                    }
                }
                session.end_frame(timing, views, has_layer);
            }
            vkDeviceWaitIdle(vk.device());
        }
        frame.destroy(vk.device());
        // HMD の画像を参照する framebuffer を、 その画像の持ち主より先に壊す。
        targets.shutdown();
        // HMD の画像への view を持つ runtime を、 VkDevice より先に壊す。
        runtime.reset();
    }
    vk.shutdown();
    return exit_code;
}

} // namespace

/// @implements SPEC-PC-XR-DEMO
int main(int argc, char** argv) {
    const char* preset_id = argc > 1 ? argv[1] : "vr.film_delta";
    const StereoPreset* preset = find_stereo_preset(preset_id);
    if (!preset) {
        std::fprintf(stderr, "[vr_demo] unknown preset: %s\n  available:", preset_id);
        for (uint32_t i = 0; i < stereo_preset_count(); ++i) {
            std::fprintf(stderr, " %.*s", static_cast<int>(stereo_preset_at(i).id.size()),
                         stereo_preset_at(i).id.data());
        }
        std::fprintf(stderr, "\n");
        return 2;
    }
    return run(*preset);
}
