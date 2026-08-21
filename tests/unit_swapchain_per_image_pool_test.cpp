/// swapchain image 数が変わったときの per-image リソース再構築テスト
/// (KD-PICTOR-001)。
///
/// 再現する欠陥: 以前は command buffer / render-finished セマフォ /
/// images-in-flight 追跡が initialize() 時の image 数のまま再利用され、
/// recreate_swapchain() で image 数が増えると consumer の
/// command_buffers()[image_index] が配列外を踏んだ。
///
/// 不変条件:
///   1. rebuild(n) 後、 3 種類の per-image 配列は必ず長さ n で揃う (増減とも)
///   2. 旧リソースは生成順の逆順で解放される (セマフォ n-1..0 → command buffer)
///   3. 途中で失敗したら現行の一式は無傷のまま残り、 新しい側だけ巻き戻る
///      (成功するまで新旧を切り替えない)
///   4. 借用 fence 追跡は rebuild でクリアされ、 範囲外 index は null を返す
///
/// Vulkan device を張らずに検証するため、 PerImageResourcePool の backend policy
/// に fake を渡す (本番の policy は VulkanPerImageBackend)。

#include "pictor/surface/per_image_resource_pool.h"
#include "test_common.h"

#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

/// 生成 / 解放を記録するだけの fake backend。
/// ハンドルは 1 始まりの整数で、 0 を null 扱いにする。
class FakeBackend {
public:
    using CommandBuffer = int;
    using Semaphore     = int;
    using Fence         = int;

    static CommandBuffer null_command_buffer() { return 0; }
    static Semaphore     null_semaphore()      { return 0; }
    static Fence         null_fence()          { return 0; }

    /// 次の allocate_command_buffers を失敗させる。
    bool fail_command_buffers = false;
    /// この本数を作ったあとの create_semaphore を失敗させる (負値で無効)。
    int  fail_semaphore_after = -1;

    bool allocate_command_buffers(uint32_t count, CommandBuffer* out) {
        if (fail_command_buffers) return false;
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = next_handle_++;
            live_command_buffers_++;
        }
        return true;
    }

    void free_command_buffers(const CommandBuffer* first, uint32_t count) {
        events_.push_back("free_cmds:" + std::to_string(count));
        for (uint32_t i = 0; i < count; ++i) {
            freed_.push_back(first[i]);
            live_command_buffers_--;
        }
    }

    bool create_semaphore(Semaphore* out) {
        if (fail_semaphore_after >= 0 && created_semaphores_ >= fail_semaphore_after) {
            return false;
        }
        *out = next_handle_++;
        created_semaphores_++;
        live_semaphores_++;
        return true;
    }

    void destroy_semaphore(Semaphore sem) {
        events_.push_back("destroy_sem:" + std::to_string(sem));
        freed_.push_back(sem);
        live_semaphores_--;
    }

    int live_command_buffers() const { return live_command_buffers_; }
    int live_semaphores()      const { return live_semaphores_; }
    const std::vector<int>&         freed()  const { return freed_; }
    const std::vector<std::string>& events() const { return events_; }

    void clear_log() { freed_.clear(); events_.clear(); }
    void reset_failures() { fail_command_buffers = false; fail_semaphore_after = -1; }

private:
    int next_handle_        = 1;
    int created_semaphores_ = 0;
    int live_command_buffers_ = 0;
    int live_semaphores_      = 0;
    std::vector<int>         freed_;
    std::vector<std::string> events_;
};

using Pool = PerImageResourcePool<FakeBackend>;

/// pool の 3 配列が同じ長さで揃っているか。 images-in-flight は範囲内が
/// 参照でき、 範囲外が null になることで長さを確認する。
void check_sized(const Pool& pool, uint32_t expected, const char* what) {
    PT_ASSERT_OP(pool.image_count(), ==, expected, what);
    PT_ASSERT_OP(static_cast<uint32_t>(pool.command_buffers().size()), ==, expected,
                 "command buffer 配列は image 数と一致する");
    if (expected > 0) {
        PT_ASSERT(pool.render_finished(expected - 1) != FakeBackend::null_semaphore(),
                  "最後の image の render-finished セマフォが存在する");
    }
    PT_ASSERT(pool.render_finished(expected) == FakeBackend::null_semaphore(),
              "image 数ちょうどの index は範囲外で null セマフォ");
    PT_ASSERT(pool.in_flight_fence(expected) == FakeBackend::null_fence(),
              "image 数ちょうどの index は範囲外で null fence");
}

