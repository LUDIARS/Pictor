/// Pictor Rive-Cube Demo
///
/// Renders six Rive sample files onto the six faces of a rotating cube.
/// Each face is a dedicated 1024×1024 offscreen VkImage that a separate
/// RiveRenderer instance draws into every frame; the cube rasterizer
/// samples those images as ordinary 2D textures.
///
/// Axis rotation speeds (per-second):
///   X = 30 deg,  Y = 45 deg,  Z = 20 deg
///
/// Keys:
///   Esc — quit

#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/vector/rive_renderer.h"

#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pictor;

#ifdef NDEBUG
#  define CUBE_DBG(...) ((void)0)
#else
#  define CUBE_DBG(...) std::printf(__VA_ARGS__)
#endif

namespace {

constexpr uint32_t FACE_COUNT     = 6;
// Per-face Rive render target resolution. Bumped above the 1024×1024
// window so the cube can be zoomed in up to ~2× before per-texel
// magnification becomes visible — Rive is vector-sourced, so rasterizing
// at higher resolution is effectively free-quality. Raise to 4096 if the
// camera needs to go closer than the z ≥ 1.2 clamp in update_camera().
constexpr uint32_t FACE_TEX_SIZE  = 2048;
constexpr VkFormat FACE_FORMAT    = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT   = VK_FORMAT_D32_SFLOAT;

// ─── Simple 4x4 float matrix helpers ───────────────────────────
struct Mat4 { float m[16]; };

Mat4 mat4_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k) s += a.m[i*4+k] * b.m[k*4+j];
        r.m[i*4+j] = s;
    }
    return r;
}

Mat4 mat4_rotation_x(float t) {
    Mat4 r = mat4_identity();
    float c = std::cos(t), s = std::sin(t);
    r.m[5] = c; r.m[6] = -s; r.m[9] = s; r.m[10] = c;
    return r;
}
Mat4 mat4_rotation_y(float t) {
    Mat4 r = mat4_identity();
    float c = std::cos(t), s = std::sin(t);
    r.m[0] = c; r.m[2] = s; r.m[8] = -s; r.m[10] = c;
    return r;
}
Mat4 mat4_rotation_z(float t) {
    Mat4 r = mat4_identity();
    float c = std::cos(t), s = std::sin(t);
    r.m[0] = c; r.m[1] = -s; r.m[4] = s; r.m[5] = c;
    return r;
}

Mat4 mat4_translate(float x, float y, float z) {
    Mat4 r = mat4_identity();
    r.m[3] = x; r.m[7] = y; r.m[11] = z;
    return r;
}

// Right-handed perspective (Vulkan clip space: y-down, z ∈ [0,1]).
Mat4 mat4_perspective(float fovy_rad, float aspect, float znear, float zfar) {
    Mat4 r{};
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = -f; // flip Y for Vulkan
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = (zfar * znear) / (znear - zfar);
    r.m[14] = -1.0f;
    return r;
}

// ─── Cube geometry (24 vertices, 36 indices) ──────────────────
struct CubeVertex { float pos[3]; float uv[2]; };

