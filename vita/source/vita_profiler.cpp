#include "vita_profiler.hpp"
#include "system/exe_image.hpp"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <mutex>

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include <malloc.h>
#include <strings.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#endif

namespace outrun::vita_profile {

bool g_sd_logs_enabled = (OR2_VITA_SD_LOGS_DEFAULT != 0);

std::uint64_t g_walk_time_us = 0;
std::uint32_t g_draw_calls = 0;
std::uint32_t g_hw_draws = 0;
std::uint32_t g_sw_draws = 0;
std::uint32_t g_flushes = 0;
std::uint32_t g_texture_binds = 0;
std::uint64_t g_triangles = 0;
char g_last_hw_fail[128] = "none";

std::uint64_t g_sub_tex_lookup_bind_us = 0;
std::uint64_t g_sub_state_setup_us = 0;
std::uint64_t g_sub_gl_draw_elements_us = 0;
std::uint64_t g_sub_gl_draw_arrays_2d_us = 0;
std::uint64_t g_sub_vertex_copy_pack_us = 0;
std::uint64_t g_sub_gl_clear_us = 0;
std::uint64_t g_sub_dip_total_us = 0;
std::uint64_t g_sub_pc_flush_us = 0;
std::uint64_t g_sub_display_cb_us = 0;
std::atomic<std::uint64_t> g_core0_busy_us{0};
std::atomic<std::uint64_t> g_core0_swap_us{0};
std::atomic<std::uint64_t> g_core2_busy_us{0};

bool g_capture_render_frame = false;
bool g_capture_display_cb = false;
namespace {
struct CbAcc { char kind; std::uint32_t pc, us, n; };
CbAcc s_cb[96];
unsigned s_cb_count = 0;
}
void cb_accum(char kind, std::uint32_t pc, std::uint32_t us) {
    if (!g_sd_logs_enabled) return;
    for (unsigned i = 0; i < s_cb_count; ++i)
        if (s_cb[i].pc == pc && s_cb[i].kind == kind) { s_cb[i].us += us; ++s_cb[i].n; return; }
    if (s_cb_count < 96u) s_cb[s_cb_count++] = {kind, pc, us, 1u};
}
void cb_dump() {
    if (!g_sd_logs_enabled || !s_cb_count) return;
    std::sort(s_cb, s_cb + s_cb_count, [](const CbAcc& a, const CbAcc& b) { return a.us > b.us; });
    std::uint32_t tl = 0, td = 0;
    for (unsigned i = 0; i < s_cb_count; ++i) (s_cb[i].kind == 'L' ? tl : td) += s_cb[i].us;
    bottleneck_log("[CB_SUM] logic=%u us display+leaf=%u us\n", unsigned(tl), unsigned(td));
    for (unsigned i = 0; i < s_cb_count && i < 18u; ++i)
        bottleneck_log("[CB_%c] pc: 0x%08x | time: %u us | calls: %u\n", s_cb[i].kind, unsigned(s_cb[i].pc), unsigned(s_cb[i].us), unsigned(s_cb[i].n));
    s_cb_count = 0;
}

namespace {

static std::string s_current_zone = "Unknown";
static std::map<std::string, unsigned> s_zone_counts;
static FILE* s_bottleneck_file = nullptr;
static char s_bottleneck_buffer[65536];
static std::string s_bottleneck_log_path;
static std::mutex s_bottleneck_mutex;
static std::mutex s_log_mutex;

static std::string format_with_commas(std::uint64_t val) {
    std::string s = std::to_string(val);
    int n = static_cast<int>(s.length()) - 3;
    while (n > 0) {
        s.insert(n, ",");
        n -= 3;
    }
    return s;
}

static std::string s_timestamped_log_path;
static std::string s_session_timestamp;
static FILE* s_persistent_log_file = nullptr;

static void ensure_session_timestamp() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (s_session_timestamp.empty()) {
        SceDateTime dt{};
        if (sceRtcGetCurrentClockLocalTime(&dt) >= 0) {
            char name[64];
            std::snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u-%02u-%02u",
                          dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
            s_session_timestamp = name;
        } else {
            s_session_timestamp = "session";
        }
    }
#endif
}

} // namespace

void archive_old_logs() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_sd_logs_enabled) return;
    static bool s_logs_archived = false;
    if (s_logs_archived) return;
    s_logs_archived = true;

    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/outrun2006", 0777);
    sceIoMkdir("ux0:data/outrun2006/logs", 0777);

    // Eliminar logs residuales sin timestamp
    sceIoRemove("ux0:/profile.log");
    sceIoRemove("ux0:/render_bottleneck.log");
    sceIoRemove("ux0:/model_polygons.log");
    sceIoRemove("ux0:model_polygons.log");
    sceIoRemove("ux0:data/outrun2006/profile.log");
    sceIoRemove("ux0:data/outrun2006/render_bottleneck.log");
    sceIoRemove("ux0:data/outrun2006/model_polygons.log");

    // 1. Limpiar cualquier log que haya quedado en la raiz ux0:/ y moverlo a ux0:data/outrun2006/logs/
    SceUID root_dfd = sceIoDopen("ux0:/");
    if (root_dfd >= 0) {
        SceIoDirent entry{};
        while (sceIoDread(root_dfd, &entry) > 0) {
            if (!SCE_S_ISREG(entry.d_stat.st_mode)) continue;
            const char* name = entry.d_name;
            const bool is_our_log = (std::strncmp(name, "profile_", 8) == 0 ||
                                     std::strncmp(name, "render_bottleneck_", 18) == 0 ||
                                     std::strncmp(name, "model_polygons_", 15) == 0);
            if (!is_our_log) continue;

            char old_path[256];
            char new_path[256];
            std::snprintf(old_path, sizeof(old_path), "ux0:/%s", name);
            std::snprintf(new_path, sizeof(new_path), "ux0:data/outrun2006/logs/%s", name);
            sceIoRemove(new_path);
            if (sceIoRename(old_path, new_path) < 0) {
                sceIoRemove(old_path);
            }
        }
        sceIoDclose(root_dfd);
    }

    // 2. Mover logs de sesiones anteriores de ux0:data/outrun2006/ a ux0:data/outrun2006/logs/
    SceUID dfd = sceIoDopen("ux0:data/outrun2006");
    if (dfd >= 0) {
        SceIoDirent entry{};
        while (sceIoDread(dfd, &entry) > 0) {
            if (!SCE_S_ISREG(entry.d_stat.st_mode)) continue;
            const char* name = entry.d_name;
            const size_t len = std::strlen(name);
            bool is_log = false;
            if (len >= 4 && (name[len-4] == '.' &&
                (name[len-3] == 'l' || name[len-3] == 'L') &&
                (name[len-2] == 'o' || name[len-2] == 'O') &&
                (name[len-1] == 'g' || name[len-1] == 'G'))) {
                is_log = true;
            } else if (strcmp(name, "crash.txt") == 0) {
                is_log = true;
            }
            if (!is_log) continue;

            char old_path[256];
            char new_path[256];
            std::snprintf(old_path, sizeof(old_path), "ux0:data/outrun2006/%s", name);

            bool has_timestamp = (std::strstr(name, "_202") != nullptr);
            if (has_timestamp) {
                std::snprintf(new_path, sizeof(new_path), "ux0:data/outrun2006/logs/%s", name);
                sceIoRemove(new_path);
                sceIoRename(old_path, new_path);
            } else {
                // Eliminar cualquier log sin timestamp
                sceIoRemove(old_path);
            }
        }
        sceIoDclose(dfd);
    }
#endif
}