// 起動時 2 枚 → 再生成で 3 枚に増えるケース (本来の欠陥の再現)。
void test_rebuild_grows_all_per_image_resources() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 2), "初期構築 (image 2 枚) が成功する");
    check_sized(pool, 2, "初期構築後の image 数");

    PT_ASSERT(pool.rebuild(backend, 3), "image 3 枚への再構築が成功する");
    check_sized(pool, 3, "再構築後の image 数は新しい image 数");

    // 増えた分まで含めて全部の index が有効なハンドルを返す
    // (旧実装ではここが配列外だった)。
    for (uint32_t i = 0; i < 3; ++i) {
        PT_ASSERT(pool.command_buffers()[i] != FakeBackend::null_command_buffer(),
                  "全 image に command buffer が割り当たっている");
        PT_ASSERT(pool.render_finished(i) != FakeBackend::null_semaphore(),
                  "全 image に render-finished セマフォが割り当たっている");
    }

    PT_ASSERT_OP(backend.live_command_buffers(), ==, 3, "旧 command buffer は解放済み");
    PT_ASSERT_OP(backend.live_semaphores(), ==, 3, "旧セマフォは解放済み");
}

// 起動時 3 枚 → 再生成で 2 枚に減るケース。
void test_rebuild_shrinks_all_per_image_resources() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 3), "初期構築 (image 3 枚) が成功する");
    PT_ASSERT(pool.rebuild(backend, 2), "image 2 枚への再構築が成功する");
    check_sized(pool, 2, "縮んだあとの image 数");

    PT_ASSERT_OP(backend.live_command_buffers(), ==, 2, "余った command buffer は解放される");
    PT_ASSERT_OP(backend.live_semaphores(), ==, 2, "余ったセマフォは解放される");
}

// 旧一式は「セマフォを逆順 → command buffer」の順で解放されること。
void test_old_resources_released_in_reverse_order() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 3), "初期構築が成功する");
    const int sem0 = pool.render_finished(0);
    const int sem1 = pool.render_finished(1);
    const int sem2 = pool.render_finished(2);

    backend.clear_log();
    PT_ASSERT(pool.rebuild(backend, 4), "再構築が成功する");

    const auto& ev = backend.events();
    PT_ASSERT_OP(static_cast<int>(ev.size()), ==, 4,
                 "旧一式の解放イベントはセマフォ 3 本 + command buffer 1 回");
    if (ev.size() == 4) {
        PT_ASSERT(ev[0] == "destroy_sem:" + std::to_string(sem2), "セマフォは末尾から解放");
        PT_ASSERT(ev[1] == "destroy_sem:" + std::to_string(sem1), "セマフォは逆順に解放");
        PT_ASSERT(ev[2] == "destroy_sem:" + std::to_string(sem0), "セマフォは先頭が最後");
        PT_ASSERT(ev[3] == "free_cmds:3", "command buffer はセマフォのあとに解放");
    }
}

// command buffer の確保に失敗したら、 現行の一式は無傷のまま残ること。
void test_command_buffer_failure_keeps_existing_set() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 2), "初期構築が成功する");
    const int cmd0 = pool.command_buffers()[0];
    const int sem0 = pool.render_finished(0);

    backend.clear_log();
    backend.fail_command_buffers = true;
    PT_ASSERT(!pool.rebuild(backend, 4), "確保に失敗したら rebuild は false");
    backend.reset_failures();

    check_sized(pool, 2, "失敗しても image 数は元のまま");
    PT_ASSERT_OP(pool.command_buffers()[0], ==, cmd0, "既存 command buffer は差し替わらない");
    PT_ASSERT_OP(pool.render_finished(0), ==, sem0, "既存セマフォは差し替わらない");
    PT_ASSERT_OP(static_cast<int>(backend.freed().size()), ==, 0,
                 "失敗時に既存リソースを解放してはならない");
}