// Face order (index × 6 indices in the buffer):
//   0 = +X  1 = -X  2 = +Y  3 = -Y  4 = +Z  5 = -Z
// Each face is 4 vertices wound CCW as seen from outside the cube. Vertex
// order per face is: v0 = bottom-left, v1 = bottom-right, v2 = top-right,
// v3 = top-left, where "left/right/up/down" is defined from a viewer
// standing on the face's outward normal. This invariant is what makes
// back-face culling consistent across all 6 faces — the previous layout
// had +X/-X/+Y/-Y wound CW-from-outside, which caused those four faces
// to flip to front-facing (and the +Z/-Z faces to become back-facing)
// once the Y-flip in mat4_perspective inverted the on-screen winding.
//
// UV convention: (0,0) at top-left, (1,1) at bottom-right of each face
// — i.e. standard image orientation, which matches how Rive outputs
// its face textures.
const std::array<CubeVertex, 24> CUBE_VERTICES = {{
    // +X face (right).  Outside viewer: camera-right = -Z, camera-up = +Y.
    {{  0.5f, -0.5f,  0.5f }, {0.0f, 1.0f}},
    {{  0.5f, -0.5f, -0.5f }, {1.0f, 1.0f}},
    {{  0.5f,  0.5f, -0.5f }, {1.0f, 0.0f}},
    {{  0.5f,  0.5f,  0.5f }, {0.0f, 0.0f}},
    // -X face (left).   Outside viewer: camera-right = +Z, camera-up = +Y.
    {{ -0.5f, -0.5f, -0.5f }, {0.0f, 1.0f}},
    {{ -0.5f, -0.5f,  0.5f }, {1.0f, 1.0f}},
    {{ -0.5f,  0.5f,  0.5f }, {1.0f, 0.0f}},
    {{ -0.5f,  0.5f, -0.5f }, {0.0f, 0.0f}},
    // +Y face (top).    Outside viewer: camera-right = +X, camera-up = -Z.
    {{ -0.5f,  0.5f,  0.5f }, {0.0f, 1.0f}},
    {{  0.5f,  0.5f,  0.5f }, {1.0f, 1.0f}},
    {{  0.5f,  0.5f, -0.5f }, {1.0f, 0.0f}},
    {{ -0.5f,  0.5f, -0.5f }, {0.0f, 0.0f}},
    // -Y face (bottom). Outside viewer: camera-right = +X, camera-up = +Z.
    {{ -0.5f, -0.5f, -0.5f }, {0.0f, 1.0f}},
    {{  0.5f, -0.5f, -0.5f }, {1.0f, 1.0f}},
    {{  0.5f, -0.5f,  0.5f }, {1.0f, 0.0f}},
    {{ -0.5f, -0.5f,  0.5f }, {0.0f, 0.0f}},
    // +Z face (front).  Outside viewer: camera-right = +X, camera-up = +Y.
    {{ -0.5f, -0.5f,  0.5f }, {0.0f, 1.0f}},
    {{  0.5f, -0.5f,  0.5f }, {1.0f, 1.0f}},
    {{  0.5f,  0.5f,  0.5f }, {1.0f, 0.0f}},
    {{ -0.5f,  0.5f,  0.5f }, {0.0f, 0.0f}},
    // -Z face (back).   Outside viewer: camera-right = -X, camera-up = +Y.
    {{  0.5f, -0.5f, -0.5f }, {0.0f, 1.0f}},
    {{ -0.5f, -0.5f, -0.5f }, {1.0f, 1.0f}},
    {{ -0.5f,  0.5f, -0.5f }, {1.0f, 0.0f}},
    {{  0.5f,  0.5f, -0.5f }, {0.0f, 0.0f}},
}};
const std::array<uint16_t, 36> CUBE_INDICES = {{
    0, 1, 2, 0, 2, 3,
    4, 5, 6, 4, 6, 7,
    8, 9,10, 8,10,11,
   12,13,14,12,14,15,
   16,17,18,16,18,19,
   20,21,22,20,22,23,
}};

// ─── Vulkan utilities ───────────────────────────────────────
uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

bool create_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                   VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits, props);
    if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, buf, mem, 0);
    return true;
}

struct Offscreen {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
};

bool create_offscreen(VkDevice dev, VkPhysicalDevice phys,
                      uint32_t w, uint32_t h, VkFormat format,
                      Offscreen& out) {
    VkImageCreateInfo ii{};
    ii.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = format;
    ii.extent      = { w, h, 1 };
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    // Rive's atomic path needs STORAGE + INPUT_ATTACHMENT + COLOR_ATTACHMENT.
    // SAMPLED so the cube fragment shader can read it. TRANSFER_* for
    // initial layout convenience.
    ii.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_STORAGE_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &out.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, out.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, out.image, out.memory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(dev, &vi, nullptr, &out.view) != VK_SUCCESS) return false;
    return true;
}

// Probe for a shader .spv next to the exe, matching how `shaders/` is
// staged by `pictor_shaders` relative to the build root.
std::string resolve_shader_path(const char* name) {
    namespace fs = std::filesystem;
    const std::vector<fs::path> candidates = {
        fs::current_path() / "shaders" / name,              // cwd/shaders
        fs::current_path() / ".." / "shaders" / name,       // Debug/..
        fs::current_path() / ".." / ".." / "shaders" / name,
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec)) return fs::absolute(p, ec).string();
    }
    return std::string("shaders/") + name; // fallback (original behavior)
}

