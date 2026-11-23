#include "vita_gl_d3d9.hpp"
#include "vita_gl_display.hpp"
#include "vita_profiler.hpp"
#include "vita_render_thread.hpp"
#include "platform/pc_dds.hpp"
#include "platform/pc_vertex_shader_setup.hpp"
#include "platform/pc_render_flush.hpp"
#include "platform/pc_shader_programs.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

namespace outrun::ps5_runtime {
using namespace outrun::platform;
using namespace outrun::platform::d3d9;
using outrun::platform::pc_shader::V4;
static constexpr std::uint32_t KBuffer = 1, KDecl = 2, KVs = 3, KPs = 4, KTexture = 5, KSurface = 6;
static int s_fail_print_count = 0;

struct LastDraw3DState {
    bool valid = false;
    uint32_t type = 0;
    int32_t base = 0;
    uint32_t decl_handle = 0;
    uint32_t vs_id = 0;
    uint32_t ps_id = 0;
    uint32_t tex0_handle = 0;
    uint32_t tex1_handle = 0;
    VitaGlD3D9Device::BatchKey key{};
    uint32_t tex_mat_mode = 0;
    uint32_t pos_buffer = 0;
    uint32_t pos_offset = 0;
    uint32_t pos_stride = 0;
    uint32_t stage0_uv = 0;
    uint32_t stage1_uv = 0;
    uint32_t col_buffer = 0;
    uint32_t col_offset = 0;
    uint32_t col_stride = 0;
    float const_col[4]{};
    bool is_glass = false;
};
static LastDraw3DState s_last_3d;

struct ShadowGlState {
    int cull_enabled{-1};
    GLenum cull_face{0};
    int depth_test{-1};
    GLboolean depth_mask{0xff};
    GLenum depth_func{0};
    int blend_enabled{-1};
    GLenum blend_src{0}, blend_dst{0};
    int alpha_test{-1};
    GLenum alpha_func{0};
    GLclampf alpha_ref{-1.0f};
    GLboolean color_mask_r{0xff}, color_mask_g{0xff}, color_mask_b{0xff}, color_mask_a{0xff};
};
static ShadowGlState s_shadow_gl{};

static inline GLenum to_gl_blend(uint32_t d3d_blend, GLenum fallback) {
    switch (d3d_blend) {
        case 1: return GL_ZERO;                     // D3DBLEND_ZERO
        case 2: return GL_ONE;                      // D3DBLEND_ONE
        case 3: return GL_SRC_COLOR;                // D3DBLEND_SRCCOLOR
        case 4: return GL_ONE_MINUS_SRC_COLOR;      // D3DBLEND_INVSRCCOLOR
        case 5: return GL_SRC_ALPHA;                // D3DBLEND_SRCALPHA
        case 6: return GL_ONE_MINUS_SRC_ALPHA;      // D3DBLEND_INVSRCALPHA
        case 7: return GL_DST_ALPHA;                // D3DBLEND_DESTALPHA
        case 8: return GL_ONE_MINUS_DST_ALPHA;      // D3DBLEND_INVDESTALPHA
        case 9: return GL_DST_COLOR;                // D3DBLEND_DESTCOLOR
        case 10: return GL_ONE_MINUS_DST_COLOR;     // D3DBLEND_INVDESTCOLOR
        default: return fallback;
    }
}

inline bool render_states_equal(const VitaGlD3D9Device::BatchKey& a, const VitaGlD3D9Device::BatchKey& b) {
    return a.rhw == b.rhw &&
           a.blend_enable == b.blend_enable &&
           a.src_blend == b.src_blend &&
           a.dst_blend == b.dst_blend &&
           a.alpha_test == b.alpha_test &&
           a.alpha_func == b.alpha_func &&
           a.alpha_ref == b.alpha_ref &&
           a.z_enable == b.z_enable &&
           a.z_write == b.z_write &&
           a.z_func == b.z_func &&
           a.cull == b.cull &&
           a.color_mask == b.color_mask &&
           a.vpx == b.vpx && a.vpy == b.vpy &&
           a.vpw == b.vpw && a.vph == b.vph;
}

VitaGlD3D9Device::VitaGlD3D9Device(unsigned width, unsigned height)
    : platform::PcSoftD3D9Device(width, height), back_w_(width ? width : 960), back_h_(height ? height : 544) {
    batch_.reserve(4096);
    for (int i = 0; i < 2; ++i) {
        async_slots_[i].self = this;
        async_slots_[i].cmds.reserve(2048);
        async_slots_[i].floats.reserve(8192);
        async_slots_[i].up_bytes.reserve(128 * 1024);
    }
}

VitaGlD3D9Device::~VitaGlD3D9Device() {
    outrun::vita_render::run_sync([this]() {
        for (auto& cb : cdram_buffers_) {
            if (cb.vbo) {
                glDeleteBuffers(1, &cb.vbo);
                if (cb.vram_ptr) vglFree(cb.vram_ptr);
            }
        }
        cdram_buffers_.clear();
        for (auto& t : textures_gl_) {
            if (t.id) glDeleteTextures(1, &t.id);
        }
        textures_gl_.clear();
    });
}

void VitaGlD3D9Device::wait_async_idle() {
    outrun::vita_render::wait_idle();
}

void VitaGlD3D9Device::snapshot_live_state(ReplayState& dst) const {
    dst.render = render;
    dst.sampler = sampler;
    dst.stage = stage;
    dst.transform_world = transform_world;
    dst.transform_view = transform_view;
    dst.transform_proj = transform_proj;
    dst.textures = textures;
    dst.streams = streams;
    dst.indices = indices;
    dst.declaration = declaration;
    dst.vertex_shader = vertex_shader;
    dst.pixel_shader = pixel_shader;
    dst.fvf = fvf_;
    for (std::size_t k = 0; k < dst.vs_constants.size(); ++k) {
        dst.vs_constants[k] = vs_constants[k];
    }
}

void VitaGlD3D9Device::begin_async_frame() {
    auto& slot = async_slots_[write_slot_];
    outrun::vita_render::wait_slot(slot.busy);
    slot.reset();
    snapshot_live_state(slot.initial_state);
    has_3d_draws_ = false;
    submitted_draws = 0;
    async_recording_ = true;
}

void VitaGlD3D9Device::submit_async_frame(VitaGlDisplay* display) {
    async_recording_ = false;
    auto& slot = async_slots_[write_slot_];
    slot.display = display;
    outrun::vita_render::submit_async(execute_async_slot_thunk, &slot, &slot.busy);
    write_slot_ ^= 1;
}

void VitaGlD3D9Device::execute_async_slot_thunk(void* arg) {
    auto* slot = static_cast<AsyncFrameSlot*>(arg);
    if (slot && slot->self) {
        slot->self->execute_async_slot(*slot);
    }
}

void VitaGlD3D9Device::execute_async_slot(AsyncFrameSlot& slot) {
    replaying_async_ = true;
    replay_ = slot.initial_state;
    begin_frame_gl();
    for (const auto& cmd : slot.cmds) {
        switch (cmd.kind) {
            case GlCmdKind::SetRS:
                if (cmd.u0 < replay_.render.size()) replay_.render[cmd.u0] = cmd.u1;
                break;
            case GlCmdKind::SetTex:
                if (cmd.u0 < replay_.textures.size()) replay_.textures[cmd.u0] = cmd.u1;
                break;
            case GlCmdKind::SetSampler:
                if (cmd.u0 < replay_.sampler.size() && cmd.u1 < replay_.sampler[0].size())
                    replay_.sampler[cmd.u0][cmd.u1] = cmd.u2;
                break;
            case GlCmdKind::SetStage:
                if (cmd.u0 < replay_.stage.size() && cmd.u1 < replay_.stage[0].size())
                    replay_.stage[cmd.u0][cmd.u1] = cmd.u2;
                break;
            case GlCmdKind::SetFvf:
                replay_.fvf = cmd.u0;
                break;
            case GlCmdKind::SetStream:
                if (cmd.u0 < replay_.streams.size()) replay_.streams[cmd.u0] = {cmd.u1, cmd.u2, cmd.u3};
                break;
            case GlCmdKind::SetIndices:
                replay_.indices = cmd.u0;
                break;
            case GlCmdKind::SetDecl:
                replay_.declaration = cmd.u0;
                break;
            case GlCmdKind::SetVS:
                replay_.vertex_shader = cmd.u0;
                break;
            case GlCmdKind::SetPS:
                replay_.pixel_shader = cmd.u0;
                break;
            case GlCmdKind::SetTransform: {
                auto* t = cmd.u0 == 0x100u ? &replay_.transform_world
                        : cmd.u0 == 2u     ? &replay_.transform_view
                        : cmd.u0 == 3u     ? &replay_.transform_proj : nullptr;
                if (t && cmd.u1 + 16u <= slot.floats.size()) {
                    std::memcpy(t->data(), slot.floats.data() + cmd.u1, 64);
                    if (!batch_.empty()) flush();
                    wvp_dirty_ = true;
                    s_last_3d.valid = false;
                }
                break;
            }
            case GlCmdKind::SetVsc: {
                const std::uint32_t start = cmd.u0;
                const std::uint32_t n = cmd.u1;
                const std::uint32_t foff = cmd.u2;
                bool wvp_changed = false;
                bool col_changed = false;
                if (foff + n * 4u <= slot.floats.size()) {
                    const float* src = slot.floats.data() + foff;
                    for (std::uint32_t k = 0; k < n && start + k < replay_.vs_constants.size(); ++k) {
                        const std::uint32_t reg = start + k;
                        std::memcpy(replay_.vs_constants[reg].data(), src + k * 4u, 16);
                        if (reg >= 64 && reg <= 71) wvp_changed = true;
                        if (reg >= 18 && reg <= 20) col_changed = true;
                    }
                }
                if (wvp_changed) {
                    if (!batch_.empty()) flush();
                    wvp_dirty_ = true;
                }
                if (col_changed) {
                    s_last_3d.const_col[0] = 999.0f;
                }
                break;
            }
            case GlCmdKind::Clear:
                clear_gl(cmd.u0, cmd.u1, cmd.f0, cmd.u2);
                break;
            case GlCmdKind::DrawIndexed:
                outrun::vita_profile::set_active_model(cmd.res_id, cmd.obj_id);
                draw_indexed_primitive_gl(replay_, cmd.u0, cmd.i0, cmd.u1, cmd.u2, cmd.u3, cmd.u4);
                break;
            case GlCmdKind::DrawUp:
                if (cmd.u3 < slot.up_bytes.size()) {
                    if (last_ibo_ != 0) {
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                        last_ibo_ = 0;
                    }
                    draw_primitive_up_gl(replay_, cmd.u0, cmd.u1, slot.up_bytes.data() + cmd.u3, cmd.u2);
                }
                break;
        }
    }
    end_frame_gl();
    replaying_async_ = false;
    if (slot.display) {
        if (!released_textures_.empty()) {
            slot.display->release_pc_textures(released_textures_);
        }
        std::string dummy_err;
        slot.display->present(dummy_err);
    }
}

void VitaGlD3D9Device::resize_back_buffer(unsigned w, unsigned h) {
    outrun::vita_render::wait_idle();
    scale_zero_ = (w == 0 || h == 0);
    back_w_ = 960;
    back_h_ = 544;
}

void VitaGlD3D9Device::begin_frame() {
    outrun::vita_render::run_sync([this]() { begin_frame_gl(); });
}

void VitaGlD3D9Device::begin_frame_gl() {
    last_ibo_ = 0;
    s_shadow_gl = ShadowGlState{};
    s_fail_print_count = 0;
    triangles_drawn = 0;
    has_3d_draws_ = false;
    batch_.clear();
    last_queried_handle_ = 0;
    last_queried_id_ = 0;
    applied_2d_blend_key_ = 0xFFFFFFFFu;
    applied_2d_tex_ = 0xFFFFFFFFu;
    applied_2d_vx_ = -1;
    batch_key_ = BatchKey{};
    current_applied_state_ = BatchKey{};
    state_dirty_ = true;
    last_bound_texture_[0] = last_bound_texture_[1] = 0;
    tex2d_enabled_[0] = tex2d_enabled_[1] = false;
    tex_env_mode_[0] = tex_env_mode_[1] = TEX_ENV_UNKNOWN;
    tex_matrix_identity_ = true;
    client_vertex_enabled_ = false;
    client_texcoord_enabled_[0] = client_texcoord_enabled_[1] = false;
    client_color_enabled_ = false;
    color_const_white_ = false;
    last_vp_x_ = last_vp_y_ = last_vp_w_ = last_vp_h_ = -1;
    last_min_z_ = last_max_z_ = -1.0f;
    wvp_dirty_ = true;
    s_last_3d.valid = false;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    current_active_unit_ = GL_TEXTURE0;
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    if (outrun::vita_profile::g_capture_render_frame) {
        snapshot_draw_count_ = 0;
        snapshot_hw_draws_ = 0;
        snapshot_sw_draws_ = 0;
        snapshot_flush_count_ = 0;
        std::fill(std::begin(snapshot_flush_reasons_), std::end(snapshot_flush_reasons_), 0u);
        snapshot_draw_3d_count_ = 0;
        snapshot_draw_up_count_ = 0;
        snapshot_t_tex_lookup_bind_us_ = 0;
        snapshot_t_state_setup_us_ = 0;
        snapshot_t_gl_draw_elements_us_ = 0;
        snapshot_t_gl_draw_arrays_2d_us_ = 0;
        snapshot_t_vertex_copy_pack_us_ = 0;

        outrun::vita_profile::bottleneck_log(
            "--- [CAPTURED FRAME START (Zone: %s)] ---\n",
            outrun::vita_profile::get_current_zone().c_str()
        );
    }
}

void VitaGlD3D9Device::finish_scene() {
    if (async_recording_) return;
    outrun::vita_render::run_sync([this]() { flush(); });
}

void VitaGlD3D9Device::end_frame() {
    if (async_recording_) return;
    outrun::vita_render::run_sync([this]() { end_frame_gl(); });
}

void VitaGlD3D9Device::end_frame_gl() {
    if (last_ibo_ != 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        last_ibo_ = 0;
    }
    flush();

    if (outrun::vita_profile::g_capture_render_frame) {
        const std::string& zone = outrun::vita_profile::get_current_zone();
        const std::uint32_t total_measured_us = snapshot_t_vertex_copy_pack_us_
                                              + snapshot_t_state_setup_us_
                                              + snapshot_t_tex_lookup_bind_us_
                                              + snapshot_t_gl_draw_elements_us_
                                              + snapshot_t_gl_draw_arrays_2d_us_;

        outrun::vita_profile::bottleneck_log(
            "\n=== [SNAPSHOT FRAME SUMMARY (Zone: %s)] ===\n"
            "Total Frame Draws: %u (HW: %u, SW: %u)\n"
            "2D Flushes: %u (Triggers -> TexChange: %u, BlendChange: %u, BufFull: %u, 3DTrans: %u, EndScene: %u)\n"
            "Sub-stage Durations:\n"
            "  - Vertex Copy / Packing : %u us (%.2f ms)\n"
            "  - State & Matrix Setup  : %u us (%.2f ms)\n"
            "  - Texture Lookup & Bind : %u us (%.2f ms)\n"
            "  - glDrawElements (3D)   : %u us (%.2f ms)\n"
            "  - glDrawArrays (2D)     : %u us (%.2f ms)\n"
            "Total Measured Bottleneck: %u us (%.2f ms)\n"
            "===========================================\n\n",
            zone.c_str(),
            snapshot_draw_count_, snapshot_hw_draws_, snapshot_sw_draws_,
            snapshot_flush_count_,
            snapshot_flush_reasons_[FLUSH_REASON_TEX_CHANGE],
            snapshot_flush_reasons_[FLUSH_REASON_BLEND_CHANGE],
            snapshot_flush_reasons_[FLUSH_REASON_BUFFER_FULL],
            snapshot_flush_reasons_[FLUSH_REASON_3D_TRANSITION],
            snapshot_flush_reasons_[FLUSH_REASON_END_SCENE],
            snapshot_t_vertex_copy_pack_us_, double(snapshot_t_vertex_copy_pack_us_) / 1000.0,
            snapshot_t_state_setup_us_, double(snapshot_t_state_setup_us_) / 1000.0,
            snapshot_t_tex_lookup_bind_us_, double(snapshot_t_tex_lookup_bind_us_) / 1000.0,
            snapshot_t_gl_draw_elements_us_, double(snapshot_t_gl_draw_elements_us_) / 1000.0,
            snapshot_t_gl_draw_arrays_2d_us_, double(snapshot_t_gl_draw_arrays_2d_us_) / 1000.0,
            total_measured_us, double(total_measured_us) / 1000.0
        );
        outrun::vita_profile::bottleneck_flush();
        outrun::vita_profile::g_capture_render_frame = false;
    }
}

void VitaGlD3D9Device::set_vertex_shader_constant_f(std::uint32_t start, const float* data, std::uint32_t vec4_count) {
    if (!data || !vec4_count) return;
    bool any_changed = false;
    bool wvp_changed = false;
    bool col_changed = false;

    for (std::uint32_t k = 0; k < vec4_count && start + k < vs_constants.size(); ++k) {
        const std::uint32_t reg = start + k;
        if (std::memcmp(vs_constants[reg].data(), data + k * 4, 16) != 0) {
            std::memcpy(vs_constants[reg].data(), data + k * 4, 16);
            any_changed = true;
            if (reg >= 64 && reg <= 71) {
                wvp_changed = true;
            }
            if (reg >= 18 && reg <= 20) {
                col_changed = true;
            }
        }
    }

    if (async_recording_) {
        if (any_changed && start < 128u) {
            auto& slot = async_slots_[write_slot_];
            const std::uint32_t n = std::min<std::uint32_t>(vec4_count, 128u - start);
            const std::uint32_t foff = static_cast<std::uint32_t>(slot.floats.size());
            slot.floats.insert(slot.floats.end(), data, data + n * 4u);
            GlCmd c{};
            c.kind = GlCmdKind::SetVsc;
            c.u0 = start; c.u1 = n; c.u2 = foff;
            slot.cmds.push_back(c);
        }
        return;
    }

    if (wvp_changed) {
        if (!batch_.empty()) {
            flush();
        }
        wvp_dirty_ = true;
    }
    if (col_changed) {
        s_last_3d.const_col[0] = 999.0f;
    }
    if (any_changed && start < vs_constants.size()) {
        vs_dirty_lo = std::min(vs_dirty_lo, start);
        vs_dirty_hi = std::max(vs_dirty_hi, std::min<std::uint32_t>(start + vec4_count, std::uint32_t(vs_constants.size())));
    }
}

void VitaGlD3D9Device::set_transform(std::uint32_t state, const float matrix[16]) {
    if (async_recording_) {
        PcSoftD3D9Device::set_transform(state, matrix);
        if (state == 0x100u || state == 2u || state == 3u) {
            auto& slot = async_slots_[write_slot_];
            const std::uint32_t foff = static_cast<std::uint32_t>(slot.floats.size());
            slot.floats.insert(slot.floats.end(), matrix, matrix + 16);
            GlCmd c{};
            c.kind = GlCmdKind::SetTransform;
            c.u0 = state; c.u1 = foff;
            slot.cmds.push_back(c);
        }
        return;
    }
    if (!batch_.empty()) {
        flush();
    }
    wvp_dirty_ = true;
    s_last_3d.valid = false;
    PcSoftD3D9Device::set_transform(state, matrix);
}

void VitaGlD3D9Device::set_stream_source(std::uint32_t stream, std::uint32_t buffer, std::uint32_t offset, std::uint32_t stride) {
    if (stream < streams.size() &&
        streams[stream].buffer == buffer && streams[stream].offset == offset && streams[stream].stride == stride) {
        return;
    }
    PcSoftD3D9Device::set_stream_source(stream, buffer, offset, stride);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetStream;
        c.u0 = stream; c.u1 = buffer; c.u2 = offset; c.u3 = stride;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_indices(std::uint32_t buffer) {
    if (indices == buffer) return;
    PcSoftD3D9Device::set_indices(buffer);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetIndices;
        c.u0 = buffer;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_vertex_declaration(std::uint32_t decl) {
    if (declaration == decl) return;
    PcSoftD3D9Device::set_vertex_declaration(decl);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetDecl;
        c.u0 = decl;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_vertex_shader(std::uint32_t shader) {
    if (vertex_shader == shader) return;
    PcSoftD3D9Device::set_vertex_shader(shader);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetVS;
        c.u0 = shader;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_pixel_shader(std::uint32_t shader) {
    if (pixel_shader == shader) return;
    PcSoftD3D9Device::set_pixel_shader(shader);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetPS;
        c.u0 = shader;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_texture(std::uint32_t stage_idx, std::uint32_t tex) {
    if (stage_idx < textures.size() && textures[stage_idx] == tex) return;
    PcSoftD3D9Device::set_texture(stage_idx, tex);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetTex;
        c.u0 = stage_idx; c.u1 = tex;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_sampler_state(std::uint32_t stage_idx, std::uint32_t type, std::uint32_t value) {
    if (stage_idx < sampler.size() && type < sampler[0].size() && sampler[stage_idx][type] == value) return;
    PcSoftD3D9Device::set_sampler_state(stage_idx, type, value);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetSampler;
        c.u0 = stage_idx; c.u1 = type; c.u2 = value;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_texture_stage_state(std::uint32_t stage_idx, std::uint32_t type, std::uint32_t value) {
    if (stage_idx < stage.size() && type < stage[0].size() && stage[stage_idx][type] == value) return;
    PcSoftD3D9Device::set_texture_stage_state(stage_idx, type, value);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetStage;
        c.u0 = stage_idx; c.u1 = type; c.u2 = value;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_fvf(std::uint32_t fvf) {
    if (fvf_ == fvf) return;
    PcSoftD3D9Device::set_fvf(fvf);
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::SetFvf;
        c.u0 = fvf;
        async_slots_[write_slot_].cmds.push_back(c);
    }
}

void VitaGlD3D9Device::set_render_target(std::uint32_t index, std::uint32_t surface) {
    if (surface == colour_target_) return;
    if (!async_recording_ && !batch_.empty()) {
        flush();
    }
    PcSoftD3D9Device::set_render_target(index, surface);
}

void VitaGlD3D9Device::set_depth_stencil_surface(std::uint32_t surface) {
    if (surface == depth_target_) return;
    if (!async_recording_ && !batch_.empty()) {
        flush();
    }
    PcSoftD3D9Device::set_depth_stencil_surface(surface);
}

void VitaGlD3D9Device::set_viewport(const std::uint32_t v[6]) {
    PcSoftD3D9Device::set_viewport(v);
    if (async_recording_) return;
    float mn, mx;
    std::memcpy(&mn, &v[4], 4);
    std::memcpy(&mx, &v[5], 4);
    outrun::vita_render::run_sync([this, mn, mx]() {
        if (!batch_.empty()) {
            flush();
        }
        active_matrix_ = MATRIX_IDENTITY;
        applied_2d_vx_ = -1;
        // Always force 3D viewport to full native PS Vita screen (960x544) starting at (0, 0)
        glViewport(0, 0, int(back_w_), int(back_h_));
        glDepthRange(mn, mx);
        last_vp_x_ = 0;
        last_vp_y_ = 0;
        last_vp_w_ = int(back_w_);
        last_vp_h_ = int(back_h_);
        last_min_z_ = mn;
        last_max_z_ = mx;
        state_dirty_ = true;
        s_last_3d.valid = false;
    });
}

// Equivalente de vglUseVram para vitaGL (fuerza uso de memoria VRAM/CDRAM)
static inline void vglUseVram(GLboolean usage) {
    vglUseVramForUSSE(usage);
}

void VitaGlD3D9Device::set_render_state(std::uint32_t state, std::uint32_t value) {
    if (state < render.size()) {
        if (render[state] == value) return; // Shadow state cache: return immediately if state unchanged
        render[state] = value;
        if (async_recording_) {
            GlCmd c{};
            c.kind = GlCmdKind::SetRS;
            c.u0 = state; c.u1 = value;
            async_slots_[write_slot_].cmds.push_back(c);
        }
    }
}

std::uint32_t VitaGlD3D9Device::create_vertex_declaration(const platform::PcVertexElement* elements) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_vertex_declaration(elements);
}

std::uint32_t VitaGlD3D9Device::create_vertex_shader(const std::uint32_t* words) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_vertex_shader(words);
}

std::uint32_t VitaGlD3D9Device::create_pixel_shader(const std::uint32_t* words) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_pixel_shader(words);
}

std::uint32_t VitaGlD3D9Device::create_texture_from_file(const platform::PcTextureFileRequest& request) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_texture_from_file(request);
}

std::uint32_t VitaGlD3D9Device::create_texture(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_texture(width, height, levels, usage, format, pool);
}

std::uint32_t VitaGlD3D9Device::create_cube_texture(std::uint32_t edge, std::uint32_t levels, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_cube_texture(edge, levels, usage, format, pool);
}

std::uint32_t VitaGlD3D9Device::create_depth_stencil_surface(std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::create_depth_stencil_surface(width, height, format, a, b, c);
}

std::uint32_t VitaGlD3D9Device::get_cube_map_surface(std::uint32_t cube, std::uint32_t face, std::uint32_t level) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::get_cube_map_surface(cube, face, level);
}

std::uint32_t VitaGlD3D9Device::get_surface_level(std::uint32_t texture, std::uint32_t level) {
    outrun::vita_render::wait_idle();
    return PcSoftD3D9Device::get_surface_level(texture, level);
}

std::uint32_t VitaGlD3D9Device::create_vertex_buffer(std::uint32_t length, std::uint32_t usage, std::uint32_t fvf, std::uint32_t pool) {
    outrun::vita_render::wait_idle();
    const std::uint32_t h = PcSoftD3D9Device::create_vertex_buffer(length, usage, fvf, pool);
    if (!h) return 0;
    if (h >= cdram_buffers_.size()) cdram_buffers_.resize(h + 1);
    if (length > 0) {
        outrun::vita_render::run_sync([this, h, length]() {
            vglUseVram(GL_TRUE);
            void* vram = vglAlloc(length, VGL_MEM_VRAM);
            if (!vram) vram = vglAlloc(length, VGL_MEM_PHYCONT);
            if (!vram) vram = vglAlloc(length, VGL_MEM_RAM);
            vglUseVram(GL_FALSE);
            if (vram) {
                GLuint vbo = 0;
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                vglBufferData(GL_ARRAY_BUFFER, vram);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                cdram_buffers_[h] = CdramBuffer{vbo, vram, length};
            }
        });
    }
    return h;
}

std::uint32_t VitaGlD3D9Device::create_index_buffer(std::uint32_t length, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) {
    outrun::vita_render::wait_idle();
    const std::uint32_t h = PcSoftD3D9Device::create_index_buffer(length, usage, format, pool);
    if (!h) return 0;
    if (h >= cdram_buffers_.size()) cdram_buffers_.resize(h + 1);
    if (length > 0) {
        outrun::vita_render::run_sync([this, h, length]() {
            vglUseVram(GL_TRUE);
            void* vram = vglAlloc(length, VGL_MEM_VRAM);
            if (!vram) vram = vglAlloc(length, VGL_MEM_PHYCONT);
            if (!vram) vram = vglAlloc(length, VGL_MEM_RAM);
            vglUseVram(GL_FALSE);
            if (vram) {
                GLuint ibo = 0;
                glGenBuffers(1, &ibo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
                vglBufferData(GL_ELEMENT_ARRAY_BUFFER, vram);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                last_ibo_ = 0;
                cdram_buffers_[h] = CdramBuffer{ibo, vram, length};
            }
        });
    }
    return h;
}

std::uint8_t* VitaGlD3D9Device::lock(std::uint32_t buffer, std::uint32_t offset, std::uint32_t size, std::uint32_t flags) {
    outrun::vita_render::wait_idle();
    s_last_3d.valid = false;
    if (buffer < vb_col_cache_.size() && !vb_col_cache_[buffer].rgba.empty()) {
        vb_col_cache_bytes_ -= vb_col_cache_[buffer].rgba.size();
        std::vector<std::uint8_t>().swap(vb_col_cache_[buffer].rgba);
    }
    if (buffer < cdram_buffers_.size() && cdram_buffers_[buffer].vram_ptr) {
        auto& cb = cdram_buffers_[buffer];
        if (offset <= cb.size && size <= cb.size - offset) {
            return reinterpret_cast<std::uint8_t*>(cb.vram_ptr) + offset;
        }
    }
    return PcSoftD3D9Device::lock(buffer, offset, size, flags);
}

void VitaGlD3D9Device::unlock(std::uint32_t buffer) {
    if (buffer < cdram_buffers_.size() && cdram_buffers_[buffer].vram_ptr) {
        auto* o = object(buffer);
        if (o && o->bytes.size() == cdram_buffers_[buffer].size) {
            std::memcpy(o->bytes.data(), cdram_buffers_[buffer].vram_ptr, o->bytes.size());
        }
    }
    PcSoftD3D9Device::unlock(buffer);
}

std::uint32_t VitaGlD3D9Device::buffer_size(std::uint32_t buffer) {
    if (buffer < cdram_buffers_.size() && cdram_buffers_[buffer].size > 0) {
        return std::uint32_t(cdram_buffers_[buffer].size);
    }
    return PcSoftD3D9Device::buffer_size(buffer);
}

void VitaGlD3D9Device::clear(std::uint32_t flags, std::uint32_t colour, float z, std::uint32_t stencil) {
    if (count_only || colour_target_ != BackBuffer) return;
    if (async_recording_) {
        GlCmd c{};
        c.kind = GlCmdKind::Clear;
        c.u0 = flags; c.u1 = colour; c.u2 = stencil; c.f0 = z;
        async_slots_[write_slot_].cmds.push_back(c);
        return;
    }
    outrun::vita_render::run_sync([this, flags, colour, z, stencil]() {
        clear_gl(flags, colour, z, stencil);
    });
}

void VitaGlD3D9Device::clear_gl(std::uint32_t flags, std::uint32_t colour, float z, std::uint32_t stencil) {
    s_last_3d.valid = false;
    const std::uint32_t t0 = outrun::vita_profile::vita_time_us();
    flush();

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);

    glClearColor(float((colour >> 16) & 255) / 255.f,
                 float((colour >> 8)  & 255) / 255.f,
                 float(colour         & 255) / 255.f,
                 float((colour >> 24) & 255) / 255.f);
    glClearDepth(z);
    glClearStencil(int(stencil));

    GLbitfield mask = 0;
    if ((flags & 1) && !preserve_color_on_clear) mask |= GL_COLOR_BUFFER_BIT;
    if (flags & 2) mask |= GL_DEPTH_BUFFER_BIT;
    if (flags & 4) mask |= GL_STENCIL_BUFFER_BIT;
    if (mask) glClear(mask);

    state_dirty_ = true;
    applied_2d_blend_key_ = 0xFFFFFFFFu;
    outrun::vita_profile::g_sub_gl_clear_us += (outrun::vita_profile::vita_time_us() - t0);
}

void VitaGlD3D9Device::unlock_rect(std::uint32_t handle) {
    outrun::vita_render::wait_idle();
    auto* s = object(handle);
    const auto parent = s ? s->parent : 0;
    PcSoftD3D9Device::unlock_rect(handle);
    if (!parent || parent >= textures_gl_.size() || !textures_gl_[parent].id) return;
    outrun::vita_render::run_sync([this, handle, parent]() {
        auto* s2 = object(handle);
        auto* p = object(parent);
        if (!s2 || !p) return;
        auto& t = textures_gl_[parent];
        if (last_bound_texture_[0] != t.id) {
            glActiveTexture(GL_TEXTURE0);
            current_active_unit_ = GL_TEXTURE0;
            glBindTexture(t.target, t.id);
            last_bound_texture_[0] = t.id;
        }
        if (!p->rgba.empty() && s2->face < p->rgba.size() && s2->level < p->rgba[s2->face].size()) {
            const auto& bytes = p->rgba[s2->face][s2->level];
            if (!bytes.empty()) {
                glTexSubImage2D(t.target, int(s2->level), 0, 0, int(s2->width), int(s2->height),
                                GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
                t.has_data = true;
            }
        }
    });
}

void VitaGlD3D9Device::release(std::uint32_t handle) {
    auto* o = object(handle);
    if (!o) return;
    if (o->refs > 1) {
        PcSoftD3D9Device::release(handle);
        return;
    }
    outrun::vita_render::run_sync([this, handle]() {
        s_last_3d.valid = false;
        if (handle == last_queried_handle_) {
            last_queried_handle_ = 0;
            last_queried_id_ = 0;
        }
        if (handle < decl_cache_.size()) decl_cache_[handle] = CachedDecl{};
        if (handle < vs_cache_.size()) vs_cache_[handle] = CachedVs{};
        if (handle < ps_cache_.size()) ps_cache_[handle] = CachedPs{};
        if (handle < vb_col_cache_.size() && !vb_col_cache_[handle].rgba.empty()) {
            vb_col_cache_bytes_ -= vb_col_cache_[handle].rgba.size();
            std::vector<std::uint8_t>().swap(vb_col_cache_[handle].rgba);
        }
        if (handle < textures_gl_.size() && textures_gl_[handle].id) {
            if (last_bound_texture_[0] == textures_gl_[handle].id) last_bound_texture_[0] = 0;
            if (last_bound_texture_[1] == textures_gl_[handle].id) last_bound_texture_[1] = 0;
            if (applied_2d_tex_ == textures_gl_[handle].id) applied_2d_tex_ = 0xFFFFFFFFu;
            glDeleteTextures(1, &textures_gl_[handle].id);
            textures_gl_[handle] = GlTex{};
        }
        if (handle < cdram_buffers_.size() && cdram_buffers_[handle].vbo) {
            if (last_ibo_ == cdram_buffers_[handle].vbo) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                last_ibo_ = 0;
            }
            glDeleteBuffers(1, &cdram_buffers_[handle].vbo);
            if (cdram_buffers_[handle].vram_ptr) vglFree(cdram_buffers_[handle].vram_ptr);
            cdram_buffers_[handle] = CdramBuffer{};
        }
        released_textures_.push_back(handle);
        PcSoftD3D9Device::release(handle);
    });
}

static int s_decal_log_lines = 0;
static void log_decal_debug(const char* fmt, ...) {
    if (!outrun::vita_profile::g_sd_logs_enabled || s_decal_log_lines >= 32) return;
    FILE* f = std::fopen("ux0:data/outrun2006/decal_debug.txt", s_decal_log_lines == 0 ? "w" : "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fclose(f);
    ++s_decal_log_lines;
}

static bool crop_circular_decal(std::vector<std::uint8_t>& px, unsigned width, unsigned height) {
    if (width != 128u || height != 128u || px.size() < 128u * 128u * 4u) return false;

    // Check corners
    const unsigned corners[4] = { 0u, 127u, 127u * 128u, 127u * 128u + 127u };
    for (unsigned idx : corners) {
        const std::uint8_t r = px[idx * 4u + 0u];
        const std::uint8_t g = px[idx * 4u + 1u];
        const std::uint8_t b = px[idx * 4u + 2u];
        const std::uint8_t a = px[idx * 4u + 3u];
        if (!(a <= 32u || (r <= 32u && g <= 32u && b <= 32u))) {
            return false;
        }
    }

    const float cx = 63.5f;
    const float cy = 63.5f;

    // Check Case 1 (Daytona / Dino circle, R ~ 58.5)
    bool case1_in = true, case1_out = true;
    const int d1_in = 52, d1_out = 62;
    const int offsets1[4][2] = {{d1_in, 0}, {-d1_in, 0}, {0, d1_in}, {0, -d1_in}};
    for (const auto& off : offsets1) {
        const int x = int(cx + off[0]), y = int(cy + off[1]);
        const unsigned idx = unsigned(y * 128 + x) * 4u;
        const std::uint8_t r = px[idx], g = px[idx+1], b = px[idx+2], a = px[idx+3];
        if (a <= 32u || (r <= 32u && g <= 32u && b <= 32u)) case1_in = false;
    }
    const int offsets1_out[4][2] = {{d1_out, 0}, {-d1_out, 0}, {0, d1_out}, {0, -d1_out}};
    for (const auto& off : offsets1_out) {
        const int x = int(cx + off[0]), y = int(cy + off[1]);
        const unsigned idx = unsigned(y * 128 + x) * 4u;
        const std::uint8_t r = px[idx], g = px[idx+1], b = px[idx+2], a = px[idx+3];
        if (!(a <= 32u || (r <= 32u && g <= 32u && b <= 32u))) case1_out = false;
    }

    // Check Case 2 (512 BB, R ~ 45.0)
    bool case2_in = true, case2_out = true;
    const int d2_in = 38, d2_out = 50;
    const int offsets2[4][2] = {{d2_in, 0}, {-d2_in, 0}, {0, d2_in}, {0, -d2_in}};
    for (const auto& off : offsets2) {
        const int x = int(cx + off[0]), y = int(cy + off[1]);
        const unsigned idx = unsigned(y * 128 + x) * 4u;
        const std::uint8_t r = px[idx], g = px[idx+1], b = px[idx+2], a = px[idx+3];
        if (a <= 32u || (r <= 32u && g <= 32u && b <= 32u)) case2_in = false;
    }
    const int offsets2_out[4][2] = {{d2_out, 0}, {-d2_out, 0}, {0, d2_out}, {0, -d2_out}};
    for (const auto& off : offsets2_out) {
        const int x = int(cx + off[0]), y = int(cy + off[1]);
        const unsigned idx = unsigned(y * 128 + x) * 4u;
        const std::uint8_t r = px[idx], g = px[idx+1], b = px[idx+2], a = px[idx+3];
        if (!(a <= 32u || (r <= 32u && g <= 32u && b <= 32u))) case2_out = false;
    }

    int decal_type = 0;
    if (case1_in && case1_out) decal_type = 1;
    else if (case2_in && case2_out) decal_type = 2;
    if (!decal_type) return false;

    const float r_max = (decal_type == 2) ? 45.0f : 58.5f;
    const float r_ring = (decal_type == 2) ? 43.5f : 58.5f;

    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const float dx = float(x) - cx;
            const float dy = float(y) - cy;
            const float r = std::sqrt(dx * dx + dy * dy);
            const unsigned idx = unsigned(y * 128 + x) * 4u;
            if (r > r_max) {
                px[idx + 0] = 0;
                px[idx + 1] = 0;
                px[idx + 2] = 0;
                px[idx + 3] = 0;
            } else if (decal_type == 2 && r >= r_ring) {
                // Black outline ring delimiting the circle on 512 OutRun
                px[idx + 0] = 0;
                px[idx + 1] = 0;
                px[idx + 2] = 0;
                px[idx + 3] = 255;
            } else {
                px[idx + 3] = 255;
            }
        }
    }
    return true;
}

static void upload_texture(const std::vector<std::uint8_t>& px, unsigned width, unsigned height, bool has_alpha) {
    const std::size_t total_px = std::size_t(width) * std::size_t(height);
    if (px.size() < total_px * 4u || !total_px) return;

    if (has_alpha) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, int(width), int(height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    } else {
        std::vector<std::uint16_t> buf16(total_px);
        const std::uint32_t* src32 = reinterpret_cast<const std::uint32_t*>(px.data());
        for (std::size_t i = 0; i < total_px; ++i) {
            const std::uint32_t c = src32[i];
            const std::uint16_t r = (c & 0xF8) << 8;
            const std::uint16_t g = (c & 0xFC00) >> 5;
            const std::uint16_t b = (c & 0xF80000) >> 19;
            buf16[i] = r | g | b;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, int(width), int(height), 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, buf16.data());
    }
}

unsigned VitaGlD3D9Device::texture(std::uint32_t handle) {
    if (!handle) return 0;
    if (handle == last_queried_handle_) return last_queried_id_;

    const std::uint32_t t0 = outrun::vita_profile::vita_time_us();

    if (handle < textures_gl_.size() && textures_gl_[handle].id != 0) {
        last_queried_handle_ = handle;
        last_queried_id_ = textures_gl_[handle].id;
        const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
        outrun::vita_profile::g_sub_tex_lookup_bind_us += dt;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_tex_lookup_bind_us_ += dt;
        return last_queried_id_;
    }

    auto* o = object(handle);
    if (!o || o->kind != KTexture) {
        const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
        outrun::vita_profile::g_sub_tex_lookup_bind_us += dt;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_tex_lookup_bind_us_ += dt;
        return 0;
    }

    GlTex t{};
    t.target = GL_TEXTURE_2D;
    glGenTextures(1, &t.id);
    glActiveTexture(GL_TEXTURE0);
    current_active_unit_ = GL_TEXTURE0;
    glBindTexture(GL_TEXTURE_2D, t.id);
    last_bound_texture_[0] = t.id;
    t.is_cube = o->texture.cube;

    if (!o->texture.faces.empty() && !o->texture.faces[0].empty()) {
        const auto& size = o->texture.faces[0][0];
        t.w = size.width;
        t.h = size.height;
        t.is_npot = size.width && size.height &&
                    (((size.width & (size.width - 1u)) != 0u) || ((size.height & (size.height - 1u)) != 0u));
        auto classify_alpha = [&](const std::vector<std::uint8_t>& px) {
            std::size_t zero_a = 0;
            std::size_t mid_a = 0;
            std::size_t mid_colored_a = 0;
            bool has_transp = false;
            for (std::size_t p = 0; p + 3 < px.size(); p += 4) {
                const std::uint8_t r = px[p];
                const std::uint8_t g = px[p + 1];
                const std::uint8_t b = px[p + 2];
                const std::uint8_t a = px[p + 3];
                if (a <= 8u) {
                    ++zero_a;
                    has_transp = true;
                } else if (a < 128u) {
                    ++mid_a;
                    has_transp = true;
                    if (r > 24u || g > 24u || b > 24u) {
                        ++mid_colored_a;
                    }
                } else if (a < 250u) {
                    has_transp = true;
                }
            }
            const std::size_t total_px = px.size() / 4u;
            if ((total_px > 0u && zero_a * 100u < total_px && mid_colored_a * 4u > total_px) ||
                (total_px > 0u && zero_a == 0u && mid_a * 4u > total_px)) {
                t.is_opaque_specular = true;
            }
            t.has_alpha = has_transp && !t.is_opaque_specular;
        };
        if (!o->rgba.empty() && !o->rgba[0].empty() && !o->rgba[0][0].empty() && size.width && size.height) {
            auto& rgba = o->rgba[0][0];
            const bool cropped = crop_circular_decal(rgba, size.width, size.height);
            classify_alpha(rgba);
            if (cropped) {
                t.has_alpha = true;
                t.is_opaque_specular = false;
                t.is_cropped_decal = true;
                log_decal_debug("[UPLOAD_RGBA] handle=%u tex_id=%u %ux%u has_alpha=%d\n",
                                handle, t.id, size.width, size.height, t.has_alpha ? 1 : 0);
            }
            upload_texture(rgba, size.width, size.height, t.has_alpha);
            t.has_data = true;
        } else if (!size.data.empty() && size.width && size.height) {
            std::vector<std::uint8_t> decoded = pc_dds_rgba(o->texture, 0, 0);
            if (!decoded.empty()) {
                const bool cropped = crop_circular_decal(decoded, size.width, size.height);
                classify_alpha(decoded);
                if (cropped) {
                    t.has_alpha = true;
                    t.is_opaque_specular = false;
                    t.is_cropped_decal = true;
                    log_decal_debug("[UPLOAD_DDS] handle=%u tex_id=%u fmt=%u %ux%u has_alpha=%d corner_a=%u center_a=%u\n",
                                    handle, t.id, unsigned(o->texture.format), size.width, size.height,
                                    t.has_alpha ? 1 : 0, unsigned(decoded[3]), unsigned(decoded[(64*128+64)*4+3]));
                }
                upload_texture(decoded, size.width, size.height, t.has_alpha);
                t.has_data = true;
            }
            // Free CPU compressed DDS bytes once uploaded to GL (preserve width/height).
            std::vector<std::uint8_t>().swap(o->texture.faces[0][0].data);
        }
    }
    if (!t.has_data) {
        static const std::uint32_t kFallbackWhite = 0xFFFFFFFFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kFallbackWhite);
        std::printf("[TEX_WARN] Empty texture handle=%u cube=%d fmt=%u (%ux%u)\n",
                    handle, o->texture.cube ? 1 : 0, unsigned(o->texture.format), t.w, t.h);
    } else if (t.is_npot) {
        std::printf("[TEX_NPOT] tex_id=%u handle=%u (%ux%u)\n", t.id, handle, t.w, t.h);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    t.min_filter = GL_LINEAR;
    t.mag_filter = GL_LINEAR;
    t.wrap_s = GL_REPEAT;
    t.wrap_t = GL_REPEAT;

    if (handle >= textures_gl_.size()) {
        textures_gl_.resize(handle + 64);
    }
    textures_gl_[handle] = t;
    last_queried_handle_ = handle;
    last_queried_id_ = t.id;

    const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
    outrun::vita_profile::g_sub_tex_lookup_bind_us += dt;
    if (outrun::vita_profile::g_capture_render_frame) snapshot_t_tex_lookup_bind_us_ += dt;

    return t.id;
}

void VitaGlD3D9Device::bind_texture(unsigned tex_id, std::uint32_t handle, TexEnvMode desired_env) {
    bind_texture_unit(GL_TEXTURE0, 0, tex_id, handle, desired_env);
}

void VitaGlD3D9Device::bind_texture_unit(GLenum unit_enum, unsigned unit_idx, unsigned tex_id, std::uint32_t handle, TexEnvMode desired_env) {
    if (unit_idx >= 2) return;

    if (tex_id) {
        const auto& p = replaying_async_ ? replay_.sampler[unit_idx] : sampler[unit_idx];
        const int min_f = (p[SAMP_MINFILTER] != 1) ? GL_LINEAR : GL_NEAREST;
        const int mag_f = (p[SAMP_MAGFILTER] == 1) ? GL_NEAREST : GL_LINEAR;
        auto wrap_mode = [](unsigned m) -> int {
            if (m == 3 || m == 4) return GL_CLAMP_TO_EDGE;
            if (m == 2) return GL_MIRRORED_REPEAT;
            return GL_REPEAT;
        };
        const int ws = wrap_mode(p[SAMP_ADDRESSU]);
        const int wt = wrap_mode(p[SAMP_ADDRESSV]);

        GlTex* gtex = (handle && handle < textures_gl_.size() && textures_gl_[handle].id == tex_id)
                    ? &textures_gl_[handle]
                    : nullptr;

        const bool tex_same = (tex_id == last_bound_texture_[unit_idx]) && tex2d_enabled_[unit_idx];
        const bool env_same = (tex_env_mode_[unit_idx] == desired_env);
        const bool samp_same = gtex && (gtex->min_filter == min_f && gtex->mag_filter == mag_f &&
                                        gtex->wrap_s == ws && gtex->wrap_t == wt);

        if (tex_same && env_same && samp_same) {
            // Already active and identical in OpenGL - ignore redundant request
            return;
        }

        const std::uint32_t t0 = outrun::vita_profile::vita_time_us();

        if (current_active_unit_ != unit_enum) {
            glActiveTexture(unit_enum);
            current_active_unit_ = unit_enum;
        }

        if (!tex2d_enabled_[unit_idx]) {
            glEnable(GL_TEXTURE_2D);
            tex2d_enabled_[unit_idx] = true;
        }
        if (tex_id != last_bound_texture_[unit_idx]) {
            glBindTexture(GL_TEXTURE_2D, tex_id);
            last_bound_texture_[unit_idx] = tex_id;
            ++outrun::vita_profile::g_texture_binds;
        }

        if (!gtex || gtex->min_filter != min_f) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_f);
            if (gtex) gtex->min_filter = min_f;
        }
        if (!gtex || gtex->mag_filter != mag_f) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_f);
            if (gtex) gtex->mag_filter = mag_f;
        }
        if (!gtex || gtex->wrap_s != ws) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
            if (gtex) gtex->wrap_s = ws;
        }
        if (!gtex || gtex->wrap_t != wt) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
            if (gtex) gtex->wrap_t = wt;
        }

        if (desired_env != tex_env_mode_[unit_idx]) {
            switch (desired_env) {
                case TEX_ENV_MULTITEX_MODULATE:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PREVIOUS);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_TEXTURE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PREVIOUS);
                    break;
                case TEX_ENV_MODULATE_DIFFUSE_ALPHA:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
                    break;
                case TEX_ENV_REPLACE_RGB_DIFFUSE_ALPHA:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
                    break;
                case TEX_ENV_REPLACE_RGBA:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
                    break;
                case TEX_ENV_DECAL_DIFFUSE_ALPHA:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
                    break;
                case TEX_ENV_DIFFUSE_ONLY:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PRIMARY_COLOR);
                    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
                    break;
                case TEX_ENV_MODULATE:
                default:
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    break;
            }
            tex_env_mode_[unit_idx] = desired_env;
        }

        const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
        outrun::vita_profile::g_sub_tex_lookup_bind_us += dt;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_tex_lookup_bind_us_ += dt;
    } else {
        if (!tex2d_enabled_[unit_idx] && last_bound_texture_[unit_idx] == 0) {
            return;
        }
        const std::uint32_t t0 = outrun::vita_profile::vita_time_us();
        if (current_active_unit_ != unit_enum) {
            glActiveTexture(unit_enum);
            current_active_unit_ = unit_enum;
        }
        if (tex2d_enabled_[unit_idx]) {
            glDisable(GL_TEXTURE_2D);
            tex2d_enabled_[unit_idx] = false;
        }
        if (last_bound_texture_[unit_idx] != 0) {
            glBindTexture(GL_TEXTURE_2D, 0);
            last_bound_texture_[unit_idx] = 0;
            ++outrun::vita_profile::g_texture_binds;
        }
        const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
        outrun::vita_profile::g_sub_tex_lookup_bind_us += dt;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_tex_lookup_bind_us_ += dt;
    }
}

