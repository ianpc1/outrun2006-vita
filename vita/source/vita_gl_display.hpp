#pragma once
#include <vitaGL.h>
#include "platform/mesh_preview_pack.hpp"
#include "platform/pc_soft_d3d9.hpp"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace outrun::ps5_runtime {

class VitaGlDisplay {
public:
    VitaGlDisplay();
    ~VitaGlDisplay();

    bool open(unsigned width, unsigned height, std::string& error);
    void clear();
    unsigned texels(const void* key, platform::MeshPreviewTextureFormat format,
                    unsigned width, unsigned height, const std::vector<std::uint8_t>& bytes);
    void quad(const std::array<platform::MeshPreviewVertex, 4>& vertices,
              unsigned texture, unsigned width, unsigned height);
    void rgba(const void* key, const std::uint8_t* bytes,
              unsigned width, unsigned height, int x, int y, int dst_w, int dst_h,
              bool update = true);
    void pc_frame(unsigned texture, int x, int y, int width, int height);
    void release_pc_textures(std::vector<std::uint32_t>& handles);
    bool present(std::string& error);

private:
    unsigned upload(const void* key, const std::uint8_t* pixels, unsigned width, unsigned height, bool update);

    struct Image {
        unsigned texture{0};
        unsigned width{0};
        unsigned height{0};
        unsigned last_frame{0};
    };
    std::map<const void*, Image> images_;
    unsigned width_{960};
    unsigned height_{544};
    unsigned frame_count_{0};
    bool initialized_{false};
};

} // namespace outrun::ps5_runtime
