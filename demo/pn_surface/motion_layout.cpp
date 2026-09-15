#include "motion_layout.h"
#include <stdexcept>

namespace pictor_pn {
namespace {
using pictor::demo::kUntexturedSubmesh;
void check_patch(const pictor::demo::PolynomialPatch& patch,size_t submesh_count) {
    if (patch.submesh==kUntexturedSubmesh) {
        if (!patch.constant_color) throw std::runtime_error("Untextured motion patch must use a constant color");
        return;
    }
    if (patch.submesh<0 || size_t(patch.submesh)>=submesh_count)
        throw std::runtime_error("Motion patch submesh outside source materials");
    if (patch.constant_color) throw std::runtime_error("Textured motion patch cannot also be constant color");
}
void push(std::vector<PatchDrawRange>& ranges,PatchDrawRange range) {
    if (!range.count) return;
    if (ranges.size()==kMaxDrawRanges) throw std::runtime_error("Motion patch layout needs too many draw ranges; group patches by submesh");
    ranges.push_back(range);
}
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
std::vector<PatchDrawRange> motion_draw_ranges(std::span<const pictor::demo::MotionSubmesh> submeshes,
    std::span<const pictor::demo::PolynomialPatch> patches,uint32_t source_count,bool replaces_source) {
    if (patches.size()<source_count || patches.size()-source_count>kMaxAppendedPatches)
        throw std::runtime_error("Invalid polynomial motion patch count");
    if (replaces_source && patches.size()==source_count)
        throw std::runtime_error("Replacement motion surface has no appended patches");
    std::vector<PatchDrawRange> ranges;
    if (!replaces_source)
        for (uint32_t i=0;i<submeshes.size();++i) {
            const auto& sub=submeshes[i];
            if (size_t{sub.first_patch}+sub.patch_count>source_count) throw std::runtime_error("Source submesh outside patches");
            push(ranges,{i,sub.first_patch,sub.patch_count});
        }
    const auto texture_of=[](int32_t submesh) { return submesh==kUntexturedSubmesh?0u:uint32_t(submesh); };
    size_t start=source_count;
    for (size_t i=source_count;i<=patches.size();++i) {
        if (i<patches.size()) {
            check_patch(patches[i],submeshes.size());
            if (i==start || patches[i].submesh==patches[start].submesh) continue;
        }
        if (i>start) push(ranges,{texture_of(patches[start].submesh),uint32_t(start),uint32_t(i-start)});
        start=i;
    }
    return ranges;
}
}
