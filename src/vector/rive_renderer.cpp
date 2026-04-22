#include "pictor/vector/rive_renderer.h"

#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <vector>

#ifdef PICTOR_HAS_RIVE

// Rive public headers. These only need to be visible in this translation unit.
// Pictor consumers never include them.
#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/renderer.hpp"
#include "rive/scene.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/vulkan/render_context_vulkan_impl.hpp"
#include "rive/renderer/vulkan/render_target_vulkan.hpp"

#endif // PICTOR_HAS_RIVE

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#endif

namespace pictor {

// ═══════════════════════════════════════════════════════════════
// Impl — real when PICTOR_HAS_RIVE is defined, otherwise a minimal
// stub that only tracks initialization state so calls fail cleanly.
// ═══════════════════════════════════════════════════════════════

#ifdef PICTOR_HAS_RIVE

struct RiveRenderer::Impl {
    RiveRenderer::Options                                    options;

    std::unique_ptr<rive::gpu::RenderContext>                render_context;
    rive::rcp<rive::gpu::RenderTargetVulkanImpl>             render_target;

    std::vector<uint8_t>                                     file_bytes;
    rive::rcp<rive::File>                                    file;
    std::unique_ptr<rive::ArtboardInstance>                  artboard;
    std::unique_ptr<rive::Scene>                             scene;

    std::unique_ptr<rive::RiveRenderer>                      renderer;
    uint32_t                                                 cached_w = 0;
    uint32_t                                                 cached_h = 0;

    bool                                                     initialized = false;

    // Cached Vulkan handles — needed when creating/resizing render targets.
#ifdef PICTOR_HAS_VULKAN
    VkInstance                                               vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice                                         vk_physical = VK_NULL_HANDLE;
    VkDevice                                                 vk_device   = VK_NULL_HANDLE;

    // Per-image layout tracking. Rive's setTargetImageView() wants the
    // image's current access state, and after flush() we need to know what
    // layout to barrier from. We cache both here, keyed by VkImage.
    // Swapchain images are a small finite set so the map stays tiny.
    std::unordered_map<VkImage, rive::gpu::vkutil::ImageAccess> image_access;
#endif

    rive::gpu::RenderContextVulkanImpl* vulkan_impl() const {
        return render_context
            ? render_context->static_impl_cast<rive::gpu::RenderContextVulkanImpl>()
            : nullptr;
    }

    void reset_scene() {
        scene.reset();
        artboard.reset();
    }

    void reset_file() {
        reset_scene();
        file.reset();
        file_bytes.clear();
    }
};

#else  // !PICTOR_HAS_RIVE — stub impl

struct RiveRenderer::Impl {
    bool initialized = false;
};

#endif

// ═══════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════

RiveRenderer::RiveRenderer() : impl_(std::make_unique<Impl>()) {}

RiveRenderer::~RiveRenderer() {
    if (impl_ && impl_->initialized) shutdown();
}

bool RiveRenderer::is_initialized() const {
    return impl_ && impl_->initialized;
}

// ─── initialize / shutdown ───────────────────────────────────

bool RiveRenderer::initialize(VulkanContext& vk_ctx, const Options& opts) {
    (void)vk_ctx; (void)opts;
#if !defined(PICTOR_HAS_RIVE)
    fprintf(stderr, "[Rive] PICTOR_HAS_RIVE is not defined — rebuild Pictor "
                    "with -DPICTOR_ENABLE_RIVE=ON and PICTOR_RIVE_DIR pointing "
                    "at a prebuilt rive-runtime.\n");
    return false;
#elif !defined(PICTOR_HAS_VULKAN)
    fprintf(stderr, "[Rive] Vulkan is not available in this Pictor build.\n");
    return false;
#else
    impl_->options       = opts;
    impl_->vk_instance   = vk_ctx.instance();
    impl_->vk_physical   = vk_ctx.physical_device();
    impl_->vk_device     = vk_ctx.device();

    rive::gpu::RenderContextVulkanImpl::ContextOptions ctx_opts;
    ctx_opts.forceAtomicMode = opts.force_atomic_mode;

    impl_->render_context = rive::gpu::RenderContextVulkanImpl::MakeContext(
        impl_->vk_instance,
        impl_->vk_physical,
        impl_->vk_device,
        rive::gpu::VulkanFeatures{},                  // auto-detect
        /*vkGetInstanceProcAddr=*/nullptr,            // static linkage with loader
        ctx_opts);

    if (!impl_->render_context) {
        fprintf(stderr, "[Rive] RenderContextVulkanImpl::MakeContext failed\n");
        return false;
    }

    impl_->initialized = true;
    printf("[Rive] Initialized (atomic=%d, msaa=%u)\n",
           opts.force_atomic_mode ? 1 : 0, opts.msaa_sample_count);
    return true;
#endif
}

void RiveRenderer::shutdown() {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->initialized) return;
    // Order: scene & renderer refer to the context, tear them down first.
    impl_->renderer.reset();
    impl_->reset_file();
    impl_->render_target.reset();
    impl_->render_context.reset();
    impl_->initialized = false;
#endif
}

