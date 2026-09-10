#include "demo_pipeline.h"
#include "pictor/surface/android_surface_provider.h"
#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <stdexcept>
#include <string>

namespace {
void require(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
struct Host {
    ANativeWindow* window = nullptr;
    std::unique_ptr<pictor::AndroidSurfaceProvider> provider;
    pictor::VulkanContext context;
    std::unique_ptr<DemoPipeline> pipeline;
    ~Host() {
        if (context.device()) vkDeviceWaitIdle(context.device());
        pipeline.reset();
        context.shutdown();
        provider.reset();
        if (window) ANativeWindow_release(window);
        __android_log_print(ANDROID_LOG_INFO, "PictorDemos", "surface resources released");
    }
    void frame() {
        // The simple demo UBO is single-buffered; serialize these diagnostic frames.
        require(vkDeviceWaitIdle(context.device()), "device wait");
        const uint32_t image = context.acquire_next_image();
        if (image == UINT32_MAX) throw std::runtime_error("Swapchain acquisition failed; reopen demo to rebuild resources");
        const auto command = context.command_buffers().at(image);
        require(vkResetCommandBuffer(command, 0), "command reset");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        require(vkBeginCommandBuffer(command, &begin), "command begin");
        pipeline->record(context, image);
        require(vkEndCommandBuffer(command), "command end");
        VkSemaphore available = context.image_available_semaphore();
        VkSemaphore finished = context.render_finished_semaphore();
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &available; submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &finished;
        require(vkQueueSubmit(context.graphics_queue(), 1, &submit, context.in_flight_fence()), "queue submit");
        if (!context.present(image)) throw std::runtime_error("Presentation failed; reopen demo to rebuild resources");
    }
};
void report(JNIEnv* env, const std::exception& error) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.what());
}
}
extern "C" JNIEXPORT jlong JNICALL Java_com_ludiars_pictor_demos_RenderActivity_createRenderer(
    JNIEnv* env, jclass, jobject surface, jstring directory, jint demo) {
    try {
        if (demo < 0 || demo > 1) throw std::runtime_error("Unknown demo");
        auto host = std::make_unique<Host>();
        host->window = ANativeWindow_fromSurface(env, surface);
        if (!host->window) throw std::runtime_error("Android surface is unavailable");
        host->provider = std::make_unique<pictor::AndroidSurfaceProvider>(host->window,
            ANativeWindow_getWidth(host->window), ANativeWindow_getHeight(host->window));
        if (!host->context.initialize(host->provider.get())) throw std::runtime_error("Vulkan initialization failed (Vulkan 1.2 required)");
        const char* chars = env->GetStringUTFChars(directory, nullptr);
        if (!chars) return 0; // JVM already holds an allocation exception.
        std::string shaders;
        try { shaders = chars; } catch (...) { env->ReleaseStringUTFChars(directory, chars); throw; }
        env->ReleaseStringUTFChars(directory, chars);
        host->pipeline = demo == 0 ? make_spheres(host->context, shaders.c_str()) : make_surface_demo();
        __android_log_print(ANDROID_LOG_INFO, "PictorDemos", "surface resources created, demo=%d", demo);
        return reinterpret_cast<jlong>(host.release());
    } catch (const std::exception& error) { report(env, error); return 0; }
}
extern "C" JNIEXPORT void JNICALL Java_com_ludiars_pictor_demos_RenderActivity_drawFrame(JNIEnv* env, jclass, jlong handle) {
    try { if (handle) reinterpret_cast<Host*>(handle)->frame(); }
    catch (const std::exception& error) { report(env, error); }
}
extern "C" JNIEXPORT void JNICALL Java_com_ludiars_pictor_demos_RenderActivity_destroyRenderer(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<Host*>(handle);
}