namespace {

static void ensure_log_dir() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_sd_logs_enabled) return;
    archive_old_logs();
    ensure_session_timestamp();

    if (!s_persistent_log_file) {
        s_timestamped_log_path = "ux0:data/outrun2006/profile_" + s_session_timestamp + ".log";
        s_persistent_log_file = std::fopen(s_timestamped_log_path.c_str(), "w");
        if (s_persistent_log_file) {
            std::setvbuf(s_persistent_log_file, nullptr, _IOFBF, 16384);
            std::fprintf(s_persistent_log_file, "=== OutRun 2006 PS Vita - Telemetria [%s] (Build: %s %s) ===\n\n",
                         s_timestamped_log_path.c_str(), __DATE__, __TIME__);
        }
    }
#endif
}

} // namespace

void set_current_zone(const std::string& zone) {
    s_current_zone = zone;
}

void set_current_zone(const char* zone) {
    if (zone) s_current_zone = zone;
}

const std::string& get_current_zone() {
    return s_current_zone;
}

static void init_bottleneck_log_locked() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_sd_logs_enabled || s_bottleneck_file) return;
    {
        std::lock_guard<std::mutex> log_lk(s_log_mutex);
        ensure_log_dir();
        ensure_session_timestamp();
    }
    s_bottleneck_log_path = "ux0:data/outrun2006/render_bottleneck_" + s_session_timestamp + ".log";
    s_bottleneck_file = std::fopen(s_bottleneck_log_path.c_str(), "w");
    if (s_bottleneck_file) {
        std::setvbuf(s_bottleneck_file, s_bottleneck_buffer, _IOFBF, sizeof(s_bottleneck_buffer));
        std::fprintf(s_bottleneck_file, "=== OutRun 2006 PS Vita - Render Bottleneck Log [%s] (Build: %s %s) ===\n\n",
                     s_bottleneck_log_path.c_str(), __DATE__, __TIME__);
        std::fflush(s_bottleneck_file);
    }
#endif
}

void init_bottleneck_log() {
    std::lock_guard<std::mutex> lk(s_bottleneck_mutex);
    init_bottleneck_log_locked();
}

void bottleneck_log(const char* fmt, ...) {
    if (!g_sd_logs_enabled) return;
    std::lock_guard<std::mutex> lk(s_bottleneck_mutex);
    init_bottleneck_log_locked();
    if (!s_bottleneck_file) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(s_bottleneck_file, fmt, args);
    va_end(args);
}

void bottleneck_flush() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_sd_logs_enabled) return;
    std::lock_guard<std::mutex> lk(s_bottleneck_mutex);
    if (s_bottleneck_file) {
        std::fflush(s_bottleneck_file);
    }
#endif
}

void log_message(const char* fmt, ...) {
    if (!g_sd_logs_enabled) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lk(s_log_mutex);
    ensure_log_dir();
    // Buffered I/O only: avoid synchronous fflush on every [TRACK_LOAD] line in Core 1.
    std::fputs(buf, stdout);
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (s_persistent_log_file) { std::fputs(buf, s_persistent_log_file); }
#endif
}

// -----------------------------------------------------------------------------
// Model & Polygon Tracking Implementation (Zero-allocation during draws)
// -----------------------------------------------------------------------------

static thread_local std::uint32_t s_active_res_id = RES_ID_NONE;
static thread_local std::uint32_t s_active_obj_id = 0;

void set_active_model(std::uint32_t resource_id, std::uint32_t object_id) {
    s_active_res_id = resource_id;
    s_active_obj_id = object_id;
}

void get_active_model(std::uint32_t& resource_id, std::uint32_t& object_id) {
    resource_id = s_active_res_id;
    object_id = s_active_obj_id;
}

struct SubObjectStats {
    std::uint32_t object_id;
    std::uint32_t triangles;
    std::uint32_t draws;
    std::uint32_t skinned_draws;
};

struct ModelDrawSlot {
    std::uint32_t resource_id;
    std::uint32_t total_triangles;
    std::uint32_t total_draws;
    std::uint32_t sub_count;
    static constexpr std::size_t MAX_SUBS = 64;
    SubObjectStats subs[MAX_SUBS];
};

static constexpr std::size_t MAX_MODEL_SLOTS = 128;
static ModelDrawSlot s_model_slots[MAX_MODEL_SLOTS];
static std::size_t s_model_slot_count = 0;
static unsigned s_model_sample_frames = 0;
static std::map<std::string, std::string> s_zone_polygon_reports;

static void reset_model_polygon_accumulators() {
    s_model_slot_count = 0;
    s_model_sample_frames = 0;
    std::memset(s_model_slots, 0, sizeof(s_model_slots));
}

void accumulate_model_polygons(std::uint32_t primitive_type, std::uint32_t count, bool is_skinned) {
#if !defined(OR2_VITA_HEAVY_PROFILE)
    (void)primitive_type; (void)count; (void)is_skinned;
    return;
#else
    (void)primitive_type;
    if (count == 0) return;

    const std::uint32_t res = s_active_res_id;
    const std::uint32_t obj = s_active_obj_id;

    std::size_t slot_idx = MAX_MODEL_SLOTS;
    for (std::size_t i = 0; i < s_model_slot_count; ++i) {
        if (s_model_slots[i].resource_id == res) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == MAX_MODEL_SLOTS) {
        if (s_model_slot_count < MAX_MODEL_SLOTS) {
            slot_idx = s_model_slot_count++;
            s_model_slots[slot_idx].resource_id = res;
            s_model_slots[slot_idx].total_triangles = 0;
            s_model_slots[slot_idx].total_draws = 0;
            s_model_slots[slot_idx].sub_count = 0;
        } else {
            return;
        }
    }

    auto& slot = s_model_slots[slot_idx];
    slot.total_triangles += count;
    slot.total_draws += 1;

    std::size_t sub_idx = ModelDrawSlot::MAX_SUBS;
    for (std::size_t i = 0; i < slot.sub_count; ++i) {
        if (slot.subs[i].object_id == obj) {
            sub_idx = i;
            break;
        }
    }
    if (sub_idx == ModelDrawSlot::MAX_SUBS) {
        if (slot.sub_count < ModelDrawSlot::MAX_SUBS) {
            sub_idx = slot.sub_count++;
            slot.subs[sub_idx].object_id = obj;
            slot.subs[sub_idx].triangles = 0;
            slot.subs[sub_idx].draws = 0;
            slot.subs[sub_idx].skinned_draws = 0;
        } else {
            return;
        }
    }

    auto& sub = slot.subs[sub_idx];
    sub.triangles += count;
    sub.draws += 1;
    if (is_skinned) {
        sub.skinned_draws += 1;
    }
#endif
}

enum class ObjectCategory {
    VisibleScenery,
    VisibleBody,
    VisibleInterior,
    VisibleSteering,
    VisibleWheels,
    VisibleMirrors,
    VisibleHeadlight,
    VisibleDriver,
    VisibleParticles,
    VisibleHud2D,
    HiddenShadowVolume,
    HiddenShadowPlane,
    HiddenHeadlightAlt,
    HiddenLightVolume,
    HiddenOcclusionQuery,
    Other
};

static inline bool safe_read_u32(std::uint32_t va, std::uint32_t& out_val) {
    if (!outrun::platform::exe_image_loaded()) return false;
    try {
        out_val = outrun::platform::exe_image_u32(va);
        return true;
    } catch (...) {
        return false;
    }
}

static inline bool safe_read_string(std::uint32_t va, char* out_buf, size_t buf_len) {
    if (!outrun::platform::exe_image_loaded() || !out_buf || buf_len == 0) return false;
    try {
        size_t i = 0;
        for (; i + 1 < buf_len && i < 256; ++i) {
            char ch = static_cast<char>(*outrun::platform::exe_image_bytes(va + i, 1));
            if (!ch) break;
            out_buf[i] = ch;
        }
        out_buf[i] = '\0';
        return true;
    } catch (...) {
        out_buf[0] = '\0';
        return false;
    }
}