// ─── Asset loading ───────────────────────────────────────────

bool RiveRenderer::load_riv_file(const std::string& path) {
#ifdef PICTOR_HAS_RIVE
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        fprintf(stderr, "[Rive] Cannot open .riv: %s\n", path.c_str());
        return false;
    }
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> bytes(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(size));
    return load_riv_memory(bytes.data(), bytes.size());
#else
    (void)path;
    return false;
#endif
}

bool RiveRenderer::load_riv_memory(const uint8_t* data, size_t size) {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->initialized) return false;
    if (!data || size == 0)   return false;

    impl_->reset_file();
    impl_->file_bytes.assign(data, data + size);

    // Rive's File::import requires a Factory — the RenderContext provides one.
    impl_->file = rive::File::import(
        rive::Span<const uint8_t>(impl_->file_bytes.data(),
                                  impl_->file_bytes.size()),
        impl_->render_context.get());

    if (!impl_->file) {
        fprintf(stderr, "[Rive] File::import failed (%zu bytes)\n", size);
        impl_->file_bytes.clear();
        return false;
    }

    // Default artboard + first state machine if any.
    set_artboard(-1);
    if (impl_->artboard && impl_->artboard->stateMachineCount() > 0) {
        set_state_machine(0);
    }
    return true;
#else
    (void)data; (void)size;
    return false;
#endif
}

void RiveRenderer::unload() {
#ifdef PICTOR_HAS_RIVE
    impl_->reset_file();
#endif
}

bool RiveRenderer::has_file() const {
#ifdef PICTOR_HAS_RIVE
    return impl_ && impl_->file != nullptr;
#else
    return false;
#endif
}

// ─── Scene selection ─────────────────────────────────────────

bool RiveRenderer::set_artboard(int index) {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->file) return false;
    impl_->reset_scene();
    impl_->artboard = (index < 0)
        ? impl_->file->artboardDefault()
        : impl_->file->artboardAt(static_cast<size_t>(index));
    return impl_->artboard != nullptr;
#else
    (void)index;
    return false;
#endif
}

bool RiveRenderer::set_state_machine(int index) {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->artboard) return false;
    if (index < 0) { impl_->scene.reset(); return true; }
    impl_->scene = impl_->artboard->stateMachineAt(static_cast<size_t>(index));
    return impl_->scene != nullptr;
#else
    (void)index;
    return false;
#endif
}

bool RiveRenderer::set_animation(int index) {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->artboard) return false;
    if (index < 0) { impl_->scene.reset(); return true; }
    impl_->scene = impl_->artboard->animationAt(static_cast<size_t>(index));
    return impl_->scene != nullptr;
#else
    (void)index;
    return false;
#endif
}

bool RiveRenderer::has_scene() const {
#ifdef PICTOR_HAS_RIVE
    return impl_ && impl_->scene != nullptr;
#else
    return false;
#endif
}

// ─── Per-frame ───────────────────────────────────────────────

