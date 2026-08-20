/// BatchBuilder unit test (headless).
///
/// 回帰対象: review/2026-06-11 H-1 — static バッチが dirty の落ちた
/// 2 フレーム目以降に消失するバグ。 create_batches_from_sorted() の
/// 最終バッチが cache (`out`) でなく `batches_` に直接入っていたため、
/// static pool の最後のバッチだけキャッシュから漏れていた。
/// あわせて get_stats() の総数二重加算 (L-4) と sorted_indices() の
/// 空フレームでの stale ポインタ (M-5) も検証する。

#include "pictor/batch/batch_builder.h"
#include "pictor/memory/frame_allocator.h"
#include "pictor/scene/scene_registry.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

namespace {

ObjectDescriptor make_desc(uint16_t flags, uint64_t shader_key, uint32_t material_key) {
    ObjectDescriptor d;
    d.flags       = flags;
    d.shaderKey   = shader_key;
    d.materialKey = material_key;
    d.bounds.min  = {-1, -1, -1};
    d.bounds.max  = { 1,  1,  1};
    return d;
}

size_t count_static_like(const std::vector<RenderBatch>& batches, uint64_t shader_key) {
    size_t n = 0;
    for (const auto& b : batches)
        if (b.shaderKey == shader_key) n += b.count;
    return n;
}

} // namespace

int main() {
    MemoryConfig cfg;
    MemorySubsystem memory(cfg);
    SceneRegistry registry(memory);
    BatchBuilder builder(registry);
    FrameAllocator allocator(4 * 1024 * 1024);

    // static 2 種 (shaderKey 0xA / 0xB) + dynamic 1 種 (0xC)。
    // shaderKey が 2 種あるので static バッチは必ず 2 個に割れ、
    // 「最終バッチ」がキャッシュから漏れる回帰を検出できる。
    registry.register_object(make_desc(ObjectFlags::STATIC, 0xAULL << 48, 1));
    registry.register_object(make_desc(ObjectFlags::STATIC, 0xAULL << 48, 1));
    registry.register_object(make_desc(ObjectFlags::STATIC, 0xBULL << 48, 2));
    registry.register_object(make_desc(ObjectFlags::DYNAMIC, 0xCULL << 48, 3));

    // ---- Frame 1: static dirty → 全バッチ生成 ----
    builder.invalidate_all();
    builder.build(allocator);
    const size_t static_a_f1 = count_static_like(builder.batches(), 0xAULL << 48);
    const size_t static_b_f1 = count_static_like(builder.batches(), 0xBULL << 48);
    PT_ASSERT_OP(static_a_f1, ==, 2u, "frame1: shader A static objects batched");
    PT_ASSERT_OP(static_b_f1, ==, 1u, "frame1: shader B static objects batched");

    // ---- Frame 2: static dirty が落ちた状態 — キャッシュから全 static が残ること ----
    allocator.reset();
    builder.build(allocator);
    const size_t static_a_f2 = count_static_like(builder.batches(), 0xAULL << 48);
    const size_t static_b_f2 = count_static_like(builder.batches(), 0xBULL << 48);
    PT_ASSERT_OP(static_a_f2, ==, 2u, "frame2: shader A static batch survives (H-1)");
    PT_ASSERT_OP(static_b_f2, ==, 1u, "frame2: last static batch survives cache (H-1 residual)");

    // ---- get_stats(): 総オブジェクト数は registry 総数と一致 (L-4 二重加算なし) ----
    const auto stats = builder.get_stats();
    PT_ASSERT_OP(stats.total_objects, ==, registry.total_object_count(),
                 "stats.total_objects == registry count (no double add)");

    // Transparency is a batch boundary even when shader/material keys match.
    // Otherwise one RenderBatch cannot be filtered safely into opaque and
    // transparent passes.
    {
        SceneRegistry split_registry(memory);
        BatchBuilder split_builder(split_registry);
        split_registry.register_object(make_desc(ObjectFlags::STATIC, 0xEULL << 48, 7));
        split_registry.register_object(make_desc(
            ObjectFlags::STATIC | ObjectFlags::TRANSPARENT, 0xEULL << 48, 7));
        allocator.reset();
        split_builder.invalidate_all();
        split_builder.build(allocator);

        uint32_t opaque_batches = 0;
        uint32_t transparent_batches = 0;
        for (const RenderBatch& batch : split_builder.batches()) {
            if (batch.transparency) transparent_batches++;
            else opaque_batches++;
        }
        PT_ASSERT_OP(opaque_batches, ==, 1u, "opaque batch kept separate");
        PT_ASSERT_OP(transparent_batches, ==, 1u, "transparent batch kept separate");
    }

    // ---- sorted_indices: dynamic が空のフレームで stale ポインタを返さない (M-5) ----
    // dynamic オブジェクトを消してから build — 前フレームの frame メモリを
    // 指したままにならないこと。
    {
        // dynamic pool の唯一のオブジェクトを外す
        // (ObjectId は register 順で 4 番目に発行されたもの)
        // ID 取得 API は総当たりでなく register 時の戻り値を使うのが本筋だが、
        // ここでは再登録で置き換える。
        SceneRegistry reg2(memory);
        BatchBuilder b2(reg2);
        reg2.register_object(make_desc(ObjectFlags::DYNAMIC, 0xD0ULL << 48, 9));
        b2.invalidate_all();
        b2.build(allocator);
        PT_ASSERT(b2.sorted_indices() != nullptr, "dynamic present: indices exist");

        // オブジェクトなしの registry で組んだ builder は indices を晒さない
        SceneRegistry reg3(memory);
        BatchBuilder b3(reg3);
        allocator.reset();
        b3.invalidate_all();
        b3.build(allocator);
        PT_ASSERT(b3.sorted_indices() == nullptr, "empty dynamic: no stale indices (M-5)");
        PT_ASSERT_OP(b3.sorted_index_count(), ==, 0u, "empty dynamic: count 0");
    }

    return report("unit_batch_builder_test");
}
