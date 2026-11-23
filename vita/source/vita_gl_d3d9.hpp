#pragma once
#include <vitaGL.h>
#include "platform/pc_soft_d3d9.hpp"
#include <atomic>
#include <map>
#include <vector>

namespace outrun::ps5_runtime {

class VitaGlDisplay;

class VitaGlD3D9Device final : public platform::PcSoftD3D9Device {
public:
    VitaGlD3D9Device(unsigned width, unsigned height);
    ~VitaGlD3D9Device() override;

    std::uint32_t draw_indexed_primitive(std::uint32_t type, std::int32_t base,
                                         std::uint32_t min, std::uint32_t vertices,
                                         std::uint32_t start, std::uint32_t count) override;
    void draw_primitive_up(std::uint32_t type, std::uint32_t count,
                           const void* data, std::uint32_t stride) override;
    std::uint32_t create_vertex_buffer(std::uint32_t length, std::uint32_t usage, std::uint32_t fvf, std::uint32_t pool) override;
    std::uint32_t create_index_buffer(std::uint32_t length, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) override;
    std::uint32_t create_vertex_declaration(const platform::PcVertexElement* elements) override;
    std::uint32_t create_vertex_shader(const std::uint32_t* words) override;
    std::uint32_t create_pixel_shader(const std::uint32_t* words) override;
    std::uint32_t create_texture_from_file(const platform::PcTextureFileRequest& request) override;
    std::uint32_t create_texture(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) override;
    std::uint32_t create_cube_texture(std::uint32_t edge, std::uint32_t levels, std::uint32_t usage, std::uint32_t format, std::uint32_t pool) override;
    std::uint32_t create_depth_stencil_surface(std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t a, std::uint32_t b, std::uint32_t c) override;
    std::uint32_t get_cube_map_surface(std::uint32_t cube, std::uint32_t face, std::uint32_t level) override;
    std::uint32_t get_surface_level(std::uint32_t texture, std::uint32_t level) override;
    std::uint8_t* lock(std::uint32_t buffer, std::uint32_t offset, std::uint32_t size, std::uint32_t flags) override;
    void unlock(std::uint32_t buffer) override;
    std::uint32_t buffer_size(std::uint32_t buffer) override;
    void clear(std::uint32_t flags, std::uint32_t colour, float z, std::uint32_t stencil) override;
    void unlock_rect(std::uint32_t handle) override;
    void release(std::uint32_t handle) override;
    void begin_frame();
    void end_frame();
    void begin_async_frame();
    void submit_async_frame(VitaGlDisplay* display);
    bool preserve_color_on_clear = false;

    void wait_async_idle();

    void set_render_state(std::uint32_t state, std::uint32_t value) override;
    void set_texture(std::uint32_t stage, std::uint32_t texture) override;
    void set_sampler_state(std::uint32_t stage, std::uint32_t type, std::uint32_t value) override;
    void set_texture_stage_state(std::uint32_t stage, std::uint32_t type, std::uint32_t value) override;
    void set_fvf(std::uint32_t fvf) override;
    void set_vertex_shader_constant_f(std::uint32_t start, const float* data, std::uint32_t vec4_count) override;
    void set_transform(std::uint32_t state, const float matrix[16]) override;
    void set_stream_source(std::uint32_t stream, std::uint32_t buffer, std::uint32_t offset, std::uint32_t stride) override;
    void set_indices(std::uint32_t buffer) override;
    void set_vertex_declaration(std::uint32_t declaration) override;
    void set_vertex_shader(std::uint32_t shader) override;
    void set_pixel_shader(std::uint32_t shader) override;
    void set_render_target(std::uint32_t index, std::uint32_t surface) override;
    void set_depth_stencil_surface(std::uint32_t surface) override;
    void set_viewport(const std::uint32_t v[6]) override;

    void resize_back_buffer(unsigned w, unsigned h);
    void set_ui_rect(bool on) { (void)on; ui_rect_ = false; }
    void set_msaa(unsigned samples) { msaa_ = samples; }
    void finish_scene();
    std::uint64_t triangles_drawn{0};
    std::uint64_t submitted_draws{0};
    unsigned back_texture() const { return 0; }
    std::uint64_t gpu_nanoseconds() const { return 0; }

