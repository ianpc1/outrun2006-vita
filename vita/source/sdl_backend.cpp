#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "sdl_backend.hpp"
#include "platform/pc_dds.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace outrun::ps5_runtime {
namespace {
void require_sdl(bool ok){if(!ok)throw std::runtime_error(SDL_GetError());}
}
SdlDisplay::~SdlDisplay(){
    for(auto& [_,image]:textures_)SDL_DestroyTexture(image.texture);
    if(renderer_)SDL_DestroyRenderer(renderer_);
    if(window_)SDL_DestroyWindow(window_);
}
bool SdlDisplay::open(unsigned w,unsigned h,std::string& error){
    width_=w;height_=h;
#if defined(__VITA__) || defined(__psp2__)
    const int window_width=960,window_height=544;
#elif defined(OR2_PS5_PAYLOAD)
    // The homebrew VideoOut backend exposes a 1920x1080 display.
    const int window_width=1920,window_height=1080;
#else
    const int window_width=int(w),window_height=int(h);
#endif
    window_=SDL_CreateWindow("OutRun 2006 Vita",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,window_width,window_height,SDL_WINDOW_SHOWN);
    if(!window_){error=SDL_GetError();return false;}
    renderer_=SDL_CreateRenderer(window_,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!renderer_)renderer_=SDL_CreateRenderer(window_,-1,0);
    if(!renderer_){error=SDL_GetError();return false;}
    if(SDL_RenderSetLogicalSize(renderer_,int(w),int(h))<0){error=SDL_GetError();return false;}
    SDL_SetRenderDrawBlendMode(renderer_,SDL_BLENDMODE_BLEND);
    std::fprintf(stdout,"Vita display: SDL driver=%s, 960x544 GXM composition\n",SDL_GetCurrentVideoDriver());
    return true;
}
void SdlDisplay::clear(){require_sdl(SDL_SetRenderDrawColor(renderer_,0,0,0,255)==0);require_sdl(SDL_RenderClear(renderer_)==0);}
SDL_Texture* SdlDisplay::upload(const void* key,const std::uint8_t* pixels,unsigned w,unsigned h,bool update){
    if(!pixels||!w||!h)throw std::runtime_error("invalid display texture");
    auto& image=textures_[key];bool fresh=false;
    if(!image.texture||image.width!=w||image.height!=h){
        if(image.texture)SDL_DestroyTexture(image.texture);
        image={SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,int(w),int(h)),w,h};
        require_sdl(image.texture!=nullptr);
        require_sdl(SDL_SetTextureBlendMode(image.texture,SDL_BLENDMODE_BLEND)==0);
        SDL_SetTextureScaleMode(image.texture,SDL_ScaleModeLinear);fresh=true;
    }
    if(fresh||update)require_sdl(SDL_UpdateTexture(image.texture,nullptr,pixels,int(w)*4)==0);
    return image.texture;
}
SDL_Texture* SdlDisplay::texels(const void* key,platform::MeshPreviewTextureFormat f,unsigned w,unsigned h,const std::vector<std::uint8_t>& bytes){
    auto i=textures_.find(key);if(i!=textures_.end())return i->second.texture;
    platform::PcDdsTexture texture{};texture.width=w;texture.height=h;texture.levels=1;
    texture.format=f==platform::MeshPreviewTextureFormat::bc1?platform::PcDdsTexture::Format::bc1:
        f==platform::MeshPreviewTextureFormat::bc2?platform::PcDdsTexture::Format::bc2:
        f==platform::MeshPreviewTextureFormat::bc3?platform::PcDdsTexture::Format::bc3:platform::PcDdsTexture::Format::rgba8;
    texture.faces.resize(1);texture.faces[0].push_back({w,h,bytes});
    auto rgba=platform::pc_dds_rgba(texture,0,0);
    if(rgba.size()!=std::size_t(w)*h*4)throw std::runtime_error("DDS decode size mismatch");
    return upload(key,rgba.data(),w,h,false);
}
void SdlDisplay::quad(const std::array<platform::MeshPreviewVertex,4>& v,SDL_Texture* texture,unsigned,unsigned){
    SDL_Vertex vertices[4];
    for(unsigned k=0;k<4;++k){
        vertices[k].position={(v[k].position[0]+1)*0.5f*float(width_),(1-v[k].position[1])*0.5f*float(height_)};
        auto channel=[&](unsigned c){return Uint8(std::clamp(v[k].color[c],0.0f,1.0f)*255.0f+0.5f);};
        vertices[k].color={channel(0),channel(1),channel(2),channel(3)};
        vertices[k].tex_coord={std::clamp(v[k].uv[0],0.0f,1.0f),std::clamp(v[k].uv[1],0.0f,1.0f)};
    }
    const int indices[6]{0,1,2,2,3,0};
    require_sdl(SDL_RenderGeometry(renderer_,texture,vertices,4,indices,6)==0);
}
void SdlDisplay::rgba(const void* key,const std::uint8_t* bytes,unsigned w,unsigned h,int x,int y,int width,int height,bool blend){
    auto* texture=upload(key,bytes,w,h,true);const SDL_Rect rect{x,y,width,height};
    require_sdl(SDL_SetTextureBlendMode(texture,blend?SDL_BLENDMODE_BLEND:SDL_BLENDMODE_NONE)==0);
    require_sdl(SDL_RenderCopy(renderer_,texture,nullptr,&rect)==0);
}
void SdlDisplay::release_pc_textures(std::vector<std::uint32_t>& handles){
    for(const auto h:handles){
        const void* key=reinterpret_cast<const void*>(std::uintptr_t(0x80000000u|h));
        if(auto it=textures_.find(key);it!=textures_.end()){
            if(it->second.texture)SDL_DestroyTexture(it->second.texture);
            textures_.erase(it);
        }
    }
    handles.clear();
}
void SdlDisplay::draw_hw2d(const platform::PcSoftD3D9Device::Hw2dLayer& layer){
    if(layer.batches.empty()||layer.vertices.empty()||layer.indices.empty())return;
    static std::vector<SDL_Vertex> hw2d_verts;
    hw2d_verts.resize(layer.vertices.size());
    for(std::size_t i=0;i<layer.vertices.size();++i){
        const auto& s=layer.vertices[i];
        hw2d_verts[i].position={s.x,s.y};
        hw2d_verts[i].color={s.r,s.g,s.b,s.a};
        hw2d_verts[i].tex_coord={s.u,s.v};
    }
    static const std::uint8_t white_px[4]{255,255,255,255};
    SDL_Texture* white_tex=nullptr;
    bool clipped=false;
    for(const auto& b:layer.batches){
        SDL_Texture* tex=nullptr;
        if(b.tex_handle&&b.tex_rgba&&b.tex_w&&b.tex_h){
            const void* key=reinterpret_cast<const void*>(std::uintptr_t(0x80000000u|b.tex_handle));
            tex=upload(key,b.tex_rgba,b.tex_w,b.tex_h,false);
        }else{
            if(!white_tex)white_tex=upload(white_px,white_px,1u,1u,false);
            tex=white_tex;
        }
        const SDL_BlendMode bm=b.blend_mode==0u?SDL_BLENDMODE_NONE:(b.blend_mode==2u?SDL_BLENDMODE_ADD:SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(tex,bm);
        if(b.has_clip){
            if(b.clip_w<=0||b.clip_h<=0)continue;
            const SDL_Rect cr{b.clip_x,b.clip_y,b.clip_w,b.clip_h};
            SDL_RenderSetClipRect(renderer_,&cr);
            clipped=true;
        }else if(clipped){
            SDL_RenderSetClipRect(renderer_,nullptr);
            clipped=false;
        }
        SDL_RenderGeometry(renderer_,tex,hw2d_verts.data()+b.first_vertex,int(b.vertex_count),
                           layer.indices.data()+b.first_index,int(b.index_count));
    }
    if(clipped)SDL_RenderSetClipRect(renderer_,nullptr);
}
bool SdlDisplay::present(std::string& error){
#ifdef OR2_PS5_HOST_TEST
    // Capture the composed image, including the native 3D and frontend layers.
    if(const char* dir=std::getenv("OR2_PS5_SCREENSHOT_DIR")){
        static unsigned frame=0;
        std::filesystem::create_directories(dir);
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,int(width_),int(height_),32,SDL_PIXELFORMAT_RGBA32);
        if(!surface){error=SDL_GetError();return false;}
        char name[48];std::snprintf(name,sizeof name,"/frame-%04u.bmp",++frame);
        const bool ok=SDL_RenderReadPixels(renderer_,nullptr,surface->format->format,surface->pixels,surface->pitch)==0&&
            SDL_SaveBMP(surface,(std::string(dir)+name).c_str())==0;
        SDL_FreeSurface(surface);if(!ok){error=SDL_GetError();return false;}
    }
#endif
    SDL_ClearError();SDL_RenderPresent(renderer_);
    if(*SDL_GetError()){error=SDL_GetError();return false;}
    return true;
}
}
