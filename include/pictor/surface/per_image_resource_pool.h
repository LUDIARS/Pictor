#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pictor {

/// swapchain image 数に追従する per-image リソースの **唯一の所有者**。
///
/// swapchain を再生成すると image 数は変わりうる (driver / present mode /
/// surface capabilities 次第で minImageCount がずれる)。 その際に
///   - command buffer
///   - render-finished セマフォ
///   - images-in-flight fence 追跡 (借用ハンドル、 所有しない)
/// の 3 本が同じ長さで作り直されないと、 consumer が
/// `command_buffers()[image_index]` を触った瞬間に配列外アクセスになる。
/// この class はその「同じ長さで作り直す」責務だけを持ち、 VulkanContext は
/// rebuild() を 1 回呼ぶだけでよい。
///
/// 再構築は **replacement-before-retire**:
///   1. 新しい一式を丸ごと作る
///   2. 途中で 1 つでも失敗したら、 新しい側だけを逆順に巻き戻して false を返す
///      (現行の一式は一切触らないので、 呼び出し側は失敗しても直前の状態を
///       そのまま使い続けられる = 半端な状態を公開しない)
///   3. 全部作れたときだけ、 旧一式を逆順に解放して差し替える
/// 旧一式の解放は GPU がそれらを使い終わっている前提なので、 呼び出し側は
/// rebuild() の前に device idle を取ること。
///
/// Backend concept (Vulkan 非依存にするための policy。 テストは fake を渡す):
///   using CommandBuffer; using Semaphore; using Fence;
///   static CommandBuffer null_command_buffer();
///   static Semaphore     null_semaphore();
///   static Fence         null_fence();
///   bool allocate_command_buffers(uint32_t count, CommandBuffer* out);
///   void free_command_buffers(const CommandBuffer* first, uint32_t count);
///   bool create_semaphore(Semaphore* out);
///   void destroy_semaphore(Semaphore sem);
template <class Backend>
class PerImageResourcePool {
public:
    using CommandBuffer = typename Backend::CommandBuffer;
    using Semaphore     = typename Backend::Semaphore;
    using Fence         = typename Backend::Fence;

    PerImageResourcePool() = default;
    ~PerImageResourcePool() = default;

    PerImageResourcePool(const PerImageResourcePool&)            = delete;
    PerImageResourcePool& operator=(const PerImageResourcePool&) = delete;

    /// image_count 個ぶんの per-image リソースを作り直す。
    /// 成功時のみ内容が入れ替わる。 失敗時は既存の一式が保たれ false を返す。
    /// image_count == 0 は「全部畳む」を意味し、 常に成功する。
    bool rebuild(Backend& backend, uint32_t image_count) {
        if (image_count == 0) {
            destroy(backend);
            return true;
        }

        std::vector<CommandBuffer> new_cmds(image_count, Backend::null_command_buffer());
        if (!backend.allocate_command_buffers(image_count, new_cmds.data())) {
            return false;
        }

        std::vector<Semaphore> new_sems;
        new_sems.reserve(image_count);
        for (uint32_t i = 0; i < image_count; ++i) {
            Semaphore sem = Backend::null_semaphore();
            if (!backend.create_semaphore(&sem)) {
                // 新しい側だけを逆順に巻き戻す。 現行の一式は無傷のまま。
                destroy_set(backend, new_cmds, new_sems);
                return false;
            }
            new_sems.push_back(sem);
        }

        // ここまで来たら新一式は完成。 旧一式を逆順に解放してから差し替える。
        destroy_set(backend, command_buffers_, render_finished_sems_);
        command_buffers_     = std::move(new_cmds);
        render_finished_sems_ = std::move(new_sems);
        // images-in-flight は借用ハンドルなので破棄せず、 新しい長さで初期化する。
        in_flight_fences_.assign(image_count, Backend::null_fence());
        return true;
    }

    /// 所有している per-image リソースを逆順に解放して空にする。
    void destroy(Backend& backend) {
        destroy_set(backend, command_buffers_, render_finished_sems_);
        command_buffers_.clear();
        render_finished_sems_.clear();
        in_flight_fences_.clear();
    }

    uint32_t image_count() const {
        return static_cast<uint32_t>(command_buffers_.size());
    }

    bool empty() const { return command_buffers_.empty(); }

    /// consumer へ渡す command buffer 配列。 長さは常に image_count()。
    const std::vector<CommandBuffer>& command_buffers() const { return command_buffers_; }

    /// 範囲外なら null を返す (呼び出し側が配列外を踏まないようにする)。
    Semaphore render_finished(uint32_t image_index) const {
        if (image_index >= render_finished_sems_.size()) return Backend::null_semaphore();
        return render_finished_sems_[image_index];
    }

    /// その image を最後に使った flight の fence (借用)。 範囲外なら null。
    Fence in_flight_fence(uint32_t image_index) const {
        if (image_index >= in_flight_fences_.size()) return Backend::null_fence();
        return in_flight_fences_[image_index];
    }

    /// 借用 fence を記録する。 範囲外は無視する。
    void set_in_flight_fence(uint32_t image_index, Fence fence) {
        if (image_index >= in_flight_fences_.size()) return;
        in_flight_fences_[image_index] = fence;
    }

private:
    /// 生成順 (command buffer → セマフォ 0..n-1) の逆順に解放する。
    static void destroy_set(Backend& backend,
                            std::vector<CommandBuffer>& cmds,
                            std::vector<Semaphore>& sems) {
        for (std::size_t i = sems.size(); i-- > 0; ) {
            if (sems[i] != Backend::null_semaphore()) backend.destroy_semaphore(sems[i]);
        }
        sems.clear();
        if (!cmds.empty()) {
            backend.free_command_buffers(cmds.data(), static_cast<uint32_t>(cmds.size()));
        }
        cmds.clear();
    }

    std::vector<CommandBuffer> command_buffers_;
    std::vector<Semaphore>     render_finished_sems_;
    /// 借用 (所有しない)。 長さだけ command_buffers_ に一致させる。
    std::vector<Fence>         in_flight_fences_;
};

} // namespace pictor