    bool has_3d_draws() const { return has_3d_draws_; }
    std::vector<std::uint32_t>& released_textures() { return released_textures_; }

private:
    enum TexEnvMode {
        TEX_ENV_UNKNOWN = -1,
        TEX_ENV_MODULATE = 0,
        TEX_ENV_MODULATE_DIFFUSE_ALPHA,
        TEX_ENV_REPLACE_RGB_DIFFUSE_ALPHA,
        TEX_ENV_REPLACE_RGBA,
        TEX_ENV_DECAL_DIFFUSE_ALPHA,
        TEX_ENV_DIFFUSE_ONLY,
        TEX_ENV_MULTITEX_MODULATE
    };

    void flush();
    void apply_state();
    void bind_texture(unsigned tex_id, std::uint32_t handle = 0, TexEnvMode desired_env = TEX_ENV_MODULATE);
    void bind_texture_unit(GLenum unit_enum, unsigned unit_idx, unsigned tex_id, std::uint32_t handle, TexEnvMode desired_env);
    void set_client_states(bool vtx, bool tex, bool col);
    void set_client_states(bool vtx, bool tex0, bool tex1, bool col);
    unsigned texture(std::uint32_t handle);

    unsigned last_bound_texture_[2]{0, 0};
    bool tex2d_enabled_[2]{false, false};
    TexEnvMode tex_env_mode_[2]{TEX_ENV_UNKNOWN, TEX_ENV_UNKNOWN};
    GLenum current_active_unit_{GL_TEXTURE0};
    bool tex_matrix_identity_{true};
    bool client_vertex_enabled_{false};
    bool client_texcoord_enabled_[2]{false, false};
    bool client_color_enabled_{false};
    bool color_const_white_{false};
    int last_vp_x_{-1}, last_vp_y_{-1}, last_vp_w_{-1}, last_vp_h_{-1};
    float last_min_z_{-1.0f}, last_max_z_{-1.0f};
    bool state_dirty_{true};

public:
    struct BatchKey {
        std::uint32_t tex{0};
        bool rhw{false};
        std::uint32_t blend_enable{0};
        std::uint32_t src_blend{0};
        std::uint32_t dst_blend{0};
        std::uint32_t alpha_test{0};
        std::uint32_t alpha_func{0};
        std::uint32_t alpha_ref{0};
        std::uint32_t z_enable{0};
        std::uint32_t z_write{0};
        std::uint32_t z_func{4};
        std::uint32_t cull{0};
        std::uint32_t color_mask{0xfu};
        int vpx{0}, vpy{0}, vpw{0}, vph{0};
        bool operator==(const BatchKey& o) const {
            return tex == o.tex && rhw == o.rhw && blend_enable == o.blend_enable &&
                   src_blend == o.src_blend && dst_blend == o.dst_blend &&
                   alpha_test == o.alpha_test && alpha_func == o.alpha_func &&
                   alpha_ref == o.alpha_ref && z_enable == o.z_enable &&
                   z_write == o.z_write && z_func == o.z_func && cull == o.cull &&
                   color_mask == o.color_mask &&
                   vpx == o.vpx && vpy == o.vpy && vpw == o.vpw && vph == o.vph;
        }
        bool operator!=(const BatchKey& o) const { return !(*this == o); }
    };
private:
    BatchKey batch_key_{};
    BatchKey current_applied_state_{};

    std::vector<Vertex> batch_;
    struct GlTex {
        unsigned id{0};
        unsigned target{GL_TEXTURE_2D};
        int min_filter{-1};
        int mag_filter{-1};
        int wrap_s{-1};
        int wrap_t{-1};
        bool has_data{false};
        bool is_cube{false};
        bool is_npot{false};
        bool is_opaque_specular{false};
        bool has_alpha{false};
        bool is_cropped_decal{false};
        std::uint32_t w{0}, h{0};
    };
    std::vector<GlTex> textures_gl_;