void VitaGlD3D9Device::set_client_states(bool vtx, bool tex, bool col) {
    set_client_states(vtx, tex, false, col);
}

void VitaGlD3D9Device::set_client_states(bool vtx, bool tex0, bool tex1, bool col) {
    if (vtx != client_vertex_enabled_) {
        if (vtx) glEnableClientState(GL_VERTEX_ARRAY);
        else glDisableClientState(GL_VERTEX_ARRAY);
        client_vertex_enabled_ = vtx;
    }
    if (tex0 != client_texcoord_enabled_[0]) {
        glClientActiveTexture(GL_TEXTURE0);
        if (tex0) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        client_texcoord_enabled_[0] = tex0;
    }
    if (tex1 != client_texcoord_enabled_[1]) {
        glClientActiveTexture(GL_TEXTURE1);
        if (tex1) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        client_texcoord_enabled_[1] = tex1;
        glClientActiveTexture(GL_TEXTURE0);
    }
    if (col != client_color_enabled_) {
        if (col) glEnableClientState(GL_COLOR_ARRAY);
        else glDisableClientState(GL_COLOR_ARRAY);
        client_color_enabled_ = col;
    }
}

const VitaGlD3D9Device::CachedDecl& VitaGlD3D9Device::get_cached_decl(
    std::uint32_t handle, const platform::PcVertexElement* elements) {
    static CachedDecl fallback;
    CachedDecl* slot = &fallback;
    if (handle) {
        if (handle >= decl_cache_.size()) decl_cache_.resize(handle + 32);
        slot = &decl_cache_[handle];
        if (slot->valid) return *slot;
    }
    *slot = CachedDecl{};
    for (const auto* e = elements; e->stream != platform::PcDeclEndStream; ++e) {
        if (e->usage == 0 && e->usage_index == 0) slot->pos = e;
        else if (e->usage == 1 && e->usage_index == 0) slot->weight = e;
        else if (e->usage == 2 && e->usage_index == 0) slot->indices = e;
        else if (e->usage == 5 && e->usage_index < 4) slot->tex[e->usage_index] = e;
        else if (e->usage == 10 && e->usage_index == 0) slot->col = e;
        else if (e->usage == 3 && e->usage_index == 0) slot->norm = e;
    }
    slot->valid = (handle != 0);
    return *slot;
}

