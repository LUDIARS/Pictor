#include "grass_system.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"
#include <algorithm>
#include <cstring>

namespace ubg {

namespace {
// CPU 用の簡単なハッシュ / ノイズ (配置データ生成用)
float hash2(float x, float y) {
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}
float vnoise(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return lerpf(lerpf(a, b, ux), lerpf(c, d, ux), uy);
}
bool inside_box_xz(Vec3 p, Vec3 c, Vec3 h) {
    return std::fabs(p.x - c.x) < h.x && std::fabs(p.z - c.z) < h.z;
}
// view_proj から視錐台の 6 平面 (grass_cull.comp と同じ取り出し方)
void extract_planes(const Mat4& vp, float planes[6][4]) {
    auto row = [&](int r, int c) { return vp.m[c * 4 + r]; };
    for (int i = 0; i < 6; ++i) {
        float p[4];
        for (int c = 0; c < 4; ++c) {
            float r3 = row(3, c);
            switch (i) {
                case 0: p[c] = r3 + row(0, c); break;
                case 1: p[c] = r3 - row(0, c); break;
                case 2: p[c] = r3 + row(1, c); break;
                case 3: p[c] = r3 - row(1, c); break;
                case 4: p[c] = row(2, c); break;
                default: p[c] = r3 - row(2, c); break;
            }
        }
        float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (int c = 0; c < 4; ++c) planes[i][c] = p[c] / l;
    }
}
bool aabb_visible(const float planes[6][4], Vec3 mn, Vec3 mx) {
    for (int i = 0; i < 6; ++i) {
        // 平面の法線方向に一番出ている角 (p-vertex) が裏なら完全に外
        Vec3 pv{planes[i][0] >= 0 ? mx.x : mn.x, planes[i][1] >= 0 ? mx.y : mn.y, planes[i][2] >= 0 ? mx.z : mn.z};
        if (planes[i][0] * pv.x + planes[i][1] * pv.y + planes[i][2] * pv.z + planes[i][3] < 0) return false;
    }
    return true;
}
} // namespace

void GrassSystem::generate() {
    original_.clear();
    const float spacing = 0.55f;
    for (float z = -kAreaHalf; z < kAreaHalf; z += spacing) {
        for (float x = -kAreaHalf; x < kAreaHalf; x += spacing) {
            float jx = x + (hash2(x, z) - 0.5f) * spacing;
            float jz = z + (hash2(z, x) - 0.5f) * spacing;
            // 「ペイント」: 道は生やさない、所々に地面が見える
            float path = std::fabs(jz - 8.0f * std::sin(jx * 0.05f));
            if (path < 2.2f) continue;
            float paint = vnoise(jx * 0.06f, jz * 0.06f) * 0.7f + vnoise(jx * 0.2f, jz * 0.2f) * 0.3f;
            if (paint < 0.28f) continue;
            if (hash2(jx * 3.1f, jz * 1.7f) > smoothstepf(0.28f, 0.45f, paint)) continue;

            bool tall = vnoise(jx * 0.04f + 9.0f, jz * 0.04f) > 0.62f;
            GrassInstanceGPU g{};
            g.pos_scale[0] = jx; g.pos_scale[1] = 0.0f; g.pos_scale[2] = jz;
            g.pos_scale[3] = 0.8f + 0.4f * hash2(jx, jz * 2.0f);
            g.params[0] = hash2(jz * 5.0f, jx) * 2.0f * kPi;
            g.params[1] = tall ? 1.0f : 0.0f;
            g.params[2] = hash2(jx * 0.3f, jz * 0.3f);
            g.params[3] = tall ? 0.9f + 0.5f * hash2(jx, jz) : 0.45f + 0.35f * hash2(jz, jx);
            original_.push_back(g);
        }
    }
    current_ = original_;
    rebuild_cells();
}

Vec3 GrassSystem::building_half() const {
    // 建築中は足場が出ているので、完成後より Collider が大きい
    if (building_ == BuildingState::Constructing) return {7.0f, 4.0f, 6.0f};
    return {4.5f, 3.0f, 4.0f};
}

void GrassSystem::set_building(BuildingState s, bool from_original) {
    building_ = s;
    const std::vector<GrassInstanceGPU>& src = from_original ? original_ : current_;
    std::vector<GrassInstanceGPU> out;
    out.reserve(src.size());
    Vec3 c = building_center();
    Vec3 h = building_half() + Vec3{0.3f, 0.0f, 0.3f};
    for (const auto& g : src) {
        Vec3 p{g.pos_scale[0], 0.0f, g.pos_scale[2]};
        if (building_ != BuildingState::Empty && inside_box_xz(p, c, h)) continue;   // Collider で除外
        out.push_back(g);
    }
    current_.swap(out);
    rebuild_cells();
}

void GrassSystem::rebuild_cells() {
    // セル番号 = z 行 x 16 + x 列。同じ行の隣り合うセルは配列上でも隣り合う
    const int N = kCellsPerSide;
    const float cell_size = 2.0f * kAreaHalf / N;
    auto cell_of = [&](const GrassInstanceGPU& g) {
        int cx = std::clamp(int((g.pos_scale[0] + kAreaHalf) / cell_size), 0, N - 1);
        int cz = std::clamp(int((g.pos_scale[2] + kAreaHalf) / cell_size), 0, N - 1);
        return cz * N + cx;
    };
    std::stable_sort(current_.begin(), current_.end(),
                     [&](const GrassInstanceGPU& a, const GrassInstanceGPU& b) { return cell_of(a) < cell_of(b); });
    cells_.assign(N * N, GrassCell{});
    for (int cz = 0; cz < N; ++cz)
        for (int cx = 0; cx < N; ++cx) {
            GrassCell& c = cells_[cz * N + cx];
            c.bmin = {-kAreaHalf + cx * cell_size - 0.5f, 0.0f, -kAreaHalf + cz * cell_size - 0.5f};
            c.bmax = {c.bmin.x + cell_size + 1.0f, 1.6f, c.bmin.z + cell_size + 1.0f};
        }
    for (uint32_t i = 0; i < current_.size(); ++i) {
        GrassCell& c = cells_[cell_of(current_[i])];
        if (c.count == 0) c.start = i;
        ++c.count;
    }
    stats_.total_instances = static_cast<uint32_t>(current_.size());
    stats_.total_cells = static_cast<uint32_t>(cells_.size());
}

bool GrassSystem::create_gpu(pictor::VulkanContext& ctx, VkDescriptorSetLayout set1_layout,
                             VkDescriptorPool pool, uint32_t flights) {
    ctx_ = &ctx;
    device_ = ctx.device();
    capacity_ = static_cast<uint32_t>(std::max<size_t>(original_.size(), 1));
    if (!instances_.create(ctx, sizeof(GrassInstanceGPU) * capacity_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
        return false;
    visible_.resize(flights);
    indirect_.resize(flights);
    env_.resize(flights);
    sets_.resize(flights);
    for (uint32_t f = 0; f < flights; ++f) {
        if (!visible_[f].create(ctx, sizeof(uint32_t) * capacity_ * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;
        if (!indirect_[f].create(ctx, sizeof(uint32_t) * 8,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) return false;
        if (!env_[f].create(ctx, sizeof(float) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set1_layout;
        if (vkAllocateDescriptorSets(device_, &ai, &sets_[f]) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo bi[4] = {
            {instances_.buffer, 0, VK_WHOLE_SIZE}, {visible_[f].buffer, 0, VK_WHOLE_SIZE},
            {indirect_[f].buffer, 0, VK_WHOLE_SIZE}, {env_[f].buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = sets_[f];
            w[i].dstBinding = static_cast<uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
    }
    upload();
    return true;
}

void GrassSystem::upload() {
    if (!instances_.mapped) return;
    std::memcpy(instances_.mapped, current_.data(), current_.size() * sizeof(GrassInstanceGPU));
}

void GrassSystem::destroy(VkDevice dev) {
    instances_.destroy(dev);
    for (auto& b : visible_) b.destroy(dev);
    for (auto& b : indirect_) b.destroy(dev);
    for (auto& b : env_) b.destroy(dev);
}

void GrassSystem::record_compute(VkCommandBuffer cmd, uint32_t flight, const Mat4& view_proj, Vec3 cam,
                                 float fade_end, float lod_dist, bool gpu_cull,
                                 VkPipeline env_pipe, VkPipeline cull_pipe, VkPipelineLayout comp_layout,
                                 VkDescriptorSet set0) {
    // 1) 間接描画コマンドをリセット: {頂点数, インスタンス数 = 0, 0, 0} x 2
    uint32_t reset[8] = {kLod0Verts, 0, 0, 0, kLod1Verts, 0, 0, 0};
    vkCmdUpdateBuffer(cmd, indirect_[flight].buffer, 0, sizeof(reset), reset);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    VkDescriptorSet sets[2] = {set0, sets_[flight]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, comp_layout, 0, 2, sets, 0, nullptr);

    // 2) 環境光 (太陽光・空の色) を 1 回だけ計算
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, env_pipe);
    vkCmdDispatch(cmd, 1, 1, 1);

    // 3) CPU でセル単位の視錐台判定 → 連続する見えるセルをまとめて 1 Dispatch
    float planes[6][4];
    extract_planes(view_proj, planes);
    stats_.visible_cells = 0;
    stats_.dispatches = 0;
    stats_.candidate_instances = 0;

    struct PC { uint32_t start, count, capacity, cull_on; float fade_end, lod_dist; } pc{};
    pc.capacity = capacity_;
    pc.cull_on = gpu_cull ? 1u : 0u;
    pc.fade_end = fade_end;
    pc.lod_dist = lod_dist;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipe);
    uint32_t run_start = 0, run_count = 0;
    bool in_run = false;
    auto flush = [&]() {
        if (in_run && run_count > 0) {
            pc.start = run_start;
            pc.count = run_count;
            vkCmdPushConstants(cmd, comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (run_count + 63) / 64, 1, 1);
            ++stats_.dispatches;
            stats_.candidate_instances += run_count;
        }
        in_run = false;
        run_count = 0;
    };
    for (const GrassCell& c : cells_) {
        bool vis = true;
        if (gpu_cull) {
            vis = aabb_visible(planes, c.bmin, c.bmax);
            // 遠すぎるセルも捨てる
            Vec3 center = (c.bmin + c.bmax) * 0.5f;
            if (length(center - cam) > fade_end + 15.0f) vis = false;
        }
        if (!vis) { flush(); continue; }
        ++stats_.visible_cells;
        if (c.count == 0) continue;            // 空のセルはつながりを切らない
        if (!in_run) { in_run = true; run_start = c.start; run_count = 0; }
        run_count = c.start + c.count - run_start;
    }
    flush();

    // 4) カリング結果 → 間接描画 / 頂点シェーダー
    VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb2.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &mb2, 0, nullptr, 0, nullptr);
}

void GrassSystem::record_draw(VkCommandBuffer cmd, uint32_t flight, VkPipeline pipe, VkPipelineLayout layout,
                              VkDescriptorSet set0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[2] = {set0, sets_[flight]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
    for (uint32_t lod = 0; lod < 2; ++lod) {
        uint32_t pc[2] = {lod, capacity_};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        vkCmdDrawIndirect(cmd, indirect_[flight].buffer, lod * 16, 1, 16);
    }
}

} // namespace ubg

#endif