    struct CachedDecl {
        bool valid{false};
        const platform::PcVertexElement* pos{nullptr};
        const platform::PcVertexElement* weight{nullptr};
        const platform::PcVertexElement* indices{nullptr};
        const platform::PcVertexElement* tex[4]{nullptr, nullptr, nullptr, nullptr};
        const platform::PcVertexElement* col{nullptr};
        const platform::PcVertexElement* norm{nullptr};
    };
    struct CachedVs {
        bool valid{false};
        bool is_rigid{false};
        std::uint8_t skin_bones{0};
        std::uint8_t tex_mat_mode{0};
        bool has_texgen{false};
        bool has_envmap{false};
    };
    struct CachedPs {
        bool valid{false};
        bool uses_tex0_alpha{true};
        bool ignore_diffuse_rgb{false};
        bool is_decal_rgb{false};
        bool ignore_tex0_rgb{false};
        bool is_reflection{false};
        bool uses_stage1_mul{false};
    };
    struct VbColCache {
        std::uint32_t col_offset{0};
        std::uint32_t stride{0};
        std::vector<std::uint8_t> rgba;
    };
    const CachedDecl& get_cached_decl(std::uint32_t handle, const platform::PcVertexElement* elements);
    const CachedVs& get_cached_vs(std::uint32_t handle, const Object* vs);
    const CachedPs& get_cached_ps(std::uint32_t handle, const Object* ps);
    const std::uint8_t* get_vb_rgba_ptr(std::uint32_t buf_handle, const Object* vb,
                                        std::uint32_t stream_offset, std::uint32_t stride,
                                        std::uint32_t col_offset, std::int32_t base,
                                        std::uint32_t max_vert_needed);

    std::vector<CachedDecl> decl_cache_;
    std::vector<CachedVs> vs_cache_;
    std::vector<CachedPs> ps_cache_;
    std::vector<VbColCache> vb_col_cache_;
    std::size_t vb_col_cache_bytes_{0};

    struct CdramBuffer {
        GLuint vbo{0};
        void* vram_ptr{nullptr};
        std::size_t size{0};
    };
    std::vector<CdramBuffer> cdram_buffers_;
    std::vector<std::uint32_t> up_col_scratch_;
    std::vector<float> sw_skin_pos_;
    std::vector<float> sw_skin_norm_;
    unsigned back_w_{960}, back_h_{544};
    unsigned logical_width_{640}, logical_height_{480};
    unsigned msaa_{1};
    bool ui_rect_{false};
    bool batch_rhw_{false};
    bool scale_zero_{false};
    bool has_3d_draws_{false};
    std::vector<std::uint32_t> released_textures_;

    std::uint32_t vertex_cache_gen_{0};
    std::vector<std::uint32_t> vertex_cache_tag_;
    std::vector<Vertex> vertex_cache_;

    enum MatrixMode { MATRIX_IDENTITY, MATRIX_HW_WVP, MATRIX_2D_ORTHO };
    MatrixMode active_matrix_{MATRIX_IDENTITY};
    bool wvp_dirty_{true};

    struct Vertex2D {
        float x{0}, y{0}, z{0};
        std::uint32_t color{0xFFFFFFFFu};
        float u{0}, v{0};
    };

    enum FlushReason {
        FLUSH_REASON_TEX_CHANGE = 0,
        FLUSH_REASON_BLEND_CHANGE,
        FLUSH_REASON_BUFFER_FULL,
        FLUSH_REASON_3D_TRANSITION,
        FLUSH_REASON_END_SCENE,
        FLUSH_REASON_COUNT
    };
    static const char* flush_reason_str(FlushReason r) {
        switch (r) {
            case FLUSH_REASON_TEX_CHANGE: return "TEX_CHANGE";
            case FLUSH_REASON_BLEND_CHANGE: return "BLEND_CHANGE";
            case FLUSH_REASON_BUFFER_FULL: return "BUFFER_FULL";
            case FLUSH_REASON_3D_TRANSITION: return "3D_TRANSITION";
            case FLUSH_REASON_END_SCENE: return "END_SCENE";
            default: return "UNKNOWN";
        }
    }
    void flush_2d(FlushReason = FLUSH_REASON_END_SCENE, unsigned = 0) {}
    std::uint32_t last_queried_handle_{0};
    unsigned last_queried_id_{0};

    std::uint32_t applied_2d_blend_key_{0xFFFFFFFFu};
    unsigned applied_2d_tex_{0xFFFFFFFFu};
    int applied_2d_vx_{-1}, applied_2d_vy_{-1}, applied_2d_vw_{-1}, applied_2d_vh_{-1};

