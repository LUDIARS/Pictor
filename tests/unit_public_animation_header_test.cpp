#include "pictor/pictor.h"
#include "test_common.h"

#include <type_traits>

using namespace pictor_test;

int main() {
    static_assert(std::is_class_v<pictor::MontagePlayer>);
    static_assert(std::is_same_v<decltype(pictor::MontageDescriptor::clip), pictor::AnimationClipHandle>);

    pictor::MontageDescriptor desc;
    desc.sections.push_back(pictor::MontageSection{"main", 0.0f, 1.0f, ""});
    desc.notifies.push_back(pictor::MontageNotify{0.5f, "hit"});

    PT_ASSERT_OP(desc.sections.size(), ==, 1u, "MontageSection visible through pictor.h");
    PT_ASSERT_OP(desc.notifies.size(), ==, 1u, "MontageNotify visible through pictor.h");

    return report("unit_public_animation_header_test");
}