static void resolve_resource_path(std::uint32_t res_id, char* out_buf, size_t buf_len) {
    if (!out_buf || buf_len == 0) return;
    out_buf[0] = '\0';

    switch (res_id) {
        case RES_ID_2D_HUD:
            std::snprintf(out_buf, buf_len, "[2D HUD / Sprites / Overlay]");
            return;
        case RES_ID_FLAGMAN:
            std::snprintf(out_buf, buf_len, "[CHR: Flagman Bandera (rob_flag.cpp)]");
            return;
        case RES_ID_SHADOW_VOL:
            std::snprintf(out_buf, buf_len, "[SHADOW: Stencil Darken Volume (pc_shadow_volume.cpp)]");
            return;
        case RES_ID_HEADLIGHT:
            std::snprintf(out_buf, buf_len, "[LIGHT: Cono Faros (pc_headlight_pool.cpp)]");
            return;
        case RES_ID_PARTICLES:
            std::snprintf(out_buf, buf_len, "[FX: Particulas / Humo / Marcas]");
            return;
        case RES_ID_SUN_OCCLUSION:
            std::snprintf(out_buf, buf_len, "[OCCLUSION: Query Sol (race_course_passes.cpp)]");
            return;
        case RES_ID_OTHER_3D:
            std::snprintf(out_buf, buf_len, "[OTRA GEOMETRIA 3D]");
            return;
        case RES_ID_NONE:
            std::snprintf(out_buf, buf_len, "[DESCONOCIDO / NONE]");
            return;
        default:
            break;
    }

    std::uint32_t token = 0;
    if (res_id < 0x400 && safe_read_u32(0x633558u + res_id * 4u, token) && token != 0) {
        if (safe_read_string(token, out_buf, buf_len) && out_buf[0] != '\0') {
            return;
        }
    }

    std::snprintf(out_buf, buf_len, "RES_0x%03X (%u)", res_id, res_id);
}

static const char* classify_object(std::uint32_t res_id, const char* res_path, std::uint32_t obj_id,
                                   bool& is_hidden, ObjectCategory& category) {
    is_hidden = false;
    category = ObjectCategory::Other;

    if (res_id == RES_ID_SHADOW_VOL) {
        is_hidden = true;
        category = ObjectCategory::HiddenShadowVolume;
        return "[SOMBRA OCULTA: Stencil Volume (pc_shadow_volume.cpp)]";
    }
    if (res_id == RES_ID_HEADLIGHT) {
        is_hidden = true;
        category = ObjectCategory::HiddenLightVolume;
        return "[VOLUMEN LUZ: Cono Faros (pc_headlight_pool.cpp)]";
    }
    if (res_id == RES_ID_SUN_OCCLUSION) {
        is_hidden = true;
        category = ObjectCategory::HiddenOcclusionQuery;
        return "[OCLUSION SOL: Query Oclusion]";
    }
    if (res_id == RES_ID_FLAGMAN) {
        category = ObjectCategory::VisibleDriver;
        return "[PERSONAJE: Tela Bandera (rob_flag.cpp)]";
    }
    if (res_id == RES_ID_PARTICLES) {
        category = ObjectCategory::VisibleParticles;
        return "[PARTICULAS: Humo / Chispas / Marcas]";
    }
    if (res_id == RES_ID_2D_HUD) {
        category = ObjectCategory::VisibleHud2D;
        return "[INTERFAZ: Elemento 2D / HUD]";
    }

    const bool is_car = (res_path != nullptr) &&
                        (std::strstr(res_path, "plcar") != nullptr ||
                         std::strstr(res_path, "\\CARS\\") != nullptr ||
                         std::strstr(res_path, "/CARS/") != nullptr);

    if (is_car) {
        static const char* const kCarNames[30] = {
            "obj_plcar_f50_pmt",     "obj_plcar_dino_pmt",    "obj_plcar_gto_pmt",
            "obj_plcar_512bb_pmt",   "obj_plcar_dayts_pmt",   "obj_plcar_fx_pmt",
            "obj_plcar_testa_pmt",   "obj_plcar_360sp_pmt",   "obj_plcar_f40_pmt",
            "obj_plcar_250gto_pmt",  "obj_plcar_f355sp_pmt",  "obj_plcar_328gts_pmt",
            "obj_plcar_f430_pmt",    "obj_plcar_550b_pmt",    "obj_plcar_575sa_pmt",
            "obj_plcar_f50_t_pmt",   "obj_plcar_dino_t_pmt",  "obj_plcar_gto_t_pmt",
            "obj_plcar_512bb_t_pmt", "obj_plcar_dayts_t_pmt", "obj_plcar_fx_t_pmt",
            "obj_plcar_testa_t_pmt", "obj_plcar_360m_t_pmt",  "obj_plcar_f40_t_pmt",
            "obj_plcar_250gto_t_pmt","obj_plcar_f355sp_t_pmt","obj_plcar_328gts_t_pmt",
            "obj_plcar_f430sp_t_pmt","obj_plcar_550b_t_pmt",  "obj_plcar_575sa_t_pmt"
        };

        int matched_model = -1;
        for (int m = 0; m < 30; ++m) {
            if (std::strstr(res_path, kCarNames[m]) != nullptr) {
                matched_model = m;
                break;
            }
        }

        if (matched_model >= 0) {
            std::uint32_t defn = 0;
            if (safe_read_u32(0x5B2F68u + static_cast<std::uint32_t>(matched_model) * 4u, defn) && defn != 0) {
                std::uint32_t body = 0, sv1 = 0, sp = 0, sv2 = 0, interior = 0;
                std::uint32_t ml = 0, mr = 0, st = 0;
                std::uint32_t l0 = 0, l1 = 0, l2 = 0;
                std::uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;

                safe_read_u32(defn + 0x00u, body);      body &= 0xFFFFu;
                safe_read_u32(defn + 0x08u, sv1);       sv1 &= 0xFFFFu;
                safe_read_u32(defn + 0x0Cu, sp);        sp &= 0xFFFFu;
                safe_read_u32(defn + 0x10u, sv2);       sv2 &= 0xFFFFu;
                safe_read_u32(defn + 0x18u, interior);  interior &= 0xFFFFu;
                safe_read_u32(defn + 0x28u, ml);        ml &= 0xFFFFu;
                safe_read_u32(defn + 0x38u, mr);        mr &= 0xFFFFu;
                safe_read_u32(defn + 0x48u, st);        st &= 0xFFFFu;
                safe_read_u32(defn + 0x7Cu, l0);        l0 &= 0xFFFFu;
                safe_read_u32(defn + 0x80u, l1);        l1 &= 0xFFFFu;
                safe_read_u32(defn + 0x84u, l2);        l2 &= 0xFFFFu;
                safe_read_u32(defn + 0x88u, w0);        w0 &= 0xFFFFu;
                safe_read_u32(defn + 0xA8u, w1);        w1 &= 0xFFFFu;
                safe_read_u32(defn + 0xC8u, w2);        w2 &= 0xFFFFu;
                safe_read_u32(defn + 0xE8u, w3);        w3 &= 0xFFFFu;

                if (obj_id == sv1 || obj_id == sv2) {
                    is_hidden = true;
                    category = ObjectCategory::HiddenShadowVolume;
                    return "[SOMBRA COCHE: Volumen 3D Stencil]";
                }
                if (obj_id == sp) {
                    is_hidden = true;
                    category = ObjectCategory::HiddenShadowPlane;
                    return "[SOMBRA SUELO: Quad Proyectado Asfalto]";
                }
                if (obj_id == l1 || obj_id == l2) {
                    is_hidden = true;
                    category = ObjectCategory::HiddenHeadlightAlt;
                    return "[FAROS ALTERNATIVOS: Retractil/Extra]";
                }
                if (obj_id == l0) {
                    category = ObjectCategory::VisibleHeadlight;
                    return "[FAROS PRINCIPALES]";
                }
                if (obj_id == body) {
                    category = ObjectCategory::VisibleBody;
                    return "[CARROCERIA: Chasis Exterior]";
                }
                if (obj_id == interior) {
                    category = ObjectCategory::VisibleInterior;
                    return "[INTERIOR: Cabina / Tablero]";
                }
                if (obj_id == st) {
                    category = ObjectCategory::VisibleSteering;
                    return "[VOLANTE]";
                }
                if (obj_id == w0 || obj_id == w1 || obj_id == w2 || obj_id == w3) {
                    category = ObjectCategory::VisibleWheels;
                    return "[RUEDA / LLANTA]";
                }
                if (obj_id == ml || obj_id == mr) {
                    category = ObjectCategory::VisibleMirrors;
                    return "[RETROVISORES]";
                }
            }
        }

        // Generic car fallback if definition wasn't read
        if (obj_id == 0) {
            category = ObjectCategory::VisibleBody;
            return "[CARROCERIA: Chasis Principal]";
        }
        if (obj_id >= 14 && obj_id <= 15) {
            is_hidden = true;
            category = ObjectCategory::HiddenShadowVolume;
            return "[SOMBRA COCHE: Volumen 3D Stencil]";
        }
        if (obj_id == 16) {
            is_hidden = true;
            category = ObjectCategory::HiddenShadowPlane;
            return "[SOMBRA SUELO: Quad Proyectado]";
        }
        if (obj_id >= 1 && obj_id <= 3) {
            category = ObjectCategory::VisibleHeadlight;
            return "[FAROS]";
        }
        if (obj_id >= 8 && obj_id <= 11) {
            category = ObjectCategory::VisibleWheels;
            return "[RUEDA / LLANTA]";
        }
        category = ObjectCategory::VisibleBody;
        return "[COCHE: Sub-objeto Carroceria]";
    }

    if (res_path) {
        if (std::strstr(res_path, "\\Stage\\") || std::strstr(res_path, "/Stage/") ||
            std::strstr(res_path, "\\BK\\") || std::strstr(res_path, "/BK/") ||
            std::strstr(res_path, "cs_")) {
            category = ObjectCategory::VisibleScenery;
            return "[ESCENARIO: Tramo de Pista / Props]";
        }
        if (std::strstr(res_path, "\\DRIVER\\") || std::strstr(res_path, "/DRIVER/") ||
            std::strstr(res_path, "\\CHR\\") || std::strstr(res_path, "/CHR/")) {
            category = ObjectCategory::VisibleDriver;
            return "[PERSONAJE: Piloto / Personaje]";
        }
        if (std::strstr(res_path, "\\Traffic\\") || std::strstr(res_path, "/Traffic/")) {
            if (obj_id >= 10 && obj_id <= 16) {
                is_hidden = true;
                category = ObjectCategory::HiddenShadowVolume;
                return "[TRAFICO: Sombra/Detalle Oculto]";
            }
            category = ObjectCategory::VisibleBody;
            return "[TRAFICO: Vehiculo Civil]";
        }
    }

    category = ObjectCategory::Other;
    return "[GEOMETRIA 3D GENERAL]";
}