const VitaGlD3D9Device::CachedVs& VitaGlD3D9Device::get_cached_vs(
    std::uint32_t handle, const Object* vs) {
    static CachedVs fallback;
    if (!vs) {
        fallback = CachedVs{true, true, 0, 0, false, false};
        return fallback;
    }
    if (handle >= vs_cache_.size()) vs_cache_.resize(handle + 32);
    auto& slot = vs_cache_[handle];
    if (slot.valid) return slot;
    slot = CachedVs{};
    for (uint32_t b : vs->program.blobs) {
        if (b == 0 || b == 2 || b == 4 || b == 6 || b == 8 || (b >= 59 && b <= 72)) {
            slot.is_rigid = true;
            if (b == 4) slot.skin_bones = 1;
            else if (b == 6) slot.skin_bones = 2;
            else if (b == 8) slot.skin_bones = 3;
        }
        if (b == 23 || b == 24 || b == 25) {
            slot.tex_mat_mode = static_cast<std::uint8_t>(b);
        } else if (b >= 26 && b <= 33) {
            slot.has_texgen = true;
        } else if (b == 53 || b == 54) {
            slot.has_envmap = true;
        }
    }
    slot.valid = true;
    return slot;
}

const VitaGlD3D9Device::CachedPs& VitaGlD3D9Device::get_cached_ps(
    std::uint32_t handle, const Object* ps) {
    static CachedPs default_ps{true, true, false, false, false, false, false};
    if (!ps || ps->kind != KPs) return default_ps;
    if (handle >= ps_cache_.size()) ps_cache_.resize(handle + 32);
    auto& slot = ps_cache_[handle];
    if (slot.valid) return slot;
    slot = CachedPs{};
    if (ps->program.version == 0xffff0104u) {
        bool has_36 = false, has_37 = false, has_18 = false, has_19 = false, has_12 = false, has_stage1 = false;
        bool has_24 = false, has_34 = false, has_2 = false, has_3 = false, has_15 = false, has_16 = false;
        bool has_mul_stage1 = false;
        for (uint32_t b : ps->program.blobs) {
            if (b == 36) has_36 = true;
            else if (b == 37) has_37 = true;
            else if (b == 18) has_18 = true;
            else if (b == 19) has_19 = true;
            else if (b == 12) has_12 = true;
            else if (b == 15) has_15 = true;
            else if (b == 16) has_16 = true;
            else if (b == 24) has_24 = true;
            else if (b == 34) has_34 = true;
            else if (b == 2) has_2 = true;
            else if (b == 3) has_3 = true;
            else if (b == 21 || b == 22) { has_stage1 = true; has_mul_stage1 = true; }
            else if (b >= 20 && b <= 31) has_stage1 = true;
        }
        slot.uses_tex0_alpha = !(has_36 && !has_37);
        slot.is_decal_rgb = has_18 && !has_stage1;
        slot.ignore_diffuse_rgb = has_19 && !has_stage1;
        slot.ignore_tex0_rgb = has_12 && !has_stage1;
        // Car reflection shader: ps=[3, 15, 37, 34, 36, 24, 43]
        slot.is_reflection = (has_3 && (has_24 || has_34 || has_stage1));
        slot.uses_stage1_mul = has_mul_stage1;
    } else if (ps->program.version == 0xffff0101u) {
        // ps_1_1 shaders (particles, smoke, decals)
        slot.uses_tex0_alpha = true;
        slot.ignore_diffuse_rgb = false;
        slot.is_decal_rgb = false;
        slot.ignore_tex0_rgb = false;
        slot.is_reflection = false;
        slot.uses_stage1_mul = false;
    }
    slot.valid = true;
    return slot;
}

const std::uint8_t* VitaGlD3D9Device::get_vb_rgba_ptr(
    std::uint32_t buf_handle, const Object* vb,
    std::uint32_t stream_offset, std::uint32_t stride,
    std::uint32_t col_offset, std::int32_t base,
    std::uint32_t max_vert_needed) {
    if (!vb || vb->bytes.empty()) return nullptr;
    const std::uint8_t* raw_ptr = vb->bytes.data() + stream_offset + col_offset + std::ptrdiff_t(base) * stride;
    if (!buf_handle || stride < 4 || (stream_offset % stride) != 0 || base < 0) return raw_ptr;

    if (buf_handle >= vb_col_cache_.size()) vb_col_cache_.resize(buf_handle + 64);
    auto& entry = vb_col_cache_[buf_handle];
    if (entry.rgba.empty()) {
        const std::size_t nverts = (vb->bytes.size() > col_offset) ? ((vb->bytes.size() - col_offset + stride - 1u) / stride) : 0u;
        const std::size_t bytes_needed = nverts * 4u;
        if (bytes_needed > 0 && vb_col_cache_bytes_ + bytes_needed <= 3u * 1024u * 1024u) {
            entry.col_offset = col_offset;
            entry.stride = stride;
            entry.rgba.resize(bytes_needed);
            const std::uint8_t* src = vb->bytes.data() + col_offset;
            std::uint8_t* dst = entry.rgba.data();
            for (std::size_t i = 0; i < nverts; ++i) {
                const std::size_t so = i * stride;
                if (so + 4u <= vb->bytes.size() - col_offset) {
                    dst[i * 4 + 0] = src[so + 2];
                    dst[i * 4 + 1] = src[so + 1];
                    dst[i * 4 + 2] = src[so + 0];
                    dst[i * 4 + 3] = src[so + 3];
                }
            }
            vb_col_cache_bytes_ += bytes_needed;
        }
    }
    if (!entry.rgba.empty() && entry.col_offset == col_offset && entry.stride == stride) {
        const std::size_t vert_base = (stream_offset / stride) + std::size_t(base);
        if ((vert_base + std::size_t(max_vert_needed)) * 4u <= entry.rgba.size()) {
            return entry.rgba.data() + vert_base * 4u;
        }
    }
    return raw_ptr;
}