// セマフォ生成が途中で失敗したら、 新しい側だけ巻き戻して現行を保つこと。
void test_semaphore_failure_unwinds_new_set_only() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 2), "初期構築が成功する");
    const int cmd1 = pool.command_buffers()[1];

    backend.clear_log();
    backend.fail_semaphore_after = 3;  // 既に 2 本作ってあるので新規セマフォ 2 本目で止まる
    PT_ASSERT(!pool.rebuild(backend, 4), "セマフォ生成に失敗したら rebuild は false");
    backend.reset_failures();

    check_sized(pool, 2, "失敗しても image 数は元のまま");
    PT_ASSERT_OP(pool.command_buffers()[1], ==, cmd1, "既存 command buffer は差し替わらない");
    // 新しい側だけが巻き戻る: 新規 command buffer 4 本 + 作れたセマフォ 1 本。
    PT_ASSERT_OP(backend.live_command_buffers(), ==, 2, "新しい command buffer は巻き戻される");
    PT_ASSERT_OP(backend.live_semaphores(), ==, 2, "作りかけのセマフォは巻き戻される");
}

// images-in-flight 追跡は image 数に追従し、 rebuild でクリアされること。
void test_in_flight_tracking_follows_image_count() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 2), "初期構築が成功する");
    pool.set_in_flight_fence(1, 77);
    PT_ASSERT_OP(pool.in_flight_fence(1), ==, 77, "借用 fence を記録できる");

    PT_ASSERT(pool.rebuild(backend, 3), "image 3 枚へ再構築する");
    PT_ASSERT_OP(pool.in_flight_fence(1), ==, FakeBackend::null_fence(),
                 "再構築で借用 fence はクリアされる");
    pool.set_in_flight_fence(2, 88);
    PT_ASSERT_OP(pool.in_flight_fence(2), ==, 88, "増えた image も追跡できる");

    // 範囲外 index への書き込みは無視され、 クラッシュしない。
    pool.set_in_flight_fence(99, 99);
    PT_ASSERT_OP(pool.in_flight_fence(99), ==, FakeBackend::null_fence(),
                 "範囲外 index は無視される");
}

// image 0 枚 (surface 消失時など) は全部畳んで成功すること。
void test_rebuild_to_zero_releases_everything() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 3), "初期構築が成功する");
    PT_ASSERT(pool.rebuild(backend, 0), "image 0 枚への再構築は成功する");
    PT_ASSERT(pool.empty(), "image 0 枚なら pool は空");
    check_sized(pool, 0, "image 0 枚");
    PT_ASSERT_OP(backend.live_command_buffers(), ==, 0, "command buffer は全解放");
    PT_ASSERT_OP(backend.live_semaphores(), ==, 0, "セマフォは全解放");
}

// destroy() は所有リソースを全部返し、 二重呼び出しでも安全なこと。
void test_destroy_is_idempotent() {
    FakeBackend backend;
    Pool pool;

    PT_ASSERT(pool.rebuild(backend, 2), "初期構築が成功する");
    pool.destroy(backend);
    PT_ASSERT(pool.empty(), "destroy 後は空");
    pool.destroy(backend);
    PT_ASSERT_OP(backend.live_command_buffers(), ==, 0, "二重 destroy でも解放は 1 回ぶん");
    PT_ASSERT_OP(backend.live_semaphores(), ==, 0, "二重 destroy でもセマフォは 1 回ぶん");
}

} // namespace

int main() {
    test_rebuild_grows_all_per_image_resources();
    test_rebuild_shrinks_all_per_image_resources();
    test_old_resources_released_in_reverse_order();
    test_command_buffer_failure_keeps_existing_set();
    test_semaphore_failure_unwinds_new_set_only();
    test_in_flight_tracking_follows_image_count();
    test_rebuild_to_zero_releases_everything();
    test_destroy_is_idempotent();
    return report("unit_swapchain_per_image_pool_test");
}
