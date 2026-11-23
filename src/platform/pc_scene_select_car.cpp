// 49F4D0 = 46C090 (event 8 function 0x2B display: the OUTRUN2SP car-select car).
// 46C090 is 46C140 with the model draw 46A560 instead of 46AE70: the sun light
// 4082B0(2,0,0) diffuse scaled by the car brightness +58, 40D840(2), the car
// matrix +B0 pushed (409F90), 46A560(car, +11, 0), 40A010, the light restored
// and 40D840(2) again. 46A560 is the draw list of vehicle_model_draw.cpp; its render
// leaves 405360 / 4044F0 / 404540 / 4052B0 / 4052C0 and the shadow volume 422550
// run with the matrix recorded at each call.
#include "platform/pc_scene_renderer.hpp"
#include "platform/vehicle_model_draw.hpp"
#include "platform/frontend_vehicle_data.hpp"
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include "vita_profiler.hpp"
#endif
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace outrun::platform {
using driving::Bytes;
namespace {
inline float mulf(float a,float b){volatile float x=a*b;return x;}

static bool s_log_initialized = false;
static int s_last_logged_model = -1;

static void log_car_pmt(unsigned model_index, const char* path, const std::vector<std::uint8_t>& pmt) {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!outrun::vita_profile::g_sd_logs_enabled) return;
#endif
    if (pmt.size() < 16) return;
    const std::uint32_t object_count = *reinterpret_cast<const std::uint32_t*>(pmt.data());
    const std::uint32_t texture_count = *reinterpret_cast<const std::uint32_t*>(pmt.data() + 4);
    const std::uint32_t ss = *reinterpret_cast<const std::uint32_t*>(pmt.data() + 8);
    const std::uint32_t vs = *reinterpret_cast<const std::uint32_t*>(pmt.data() + 12);

    std::uint32_t total_tris = 0;
    std::uint32_t obj0_tris = 0;

    if (16 + ss <= pmt.size()) {
        const std::uint8_t* sys = pmt.data() + 16;
        for (std::uint32_t oi = 0; oi < object_count; ++oi) {
            const std::uint32_t obj_off = 24 + oi * 60;
            if (obj_off + 60 > ss) break;
            const std::uint32_t* fields = reinterpret_cast<const std::uint32_t*>(sys + obj_off);
            const std::uint32_t meshes_off = fields[8];
            const std::uint32_t prims_off = fields[9];
            const std::uint32_t draws_off = fields[10];
            const std::uint32_t ranges_off = fields[11];

            if (meshes_off + 8 > ss) continue;
            const std::uint32_t first_prim = *reinterpret_cast<const std::uint32_t*>(sys + meshes_off);
            const std::int32_t num_prims = *reinterpret_cast<const std::int32_t*>(sys + meshes_off + 4);

            std::uint32_t obj_tris = 0;
            for (std::int32_t pi = 0; pi < num_prims; ++pi) {
                const std::uint32_t p_off = prims_off + (first_prim + pi) * 0x14;
                if (p_off + 0x14 > ss) break;
                const std::uint32_t* p_fields = reinterpret_cast<const std::uint32_t*>(sys + p_off);
                const std::uint32_t fd0 = p_fields[1];
                const std::int32_t d0 = static_cast<std::int32_t>(p_fields[3]);
                for (std::int32_t di = 0; di < d0; ++di) {
                    const std::uint32_t d_off = draws_off + (fd0 + di) * 0x20;
                    if (d_off + 0x20 > ss) break;
                    const std::uint32_t r_idx = *reinterpret_cast<const std::uint32_t*>(sys + d_off + 8);
                    const std::uint32_t r_off = ranges_off + r_idx * 16;
                    if (r_off + 16 > ss) break;
                    const std::uint32_t rcount = *reinterpret_cast<const std::uint32_t*>(sys + r_off + 8);
                    obj_tris += rcount;
                }
            }
            total_tris += obj_tris;
            if (oi == 0) obj0_tris = obj_tris;
        }
    }

    if (!s_log_initialized) {
        s_log_initialized = true;
        FILE* fi = std::fopen("ux0:data/outrun2006/car_triangles.txt", "w");
        if (!fi) fi = std::fopen("car_triangles.txt", "w");
        if (fi) {
            std::fprintf(fi, "========================================================\n");
            std::fprintf(fi, "  REGISTRO EN VIVO DE MODELOS Y POLIGONOS (PS VITA)\n");
            std::fprintf(fi, "========================================================\n\n");
            std::fclose(fi);
        }
    }

    FILE* f = std::fopen("ux0:data/outrun2006/car_triangles.txt", "a");
    if (!f) f = std::fopen("car_triangles.txt", "a");
    if (f) {
        std::fprintf(f, "[CARGA DE COCHE] Modelo ID: %u\n", model_index);
        std::fprintf(f, "  Archivo: %s\n", path ? path : "desconocido");
        std::fprintf(f, "  Bytes PMT Descomprimido: %u (sys=%u, vid=%u)\n", (unsigned)pmt.size(), ss, vs);
        std::fprintf(f, "  Triangulos Totales del Modelo: %u\n", total_tris);
        std::fprintf(f, "  Carroceria (Obj 0): %u triangulos\n", obj0_tris);
        if (total_tris >= 33000) {
            std::fprintf(f, "  ESTADO: MODELO REDUCIDO / INDICES DE TIRAS (%u tris)\n\n", total_tris);
        } else {
            std::fprintf(f, "  ESTADO: MODELO ORIGINAL RETAIL (%u tris)\n\n", total_tris);
        }
        std::fflush(f);
        std::fclose(f);
    }
}

} // namespace