void VitaGlD3D9Device::apply_state() {
    if (render_states_equal(batch_key_, current_applied_state_) && !state_dirty_) return;
    const std::uint32_t t0 = outrun::vita_profile::vita_time_us();

    current_applied_state_ = batch_key_;
    state_dirty_ = false;

    int vx, vy, vw, vh;
    if (batch_key_.rhw) {
        const float sx = float(back_w_) / float(logical_width_ ? logical_width_ : width_);
        const float sy = float(back_h_) / float(logical_height_ ? logical_height_ : height_);
        vx = int(std::lround(soft_viewport_.x * sx));
        vy = int(std::lround(soft_viewport_.y * sy));
        vw = int(std::lround(soft_viewport_.w * sx));
        vh = int(std::lround(soft_viewport_.h * sy));
    } else {
        // En 3D forzar siempre glViewport(0, 0, 960, 544) a pantalla completa
        vx = 0;
        vy = 0;
        vw = int(back_w_);
        vh = int(back_h_);
    }

    if (vx != last_vp_x_ || vy != last_vp_y_ || vw != last_vp_w_ || vh != last_vp_h_ ||
        soft_viewport_.min_z != last_min_z_ || soft_viewport_.max_z != last_max_z_) {
        // OpenGL viewport origin is lower-left
        glViewport(vx, int(back_h_) - (vy + vh), vw, vh);
        glDepthRange(soft_viewport_.min_z, soft_viewport_.max_z);
        last_vp_x_ = vx;
        last_vp_y_ = vy;
        last_vp_w_ = vw;
        last_vp_h_ = vh;
        last_min_z_ = soft_viewport_.min_z;
        last_max_z_ = soft_viewport_.max_z;
    }

    // Culling
    const bool want_cull = !(batch_key_.rhw || batch_key_.cull == CULL_NONE);
    if (!want_cull) {
        if (s_shadow_gl.cull_enabled != 0) {
            glDisable(GL_CULL_FACE);
            s_shadow_gl.cull_enabled = 0;
        }
    } else {
        if (s_shadow_gl.cull_enabled != 1) {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CW);
            s_shadow_gl.cull_enabled = 1;
        }
        const GLenum face = (batch_key_.cull == CULL_CCW ? GL_BACK : GL_FRONT);
        if (s_shadow_gl.cull_face != face) {
            glCullFace(face);
            s_shadow_gl.cull_face = face;
        }
    }

    // Depth Test & Write
    bool want_depth_test = false;
    GLenum want_depth_func = GL_LESS;
    GLboolean want_depth_mask = GL_FALSE;

    if (batch_key_.rhw) {
        if (batch_key_.z_enable && batch_key_.z_func != 8u) {
            want_depth_test = true;
            static const GLenum z_funcs[] = {
                GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
            };
            const unsigned zf = std::clamp(batch_key_.z_func, 1u, 8u) - 1u;
            want_depth_func = z_funcs[zf];
        }
        want_depth_mask = GL_FALSE;
    } else {
        want_depth_test = (batch_key_.z_enable != 0);
        want_depth_mask = (batch_key_.z_write && !batch_key_.blend_enable) ? GL_TRUE : GL_FALSE;
        static const GLenum z_funcs[] = {
            GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
            GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
        };
        const unsigned zf = std::clamp(batch_key_.z_func, 1u, 8u) - 1u;
        want_depth_func = z_funcs[zf];
    }

    if (s_shadow_gl.depth_test != (want_depth_test ? 1 : 0)) {
        if (want_depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        s_shadow_gl.depth_test = want_depth_test ? 1 : 0;
    }
    if (want_depth_test && s_shadow_gl.depth_func != want_depth_func) {
        glDepthFunc(want_depth_func);
        s_shadow_gl.depth_func = want_depth_func;
    }
    if (s_shadow_gl.depth_mask != want_depth_mask) {
        glDepthMask(want_depth_mask);
        s_shadow_gl.depth_mask = want_depth_mask;
    }

    // Alpha Blending
    if (batch_key_.blend_enable) {
        if (s_shadow_gl.blend_enabled != 1) {
            glEnable(GL_BLEND);
            s_shadow_gl.blend_enabled = 1;
        }
        GLenum sfactor = to_gl_blend(batch_key_.src_blend, GL_SRC_ALPHA);
        GLenum dfactor = to_gl_blend(batch_key_.dst_blend, GL_ONE_MINUS_SRC_ALPHA);
        // Smoke / particles use D3DBLEND_ONE (2) + D3DBLEND_INVSRCALPHA (6) with unpremultiplied white RGB textures
        if (batch_key_.src_blend == 2u && batch_key_.dst_blend == 6u) {
            sfactor = GL_SRC_ALPHA;
        }
        if (s_shadow_gl.blend_src != sfactor || s_shadow_gl.blend_dst != dfactor) {
            glBlendFunc(sfactor, dfactor);
            s_shadow_gl.blend_src = sfactor;
            s_shadow_gl.blend_dst = dfactor;
        }
    } else {
        if (s_shadow_gl.blend_enabled != 0) {
            glDisable(GL_BLEND);
            s_shadow_gl.blend_enabled = 0;
        }
    }

    // Alpha Test
    bool enable_at = (batch_key_.alpha_test != 0);
    GLenum gl_alpha_func = GL_GREATER;
    switch (batch_key_.alpha_func) {
        case 1: gl_alpha_func = GL_NEVER; break;
        case 2: gl_alpha_func = GL_LESS; break;
        case 3: gl_alpha_func = GL_EQUAL; break;
        case 4: gl_alpha_func = GL_LEQUAL; break;
        case 5: gl_alpha_func = GL_GREATER; break;
        case 6: gl_alpha_func = GL_NOTEQUAL; break;
        case 7: gl_alpha_func = GL_GEQUAL; break;
        case 8: gl_alpha_func = GL_ALWAYS; break;
        default: gl_alpha_func = GL_GEQUAL; break;
    }

    // If alpha_ref is 0 and func is GEQUAL or ALWAYS, in D3D9 (alpha >= 0) all fragments pass.
    if (batch_key_.alpha_ref == 0 && (gl_alpha_func == GL_GEQUAL || gl_alpha_func == GL_ALWAYS)) {
        enable_at = false;
    }
    // Also, opaque geometry with ref == 0 should never discard anything
    if (!batch_key_.blend_enable && batch_key_.alpha_ref == 0) {
        enable_at = false;
    }

    if (enable_at) {
        if (s_shadow_gl.alpha_test != 1) {
            glEnable(GL_ALPHA_TEST);
            s_shadow_gl.alpha_test = 1;
        }
        const GLclampf ref_val = (float)batch_key_.alpha_ref / 255.0f;
        if (s_shadow_gl.alpha_func != gl_alpha_func || s_shadow_gl.alpha_ref != ref_val) {
            glAlphaFunc(gl_alpha_func, ref_val);
            s_shadow_gl.alpha_func = gl_alpha_func;
            s_shadow_gl.alpha_ref = ref_val;
        }
    } else {
        if (s_shadow_gl.alpha_test != 0) {
            glDisable(GL_ALPHA_TEST);
            s_shadow_gl.alpha_test = 0;
        }
    }

    // Color Mask
    const auto mask = batch_key_.color_mask;
    const GLboolean mr = (mask & 1) ? GL_TRUE : GL_FALSE;
    const GLboolean mg = (mask & 2) ? GL_TRUE : GL_FALSE;
    const GLboolean mb = (mask & 4) ? GL_TRUE : GL_FALSE;
    const GLboolean ma = (mask & 8) ? GL_TRUE : GL_FALSE;
    if (s_shadow_gl.color_mask_r != mr || s_shadow_gl.color_mask_g != mg ||
        s_shadow_gl.color_mask_b != mb || s_shadow_gl.color_mask_a != ma) {
        glColorMask(mr, mg, mb, ma);
        s_shadow_gl.color_mask_r = mr;
        s_shadow_gl.color_mask_g = mg;
        s_shadow_gl.color_mask_b = mb;
        s_shadow_gl.color_mask_a = ma;
    }

    applied_2d_vx_ = -1;
    applied_2d_blend_key_ = 0xFFFFFFFFu;
    applied_2d_tex_ = 0xFFFFFFFFu;

    const std::uint32_t dt = outrun::vita_profile::vita_time_us() - t0;
    outrun::vita_profile::g_sub_state_setup_us += dt;
    if (outrun::vita_profile::g_capture_render_frame) snapshot_t_state_setup_us_ += dt;
}

void VitaGlD3D9Device::flush() {
    if (batch_.empty()) return;
    s_last_3d.valid = false;

    const std::uint32_t t_setup_start = outrun::vita_profile::vita_time_us();

    if (active_matrix_ != MATRIX_IDENTITY) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        active_matrix_ = MATRIX_IDENTITY;
    }
    if (!tex_matrix_identity_) {
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        tex_matrix_identity_ = true;
    }

    const unsigned tex_id = texture(batch_key_.tex);
    const bool is_decal_flush = (batch_key_.tex && batch_key_.tex < textures_gl_.size() &&
                                 textures_gl_[batch_key_.tex].is_cropped_decal);
    if (is_decal_flush) {
        batch_key_.blend_enable = 1;
        batch_key_.src_blend = 5;
        batch_key_.dst_blend = 6;
        batch_key_.alpha_test = 1;
        batch_key_.alpha_ref = 128;
        batch_key_.alpha_func = 7;
        state_dirty_ = true;
        tex_env_mode_[0] = TEX_ENV_UNKNOWN;
        s_shadow_gl.alpha_test = -1;
        s_shadow_gl.blend_enabled = -1;
        s_shadow_gl.alpha_func = 0;
        s_shadow_gl.alpha_ref = -1.0f;
        s_shadow_gl.blend_src = 0;
        s_shadow_gl.blend_dst = 0;
    }
    if (tex2d_enabled_[1]) {
        bind_texture_unit(GL_TEXTURE1, 1, 0, 0, TEX_ENV_MODULATE);
    }
    bind_texture_unit(GL_TEXTURE0, 0, tex_id, batch_key_.tex, is_decal_flush ? TEX_ENV_REPLACE_RGBA : TEX_ENV_MODULATE);
    glActiveTexture(GL_TEXTURE0);
    current_active_unit_ = GL_TEXTURE0;
    apply_state();

    set_client_states(true, tex_id != 0, true);
    color_const_white_ = false;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(4, GL_FLOAT, sizeof(Vertex), &batch_[0].pos[0]);
    glColorPointer(4, GL_FLOAT, sizeof(Vertex), &batch_[0].d[0].x);
    if (tex_id) {
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &batch_[0].t[0].x);
    }

    const std::uint32_t dt_setup = outrun::vita_profile::vita_time_us() - t_setup_start;
    outrun::vita_profile::g_sub_state_setup_us += dt_setup;
    if (outrun::vita_profile::g_capture_render_frame) snapshot_t_state_setup_us_ += dt_setup;

    const std::uint32_t t_draw_start = outrun::vita_profile::vita_time_us();
    if (is_decal_flush) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-3.0f, -6.0f);
    }
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch_.size()));
    if (is_decal_flush) {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    const std::uint32_t dt_draw = outrun::vita_profile::vita_time_us() - t_draw_start;

    outrun::vita_profile::g_sub_gl_draw_elements_us += dt_draw;
    if (outrun::vita_profile::g_capture_render_frame) snapshot_t_gl_draw_elements_us_ += dt_draw;

    ++outrun::vita_profile::g_flushes;

    batch_.clear();
}

std::uint32_t VitaGlD3D9Device::draw_indexed_primitive(std::uint32_t type, std::int32_t base,
    std::uint32_t min, std::uint32_t vertices, std::uint32_t start, std::uint32_t count) {
    if (scale_zero_ || count_only || colour_target_ != BackBuffer) return 0;
    if (async_recording_) {
        has_3d_draws_ = true;
        ++submitted_draws;
        GlCmd c{};
        c.kind = GlCmdKind::DrawIndexed;
        c.u0 = type; c.i0 = base; c.u1 = min; c.u2 = vertices; c.u3 = start; c.u4 = count;
        outrun::vita_profile::get_active_model(c.res_id, c.obj_id);
        async_slots_[write_slot_].cmds.push_back(c);
        return 1;
    }
    std::uint32_t res = 0;
    outrun::vita_render::run_sync([&]() {
        snapshot_live_state(replay_);
        replaying_async_ = true;
        res = draw_indexed_primitive_gl(replay_, type, base, min, vertices, start, count);
        replaying_async_ = false;
    });
    return res;
}

