#pragma once
// Enhancement rows of Options > Settings on PS Vita (src/enhancements/):
// 16:9, the 3D render resolution (960x544, 720x408, 480x272) and antialiasing (MSAA),
// applied live. The frame buffer is 960x544 (Vita native).
#include "enhancements/video.hpp"
#include <functional>
#include <string>

namespace outrun::vita_runtime {

class VitaVideoBackend final : public enhancements::VideoBackend {
public:
    struct Hooks {
        std::string options_path;           // options.ini, rewritten on every change
        unsigned* render_scale{};           // render scale percent (50, 75, 100)
        std::function<bool()> msaa_ready;   // MSAA supported by vitaGL / GXM
    };
    explicit VitaVideoBackend(Hooks hooks):hooks_(std::move(hooks)){}
    bool widescreen_available()const override{return false;}
    std::vector<std::uint32_t> heights()const override;
    std::vector<enhancements::Antialiasing> antialiasing_modes()const override;
    enhancements::VideoSettings current()const override;
    void apply(const enhancements::VideoSettings&)override;
private:
    Hooks hooks_;
};

} // namespace outrun::vita_runtime
