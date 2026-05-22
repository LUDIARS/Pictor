// Pictor C ABI 実装 (skeleton)。
// 内部実装は Pictor の既存 Renderer / ObjectDescriptor に橋渡しするだけ。
// 当面は WebGL backend のみ動作可能、 Vulkan backend は stub (NULL 返却) で、
// 既存 Renderer 統合は Phase 2 で進める。

#include "pictor/c_api.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

thread_local std::string g_last_error;

void set_error(const char* msg) {
  g_last_error = msg ? msg : "";
}

} // namespace

/* ── PictorRenderer 内部表現 ────────────────────────── */
// 当面は最小データ + Pictor の上位レイヤーへの opaque ハンドルだけ持つ。
// Phase 2 で実 Renderer に置き換える。

struct PictorRenderer {
  enum class Backend { WebGL, Vulkan };
  Backend backend;
  int width_px = 0;
  int height_px = 0;
  uint32_t clear_color = 0xf7f1e8ff;

  std::atomic<uint64_t> next_object_id{1};
  std::mutex objects_mutex;
  std::unordered_map<uint64_t, PictorObjectDescriptor> objects;
  std::vector<std::vector<uint8_t>> textures; // texture_id - 1 がインデックス

  std::string canvas_id;      // WebGL のみ
  void* native_window = nullptr;
  void* vulkan_instance = nullptr;
};

extern "C" {

PICTOR_API PictorRenderer* pictor_create_renderer_webgl(const char* canvas_dom_id) {
  if (!canvas_dom_id) {
    set_error("canvas_dom_id is null");
    return nullptr;
  }
  auto* r = new PictorRenderer();
  r->backend = PictorRenderer::Backend::WebGL;
  r->canvas_id = canvas_dom_id;
  // 実装 Phase 2: emscripten WebGL2 context を canvas にバインドして
  // pictor_webgl ライブラリを初期化する。
  return r;
}

PICTOR_API PictorRenderer* pictor_create_renderer_vulkan(void* native_window_handle,
                                                          void* vulkan_instance) {
  if (!native_window_handle || !vulkan_instance) {
    set_error("native_window or vulkan_instance is null");
    return nullptr;
  }
  auto* r = new PictorRenderer();
  r->backend = PictorRenderer::Backend::Vulkan;
  r->native_window = native_window_handle;
  r->vulkan_instance = vulkan_instance;
  // 実装 Phase 2: VkSurfaceKHR を作って pictor の VulkanRenderer に渡す。
  return r;
}

PICTOR_API void pictor_destroy_renderer(PictorRenderer* r) {
  if (!r) return;
  delete r;
}

PICTOR_API void pictor_set_size(PictorRenderer* r, int width_px, int height_px) {
  if (!r) return;
  r->width_px = width_px;
  r->height_px = height_px;
}

PICTOR_API void pictor_begin_scene(PictorRenderer* r, uint32_t clear_color) {
  if (!r) return;
  r->clear_color = clear_color;
}

PICTOR_API void pictor_end_scene(PictorRenderer* r) {
  if (!r) return;
  // Phase 2: 既存 Pictor pipeline の Frame end を呼ぶ。
}

PICTOR_API PictorObjectHandle pictor_submit_object(PictorRenderer* r,
                                                    const PictorObjectDescriptor* desc) {
  if (!r || !desc) {
    set_error("renderer or descriptor is null");
    return 0;
  }
  const uint64_t id = r->next_object_id.fetch_add(1);
  PictorObjectDescriptor copy = *desc;
  // text ポインタは caller が次のフレームまで保持しない可能性が高いので
  // Phase 2 ではディープコピーする (現状は浅いコピーのまま、 caller 注意)。
  std::lock_guard<std::mutex> lock(r->objects_mutex);
  r->objects[id] = copy;
  return id;
}

PICTOR_API void pictor_update_object(PictorRenderer* r,
                                      PictorObjectHandle handle,
                                      const PictorObjectDescriptor* desc) {
  if (!r || !desc || handle == 0) return;
  std::lock_guard<std::mutex> lock(r->objects_mutex);
  auto it = r->objects.find(handle);
  if (it != r->objects.end()) it->second = *desc;
}

PICTOR_API void pictor_remove_object(PictorRenderer* r, PictorObjectHandle handle) {
  if (!r || handle == 0) return;
  std::lock_guard<std::mutex> lock(r->objects_mutex);
  r->objects.erase(handle);
}

PICTOR_API void pictor_render_frame(PictorRenderer* r, double dt_seconds) {
  if (!r) return;
  (void)dt_seconds;
  // Phase 2: 既存 Pictor の RenderFrame を呼んで objects を描画する。
  // 現状は no-op (objects は保持されているだけ)。
}

PICTOR_API uint64_t pictor_create_texture_rgba8(PictorRenderer* r,
                                                  const uint8_t* pixels,
                                                  int width, int height) {
  if (!r || !pixels || width <= 0 || height <= 0) {
    set_error("invalid texture parameters");
    return 0;
  }
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  std::vector<uint8_t> buf(pixels, pixels + bytes);
  r->textures.push_back(std::move(buf));
  return static_cast<uint64_t>(r->textures.size()); // 1-based id
}

PICTOR_API void pictor_release_texture(PictorRenderer* r, uint64_t texture_id) {
  if (!r || texture_id == 0 || texture_id > r->textures.size()) return;
  // 簡略: 空 vector に置き換える (id を維持するため要素ごと削除はしない)
  r->textures[texture_id - 1].clear();
  r->textures[texture_id - 1].shrink_to_fit();
}

PICTOR_API const char* pictor_last_error(void) {
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

PICTOR_API uint32_t pictor_c_api_version(void) {
  return PICTOR_C_API_VERSION;
}

} // extern "C"