std::uint32_t VitaGlD3D9Device::draw_indexed_primitive_gl(const ReplayState& st, std::uint32_t type, std::int32_t base,
    std::uint32_t min, std::uint32_t vertices, std::uint32_t start, std::uint32_t count) {
    const auto& render = st.render;
    const auto& textures = st.textures;
    const auto& streams = st.streams;
    const auto& stage = st.stage;
    const auto& vs_constants = st.vs_constants;
    const auto indices = st.indices;
    const auto declaration = st.declaration;
    const auto vertex_shader = st.vertex_shader;
    const auto pixel_shader = st.pixel_shader;
    const auto fvf_ = st.fvf;
    auto transform_wvp = [&st]() { return st.transform_wvp(); };
    struct DipScope {
        std::uint32_t t0;
        DipScope() : t0(outrun::vita_profile::vita_time_us()) {}
        ~DipScope() { outrun::vita_profile::g_sub_dip_total_us += (outrun::vita_profile::vita_time_us() - t0); }
    } dip_scope;

    has_3d_draws_ = true;
    ++outrun::vita_profile::g_draw_calls;
    if (outrun::vita_profile::g_capture_render_frame) {
        ++snapshot_draw_count_;
        ++snapshot_draw_3d_count_;
    }

    BatchKey key{
        textures[0],
        false,
        render[RS_ALPHABLENDENABLE],
        render[RS_SRCBLEND],
        render[RS_DESTBLEND],
        render[RS_ALPHATESTENABLE],
        render[RS_ALPHAFUNC],
        render[RS_ALPHAREF] & 0xffu,
        render[RS_ZENABLE],
        render[RS_ZWRITEENABLE],
        render[RS_ZFUNC],
        render[RS_CULLMODE],
        render[RS_COLORWRITEENABLE] & 0xfu,
        0, 0, int(back_w_), int(back_h_)
    };

    auto* ib = object(indices);
    if (!ib) return 1;

    auto* vs = object(vertex_shader);
    auto* decl = object(declaration);

    int active_vs_id = -1;
    if (vs && vs->kind == KVs) {
        active_vs_id = vs->program.blobs.empty() ? -2 : int(vs->program.blobs[0]);
    } else if (vs) {
        active_vs_id = -3;
    }

    bool use_hw_path = false;
    const char* fail_reason = nullptr;
    static char s_rigid_fail[128];

    const platform::PcVertexElement* pos_elem = nullptr;
    const platform::PcVertexElement* tex_elem = nullptr;
    const platform::PcVertexElement* col_elem = nullptr;

    platform::PcVertexDeclaration fvf_decl{};
    const platform::PcVertexElement* elements = decl ? decl->elements.data() : nullptr;
    std::uint32_t decl_handle = decl ? declaration : 0u;
    if (!elements && fvf_) {
        if (platform::d3dx_declarator_from_fvf(fvf_, fvf_decl)) {
            elements = fvf_decl.data();
        }
    }

    CachedVs cvs{};
    CachedDecl cdecl{};
    if (vs && vs->kind != KVs) {
        fail_reason = "vs kind is not KVs";
    } else if (vs && vs->program.blobs.empty()) {
        fail_reason = "vs blobs empty";
    } else if (!elements) {
        fail_reason = "decl and fvf are null";
    } else if (type != 4 && type != 5) {
        fail_reason = "type not tri_list or tri_strip";
    } else {
        cvs = get_cached_vs(vertex_shader, vs);
        if (!cvs.is_rigid) {
            std::string bstr;
            for (size_t i = 0; i < vs->program.blobs.size(); ++i) {
                if (i > 0) bstr += ",";
                bstr += std::to_string(vs->program.blobs[i]);
            }
            std::snprintf(s_rigid_fail, sizeof(s_rigid_fail), "not rigid (blobs=[%s])", bstr.c_str());
            fail_reason = s_rigid_fail;
        } else {
            cdecl = get_cached_decl(decl_handle, elements);
            pos_elem = cdecl.pos;
            col_elem = cdecl.col;

            if (!pos_elem) {
                fail_reason = "no position element in decl";
            } else if (pos_elem->type != 2 && pos_elem->type != 3) {
                fail_reason = "pos_elem type is not FLOAT3/4";
            } else if (pos_elem->stream >= streams.size()) {
                fail_reason = "pos_elem stream out of bounds";
            } else {
                auto* vb = object(streams[pos_elem->stream].buffer);
                if (!vb) {
                    fail_reason = "pos vb is null";
                } else {
                    use_hw_path = true;
                }
            }
        }
    }

    if (!use_hw_path) {
        ++outrun::vita_profile::g_sw_draws;
        if (outrun::vita_profile::g_capture_render_frame) ++snapshot_sw_draws_;
        if (s_fail_print_count++ < 1) {
            std::printf("[HW_FAIL] active_vs: %d, reason: %s\n", active_vs_id, fail_reason ? fail_reason : "unknown");
            std::fflush(stdout);
        }
        if (fail_reason) {
            std::snprintf(outrun::vita_profile::g_last_hw_fail, sizeof(outrun::vita_profile::g_last_hw_fail),
                          "vs=%d: %s", active_vs_id, fail_reason);
        }
    } else {
        ++outrun::vita_profile::g_hw_draws;
        if (outrun::vita_profile::g_capture_render_frame) ++snapshot_hw_draws_;
    }

    if (use_hw_path) {
        const std::size_t num_indices = (type == 5) ? (std::size_t(count) + 2u) : (std::size_t(count) * 3u);
        if (!num_indices || (std::size_t(start) + num_indices) * 2u > ib->bytes.size()) return 1;

        if (!batch_.empty()) {
            flush();
        }
        const std::uint32_t tex0_handle = textures[0];
        const std::uint32_t tex1_handle = textures[1];
        const unsigned tex0_id = texture(tex0_handle);
        auto* ps = object(pixel_shader);
        const CachedPs& cps = get_cached_ps(pixel_shader, ps);
        const bool is_opaque_spec = (tex0_handle && tex0_handle < textures_gl_.size() && textures_gl_[tex0_handle].is_opaque_specular);
        if (is_opaque_spec && !key.blend_enable && key.alpha_ref >= 64u) {
            key.alpha_test = 0;
        }

        const uint32_t primary_handle = tex0_handle ? tex0_handle : tex1_handle;
        const bool is_cropped_decal = (primary_handle && primary_handle < textures_gl_.size() &&
                                       textures_gl_[primary_handle].is_cropped_decal);
        const bool has_tex_alpha = (primary_handle && primary_handle < textures_gl_.size() &&
                                    textures_gl_[primary_handle].has_alpha &&
                                    !textures_gl_[primary_handle].is_opaque_specular);
        if (is_cropped_decal) {
            log_decal_debug("[DRAW_HW] h0=%u h1=%u id0=%u ab=%u(%u/%u) at=%u(ref=%u,fn=%u) ma=%.3f ps=%u vs=%d\n",
                            tex0_handle, tex1_handle, tex0_id,
                            key.blend_enable, key.src_blend, key.dst_blend,
                            key.alpha_test, key.alpha_ref, key.alpha_func,
                            vs ? vs_constants[18][3] : -1.0f,
                            pixel_shader, active_vs_id);
            key.blend_enable = 1;
            key.src_blend = 5; // D3DBLEND_SRCALPHA
            key.dst_blend = 6; // D3DBLEND_INVSRCALPHA
            key.alpha_test = 1;
            key.alpha_ref = 128;
            key.alpha_func = 7; // D3DCMP_GREATEREQUAL
            s_last_3d.valid = false;
            state_dirty_ = true;
            tex_env_mode_[0] = TEX_ENV_UNKNOWN;
            s_shadow_gl.alpha_test = -1;
            s_shadow_gl.blend_enabled = -1;
            s_shadow_gl.alpha_func = 0;
            s_shadow_gl.alpha_ref = -1.0f;
            s_shadow_gl.blend_src = 0;
            s_shadow_gl.blend_dst = 0;
        } else if (has_tex_alpha) {
            if (key.blend_enable) {
                key.src_blend = 5; // D3DBLEND_SRCALPHA
                key.dst_blend = 6; // D3DBLEND_INVSRCALPHA
                key.alpha_test = 1;
                key.alpha_ref = 8;
                key.alpha_func = 7; // D3DCMP_GREATEREQUAL
            } else {
                key.alpha_test = 1;
                key.alpha_ref = 64;
                key.alpha_func = 7; // D3DCMP_GREATEREQUAL
            }
        }
        batch_key_ = key;
        batch_rhw_ = false;

        const auto& pos_st = streams[pos_elem->stream];
        const bool is_skinned = (cvs.skin_bones > 0) || (cdecl.weight != nullptr);

        // State Cache check
        bool cache_match = !is_skinned && s_last_3d.valid &&
            !wvp_dirty_ &&
            active_matrix_ == MATRIX_HW_WVP &&
            s_last_3d.type == type &&
            s_last_3d.base == base &&
            s_last_3d.decl_handle == decl_handle &&
            s_last_3d.vs_id == vertex_shader &&
            s_last_3d.ps_id == pixel_shader &&
            s_last_3d.tex0_handle == tex0_handle &&
            s_last_3d.tex1_handle == tex1_handle &&
            s_last_3d.key == key &&
            s_last_3d.tex_mat_mode == cvs.tex_mat_mode &&
            s_last_3d.pos_buffer == pos_st.buffer &&
            s_last_3d.pos_offset == pos_st.offset &&
            s_last_3d.pos_stride == pos_st.stride &&
            s_last_3d.stage0_uv == stage[0][11] &&
            s_last_3d.stage1_uv == stage[1][11];

        if (cache_match) {
            if (col_elem && col_elem->stream < streams.size()) {
                if (s_last_3d.col_buffer != streams[col_elem->stream].buffer ||
                    s_last_3d.col_offset != streams[col_elem->stream].offset ||
                    s_last_3d.col_stride != streams[col_elem->stream].stride) {
                    cache_match = false;
                }
            } else {
                if (s_last_3d.col_buffer != 0xFFFFFFFFu) {
                    cache_match = false;
                } else if (vs && (s_last_3d.const_col[0] != vs_constants[18][0] ||
                                  s_last_3d.const_col[1] != vs_constants[18][1] ||
                                  s_last_3d.const_col[2] != vs_constants[18][2] ||
                                  s_last_3d.const_col[3] != vs_constants[18][3])) {
                    cache_match = false;
                }
            }
        }

        if (cache_match) {
            const GLenum mode = (type == 5) ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
            const auto& ib_cb = (indices < cdram_buffers_.size()) ? cdram_buffers_[indices] : CdramBuffer{};

            const std::uint32_t t_draw_start = outrun::vita_profile::vita_time_us();
            if (is_cropped_decal) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-3.0f, -6.0f);
            }
            if (ib_cb.vbo) {
                if (last_ibo_ != ib_cb.vbo) {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_cb.vbo);
                    last_ibo_ = ib_cb.vbo;
                }
                glDrawElements(mode, GLsizei(num_indices), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(std::size_t(start) * 2u));
            } else {
                if (last_ibo_ != 0) {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                    last_ibo_ = 0;
                }
                const void* idx_ptr = reinterpret_cast<const uint8_t*>(ib->bytes.data()) + std::size_t(start) * 2u;
                glDrawElements(mode, GLsizei(num_indices), GL_UNSIGNED_SHORT, idx_ptr);
            }
            if (is_cropped_decal) {
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            const std::uint32_t dt_draw = outrun::vita_profile::vita_time_us() - t_draw_start;

            outrun::vita_profile::g_sub_gl_draw_elements_us += dt_draw;
            if (outrun::vita_profile::g_capture_render_frame) snapshot_t_gl_draw_elements_us_ += dt_draw;

            triangles_drawn += count;
            outrun::vita_profile::g_triangles += count;
            outrun::vita_profile::accumulate_model_polygons(type, count, is_skinned);
            return 1;
        }

        const std::uint32_t t_setup_start = outrun::vita_profile::vita_time_us();

        const bool allow_stage1 = !is_cropped_decal && tex0_id && tex1_handle && !cvs.has_envmap && cps.uses_stage1_mul;
        const unsigned tex1_id = allow_stage1 ? texture(tex1_handle) : 0;
        const unsigned effective_tex0_id = tex0_id ? tex0_id : tex1_id;

        TexEnvMode desired_env0 = TEX_ENV_MODULATE;

        const float ma_val = vs ? vs_constants[18][3] : 0.0f;
        const bool is_glass = batch_key_.blend_enable && (ma_val > 0.0f && ma_val < 1.0f) && !has_tex_alpha && !is_cropped_decal;

        const bool same_pipeline = !is_skinned && !is_cropped_decal && s_last_3d.valid &&
            s_last_3d.decl_handle == decl_handle &&
            s_last_3d.vs_id == vertex_shader &&
            s_last_3d.ps_id == pixel_shader &&
            s_last_3d.tex0_handle == tex0_handle &&
            s_last_3d.tex1_handle == tex1_handle &&
            s_last_3d.key == key &&
            s_last_3d.tex_mat_mode == cvs.tex_mat_mode &&
            s_last_3d.is_glass == is_glass;

        if (!same_pipeline) {
            if (is_cropped_decal) {
                desired_env0 = TEX_ENV_REPLACE_RGBA;
            } else if (is_glass) {
                desired_env0 = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
            } else if (is_opaque_spec && !batch_key_.blend_enable && batch_key_.alpha_ref >= 64u) {
                desired_env0 = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
            } else if (has_tex_alpha || batch_key_.blend_enable || (batch_key_.alpha_test && batch_key_.alpha_ref < 64u)) {
                desired_env0 = TEX_ENV_MODULATE;
            } else if (!ps || ps->kind != KPs) {
                const std::uint32_t aop = stage[0][4];
                if (aop == 1u ||
                    (aop == 2u && (stage[0][5] & 0xfu) != 2u) ||
                    (aop == 3u && (stage[0][6] & 0xfu) != 2u)) {
                    desired_env0 = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
                }
            } else {
                if (cps.ignore_tex0_rgb) {
                    desired_env0 = TEX_ENV_DIFFUSE_ONLY;
                } else if (cps.is_decal_rgb) {
                    desired_env0 = TEX_ENV_DECAL_DIFFUSE_ALPHA;
                } else if (cps.ignore_diffuse_rgb) {
                    desired_env0 = cps.uses_tex0_alpha ? TEX_ENV_REPLACE_RGBA : TEX_ENV_REPLACE_RGB_DIFFUSE_ALPHA;
                } else {
                    desired_env0 = cps.uses_tex0_alpha ? TEX_ENV_MODULATE : TEX_ENV_MODULATE_DIFFUSE_ALPHA;
                }
            }

            if (allow_stage1) {
                bind_texture_unit(GL_TEXTURE0, 0, tex0_id, tex0_handle, desired_env0);
                bind_texture_unit(GL_TEXTURE1, 1, tex1_id, tex1_handle, TEX_ENV_MULTITEX_MODULATE);
            } else {
                const unsigned id0 = tex0_id ? tex0_id : tex1_id;
                const uint32_t h0 = tex0_id ? tex0_handle : tex1_handle;
                bind_texture_unit(GL_TEXTURE0, 0, id0, h0, desired_env0);
                if (tex2d_enabled_[1]) {
                    bind_texture_unit(GL_TEXTURE1, 1, 0, 0, TEX_ENV_MODULATE);
                }
            }
            if (current_active_unit_ != GL_TEXTURE0) {
                glActiveTexture(GL_TEXTURE0);
                current_active_unit_ = GL_TEXTURE0;
            }

            apply_state();

            if (vs && cvs.tex_mat_mode != 0) {
                float tm[16]{
                    vs_constants[68][0], vs_constants[69][0], 0.0f, 0.0f,
                    vs_constants[68][1], vs_constants[69][1], 0.0f, 0.0f,
                    vs_constants[68][2], vs_constants[69][2], 1.0f, 0.0f,
                    vs_constants[68][3], vs_constants[69][3], 0.0f, 1.0f
                };
                if (cvs.tex_mat_mode == 24 || cvs.tex_mat_mode == 25) {
                    tm[2] = vs_constants[70][0]; tm[6] = vs_constants[70][1];
                    tm[10] = vs_constants[70][2]; tm[14] = vs_constants[70][3];
                }
                if (cvs.tex_mat_mode == 25) {
                    tm[3] = vs_constants[71][0]; tm[7] = vs_constants[71][1];
                    tm[11] = vs_constants[71][2]; tm[15] = vs_constants[71][3];
                }
                glMatrixMode(GL_TEXTURE);
                glLoadMatrixf(tm);
                glMatrixMode(GL_MODELVIEW);
                tex_matrix_identity_ = false;
            } else if (!tex_matrix_identity_) {
                glMatrixMode(GL_TEXTURE);
                glLoadIdentity();
                glMatrixMode(GL_MODELVIEW);
                tex_matrix_identity_ = true;
            }
        }

        if (vs) {
            if (wvp_dirty_ || active_matrix_ != MATRIX_HW_WVP) {
                float gl_m[16];
                // Transpose row-major vs_constants[64..67] to column-major OpenGL,
                // mapping D3D depth [0, w] to OpenGL depth [-w, w]: z_gl = 2*z_d3d - w_d3d
                gl_m[0]  = vs_constants[64][0];
                gl_m[1]  = vs_constants[65][0];
                gl_m[2]  = 2.0f * vs_constants[66][0] - vs_constants[67][0];
                gl_m[3]  = vs_constants[67][0];

                gl_m[4]  = vs_constants[64][1];
                gl_m[5]  = vs_constants[65][1];
                gl_m[6]  = 2.0f * vs_constants[66][1] - vs_constants[67][1];
                gl_m[7]  = vs_constants[67][1];

                gl_m[8]  = vs_constants[64][2];
                gl_m[9]  = vs_constants[65][2];
                gl_m[10] = 2.0f * vs_constants[66][2] - vs_constants[67][2];
                gl_m[11] = vs_constants[67][2];

                gl_m[12] = vs_constants[64][3];
                gl_m[13] = vs_constants[65][3];
                gl_m[14] = 2.0f * vs_constants[66][3] - vs_constants[67][3];
                gl_m[15] = vs_constants[67][3];

                glMatrixMode(GL_PROJECTION);
                glLoadMatrixf(gl_m);
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();

                active_matrix_ = MATRIX_HW_WVP;
                wvp_dirty_ = false;
            }
        } else {
            if (wvp_dirty_ || active_matrix_ != MATRIX_HW_WVP) {
                const auto wvp = transform_wvp();
                float gl_m[16];
                gl_m[0]  = wvp[0];
                gl_m[1]  = wvp[1];
                gl_m[2]  = 2.0f * wvp[2] - wvp[3];
                gl_m[3]  = wvp[3];

                gl_m[4]  = wvp[4];
                gl_m[5]  = wvp[5];
                gl_m[6]  = 2.0f * wvp[6] - wvp[7];
                gl_m[7]  = wvp[7];

                gl_m[8]  = wvp[8];
                gl_m[9]  = wvp[9];
                gl_m[10] = 2.0f * wvp[10] - wvp[11];
                gl_m[11] = wvp[11];

                gl_m[12] = wvp[12];
                gl_m[13] = wvp[13];
                gl_m[14] = 2.0f * wvp[14] - wvp[15];
                gl_m[15] = wvp[15];

                glMatrixMode(GL_PROJECTION);
                glLoadMatrixf(gl_m);
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();

                active_matrix_ = MATRIX_HW_WVP;
                wvp_dirty_ = false;
            }
            if (!same_pipeline && !tex_matrix_identity_) {
                glMatrixMode(GL_TEXTURE);
                glLoadIdentity();
                glMatrixMode(GL_MODELVIEW);
                tex_matrix_identity_ = true;
            }
        }

        const unsigned uv_idx0 = stage[0][11] & 7u;
        const auto* tex_elem0 = (uv_idx0 < 4 && cdecl.tex[uv_idx0]) ? cdecl.tex[uv_idx0] : cdecl.tex[0];
        if (!tex_elem0 && cdecl.tex[1]) {
            tex_elem0 = cdecl.tex[1];
        }

        const unsigned uv_idx1 = stage[1][11] & 7u;
        const auto* tex_elem1 = (uv_idx1 < 4 && cdecl.tex[uv_idx1]) ? cdecl.tex[uv_idx1] : cdecl.tex[1];
        if (!tex_elem1) {
            tex_elem1 = tex_elem0;
        }

        auto* pos_b = object(pos_st.buffer);
        const auto& pos_cb = (pos_st.buffer < cdram_buffers_.size()) ? cdram_buffers_[pos_st.buffer] : CdramBuffer{};
        const std::size_t pos_byte_offset = pos_st.offset + pos_elem->offset + std::size_t(base) * pos_st.stride;
        const uint8_t* pos_data = pos_b ? pos_b->bytes.data() : nullptr;
        const uint8_t* pos_ptr = pos_data ? (pos_data + pos_byte_offset) : nullptr;

        bool tex0_enabled = false;
        int tex0_coords = 2;
        const uint8_t* tex0_ptr = nullptr;
        std::uint32_t tex0_stride = 0;
        std::uint32_t tex0_buf_handle = 0;
        std::size_t tex0_byte_offset = 0;
        if (tex_elem0 && effective_tex0_id && tex_elem0->stream < streams.size()) {
            const auto& tex_st = streams[tex_elem0->stream];
            auto* tex_b = object(tex_st.buffer);
            if (tex_b && tex_elem0->type >= 1 && tex_elem0->type <= 3) { // FLOAT2/3/4
                tex0_buf_handle = tex_st.buffer;
                tex0_byte_offset = tex_st.offset + tex_elem0->offset + std::size_t(base) * tex_st.stride;
                tex0_ptr = tex_b->bytes.data() + tex0_byte_offset;
                tex0_stride = tex_st.stride;
                tex0_coords = (tex_elem0->type == 1) ? 2 : (tex_elem0->type == 2 ? 3 : 4);
                tex0_enabled = true;
            }
        }

        if (!tex0_enabled && effective_tex0_id && (cvs.has_envmap || cvs.has_texgen) && cdecl.norm && cdecl.norm->stream < streams.size()) {
            const auto& norm_st = streams[cdecl.norm->stream];
            auto* norm_b = object(norm_st.buffer);
            if (norm_b && (cdecl.norm->type == 2 || cdecl.norm->type == 3)) { // FLOAT3/4
                tex0_buf_handle = norm_st.buffer;
                tex0_byte_offset = norm_st.offset + cdecl.norm->offset + std::size_t(base) * norm_st.stride;
                tex0_ptr = norm_b->bytes.data() + tex0_byte_offset;
                tex0_stride = norm_st.stride;
                tex0_coords = 3;
                tex0_enabled = true;
                if (cvs.tex_mat_mode == 0) {
                    float tm[16]{
                        0.5f,  0.0f, 0.0f, 0.0f,
                        0.0f, -0.5f, 0.0f, 0.0f,
                        0.0f,  0.0f, 1.0f, 0.0f,
                        0.5f,  0.5f, 0.0f, 1.0f
                    };
                    glMatrixMode(GL_TEXTURE);
                    glLoadMatrixf(tm);
                    glMatrixMode(GL_MODELVIEW);
                    tex_matrix_identity_ = false;
                }
            }
        }

        bool tex1_enabled = false;
        int tex1_coords = 2;
        const uint8_t* tex1_ptr = nullptr;
        std::uint32_t tex1_stride = 0;
        std::uint32_t tex1_buf_handle = 0;
        std::size_t tex1_byte_offset = 0;
        if (allow_stage1 && tex_elem1 && tex_elem1->stream < streams.size()) {
            const auto& tex_st = streams[tex_elem1->stream];
            auto* tex_b = object(tex_st.buffer);
            if (tex_b && tex_elem1->type >= 1 && tex_elem1->type <= 3) {
                tex1_buf_handle = tex_st.buffer;
                tex1_byte_offset = tex_st.offset + tex_elem1->offset + std::size_t(base) * tex_st.stride;
                tex1_ptr = tex_b->bytes.data() + tex1_byte_offset;
                tex1_stride = tex_st.stride;
                tex1_coords = (tex_elem1->type == 1) ? 2 : (tex_elem1->type == 2 ? 3 : 4);
                tex1_enabled = true;
            }
        }

        bool col_enabled = false;
        const uint8_t* col_ptr = nullptr;
        std::uint32_t col_stride = 0;
        std::uint32_t col_buf_handle = 0;
        std::size_t col_byte_offset = 0;
        const uint8_t* raw_col = nullptr;
        if (col_elem && col_elem->stream < streams.size()) {
            const auto& col_st = streams[col_elem->stream];
            auto* col_b = object(col_st.buffer);
            if (col_b && col_elem->type == 4) { // D3DCOLOR -> RGBA
                col_buf_handle = col_st.buffer;
                col_byte_offset = col_st.offset + col_elem->offset + std::size_t(base) * col_st.stride;
                raw_col = col_b->bytes.data() + col_byte_offset;
                col_ptr = get_vb_rgba_ptr(col_st.buffer, col_b, col_st.offset, col_st.stride,
                                          col_elem->offset, base, min + vertices);
                col_stride = (col_ptr == raw_col) ? col_st.stride : 4u;
                col_enabled = true;
                color_const_white_ = false;
            }
        }

        set_client_states(true, tex0_enabled, tex1_enabled, col_enabled);
        const platform::PcVertexElement* weight_elem = cdecl.weight;
        platform::PcVertexElement fallback_weight{};
        if (!weight_elem && cvs.skin_bones > 0 && pos_elem && pos_st.stride >= 16u) {
            fallback_weight.stream = pos_elem->stream;
            fallback_weight.offset = pos_elem->offset + 12u;
            fallback_weight.type = (cvs.skin_bones == 1) ? 0 : (cvs.skin_bones == 2 ? 1 : 2);
            weight_elem = &fallback_weight;
        }

        bool did_skin = false;
        if (is_skinned && weight_elem && weight_elem->stream < streams.size()) {
            const auto& w_st = streams[weight_elem->stream];
            auto* w_b = object(w_st.buffer);
            const uint8_t* w_data = w_b ? w_b->bytes.data() : nullptr;
            const std::size_t w_sz = w_b ? w_b->bytes.size() : 0u;
            const std::size_t pos_sz = pos_b ? pos_b->bytes.size() : 0u;
            const std::uint32_t nverts = min + vertices;
            const uint8_t* weight_ptr = (w_b && weight_elem->offset + std::ptrdiff_t(base) * w_st.stride >= 0)
                ? (w_data + w_st.offset + weight_elem->offset + std::ptrdiff_t(base) * w_st.stride) : nullptr;

            const std::size_t pos_base_offset = pos_st.offset + std::size_t(base) * pos_st.stride;
            const std::size_t w_base_offset = w_st.offset + std::size_t(base) * w_st.stride;

            std::uint32_t avail_pos = (pos_b && pos_st.stride > 0 && pos_sz > pos_base_offset)
                ? static_cast<std::uint32_t>((pos_sz - pos_base_offset) / pos_st.stride) : 0u;
            std::uint32_t avail_w = (w_b && w_st.stride > 0 && w_sz > w_base_offset)
                ? static_cast<std::uint32_t>((w_sz - w_base_offset) / w_st.stride) : 0u;
            std::uint32_t max_avail = std::min(avail_pos, avail_w);

            const uint8_t* ib_data = ib ? ib->bytes.data() : nullptr;
            const uint16_t* ib_ptr = reinterpret_cast<const uint16_t*>(ib_data + std::size_t(start) * 2u);
            std::uint32_t max_idx = 0;
            for (std::size_t i = 0; i < num_indices; ++i) {
                if (ib_ptr[i] > max_idx) max_idx = ib_ptr[i];
            }

            std::uint32_t needed_verts = std::max(nverts, max_idx + 1u);
            std::uint32_t count_verts = std::min(needed_verts, max_avail);
            if (count_verts > 8192u) count_verts = 8192u;

            if (w_b && weight_ptr && count_verts > 0 && pos_ptr &&
                pos_ptr >= pos_data && weight_ptr >= w_data) {
                did_skin = true;
                const std::uint32_t total_alloc = std::max(count_verts, max_idx + 1u);
                sw_skin_pos_.resize(std::size_t(total_alloc) * 3u);

                const uint8_t* idx_ptr_vtx = nullptr;
                std::uint32_t idx_stride = 0;
                if (cdecl.indices && cdecl.indices->stream < streams.size()) {
                    const auto& idx_st = streams[cdecl.indices->stream];
                    auto* idx_b = object(idx_st.buffer);
                    const uint8_t* idx_data = idx_b ? idx_b->bytes.data() : nullptr;
                    const std::size_t idx_sz = idx_b ? idx_b->bytes.size() : 0u;
                    if (idx_b && cdecl.indices->offset + std::ptrdiff_t(base) * idx_st.stride >= 0) {
                        const uint8_t* ip = idx_data + idx_st.offset + cdecl.indices->offset + std::ptrdiff_t(base) * idx_st.stride;
                        if (ip >= idx_data && (ip + std::size_t(count_verts) * idx_st.stride <= idx_data + idx_sz)) {
                            idx_ptr_vtx = ip;
                            idx_stride = idx_st.stride;
                        }
                    }
                }
                const uint8_t w_type = weight_elem->type;
                const float* c84 = vs_constants[84].data();
                const float* c85 = vs_constants[85].data();
                const float* c86 = vs_constants[86].data();
                const float* c88 = vs_constants[88].data();
                const float* c89 = vs_constants[89].data();
                const float* c90 = vs_constants[90].data();
                const float* c92 = vs_constants[92].data();
                const float* c93 = vs_constants[93].data();
                const float* c94 = vs_constants[94].data();
                const uint8_t num_bones = cvs.skin_bones ? cvs.skin_bones : (w_type == 0 ? 1 : (w_type == 1 ? 2 : 3));

                for (std::uint32_t v = 0; v < count_verts; ++v) {
                    const float* p = reinterpret_cast<const float*>(pos_ptr + v * pos_st.stride);
                    const uint8_t* w_raw = weight_ptr + v * w_st.stride;
                    float px = p[0], py = p[1], pz = p[2];
                    float w0 = 1.0f, w1 = 0.0f, w2 = 0.0f, w3 = 0.0f;
                    if (w_type == 0) {
                        w0 = std::clamp(*reinterpret_cast<const float*>(w_raw), 0.0f, 1.0f);
                        w1 = 1.0f - w0;
                    } else if (w_type == 1) {
                        const float* wf = reinterpret_cast<const float*>(w_raw);
                        w0 = std::max(0.0f, wf[0]);
                        w1 = std::max(0.0f, wf[1]);
                        w2 = std::max(0.0f, 1.0f - (w0 + w1));
                        float sum = w0 + w1 + w2;
                        if (sum > 1e-4f) { w0 /= sum; w1 /= sum; w2 /= sum; }
                        else { w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; }
                    } else if (w_type == 2) {
                        const float* wf = reinterpret_cast<const float*>(w_raw);
                        w0 = std::max(0.0f, wf[0]);
                        w1 = std::max(0.0f, wf[1]);
                        w2 = std::max(0.0f, wf[2]);
                        w3 = std::max(0.0f, 1.0f - (w0 + w1 + w2));
                        float sum = w0 + w1 + w2 + w3;
                        if (sum > 1e-4f) { w0 /= sum; w1 /= sum; w2 /= sum; w3 /= sum; }
                        else { w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; w3 = 0.0f; }
                    } else if (w_type == 3) {
                        const float* wf = reinterpret_cast<const float*>(w_raw);
                        w0 = std::max(0.0f, wf[0]);
                        w1 = std::max(0.0f, wf[1]);
                        w2 = std::max(0.0f, wf[2]);
                        w3 = std::max(0.0f, wf[3]);
                        float sum = w0 + w1 + w2 + w3;
                        if (sum > 1e-4f) { w0 /= sum; w1 /= sum; w2 /= sum; w3 /= sum; }
                        else { w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; w3 = 0.0f; }
                    } else {
                        w0 = w_raw[0] * (1.0f / 255.0f);
                        w1 = w_raw[1] * (1.0f / 255.0f);
                        w2 = w_raw[2] * (1.0f / 255.0f);
                        w3 = w_raw[3] * (1.0f / 255.0f);
                        float sum = w0 + w1 + w2 + w3;
                        if (sum > 1e-4f) { w0 /= sum; w1 /= sum; w2 /= sum; w3 /= sum; }
                        else { w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; w3 = 0.0f; }
                    }
                    float b0x = px, b0y = py, b0z = pz;
                    float b1x = px * c84[0] + py * c84[1] + pz * c84[2] + c84[3];
                    float b1y = px * c85[0] + py * c85[1] + pz * c85[2] + c85[3];
                    float b1z = px * c86[0] + py * c86[1] + pz * c86[2] + c86[3];
                    float b2x = px * c88[0] + py * c88[1] + pz * c88[2] + c88[3];
                    float b2y = px * c89[0] + py * c89[1] + pz * c89[2] + c89[3];
                    float b2z = px * c90[0] + py * c90[1] + pz * c90[2] + c90[3];
                    float b3x = px * c92[0] + py * c92[1] + pz * c92[2] + c92[3];
                    float b3y = px * c93[0] + py * c93[1] + pz * c93[2] + c93[3];
                    float b3z = px * c94[0] + py * c94[1] + pz * c94[2] + c94[3];
                    float rx, ry, rz;
                    if (idx_ptr_vtx) {
                        const uint8_t* idx = idx_ptr_vtx + v * idx_stride;
                        auto decode_bone = [](uint8_t raw) -> int {
                            if (raw >= 84) return (raw - 84) / 4;
                            if (raw % 4 == 0 && raw <= 12) return raw / 4;
                            if (raw % 3 == 0 && raw <= 9) return raw / 3;
                            return raw;
                        };
                        auto get_bx = [&](int i) { return i == 1 ? b1x : (i == 2 ? b2x : (i == 3 ? b3x : b0x)); };
                        auto get_by = [&](int i) { return i == 1 ? b1y : (i == 2 ? b2y : (i == 3 ? b3y : b0y)); };
                        auto get_bz = [&](int i) { return i == 1 ? b1z : (i == 2 ? b2z : (i == 3 ? b3z : b0z)); };
                        rx = w0 * get_bx(decode_bone(idx[0])) + w1 * get_bx(decode_bone(idx[1])) +
                             w2 * get_bx(decode_bone(idx[2])) + w3 * get_bx(decode_bone(idx[3]));
                        ry = w0 * get_by(decode_bone(idx[0])) + w1 * get_by(decode_bone(idx[1])) +
                             w2 * get_by(decode_bone(idx[2])) + w3 * get_by(decode_bone(idx[3]));
                        rz = w0 * get_bz(decode_bone(idx[0])) + w1 * get_bz(decode_bone(idx[1])) +
                             w2 * get_bz(decode_bone(idx[2])) + w3 * get_bz(decode_bone(idx[3]));
                    } else if (num_bones == 1) {
                        rx = w0 * b0x + w1 * b1x; ry = w0 * b0y + w1 * b1y; rz = w0 * b0z + w1 * b1z;
                    } else if (num_bones == 2) {
                        rx = w0 * b0x + w1 * b1x + w2 * b2x; ry = w0 * b0y + w1 * b1y + w2 * b2y; rz = w0 * b0z + w1 * b1z + w2 * b2z;
                    } else {
                        rx = w0 * b0x + w1 * b1x + w2 * b2x + w3 * b3x; ry = w0 * b0y + w1 * b1y + w2 * b2y + w3 * b3y; rz = w0 * b0z + w1 * b1z + w2 * b2z + w3 * b3z;
                    }
                    sw_skin_pos_[v * 3 + 0] = rx;
                    sw_skin_pos_[v * 3 + 1] = ry;
                    sw_skin_pos_[v * 3 + 2] = rz;
                }
                for (std::uint32_t v = count_verts; v < total_alloc; ++v) {
                    const float* p = reinterpret_cast<const float*>(pos_ptr + v * pos_st.stride);
                    sw_skin_pos_[v * 3 + 0] = p[0];
                    sw_skin_pos_[v * 3 + 1] = p[1];
                    sw_skin_pos_[v * 3 + 2] = p[2];
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glVertexPointer(3, GL_FLOAT, GLsizei(3 * sizeof(float)), sw_skin_pos_.data());
            } else if (pos_cb.vbo) {
                glBindBuffer(GL_ARRAY_BUFFER, pos_cb.vbo);
                glVertexPointer(pos_elem->type == 3 ? 4 : 3, GL_FLOAT, GLsizei(pos_st.stride), reinterpret_cast<const void*>(pos_byte_offset));
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glVertexPointer(pos_elem->type == 3 ? 4 : 3, GL_FLOAT, GLsizei(pos_st.stride), pos_ptr);
            }
        } else if (pos_cb.vbo) {
            glBindBuffer(GL_ARRAY_BUFFER, pos_cb.vbo);
            glVertexPointer(pos_elem->type == 3 ? 4 : 3, GL_FLOAT, GLsizei(pos_st.stride), reinterpret_cast<const void*>(pos_byte_offset));
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexPointer(pos_elem->type == 3 ? 4 : 3, GL_FLOAT, GLsizei(pos_st.stride), pos_ptr);
        }
        if (tex0_enabled) {
            glClientActiveTexture(GL_TEXTURE0);
            const auto& tex0_cb = (tex0_buf_handle < cdram_buffers_.size()) ? cdram_buffers_[tex0_buf_handle] : CdramBuffer{};
            if (tex0_cb.vbo) {
                glBindBuffer(GL_ARRAY_BUFFER, tex0_cb.vbo);
                glTexCoordPointer(tex0_coords >= 3 ? 3 : 2, GL_FLOAT, GLsizei(tex0_stride), reinterpret_cast<const void*>(tex0_byte_offset));
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glTexCoordPointer(tex0_coords >= 3 ? 3 : 2, GL_FLOAT, GLsizei(tex0_stride), tex0_ptr);
            }
        }
        if (tex1_enabled) {
            glClientActiveTexture(GL_TEXTURE1);
            const auto& tex1_cb = (tex1_buf_handle < cdram_buffers_.size()) ? cdram_buffers_[tex1_buf_handle] : CdramBuffer{};
            if (tex1_cb.vbo) {
                glBindBuffer(GL_ARRAY_BUFFER, tex1_cb.vbo);
                glTexCoordPointer(tex1_coords >= 3 ? 3 : 2, GL_FLOAT, GLsizei(tex1_stride), reinterpret_cast<const void*>(tex1_byte_offset));
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glTexCoordPointer(tex1_coords >= 3 ? 3 : 2, GL_FLOAT, GLsizei(tex1_stride), tex1_ptr);
            }
            glClientActiveTexture(GL_TEXTURE0);
        }
        if (col_enabled) {
            const auto& col_cb = (col_buf_handle < cdram_buffers_.size()) ? cdram_buffers_[col_buf_handle] : CdramBuffer{};
            if (col_cb.vbo && col_ptr == raw_col) {
                glBindBuffer(GL_ARRAY_BUFFER, col_cb.vbo);
                glColorPointer(4, GL_UNSIGNED_BYTE, GLsizei(col_stride), reinterpret_cast<const void*>(col_byte_offset));
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glColorPointer(4, GL_UNSIGNED_BYTE, GLsizei(col_stride), col_ptr);
            }
        } else {
            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
            if (vs) {
                const float mr = vs_constants[18][0], mg = vs_constants[18][1], mb = vs_constants[18][2], ma = vs_constants[18][3];
                if (effective_tex0_id == 0) {
                    if (mr != 0.0f || mg != 0.0f || mb != 0.0f || ma != 0.0f) {
                        ca = std::clamp(ma, 0.0f, 1.0f);
                        cr = std::clamp(mr * 0.75f + vs_constants[19][0] * 0.25f + vs_constants[20][0], 0.0f, 1.0f);
                        cg = std::clamp(mg * 0.75f + vs_constants[19][1] * 0.25f + vs_constants[20][1], 0.0f, 1.0f);
                        cb = std::clamp(mb * 0.75f + vs_constants[19][2] * 0.25f + vs_constants[20][2], 0.0f, 1.0f);
                    } else {
                        cr = 1.0f;
                        cg = 1.0f;
                        cb = 1.0f;
                        ca = 1.0f;
                    }
                } else {
                    cr = 1.0f;
                    cg = 1.0f;
                    cb = 1.0f;
                    if (batch_key_.blend_enable && ma > 0.0f && ma < 1.0f) {
                        ca = std::clamp(ma < 0.58f ? 0.58f : ma, 0.0f, 1.0f);
                    } else {
                        ca = 1.0f;
                    }
                }
            }
            if (cr != 1.0f || cg != 1.0f || cb != 1.0f || ca != 1.0f) {
                glColor4f(cr, cg, cb, ca);
                color_const_white_ = false;
            } else if (!color_const_white_) {
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                color_const_white_ = true;
            }
        }

        const std::uint32_t dt_setup = outrun::vita_profile::vita_time_us() - t_setup_start;
        outrun::vita_profile::g_sub_state_setup_us += dt_setup;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_state_setup_us_ += dt_setup;

        const GLenum mode = (type == 5) ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
        const auto& ib_cb = (indices < cdram_buffers_.size()) ? cdram_buffers_[indices] : CdramBuffer{};

        const std::uint32_t t_draw_start = outrun::vita_profile::vita_time_us();
        if (is_cropped_decal) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-3.0f, -6.0f);
        }
        if (ib_cb.vbo) {
            if (last_ibo_ != ib_cb.vbo) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_cb.vbo);
                last_ibo_ = ib_cb.vbo;
            }
            glDrawElements(mode, GLsizei(num_indices), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(std::size_t(start) * 2u));
        } else {
            if (last_ibo_ != 0) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                last_ibo_ = 0;
            }
            const void* idx_ptr = reinterpret_cast<const uint8_t*>(ib->bytes.data()) + std::size_t(start) * 2u;
            glDrawElements(mode, GLsizei(num_indices), GL_UNSIGNED_SHORT, idx_ptr);
        }
        if (is_cropped_decal) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        const std::uint32_t dt_draw = outrun::vita_profile::vita_time_us() - t_draw_start;

        outrun::vita_profile::g_sub_gl_draw_elements_us += dt_draw;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_gl_draw_elements_us_ += dt_draw;

        triangles_drawn += count;
        outrun::vita_profile::g_triangles += count;
        outrun::vita_profile::accumulate_model_polygons(type, count, is_skinned);

        s_last_3d.valid = true;
        s_last_3d.type = type;
        s_last_3d.base = base;
        s_last_3d.decl_handle = decl_handle;
        s_last_3d.vs_id = vertex_shader;
        s_last_3d.ps_id = pixel_shader;
        s_last_3d.tex0_handle = tex0_handle;
        s_last_3d.tex1_handle = tex1_handle;
        s_last_3d.key = key;
        s_last_3d.tex_mat_mode = cvs.tex_mat_mode;
        s_last_3d.pos_buffer = is_skinned ? 0xFFFFFFFEu : pos_st.buffer;
        s_last_3d.pos_offset = pos_st.offset;
        s_last_3d.pos_stride = pos_st.stride;
        s_last_3d.stage0_uv = stage[0][11];
        s_last_3d.stage1_uv = stage[1][11];
        s_last_3d.is_glass = is_glass;
        if (col_elem && col_elem->stream < streams.size()) {
            s_last_3d.col_buffer = streams[col_elem->stream].buffer;
            s_last_3d.col_offset = streams[col_elem->stream].offset;
            s_last_3d.col_stride = streams[col_elem->stream].stride;
        } else {
            s_last_3d.col_buffer = 0xFFFFFFFFu;
            s_last_3d.const_col[0] = vs ? vs_constants[18][0] : 1.0f;
            s_last_3d.const_col[1] = vs ? vs_constants[18][1] : 1.0f;
            s_last_3d.const_col[2] = vs ? vs_constants[18][2] : 1.0f;
            s_last_3d.const_col[3] = vs ? vs_constants[18][3] : 1.0f;
        }

        if (outrun::vita_profile::g_capture_render_frame) {
            std::string bstr;
            if (vs && !vs->program.blobs.empty()) {
                for (size_t i = 0; i < vs->program.blobs.size(); ++i) {
                    if (i > 0) bstr += ",";
                    bstr += std::to_string(vs->program.blobs[i]);
                }
            } else {
                bstr = "none";
            }
            outrun::vita_profile::bottleneck_log(
                "[DRAW_3D #%u] %s [%s] | VS_ID: %d (blobs: [%s]) | Type: %s | Tris: %u | BaseVtx: %d | TexID: %u | Setup: %u us | glDraw_time: %u us\n",
                snapshot_draw_3d_count_,
                outrun::vita_profile::get_active_model_name(),
                outrun::vita_profile::get_active_object_name(),
                active_vs_id,
                bstr.c_str(),
                type == 5 ? "TRI_STRIP" : (type == 4 ? "TRI_LIST" : "OTHER"),
                count,
                base,
                effective_tex0_id,
                dt_setup,
                dt_draw
            );
            if (is_skinned || effective_tex0_id == 0 || !tex0_enabled || textures[1] != 0 || cvs.tex_mat_mode != 0 || cvs.has_texgen ||
                active_vs_id == 60 || active_vs_id == 70 || snapshot_draw_3d_count_ <= 8) {
                std::string pstr = "none";
                if (ps && ps->kind == KPs && !ps->program.blobs.empty()) {
                    pstr.clear();
                    for (size_t i = 0; i < ps->program.blobs.size(); ++i) {
                        if (i > 0) pstr += ",";
                        pstr += std::to_string(ps->program.blobs[i]);
                    }
                }
                outrun::vita_profile::bottleneck_log(
                    "  [DRAW_3D_EX] ps=%x([%s]) env=%d uv_ok=%d,%d uv_type=%d,%d col=%d t0=%u t1=%u at=%u(ref=%u) ab=%u(%u/%u) skin=%d(bones=%u)\n",
                    ps ? ps->program.version : 0u, pstr.c_str(), int(desired_env0),
                    tex0_enabled ? 1 : 0, tex1_enabled ? 1 : 0,
                    tex_elem0 ? int(tex_elem0->type) : -1, tex_elem1 ? int(tex_elem1->type) : -1,
                    col_enabled ? 1 : 0, textures[0], textures[1],
                    render[RS_ALPHATESTENABLE], render[RS_ALPHAREF] & 0xffu,
                    render[RS_ALPHABLENDENABLE], render[RS_SRCBLEND], render[RS_DESTBLEND],
                    did_skin ? 1 : 0, unsigned(cvs.skin_bones)
                );
            }
        }

        return 0;
    }

    const std::uint32_t t_sw_start = outrun::vita_profile::vita_time_us();

    const unsigned sw_tex_id = texture(textures[0]);
    (void)sw_tex_id;
    if (textures[0] && textures[0] < textures_gl_.size() && textures_gl_[textures[0]].is_cropped_decal) {
        log_decal_debug("[DRAW_SW] h0=%u h1=%u ab=%u(%u/%u) at=%u(ref=%u) reason=%s\n",
                        textures[0], textures[1], key.blend_enable, key.src_blend, key.dst_blend,
                        key.alpha_test, key.alpha_ref, fail_reason ? fail_reason : "none");
        key.blend_enable = 1;
        key.src_blend = 5;
        key.dst_blend = 6;
        key.alpha_test = 1;
        key.alpha_ref = 128;
        key.alpha_func = 7;
    }

    if (!batch_.empty() && (key != batch_key_ || batch_.size() + count * 3 >= 4096)) {
        flush();
    }
    batch_key_ = key;
    batch_rhw_ = false;

    auto index = [&](std::uint32_t k) -> std::uint16_t {
        std::uint16_t v;
        std::memcpy(&v, ib->bytes.data() + std::size_t(k) * 2, 2);
        return v;
    };

    if (++vertex_cache_gen_ == 0u) {
        std::fill(vertex_cache_tag_.begin(), vertex_cache_tag_.end(), 0u);
        vertex_cache_gen_ = 1u;
    }
    const std::uint32_t gen = vertex_cache_gen_;

    auto vertex = [&](std::uint32_t i) -> const Vertex* {
        if (i >= vertex_cache_.size()) {
            vertex_cache_.resize(std::size_t(i) + 1024u);
            vertex_cache_tag_.resize(vertex_cache_.size(), 0u);
        }
        if (vertex_cache_tag_[i] == gen) return &vertex_cache_[i];
        if (!shade_vertex(i, base, vertex_cache_[i])) return nullptr;
        vertex_cache_tag_[i] = gen;
        return &vertex_cache_[i];
    };

    const std::size_t total = type == 5 ? std::size_t(count) + 2 : (type == 4 ? std::size_t(count) * 3 : 0);
    if (!total || (std::size_t(start) + total) * 2 > ib->bytes.size()) return 1;

    auto push_tri = [&](const Vertex& v0, const Vertex& v1, const Vertex& v2) {
        Vertex a = v0, b = v1, c = v2;
        // Direct3D depth [0, w] -> OpenGL depth [-w, w]
        a.pos[2] = 2.0f * a.pos[2] - a.pos[3];
        b.pos[2] = 2.0f * b.pos[2] - b.pos[3];
        c.pos[2] = 2.0f * c.pos[2] - c.pos[3];

        batch_.push_back(a);
        batch_.push_back(b);
        batch_.push_back(c);
        ++triangles_drawn;
        ++outrun::vita_profile::g_triangles;
    };

    for (std::uint32_t p = 0; p < count; ++p) {
        std::uint32_t i0, i1, i2;
        if (type == 5) { // Triangle Strip
            i0 = index(start + p);
            i1 = index(start + p + 1);
            i2 = index(start + p + 2);
            if (p & 1u) std::swap(i0, i1);
        } else { // Triangle List
            i0 = index(start + p * 3);
            i1 = index(start + p * 3 + 1);
            i2 = index(start + p * 3 + 2);
        }
        if (i0 == i1 || i1 == i2 || i0 == i2) continue; // Degenerate triangle
        const Vertex* va = vertex(i0);
        const Vertex* vb = vertex(i1);
        const Vertex* vc = vertex(i2);
        if (!va || !vb || !vc) continue;
        push_tri(*va, *vb, *vc);
    }

    if (batch_.size() >= 4096) {
        flush();
    }
    outrun::vita_profile::accumulate_model_polygons(type, count, false);

    const std::uint32_t dt_sw = outrun::vita_profile::vita_time_us() - t_sw_start;
    outrun::vita_profile::g_sub_vertex_copy_pack_us += dt_sw;
    if (outrun::vita_profile::g_capture_render_frame) {
        snapshot_t_vertex_copy_pack_us_ += dt_sw;
        outrun::vita_profile::bottleneck_log(
            "[DRAW_3D #%u] %s [%s] | SW_FALLBACK | VS_ID: %d | Tris: %u | Reason: %s | Time: %u us\n",
            snapshot_draw_3d_count_,
            outrun::vita_profile::get_active_model_name(),
            outrun::vita_profile::get_active_object_name(),
            active_vs_id,
            count,
            fail_reason ? fail_reason : "unknown",
            dt_sw
        );
    }
    return 0;
}

