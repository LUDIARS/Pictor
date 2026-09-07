#include "motion_bridge.h"
#include "pictor/animation/skeleton.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pictor_kuzuha {
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
    motion_->initialize(vertices,mesh.indices,bones,patches);
    if (motion_->patches().size()<source_count_ || motion_->patches().size()>source_count_+65536)
        throw std::runtime_error("Invalid polynomial motion patch count");
    total_count_=static_cast<uint32_t>(motion_->patches().size());
    for (const auto index:motion_->changed_patches())
        if (index>=total_count_) throw std::runtime_error("Invalid motion patch index");
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
std::vector<GpuPnPatch> MotionBridge::initial_patches(const PnModel& model) const {
    auto result=make_gpu_patches(model);result.resize(total_count_);
    if (motion_) for (const auto index:motion_->changed_patches())
        result[index]=make_gpu_patch(motion_->patches()[index]);
    return result;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::update(double seconds,RaymarchPass& pass) {
    if (!motion_) return;
    motion_->update(seconds);
    if (motion_->patches().size()!=total_count_) throw std::runtime_error("Motion patch storage changed after initialization");
    for (const auto index:motion_->changed_patches()) {
        if (index>=total_count_) throw std::runtime_error("Invalid motion patch index");
        pass.update_patch(index,make_gpu_patch(motion_->patches()[index]));
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
    char* line=status;float y=326;
    while (*line) {
        char* end=std::strchr(line,'\n');if (end) *end=0;
        text.draw_text(17,y+1,line,0,0,0);text.draw_text(16,y,line,.9f,1.f,.65f);
        if (!end) break;line=end+1;y+=18;
    }
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void MotionBridge::record_capture() const { if (motion_) motion_->record_capture(); }
}
