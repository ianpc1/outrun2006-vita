#include "vita_gl_display.hpp"
#include "vita_profiler.hpp"
#include "vita_render_thread.hpp"
#include "platform/pc_dds.hpp"
#include <vitashark.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace outrun::ps5_runtime {

// Disable vitaGL internal splashscreen completely: prevents thread creation,
// prevents semaphore deadlocks, and skips the 3D rotating logo entirely.
extern "C" {
uint8_t is_splashscreen_active = 0;
uint32_t splash_mutex[2] = {0, 0};
void invoke_splashscreen(void) {}
void clear_splashscreen(void) {}
}

static bool s_vgl_globally_initialized = false;

VitaGlDisplay::VitaGlDisplay() = default;

VitaGlDisplay::~VitaGlDisplay() {
    outrun::vita_render::run_sync([this]() {
        for (auto& [_, img] : images_) {
            if (img.texture) glDeleteTextures(1, &img.texture);
        }
        images_.clear();
    });
}

bool VitaGlDisplay::open(unsigned width, unsigned height, std::string& error) {
    (void)width; (void)height; (void)error;
    width_ = 960;
    height_ = 544;

    outrun::vita_render::run_sync([this]() {
        if (!s_vgl_globally_initialized) {
            // Optimize vitaGL performance parameters before init
            vglUseCachedMem(GL_TRUE);
            vglUseTripleBuffering(GL_TRUE);
            vglUseVramForUSSE(GL_TRUE);
            vglUseLowPrecision(GL_TRUE);

            // Initialize vitaGL: 24 MB RAM threshold, 0 CDRAM threshold (fuerza uso total de los 64-128 MB CDRAM en VGL_MEM_VRAM)
            vglInitWithCustomThreshold(0, 960, 544, 24 * 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
            vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, 1, 1, 1);

            s_vgl_globally_initialized = true;
            std::fprintf(stdout, "VitaGlDisplay: initialized hardware GPU 960x544 SGX543MP4+ on Core 0 (24MB RAM / 64MB CDRAM)\n");
        }

        glViewport(0, 0, 960, 544);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0f);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    });

    initialized_ = true;
    return true;
}

void VitaGlDisplay::clear() {
    outrun::vita_render::run_sync([]() {
        glViewport(0, 0, 960, 544);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    });
}

