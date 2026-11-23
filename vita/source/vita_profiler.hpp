#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include <psp2/kernel/processmgr.h>
#endif

namespace outrun::vita_profile {

// Compile-time default for SD log output (0 = off by default; override at runtime via options.ini sd_logs=1).
#ifndef OR2_VITA_SD_LOGS_DEFAULT
#define OR2_VITA_SD_LOGS_DEFAULT 0
#endif
extern bool g_sd_logs_enabled;

// Metric accumulators (summed over 60 frames)
extern std::uint64_t g_walk_time_us;
extern std::uint32_t g_draw_calls;
extern std::uint32_t g_hw_draws;
extern std::uint32_t g_sw_draws;
extern std::uint32_t g_flushes;
extern std::uint32_t g_texture_binds;
extern std::uint64_t g_triangles;
extern char g_last_hw_fail[128];

// Special Resource IDs for non-PMT draws
inline constexpr std::uint32_t RES_ID_NONE          = 0xFFFFFFFFu;
inline constexpr std::uint32_t RES_ID_2D_HUD        = 0xF0000001u;
inline constexpr std::uint32_t RES_ID_FLAGMAN       = 0xF0000002u;
inline constexpr std::uint32_t RES_ID_SHADOW_VOL    = 0xF0000003u;
inline constexpr std::uint32_t RES_ID_HEADLIGHT     = 0xF0000004u;
inline constexpr std::uint32_t RES_ID_PARTICLES     = 0xF0000005u;
inline constexpr std::uint32_t RES_ID_SUN_OCCLUSION = 0xF0000006u;
inline constexpr std::uint32_t RES_ID_OTHER_3D      = 0xF0000007u;

void set_active_model(std::uint32_t resource_id, std::uint32_t object_id);
void get_active_model(std::uint32_t& resource_id, std::uint32_t& object_id);
void accumulate_model_polygons(std::uint32_t primitive_type, std::uint32_t count, bool is_skinned);
const char* get_active_model_name();
const char* get_active_object_name();
void get_resource_name(std::uint32_t res_id, char* out_buf, std::size_t buf_len);
void dump_model_polygon_log();

// Sub-stage timers (summed over 60 frames, in us)
extern std::uint64_t g_sub_tex_lookup_bind_us;
extern std::uint64_t g_sub_state_setup_us;
extern std::uint64_t g_sub_gl_draw_elements_us;
extern std::uint64_t g_sub_gl_draw_arrays_2d_us;
extern std::uint64_t g_sub_vertex_copy_pack_us;
extern std::uint64_t g_sub_gl_clear_us;
extern std::uint64_t g_sub_dip_total_us;
extern std::uint64_t g_sub_pc_flush_us;
extern std::uint64_t g_sub_display_cb_us;

// Per-core & GPU thread-safe accumulators (summed over 60 frames, in us)
extern std::atomic<std::uint64_t> g_core0_busy_us;
extern std::atomic<std::uint64_t> g_core0_swap_us;
extern std::atomic<std::uint64_t> g_core2_busy_us;

// Snapshot capture control
extern bool g_capture_render_frame;
extern bool g_capture_display_cb;
// Captured frame: every callback (no threshold) summed per pc; dumped once at frame end.
void cb_accum(char kind, std::uint32_t pc, std::uint32_t us);
void cb_dump();

void set_current_zone(const std::string& zone);
void set_current_zone(const char* zone);
const std::string& get_current_zone();

void archive_old_logs();
void shutdown_profiler();
void init_bottleneck_log();
void bottleneck_log(const char* fmt, ...);
void bottleneck_flush();

static inline std::uint32_t vita_time_us() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    return sceKernelGetProcessTimeLow();
#else
    return 0;
#endif
}

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
// RAII scope to measure walk_404d10 recursively without double counting
struct WalkScope {
    SceUInt64 t0{0};
    bool active{false};
    static inline int depth{0};

    WalkScope() {
        if (depth == 0) {
            t0 = sceKernelGetProcessTimeWide();
            active = true;
        }
        depth++;
    }

    ~WalkScope() {
        depth--;
        if (active) {
            g_walk_time_us += (sceKernelGetProcessTimeWide() - t0);
        }
    }
};

// RAII scope to measure PcRenderQueue flush overhead excluding child walk (Core 1 only)
struct FlushScope {
    SceUInt64 t0{0};
    std::uint64_t walk0{0};
    bool active{false};
    static inline int depth{0};

    FlushScope() {
        if (depth == 0) {
            t0 = sceKernelGetProcessTimeWide();
            walk0 = g_walk_time_us;
            active = true;
        }
        depth++;
    }

    ~FlushScope() {
        depth--;
        if (active) {
            const SceUInt64 dt = sceKernelGetProcessTimeWide() - t0;
            const std::uint64_t child = (g_walk_time_us - walk0);
            if (dt > child) g_sub_pc_flush_us += (dt - child);
        }
    }
};

// RAII scope to measure display callbacks excluding child walk/flush (Core 1 only)
struct DisplayCbScope {
    SceUInt64 t0{0};
    std::uint64_t walk0{0}, flush0{0};
    bool active{false};
    static inline int depth{0};

    DisplayCbScope() {
        if (depth == 0) {
            t0 = sceKernelGetProcessTimeWide();
            walk0 = g_walk_time_us;
            flush0 = g_sub_pc_flush_us;
            active = true;
        }
        depth++;
    }

    ~DisplayCbScope() {
        depth--;
        if (active) {
            const SceUInt64 dt = sceKernelGetProcessTimeWide() - t0;
            const std::uint64_t child = (g_walk_time_us - walk0)
                                      + (g_sub_pc_flush_us - flush0);
            if (dt > child) g_sub_display_cb_us += (dt - child);
        }
    }
};
#endif

// Record per-frame timings and print profile summary every 60 frames
void record_frame(double logic_ms, double walk_ms, double draw_sub_ms, double ui_2d_ms, double swap_ms);
void log_message(const char* fmt, ...);

} // namespace outrun::vita_profile
