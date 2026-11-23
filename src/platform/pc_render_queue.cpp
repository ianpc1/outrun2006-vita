#include "platform/pc_render_queue.hpp"
#include "driving/pc_x87.hpp"
#include "system/perf.hpp"
#include "platform/pc_pmt_records.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include "vita_profiler.hpp"
#include "vita_job_system.hpp"
#endif
namespace outrun::platform {
std::int32_t g_lod_culling_distance = 6;
std::int32_t g_lod_bias_offset = 1;
namespace {
using driving::Bytes;
using driving::CourseProbe;
using driving::PcMatrixStack;
using namespace pmt_records;
// x87 register value: every arithmetic step rounds to the precision control
// (24-bit after the Direct3D device is created without FPU_PRESERVE).
using X=driving::X87;
constexpr u32 Invalid=0xffffffffu;
// The 0x48-byte draw descriptor 405360 builds on its stack: the queue entry
// words, then the walk's own state.
struct Descriptor : DrawEntry {
    u32 alpha_queue;     // +3C 0 opaque queue, 1 alpha queue
    u32 matrix_dirty;    // +40 the current matrix is not in the pool yet (404A80)
    u32 unknown44;
};
static_assert(sizeof(Descriptor)==0x48);
// A new descriptor of object `id` (405360 / 405450 / 405580 headers).
Descriptor descriptor(u32 id,u32 resource,u32 object,std::int32_t opaque,u32 colour,std::int32_t colour_byte){
    Descriptor d{};
    d.palette_count=1;d.id_high=id&0xffff0000u;d.resource=resource;d.object=object;
    d.colour_list=colour_byte>0?colour:0u;d.colour=u32(colour_byte);
    d.id=id;d.alpha_queue=opaque!=0?0u:1u;d.matrix_dirty=1;
    return d;
}
// Object record offset of index in a bank (object table at system +0x18).
u32 object_offset(u32 index){return 0x18u+index*0x3cu;}
// 409FD0 inline form: depth++, and when it fits new = m * current.
void push_multiply(PcMatrixStack& s,Bytes m){
    const bool fits=s.depth+1<s.capacity;driving::pc_matrix_push(s);
    if(fits){std::array<std::uint8_t,64> copy;m.check(0,64);std::memcpy(copy.data(),m.data(),64);
        driving::pc_matrix_multiply_current(s,Bytes(copy.data(),64));}
}
// 40A580: rotation R about the unit axis by (sin, cos), current = R * current.
void rotate_axis_40a580(PcMatrixStack& s,const CourseProbe& a,float sn,float cs){
    const X k=X(1.f)-cs;                          // fld 1; fsub cos (stays in ST)
    const float x=a.x,y=a.y,z=a.z;
    const float xy=float((X(y)*x)*k);
    const float yz=float((X(z)*y)*k);
    const float xz=float((X(z)*x)*k);
    const float xs=float(X(sn)*x);
    const float ys=float(X(y)*sn);
    const float zs=float(X(z)*sn);
    const X xx=X(x)*x,yy=X(y)*y;                  // kept in x87 registers
    const float zz=float(X(z)*z);                 // stored
    std::array<float,16> r{};
    r[0]=float((X(1.f)-xx)*cs+xx);
    r[1]=float(X(zs)+xy);r[2]=float(X(xz)-ys);
    r[4]=float(X(xy)-zs);r[5]=float((X(1.f)-yy)*cs+yy);r[6]=float(X(xs)+yz);
    r[8]=float(X(ys)+xz);r[9]=float(X(yz)-xs);r[10]=float((X(1.f)-zz)*cs+zz);
    r[15]=1.f;
    driving::pc_matrix_multiply_current(s,Bytes(r.data(),64));
}
// Node flag 0x800: the node matrix pushed, then turned about y so its z
// axis faces the camera (eye-to-target direction projected on xz).
void face_camera(PcRenderContext& c,Bytes m){
    auto& s=c.matrices;
    const auto a=driving::pc_transform_point(s.current(),{0.f,0.f,1.f});
    const auto o=driving::pc_transform_point(s.current(),{0.f,0.f,0.f});
    CourseProbe dir{float(X(a.x)-o.x),0.f,float(X(a.z)-o.z)};
    CourseProbe to{float(X(c.view.eye[0])-c.view.target[0]),0.f,float(X(c.view.eye[2])-c.view.target[2])};
    const X len=driving::x87_sqrt(X(to.z)*to.z+X(to.x)*to.x);
    if(len>X(0.0001)){ // 6282A8 (double 1e-4)
        const X inv=X(1.f)/len;
        to.x=float(X(to.x)*inv);to.y=float(X(0.f)*inv);to.z=float(X(to.z)*inv);
    }
    push_multiply(s,m);
    CourseProbe axis{float(X(to.z)*dir.y-X(dir.z)*to.y),
                     float(X(dir.z)*to.x-X(to.z)*dir.x),
                     float(X(dir.x)*to.y-X(to.x)*dir.y)};
    const float dot=float((X(to.z)*dir.z+X(to.x)*dir.x)+X(to.y)*dir.y);
    const X sq=(X(axis.z)*axis.z+X(axis.y)*axis.y)+X(axis.x)*axis.x;
    if(!(driving::x87_abs(sq)<X(1.1920928955078125e-7f)||std::isnan(sq.v))){
        const X root=driving::x87_sqrt(sq);const float sn=float(root);
        if(root>X(0.0001)){
            const X inv=X(1.f)/sn;
            axis.x=float(X(axis.x)*inv);axis.y=float(X(axis.y)*inv);axis.z=float(X(axis.z)*inv);
        }
        rotate_axis_40a580(s,axis,sn,dot);
    }else if(dot<0.f){
        rotate_axis_40a580(s,{0.f,1.f,0.f},0.f,-1.f);
    }
}
// 404A80: matrix pool slot of the next entry: the current matrix is
// appended when it changed since the last entry (a palette's first entry
// reuses the slots 405580 appended).
u32 matrix_slot_404a80(PcRenderQueue& q,Descriptor& d,PcMatrixStack& s){
    if(d.matrix_dirty==0)return q.matrix_count-d.palette_count;
    d.matrix_dirty=0;
    if(std::int32_t(q.matrix_count)>=std::int32_t(q.matrix_capacity))q.matrix_count=q.matrix_capacity-1u;
    u32 slot;
    if(d.palette_count==1){slot=q.matrix_count;++q.matrix_count;}
    else{slot=q.matrix_count-d.palette_count;d.palette_count=1;}
    if(slot>=q.matrices.size())throw std::out_of_range("404A80 matrix pool slot");
    const auto m=s.current();m.check(0,64);std::memcpy(q.matrices[slot].data(),m.data(),64);
    return slot;
}
enum ResourceClass : uint8_t {
    RES_CLASS_UNKNOWN = 0,
    RES_CLASS_SKY = 1,
    RES_CLASS_BK = 2,
    RES_CLASS_COURSE = 3,
    RES_CLASS_OTHER = 4
};

static inline ResourceClass classify_resource_fast(std::uint32_t resource_id) {
    if (resource_id == 0 || resource_id == 0xFFFFFFFFu) return RES_CLASS_OTHER;
    if (resource_id == 0x117u) return RES_CLASS_SKY;
    if (resource_id == 0xDBu || resource_id == 0xFEu || resource_id == 0x126u ||
        resource_id == 0x162u || resource_id == 0x180u || resource_id == 0x19Eu) {
        return RES_CLASS_COURSE;
    }
    if ((resource_id >= 35u && resource_id <= 85u) || (resource_id >= 226u && resource_id <= 243u)) {
        return RES_CLASS_BK;
    }

    static ResourceClass s_cache[1024] = {RES_CLASS_UNKNOWN};
    if (resource_id < 1024) {
        ResourceClass c = s_cache[resource_id];
        if (c != RES_CLASS_UNKNOWN) return c;

        char name[128] = "";
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        outrun::vita_profile::get_resource_name(resource_id, name, sizeof(name));
#endif
        if (std::strstr(name, "sky") != nullptr || std::strstr(name, "SKY") != nullptr) {
            c = RES_CLASS_SKY;
        } else if (std::strstr(name, "BK") != nullptr || std::strstr(name, "bk") != nullptr) {
            c = RES_CLASS_BK;
        } else if (std::strstr(name, "Stage") != nullptr || std::strstr(name, "cs_CS") != nullptr || std::strstr(name, "CS_") != nullptr) {
            c = RES_CLASS_COURSE;
        } else {
            c = RES_CLASS_OTHER;
        }
        s_cache[resource_id] = c;
        return c;
    }
    return RES_CLASS_OTHER;
}

static inline bool fast_distance_cull(ResourceClass rclass, float effective_radius, float dist_sq) {
    // Only distance-cull small/medium props (< 35m); road/grass/terrain chunks (100..1500m+)
    // can have centers > 650m away while their geometry is right beside the car.
    if (effective_radius >= 35.0f) return false;
    float limit = 0.0f;
    if (rclass == RES_CLASS_BK) {
        limit = (effective_radius < 5.0f) ? 110.0f : (effective_radius < 15.0f) ? 190.0f : 300.0f;
    } else if (rclass == RES_CLASS_COURSE) {
        limit = (effective_radius < 5.0f) ? 180.0f : (effective_radius < 15.0f) ? 320.0f : 480.0f;
    } else {
        return false;
    }
    const float d = limit + effective_radius;
    return dist_sq > (d * d);
}

// 404B00: view-space centre, frustum test (near, the four planes, far) and
// LOD from the projected size (exponent of the screen radius against the
// threshold; nodes with flag 0x10 pick mesh e - bias - 1 in 0..3).
bool node_visible(PcRenderContext& c,const PmtNode& node,CourseProbe& v,std::int32_t& lod_out,ResourceClass rclass = RES_CLASS_OTHER){
#if !defined(OR2_VITA) && !defined(__VITA__) && !defined(__psp2__)
    OR2_PERF_ZONE("404B00 node visible");
#endif
    auto& s=c.matrices;
    v=driving::pc_transform_point(s.current(),{node.centre[0],node.centre[1],node.centre[2]});
    const float* vm = reinterpret_cast<const float*>(c.view.view_95d860.data());
    const float vx=v.x*vm[0] + v.y*vm[4] + v.z*vm[8]  + vm[12];
    const float vy=v.x*vm[1] + v.y*vm[5] + v.z*vm[9]  + vm[13];
    const float vz=v.x*vm[2] + v.y*vm[6] + v.z*vm[10] + vm[14];
    v.x=vx; v.y=vy; v.z=vz;

    float effective_radius = std::abs(node.radius);
    if (effective_radius < 0.001f) {
        effective_radius = 2.0f;
    }

    const auto& f=c.view.frustum_95bf40;const auto& p=c.view.planes_95bf58;
    const float near_limit = -f[4];

    if (fast_distance_cull(rclass, effective_radius, vx*vx + vy*vy + vz*vz)) {
        lod_out = 0;
        return false;
    }

    const auto u=driving::pc_matrix_vector(s,{0.577350259f,0.577350259f,0.577350259f});
    const float r = std::sqrt(u.x*u.x + u.y*u.y + u.z*u.z) * effective_radius;

    std::int32_t lod=0;bool visible=false;
    if((v.z - r) > near_limit) goto done;
    if((p[1][2]*v.z + p[1][0]*v.x) > r) goto done;
    if((p[0][2]*v.z + p[0][0]*v.x) > r) goto done;
    if((p[2][1]*v.y + p[2][2]*v.z) > r) goto done;
    if((p[3][1]*v.y + p[3][2]*v.z) > r) goto done;
    if((r + v.z) < -f[5]) goto done;         // far: TEST 5 / JNP
    {
        std::int32_t e=-126;
        if(v.z != 0.f){
            e=127;
            const float w = (((-1.f / f[3]) / v.z) * f[4]) * r;
            std::uint32_t bits;std::memcpy(&bits,&w,4);
            e -= std::int32_t((bits>>23)&0xffu);
            const bool is_close_range = (v.z > -20.0f);
            const std::int32_t thresh = ((node.flags & 0x40u) || is_close_range || c.lod_threshold_8999b8 >= 7)
                ? c.lod_threshold_8999b8
                : std::min(c.lod_threshold_8999b8, g_lod_culling_distance);
            if(e>=thresh)goto done;
        }
        if(node.flags&0x10u){
            lod=e-node.lod_bias-1 + g_lod_bias_offset;
            if(lod>3)lod=3;else if(lod<0)lod=0;
            while(lod>0 && node.mesh[lod]<0) --lod;
        }
        visible=true;
    }
done:
    lod_out=lod;return visible;
}
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
static inline void mul4x4_fast(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r) {
        const float a0 = a[r*4 + 0], a1 = a[r*4 + 1], a2 = a[r*4 + 2], a3 = a[r*4 + 3];
        out[r*4 + 0] = a0*b[0] + a1*b[4] + a2*b[8]  + a3*b[12];
        out[r*4 + 1] = a0*b[1] + a1*b[5] + a2*b[9]  + a3*b[13];
        out[r*4 + 2] = a0*b[2] + a1*b[6] + a2*b[10] + a3*b[14];
        out[r*4 + 3] = a0*b[3] + a1*b[7] + a2*b[11] + a3*b[15];
    }
}

static inline void apply_face_camera_fast(const float* base, const float* node_m, const float* eye, const float* target, float* out) {
    // base * (0,0,1) - base * (0,0,0) = row 2 of base
    const float dir_x = base[8], dir_z = base[10];
    float to_x = eye[0] - target[0];
    float to_z = eye[2] - target[2];
    const float len = std::sqrt(to_x*to_x + to_z*to_z);
    if (len > 0.0001f) {
        const float inv = 1.0f / len;
        to_x *= inv; to_z *= inv;
    }
    float tmp[16];
    mul4x4_fast(node_m, base, tmp);
    // axis = cross(dir, to) with y=0 -> (0, dir_z*to_x - to_z*dir_x, 0)
    float ay = dir_z * to_x - to_z * dir_x;
    const float dot = to_z * dir_z + to_x * dir_x;
    const float sq = ay * ay;
    float rot[16] = {
        1.f,0.f,0.f,0.f,
        0.f,1.f,0.f,0.f,
        0.f,0.f,1.f,0.f,
        0.f,0.f,0.f,1.f
    };
    if (sq >= 1.1920928955078125e-7f) {
        const float sn = std::sqrt(sq);
        if (sn > 0.0001f) ay /= sn;
        const float k = 1.0f - dot;
        const float yy = ay * ay;
        const float ys = ay * sn;
        rot[0] = dot;          rot[2] = -ys;
        rot[5] = (1.f - yy)*dot + yy;
        rot[8] = ys;           rot[10] = dot;
        mul4x4_fast(rot, tmp, out);
    } else if (dot < 0.0f) {
        rot[0] = -1.f; rot[10] = -1.f;
        mul4x4_fast(rot, tmp, out);
    } else {
        std::memcpy(out, tmp, 64);
    }
}

struct ParallelLeafResult {
    float depth;
    u32 mesh;
    u32 node_at;
    bool has_custom_mat;
    float mat[16];
};

struct ParallelLeafBatch {
    const std::uint8_t* sys_data;
    std::size_t sys_size;
    u32 obj_nodes;
    u32 obj_matrices;
    u32 obj_meshes;
    float base_mat[16];
    float view_mat[16];
    float frustum[6];
    float planes[4][4];
    float eye[3];
    float target[3];
    std::int32_t base_lod_thresh;
    ResourceClass rclass;
    const std::int32_t* indices;
    std::vector<ParallelLeafResult>* out[3];
};

static void parallel_leaf_worker(std::uint32_t begin, std::uint32_t end, std::uint32_t worker_idx, void* user_data) {
    const auto* batch = static_cast<const ParallelLeafBatch*>(user_data);
    auto& out = *batch->out[worker_idx];
    const float* vm = batch->view_mat;
    const float* f = batch->frustum;
    const float near_limit = -f[4];
    const float far_limit = -f[5];
    const float inv_f3_f4 = (-1.0f / f[3]) * f[4];

    for (std::uint32_t i = begin; i < end; ++i) {
        const std::int32_t idx = batch->indices[i];
        const u32 node_at = batch->obj_nodes + u32(idx) * 0x38u;
        if (std::size_t(node_at) + sizeof(PmtNode) > batch->sys_size) continue;
        PmtNode node{};
        std::memcpy(&node, batch->sys_data + node_at, sizeof(PmtNode));

        if (node.mesh[0] < 0 && node.mesh[1] < 0 && node.mesh[2] < 0 && node.mesh[3] < 0) continue;

        const float* bm = batch->base_mat;
        const float* node_m_ptr = nullptr;
        const float cx = node.centre[0], cy = node.centre[1], cz = node.centre[2];
        float wx, wy, wz;
        if (node.matrix >= 0) {
            const u32 mat_off = batch->obj_matrices + u32(node.matrix) * 0x40u;
            if (std::size_t(mat_off) + 64u > batch->sys_size) continue;
            node_m_ptr = reinterpret_cast<const float*>(batch->sys_data + mat_off);
            const float lx = cx*node_m_ptr[0] + cy*node_m_ptr[4] + cz*node_m_ptr[8]  + node_m_ptr[12];
            const float ly = cx*node_m_ptr[1] + cy*node_m_ptr[5] + cz*node_m_ptr[9]  + node_m_ptr[13];
            const float lz = cx*node_m_ptr[2] + cy*node_m_ptr[6] + cz*node_m_ptr[10] + node_m_ptr[14];
            wx = lx*bm[0] + ly*bm[4] + lz*bm[8]  + bm[12];
            wy = lx*bm[1] + ly*bm[5] + lz*bm[9]  + bm[13];
            wz = lx*bm[2] + ly*bm[6] + lz*bm[10] + bm[14];
        } else {
            wx = cx*bm[0] + cy*bm[4] + cz*bm[8]  + bm[12];
            wy = cx*bm[1] + cy*bm[5] + cz*bm[9]  + bm[13];
            wz = cx*bm[2] + cy*bm[6] + cz*bm[10] + bm[14];
        }

        const float vx = wx*vm[0] + wy*vm[4] + wz*vm[8]  + vm[12];
        const float vy = wx*vm[1] + wy*vm[5] + wz*vm[9]  + vm[13];
        const float vz = wx*vm[2] + wy*vm[6] + wz*vm[10] + vm[14];

        float effective_radius = std::abs(node.radius);
        if (effective_radius < 0.001f) effective_radius = 2.0f;

        if (fast_distance_cull(batch->rclass, effective_radius, vx*vx + vy*vy + vz*vz)) continue;

        // Only compute full 4x4 world matrix after surviving distance cull!
        float world_mat[16];
        const float* wm = bm;
        if (node_m_ptr != nullptr) {
            mul4x4_fast(node_m_ptr, bm, world_mat);
            wm = world_mat;
        }

        // Scale of wm applied to (0.57735, 0.57735, 0.57735)
        constexpr float kInvSqrt3 = 0.577350259f;
        const float ux = (wm[0] + wm[4] + wm[8])  * kInvSqrt3;
        const float uy = (wm[1] + wm[5] + wm[9])  * kInvSqrt3;
        const float uz = (wm[2] + wm[6] + wm[10]) * kInvSqrt3;
        const float r = std::sqrt(ux*ux + uy*uy + uz*uz) * effective_radius;

        if ((vz - r) > near_limit) continue;
        if ((batch->planes[1][2]*vz + batch->planes[1][0]*vx) > r) continue;
        if ((batch->planes[0][2]*vz + batch->planes[0][0]*vx) > r) continue;
        if ((batch->planes[2][1]*vy + batch->planes[2][2]*vz) > r) continue;
        if ((batch->planes[3][1]*vy + batch->planes[3][2]*vz) > r) continue;
        if ((r + vz) < far_limit) continue;

        const std::int32_t lod_thresh_cur = (node.flags & 0x40u) ? 7 : batch->base_lod_thresh;
        std::int32_t e = -126;
        if (vz != 0.0f) {
            e = 127;
            const float w = (inv_f3_f4 / vz) * r;
            std::uint32_t bits; std::memcpy(&bits, &w, 4);
            e -= std::int32_t((bits >> 23) & 0xffu);
            const bool is_close_range = (vz > -20.0f);
            const std::int32_t thresh = ((node.flags & 0x40u) || is_close_range || lod_thresh_cur >= 7)
                ? lod_thresh_cur
                : std::min(lod_thresh_cur, g_lod_culling_distance);
            if (e >= thresh) continue;
        }
        std::int32_t lod = 0;
        if (node.flags & 0x10u) {
            lod = e - node.lod_bias - 1 + g_lod_bias_offset;
            if (lod > 3) lod = 3;
            else if (lod < 0) lod = 0;
            while (lod > 0 && node.mesh[lod] < 0) --lod;
        }
        if (node.mesh[lod] < 0) continue;

        ParallelLeafResult res{};
        res.depth = vz;
        res.mesh = batch->obj_meshes + u32(node.mesh[lod]) * 8u;
        res.node_at = node_at;
        if (node_m_ptr != nullptr) {
            res.has_custom_mat = true;
            if (node.flags & 0x800u) {
                apply_face_camera_fast(batch->base_mat, node_m_ptr, batch->eye, batch->target, res.mat);
            } else {
                std::memcpy(res.mat, world_mat, 64);
            }
        } else {
            res.has_custom_mat = false;
        }
        out.push_back(res);
    }
}
#endif

// 404D10: the node tree from `first` (siblings in order, children first):
// each node's matrix pushed (flag 0x800: facing the camera), LOD threshold
// 7 under flag 0x40 (restored on the way back), and each visible node's
// mesh of its LOD queued with its depth and matrix.
void walk_404d10(PcRenderContext& c,const PcModelBank& bank,const PmtObject& obj,Descriptor& d,std::int32_t first,bool allow_parallel = true){
    const Bytes sys=bank.system;
    const ResourceClass rclass = classify_resource_fast(d.resource);
    u32 saved=u32(c.lod_threshold_8999b8); // VM bridge 1039C0C
    std::int32_t index=first;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    std::uint32_t skip_scan = 0u;
#endif
    for(;;){
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        // 3-Core parallel fast-path for flat leaf sibling chains (O(N) via skip_scan)
        if (allow_parallel && skip_scan == 0u && d.palette_count == 1 && outrun::vita_jobs::is_initialized()) {
            std::int32_t run_indices[512];
            std::uint32_t run_count = 0;
            std::int32_t scan = index;
            while (scan >= 0 && run_count < 512u) {
                const u32 n_at = obj.nodes + u32(scan) * 0x38u;
                const auto n = pmt<PmtNode>(sys, n_at);
                if (n.child >= 0 && n.child != first) break;
                run_indices[run_count++] = scan;
                scan = n.sibling;
            }
            if (run_count >= 8u) {
                static std::vector<ParallelLeafResult> s_worker_out[3];
                for (int w = 0; w < 3; ++w) {
                    s_worker_out[w].clear();
                    if (s_worker_out[w].capacity() < 128) s_worker_out[w].reserve(128);
                }
                ParallelLeafBatch batch{};
                batch.sys_data = sys.data();
                batch.sys_size = sys.size();
                batch.obj_nodes = obj.nodes;
                batch.obj_matrices = obj.matrices;
                batch.obj_meshes = obj.meshes;
                std::memcpy(batch.base_mat, c.matrices.current().data(), 64);
                std::memcpy(batch.view_mat, c.view.view_95d860.data(), 64);
                for (int k = 0; k < 6; ++k) batch.frustum[k] = c.view.frustum_95bf40[k];
                for (int k = 0; k < 4; ++k)
                    for (int j = 0; j < 4; ++j) batch.planes[k][j] = c.view.planes_95bf58[k][j];
                for (int k = 0; k < 3; ++k) { batch.eye[k] = c.view.eye[k]; batch.target[k] = c.view.target[k]; }
                batch.base_lod_thresh = c.lod_threshold_8999b8;
                batch.rclass = rclass;
                batch.indices = run_indices;
                batch.out[0] = &s_worker_out[0];
                batch.out[1] = &s_worker_out[1];
                batch.out[2] = &s_worker_out[2];

                outrun::vita_jobs::parallel_for(run_count, 8u, parallel_leaf_worker, &batch);

                auto& q = d.alpha_queue ? c.alpha : c.opaque;
                for (int w = 0; w < 3; ++w) {
                    for (const auto& leaf : s_worker_out[w]) {
                        if (q.count >= q.capacity) break;
                        const auto slot = q.count++;
                        std::memcpy(&d.depth, &leaf.depth, 4);
                        d.mesh = leaf.mesh;
                        d.node = leaf.node_at;
                        if (leaf.has_custom_mat) {
                            if (std::int32_t(q.matrix_count) >= std::int32_t(q.matrix_capacity))
                                q.matrix_count = q.matrix_capacity - 1u;
                            const u32 mslot = q.matrix_count++;
                            if (mslot < q.matrices.size()) {
                                std::memcpy(q.matrices[mslot].data(), leaf.mat, 64);
                            }
                            d.matrix = mslot;
                            d.matrix_dirty = 1;
                        } else {
                            d.matrix = matrix_slot_404a80(q, d, c.matrices);
                        }
                        std::memcpy(q.entries[slot].data(), static_cast<const DrawEntry*>(&d), sizeof(DrawEntry));
                    }
                }
                if (scan < 0) return;
                index = scan;
                continue;
            }
            // Avoid O(N^2) re-scanning on short leaf runs (<8)
            skip_scan = (run_count > 0u) ? run_count : 1u;
        }
        if (skip_scan > 0u) --skip_scan;
#endif
        const u32 node_at=obj.nodes+u32(index)*0x38u;
        const auto node=pmt<PmtNode>(sys,node_at);
        const bool has_child = (node.child >= 0 && node.child != first);
        const bool has_any_mesh = (node.mesh[0]>=0 || node.mesh[1]>=0 || node.mesh[2]>=0 || node.mesh[3]>=0);
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        // Fast leaf reject in serial walk: avoid matrix stack push/multiply/face_camera/pop for invisible leaves!
        if (!has_child) {
            if (!has_any_mesh) {
                const std::int32_t sibling = node.sibling;
                if (sibling < 0) return;
                index = sibling;
                continue;
            }
            float effective_radius = std::abs(node.radius);
            if (effective_radius < 0.001f) effective_radius = 2.0f;
            if (effective_radius < 35.0f && (rclass == RES_CLASS_BK || rclass == RES_CLASS_COURSE)) {
                const float* bm = reinterpret_cast<const float*>(c.matrices.current().data());
                const float* vm = reinterpret_cast<const float*>(c.view.view_95d860.data());
                const float cx = node.centre[0], cy = node.centre[1], cz = node.centre[2];
                float wx, wy, wz;
                if (node.matrix >= 0) {
                    const float* nm = reinterpret_cast<const float*>(sys.data() + obj.matrices + u32(node.matrix) * 0x40u);
                    const float lx = cx*nm[0] + cy*nm[4] + cz*nm[8]  + nm[12];
                    const float ly = cx*nm[1] + cy*nm[5] + cz*nm[9]  + nm[13];
                    const float lz = cx*nm[2] + cy*nm[6] + cz*nm[10] + nm[14];
                    wx = lx*bm[0] + ly*bm[4] + lz*bm[8]  + bm[12];
                    wy = lx*bm[1] + ly*bm[5] + lz*bm[9]  + bm[13];
                    wz = lx*bm[2] + ly*bm[6] + lz*bm[10] + bm[14];
                } else {
                    wx = cx*bm[0] + cy*bm[4] + cz*bm[8]  + bm[12];
                    wy = cx*bm[1] + cy*bm[5] + cz*bm[9]  + bm[13];
                    wz = cx*bm[2] + cy*bm[6] + cz*bm[10] + bm[14];
                }
                const float vx = wx*vm[0] + wy*vm[4] + wz*vm[8]  + vm[12];
                const float vy = wx*vm[1] + wy*vm[5] + wz*vm[9]  + vm[13];
                const float vz = wx*vm[2] + wy*vm[6] + wz*vm[10] + vm[14];
                if (fast_distance_cull(rclass, effective_radius, vx*vx + vy*vy + vz*vz)) {
                    const std::int32_t sibling = node.sibling;
                    if (sibling < 0) return;
                    index = sibling;
                    continue;
                }
            }
        }
#endif
        if(c.flag_depth_8999a0<0||c.flag_depth_8999a0>=64)throw std::out_of_range("899560 node stack");
        c.flag_stack_899560[std::size_t(c.flag_depth_8999a0)]=saved;
        ++c.flag_depth_8999a0;
        bool pushed=false;
        if(node.flags&0x40u)c.lod_threshold_8999b8=7;
        if(node.matrix>=0){
            const Bytes m=sys.sub(obj.matrices+u32(node.matrix)*0x40u,0x40);
            if(node.flags&0x800u)face_camera(c,m);
            else push_multiply(c.matrices,m);
            d.matrix_dirty=1;pushed=true;
        }
        CourseProbe centre{};std::int32_t lod=0;
        if(has_any_mesh && node_visible(c,node,centre,lod,rclass) && node.mesh[lod]>=0){
            std::memcpy(&d.depth,&centre.z,4);
            d.mesh=obj.meshes+u32(node.mesh[lod])*8u;
            d.node=node_at;
            auto& q=d.alpha_queue?c.alpha:c.opaque;
            if(q.count<q.capacity){
                const auto slot=q.count;++q.count;
                d.matrix=matrix_slot_404a80(q,d,c.matrices);
                std::memcpy(q.entries[slot].data(),static_cast<const DrawEntry*>(&d),sizeof(DrawEntry));
            }
        }
        if(has_child)walk_404d10(c,bank,obj,d,node.child,true);
        const std::int32_t sibling=node.sibling;
        if(pushed){driving::pc_matrix_pop(c.matrices);d.matrix_dirty=1;}
        --c.flag_depth_8999a0;
        if(c.flag_depth_8999a0<0||c.flag_depth_8999a0>=64)throw std::out_of_range("899560 node stack");
        saved=c.flag_stack_899560[std::size_t(c.flag_depth_8999a0)];
        c.lod_threshold_8999b8=std::int32_t(saved);
        if(sibling<0)return;
        index=sibling;
    }
}
// The walk of a whole object; in immediate mode (8999B0) the opaque queue
// is flushed at once (4052C0).
void walk_descriptor(PcRenderContext& c,const PcModelBank& bank,const PmtObject& obj,Descriptor& d){
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    outrun::vita_profile::WalkScope walk_scope;
#endif
    if(c.immediate_8999b0==0u){walk_404d10(c,bank,obj,d,0);return;}
    c.immediate_8999b0=0;
    walk_404d10(c,bank,obj,d,0);
    if(!c.flush_4052c0)throw std::runtime_error("immediate mode without a 4052C0 owner");
    c.flush_4052c0();
}
const PcModelBank& bank_of(PcRenderContext& c,u32 id,const char* what){
    const auto* bank=c.bank(id>>16);
    if(!bank)throw std::runtime_error(std::string(what)+" object bank not loaded");
    return *bank;
}
}
void render_queues_reset_405160(PcRenderQueue& o,PcRenderQueue& a){
    o=PcRenderQueue{};a=PcRenderQueue{};
    o.capacity=0x100;o.matrix_capacity=0x100;o.entries.resize(0x100);o.matrices.resize(0x100);
    a.capacity=0x600;a.matrix_capacity=0x600;a.entries.resize(0x600);a.matrices.resize(0x600);
}
bool render_node_visible_404b00(PcRenderContext& c,Bytes node,CourseProbe& v,std::int32_t& lod_out){
    return node_visible(c,pmt<PmtNode>(node,0),v,lod_out);
}
void render_rotate_axis_40a580(PcMatrixStack& s,const CourseProbe& a,float sn,float cs){rotate_axis_40a580(s,a,sn,cs);}
// 405360: object `id` queued (opaque or alpha) with its colour list; a node
// list (u16, 0xFFFF-terminated) walks only those nodes, each with its
// sibling link cut for the walk (4050F0).
void render_object_405360(PcRenderContext& c,std::uint32_t id,std::int32_t opaque,Bytes node_list,
    std::uint32_t colour,std::int32_t colour_byte,std::int32_t flag){
    if(id==Invalid)return;
    OR2_PERF_ZONE("405360 object");
    const auto& bank=bank_of(c,id,"405360");
    const auto index=id&0xffffu;
    if(index>=bank.object_count)return;
    const auto object=object_offset(index);
    const auto obj=pmt<PmtObject>(bank.system,object);
    auto d=descriptor(id,id>>16,object,opaque,colour,colour_byte);
    d.flags=flag!=0?1u:0u;
    auto build=[&]{
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        outrun::vita_profile::WalkScope walk_scope;
#else
        OR2_PERF_ZONE("4050F0 walk");
#endif
        if(node_list.size()==0){walk_404d10(c,bank,obj,d,0,true);return;}
        for(std::size_t k=0;;k+=2){
            const auto n=node_list.i16(k);if(std::uint16_t(n)==0xffffu)return;
            const auto sibling=obj.nodes+u32(std::int32_t(n))*0x38u+0x24u;
            const auto saved=bank.system.u32(sibling);
            bank.system.put32(sibling,Invalid);
            walk_404d10(c,bank,obj,d,n,false);
            bank.system.put32(sibling,saved);
        }
    };
    if(c.immediate_8999b0==0u){build();return;}
    c.immediate_8999b0=0;
    build();
    if(!c.flush_4052c0)throw std::runtime_error("405360 immediate mode without a 4052C0 owner");
    c.flush_4052c0();
}
// 405450: object a drawn with object b of the same bank as its morph
// target (stream 1) at the weight 95AECC.
void render_object_morph_405450(PcRenderContext& c,std::uint32_t a,std::uint32_t b,std::int32_t opaque,
    std::uint32_t colour,std::int32_t colour_byte){
    if(a==Invalid)return;
    const auto& bank_a=bank_of(c,a,"405450");
    if((a&0xffffu)>=bank_a.object_count)return;
    if(b==Invalid)return;
    const auto& bank_b=bank_of(c,b,"405450 morph");
    if((b&0xffffu)>=bank_b.object_count)return;
    if((a>>16)!=(b>>16))throw std::runtime_error("405450 morph target in another bank (descriptor +10 is b's bank)");
    const auto object=object_offset(a&0xffffu);
    auto d=descriptor(a,b>>16,object,opaque,colour,colour_byte);
    d.morph_target=object_offset(b&0xffffu);d.morph_weight=c.morph_weight_95aecc;
    walk_descriptor(c,bank_a,pmt<PmtObject>(bank_a.system,object),d);
}
// 405580: the object with a palette of `count` matrices appended to the
// queue's pool (nothing when it does not fit), walked under palette[0].
void render_object_palette_405580(PcRenderContext& c,std::uint32_t id,std::int32_t opaque,std::uint32_t colour,
    std::int32_t colour_byte,const std::uint8_t* palette,std::uint32_t count){
    if(id==Invalid)return;
    const auto& bank=bank_of(c,id,"405580");
    if((id&0xffffu)>=bank.object_count)return;
    const auto object=object_offset(id&0xffffu);
    auto d=descriptor(id,id>>16,object,opaque,colour,colour_byte);
    d.palette_count=count;d.matrix_dirty=0;
    auto& q=opaque!=0?c.opaque:c.alpha;
    if(std::int32_t(q.matrix_count+count)>std::int32_t(q.matrix_capacity))return;
    for(std::uint32_t k=0;k<count;++k){
        if(q.matrix_count+k>=q.matrices.size())throw std::out_of_range("405580 matrix pool");
        std::memcpy(q.matrices[q.matrix_count+k].data(),palette+k*64u,64);
    }
    q.matrix_count+=count;
    auto& s=c.matrices;
    ++s.depth;
    if(s.depth<s.capacity){s.current_offset+=64;auto cur=s.current();cur.check(0,64);std::memcpy(cur.data(),palette,64);}
    walk_descriptor(c,bank,pmt<PmtObject>(bank.system,object),d);
    --s.depth;
    if(s.depth>=0)s.current_offset-=64;
}
// 4056D0: every colour record's +0C set to value (the old values kept in
// the opaque queue's undo list for 4052C0; nothing when the list would
// reach 0x80), then 405360 into the opaque queue.
void render_object_override_4056d0(PcRenderContext& c,std::uint32_t id,std::uint32_t value,
    std::uint32_t colour,std::int32_t colour_byte){
    if(id==Invalid)return;
    const auto& bank=bank_of(c,id,"4056D0");
    const auto index=id&0xffffu;
    if(index>=bank.object_count)return;
    const auto obj=pmt<PmtObject>(bank.system,object_offset(index));
    const auto n=bank.system.u32(obj.info+0x30);
    if(std::int32_t(std::uint32_t(c.opaque.undo.size())+n)>=0x80)return;
    for(std::uint32_t k=0;k<n;++k){
        const auto a=obj.colours+k*0x48u+0xcu;
        c.opaque.undo.push_back({id>>16,a,bank.system.u32(a)});
        bank.system.put32(a,value);
    }
    render_object_405360(c,id,1,Bytes(nullptr,0),colour,colour_byte,0);
}
}