static char s_cur_model_name[128] = "Unknown";
static char s_cur_object_name[96] = "None";

const char* get_active_model_name() {
    resolve_resource_path(s_active_res_id, s_cur_model_name, sizeof(s_cur_model_name));
    return s_cur_model_name;
}

void get_resource_name(std::uint32_t res_id, char* out_buf, std::size_t buf_len) {
    resolve_resource_path(res_id, out_buf, buf_len);
}

const char* get_active_object_name() {
    char res_name[128];
    resolve_resource_path(s_active_res_id, res_name, sizeof(res_name));
    bool is_hidden = false;
    ObjectCategory cat = ObjectCategory::Other;
    const char* label = classify_object(s_active_res_id, res_name, s_active_obj_id, is_hidden, cat);
    std::snprintf(s_cur_object_name, sizeof(s_cur_object_name), "Obj %u: %s", s_active_obj_id, label);
    return s_cur_object_name;
}

static void dump_model_polygon_log_impl(const std::string& zone_name, unsigned sample_frames, bool log_to_profile) {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_sd_logs_enabled) return;
    ensure_log_dir();
    ensure_session_timestamp();

    if (s_model_slot_count == 0) {
        reset_model_polygon_accumulators();
        return;
    }
    const std::uint32_t denom_frames = (sample_frames > 0u) ? sample_frames : 1u;

    std::uint64_t total_frame_triangles = 0;
    std::uint32_t total_frame_draws = 0;

    std::uint64_t cat_scenery_tris = 0, cat_cars_tris = 0, cat_chars_tris = 0;
    std::uint64_t cat_hidden_tris = 0, cat_fx_tris = 0, cat_hud_tris = 0, cat_other_tris = 0;

    std::uint32_t cat_scenery_draws = 0, cat_cars_draws = 0, cat_chars_draws = 0;
    std::uint32_t cat_hidden_draws = 0, cat_fx_draws = 0, cat_hud_draws = 0, cat_other_draws = 0;

    std::uint64_t hidden_darken_vol_tris = 0;
    std::uint64_t hidden_car_vol_tris = 0;
    std::uint64_t hidden_car_plane_tris = 0;
    std::uint64_t hidden_light_vol_tris = 0;
    std::uint64_t hidden_sun_occ_tris = 0;
    std::uint64_t hidden_alt_light_tris = 0;

    struct ModelReportItem {
        std::size_t slot_idx;
        std::uint64_t avg_triangles;
        std::uint32_t avg_draws;
        char name[128];
    };
    std::vector<ModelReportItem> report_items;
    report_items.reserve(s_model_slot_count);

    for (std::size_t i = 0; i < s_model_slot_count; ++i) {
        const auto& slot = s_model_slots[i];
        const std::uint64_t avg_t = slot.total_triangles / denom_frames;
        const std::uint32_t avg_d = (slot.total_draws + denom_frames - 1u) / denom_frames;

        total_frame_triangles += avg_t;
        total_frame_draws += avg_d;

        ModelReportItem item;
        item.slot_idx = i;
        item.avg_triangles = avg_t;
        item.avg_draws = avg_d;
        resolve_resource_path(slot.resource_id, item.name, sizeof(item.name));
        report_items.push_back(item);

        for (std::size_t s = 0; s < slot.sub_count; ++s) {
            const auto& sub = slot.subs[s];
            const std::uint64_t sub_t = sub.triangles / denom_frames;
            const std::uint32_t sub_d = (sub.draws + denom_frames - 1u) / denom_frames;

            bool is_hidden = false;
            ObjectCategory cat = ObjectCategory::Other;
            classify_object(slot.resource_id, item.name, sub.object_id, is_hidden, cat);

            if (is_hidden) {
                cat_hidden_tris += sub_t;
                cat_hidden_draws += sub_d;
                if (cat == ObjectCategory::HiddenShadowVolume) {
                    if (slot.resource_id == RES_ID_SHADOW_VOL) hidden_darken_vol_tris += sub_t;
                    else hidden_car_vol_tris += sub_t;
                } else if (cat == ObjectCategory::HiddenShadowPlane) {
                    hidden_car_plane_tris += sub_t;
                } else if (cat == ObjectCategory::HiddenLightVolume) {
                    hidden_light_vol_tris += sub_t;
                } else if (cat == ObjectCategory::HiddenOcclusionQuery) {
                    hidden_sun_occ_tris += sub_t;
                } else if (cat == ObjectCategory::HiddenHeadlightAlt) {
                    hidden_alt_light_tris += sub_t;
                }
            } else {
                switch (cat) {
                    case ObjectCategory::VisibleScenery:
                        cat_scenery_tris += sub_t;
                        cat_scenery_draws += sub_d;
                        break;
                    case ObjectCategory::VisibleBody:
                    case ObjectCategory::VisibleInterior:
                    case ObjectCategory::VisibleSteering:
                    case ObjectCategory::VisibleWheels:
                    case ObjectCategory::VisibleMirrors:
                    case ObjectCategory::VisibleHeadlight:
                        cat_cars_tris += sub_t;
                        cat_cars_draws += sub_d;
                        break;
                    case ObjectCategory::VisibleDriver:
                        cat_chars_tris += sub_t;
                        cat_chars_draws += sub_d;
                        break;
                    case ObjectCategory::VisibleParticles:
                        cat_fx_tris += sub_t;
                        cat_fx_draws += sub_d;
                        break;
                    case ObjectCategory::VisibleHud2D:
                        cat_hud_tris += sub_t;
                        cat_hud_draws += sub_d;
                        break;
                    default:
                        cat_other_tris += sub_t;
                        cat_other_draws += sub_d;
                        break;
                }
            }
        }
    }

    std::sort(report_items.begin(), report_items.end(),
              [](const ModelReportItem& a, const ModelReportItem& b) {
                  return a.avg_triangles > b.avg_triangles;
              });

    const double denom_tris = (total_frame_triangles > 0) ? double(total_frame_triangles) : 1.0;
    auto pct = [&](std::uint64_t val) -> double {
        return (double(val) / denom_tris) * 100.0;
    };

    std::string report;
    report.reserve(16384);

    char cur_time_str[64]{"Desconocida"};
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    SceDateTime cur{};
    if (sceRtcGetCurrentClockLocalTime(&cur) >= 0) {
        std::snprintf(cur_time_str, sizeof(cur_time_str), "%04u-%02u-%02u %02u:%02u:%02u",
                      cur.year, cur.month, cur.day, cur.hour, cur.minute, cur.second);
    }