void RiveRenderer::advance(float delta_time) {
#ifdef PICTOR_HAS_RIVE
    if (impl_->scene) {
        impl_->scene->advanceAndApply(delta_time);
    } else if (impl_->artboard) {
        impl_->artboard->advance(delta_time);
    }
#else
    (void)delta_time;
#endif
}

#ifdef PICTOR_HAS_VULKAN
bool RiveRenderer::render(VkCommandBuffer cmd,
                           VkImage         target_image,
                           VkImageView     target_view,
                           VkExtent2D      extent,
                           VkFormat        format,
                           uint32_t        current_frame_number,
                           uint32_t        safe_frame_number,
                           VkImageLayout   final_layout) {
#ifdef PICTOR_HAS_RIVE
    if (!impl_->initialized || !impl_->artboard) return false;

    // (Re)create the render target when dimensions change.
    if (!impl_->render_target ||
        impl_->cached_w != extent.width || impl_->cached_h != extent.height) {
        auto* vkimpl = impl_->vulkan_impl();
        impl_->render_target = vkimpl->makeRenderTarget(
            extent.width, extent.height, format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT);
        impl_->renderer = std::make_unique<rive::RiveRenderer>(
            impl_->render_context.get());
        impl_->cached_w = extent.width;
        impl_->cached_h = extent.height;
    }

    // Look up the image's last known access state. First sight of an image
    // (e.g. just-acquired swapchain image) → default-constructed ImageAccess
    // (UNDEFINED layout, which tells Rive it may clobber the contents).
    rive::gpu::vkutil::ImageAccess last_access;
    if (auto it = impl_->image_access.find(target_image);
        it != impl_->image_access.end()) {
        last_access = it->second;
    }

    impl_->render_target->setTargetImageView(target_view,
                                             target_image,
                                             last_access);

    // Begin a frame.
    rive::gpu::RenderContext::FrameDescriptor frame_desc;
    frame_desc.renderTargetWidth    = extent.width;
    frame_desc.renderTargetHeight   = extent.height;
    frame_desc.clearColor           = impl_->options.clear_color;
    frame_desc.msaaSampleCount      = impl_->options.msaa_sample_count;
    frame_desc.disableRasterOrdering = impl_->options.force_atomic_mode;
    impl_->render_context->beginFrame(std::move(frame_desc));

    // Draw the artboard.
    impl_->renderer->save();
    impl_->artboard->draw(impl_->renderer.get());
    impl_->renderer->restore();

    // Flush into the caller's command buffer.
    rive::gpu::RenderContext::FlushResources flush;
    flush.renderTarget         = impl_->render_target.get();
    flush.externalCommandBuffer = cmd;
    flush.currentFrameNumber    = current_frame_number;
    flush.safeFrameNumber       = safe_frame_number;
    impl_->render_context->flush(flush);

    // Rive's flush completes with the target image in some layout that
    // depends on the rendering path taken (atomic vs raster-ordered, MSAA,
    // etc.). Read it back and, if the caller requested a different final
    // layout, emit a single barrier — this is the only place that knows
    // precisely where Rive left off.
    rive::gpu::vkutil::ImageAccess after_flush =
        impl_->render_target->targetLastAccess();

    if (final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
        final_layout != after_flush.layout) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = after_flush.layout;
        b.newLayout           = final_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = target_image;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = after_flush.accessMask;
        b.dstAccessMask       = 0;
        vkCmdPipelineBarrier(cmd,
                             after_flush.pipelineStages,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        // Record the transition so the next render() that sees this image
        // reports the correct starting layout back to Rive.
        impl_->image_access[target_image] = rive::gpu::vkutil::ImageAccess{
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_NONE,
            final_layout,
        };
    } else {
        impl_->image_access[target_image] = after_flush;
    }
    return true;
#else
    (void)cmd; (void)target_image; (void)target_view; (void)extent;
    (void)format; (void)final_layout;
    (void)current_frame_number; (void)safe_frame_number;
    return false;
#endif
}
#endif // PICTOR_HAS_VULKAN

} // namespace pictor
