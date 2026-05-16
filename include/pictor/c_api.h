// Pictor C ABI export — Ludellus 等の上位レイヤーが Pictor を WASM / Native plugin 経由で
// 叩くための窓口。 [[feedback_pictor_no_upper_dep]] のとおり Pictor 自身は Ludellus に依存しない。
// 上位は本ヘッダの opaque 構造体だけを介してやり取りする。
//
// 想定:
//   - Web (WASM):  Emscripten で pictor_webgl をリンクし、 cwrap で叩く
//   - Native:      Capacitor plugin / Electron N-API が直接リンク
//
// 設計方針: ABI 安定性 + 最小ホットパス
//   - 1 シーン = 1 PictorRenderer
//   - submit/update/remove は O(1)、 描画は render_frame で一括
//   - 文字列は UTF-8、 NUL 終端

#ifndef PICTOR_C_API_H
#define PICTOR_C_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(PICTOR_STATIC)
  #define PICTOR_API __declspec(dllexport)
#else
  #define PICTOR_API __attribute__((visibility("default")))
#endif

/* ── オブジェクト ───────────────────────────────────── */
typedef struct PictorRenderer PictorRenderer;
typedef uint64_t PictorObjectHandle;   /* 0 = invalid */

typedef enum {
  PICTOR_OBJ_CIRCLE = 1,
  PICTOR_OBJ_LINE   = 2,
  PICTOR_OBJ_QUAD   = 3,
  PICTOR_OBJ_TEXT   = 4,
  PICTOR_OBJ_IMAGE  = 5,
} PictorObjectKind;

typedef struct {
  PictorObjectKind kind;
  /* 共通: 色は 0xRRGGBBAA */
  uint32_t color;
  /* 位置 */
  float x, y;
  /* 円: r、 線分: x2/y2/width、 四角: w/h/rotate、 テキスト: w (= font size) + text */
  float a, b, c, d;
  /* テキスト/画像のリソース参照 (text の場合は UTF-8 ptr、 画像の場合は texture handle index) */
  const char* text;       /* nullable */
  uint64_t texture_id;    /* 0 = no texture */
} PictorObjectDescriptor;

/* ── ライフサイクル ─────────────────────────────────── */

/* WebGL2 (Emscripten) 用: HTML canvas の DOM id を渡す。 native では使わない。 */
PICTOR_API PictorRenderer* pictor_create_renderer_webgl(const char* canvas_dom_id);

/* Vulkan native 用: VkInstance / VkSurfaceKHR の opaque pointer。 caller が用意済みであること。 */
PICTOR_API PictorRenderer* pictor_create_renderer_vulkan(void* native_window_handle,
                                                          void* vulkan_instance);

PICTOR_API void pictor_destroy_renderer(PictorRenderer* r);

PICTOR_API void pictor_set_size(PictorRenderer* r, int width_px, int height_px);

/* ── シーン操作 ─────────────────────────────────────── */

PICTOR_API void pictor_begin_scene(PictorRenderer* r, uint32_t clear_color);

PICTOR_API void pictor_end_scene(PictorRenderer* r);

/* 新しいオブジェクトを追加。 戻り値はそのオブジェクトの handle (以降の update/remove で使う)。
   失敗時は 0。 */
PICTOR_API PictorObjectHandle pictor_submit_object(PictorRenderer* r,
                                                    const PictorObjectDescriptor* desc);

PICTOR_API void pictor_update_object(PictorRenderer* r,
                                      PictorObjectHandle handle,
                                      const PictorObjectDescriptor* desc);

PICTOR_API void pictor_remove_object(PictorRenderer* r, PictorObjectHandle handle);

/* delta time 単位は秒。 通常 60fps なら 1/60。 */
PICTOR_API void pictor_render_frame(PictorRenderer* r, double dt_seconds);

/* ── テクスチャ管理 (画像 / フォントアトラス) ───────── */

/* RGBA8 ピクセルから texture を作る。 戻り値の id を ObjectDescriptor.texture_id に入れる。 */
PICTOR_API uint64_t pictor_create_texture_rgba8(PictorRenderer* r,
                                                  const uint8_t* pixels,
                                                  int width, int height);

PICTOR_API void pictor_release_texture(PictorRenderer* r, uint64_t texture_id);

/* ── 診断 ───────────────────────────────────────────── */

/* 最後のエラーメッセージ (NULL = エラーなし)。 メモリは Pictor 所有、 caller は free しない。 */
PICTOR_API const char* pictor_last_error(void);

/* ABI バージョン。 上位は起動時にチェックして不一致なら fail-fast する想定。 */
PICTOR_API uint32_t pictor_c_api_version(void);
#define PICTOR_C_API_VERSION 1u

#ifdef __cplusplus
}
#endif

#endif /* PICTOR_C_API_H */
