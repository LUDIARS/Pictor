/// VisusDesc → SceneRegistry 経由で N 個の ObjectDescriptor を発行する slot 展開テスト。

#include "pictor/visus/visus.h"
#include "pictor/visus/visus_registry.h"
#include "pictor/visus/visus_instantiator.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/memory/memory_subsystem.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);

    // 1. materials が空 → 1 個だけ発行、 material は INVALID_MATERIAL。
    {
        VisusDesc d;
        d.name          = "empty_mat";
        d.geometry_kind = VisusGeometryKind::PRIMITIVE;
        d.mesh          = 42;
        d.default_flags = ObjectFlags::STATIC;
        d.layer         = 1;
        d.initial_lod   = 3;

        const float4x4 xf = float4x4::identity();
        const AABB     bb = {{0, 0, 0}, {1, 1, 1}};

        auto ids = instantiate_visus(scene, d, xf, bb);
        PT_ASSERT_OP(ids.size(), ==, size_t{1}, "no materials -> 1 ObjectDescriptor");

        auto loc = scene.find_object(ids[0]);
        PT_ASSERT(loc.valid, "registered object is locatable");
    }

    // 2. materials が 3 個 → 3 個発行、 各 ObjectDescriptor は別 material handle 持つ。
    {
        VisusDesc d;
        d.name          = "multi_slot";
        d.geometry_kind = VisusGeometryKind::MODEL;
        d.mesh          = 7;
        d.default_flags = ObjectFlags::DYNAMIC;

        for (uint32_t i = 0; i < 3; ++i) {
            VisusMaterialSlot s;
            s.slot_name = "slot" + std::to_string(i);
            s.material  = 100 + i;
            d.materials.push_back(s);
        }

        const float4x4 xf = float4x4::identity();
        const AABB     bb = {{-1, -1, -1}, {1, 1, 1}};

        auto ids = instantiate_visus(scene, d, xf, bb);
        PT_ASSERT_OP(ids.size(), ==, size_t{3}, "3 slots -> 3 ObjectDescriptors");

        // ids が一意 (slot ごとに別 ObjectId が降る)
        PT_ASSERT(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2],
                  "distinct ObjectIds per slot");
    }

    // 3. VisusRegistry に登録 → handle 単調増加 + get で復元できる。
    {
        VisusRegistry reg;
        VisusDesc a; a.name = "a";
        VisusDesc b; b.name = "b";

        VisusHandle ha = reg.register_visus(a);
        VisusHandle hb = reg.register_visus(b);
        PT_ASSERT_OP(ha, ==, 0u, "first handle is 0");
        PT_ASSERT_OP(hb, ==, 1u, "second handle is 1");
        PT_ASSERT_OP(reg.count(), ==, size_t{2}, "registry size matches");

        const VisusDesc* got = reg.get(ha);
        PT_ASSERT(got != nullptr, "get returns non-null");
        PT_ASSERT(got->name == "a", "get returns the right entry");

        PT_ASSERT(reg.get(99) == nullptr, "out-of-range handle returns nullptr");
    }

    return report("unit_visus_instantiator_test");
}