VkShaderModule load_shader(VkDevice dev, const char* name) {
    std::string path = resolve_shader_path(name);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        fprintf(stderr, "[rive-cube] cannot open shader: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> code(size);
    f.seekg(0);
    f.read(code.data(), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo si{};
    si.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = size;
    si.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &si, nullptr, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[rive-cube] shader module create failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    return mod;
}

// ─── Rive sample dir resolution ───────────────────────────
std::string resolve_rive_dir() {
    namespace fs = std::filesystem;
    const std::vector<fs::path> candidates = {
        fs::current_path() / "rive",
        fs::current_path() / ".." / "rive",
        fs::current_path() / ".." / ".." / "rive",
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            return fs::absolute(p, ec).string();
        }
    }
    return "";
}

// ─── Per-face worker thread ──────────────────────────
// Each worker owns an independent VkCommandPool + primary VkCommandBuffer
// (Vulkan rule: one command pool per thread per queue family). On each
// frame the main thread wakes the 6 workers via cv_start, each worker
// records `rive[i].render(...)` into its own command buffer, then
// signals cv_done. Main thread joins all 6 and submits all primary cmd
// buffers (6 Rive + 1 cube) in a single vkQueueSubmit — Vulkan
// guarantees the array is executed in order on the queue, so no
// inter-buffer semaphores are needed.
//
// One VkQueue must only be touched from one thread at a time, but
// recording into separate pools/buffers from separate threads is
// perfectly legal — that is the whole reason Vulkan has per-thread
// command pools.
struct FaceWorker {
    int                     index = 0;
    std::thread             thread;
    std::mutex              mu;
    std::condition_variable cv_start;
    std::condition_variable cv_done;
    bool                    has_task  = false;
    bool                    task_done = true;
    bool                    stopping  = false;

    VkDevice        device = VK_NULL_HANDLE;
    VkCommandPool   pool   = VK_NULL_HANDLE;
    VkCommandBuffer cmd    = VK_NULL_HANDLE;

    // Inputs set by the main thread before notifying cv_start.
    pictor::RiveRenderer* rive      = nullptr;
    VkImage               image     = VK_NULL_HANDLE;
    VkImageView           view      = VK_NULL_HANDLE;
    uint32_t              current_f = 0;
    uint32_t              safe_f    = 0;

    // Worker 0 owns the top-of-pipe timestamp for frame-wide GPU profiling
    // (it's the first cmd buffer in the submit array).
    bool        write_ts_start = false;
    VkQueryPool ts_pool        = VK_NULL_HANDLE;

    void run(uint32_t face_tex_size, VkFormat face_format) {
        while (true) {
            std::unique_lock<std::mutex> lk(mu);
            cv_start.wait(lk, [this]{ return has_task || stopping; });
            if (stopping) return;
            has_task = false;
            lk.unlock();

            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd, &bi);
            if (write_ts_start && ts_pool) {
                vkCmdResetQueryPool(cmd, ts_pool, 0, 2);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    ts_pool, 0);
            }
            rive->render(cmd, image, view,
                         { face_tex_size, face_tex_size }, face_format,
                         current_f, safe_f,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkEndCommandBuffer(cmd);

            lk.lock();
            task_done = true;
            lk.unlock();
            cv_done.notify_one();
        }
    }

    void dispatch(pictor::RiveRenderer* r, VkImage img, VkImageView v,
                  uint32_t cur, uint32_t safe) {
        {
            std::lock_guard<std::mutex> lk(mu);
            rive      = r;
            image     = img;
            view      = v;
            current_f = cur;
            safe_f    = safe;
            has_task  = true;
            task_done = false;
        }
        cv_start.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        cv_done.wait(lk, [this]{ return task_done; });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
        }
        cv_start.notify_one();
        if (thread.joinable()) thread.join();
    }
};

bool g_should_close = false;
// Interactive camera state. X is fixed at 0; the up/down and left/right
// arrow keys move the camera on world Y and Z respectively. Initial values
// place the camera at (0, 0, 2.5) looking at the origin — same as the
// original hard-coded view.
float g_cam_y = 0.0f;
float g_cam_z = 2.5f;
bool  g_cam_dirty = true;

void key_callback(GLFWwindow* w, int key, int, int action, int) {
    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
        g_should_close = true;
    }
}

