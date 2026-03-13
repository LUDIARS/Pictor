# Pictor クラス定義リファレンス

本ドキュメントは、Pictor の主要クラスとデータ構造の定義を記述します。

---

## 目次

1. [コア](#コア)
   - [PictorRenderer](#pictorrenderer)
   - [RendererConfig](#rendererconfig)
   - [Camera](#camera)
   - [ObjectDescriptor](#objectdescriptor)
2. [型定義](#型定義)
   - [プリミティブ型](#プリミティブ型)
   - [ハンドル型](#ハンドル型)
   - [列挙型](#列挙型)
3. [データ管理](#データ管理)
   - [DataHandler](#datahandler)
   - [TextureRegistry](#textureregistry)
   - [VertexDataUploader](#vertexdatauploader)
   - [DataQueryAPI](#dataqueryapi)
4. [マテリアル](#マテリアル)
   - [BaseMaterialBuilder](#basematerialbuilder)
   - [MaterialRegistry](#materialregistry)
   - [MaterialDesc](#materialdesc)
   - [PassMaterialVariant](#passmaterialvariant)
5. [GI](#gi)
   - [GILightingSystem](#gilightingsystem)
   - [GIBakeSystem](#gibakesystem)
6. [サーフェス](#サーフェス)
   - [VulkanContext](#vulkancontext)
   - [GlfwSurfaceProvider](#glfwsurfaceprovider)
7. [パイプライン](#パイプライン)
   - [PipelineProfileManager](#pipelineprofilemanager)
   - [RenderPassScheduler](#renderpassscheduler)
   - [CommandEncoder](#commandencoder)
8. [シーン管理](#シーン管理)
   - [SceneRegistry](#sceneregistry)
   - [ObjectClassifier](#objectclassifier)
9. [メモリ](#メモリ)
   - [MemorySubsystem](#memorysubsystem)
   - [FrameAllocator](#frameallocator)
10. [プロファイラ](#プロファイラ)
    - [Profiler](#profiler)
    - [DataExporter](#dataexporter)

---

## コア

### PictorRenderer

**ヘッダ:** `pictor/core/pictor_renderer.h`

パイプライン全体を統括するエントリポイント。全サブシステムを所有・管理します。

```cpp
class PictorRenderer {
public:
    // ライフサイクル
    void initialize();
    void initialize(const RendererConfig& config);
    void shutdown();
    bool is_initialized() const;

    // フレームレンダリング
    void begin_frame(float delta_time);
    void render(const Camera& camera);
    void end_frame();

    // オブジェクト操作
    ObjectId register_object(const ObjectDescriptor& desc);
    void     unregister_object(ObjectId id);
    void     update_transform(ObjectId id, const float4x4& transform);

    // プロファイル操作
    bool set_profile(const std::string& name);
    void register_custom_profile(const PipelineProfileDef& def);

    // データハンドラ
    TextureHandle register_texture(const TextureDescriptor& desc);
    void          unregister_texture(TextureHandle handle);
    MeshHandle    register_mesh_data(const MeshDataDescriptor& desc);
    void          unregister_mesh_data(MeshHandle handle);
    DataHandler&       data_handler();
    DataQueryAPI       create_query_api() const;

    // GI
    void         set_directional_light(const DirectionalLight& light);
    void         set_gi_config(const GIConfig& config);
    GIBakeResult bake_static_gi();
    GIBakeResult bake_static_gi(BakeProgressCallback progress);
    void         apply_bake(const GIBakeResult& result);
    void         invalidate_bake();
    bool         save_bake(const std::string& path, const GIBakeResult& result);
    GIBakeResult load_bake(const std::string& path);

    // 拡張ポイント
    void set_update_callback(IUpdateCallback* callback);
    void set_culling_provider(ICullingProvider* provider);
    void set_batch_policy(IBatchPolicy* policy);
    void set_job_dispatcher(IJobDispatcher* dispatcher);
    void register_custom_pass(ICustomRenderPass* pass);

    // プロファイラデータエクスポート
    void begin_profiler_recording(const std::string& path);
    void end_profiler_recording();
    bool export_profiler_json(const std::string& path);
    bool export_profiler_chrome_tracing(const std::string& path);
    bool export_profiler_csv(const std::string& path);

    // サブシステムアクセス
    MemorySubsystem&        memory();
    SceneRegistry&          scene();
    Profiler&               profiler();
    PipelineProfileManager& profile_manager();
    GILightingSystem*       gi_system();
    GIBakeSystem*           bake_system();
};
```

### RendererConfig

```cpp
struct RendererConfig {
    std::string  initial_profile  = "Standard";
    MemoryConfig memory_config;
    UpdateConfig update_config;
    uint32_t     screen_width     = 1920;
    uint32_t     screen_height    = 1080;
    bool         profiler_enabled = true;
    OverlayMode  overlay_mode     = OverlayMode::STANDARD;
};
```

### Camera

```cpp
struct Camera {
    float4x4 view;
    float4x4 projection;
    float3   position;
    Frustum  frustum;   // 6平面
};
```

### ObjectDescriptor

```cpp
struct ObjectDescriptor {
    MeshHandle     mesh       = INVALID_MESH;
    MaterialHandle material   = INVALID_MATERIAL;
    float4x4       transform  = float4x4::identity();  // 64B, キャッシュライン整列
    AABB           bounds     = {};                     // 24B
    uint16_t       flags      = ObjectFlags::DYNAMIC;
    uint64_t       shaderKey  = 0;
    uint32_t       materialKey = 0;
    uint8_t        lodLevel   = 0;
};
```

---

## 型定義

### プリミティブ型

| 型 | サイズ | 説明 |
|----|--------|------|
| `float3` | 12B | 3次元ベクトル (`x`, `y`, `z`) |
| `float4` | 16B | 4次元ベクトル (`x`, `y`, `z`, `w`) |
| `float4x4` | 64B | 4x4行列（`alignas(64)`、キャッシュライン整列） |
| `AABB` | 24B | 軸平行バウンディングボックス (`min`, `max`) |
| `Plane` | 16B | 平面 (`normal`, `distance`) |
| `Frustum` | 96B | 6平面によるフラスタム |

### ハンドル型

全て `uint32_t` のエイリアスです。無効値として `UINT32_MAX` が定義されています。

| ハンドル型 | 無効値定数 |
|-----------|-----------|
| `ObjectId` | `INVALID_OBJECT_ID` |
| `MeshHandle` | `INVALID_MESH` |
| `MaterialHandle` | `INVALID_MATERIAL` |
| `TextureHandle` | `INVALID_TEXTURE` |
| `ShaderHandle` | — |
| `PoolId` | — |

### 列挙型

#### TextureFormat

```cpp
enum class TextureFormat : uint8_t {
    RGBA8_UNORM, RGBA8_SRGB, RGBA16_FLOAT, RGBA32_FLOAT,
    R8_UNORM, RG8_UNORM,
    BC1_UNORM, BC3_UNORM, BC5_UNORM, BC7_UNORM,
    DEPTH_32F, DEPTH_24_STENCIL_8
};
```

#### TextureType

```cpp
enum class TextureType : uint8_t {
    TEXTURE_2D, TEXTURE_3D, TEXTURE_CUBE, TEXTURE_2D_ARRAY
};
```

#### VertexSemantic

```cpp
enum class VertexSemantic : uint8_t {
    POSITION, NORMAL, TANGENT,
    TEXCOORD0, TEXCOORD1, COLOR0,
    JOINTS, WEIGHTS,
    CUSTOM0, CUSTOM1, CUSTOM2, CUSTOM3
};
```

#### VertexAttributeType

```cpp
enum class VertexAttributeType : uint8_t {
    FLOAT, FLOAT2, FLOAT3, FLOAT4,
    UINT32, INT32, UNORM8X4,
    HALF2, HALF4
};
```

#### PassType

```cpp
enum class PassType : uint8_t {
    DEPTH_ONLY, OPAQUE, TRANSPARENT, SHADOW,
    POST_PROCESS, COMPUTE, CUSTOM
};
```

#### PoolType

```cpp
enum class PoolType : uint8_t { STATIC, DYNAMIC, GPU_DRIVEN };
```

#### RenderingPath

```cpp
enum class RenderingPath : uint8_t { FORWARD, FORWARD_PLUS, DEFERRED, HYBRID };
```

#### OverlayMode

```cpp
enum class OverlayMode : uint8_t { OFF, MINIMAL, STANDARD, DETAILED, TIMELINE };
```

#### ObjectFlags

名前空間 `ObjectFlags` 内のビットフラグ定数です。

| Bit | 名称 | 値 |
|-----|------|----|
| 0 | `STATIC` | `0x0001` |
| 1 | `DYNAMIC` | `0x0002` |
| 2 | `GPU_DRIVEN` | `0x0004` |
| 3 | `CAST_SHADOW` | `0x0008` |
| 4 | `RECEIVE_SHADOW` | `0x0010` |
| 5 | `TRANSPARENT` | `0x0020` |
| 6 | `TWO_SIDED` | `0x0040` |
| 7 | `INSTANCED` | `0x0080` |
| 8-9 | `LAYER_MASK` | `0x0300` |

ヘルパー関数: `ObjectFlags::layer(flags)`, `ObjectFlags::set_layer(flags, layer)`

---

## データ管理

### DataHandler

**ヘッダ:** `pictor/data/data_handler.h`

テクスチャと頂点データの登録・アップロードを統合するファサードクラスです。

```cpp
class DataHandler {
public:
    DataHandler(GpuMemoryAllocator& gpu_allocator, GPUBufferManager& buffer_manager);

    // テクスチャ操作
    TextureHandle register_texture(const TextureDescriptor& desc);
    bool upload_texture_data(TextureHandle handle, const void* data, size_t size,
                             uint32_t mip_level = 0, uint32_t array_layer = 0);
    void unregister_texture(TextureHandle handle);

    // メッシュ/頂点操作
    MeshHandle register_mesh(const MeshDataDescriptor& desc);
    bool upload_vertex_data(MeshHandle handle, const void* data, size_t size);
    bool upload_index_data(MeshHandle handle, const void* data, size_t size);
    bool update_vertex_region(MeshHandle handle, size_t offset, const void* data, size_t size);
    void unregister_mesh(MeshHandle handle);

    // サブシステムアクセス
    TextureRegistry&       textures();
    VertexDataUploader&    meshes();

    // 統計
    Stats get_stats() const;
};
```

### TextureRegistry

**ヘッダ:** `pictor/data/texture_registry.h`

テクスチャの登録・GPUメモリ管理を行います。遅延アップロードと即時アップロードの両方に対応しています。

**主要な記述子:**

```cpp
struct TextureDescriptor {
    std::string   name;
    uint32_t      width, height, depth;
    uint32_t      mip_levels, array_layers;
    TextureFormat format;
    TextureType   type;
    const void*   initial_data;  // nullptr なら遅延アップロード
    size_t        data_size;
};
```

### VertexDataUploader

**ヘッダ:** `pictor/data/vertex_data_uploader.h`

任意の頂点レイアウトによるメッシュ登録とGPU転送を管理します。部分更新にも対応しています。

**主要な記述子:**

```cpp
struct VertexLayout {
    std::vector<VertexAttribute> attributes;
    uint32_t stride;  // 0 = 属性から自動計算
};

struct MeshDataDescriptor {
    std::string    name;
    VertexLayout   layout;
    const void*    vertex_data;
    size_t         vertex_data_size;
    uint32_t       vertex_count;
    const void*    index_data;       // オプション
    size_t         index_data_size;
    uint32_t       index_count;
    bool           index_32bit;      // true=uint32, false=uint16
};
```

### DataQueryAPI

**ヘッダ:** `pictor/data/data_query_api.h`

外部ツール向けの読み取り専用クエリAPIです。レンダリングパイプラインに影響を与えずにデータを列挙・検索できます。

```cpp
class DataQueryAPI {
public:
    explicit DataQueryAPI(const DataHandler& handler);

    DataSummary get_summary() const;

    // テクスチャクエリ
    TextureInfo              get_texture_info(TextureHandle handle) const;
    std::vector<TextureInfo> get_all_textures() const;
    TextureInfo              find_texture(const std::string& name) const;
    void for_each_texture(std::function<void(const TextureInfo&)> callback) const;
    std::vector<TextureInfo> get_textures_by_format(TextureFormat format) const;

    // メッシュクエリ
    MeshInfo              get_mesh_info(MeshHandle handle) const;
    std::vector<MeshInfo> get_all_meshes() const;
    MeshInfo              find_mesh(const std::string& name) const;
    void for_each_mesh(std::function<void(const MeshInfo&)> callback) const;
    std::vector<MeshInfo> get_meshes_by_semantic(VertexSemantic semantic) const;

    // JSONエクスポート
    std::string export_json() const;
    std::string export_textures_json() const;
    std::string export_meshes_json() const;
};
```

**クエリ結果の構造体:**

```cpp
struct TextureInfo {
    TextureHandle handle;
    std::string   name;
    uint32_t      width, height, depth;
    uint32_t      mip_levels, array_layers;
    TextureFormat format;
    TextureType   type;
    size_t        gpu_bytes;
    bool          uploaded;
};

struct MeshInfo {
    MeshHandle    handle;
    std::string   name;
    VertexLayout  layout;
    uint32_t      vertex_count, index_count;
    bool          index_32bit;
    size_t        vertex_bytes, index_bytes;
    bool          uploaded;
};

struct DataSummary {
    uint32_t total_textures, total_meshes;
    size_t   total_texture_gpu_bytes, total_mesh_gpu_bytes;
    uint32_t textures_uploaded, meshes_uploaded;
};
```

---

## マテリアル

### BaseMaterialBuilder

**ヘッダ:** `pictor/material/base_material_builder.h`

Fluent API で PBR マテリアルを構築し、パスごとに最適化されたバリアントを自動生成します。

```cpp
class BaseMaterialBuilder {
public:
    // テクスチャ設定 (Fluent)
    BaseMaterialBuilder& albedo(TextureHandle tex);
    BaseMaterialBuilder& normal_map(TextureHandle tex);
    BaseMaterialBuilder& metallic_map(TextureHandle tex);
    BaseMaterialBuilder& roughness_map(TextureHandle tex);
    BaseMaterialBuilder& ao_map(TextureHandle tex);
    BaseMaterialBuilder& emissive_map(TextureHandle tex);

    // スカラー/ベクトル設定 (Fluent)
    BaseMaterialBuilder& base_color(float r, float g, float b, float a = 1.0f);
    BaseMaterialBuilder& emissive_color(float r, float g, float b);
    BaseMaterialBuilder& metallic_value(float v);
    BaseMaterialBuilder& roughness_value(float v);
    BaseMaterialBuilder& alpha_cutoff(float v);
    BaseMaterialBuilder& normal_scale(float v);
    BaseMaterialBuilder& ao_strength(float v);

    // フィーチャーオーバーライド
    BaseMaterialBuilder& enable_two_sided(bool enabled = true);
    BaseMaterialBuilder& enable_vertex_color(bool enabled = true);
    BaseMaterialBuilder& enable_alpha_test(bool enabled = true);

    // パス別フィーチャーオーバーライド
    BaseMaterialBuilder& set_pass_features(PassType pass, uint32_t features);

    // ビルド
    BuiltMaterial build(MaterialHandle handle) const;
    const MaterialDesc& descriptor() const;
};
```

### MaterialRegistry

```cpp
class MaterialRegistry {
public:
    bool register_material(BuiltMaterial&& mat);
    const BuiltMaterial* get(MaterialHandle handle) const;
    const PassMaterialVariant* variant_for(MaterialHandle handle, PassType pass) const;
    MaterialHandle allocate_handle();
    size_t count() const;
};
```

### MaterialDesc

マテリアルの完全な記述子です。パス固有のストリップ前のフル定義を保持します。

```cpp
struct MaterialDesc {
    // テクスチャバインディング
    TextureHandle albedo_texture, normal_texture, metallic_texture;
    TextureHandle roughness_texture, ao_texture, emissive_texture;

    // スカラー/ベクトルパラメータ
    float base_color[4];
    float emissive[3];
    float metallic, roughness, alpha_cutoff, normal_scale, ao_strength;

    // フィーチャーフラグ (MaterialFeature::*)
    uint32_t features;
};
```

### PassMaterialVariant

特定のレンダーパス向けにストリップされたマテリアルビューです。

```cpp
struct PassMaterialVariant {
    PassType      pass_type;
    uint32_t      features;      // このパスで有効なフィーチャー
    uint64_t      shader_key;    // パス固有のシェーダーパーミュテーションキー
    uint32_t      material_key;  // バッチングキー

    // テクスチャバインディング (不要な場合は INVALID_TEXTURE)
    TextureHandle albedo_texture, normal_texture, metallic_texture;
    TextureHandle roughness_texture, ao_texture, emissive_texture;

    // スカラーパラメータ (パスに関連する場合のみ値が設定される)
    float alpha_cutoff, base_color[4], emissive[3];
    float metallic, roughness, normal_scale, ao_strength;
};
```

### MaterialFeature フラグ

```cpp
namespace MaterialFeature {
    constexpr uint32_t ALBEDO_MAP      = 1 << 0;
    constexpr uint32_t NORMAL_MAP      = 1 << 1;
    constexpr uint32_t METALLIC_MAP    = 1 << 2;
    constexpr uint32_t ROUGHNESS_MAP   = 1 << 3;
    constexpr uint32_t AO_MAP          = 1 << 4;
    constexpr uint32_t EMISSIVE_MAP    = 1 << 5;
    constexpr uint32_t ALPHA_TEST      = 1 << 6;
    constexpr uint32_t TWO_SIDED       = 1 << 7;
    constexpr uint32_t VERTEX_COLOR    = 1 << 8;
    constexpr uint32_t METALLIC_ROUGHNESS_PACKED = 1 << 9;
    constexpr uint32_t PBR_FULL = ALBEDO_MAP | NORMAL_MAP | METALLIC_MAP
                                | ROUGHNESS_MAP | AO_MAP | EMISSIVE_MAP;
}
```

---

## GI

### GILightingSystem

**ヘッダ:** `pictor/gi/gi_lighting_system.h`

シャドウマップ(CSM)、SSAO、Light Probe を統合した GI プリパスシステムです。

```cpp
class GILightingSystem {
public:
    GILightingSystem(GPUBufferManager& buffer_manager, SceneRegistry& registry);
    GILightingSystem(GPUBufferManager& buffer_manager, SceneRegistry& registry,
                     const GIConfig& config);

    void initialize(uint32_t max_objects, uint32_t screen_width, uint32_t screen_height);
    void execute(const float4x4& camera_view, const float4x4& camera_projection);

    // ライト設定
    void set_directional_light(const DirectionalLight& light);
    void upload_probe_data(const float* sh_data, uint32_t probe_count);

    // コンフィグ
    void set_config(const GIConfig& config);
    void set_shadow_config(const ShadowMapConfig& cfg);
    void set_ssao_config(const SSAOConfig& cfg);
    void set_probe_config(const GIProbeConfig& cfg);
    void set_shadow_enabled(bool enabled);
    void set_ssao_enabled(bool enabled);
    void set_gi_probes_enabled(bool enabled);

    // リソースアクセス
    const GIResourceLayout& resource_layout() const;

    // ベイク統合
    void     set_baked_static_count(uint32_t count);
    uint32_t baked_static_count() const;

    bool is_initialized() const;
    Stats get_stats() const;
};
```

**設定構造体:**

```cpp
struct DirectionalLight {
    float3 direction;
    float  intensity;
    float3 color;
};

struct PointLight {
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
};

struct ShadowMapConfig {
    uint32_t cascade_count;      // CSM カスケード数 (1..4)
    uint32_t resolution;         // 各カスケードの解像度
    float    depth_bias, normal_bias;
    float    cascade_lambda;     // 対数分割係数
    float    max_shadow_dist;
};

struct SSAOConfig {
    uint32_t sample_count;
    float    radius, bias, intensity;
    float    falloff_start, falloff_end;
    bool     blur_enabled;
};

struct GIProbeConfig {
    float3   grid_origin, grid_spacing;
    uint32_t grid_x, grid_y, grid_z;
    float    gi_intensity, max_probe_distance;
};

struct GIConfig {
    ShadowMapConfig shadow;
    SSAOConfig      ssao;
    GIProbeConfig   probes;
    bool shadow_enabled, ssao_enabled, gi_probes_enabled;
};
```

### GIBakeSystem

**ヘッダ:** `pictor/gi/gi_bake.h`

静的オブジェクト用のオフライン GI ベイクシステムです。

```cpp
class GIBakeSystem {
public:
    GIBakeSystem(GPUBufferManager& buffer_manager,
                 SceneRegistry& registry,
                 GILightingSystem& gi_system);

    void set_config(const GIBakeConfig& config);
    void set_directional_light(const DirectionalLight& light);
    void set_bake_data_provider(IBakeDataProvider* provider);

    // ベイク操作
    GIBakeResult bake();
    GIBakeResult bake(BakeProgressCallback progress);
    void apply(const GIBakeResult& result);
    void invalidate();

    // 状態
    bool is_valid() const;
    bool is_baked() const;
    bool is_dirty() const;

    // シリアライズ
    bool save(const std::string& path, const GIBakeResult& result) const;
    GIBakeResult load(const std::string& path) const;

    Stats get_stats() const;
};
```

**ベイク設定と結果:**

```cpp
enum class BakeTarget : uint8_t {
    SHADOW_MAP, AMBIENT_OCCLUSION, PROBE_IRRADIANCE, LIGHTMAP, ALL
};

struct GIBakeConfig {
    BakeTarget          targets;
    ShadowMapConfig     shadow;
    BakeAOConfig        ao;       // sample_count, radius, bias, intensity
    GIProbeConfig       probes;
    BakeLightmapConfig  lightmap; // bounce_count, samples_per_texel, resolution
    uint32_t            workgroup_size;
};

struct GIBakeResult {
    std::vector<ObjectId>        object_ids;
    std::vector<BakedShadow>     shadows;
    std::vector<BakedAO>         ao;
    std::vector<BakedIrradiance> irradiance;
    std::vector<BakedLightmap>   lightmaps;
    uint64_t                     bake_timestamp;
    bool                         valid;
};
```

---

## サーフェス

### VulkanContext

**ヘッダ:** `pictor/surface/vulkan_context.h`

Vulkan インスタンス・デバイス・スワップチェーンを一括管理します。

```cpp
class VulkanContext {
public:
    bool initialize(ISurfaceProvider* provider, const VulkanContextConfig& cfg = {});
    void shutdown();
    bool recreate_swapchain();

    uint32_t acquire_next_image();
    bool     present(uint32_t image_index);
    void     device_wait_idle();

    bool is_initialized() const;

    // Vulkan ハンドルアクセス (PICTOR_HAS_VULKAN 定義時のみ有効)
    VkInstance       instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice         device() const;
    VkQueue          graphics_queue() const;
    VkSwapchainKHR   swapchain() const;
    VkRenderPass     default_render_pass() const;
    // ...
};
```

### GlfwSurfaceProvider

**ヘッダ:** `pictor/surface/glfw_surface_provider.h`

GLFW ウィンドウを所有する `ISurfaceProvider` の具象実装です。デモ・ツール向け。

```cpp
class GlfwSurfaceProvider : public ISurfaceProvider {
public:
    bool create(const GlfwWindowConfig& config = {});
    void destroy();

    // ISurfaceProvider 実装
    NativeWindowHandle get_native_handle() const override;
    SwapchainConfig    get_swapchain_config() const override;
    void               on_swapchain_created(uint32_t w, uint32_t h) override;
    void               poll_events() override;
    bool               should_close() const override;
    uint32_t           get_required_instance_extensions(const char** out, uint32_t max) const override;

    GLFWwindow* glfw_window() const;
};

struct GlfwWindowConfig {
    uint32_t    width     = 1280;
    uint32_t    height    = 720;
    std::string title     = "Pictor";
    bool        resizable = true;
    bool        vsync     = true;
};
```

---

## パイプライン

### PipelineProfileManager

**ヘッダ:** `pictor/pipeline/pipeline_profile.h`

ビルトインプリセット (Lite / Standard / Ultra) とカスタムプロファイルを管理します。

```cpp
class PipelineProfileManager {
public:
    void register_defaults();
    void register_profile(const PipelineProfileDef& def);
    bool set_profile(const std::string& name);

    const PipelineProfileDef& current_profile() const;
    const std::string& current_profile_name() const;
    const PipelineProfileDef* get_profile(const std::string& name) const;
    std::vector<std::string> profile_names() const;

    static PipelineProfileDef create_lite_profile();
    static PipelineProfileDef create_standard_profile();
    static PipelineProfileDef create_ultra_profile();
};
```

**プロファイル定義:**

```cpp
struct PipelineProfileDef {
    std::string                profile_name;
    RenderingPath              rendering_path;
    std::vector<RenderPassDef> render_passes;
    ShadowConfig               shadow_config;
    std::vector<PostProcessDef> post_process_stack;
    bool                       gpu_driven_enabled;
    bool                       compute_update_enabled;
    GPUDrivenConfig            gpu_driven_config;
    MemoryConfig               memory_config;
    UpdateConfig               update_config;
    ProfilerConfig             profiler_config;
    GIConfig                   gi_config;
    uint32_t                   max_lights;
    uint8_t                    msaa_samples;
};
```

### RenderPassScheduler

**ヘッダ:** `pictor/pipeline/render_pass_scheduler.h`

プロファイルに基づくパス実行順序の決定と実行を担当します。

```cpp
class RenderPassScheduler {
public:
    explicit RenderPassScheduler(const PipelineProfileDef& profile);

    void reconfigure(const PipelineProfileDef& profile);
    void register_custom_pass(ICustomRenderPass* pass);
    void set_material_registry(const MaterialRegistry* registry);

    void execute(const BatchBuilder& batch_builder,
                 const CullingSystem& culling,
                 GPUDrivenPipeline* gpu_pipeline);

    const std::vector<RenderPassDef>& pass_order() const;
    uint32_t pass_count() const;
};
```

### CommandEncoder

**ヘッダ:** `pictor/pipeline/command_encoder.h`

レンダーバッチから DrawCommand を生成し、コマンドバッファに記録します。

```cpp
class CommandEncoder {
public:
    void encode(const std::vector<RenderBatch>& batches,
                PassType pass_type, FrameAllocator& allocator);
    void encode(const std::vector<RenderBatch>& batches,
                PassType pass_type, FrameAllocator& allocator,
                const MaterialRegistry* material_registry);

    void encode_compute(uint32_t group_x, uint32_t group_y, uint32_t group_z,
                        uint64_t shader_key);
    void encode_indirect(uint32_t indirect_offset, uint32_t max_draw_count,
                         uint64_t shader_key);

    const std::vector<DrawCommand>& commands() const;
    void reset();

    uint32_t draw_call_count() const;
    uint64_t triangle_count() const;
};
```

---

## シーン管理

### SceneRegistry

**ヘッダ:** `pictor/scene/scene_registry.h`

オブジェクトプールと SoA ストリームを管理します。Static / Dynamic / GPU Driven の3プールを保持します。

### ObjectClassifier

**ヘッダ:** `pictor/scene/object_classifier.h`

`ObjectDescriptor` のフラグに基づき、登録時にオブジェクトを適切なプールに自動分類します。

---

## メモリ

### MemorySubsystem

**ヘッダ:** `pictor/memory/memory_subsystem.h`

Frame Allocator（バンプ）、Pool Allocator、GPU Memory Allocator を統合管理します。

### FrameAllocator

**ヘッダ:** `pictor/memory/frame_allocator.h`

フレーム単位のバンプアロケータです。BeginFrame でリセットされ、フレーム内の一時データ確保に使用します。

---

## プロファイラ

### Profiler

**ヘッダ:** `pictor/profiler/profiler.h`

FPS、パス別 GPU/CPU 時間、メモリ統計を収集します。オーバーレイUI表示にも対応しています。

### DataExporter

**ヘッダ:** `pictor/profiler/data_exporter.h`

プロファイリングデータを JSON、Chrome Tracing 形式、CSV にエクスポートします。
