#include "pn_model.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace pictor_kuzuha {
using namespace pictor_fbx_viewer;
namespace {
Vec pos(const TexturedSkinnedVertex& v) { return {v.position[0],v.position[1],v.position[2]}; }
Vec normal(const TexturedSkinnedVertex& v) { return unit({v.normal[0],v.normal[1],v.normal[2]}); }
struct Edge { TopologyVertex a,b; auto operator<=>(const Edge&) const = default; };
struct EdgeData { Vec a{},b{}; unsigned count=0; bool sharp=false; };
bool agrees(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z > .9999f; }
Edge edge_key(const PackedMesh& mesh, uint32_t a, uint32_t b) {
    auto x=mesh.topology[a], y=mesh.topology[b];
    return x<y ? Edge{x,y} : Edge{y,x};
}
TexturedSkinnedVertex interpolate(const std::array<TexturedSkinnedVertex,3>& corners,
                                  const std::array<float,3>& weights) {
    TexturedSkinnedVertex out{};
    // One slot per corner influence: three corners contribute four joints each,
    // so the merge can never need more than this many distinct bones.
    static_assert(std::extent_v<decltype(TexturedSkinnedVertex::joint_indices)> == 4,
                  "Influence capacity assumes four joints per vertex");
    constexpr size_t kMaxInfluences=3*std::extent_v<decltype(TexturedSkinnedVertex::joint_indices)>;
    std::array<std::pair<uint32_t,float>,kMaxInfluences> influences{};
    size_t count=0;
    for (int i=0;i<3;++i) {
        for (int k=0;k<3;++k) {
            out.position[k]+=weights[i]*corners[i].position[k];
            out.normal[k]+=weights[i]*corners[i].normal[k];
        }
        for (int k=0;k<2;++k) out.uv[k]+=weights[i]*corners[i].uv[k];
        for (int k=0;k<4;++k) {
            const auto bone=corners[i].joint_indices[k];
            const float weight=weights[i]*corners[i].joint_weights[k];
            if (weight<=0) continue;
            size_t slot=0;
            while (slot<count && influences[slot].first!=bone) ++slot;
            if (slot==count) {
                if (count==kMaxInfluences) continue; // Unreachable; guards the write.
                influences[count++]={bone,0.f};
            }
            influences[slot].second+=weight;
        }
    }
    std::sort(influences.begin(),influences.begin()+count,[](auto a,auto b) {
        return a.second!=b.second ? a.second>b.second : a.first<b.first;
    });
    float sum=0;
    for (size_t i=0;i<std::min(count,size_t{4});++i) sum+=influences[i].second;
    if (!(sum>0)) throw std::runtime_error("PN input has no skin influence");
    for (size_t i=0;i<std::min(count,size_t{4});++i) {
        out.joint_indices[i]=influences[i].first;
        out.joint_weights[i]=influences[i].second/sum;
    }
    const Vec n=normal(out); out.normal[0]=n.x;out.normal[1]=n.y;out.normal[2]=n.z;
    return out;
}
}
PnModel::PnModel(PackedMesh source):source_(std::move(source)) {
    if (source_.indices.empty() || source_.indices.size()%3 || source_.topology.size()!=source_.vertices.size())
        throw std::runtime_error("PN requires triangle topology from fresh FBX import");
    std::map<Edge,EdgeData> edges; // Init only; render path uses flat precomputed patches.
    for (const auto& vertex:source_.vertices)
        for (int k=0;k<3;++k)
            if (!std::isfinite(vertex.position[k]) || !std::isfinite(vertex.normal[k]))
                throw std::runtime_error("PN input contains non-finite geometry");
    for (size_t t=0;t<source_.indices.size();t+=3) for (int e=0;e<3;++e) {
        auto a=source_.indices[t+e],b=source_.indices[t+(e+1)%3];
        if (a>=source_.vertices.size() || b>=source_.vertices.size()) throw std::runtime_error("Invalid PN index");
        if (source_.topology[b]<source_.topology[a]) std::swap(a,b);
        auto& edge=edges[edge_key(source_,a,b)];
        const Vec na=normal(source_.vertices[a]),nb=normal(source_.vertices[b]);
        if (edge.count && (!agrees(edge.a,na)||!agrees(edge.b,nb))) edge.sharp=true;
        edge.a=na; edge.b=nb; ++edge.count;
    }
    for (const auto& [key,edge]:edges) if (edge.count!=2 || edge.sharp) ++protected_edges_;
    patches_.reserve(source_.indices.size()/3);
    for (size_t t=0;t<source_.indices.size();t+=3) {
        std::array<Vec,3> p;
        std::array<Vec,6> n;
        std::array<bool,3> straight;
        for (int k=0;k<3;++k) {
            const auto a=source_.indices[t+k],b=source_.indices[t+(k+1)%3];
            p[k]=pos(source_.vertices[a]);
            const auto& edge=edges.at(edge_key(source_,a,b));
            const bool ordered=source_.topology[a]<source_.topology[b];
            // Both sides consume the very same endpoint normals, even when
            // importer roundoff is below the discontinuity tolerance.
            n[k*2]=ordered ? edge.a:edge.b;
            n[k*2+1]=ordered ? edge.b:edge.a;
            straight[k]=edge.count!=2 || edge.sharp;
        }
        patches_.push_back(make_patch(p,n,straight));
    }
}
PackedMesh PnModel::sample(unsigned factor,float strength,RefinementStats& stats) const {
    if (factor<1 || factor>8 || !std::isfinite(strength) || strength<0 || strength>1)
        throw std::runtime_error("PN factor must be 1..8 and strength 0..1");
    stats={};stats.source_triangles=patches_.size();stats.protected_edges=protected_edges_;
    if (factor==1) { stats.output_triangles=patches_.size();return source_; }
    const size_t per_patch=(factor+1)*(factor+2)/2;
    if (patches_.size()*per_patch>8'000'000) throw std::runtime_error("PN vertex budget exceeded; reduce level");
    PackedMesh out;out.center=source_.center;out.radius=source_.radius;out.submeshes=source_.submeshes;
    out.vertices.reserve(patches_.size()*per_patch);out.indices.reserve(source_.indices.size()*factor*factor);
    for (auto& sm:out.submeshes) { sm.index_start*=factor*factor;sm.index_count*=factor*factor; }
    double squared=0;
    for (size_t t=0;t<patches_.size();++t) {
        const uint32_t base=static_cast<uint32_t>(out.vertices.size());
        const std::array<TexturedSkinnedVertex,3> corners{source_.vertices[source_.indices[t*3]],
            source_.vertices[source_.indices[t*3+1]],source_.vertices[source_.indices[t*3+2]]};
        auto index=[&](unsigned i,unsigned j) { return base+i*(factor+1)-i*(i-1)/2+j; };
        for (unsigned i=0;i<=factor;++i) for (unsigned j=0;j<=factor-i;++j) {
            const float v=float(i)/factor,w=float(j)/factor,u=1-v-w;
            auto vertex=interpolate(corners,{u,v,w});
            const Vec p=evaluate(patches_[t],u,v,w);
            float delta[3]{strength*(p.x-vertex.position[0]),strength*(p.y-vertex.position[1]),strength*(p.z-vertex.position[2])};
            const double d2=double(delta[0])*delta[0]+double(delta[1])*delta[1]+double(delta[2])*delta[2];
            squared+=d2;stats.max_displacement=std::max(stats.max_displacement,float(std::sqrt(d2)));
            for (int k=0;k<3;++k) vertex.position[k]+=delta[k];
            out.vertices.push_back(vertex);
        }
        for (unsigned i=0;i<factor;++i) for (unsigned j=0;j<factor-i;++j) {
            out.indices.insert(out.indices.end(),{index(i,j),index(i+1,j),index(i,j+1)});
            if (i+j+1<factor) out.indices.insert(out.indices.end(),{index(i+1,j),index(i+1,j+1),index(i,j+1)});
        }
    }
    stats.output_triangles=out.indices.size()/3;
    if (!out.vertices.empty()) stats.rms_displacement=std::sqrt(squared/out.vertices.size());
    return out;
}
}