    std::uint32_t snapshot_draw_count_{0};
    std::uint32_t snapshot_hw_draws_{0};
    std::uint32_t snapshot_sw_draws_{0};
    std::uint32_t snapshot_flush_count_{0};
    std::uint32_t snapshot_flush_reasons_[FLUSH_REASON_COUNT]{0};
    std::uint32_t snapshot_draw_3d_count_{0};
    std::uint32_t snapshot_draw_up_count_{0};
    std::uint32_t snapshot_t_tex_lookup_bind_us_{0};
    std::uint32_t snapshot_t_state_setup_us_{0};
    std::uint32_t snapshot_t_gl_draw_elements_us_{0};
    std::uint32_t snapshot_t_gl_draw_arrays_2d_us_{0};
    std::uint32_t snapshot_t_vertex_copy_pack_us_{0};

    // --- Core 0 vitaGL Render Thread: Double-Buffered Delta Command Stream ---
    struct ReplayState {
        std::array<std::uint32_t, 256> render{};
        std::array<std::array<std::uint32_t, 14>, 16> sampler{};
        std::array<std::array<std::uint32_t, 33>, 8> stage{};
        std::array<float, 16> transform_world{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::array<float, 16> transform_view{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::array<float, 16> transform_proj{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::array<std::uint32_t, 16> textures{};
        std::array<Stream, 4> streams{};
        std::uint32_t indices{}, declaration{}, vertex_shader{}, pixel_shader{}, fvf{};
        std::array<std::array<float, 4>, 128> vs_constants{};
        std::array<float, 16> transform_wvp() const {
            auto mul = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
                std::array<float, 16> r{};
                for (unsigned i = 0; i < 4; ++i) {
                    const float a0 = a[i*4+0], a1 = a[i*4+1], a2 = a[i*4+2], a3 = a[i*4+3];
                    r[i*4+0] = a0*b[0] + a1*b[4] + a2*b[8]  + a3*b[12];
                    r[i*4+1] = a0*b[1] + a1*b[5] + a2*b[9]  + a3*b[13];
                    r[i*4+2] = a0*b[2] + a1*b[6] + a2*b[10] + a3*b[14];
                    r[i*4+3] = a0*b[3] + a1*b[7] + a2*b[11] + a3*b[15];
                }
                return r;
            };
            return mul(mul(transform_world, transform_view), transform_proj);
        }
    };

    enum class GlCmdKind : std::uint8_t {
        SetRS, SetTex, SetSampler, SetStage, SetFvf, SetStream, SetIndices,
        SetDecl, SetVS, SetPS, SetTransform, SetVsc, Clear, DrawIndexed, DrawUp
    };

    struct GlCmd {
        GlCmdKind kind{GlCmdKind::SetRS};
        std::uint32_t u0{0}, u1{0}, u2{0}, u3{0}, u4{0}, u5{0};
        std::int32_t i0{0};
        float f0{0.0f};
        std::uint32_t res_id{0xFFFFFFFFu}, obj_id{0};
    };

    struct AsyncFrameSlot {
        std::atomic<bool> busy{false};
        VitaGlD3D9Device* self{nullptr};
        VitaGlDisplay* display{nullptr};
        ReplayState initial_state{};
        std::vector<GlCmd> cmds;
        std::vector<float> floats;
        std::vector<std::uint8_t> up_bytes;
        void reset() {
            cmds.clear();
            floats.clear();
            up_bytes.clear();
        }
    };

    AsyncFrameSlot async_slots_[2];
    int write_slot_{0};
    bool async_recording_{false};
    bool replaying_async_{false};
    ReplayState replay_{};
    GLuint last_ibo_{0};

    void snapshot_live_state(ReplayState& dst) const;
    void begin_frame_gl();
    void end_frame_gl();
    void clear_gl(std::uint32_t flags, std::uint32_t colour, float z, std::uint32_t stencil);
    std::uint32_t draw_indexed_primitive_gl(const ReplayState& st, std::uint32_t type, std::int32_t base,
                                            std::uint32_t min, std::uint32_t vertices,
                                            std::uint32_t start, std::uint32_t count);
    void draw_primitive_up_gl(const ReplayState& st, std::uint32_t type, std::uint32_t count,
                              const void* data, std::uint32_t stride);
    static void execute_async_slot_thunk(void* arg);
    void execute_async_slot(AsyncFrameSlot& slot);
};

} // namespace outrun::ps5_runtime