void VitaGlD3D9Device::draw_primitive_up(std::uint32_t type, std::uint32_t count,
                                         const void* data, std::uint32_t stride) {
    if (!data || !count || count_only || colour_target_ != BackBuffer) return;
    if (async_recording_) {
        std::size_t total = 0;
        switch (type) {
            case 4: total = std::size_t(count) * 3; break;
            case 5: case 6: total = std::size_t(count) + 2; break;
            default: return;
        }
        const std::size_t byte_len = total * stride;
        auto& slot = async_slots_[write_slot_];
        // Sync fields that PcSoftD3DXSprite::draw / draw_indexed_primitive_up mutate directly without calling set_*
        auto emit_rs = [&](std::uint32_t s) {
            GlCmd r{}; r.kind = GlCmdKind::SetRS; r.u0 = s; r.u1 = render[s]; slot.cmds.push_back(r);
        };
        { GlCmd t{}; t.kind = GlCmdKind::SetTex; t.u0 = 0u; t.u1 = textures[0]; slot.cmds.push_back(t); }
        { GlCmd v{}; v.kind = GlCmdKind::SetVS;  v.u0 = vertex_shader; slot.cmds.push_back(v); }
        { GlCmd p{}; p.kind = GlCmdKind::SetPS;  p.u0 = pixel_shader;  slot.cmds.push_back(p); }
        { GlCmd f{}; f.kind = GlCmdKind::SetFvf; f.u0 = fvf_;          slot.cmds.push_back(f); }
        emit_rs(RS_CULLMODE);
        emit_rs(RS_ALPHABLENDENABLE);
        emit_rs(RS_SRCBLEND);
        emit_rs(RS_DESTBLEND);
        emit_rs(RS_ALPHATESTENABLE);
        emit_rs(RS_ALPHAFUNC);
        emit_rs(RS_ALPHAREF);
        emit_rs(RS_ZENABLE);
        emit_rs(RS_ZWRITEENABLE);
        emit_rs(RS_ZFUNC);
        emit_rs(RS_COLORWRITEENABLE);
        emit_rs(RS_LIGHTING);
        { GlCmd st0{}; st0.kind = GlCmdKind::SetStage; st0.u0 = 0u; st0.u1 = 4u; st0.u2 = stage[0][4]; slot.cmds.push_back(st0); }
        { GlCmd st1{}; st1.kind = GlCmdKind::SetStage; st1.u0 = 0u; st1.u1 = 5u; st1.u2 = stage[0][5]; slot.cmds.push_back(st1); }
        { GlCmd st2{}; st2.kind = GlCmdKind::SetStage; st2.u0 = 0u; st2.u1 = 6u; st2.u2 = stage[0][6]; slot.cmds.push_back(st2); }

        // Align to 4 bytes
        const std::size_t aligned_off = (slot.up_bytes.size() + 3u) & ~std::size_t(3u);
        slot.up_bytes.resize(aligned_off + byte_len);
        std::memcpy(slot.up_bytes.data() + aligned_off, data, byte_len);
        GlCmd c{};
        c.kind = GlCmdKind::DrawUp;
        c.u0 = type; c.u1 = count; c.u2 = stride; c.u3 = static_cast<std::uint32_t>(aligned_off);
        slot.cmds.push_back(c);
        ++submitted_draws;
        return;
    }
    outrun::vita_render::run_sync([&]() {
        snapshot_live_state(replay_);
        replaying_async_ = true;
        draw_primitive_up_gl(replay_, type, count, data, stride);
        replaying_async_ = false;
    });
}

