# Pictor 外部インタフェース定義

本ドキュメントは、Pictor が外部に公開するインタフェース（抽象基底クラス、コールバック）の仕様を記述します。
ホストアプリケーションやプラグインがこれらのインタフェースを実装することで、Pictor の動作をカスタマイズできます。

---

## 目次

1. [ISurfaceProvider](#isurfaceprovider)
2. [ICustomRenderPass](#icustomrenderpass)
3. [IUpdateCallback](#iupdatecallback)
4. [ICullingProvider](#icullingprovider)
5. [IBatchPolicy](#ibatchpolicy)
6. [IJobDispatcher](#ijobdispatcher)
7. [IBakeDataProvider](#ibakedataprovider)
8. [BakeProgressCallback](#bakeprogresscallback)

---

## ISurfaceProvider

**ヘッダ:** `pictor/surface/surface_provider.h`

Pictor をウィンドウシステムから分離するための抽象インタフェースです。ホストアプリケーションが自身のウィンドウを所有し、Pictor はこのインタフェースを通じて Vulkan サーフェスを生成します。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `get_native_handle()` | `NativeWindowHandle` | **必須**。VkSurfaceKHR 生成に必要なプラットフォーム固有ハンドルを返す |
| `get_swapchain_config()` | `SwapchainConfig` | **必須**。希望するスワップチェーン設定（サイズ、VSync、イメージ数）を返す |
| `on_swapchain_created(w, h)` | `void` | スワップチェーン作成/再作成時に呼ばれる通知。実際のサイズが渡される |
| `poll_events()` | `void` | 毎フレーム呼ばれる。GLFW 等のイベントポーリングに使用 |
| `should_close()` | `bool` | アプリケーションを終了すべきかどうか |
| `get_required_instance_extensions(out, max)` | `uint32_t` | Vulkan インスタンス拡張名を返す（件数を返す） |

### NativeWindowHandle

プラットフォームごとのウィンドウハンドルを格納する共用体です。

| Type | プラットフォーム | フィールド |
|------|----------------|-----------|
| `WIN32` | Windows | `hwnd`, `hinstance` |
| `XLIB` | Linux (X11) | `display`, `window` |
| `XCB` | Linux (XCB) | `connection`, `window` |
| `WAYLAND` | Linux (Wayland) | `display`, `surface` |
| `COCOA` | macOS | `ns_view` |
| `ANDROID` | Android | `native_window` |

### SwapchainConfig

```cpp
struct SwapchainConfig {
    uint32_t width       = 0;
    uint32_t height      = 0;
    bool     vsync       = true;
    uint32_t image_count = 3;   // トリプルバッファリング
};
```

### 実装例

```cpp
class MyWindowProvider : public pictor::ISurfaceProvider {
    HWND hwnd_;
    HINSTANCE hinstance_;
public:
    NativeWindowHandle get_native_handle() const override {
        NativeWindowHandle h{};
        h.type = NativeWindowHandle::Type::WIN32;
        h.win32.hwnd      = hwnd_;
        h.win32.hinstance = hinstance_;
        return h;
    }

    SwapchainConfig get_swapchain_config() const override {
        return {1920, 1080, true, 3};
    }
};
```

---

## ICustomRenderPass

**ヘッダ:** `pictor/pipeline/render_pass_scheduler.h`

カスタムレンダーパスを定義するためのインタフェースです。`RenderPassScheduler` に登録することで、標準パスと合わせて実行されます。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `name()` | `const char*` | **必須**。パス名を返す |
| `type()` | `PassType` | **必須**。パスタイプを返す |
| `required_streams()` | `vector<string>` | このパスが必要とする SoA ストリーム名（プリフェッチヒント） |
| `execute(batches)` | `void` | **必須**。レンダーバッチを受け取りパスを実行する |

### 実装例

```cpp
class OutlinePass : public pictor::ICustomRenderPass {
public:
    const char* name() const override { return "Outline"; }
    PassType type() const override { return PassType::CUSTOM; }

    void execute(const std::vector<RenderBatch>& batches) override {
        // 選択オブジェクトのアウトライン描画
    }
};

renderer.register_custom_pass(&outline_pass);
```

---

## IUpdateCallback

**ヘッダ:** `pictor/update/update_scheduler.h`

フレーム更新時にカスタムロジックを注入するためのコールバックインタフェースです。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `on_pre_update(pool_type, count)` | `void` | プール更新前に呼ばれる |
| `on_post_update(pool_type, count)` | `void` | プール更新後に呼ばれる |
| `on_compute_dispatch(count)` | `void` | GPU Compute 更新ディスパッチ後に呼ばれる |

### 登録

```cpp
renderer.set_update_callback(&my_callback);
```

---

## ICullingProvider

**ヘッダ:** `pictor/culling/culling_system.h`

カリングアルゴリズムをカスタマイズするためのインタフェースです。デフォルトの Frustum + Hi-Z カリングを独自実装に置き換えられます。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `cull(bounds, visibility, count, frustum)` | `uint32_t` | カリングを実行し可視オブジェクト数を返す |
| `supports_gpu_culling()` | `bool` | GPU カリングをサポートするかどうか |

### 登録

```cpp
renderer.set_culling_provider(&my_culler);
```

---

## IBatchPolicy

**ヘッダ:** `pictor/batch/batch_builder.h`

バッチング戦略をカスタマイズするためのインタフェースです。ソートキー生成やバッチ分割の方法をオーバーライドできます。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `compute_sort_key(shader_key, material_key, depth)` | `uint64_t` | ソートキーを計算する |
| `should_break_batch(prev, current)` | `bool` | バッチを分割すべきかどうかを判定する |

### 登録

```cpp
renderer.set_batch_policy(&my_policy);
```

---

## IJobDispatcher

**ヘッダ:** `pictor/update/job_dispatcher.h`

並列ジョブ実行をカスタマイズするためのインタフェースです。アプリケーション固有のスレッドプールと統合できます。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `dispatch(job, count)` | `void` | ジョブを並列実行キューにディスパッチする |
| `wait_all()` | `void` | 全ジョブの完了を待機する |
| `worker_count()` | `uint32_t` | 利用可能なワーカースレッド数を返す |

### 登録

```cpp
renderer.set_job_dispatcher(&my_dispatcher);
```

---

## IBakeDataProvider

**ヘッダ:** `pictor/gi/gi_bake.h`

GIベイク時に追加データを供給するためのインタフェースです。カスタムライトソースやエミッシブサーフェスの情報を提供できます。

### メソッド

| メソッド | 戻り値 | 説明 |
|---------|--------|------|
| `get_bake_point_lights()` | `vector<PointLight>` | ベイク用の追加ポイントライトを返す |
| `get_emissive_objects()` | `vector<ObjectId>` | 発光オブジェクトのIDリストを返す |
| `on_bake_begin()` | `void` | ベイク開始前のデータ準備コールバック |
| `on_bake_complete(result)` | `void` | ベイク完了後のコールバック |

### 登録

```cpp
renderer.set_bake_data_provider(&my_provider);
```

---

## BakeProgressCallback

**ヘッダ:** `pictor/gi/gi_bake.h`

GIベイクの進捗を報告する関数型です。`false` を返すとベイクをキャンセルできます。

```cpp
using BakeProgressCallback = std::function<bool(float progress, const char* stage)>;
```

### 使用例

```cpp
GIBakeResult result = renderer.bake_static_gi([](float progress, const char* stage) {
    printf("[%.0f%%] %s\n", progress * 100.0f, stage);
    return true; // false を返すとキャンセル
});
```