#endif

    char line[2048];
    std::snprintf(line, sizeof(line),
        "====================================================================================================\n"
        "           OUTRUN 2006: COAST 2 COAST - AUDITORIA DE MODELOS Y POLIGONOS (PS VITA)\n"
        "====================================================================================================\n"
        "Fecha / Hora: %s | Sesion: %s\n"
        "Zona Actual: %s\n"
        "Poligonos Totales (Promedio por Frame): %s triangulos\n"
        "Draw Calls Totales (Promedio por Frame): %u llamadas\n"
        "Modelos 3D Activos en Escena: %u modelos\n\n"
        "----------------------------------------------------------------------------------------------------\n"
        "  RESUMEN DE CARGA POLIGONAL POR CATEGORIA (Por Frame)\n"
        "----------------------------------------------------------------------------------------------------\n",
        cur_time_str,
        s_session_timestamp.c_str(),
        zone_name.c_str(),
        format_with_commas(total_frame_triangles).c_str(),
        total_frame_draws,
        static_cast<unsigned>(report_items.size())
    );
    report += line;

    auto append_cat = [&](const char* tag, std::uint64_t tris, std::uint32_t draws, const char* note = "") {
        std::snprintf(line, sizeof(line), "  %-42s: %10s tris (%5.1f%%) | %4u draws%s\n",
                      tag, format_with_commas(tris).c_str(), pct(tris), draws, note);
        report += line;
    };
    auto append_sub_cat = [&](const char* tag, std::uint64_t tris) {
        std::snprintf(line, sizeof(line), "      - %-36s: %10s tris\n", tag, format_with_commas(tris).c_str());
        report += line;
    };

    append_cat("[1] ESCENARIO Y PISTAS (Stage / BK)", cat_scenery_tris, cat_scenery_draws);
    append_cat("[2] VEHICULOS VISIBLES (Coches/Cabina)", cat_cars_tris, cat_cars_draws);
    append_cat("[3] PERSONAJES (Pilotos / Flagman)", cat_chars_tris, cat_chars_draws);
    append_cat("[4] POLIGONOS OCULTOS Y SOMBRAS (Shadow)", cat_hidden_tris, cat_hidden_draws, "  <-- [ATENCION]");
    append_sub_cat("Volumenes Sombra Stencil (Darken)", hidden_darken_vol_tris);
    append_sub_cat("Volumenes Sombra In-Mesh Coches", hidden_car_vol_tris);
    append_sub_cat("Planos de Sombra Suelo Asfalto", hidden_car_plane_tris);
    append_sub_cat("Conos de Luz Faros / Oclusion Sol", hidden_light_vol_tris + hidden_sun_occ_tris);
    append_sub_cat("Faros Alternativos Duplicados", hidden_alt_light_tris);
    append_cat("[5] EFECTOS Y PARTICULAS (Humo/Marcas)", cat_fx_tris, cat_fx_draws);
    append_cat("[6] INTERFAZ 2D / HUD", cat_hud_tris, cat_hud_draws);
    append_cat("[7] OTROS / NO CLASIFICADO", cat_other_tris, cat_other_draws);

    // Ranking de modelos por generacion de Draw Calls (para diagnostico directo de cuellos de botella)
    std::vector<ModelReportItem> dc_ranked_items = report_items;
    std::sort(dc_ranked_items.begin(), dc_ranked_items.end(),
              [](const ModelReportItem& a, const ModelReportItem& b) {
                  return a.avg_draws > b.avg_draws;
              });

    report += "\n----------------------------------------------------------------------------------------------------\n"
              "  RANKING DE MODELOS POR GENERACION DE DRAW CALLS (Llamadas de Dibujo / Frame)\n"
              "----------------------------------------------------------------------------------------------------\n";
    for (std::size_t rank = 0; rank < dc_ranked_items.size(); ++rank) {
        const auto& it = dc_ranked_items[rank];
        const double d_pct = (total_frame_draws > 0) ? (double(it.avg_draws) / double(total_frame_draws)) * 100.0 : 0.0;
        std::snprintf(line, sizeof(line), "  #%2u: %-52s: %4u draws (%5.1f%%) | %s tris\n",
                      static_cast<unsigned>(rank + 1), it.name, it.avg_draws, d_pct, format_with_commas(it.avg_triangles).c_str());
        report += line;
    }

    report += "\n====================================================================================================\n"
              "  DESGLOSE DETALLADO MODELO POR MODELO\n"
              "====================================================================================================\n\n";

    for (std::size_t rank = 0; rank < report_items.size(); ++rank) {
        const auto& item = report_items[rank];
        const auto& slot = s_model_slots[item.slot_idx];

        std::snprintf(line, sizeof(line),
            "MODELO #%u: %s [Resource ID: 0x%03X / %u]\n"
            "  Total: %s tris/frame (%5.1f%% del frame) | %u draw calls/frame\n"
            "  Sub-objetos:\n",
            static_cast<unsigned>(rank + 1),
            item.name,
            slot.resource_id, slot.resource_id,
            format_with_commas(item.avg_triangles).c_str(),
            pct(item.avg_triangles),
            item.avg_draws
        );
        report += line;

        struct SubSortItem {
            std::uint32_t obj_id;
            std::uint64_t triangles;
            std::uint32_t draws;
            std::uint32_t skinned_draws;
        };
        std::vector<SubSortItem> sub_items;
        sub_items.reserve(slot.sub_count);
        for (std::size_t s = 0; s < slot.sub_count; ++s) {
            sub_items.push_back({
                slot.subs[s].object_id,
                slot.subs[s].triangles / denom_frames,
                (slot.subs[s].draws + denom_frames - 1u) / denom_frames,
                (slot.subs[s].skinned_draws + denom_frames - 1u) / denom_frames
            });
        }
        std::sort(sub_items.begin(), sub_items.end(),
                  [](const SubSortItem& a, const SubSortItem& b) {
                      return a.obj_id < b.obj_id;
                  });

        for (const auto& sub : sub_items) {
            bool is_hidden = false;
            ObjectCategory cat = ObjectCategory::Other;
            const char* label = classify_object(slot.resource_id, item.name, sub.obj_id, is_hidden, cat);

            std::snprintf(line, sizeof(line),
                "    * Obj %2u %-42s : %8s tris | %3u draws%s%s\n",
                sub.obj_id,
                label,
                format_with_commas(sub.triangles).c_str(),
                sub.draws,
                (sub.skinned_draws > 0 ? " [SKINNED]" : ""),
                (is_hidden ? "  <-- [POLIGONO OCULTO]" : "")
            );
            report += line;
        }
        report += "\n";
    }

    report += "====================================================================================================\n\n";

    std::string zone_key = zone_name;
    if (zone_key.compare(0, 12, "Race Loading") == 0) {
        zone_key = "Race Loading";
    }
    s_zone_polygon_reports[zone_key] = report;

    // Construir reporte combinado preservando todas las zonas (In-Game Race primero, luego Car Select, luego el resto)
    std::string combined_report;
    combined_report.reserve(s_zone_polygon_reports.size() * 8192);
    auto append_zone_if_present = [&](const char* key) {
        auto it = s_zone_polygon_reports.find(key);
        if (it != s_zone_polygon_reports.end()) {
            combined_report += it->second;
        }
    };
    append_zone_if_present("In-Game Race");
    append_zone_if_present("Car Select");
    for (const auto& kv : s_zone_polygon_reports) {
        if (kv.first == "In-Game Race" || kv.first == "Car Select") continue;
        combined_report += kv.second;
    }

    auto write_report = [](const char* path, const std::string& data, const char* mode) {
        if (!path || !path[0]) return;
        FILE* f = std::fopen(path, mode);
        if (f) {
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
        }
    };

    // Guardar reporte de la sesion actual en ux0:data/outrun2006/
    if (!s_session_timestamp.empty()) {
        std::string path_data_ts = "ux0:data/outrun2006/model_polygons_" + s_session_timestamp + ".log";
        write_report(path_data_ts.c_str(), combined_report, "w");
    }

    // Nombre estatico que no cambia para facil acceso
    write_report("ux0:data/outrun2006/model_polygons.log", combined_report, "w");

    reset_model_polygon_accumulators();