bool PcSceneRenderer::display_select_car(){
    Bytes vehicle(nullptr,0),camera(nullptr,0),body(nullptr,0);
    if(!vehicle_camera||!vehicle_camera(vehicle,camera)||!body_view||!body_view(body))return false;
    const auto model=vehicle.u8(0x11);
    if(model>=VehicleResourceIds.size())return false;
    for(const std::uint32_t id:{VehicleResourceIds[model],0xbbu}){
        if(bank_loaded(id))continue;
        std::vector<std::uint8_t> pmt;std::string error;
        if(!bank_source||!bank_source(id,pmt)){
            ++bank_failures;last_error="bank "+std::to_string(id)+": source missing";return false;
        }
        if(id == VehicleResourceIds[model]) {
            const char* path = (model < VehicleResourcePaths.size()) ? VehicleResourcePaths[model] : "unknown";
            log_car_pmt(model, path, pmt);
        }
        if(!load_bank(id,std::move(pmt),error)){++bank_failures;last_error="bank "+std::to_string(id)+": "+error;return false;}
    }
    PcEnvironmentRenderTables tables{Bytes(environment_.lights_899b98.data(),environment_.lights_899b98.size()),
        Bytes(environment_.fog_7d3a10.data(),environment_.fog_7d3a10.size()),0};
    PcVehicleDisplayContext c{*flush_,*queue_,tables,events_?events_->slots[386].flags:std::uint8_t(0),frame_.layer_7d25f0,matrices_,body,
        std::int32_t(current_mode_),scene_82e7d4};
    c.shadow=[this,vehicle](const PcVehicleDrawCall& call){return shadow_leaf(call,vehicle);};
    c.unported=[this](std::uint32_t pc){++unported_leaves[pc];};
    Bytes sun=tables.lights_899b98.sub(0x899cd8u-0x899b98u,0xa0);
    std::array<std::uint8_t,16> saved{};
    for(unsigned k=0;k<16;++k)saved[k]=sun.u8(8+k);
    const float scale=vehicle.f32(0x58);
    sun.putf(8,mulf(sun.f32(8),scale));
    sun.putf(0xc,mulf(scale,sun.f32(0xc)));
    sun.putf(0x10,mulf(scale,sun.f32(0x10)));
    render_environment_40d840(c.flush,2,c.flags_79fcca,c.environment);
    const auto depth=matrices_.depth;
    bool ok=true;
    try{
        driving::pc_matrix_push_load(matrices_,vehicle.sub(0xb0,64));             // 409F90
        PcVehicleModelDraw in{vehicle,std::int32_t(vehicle.i8(0x11)),0,body,-1,std::int32_t(current_mode_),scene_82e7d4};
        std::vector<PcVehicleDrawCall> calls;
        vehicle_select_model_draw_46a560(in,matrices_,calls);
        vehicle_draw_calls_execute(c,calls);
    }catch(const std::exception& e){last_error=std::string("46A560: ")+e.what();ok=false;}
    while(matrices_.depth>depth)driving::pc_matrix_pop(matrices_);               // 40A010
    for(unsigned k=0;k<16;++k)sun.put8(8+k,saved[k]);
    render_environment_40d840(c.flush,2,c.flags_79fcca,c.environment);
    if(ok)++select_car_displays;
    return ok;
}
}
