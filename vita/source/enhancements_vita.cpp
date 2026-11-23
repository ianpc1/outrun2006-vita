#include "enhancements_vita.hpp"
#include <algorithm>

namespace outrun::vita_runtime {
namespace en = outrun::enhancements;

namespace {
// PlayStation Vita native screen height (960x544).
constexpr unsigned FrameHeight = 544u;

// Global states for vita renderer
unsigned g_render_scale_percent = 100u;
unsigned g_aa_mode = 0u;
}
extern bool g_widescreen;

// 3D resolution heights for Vita: 0% to 100% with step of 10%
std::vector<std::uint32_t> VitaVideoBackend::heights() const {
    return { 0u, 54u, 108u, 163u, 217u, 272u, 326u, 380u, 435u, 489u, 544u };
}

std::vector<en::Antialiasing> VitaVideoBackend::antialiasing_modes() const {
    // SGX543MP4+ on PS Vita natively supports MSAA 2x and 4x in hardware
    std::vector<en::Antialiasing> modes{ en::Antialiasing::Off };
    if (!hooks_.msaa_ready || hooks_.msaa_ready()) {
        modes.push_back(en::Antialiasing::Msaa2x);
        modes.push_back(en::Antialiasing::Msaa4x);
    }
    return modes;
}

en::VideoSettings VitaVideoBackend::current() const {
    en::VideoSettings s;
    s.widescreen = true;
    const unsigned current_h = (g_render_scale_percent == 0u) ? 0u :
                               (g_render_scale_percent * 544u + 50u) / 100u;
    s.resolution = en::resolution_at(en::nearest_height(heights(), current_h), true);
    s.antialiasing = (g_aa_mode == 2u) ? en::Antialiasing::Msaa2x :
                     (g_aa_mode == 4u) ? en::Antialiasing::Msaa4x : en::Antialiasing::Off;
    return s;
}

void VitaVideoBackend::apply(const en::VideoSettings& s) {
    g_widescreen = true;
    const unsigned percent = (s.resolution.height == 0u) ? 0u :
                             (s.resolution.height * 100u + 272u) / 544u;

    if (hooks_.render_scale) {
        *hooks_.render_scale = percent;
    }
    g_render_scale_percent = percent;

    if (s.antialiasing == en::Antialiasing::Msaa2x) g_aa_mode = 2u;
    else if (s.antialiasing == en::Antialiasing::Msaa4x) g_aa_mode = 4u;
    else g_aa_mode = 0u;

    if (!hooks_.options_path.empty()) {
        en::write_options(hooks_.options_path, {
            { "widescreen", s.widescreen ? "1" : "0" },
            { "antialiasing", en::antialiasing_key(s.antialiasing) },
            { "render_scale", std::to_string(percent) }
        });
    }
}

} // namespace outrun::vita_runtime