#else
    (void)zone_name;
    (void)sample_frames;
    (void)log_to_profile;
#endif
}

void dump_model_polygon_log() {
    dump_model_polygon_log_impl(s_current_zone, s_model_sample_frames > 0u ? s_model_sample_frames : 1u, true);
}

static inline bool is_profile_scheduled_window(unsigned w) {
    if (w == 1u || w == 2u || w == 5u || w == 10u || w == 20u || w == 30u) {
        return true;
    }
    if (w >= 60u && (w % 60u == 0u)) {
        return true;
    }
    return false;
}

void record_frame(double logic_ms, double walk_ms, double draw_sub_ms, double ui_2d_ms, double swap_ms) {
    if (!g_sd_logs_enabled) return;
    static double sum_logic = 0.0;
    static double sum_walk = 0.0;
    static double sum_draw_sub = 0.0;
    static double sum_ui_2d = 0.0;
    static double sum_swap = 0.0;
    static unsigned frame_count = 0;
    static unsigned total_windows = 0;
    static unsigned window_in_state = 0;
    static std::string last_captured_zone;
    static std::uint32_t last_model_signature = 0;

    sum_logic += logic_ms;
    sum_walk += walk_ms;
    sum_draw_sub += draw_sub_ms;
    sum_ui_2d += ui_2d_ms;
    sum_swap += swap_ms;
    ++frame_count;
    ++s_model_sample_frames;

    // Log per-frame drops below 28 FPS (>35.714 ms) during race to both profile and render_bottleneck logs
    static unsigned s_drop_cooldown = 0;
    if (s_drop_cooldown > 0) --s_drop_cooldown;
    const double cur_frame_ms = logic_ms + walk_ms + draw_sub_ms + ui_2d_ms + swap_ms;
    if (s_current_zone == "In-Game Race" && cur_frame_ms > (1000.0 / 28.0) && s_drop_cooldown == 0) {
        const double cur_fps = 1000.0 / cur_frame_ms;
        log_message("[FPS_DROP <28] FPS: %.1f (%.1f ms) | Logic: %.1f ms | Walk: %.1f ms | Sub: %.1f ms | UI2D: %.1f ms | Swap: %.1f ms",
                    cur_fps, cur_frame_ms, logic_ms, walk_ms, draw_sub_ms, ui_2d_ms, swap_ms);
        bottleneck_log("\n--- [FPS_DROP <28] FPS: %.1f (%.1f ms) | Logic: %.1f ms | Walk: %.1f ms | Sub: %.1f ms | UI2D: %.1f ms | Swap: %.1f ms ---\n",
                       cur_fps, cur_frame_ms, logic_ms, walk_ms, draw_sub_ms, ui_2d_ms, swap_ms);
        g_capture_display_cb = true;
        s_drop_cooldown = 60;
    }

    // Trigger detailed snapshot at the start of each session/zone or on scheduled windows
    const bool zone_changed = (!last_captured_zone.empty() && last_captured_zone != s_current_zone);
    if (frame_count == 59) {
#if defined(OR2_VITA_HEAVY_PROFILE)
        const bool will_be_scheduled = is_profile_scheduled_window(window_in_state + 1u);
        if (total_windows == 0 || zone_changed || will_be_scheduled) {
            g_capture_render_frame = true;
            g_capture_display_cb = true;
        }
#endif
    }

    s_zone_counts[s_current_zone]++;

    if (frame_count >= 60) {
        ++total_windows;

        // Primary zone in this 60-frame window
        std::string primary_zone = s_current_zone;
        unsigned max_count = 0;
        for (const auto& kv : s_zone_counts) {
            if (kv.second > max_count) {
                max_count = kv.second;
                primary_zone = kv.first;
            }
        }

        // Detectar si cambio la zona o si cambio/se encendio/apago un modelo 3D
        std::uint32_t model_signature = 0;
        for (std::size_t i = 0; i < s_model_slot_count; ++i) {
            model_signature ^= (s_model_slots[i].resource_id * 2654435761u + std::uint32_t(i));
        }

        const bool state_changed = (primary_zone != last_captured_zone || model_signature != last_model_signature);
        if (state_changed) {
            window_in_state = 0;
            last_captured_zone = primary_zone;
            last_model_signature = model_signature;
        }
        ++window_in_state;

        // Intervalo espaciado: 1s, 2s, 5s, 10s, 20s, 30s, 60s, 120s... + cualquier ventana de carrera < 28 FPS
        const double win_total_ms = (sum_logic + sum_walk + sum_draw_sub + sum_ui_2d + sum_swap) / 60.0;
        const bool drop_window = (primary_zone == "In-Game Race" && win_total_ms > (1000.0 / 28.0));
        const bool should_log = is_profile_scheduled_window(window_in_state) || drop_window;

        if (should_log) {
            g_capture_render_frame = false;
            bottleneck_flush();
        }
        const double avg_logic = sum_logic / 60.0;
        const double avg_walk = sum_walk / 60.0;
        const double avg_draw_sub = sum_draw_sub / 60.0;
        const double avg_ui_2d = sum_ui_2d / 60.0;
        const double avg_swap = sum_swap / 60.0;
        const double total_frame_ms = avg_logic + avg_walk + avg_draw_sub + avg_ui_2d + avg_swap;
        const double fps = (total_frame_ms > 0.0001) ? (1000.0 / total_frame_ms) : 0.0;

        // Running FPS average / min / max for In-Game Race
        static std::uint64_t s_race_windows = 0;
        static double s_race_sum_ms = 0.0;
        static double s_race_min_fps = 999.0;
        static double s_race_max_fps = 0.0;
        if (primary_zone == "In-Game Race" && total_frame_ms > 1.0 && total_frame_ms < 120.0) {
            ++s_race_windows;
            s_race_sum_ms += total_frame_ms;
            if (fps < s_race_min_fps) s_race_min_fps = fps;
            if (fps > s_race_max_fps) s_race_max_fps = fps;
        }
        const double race_avg_fps = (s_race_windows > 0 && s_race_sum_ms > 0.0)
            ? (1000.0 * double(s_race_windows) / s_race_sum_ms) : fps;

        // Per-core CPU usage & GPU metrics over the 60-frame window
        const std::uint64_t c0_total_us = g_core0_busy_us.exchange(0, std::memory_order_relaxed);
        const std::uint64_t c0_swap_us  = g_core0_swap_us.exchange(0, std::memory_order_relaxed);
        const std::uint64_t c2_busy_us  = g_core2_busy_us.exchange(0, std::memory_order_relaxed);
        const double window_us = std::max(1.0, total_frame_ms * 60.0 * 1000.0);
        const std::uint64_t c0_active_us = (c0_total_us > c0_swap_us) ? (c0_total_us - c0_swap_us) : 0u;
        const double core0_pct = std::min(100.0, 100.0 * double(c0_active_us) / window_us);
        const double core1_pct = std::min(100.0, 100.0 * (avg_logic + avg_walk + avg_draw_sub + avg_ui_2d) / std::max(0.1, total_frame_ms));
        const double core2_pct = std::min(100.0, 100.0 * double(c2_busy_us) / window_us);
        const double cpu_avg_pct = (core0_pct + core1_pct + core2_pct) / 3.0;
        const double core0_swap_ms = double(c0_swap_us) / 60000.0;
        int arm_mhz = 0, gpu_mhz = 0;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        arm_mhz = scePowerGetArmClockFrequency();
        gpu_mhz = scePowerGetGpuClockFrequency();
#endif

        const double avg_tex_bind = double(g_sub_tex_lookup_bind_us / 60u) / 1000.0;
        const double avg_state_setup = double(g_sub_state_setup_us / 60u) / 1000.0;
        const double avg_draw_elem = double(g_sub_gl_draw_elements_us / 60u) / 1000.0;
        const double avg_draw_arr = double(g_sub_gl_draw_arrays_2d_us / 60u) / 1000.0;
        const double avg_copy_pack = double(g_sub_vertex_copy_pack_us / 60u) / 1000.0;
        const double avg_gl_clear = double(g_sub_gl_clear_us / 60u) / 1000.0;
        const std::uint64_t core_dip_us = g_sub_tex_lookup_bind_us + g_sub_state_setup_us
                                        + g_sub_gl_draw_elements_us + g_sub_gl_draw_arrays_2d_us
                                        + g_sub_vertex_copy_pack_us;
        const double avg_dip_overhead = (g_sub_dip_total_us > core_dip_us)
                                      ? double((g_sub_dip_total_us - core_dip_us) / 60u) / 1000.0
                                      : 0.0;
        const double avg_pc_flush = double(g_sub_pc_flush_us / 60u) / 1000.0;
        const double avg_display_cb = double(g_sub_display_cb_us / 60u) / 1000.0;
        const double accounted_sub = avg_copy_pack + avg_state_setup + avg_tex_bind
                                   + avg_draw_elem + avg_draw_arr + avg_gl_clear
                                   + avg_dip_overhead + avg_pc_flush + avg_display_cb;
        const double avg_scene_other = (avg_draw_sub > accounted_sub) ? (avg_draw_sub - accounted_sub) : 0.0;

        const std::uint32_t avg_draws = g_draw_calls / 60u;
        const std::uint32_t avg_hw_draws = g_hw_draws / 60u;
        const std::uint32_t avg_sw_draws = g_sw_draws / 60u;
        const std::uint32_t avg_flushes = g_flushes / 60u;
        const std::uint32_t avg_tex_binds = g_texture_binds / 60u;
        const std::uint64_t avg_triangles = g_triangles / 60u;

        int free_user_mb = 0;
        int free_cdram_mb = 0;
        unsigned heap_used_mb = 0;
        unsigned heap_free_mb = 0;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        static unsigned s_last_heap_used_mb = 0;
        static unsigned s_last_heap_free_mb = 0;
        if (should_log) {
            SceKernelFreeMemorySizeInfo mem_info{};
            mem_info.size = sizeof(SceKernelFreeMemorySizeInfo);
            if (sceKernelGetFreeMemorySize(&mem_info) >= 0) {
                free_user_mb = mem_info.size_user / (1024 * 1024);
                free_cdram_mb = mem_info.size_cdram / (1024 * 1024);
            }
            const struct mallinfo mi = mallinfo();
            s_last_heap_used_mb = static_cast<unsigned>(mi.uordblks) / (1024u * 1024u);
            s_last_heap_free_mb = static_cast<unsigned>(mi.fordblks) / (1024u * 1024u);
        }
        heap_used_mb = s_last_heap_used_mb;
        heap_free_mb = s_last_heap_free_mb;
#endif

        char time_str[64]{};
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
        SceDateTime cur_dt{};
        if (sceRtcGetCurrentClockLocalTime(&cur_dt) >= 0) {
            std::snprintf(time_str, sizeof(time_str), "%04u-%02u-%02u %02u:%02u:%02u",
                          cur_dt.year, cur_dt.month, cur_dt.day, cur_dt.hour, cur_dt.minute, cur_dt.second);
        } else {
            std::snprintf(time_str, sizeof(time_str), "%s", s_session_timestamp.c_str());
        }
#endif

        // Desglose de Draw Calls por categoria y mayores generadores
        std::uint32_t dc_scenery = 0, dc_cars = 0, dc_chars = 0, dc_hidden = 0, dc_fx = 0, dc_hud = 0, dc_other = 0;
        const std::uint32_t denom_f = (s_model_sample_frames > 0) ? s_model_sample_frames : 60u;

        struct ModelDcRank {
            char name[96];
            std::uint32_t draws;
        };
        std::vector<ModelDcRank> dc_ranks;
        dc_ranks.reserve(s_model_slot_count);

        for (std::size_t i = 0; i < s_model_slot_count; ++i) {
            const auto& slot = s_model_slots[i];
            const std::uint32_t slot_d = (slot.total_draws + denom_f - 1u) / denom_f;
            if (slot_d == 0) continue;

            ModelDcRank r{};
            resolve_resource_path(slot.resource_id, r.name, sizeof(r.name));
            r.draws = slot_d;
            dc_ranks.push_back(r);

            for (std::size_t s = 0; s < slot.sub_count; ++s) {
                const auto& sub = slot.subs[s];
                const std::uint32_t sub_d = (sub.draws + denom_f - 1u) / denom_f;
                bool is_hidden = false;
                ObjectCategory cat = ObjectCategory::Other;
                classify_object(slot.resource_id, r.name, sub.object_id, is_hidden, cat);
                if (is_hidden) {
                    dc_hidden += sub_d;
                } else {
                    switch (cat) {
                        case ObjectCategory::VisibleScenery: dc_scenery += sub_d; break;
                        case ObjectCategory::VisibleBody:
                        case ObjectCategory::VisibleInterior:
                        case ObjectCategory::VisibleSteering:
                        case ObjectCategory::VisibleWheels:
                        case ObjectCategory::VisibleMirrors:
                        case ObjectCategory::VisibleHeadlight: dc_cars += sub_d; break;
                        case ObjectCategory::VisibleDriver: dc_chars += sub_d; break;
                        case ObjectCategory::VisibleParticles: dc_fx += sub_d; break;
                        case ObjectCategory::VisibleHud2D: dc_hud += sub_d; break;
                        default: dc_other += sub_d; break;
                    }
                }
            }
        }

        std::sort(dc_ranks.begin(), dc_ranks.end(), [](const ModelDcRank& a, const ModelDcRank& b) {
            return a.draws > b.draws;
        });

        std::string top_dc_str;
        const std::size_t max_top = std::min(std::size_t(6), dc_ranks.size());
        for (std::size_t k = 0; k < max_top; ++k) {
            char tline[160];
            dc_ranks[k].name[sizeof(dc_ranks[k].name) - 1] = '\0';
            std::snprintf(tline, sizeof(tline), "      %u. %-40s: %3u draws\n",
                          static_cast<unsigned>(k + 1), dc_ranks[k].name, dc_ranks[k].draws);
            top_dc_str += tline;
        }
        if (top_dc_str.empty()) {
            top_dc_str = "      (Acumulando modelos...)\n";
        }

        const double gpu_submit_ms = avg_draw_elem + avg_draw_arr + core0_swap_ms;
        const double gpu_load_pct = std::min(100.0, 100.0 * gpu_submit_ms / std::max(0.1, total_frame_ms));
        const double mtris_per_sec = (double(avg_triangles) * fps) / 1000000.0;

        char buffer[4096];
        std::snprintf(buffer, sizeof(buffer),
            "--- [FRAME PROFILE (avg over 60 frames)] [%s] ---\n"
            "Zone: %s\n"
            "FPS: %.1f | Media Carrera: %.1f FPS (Min: %.1f / Max: %.1f) | Total Frame Time: %.1f ms\n"
            "[CPU / GPU Usage]:\n"
            "  - CPU Total (3 Cores, %d MHz): %.1f%% (Core0[vitaGL]: %.1f%% | Core1[Game]: %.1f%% | Core2[Jobs]: %.1f%%)\n"
            "  - GPU PowerVR (%d MHz): %.1f%% est. (Swap Core0: %.2f ms | %.1fK tris/frame | %.2f Mtris/s)\n"
            "[CPU Timers]:\n"
            "  - Logic/Physics: %.1f ms\n"
            "  - Scene Traversal (walk_404d10): %.1f ms\n"
            "  - Draw Submission (vitaGL/D3D9): %.1f ms\n"
            "    * Vertex Packing / Copy : %.2f ms\n"
            "    * State & Matrix Setup  : %.2f ms\n"
            "    * Texture Lookup & Bind : %.2f ms\n"
            "    * glDrawElements (3D)   : %.2f ms\n"
            "    * glDrawArrays (2D)     : %.2f ms\n"
            "    * DIP / Cleanup Overhead: %.2f ms\n"
            "    * glClear (Scene Clear) : %.2f ms\n"
            "    * PcRenderFlush (Mat/VS): %.2f ms\n"
            "    * Display Callbacks     : %.2f ms\n"
            "    * Other Scene Overhead  : %.2f ms\n"
            "  - 2D / UI / HUD: %.1f ms\n"
            "[GPU Timers]:\n"
            "  - Swap / GPU Wait (eglSwapBuffers): %.1f ms\n"
            "[Render Metrics]:\n"
            "  - Draw Calls: %u\n"
            "    * Escenario y Carretera : %u draws\n"
            "    * Coches y Trafico      : %u draws\n"
            "    * Sombras y Ocultos     : %u draws\n"
            "    * Interfaz 2D / HUD     : %u draws\n"
            "    * Efectos y Particulas  : %u draws\n"
            "    * Otros                 : %u draws\n"
            "    * Mayores Generadores:\n%s"
            "  - HW Draws: %u | SW Draws: %u%s%s\n"
            "  - Flushes executed: %u\n"
            "  - Texture Binds: %u\n"
            "[Memory]:\n"
            "  - Free User LPDDR2: %d MB\n"
            "  - Free CDRAM: %d MB\n"
            "  - Heap Used / Free: %u MB / %u MB\n"
            "-------------------------------------------\n",
            time_str,
            primary_zone.c_str(),
            fps, race_avg_fps, (s_race_windows > 0 ? s_race_min_fps : fps), (s_race_windows > 0 ? s_race_max_fps : fps), total_frame_ms,
            arm_mhz, cpu_avg_pct, core0_pct, core1_pct, core2_pct,
            gpu_mhz, gpu_load_pct, core0_swap_ms, double(avg_triangles) / 1000.0, mtris_per_sec,
            avg_logic, avg_walk, avg_draw_sub,
            avg_copy_pack, avg_state_setup, avg_tex_bind, avg_draw_elem, avg_draw_arr,
            avg_dip_overhead, avg_gl_clear, avg_pc_flush, avg_display_cb, avg_scene_other,
            avg_ui_2d,
            avg_swap,
            avg_draws,
            dc_scenery, dc_cars, dc_hidden, dc_hud, dc_fx, dc_other,
            top_dc_str.c_str(),
            avg_hw_draws, avg_sw_draws,
            (avg_sw_draws > 0 ? " | Last SW fail: " : ""),
            (avg_sw_draws > 0 ? g_last_hw_fail : ""),
            avg_flushes, avg_tex_binds,
            free_user_mb, free_cdram_mb,
            heap_used_mb, heap_free_mb
        );

        if (should_log) {
            std::lock_guard<std::mutex> lk(s_log_mutex);
            std::fprintf(stdout, "%s", buffer);
            std::fflush(stdout);

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
            ensure_log_dir();
            if (s_persistent_log_file) {
                std::fprintf(s_persistent_log_file, "%s", buffer);
                std::fflush(s_persistent_log_file);
            }
#endif
        }
        // Dump model_polygons.log AFTER dc_scenery/dc_ranks are computed, and only on zone/model change (avoid SD I/O mid-race).
        if (state_changed && s_model_slot_count > 0) {
            dump_model_polygon_log_impl(primary_zone, s_model_sample_frames > 0u ? s_model_sample_frames : 60u, window_in_state <= 2u);
        }

        // Reset accumulators
        sum_logic = 0.0;
        sum_walk = 0.0;
        sum_draw_sub = 0.0;
        sum_ui_2d = 0.0;
        sum_swap = 0.0;
        frame_count = 0;
        g_draw_calls = 0;
        g_hw_draws = 0;
        g_sw_draws = 0;
        g_flushes = 0;
        g_texture_binds = 0;
        g_triangles = 0;
        g_sub_tex_lookup_bind_us = 0;
        g_sub_state_setup_us = 0;
        g_sub_gl_draw_elements_us = 0;
        g_sub_gl_draw_arrays_2d_us = 0;
        g_sub_vertex_copy_pack_us = 0;
        g_sub_gl_clear_us = 0;
        g_sub_dip_total_us = 0;
        g_sub_pc_flush_us = 0;
        g_sub_display_cb_us = 0;
        s_zone_counts.clear();
        reset_model_polygon_accumulators();
    }
}

void shutdown_profiler() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (s_persistent_log_file) {
        dump_model_polygon_log();
        std::fflush(s_persistent_log_file);
        std::fclose(s_persistent_log_file);
        s_persistent_log_file = nullptr;
    }
    if (s_bottleneck_file) {
        bottleneck_flush();
        std::fclose(s_bottleneck_file);
        s_bottleneck_file = nullptr;
    }
#endif
}

} // namespace outrun::vita_profile