unsigned VitaGlDisplay::upload(const void* key, const std::uint8_t* pixels, unsigned width, unsigned height, bool update) {
    if (!pixels || !width || !height) return 0;
    unsigned tex_id = 0;
    outrun::vita_render::run_sync([&]() {
        auto& img = images_[key];
        const bool fresh = (!img.texture || img.width != width || img.height != height);
        if (fresh) {
            if (img.texture) glDeleteTextures(1, &img.texture);
            glGenTextures(1, &img.texture);
            img.width = width;
            img.height = height;
        }
        img.last_frame = frame_count_;
        glBindTexture(GL_TEXTURE_2D, img.texture);
        if (fresh) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, int(width), int(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else if (update) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, int(width), int(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        tex_id = img.texture;
    });
    return tex_id;
}

unsigned VitaGlDisplay::texels(const void* key, platform::MeshPreviewTextureFormat format,
                              unsigned width, unsigned height, const std::vector<std::uint8_t>& bytes) {
    auto it = images_.find(key);
    if (it != images_.end()) {
        it->second.last_frame = frame_count_;
        return it->second.texture;
    }

    platform::PcDdsTexture texture{};
    texture.width = width;
    texture.height = height;
    texture.levels = 1;
    texture.format = format == platform::MeshPreviewTextureFormat::bc1 ? platform::PcDdsTexture::Format::bc1 :
                     format == platform::MeshPreviewTextureFormat::bc2 ? platform::PcDdsTexture::Format::bc2 :
                     format == platform::MeshPreviewTextureFormat::bc3 ? platform::PcDdsTexture::Format::bc3 :
                     platform::PcDdsTexture::Format::rgba8;
    texture.faces.resize(1);
    texture.faces[0].push_back({width, height, bytes});

    auto rgba = platform::pc_dds_rgba(texture, 0, 0);
    if (rgba.size() != std::size_t(width) * height * 4) return 0;
    return upload(key, rgba.data(), width, height, false);
}

void VitaGlDisplay::quad(const std::array<platform::MeshPreviewVertex, 4>& vertices,
                         unsigned texture, unsigned width, unsigned height) {
    (void)width; (void)height;
    outrun::vita_render::run_sync([&]() {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, 960, 544);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    if (texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        ++outrun::vita_profile::g_texture_binds;
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    ++outrun::vita_profile::g_draw_calls;
    outrun::vita_profile::g_triangles += 2;

    float pos[6][3];
    float col[6][4];
    float uv[6][2];
    const unsigned order[6] = {0, 1, 2, 2, 3, 0};
    for (int i = 0; i < 6; ++i) {
        const auto& v = vertices[order[i]];
        pos[i][0] = v.position[0];
        pos[i][1] = v.position[1];
        pos[i][2] = 0.0f;
        col[i][0] = v.color[0];
        col[i][1] = v.color[1];
        col[i][2] = v.color[2];
        col[i][3] = v.color[3];
        uv[i][0]  = v.uv[0];
        uv[i][1]  = v.uv[1];
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, col);
    if (texture) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    if (texture) glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    });
}

void VitaGlDisplay::rgba(const void* key, const std::uint8_t* bytes,
                         unsigned width, unsigned height, int x, int y, int dst_w, int dst_h,
                         bool update) {
    if (!bytes || !width || !height || dst_w <= 0 || dst_h <= 0) return;
    outrun::vita_render::run_sync([&]() {
    unsigned tex = upload(key, bytes, width, height, update);
    if (!tex) return;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, 960, 544);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    ++outrun::vita_profile::g_texture_binds;
    ++outrun::vita_profile::g_draw_calls;
    outrun::vita_profile::g_triangles += 2;

    const float x0 = (2.0f * float(x) / 960.0f) - 1.0f;
    const float x1 = (2.0f * float(x + dst_w) / 960.0f) - 1.0f;
    const float y0 = 1.0f - (2.0f * float(y) / 544.0f);
    const float y1 = 1.0f - (2.0f * float(y + dst_h) / 544.0f);

    float pos[6][3] = {
        { x0, y0, 0.0f }, { x1, y0, 0.0f }, { x1, y1, 0.0f },
        { x1, y1, 0.0f }, { x0, y1, 0.0f }, { x0, y0, 0.0f }
    };
    float uv[6][2] = {
        { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
        { 1.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f }
    };
    float col[6][4];
    for (int i = 0; i < 6; ++i) {
        col[i][0] = 1.0f; col[i][1] = 1.0f; col[i][2] = 1.0f; col[i][3] = 1.0f;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, col);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    });
}

void VitaGlDisplay::pc_frame(unsigned texture, int x, int y, int width, int height) {
    (void)texture; (void)x; (void)y; (void)width; (void)height;
}

void VitaGlDisplay::release_pc_textures(std::vector<std::uint32_t>& handles) {
    if (handles.empty()) return;
    outrun::vita_render::run_sync([&]() {
        for (const auto h : handles) {
            const void* key = reinterpret_cast<const void*>(std::uintptr_t(0x80000000u | h));
            auto it = images_.find(key);
            if (it != images_.end()) {
                if (it->second.texture) glDeleteTextures(1, &it->second.texture);
                images_.erase(it);
            }
        }
        handles.clear();
    });
}

bool VitaGlDisplay::present(std::string& error) {
    (void)error;
    outrun::vita_render::run_sync([this]() {
        const std::uint32_t t0 = outrun::vita_profile::vita_time_us();
        vglSwapBuffers(GL_TRUE);
        outrun::vita_profile::g_core0_swap_us.fetch_add(outrun::vita_profile::vita_time_us() - t0, std::memory_order_relaxed);
        ++frame_count_;

        // Garbage collect stale 2D textures every 128 frames (~2-4 seconds)
        // to prevent memory exhaustion across menus and car selection.
        if ((frame_count_ & 127u) == 0u) {
            for (auto it = images_.begin(); it != images_.end(); ) {
                if (frame_count_ - it->second.last_frame > 120u) {
                    if (it->second.texture) glDeleteTextures(1, &it->second.texture);
                    it = images_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });
    return true;
}

} // namespace outrun::ps5_runtime
