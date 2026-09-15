#include "motion_bridge.h"
#include "pictor/animation/skeleton.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pictor_<term-c01> {
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::initialize(pictor::demo::PolynomialMotion* motion,const PnModel& model,
                              const pictor::SkeletonDescriptor& skeleton) {
    motion_=motion;source_count_=static_cast<uint32_t>(model.patches().size());total_count_=source_count_;
    if (!motion_) return;
    const auto& mesh=model.source();
    std::vector<pictor::demo::MotionVertex> vertices(mesh.vertices.size());
    for (size_t i=0;i<vertices.size();++i) {
        const auto& s=mesh.vertices[i];auto& d=vertices[i];
        d.position={s.position[0],s.position[1],s.position[2]};d.normal={s.normal[0],s.normal[1],s.normal[2]};
        std::copy_n(s.joint_indices,4,d.joints.begin());std::copy_n(s.joint_weights,4,d.weights.begin());
    }
    pictor::Skeleton hierarchy(skeleton);
    std::vector<pictor::Transform> local(skeleton.bones.size());
    std::vector<pictor::float4x4> world(skeleton.bones.size());
    hierarchy.get_bind_pose(local.data());hierarchy.compute_world_matrices(local.data(),world.data());
    std::vector<pictor::demo::MotionBone> bones(skeleton.bones.size());
    for (size_t i=0;i<bones.size();++i)
        bones[i]={skeleton.bones[i].name,skeleton.bones[i].parent_index,{world[i].m[3][0],world[i].m[3][1],world[i].m[3][2]}};
    // Patch i owns source triangle i; the corner reads below depend on it.
    if (mesh.indices.size()!=size_t{source_count_}*3)
        throw std::runtime_error("PN patch count does not match source triangle count");
    std::vector<pictor::demo::PolynomialPatch> patches(source_count_);
    for (size_t i=0;i<patches.size();++i) {
        patches[i].control=model.patches()[i].control;
        for (unsigned c=0;c<3;++c) {
            const auto& v=mesh.vertices[mesh.indices[i*3+c]];
            patches[i].normals[c]={v.normal[0],v.normal[1],v.normal[2]};patches[i].uv[c]={v.uv[0],v.uv[1]};
        }
    }
    std::vector<pictor::demo::MotionSubmesh> submeshes(mesh.submeshes.size());
    submesh_kinds_.resize(mesh.submeshes.size());
    for (size_t i=0;i<submeshes.size();++i) {
        const auto& sub=mesh.submeshes[i];
        submeshes[i]={sub.index_start/3,sub.index_count/3,sub.texture_basename};
        submesh_kinds_[i]=material_kind(sub.texture_basename);
    }
    // Source patches rewritten by the provider keep their material class; the
    // GPU upload path must not reclassify them as untextured motion patches.
    source_kinds_.assign(source_count_,0.f);
    for (size_t i=0;i<submeshes.size();++i)
        for (size_t t=submeshes[i].first_patch;t<size_t{submeshes[i].first_patch}+submeshes[i].patch_count;++t)
            source_kinds_.at(t)=submesh_kinds_[i];
    motion_->describe_submeshes(submeshes);
    motion_->initialize(vertices,mesh.indices,bones,patches);
    ranges_=motion_draw_ranges(submeshes,motion_->patches(),source_count_,motion_->replaces_source());
    total_count_=static_cast<uint32_t>(motion_->patches().size());
    for (const auto index:motion_->changed_patches())
        if (index>=total_count_) throw std::runtime_error("Invalid motion patch index");
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
float MotionBridge::kind_of(uint32_t index) const {
    if (index<source_count_) return source_kinds_[index];
    const auto submesh=motion_->patches()[index].submesh;
    return submesh==pictor::demo::kUntexturedSubmesh?0.f:submesh_kinds_.at(size_t(submesh));
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
std::vector<GpuPnPatch> MotionBridge::initial_patches(const PnModel& model) const {
    auto result=make_gpu_patches(model);result.resize(total_count_);
    if (motion_) for (const auto index:motion_->changed_patches())
        result[index]=make_gpu_patch(motion_->patches()[index],kind_of(index));
    return result;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::update(double seconds,RaymarchPass& pass) {
    if (!motion_) return;
    motion_->update(seconds);
    if (motion_->patches().size()!=total_count_) throw std::runtime_error("Motion patch storage changed after initialization");
    for (const auto index:motion_->changed_patches()) {
        if (index>=total_count_) throw std::runtime_error("Invalid motion patch index");
        pass.update_patch(index,make_gpu_patch(motion_->patches()[index],kind_of(index)));
    }
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
bool MotionBridge::key(int code) {
    if (!motion_) return false;
    using Command=pictor::demo::MotionCommand;
    if (code==GLFW_KEY_0) motion_->command(Command::zero_force);
    else if (code==GLFW_KEY_1) motion_->command(Command::positive_force);
    else if (code==GLFW_KEY_2) motion_->command(Command::negative_force);
    else if (code==GLFW_KEY_P) motion_->command(Command::pause);
    else if (code==GLFW_KEY_R) motion_->command(Command::reset);
    else return false;
    return true;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::draw_status(pictor::BitmapTextRenderer& text) const {
    if (!motion_) return;
    char status[1024]{};motion_->status(status,sizeof(status));status[sizeof(status)-1]=0;
    // The status text comes from an external implementation, so its line count
    // is not ours to trust. Stop before the block draw_debug_hud owns at the
    // bottom of the screen rather than drawing off-screen over it.
    constexpr unsigned kMaxLines=12;
    char* line=status;float y=326;
    for (unsigned drawn=0;*line && drawn<kMaxLines;++drawn) {
        char* end=std::strchr(line,'\n');if (end) *end=0;
        text.draw_text(17,y+1,line,0,0,0);text.draw_text(16,y,line,.9f,1.f,.65f);
        if (!end) break;line=end+1;y+=18;
    }
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::record_capture() const { if (motion_) motion_->record_capture(); }
}