void VitaGlD3D9Device::draw_primitive_up_gl(const ReplayState& st, std::uint32_t type, std::uint32_t count,
                                            const void* data, std::uint32_t stride) {
    const auto& render = st.render;
    const auto& textures = st.textures;
    const auto& stage = st.stage;
    const auto& vs_constants = st.vs_constants;
    const auto vertex_shader = st.vertex_shader;
    const auto fvf_ = st.fvf;
    auto transform_wvp = [&st]() { return st.transform_wvp(); };
    // Note: colour_target_ == BackBuffer and !count_only are already validated on Core 1
    // in draw_primitive_up() before recording/dispatching. Do NOT re-check live colour_target_
    // here on Core 0, because Core 1 may have switched to an offscreen RT for frame N+1!
    if (!data || !count) return;
    struct DipScope {
        std::uint32_t t0;
        DipScope() : t0(outrun::vita_profile::vita_time_us()) {}
        ~DipScope() { outrun::vita_profile::g_sub_dip_total_us += (outrun::vita_profile::vita_time_us() - t0); }
    } dip_scope;

    const std::uint32_t fvf = fvf_;
    const bool rhw = (fvf & 0x400eu) == 0x004u;
    const bool xyz = (fvf & 0x400eu) == 0x002u;
    if (!rhw && !xyz) return;
    s_last_3d.valid = false;

    if (!rhw && scale_zero_) return;
    if (!rhw) has_3d_draws_ = true;
    else outrun::vita_profile::set_active_model(outrun::vita_profile::RES_ID_2D_HUD, 0);
    ++outrun::vita_profile::g_draw_calls;
    if (outrun::vita_profile::g_capture_render_frame) {
        ++snapshot_draw_count_;
        ++snapshot_draw_up_count_;
    }
    const std::uint32_t t_call_start = outrun::vita_profile::vita_time_us();

    BatchKey key{
        textures[0],
        rhw,
        render[RS_ALPHABLENDENABLE],
        render[RS_SRCBLEND],
        render[RS_DESTBLEND],
        render[RS_ALPHATESTENABLE],
        render[RS_ALPHAFUNC],
        render[RS_ALPHAREF] & 0xffu,
        render[RS_ZENABLE],
        render[RS_ZWRITEENABLE],
        render[RS_ZFUNC],
        render[RS_CULLMODE],
        render[RS_COLORWRITEENABLE] & 0xfu,
        0, 0, int(back_w_), int(back_h_)
    };

    std::size_t total = 0;
    switch (type) {
        case 4: total = std::size_t(count) * 3; break;
        case 5: case 6: total = std::size_t(count) + 2; break;
        default: return;
    }

    constexpr std::size_t none = ~std::size_t(0);
    std::size_t off = rhw ? 16 : 12;
    if (fvf & 0x010u) off += 12;
    if (fvf & 0x020u) off += 4;
    const std::size_t diffuse_at = (fvf & 0x040u) ? off : none; if (fvf & 0x040u) off += 4;
    const std::size_t specular_at = (fvf & 0x080u) ? off : none; if (fvf & 0x080u) off += 4;
    const unsigned sets = std::min(8u, (fvf >> 8) & 0xfu);
    std::size_t tex_at[8]{}; unsigned tex_n[8]{};
    for (unsigned k = 0; k < sets; ++k) {
        const unsigned f = (fvf >> (16 + 2 * k)) & 3u;
        tex_n[k] = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        tex_at[k] = off;
        off += 4u * tex_n[k];
    }
    if (stride < off) return;

    if (rhw) {
        if (!batch_.empty()) {
            flush();
        }

        const unsigned tex_id = texture(textures[0]);
        const bool alpha_blend = (render[RS_ALPHABLENDENABLE] != 0);
        const bool alpha_test = (render[RS_ALPHATESTENABLE] != 0);
        const bool z_en = (render[RS_ZENABLE] != 0) && (render[RS_ZFUNC] != 8u);

        const uint32_t full_blend_key = (alpha_blend ? 1u : 0u)
                                      | (alpha_blend ? ((render[RS_SRCBLEND] & 0x0fu) << 1) : 0u)
                                      | (alpha_blend ? ((render[RS_DESTBLEND] & 0x0fu) << 5) : 0u)
                                      | (alpha_test ? (1u << 9) : 0u)
                                      | (alpha_test ? ((render[RS_ALPHAFUNC] & 0x0fu) << 10) : 0u)
                                      | (alpha_test ? ((render[RS_ALPHAREF] & 0xffu) << 14) : 0u)
                                      | (z_en ? (1u << 22) : 0u)
                                      | ((render[RS_COLORWRITEENABLE] & 0x0fu) << 23)
                                      | (z_en ? ((render[RS_ZFUNC] & 0x0fu) << 27) : 0u);

        const std::uint32_t t_setup_start = outrun::vita_profile::vita_time_us();

        if (active_matrix_ != MATRIX_2D_ORTHO) {
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0f, 640.0f, 480.0f, 0.0f, -1.0f, 1.0f);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glDepthRange(0.0f, 1.0f);
            active_matrix_ = MATRIX_2D_ORTHO;
        }
        if (!tex_matrix_identity_) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            tex_matrix_identity_ = true;
        }

        if (last_vp_w_ != int(back_w_) || last_vp_h_ != int(back_h_) || last_vp_x_ != 0 || last_vp_y_ != 0) {
            glViewport(0, 0, int(back_w_), int(back_h_));
            glDepthRange(0.0f, 1.0f);
            last_vp_x_ = 0;
            last_vp_y_ = 0;
            last_vp_w_ = int(back_w_);
            last_vp_h_ = int(back_h_);
        }
        if (s_shadow_gl.cull_enabled != 0) {
            glDisable(GL_CULL_FACE);
            s_shadow_gl.cull_enabled = 0;
        }
        if (s_shadow_gl.depth_mask != GL_FALSE) {
            glDepthMask(GL_FALSE);
            s_shadow_gl.depth_mask = GL_FALSE;
        }

        if (full_blend_key != applied_2d_blend_key_) {
            applied_2d_blend_key_ = full_blend_key;

            if (s_shadow_gl.depth_test != (z_en ? 1 : 0)) {
                if (z_en) glEnable(GL_DEPTH_TEST);
                else glDisable(GL_DEPTH_TEST);
                s_shadow_gl.depth_test = z_en ? 1 : 0;
            }
            if (z_en) {
                static const GLenum z_funcs[] = {
                    GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                    GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
                };
                const unsigned zf = std::clamp(render[RS_ZFUNC], 1u, 8u) - 1u;
                if (s_shadow_gl.depth_func != z_funcs[zf]) {
                    glDepthFunc(z_funcs[zf]);
                    s_shadow_gl.depth_func = z_funcs[zf];
                }
            }

            if (alpha_blend) {
                if (s_shadow_gl.blend_enabled != 1) {
                    glEnable(GL_BLEND);
                    s_shadow_gl.blend_enabled = 1;
                }
                uint32_t sb = (full_blend_key >> 1) & 0x0fu;
                uint32_t db = (full_blend_key >> 5) & 0x0fu;
                GLenum sfactor = to_gl_blend(sb, GL_SRC_ALPHA);
                GLenum dfactor = to_gl_blend(db, GL_ONE_MINUS_SRC_ALPHA);
                if (sb == 2u && db == 6u) {
                    sfactor = GL_SRC_ALPHA;
                }
                if (s_shadow_gl.blend_src != sfactor || s_shadow_gl.blend_dst != dfactor) {
                    glBlendFunc(sfactor, dfactor);
                    s_shadow_gl.blend_src = sfactor;
                    s_shadow_gl.blend_dst = dfactor;
                }
            } else {
                if (s_shadow_gl.blend_enabled != 0) {
                    glDisable(GL_BLEND);
                    s_shadow_gl.blend_enabled = 0;
                }
            }

            const unsigned d3d_afunc = (full_blend_key >> 10) & 0x0fu;
            unsigned raw_ref = (full_blend_key >> 14) & 0xffu;
            bool enable_at = alpha_test;
            if (!alpha_blend && raw_ref >= 128u) {
                enable_at = false;
            }
            if (raw_ref == 0 && (d3d_afunc == 7u || d3d_afunc == 8u || d3d_afunc == 0u)) {
                enable_at = false;
            }
            if (enable_at) {
                if (s_shadow_gl.alpha_test != 1) {
                    glEnable(GL_ALPHA_TEST);
                    s_shadow_gl.alpha_test = 1;
                }
                GLenum gl_af = (d3d_afunc == 7u) ? GL_GEQUAL : GL_GREATER;
                const float ref_f = (float)raw_ref / 255.0f;
                if (s_shadow_gl.alpha_func != gl_af || s_shadow_gl.alpha_ref != ref_f) {
                    glAlphaFunc(gl_af, ref_f);
                    s_shadow_gl.alpha_func = gl_af;
                    s_shadow_gl.alpha_ref = ref_f;
                }
            } else {
                if (s_shadow_gl.alpha_test != 0) {
                    glDisable(GL_ALPHA_TEST);
                    s_shadow_gl.alpha_test = 0;
                }
            }

            const auto mask = render[RS_COLORWRITEENABLE];
            const GLboolean mr = (mask & 1) ? GL_TRUE : GL_FALSE;
            const GLboolean mg = (mask & 2) ? GL_TRUE : GL_FALSE;
            const GLboolean mb = (mask & 4) ? GL_TRUE : GL_FALSE;
            const GLboolean ma = (mask & 8) ? GL_TRUE : GL_FALSE;
            if (s_shadow_gl.color_mask_r != mr || s_shadow_gl.color_mask_g != mg ||
                s_shadow_gl.color_mask_b != mb || s_shadow_gl.color_mask_a != ma) {
                glColorMask(mr, mg, mb, ma);
                s_shadow_gl.color_mask_r = mr;
                s_shadow_gl.color_mask_g = mg;
                s_shadow_gl.color_mask_b = mb;
                s_shadow_gl.color_mask_a = ma;
            }
        }

        if (tex2d_enabled_[1]) {
            bind_texture_unit(GL_TEXTURE1, 1, 0, 0, TEX_ENV_MODULATE);
        }

        if (((tex_id != 0) != tex2d_enabled_[0]) ||
            (tex_id != 0 &&
             (tex_id != applied_2d_tex_ ||
              tex_id != last_bound_texture_[0] ||
              tex_env_mode_[0] != TEX_ENV_MODULATE))) {
            bind_texture_unit(GL_TEXTURE0, 0, tex_id, textures[0], TEX_ENV_MODULATE);
            applied_2d_tex_ = tex_id;
        }

        const auto* bytes = static_cast<const std::uint8_t*>(data);
        auto rdf = [](const std::uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; };
        auto rdu32 = [](const std::uint8_t* p) { std::uint32_t u; std::memcpy(&u, p, 4); return u; };
        auto d3d_to_rgba = [](std::uint32_t c) -> std::uint32_t {
            return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
        };

        Vertex2D local_v[64];
        std::vector<Vertex2D> heap_v;
        Vertex2D* v_ptr = local_v;
        if (total > 64) {
            heap_v.resize(total);
            v_ptr = heap_v.data();
        }

        for (std::size_t i = 0; i < total; ++i) {
            const auto* p = bytes + i * stride;
            Vertex2D& v = v_ptr[i];
            v.x = rdf(p);
            v.y = rdf(p + 4);
            v.z = rdf(p + 8);
            v.color = (diffuse_at != none) ? d3d_to_rgba(rdu32(p + diffuse_at)) : 0xFFFFFFFFu;
            if (sets > 0 && tex_n[0] >= 2) {
                v.u = rdf(p + tex_at[0]);
                v.v = rdf(p + tex_at[0] + 4);
            } else {
                v.u = 0.0f;
                v.v = 0.0f;
            }
        }

        set_client_states(true, tex_id != 0, false, true);
        color_const_white_ = false;
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex2D), &v_ptr[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex2D), &v_ptr[0].color);
        if (tex_id) {
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex2D), &v_ptr[0].u);
        }

        const std::uint32_t dt_setup = outrun::vita_profile::vita_time_us() - t_setup_start;
        outrun::vita_profile::g_sub_state_setup_us += dt_setup;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_state_setup_us_ += dt_setup;

        const std::uint32_t t_draw_start = outrun::vita_profile::vita_time_us();
        const GLenum mode = (type == 5) ? GL_TRIANGLE_STRIP : (type == 6 ? GL_TRIANGLE_FAN : GL_TRIANGLES);
        glDrawArrays(mode, 0, GLsizei(total));
        const std::uint32_t dt_draw = outrun::vita_profile::vita_time_us() - t_draw_start;

        outrun::vita_profile::g_sub_gl_draw_arrays_2d_us += dt_draw;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_gl_draw_arrays_2d_us_ += dt_draw;

        triangles_drawn += count;
        outrun::vita_profile::g_triangles += count;
        outrun::vita_profile::accumulate_model_polygons(type, count, false);
        s_last_3d.valid = false;
        state_dirty_ = true;
        ++outrun::vita_profile::g_hw_draws;

        if (outrun::vita_profile::g_capture_render_frame) {
            ++snapshot_hw_draws_;
            const std::uint32_t dt_total = outrun::vita_profile::vita_time_us() - t_call_start;
            outrun::vita_profile::bottleneck_log(
                "[DRAW_UP #%u] %s [%s] | Mode: 2D_RHW | PrimType: %u | Tris: %u | TexID: %u | Action: DIRECT | Time: %u us\n",
                snapshot_draw_up_count_,
                outrun::vita_profile::get_active_model_name(),
                outrun::vita_profile::get_active_object_name(),
                type,
                count,
                tex_id,
                dt_total
            );
        }
        return;
    }

    auto* vs = rhw ? nullptr : object(vertex_shader);
    int active_vs_id = -1;
    if (vs && vs->kind == KVs) {
        active_vs_id = vs->program.blobs.empty() ? -2 : int(vs->program.blobs[0]);
    } else if (vs) {
        active_vs_id = -3;
    }

    bool use_hw_path = false;
    const char* fail_reason = nullptr;

    if (!rhw) {
        if (vs && vs->kind == KVs) {
            const CachedVs& cvs = get_cached_vs(vertex_shader, vs);
            if (cvs.is_rigid) {
                use_hw_path = true;
            } else {
                fail_reason = "draw_up vs not rigid";
            }
        } else if (!vs) {
            use_hw_path = true;
        } else {
            fail_reason = "draw_up vs kind not KVs";
        }
    }

    if (use_hw_path) {
        if (!batch_.empty()) {
            flush();
        }
        batch_key_ = key;
        batch_rhw_ = false;

        const std::uint32_t t_setup_start = outrun::vita_profile::vita_time_us();

        const unsigned tex_id = texture(textures[0]);
        auto* ps = object(pixel_shader);
        const CachedPs& cps = get_cached_ps(pixel_shader, ps);
        TexEnvMode desired_env = TEX_ENV_MODULATE;
        const bool is_audience = (textures[0] >= 192u && textures[0] <= 195u) || textures[0] == 299u ||
                                 (tex_id >= 192u && tex_id <= 195u);
        const float ma_val = vs ? vs_constants[18][3] : 0.0f;
        const bool has_tex_alpha_up = (textures[0] && textures[0] < textures_gl_.size() &&
                                       textures_gl_[textures[0]].has_alpha &&
                                       !textures_gl_[textures[0]].is_opaque_specular);
        const bool is_glass = key.blend_enable && (ma_val > 0.0f && ma_val < 1.0f) && !has_tex_alpha_up;
        if (is_glass) {
            desired_env = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
        } else if (is_audience || has_tex_alpha_up) {
            desired_env = TEX_ENV_MODULATE;
            key.blend_enable = 1;
            key.src_blend = 5; // D3DBLEND_SRCALPHA
            key.dst_blend = 6; // D3DBLEND_INVSRCALPHA
            key.alpha_test = 1;
            key.alpha_ref = 8;
            key.alpha_func = 7; // D3DCMP_GREATEREQUAL
            batch_key_ = key;
        } else if (key.blend_enable || (key.alpha_test && (key.alpha_ref & 0xffu) < 64u)) {
            desired_env = TEX_ENV_MODULATE;
        } else if (!ps || ps->kind != KPs) {
            const std::uint32_t aop = stage[0][4];
            if (aop == 1u || (aop == 2u && (stage[0][5] & 0xfu) != 2u) || (aop == 3u && (stage[0][6] & 0xfu) != 2u)) {
                desired_env = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
            }
        } else if (!cps.uses_tex0_alpha) {
            desired_env = TEX_ENV_MODULATE_DIFFUSE_ALPHA;
        }
        glDisable(GL_LIGHTING);
        glDisable(GL_NORMALIZE);
        if (tex2d_enabled_[1]) {
            bind_texture_unit(GL_TEXTURE1, 1, 0, 0, TEX_ENV_MODULATE);
        }
        bind_texture_unit(GL_TEXTURE0, 0, tex_id, textures[0], desired_env);
        glActiveTexture(GL_TEXTURE0);
        current_active_unit_ = GL_TEXTURE0;
        apply_state();
        if (!tex_matrix_identity_) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            tex_matrix_identity_ = true;
        }

        if (vs) {
            if (wvp_dirty_ || active_matrix_ != MATRIX_HW_WVP) {
                float gl_m[16];
                gl_m[0]  = vs_constants[64][0];
                gl_m[1]  = vs_constants[65][0];
                gl_m[2]  = 2.0f * vs_constants[66][0] - vs_constants[67][0];
                gl_m[3]  = vs_constants[67][0];

                gl_m[4]  = vs_constants[64][1];
                gl_m[5]  = vs_constants[65][1];
                gl_m[6]  = 2.0f * vs_constants[66][1] - vs_constants[67][1];
                gl_m[7]  = vs_constants[67][1];

                gl_m[8]  = vs_constants[64][2];
                gl_m[9]  = vs_constants[65][2];
                gl_m[10] = 2.0f * vs_constants[66][2] - vs_constants[67][2];
                gl_m[11] = vs_constants[67][2];

                gl_m[12] = vs_constants[64][3];
                gl_m[13] = vs_constants[65][3];
                gl_m[14] = 2.0f * vs_constants[66][3] - vs_constants[67][3];
                gl_m[15] = vs_constants[67][3];

                glMatrixMode(GL_PROJECTION);
                glLoadMatrixf(gl_m);
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();

                active_matrix_ = MATRIX_HW_WVP;
                wvp_dirty_ = false;
            }
        } else {
            const auto wvp = transform_wvp();
            float gl_m[16];
            gl_m[0]  = wvp[0];
            gl_m[1]  = wvp[1];
            gl_m[2]  = 2.0f * wvp[2] - wvp[3];
            gl_m[3]  = wvp[3];

            gl_m[4]  = wvp[4];
            gl_m[5]  = wvp[5];
            gl_m[6]  = 2.0f * wvp[6] - wvp[7];
            gl_m[7]  = wvp[7];

            gl_m[8]  = wvp[8];
            gl_m[9]  = wvp[9];
            gl_m[10] = 2.0f * wvp[10] - wvp[11];
            gl_m[11] = wvp[11];

            gl_m[12] = wvp[12];
            gl_m[13] = wvp[13];
            gl_m[14] = 2.0f * wvp[14] - wvp[15];
            gl_m[15] = wvp[15];

            glMatrixMode(GL_PROJECTION);
            glLoadMatrixf(gl_m);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();

            active_matrix_ = MATRIX_HW_WVP;
        }

        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        const bool tex_enabled = (sets > 0 && tex_n[0] >= 2 && tex_id);
        const bool col_enabled = (diffuse_at != none);
        set_client_states(true, tex_enabled, col_enabled);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexPointer(3, GL_FLOAT, GLsizei(stride), bytes);
        if (tex_enabled) {
            glTexCoordPointer(2, GL_FLOAT, GLsizei(stride), bytes + tex_at[0]);
        }
        if (col_enabled) {
            up_col_scratch_.resize(total);
            for (std::size_t i = 0; i < total; ++i) {
                std::uint32_t c;
                std::memcpy(&c, bytes + i * stride + diffuse_at, 4);
                up_col_scratch_[i] = (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
            }
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, up_col_scratch_.data());
            color_const_white_ = false;
        } else {
            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
            if (vs) {
                const float mr = vs_constants[18][0], mg = vs_constants[18][1], mb = vs_constants[18][2], ma = vs_constants[18][3];
                if (tex_id == 0) {
                    if (mr != 0.0f || mg != 0.0f || mb != 0.0f || ma != 0.0f) {
                        ca = std::clamp(ma, 0.0f, 1.0f);
                        cr = std::clamp(mr * 0.75f + vs_constants[19][0] * 0.25f + vs_constants[20][0], 0.0f, 1.0f);
                        cg = std::clamp(mg * 0.75f + vs_constants[19][1] * 0.25f + vs_constants[20][1], 0.0f, 1.0f);
                        cb = std::clamp(mb * 0.75f + vs_constants[19][2] * 0.25f + vs_constants[20][2], 0.0f, 1.0f);
                    } else {
                        cr = 1.0f;
                        cg = 1.0f;
                        cb = 1.0f;
                        ca = 1.0f;
                    }
                } else {
                    cr = 1.0f;
                    cg = 1.0f;
                    cb = 1.0f;
                    if (batch_key_.blend_enable && ma > 0.0f && ma < 1.0f) {
                        ca = std::clamp(ma < 0.58f ? 0.58f : ma, 0.0f, 1.0f);
                    } else {
                        ca = 1.0f;
                    }
                }
            }
            if (cr != 1.0f || cg != 1.0f || cb != 1.0f || ca != 1.0f) {
                glColor4f(cr, cg, cb, ca);
                color_const_white_ = false;
            } else if (!color_const_white_) {
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                color_const_white_ = true;
            }
        }

        glDisable(GL_NORMALIZE);
        glDisable(GL_LIGHTING);

        const std::uint32_t dt_setup = outrun::vita_profile::vita_time_us() - t_setup_start;
        outrun::vita_profile::g_sub_state_setup_us += dt_setup;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_state_setup_us_ += dt_setup;

        const std::uint32_t t_draw_start = outrun::vita_profile::vita_time_us();
        const GLenum mode = (type == 5) ? GL_TRIANGLE_STRIP : (type == 6 ? GL_TRIANGLE_FAN : GL_TRIANGLES);
        glDrawArrays(mode, 0, GLsizei(total));
        const std::uint32_t dt_draw = outrun::vita_profile::vita_time_us() - t_draw_start;

        outrun::vita_profile::g_sub_gl_draw_elements_us += dt_draw;
        if (outrun::vita_profile::g_capture_render_frame) snapshot_t_gl_draw_elements_us_ += dt_draw;

        triangles_drawn += count;
        outrun::vita_profile::g_triangles += count;
        outrun::vita_profile::accumulate_model_polygons(type, count, false);
        s_last_3d.valid = false;
        ++outrun::vita_profile::g_hw_draws;

        if (outrun::vita_profile::g_capture_render_frame) {
            ++snapshot_hw_draws_;
            const std::uint32_t dt_total = outrun::vita_profile::vita_time_us() - t_call_start;
            outrun::vita_profile::bottleneck_log(
                "[DRAW_UP #%u] %s [%s] | Mode: 3D_UP | PrimType: %u | Tris: %u | TexID: %u | Action: HW_DIRECT | Time: %u us\n",
                snapshot_draw_up_count_,
                outrun::vita_profile::get_active_model_name(),
                outrun::vita_profile::get_active_object_name(),
                type,
                count,
                tex_id,
                dt_total
            );
        }
        return;
    }

    ++outrun::vita_profile::g_sw_draws;
    if (outrun::vita_profile::g_capture_render_frame) {
        ++snapshot_sw_draws_;
    }
    if (!rhw && s_fail_print_count++ < 1) {
        std::printf("[HW_FAIL_UP] active_vs: %d, reason: %s\n", active_vs_id, fail_reason ? fail_reason : "unknown");
        std::fflush(stdout);
    }

    if (!batch_.empty() && (key != batch_key_ || batch_.size() >= 4096)) {
        flush();
    }
    batch_key_ = key;
    batch_rhw_ = rhw;

    const std::uint32_t t_sw_start = outrun::vita_profile::vita_time_us();

    const auto wvp = !vs && !rhw ? transform_wvp() : std::array<float, 16>{};

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    up_vertices_.resize(total);
    auto rdf = [](const std::uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; };
    constexpr float Inv255 = 1.f / 255.f;
    auto colour = [&](const std::uint8_t* p) -> V4 {
        return V4{ p[2] * Inv255, p[1] * Inv255, p[0] * Inv255, p[3] * Inv255 };
    };

    if (vs && vs->kind != 3) vs = nullptr; // KVs == 3
    const V4* vc = reinterpret_cast<const V4*>(vs_constants.data());
    const std::size_t normal_at = (fvf & 0x010u) ? 12u : none;

    for (std::size_t i = 0; vs && i < total; ++i) {
        const auto* p = bytes + i * stride;
        Vertex& v = up_vertices_[i];
        v = Vertex{};
        pc_shader::VsState s;
        s.C = vc;
        for (auto& r : s.V) r = {0, 0, 0, 1};
        auto input = [&](std::uint8_t usage, std::uint8_t index, const V4& value) {
            for (const auto& in : vs->program.inputs)
                if (in.usage == usage && in.usage_index == index) s.V[in.reg] = value;
        };
        input(0, 0, {rdf(p), rdf(p + 4), rdf(p + 8), 1.f});
        if (normal_at != none) input(3, 0, {rdf(p + normal_at), rdf(p + normal_at + 4), rdf(p + normal_at + 8), 1.f});
        if (diffuse_at != none) input(10, 0, colour(p + diffuse_at));
        if (specular_at != none) input(10, 1, colour(p + specular_at));
        for (unsigned k = 0; k < sets; ++k) {
            V4 t{0, 0, 0, 1};
            for (unsigned q = 0; q < tex_n[k]; ++q) t[q] = rdf(p + tex_at[k] + 4 * q);
            input(5, std::uint8_t(k), t);
        }
        pc_shader_run_vs(vs->program, s);
        std::memcpy(v.pos, &s.oPos, 16);
        v.d[0] = pc_shader::sat(s.oD[0]);
        v.t[0] = s.oT[0];
        v.fog = pc_shader::clamp01(s.oFog.x);
    }

    for (std::size_t i = 0; !vs && i < total; ++i) {
        const auto* p = bytes + i * stride;
        Vertex& v = up_vertices_[i];
        v = Vertex{};
        if (rhw) {
            for (unsigned k = 0; k < 4; ++k) v.pos[k] = rdf(p + 4 * k);
        } else {
            const float x = rdf(p), y = rdf(p + 4), z = rdf(p + 8);
            for (unsigned k = 0; k < 4; ++k) v.pos[k] = x * wvp[k] + y * wvp[4 + k] + z * wvp[8 + k] + wvp[12 + k];
        }
        v.d[0] = (diffuse_at != none) ? colour(p + diffuse_at) : V4{1, 1, 1, 1};
        v.fog = 1.f;
        if (sets > 0 && tex_n[0] > 0) {
            V4 tc{0, 0, 0, 1};
            for (unsigned c = 0; c < tex_n[0]; ++c) tc[c] = rdf(p + tex_at[0] + 4 * c);
            v.t[0] = tc;
        } else {
            v.t[0] = V4{0, 0, 0, 1};
        }
    }

    auto push_v = [&](const Vertex& s) {
        Vertex v = s;
        if (rhw) {
            v.pos[0] = (2.0f * (v.pos[0] - soft_viewport_.x) / soft_viewport_.w) - 1.0f;
            v.pos[1] = 1.0f - (2.0f * (v.pos[1] - soft_viewport_.y) / soft_viewport_.h);
            v.pos[2] = 2.0f * v.pos[2] - 1.0f;
            v.pos[3] = 1.0f;
        } else {
            v.pos[2] = 2.0f * v.pos[2] - v.pos[3];
        }
        batch_.push_back(v);
    };

    for (std::uint32_t p = 0; p < count; ++p) {
        std::size_t a, b, c;
        if (type == 4) { a = p * 3u; b = a + 1; c = a + 2; }
        else if (type == 5) { a = p; b = a + 1; c = a + 2; if (p & 1u) std::swap(a, b); }
        else { a = 0; b = p + 1; c = b + 1; }
        push_v(up_vertices_[a]);
        push_v(up_vertices_[b]);
        push_v(up_vertices_[c]);
        ++triangles_drawn;
        ++outrun::vita_profile::g_triangles;
    }
    outrun::vita_profile::accumulate_model_polygons(type, count, false);

    const std::uint32_t dt_sw = outrun::vita_profile::vita_time_us() - t_sw_start;
    outrun::vita_profile::g_sub_vertex_copy_pack_us += dt_sw;
    if (outrun::vita_profile::g_capture_render_frame) {
        snapshot_t_vertex_copy_pack_us_ += dt_sw;
        const std::uint32_t dt_total = outrun::vita_profile::vita_time_us() - t_call_start;
        outrun::vita_profile::bottleneck_log(
            "[DRAW_UP #%u] %s [%s] | Mode: 3D_UP | PrimType: %u | Tris: %u | TexID: %u | Action: SW_SHADED | Time: %u us\n",
            snapshot_draw_up_count_,
            outrun::vita_profile::get_active_model_name(),
            outrun::vita_profile::get_active_object_name(),
            type,
            count,
            textures[0],
            dt_total
        );
    }
}

} // namespace outrun::ps5_runtime