// Poll held arrow keys once per frame and update the camera. Using the
// continuous `glfwGetKey` state (instead of the key_callback press events)
// gives smooth motion when the key is held down.
void update_camera(GLFWwindow* w, float dt) {
    const float speed = 2.0f; // world units per second
    float prev_y = g_cam_y, prev_z = g_cam_z;
    if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) g_cam_y += speed * dt;
    if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) g_cam_y -= speed * dt;
    if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) g_cam_z -= speed * dt; // move closer
    if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) g_cam_z += speed * dt; // pull back
    // Keep camera outside the cube (radius ~0.87) and not behind the near plane.
    if (g_cam_z < 1.2f) g_cam_z = 1.2f;
    if (g_cam_y !=  prev_y || g_cam_z != prev_z) g_cam_dirty = true;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    CUBE_DBG("[rive-cube] boot\n");

    // ─── CLI: frame-rate control ──────────────────────────
    // Default: vsync on (60fps on a 60 Hz display).
    //   --fps=N   N > 0: vsync off + software throttle to N fps (measurement)
    //   --fps=0   vsync off, uncapped (see raw perf)
    int fps_cap = -1; // -1 = vsync on
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--fps=", 0) == 0) {
            fps_cap = std::atoi(a.c_str() + 6);
            if (fps_cap < 0) fps_cap = 0;
        }
    }
    const bool vsync_on = (fps_cap < 0);
    if (fps_cap < 0)       printf("[rive-cube] frame pacing: vsync on (refresh-locked)\n");
    else if (fps_cap == 0) printf("[rive-cube] frame pacing: vsync OFF, uncapped\n");
    else                   printf("[rive-cube] frame pacing: vsync OFF, target %d fps\n", fps_cap);

    std::string rive_dir = resolve_rive_dir();
    if (rive_dir.empty()) {
        fprintf(stderr, "[rive-cube] rive/ asset directory not found — "
                        "expected near the exe (pictor_rive_assets CMake target)\n");
        return 1;
    }
    CUBE_DBG("[rive-cube] rive dir: %s\n", rive_dir.c_str());

    // ─── GLFW + Vulkan ────────────────────────────────────
    GlfwSurfaceProvider surface;
    GlfwWindowConfig win{};
    win.width  = 1024;
    win.height = 1024;
    win.title  = "Pictor — Rive Cube Demo";
    win.vsync  = vsync_on;
    if (!surface.create(win)) { fprintf(stderr, "GLFW create failed\n"); return 1; }
    glfwSetKeyCallback(surface.glfw_window(), key_callback);

    VulkanContext vk;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Rive Cube";
    vk_cfg.validation = true;
    if (!vk.initialize(&surface, vk_cfg)) { fprintf(stderr, "Vulkan init failed\n"); return 1; }

    VkDevice         device   = vk.device();
    VkPhysicalDevice physical = vk.physical_device();

    // ─── Offscreen face images (6 × 1024×1024) ────────────
    std::array<Offscreen, FACE_COUNT> faces{};
    for (uint32_t i = 0; i < FACE_COUNT; ++i) {
        if (!create_offscreen(device, physical, FACE_TEX_SIZE, FACE_TEX_SIZE, FACE_FORMAT, faces[i])) {
            fprintf(stderr, "[rive-cube] offscreen[%u] create failed\n", i);
            return 1;
        }
    }
    CUBE_DBG("[rive-cube] 6 offscreen images created\n");

    // ─── Depth image (swapchain sized) ───────────────────
    VkExtent2D sc_extent = vk.swapchain_extent();
    Offscreen depth{};
    {
        VkImageCreateInfo ii{};
        ii.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType   = VK_IMAGE_TYPE_2D;
        ii.format      = DEPTH_FORMAT;
        ii.extent      = { sc_extent.width, sc_extent.height, 1 };
        ii.mipLevels   = 1; ii.arrayLayers = 1;
        ii.samples     = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &ii, nullptr, &depth.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depth.image, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = find_memory_type(physical, req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ai, nullptr, &depth.memory);
        vkBindImageMemory(device, depth.image, depth.memory, 0);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = depth.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = DEPTH_FORMAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device, &vi, nullptr, &depth.view);
    }

    // ─── Cube render pass (color + depth) ────────────────
    VkRenderPass cube_render_pass = VK_NULL_HANDLE;
    {
        VkAttachmentDescription attach[2]{};
        attach[0].format         = vk.swapchain_format();
        attach[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attach[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attach[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attach[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attach[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attach[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attach[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attach[1].format         = DEPTH_FORMAT;
        attach[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attach[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attach[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attach[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attach[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attach[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attach[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depth_ref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp{};
        sp.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments    = &color_ref;
        sp.pDepthStencilAttachment = &depth_ref;
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpi{};
        rpi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 2;
        rpi.pAttachments    = attach;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &sp;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        if (vkCreateRenderPass(device, &rpi, nullptr, &cube_render_pass) != VK_SUCCESS) {
            fprintf(stderr, "[rive-cube] render pass create failed\n");
            return 1;
        }
    }

    // ─── Framebuffers (one per swapchain image) ──────────
    const auto& sc_views = vk.swapchain_image_views();
    std::vector<VkFramebuffer> framebuffers(sc_views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < sc_views.size(); ++i) {
        VkImageView attaches[2] = { sc_views[i], depth.view };
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass      = cube_render_pass;
        fi.attachmentCount = 2;
        fi.pAttachments    = attaches;
        fi.width           = sc_extent.width;
        fi.height          = sc_extent.height;
        fi.layers          = 1;
        vkCreateFramebuffer(device, &fi, nullptr, &framebuffers[i]);
    }

    // ─── Vertex + index buffer (host-visible for simplicity) ─
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    const VkDeviceSize vsize = sizeof(CUBE_VERTICES);
    const VkDeviceSize isize = sizeof(CUBE_INDICES);
    if (!create_buffer(device, physical, vsize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            vbuf, vmem)) { fprintf(stderr, "vbuf fail\n"); return 1; }
    if (!create_buffer(device, physical, isize,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            ibuf, imem)) { fprintf(stderr, "ibuf fail\n"); return 1; }
    {
        void* p = nullptr;
        vkMapMemory(device, vmem, 0, vsize, 0, &p);
        std::memcpy(p, CUBE_VERTICES.data(), vsize);
        vkUnmapMemory(device, vmem);
        vkMapMemory(device, imem, 0, isize, 0, &p);
        std::memcpy(p, CUBE_INDICES.data(), isize);
        vkUnmapMemory(device, imem);
    }

    // ─── Sampler + descriptor layout / pool / 6 sets ─────
    // Sampler uses linear min/mag with anisotropy so oblique face views
    // (near-grazing angle) do not smear. samplerAnisotropy is enabled in
    // VulkanContext::create_logical_device — if the GPU doesn't expose it
    // VulkanContext leaves the feature off and we fall back to plain
    // bilinear below.
    VkSampler sampler = VK_NULL_HANDLE;
    {
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physical, &supported);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (supported.samplerAnisotropy) {
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy    = props.limits.maxSamplerAnisotropy;
        }
        vkCreateSampler(device, &si, nullptr, &sampler);
    }
    VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings    = &b;
        vkCreateDescriptorSetLayout(device, &li, nullptr, &ds_layout);
    }
    VkDescriptorPool ds_pool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize ps{};
        ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = FACE_COUNT;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = FACE_COUNT;
        pi.poolSizeCount = 1;
        pi.pPoolSizes    = &ps;
        vkCreateDescriptorPool(device, &pi, nullptr, &ds_pool);
    }
    std::array<VkDescriptorSet, FACE_COUNT> ds_sets{};
    {
        std::array<VkDescriptorSetLayout, FACE_COUNT> layouts;
        layouts.fill(ds_layout);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = ds_pool;
        ai.descriptorSetCount = FACE_COUNT;
        ai.pSetLayouts        = layouts.data();
        vkAllocateDescriptorSets(device, &ai, ds_sets.data());
        for (uint32_t i = 0; i < FACE_COUNT; ++i) {
            VkDescriptorImageInfo ii{};
            ii.sampler     = sampler;
            ii.imageView   = faces[i].view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = ds_sets[i];
            w.dstBinding      = 0;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo      = &ii;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
    }

    // ─── Pipeline ────────────────────────────────────────
    VkShaderModule vs = load_shader(device, "rive_cube.vert.spv");
    VkShaderModule fs_mod = load_shader(device, "rive_cube.frag.spv");
    if (!vs || !fs_mod) return 1;

    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pc.offset     = 0;
        pc.size       = sizeof(Mat4);
        VkPipelineLayoutCreateInfo li{};
        li.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount         = 1;
        li.pSetLayouts            = &ds_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges    = &pc;
        vkCreatePipelineLayout(device, &li, nullptr, &pipe_layout);
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs_mod;
        stages[1].pName  = "main";
        VkVertexInputBindingDescription vbind{};
        vbind.binding   = 0;
        vbind.stride    = sizeof(CubeVertex);
        vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription vattr[2]{};
        vattr[0].location = 0; vattr[0].binding = 0;
        vattr[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        vattr[0].offset   = offsetof(CubeVertex, pos);
        vattr[1].location = 1; vattr[1].binding = 0;
        vattr[1].format   = VK_FORMAT_R32G32_SFLOAT;
        vattr[1].offset   = offsetof(CubeVertex, uv);
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &vbind;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions    = vattr;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_BACK_BIT;
        // The vertex buffer lists each face CCW as seen from outside. Since
        // gl_Position is (proj * view * rot) * pos and we write the MVP as
        // row-major (see rive_cube.vert), the on-screen winding matches
        // the world-space winding. Use CCW for front-facing so the
        // outward-facing (nearest) faces are drawn and the inside-facing
        // (far-side) ones are culled.
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &ba;
        VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dyn_states;
        VkGraphicsPipelineCreateInfo pci{};
        pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = pipe_layout;
        pci.renderPass          = cube_render_pass;
        pci.subpass             = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) != VK_SUCCESS) {
            fprintf(stderr, "[rive-cube] pipeline create failed\n");
            return 1;
        }
    }
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs_mod, nullptr);

    // ─── 6 Rive renderers, one per sample ────────────────
    std::array<RiveRenderer, FACE_COUNT> rives;
    for (uint32_t i = 0; i < FACE_COUNT; ++i) {
        RiveRenderer::Options opts;
        // force_atomic_mode defaults to false — Rive auto-selects interlock
        // or ROV when Pictor's VulkanContext enabled the extensions, falling
        // back to atomic on GPUs (e.g. Pascal) that don't support either.
        opts.clear_color       = 0xff202020;
        // Fit each artboard into the 2048×2048 face target so Rive fills
        // the full texture regardless of the .riv's native artboard size.
        opts.fit               = RiveRenderer::Fit::contain;
        if (!rives[i].initialize(vk, opts)) {
            fprintf(stderr, "[rive-cube] Rive init failed on face %u\n", i);
            return 1;
        }
        char fname[64];
        std::snprintf(fname, sizeof(fname), "sample%u.riv", i + 1);
        std::filesystem::path p = std::filesystem::path(rive_dir) / fname;
        if (!rives[i].load_riv_file(p.string())) {
            fprintf(stderr, "[rive-cube] load %s failed\n", fname);
            return 1;
        }
    }
    CUBE_DBG("[rive-cube] 6 Rive instances loaded (face tex=%ux%u)\n",
             FACE_TEX_SIZE, FACE_TEX_SIZE);

    // ─── Per-face worker threads + command pools ────────
    // Each worker gets its own VkCommandPool + primary VkCommandBuffer.
    // Pool uses VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT so we can
    // vkResetCommandBuffer() per frame without resetting the whole pool.
    std::array<FaceWorker, FACE_COUNT> workers{};
    for (uint32_t i = 0; i < FACE_COUNT; ++i) {
        workers[i].index  = static_cast<int>(i);
        workers[i].device = device;

        VkCommandPoolCreateInfo pi{};
        pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk.queue_family();
        if (vkCreateCommandPool(device, &pi, nullptr, &workers[i].pool) != VK_SUCCESS) {
            fprintf(stderr, "[rive-cube] worker cmd pool[%u] create failed\n", i);
            return 1;
        }
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = workers[i].pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &ai, &workers[i].cmd) != VK_SUCCESS) {
            fprintf(stderr, "[rive-cube] worker cmd buffer[%u] alloc failed\n", i);
            return 1;
        }
    }
    // Worker 0 carries the frame-start timestamp (first cmd in submit order).
    workers[0].write_ts_start = true;
    // Spawn the threads. They block on cv_start until the main loop dispatches.
    for (auto& w : workers) {
        w.ts_pool = VK_NULL_HANDLE; // set below once ts_pool is created
        w.thread  = std::thread([&w]{ w.run(FACE_TEX_SIZE, FACE_FORMAT); });
    }
    CUBE_DBG("[rive-cube] spawned %u worker threads\n", FACE_COUNT);

    // ─── Timing infrastructure ──────────────────────────
    // CPU stage timers accumulate across a rolling window (every 60
    // frames the average of each stage is printed). The GPU timestamp
    // query pool measures total GPU time per frame — TOP_OF_PIPE to
    // BOTTOM_OF_PIPE, so it captures Rive + cube-pass combined.
    VkQueryPool ts_pool = VK_NULL_HANDLE;
    float       ts_period_ns = 0.0f;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        ts_period_ns = props.limits.timestampPeriod;
        if (props.limits.timestampComputeAndGraphics && ts_period_ns > 0.0f) {
            VkQueryPoolCreateInfo qi{};
            qi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            vkCreateQueryPool(device, &qi, nullptr, &ts_pool);
        }
    }
    // Worker 0 writes the TOP_OF_PIPE timestamp at the start of its cmd
    // buffer, which is the first cmd in submit order → covers Rive+cube.
    workers[0].ts_pool = ts_pool;

    struct Accum {
        double advance_ms  = 0;
        double acquire_ms  = 0;
        double rive_rec_ms = 0;
        double cube_rec_ms = 0;
        double submit_ms   = 0;
        double gpu_ms      = 0;
        int    samples     = 0;
    } acc{};

    // ─── Main loop ───────────────────────────────────────
    auto t_start = std::chrono::high_resolution_clock::now();
    auto t_prev  = t_start;
    uint64_t frame_number = 1;
    uint64_t frame_count  = 0;

    printf("Camera controls:  Up/Down = Y,  Left/Right = Z,  Esc = quit\n");
    printf("Initial camera: (0, %.2f, %.2f)\n\n", g_cam_y, g_cam_z);
#ifdef NDEBUG
    const char* build_label = "Release";
#else
    const char* build_label = "Debug";
#endif
    printf("[profile] %s build. 6x Rive @ %ux%u (coverage mode: %s, contain fit).\n",
           build_label, FACE_TEX_SIZE, FACE_TEX_SIZE,
           vk.has_fragment_shader_interlock() ? "interlock" :
           vk.has_rasterization_order_attachment_access() ? "ROV" : "atomic");
    if (ts_pool) printf("[profile] GPU timestamps enabled (period=%.2f ns).\n", ts_period_ns);

    while (!surface.should_close() && !g_should_close) {
        surface.poll_events();
        auto t_now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev   = t_now;
        float t  = std::chrono::duration<float>(t_now - t_start).count();
        if (dt > 0.1f) dt = 0.1f;

        update_camera(surface.glfw_window(), dt);
        if (g_cam_dirty) {
            printf("camera: (0.00, %+.2f, %+.2f)\n", g_cam_y, g_cam_z);
            g_cam_dirty = false;
        }

        // Advance all 6 Rive scenes. Must happen on the main thread
        // before dispatching workers — Rive's scene graph update touches
        // state the worker thread will then read during render().
        for (auto& r : rives) r.advance(dt);
        auto ts_advance_e = std::chrono::high_resolution_clock::now();

        uint32_t image_idx = vk.acquire_next_image();
        auto ts_acquire_e = std::chrono::high_resolution_clock::now();
        if (image_idx == UINT32_MAX) continue;

        const uint32_t current_f = static_cast<uint32_t>(frame_number);
        const uint32_t safe_f    = static_cast<uint32_t>(frame_number - 1);

        // 1) Dispatch 6 Rive face recordings to worker threads. Each
        //    worker records its own primary cmd buffer independently —
        //    they will be submitted in sequence by vkQueueSubmit.
        for (uint32_t i = 0; i < FACE_COUNT; ++i) {
            workers[i].dispatch(&rives[i], faces[i].image, faces[i].view,
                                current_f, safe_f);
        }

        // 2) While workers record Rive, the main thread records the cube
        //    pass into its own cmd buffer. This is pure parallelism — the
        //    cube cmd buffer references the face images via descriptor
        //    sets but does not touch their cmd buffers.
        VkCommandBuffer cmd = vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        // 3) Begin the cube render pass and draw 6 faces.
        VkClearValue clears[2]{};
        clears[0].color = { {0.05f, 0.05f, 0.08f, 1.0f} };
        clears[1].depthStencil = { 1.0f, 0 };
        VkRenderPassBeginInfo rbi{};
        rbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass        = cube_render_pass;
        rbi.framebuffer       = framebuffers[image_idx];
        rbi.renderArea        = { {0, 0}, sc_extent };
        rbi.clearValueCount   = 2;
        rbi.pClearValues      = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{}; vp.width = float(sc_extent.width); vp.height = float(sc_extent.height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        VkRect2D   sc{ {0,0}, sc_extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor (cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize offs = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &offs);
        vkCmdBindIndexBuffer  (cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);

        // Build MVP. The cube is at origin; camera sits on +Z looking at it.
        const float ax = t * (30.0f * 3.14159265f / 180.0f);
        const float ay = t * (45.0f * 3.14159265f / 180.0f);
        const float az = t * (20.0f * 3.14159265f / 180.0f);
        Mat4 rot = mat4_mul(mat4_mul(mat4_rotation_z(az), mat4_rotation_y(ay)), mat4_rotation_x(ax));
        // World translation opposite the camera position: camera sits at
        // (0, g_cam_y, g_cam_z), so the world shifts by (0, -y, -z).
        Mat4 view = mat4_translate(0.0f, -g_cam_y, -g_cam_z);
        float aspect = float(sc_extent.width) / float(sc_extent.height);
        Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
        Mat4 mvp  = mat4_mul(mat4_mul(proj, view), rot);
        vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(Mat4), &mvp);

        // Bind per-face descriptor set and issue 6 indexed draws, each
        // covering one quad (2 triangles, 6 indices) of the cube.
        for (uint32_t i = 0; i < FACE_COUNT; ++i) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipe_layout, 0, 1, &ds_sets[i], 0, nullptr);
            vkCmdDrawIndexed(cmd, 6, 1, i * 6, 0, 0);
        }
        vkCmdEndRenderPass(cmd);

        if (ts_pool) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, 1);
        }
        vkEndCommandBuffer(cmd);
        auto ts_cube_rec_e = std::chrono::high_resolution_clock::now();

        // 4) Wait for the 6 Rive workers to finish their cmd buffer
        //    recording. These ran in parallel with the cube cmd record
        //    above — the join is effectively free when Rive record is
        //    the bottleneck (the cube pass finishes in 0.1ms).
        for (auto& w : workers) w.wait();
        auto ts_rive_rec_e = std::chrono::high_resolution_clock::now();

        // 5) Submit all 7 primary cmd buffers in one vkQueueSubmit. The
        //    queue executes them strictly in array order, so the 6 Rive
        //    renders complete (and leave faces in SHADER_READ_ONLY_OPTIMAL)
        //    before the cube pass samples them — no inter-cmd sync needed.
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk.image_available_semaphore();
        VkSemaphore sig_sem  = vk.render_finished_semaphore();
        std::array<VkCommandBuffer, FACE_COUNT + 1> submit_cmds{};
        for (uint32_t i = 0; i < FACE_COUNT; ++i) submit_cmds[i] = workers[i].cmd;
        submit_cmds[FACE_COUNT] = cmd;

        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &wait_sem;
        si.pWaitDstStageMask    = &wait_stage;
        si.commandBufferCount   = static_cast<uint32_t>(submit_cmds.size());
        si.pCommandBuffers      = submit_cmds.data();
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &sig_sem;
        vkQueueSubmit(vk.graphics_queue(), 1, &si, vk.in_flight_fence());
        vk.present(image_idx);
        auto ts_submit_e = std::chrono::high_resolution_clock::now();

        // Read back the previous frame's GPU timestamps (non-blocking). We
        // query with WAIT on the previous frame's pool so the call does
        // not stall the current frame. For simplicity here we just query
        // with DONT_WAIT and accept that samples lag by one frame.
        double gpu_ms = 0.0;
        if (ts_pool && frame_count >= 1) {
            uint64_t ts_values[2] = {0, 0};
            VkResult qr = vkGetQueryPoolResults(
                device, ts_pool, 0, 2, sizeof(ts_values), ts_values,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (qr == VK_SUCCESS && ts_values[1] > ts_values[0]) {
                gpu_ms = double(ts_values[1] - ts_values[0]) * ts_period_ns * 1e-6;
            }
        }

        auto to_ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        acc.advance_ms  += to_ms(t_now,          ts_advance_e);
        acc.acquire_ms  += to_ms(ts_advance_e,   ts_acquire_e);
        // cube_rec measures main-thread cube cmd recording (runs in
        // parallel with the 6 worker threads' Rive recording).
        acc.cube_rec_ms += to_ms(ts_acquire_e,   ts_cube_rec_e);
        // rive_rec = wall-clock time the main thread had to wait for all
        // 6 workers to finish AFTER the cube cmd was already recorded. If
        // workers outpaced the cube, this is ~0; otherwise it's the
        // remaining Rive record time the parallelism could not hide.
        acc.rive_rec_ms += to_ms(ts_cube_rec_e,  ts_rive_rec_e);
        acc.submit_ms   += to_ms(ts_rive_rec_e,  ts_submit_e);
        acc.gpu_ms      += gpu_ms;
        acc.samples     += 1;

        // Software fps cap when vsync is off and --fps=N (N>0) was given.
        // sleep_until handles the bulk; a short spin absorbs OS jitter so
        // the next-frame time is hit reasonably precisely (±0.2ms on Win).
        if (fps_cap > 0) {
            using namespace std::chrono;
            static auto next_frame = high_resolution_clock::now();
            next_frame += duration_cast<high_resolution_clock::duration>(
                duration<double>(1.0 / fps_cap));
            auto slack = next_frame - milliseconds(1);
            if (high_resolution_clock::now() < slack) std::this_thread::sleep_until(slack);
            while (high_resolution_clock::now() < next_frame) { /* spin */ }
        }

        ++frame_number;
        ++frame_count;
        if (frame_count % 60 == 0) {
            const double n = acc.samples;
            printf("[profile] frame=%llu  fps=%.1f  cpu: adv=%.2f acq=%.2f "
                   "cube_rec=%.2f rive_wait=%.2f submit=%.2f  GPU=%.2f ms (avg/%d)\n",
                   static_cast<unsigned long long>(frame_count), 1.0f / dt,
                   acc.advance_ms/n, acc.acquire_ms/n,
                   acc.cube_rec_ms/n, acc.rive_rec_ms/n, acc.submit_ms/n,
                   acc.gpu_ms/n, acc.samples);
            acc = {};
        }
    }

    // ─── Cleanup ─────────────────────────────────────────
    vk.device_wait_idle();
    // Stop and join worker threads before destroying their cmd pools /
    // the Rive contexts those pools reference.
    for (auto& w : workers) w.stop();
    for (auto& w : workers) {
        if (w.pool) vkDestroyCommandPool(device, w.pool, nullptr);
    }
    for (auto& r : rives) r.shutdown();
    if (ts_pool) vkDestroyQueryPool(device, ts_pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipe_layout, nullptr);
    vkDestroyDescriptorPool(device, ds_pool, nullptr);
    vkDestroyDescriptorSetLayout(device, ds_layout, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyBuffer(device, vbuf, nullptr);
    vkFreeMemory(device, vmem, nullptr);
    vkDestroyBuffer(device, ibuf, nullptr);
    vkFreeMemory(device, imem, nullptr);
    for (auto& f : framebuffers) vkDestroyFramebuffer(device, f, nullptr);
    vkDestroyRenderPass(device, cube_render_pass, nullptr);
    vkDestroyImageView(device, depth.view, nullptr);
    vkDestroyImage(device, depth.image, nullptr);
    vkFreeMemory(device, depth.memory, nullptr);
    for (auto& f : faces) {
        vkDestroyImageView(device, f.view, nullptr);
        vkDestroyImage(device, f.image, nullptr);
        vkFreeMemory(device, f.memory, nullptr);
    }
    vk.shutdown();
    surface.destroy();
    CUBE_DBG("[rive-cube] exit\n");
    return 0;
}
