#include "pc_pmt.hpp"
#include "ports/pecompact_memory.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <map>
#include <set>
#include <cmath>
#include <iomanip>
#include <filesystem>

namespace fs = std::filesystem;
using namespace outrun::assets;

struct CarLayout {
    uint32_t body_obj = 0;
    std::set<uint32_t> shadow_objs;
    std::set<uint32_t> alt_light_objs;
    struct Part {
        uint32_t obj_id = 0xFFFFFFFF;
        std::string name;
        float tx = 0.f, ty = 0.f, tz = 0.f;
        float rx = 0.f, rz = 0.f;
    };
    std::vector<Part> parts;
};

CarLayout extract_layout(const std::vector<uint8_t>& image, int model_idx) {
    auto r32 = [&](uint32_t va) -> uint32_t {
        uint32_t rva = va - 0x400000;
        uint32_t v; std::memcpy(&v, image.data() + rva, 4); return v;
    };
    auto rf32 = [&](uint32_t va) -> float {
        uint32_t rva = va - 0x400000;
        float v; std::memcpy(&v, image.data() + rva, 4); return v;
    };

    uint32_t defn = r32(0x5B2F68 + model_idx * 4);
    CarLayout l;
    l.body_obj = r32(defn + 0x00) & 0xFFFF;
    
    // Shadows: 0x08 (volume 1), 0x0c (plane), 0x10 (volume 2)
    for (uint32_t off : {0x08u, 0x0cu, 0x10u}) {
        uint32_t s = r32(defn + off);
        if (s != 0xFFFFFFFF) l.shadow_objs.insert(s & 0xFFFF);
    }

    // Mirrors at +0x28 (Left) and +0x38 (Right)
    for (int i = 0; i < 2; ++i) {
        uint32_t p_va = defn + 0x28 + i * 0x10;
        uint32_t obj = r32(p_va);
        if (obj != 0xFFFFFFFF) {
            l.parts.push_back({obj & 0xFFFF, (i == 0 ? "Mirror_Left" : "Mirror_Right"),
                               rf32(p_va + 4), rf32(p_va + 8), rf32(p_va + 12)});
        }
    }

    // Steering wheel at +0x48
    uint32_t steer_obj = r32(defn + 0x48);
    if (steer_obj != 0xFFFFFFFF) {
        l.parts.push_back({steer_obj & 0xFFFF, "Steering_Wheel",
                           rf32(defn + 0x4C), rf32(defn + 0x50), rf32(defn + 0x54),
                           rf32(defn + 0x58), 0.f});
    }

    // Interior shifter / extra at +0x18
    uint32_t roof_obj = r32(defn + 0x18);
    if (roof_obj != 0xFFFFFFFF) {
        l.parts.push_back({roof_obj & 0xFFFF, "Interior_Extra",
                           rf32(defn + 0x1C), rf32(defn + 0x20), rf32(defn + 0x24)});
    }

    // Headlights: 0x7c (normal/closed), 0x80 (alt 1), 0x84 (alt 2 / pop-up open)
    uint32_t l0 = r32(defn + 0x7C);
    uint32_t l1 = r32(defn + 0x80);
    uint32_t l2 = r32(defn + 0x84);
    if (l0 != 0xFFFFFFFF) l.parts.push_back({l0 & 0xFFFF, "Headlights_Default"});
    if (l1 != 0xFFFFFFFF && l1 != l0) l.alt_light_objs.insert(l1 & 0xFFFF);
    if (l2 != 0xFFFFFFFF && l2 != l0) l.alt_light_objs.insert(l2 & 0xFFFF);

    // Wheels at +0x88, +0xA8, +0xC8, +0xE8
    const char* w_names[4] = {"Wheel_Front_Left", "Wheel_Front_Right", "Wheel_Rear_Left", "Wheel_Rear_Right"};
    for (int w = 0; w < 4; ++w) {
        uint32_t w_rec = defn + 0x88 + w * 0x20;
        uint32_t w_obj = r32(w_rec);
        if (w_obj != 0xFFFFFFFF) {
            l.parts.push_back({w_obj & 0xFFFF, w_names[w],
                               rf32(w_rec + 0x10), rf32(w_rec + 0x14), rf32(w_rec + 0x18),
                               0.f, rf32(w_rec + 0x1C)});
        }
    }

    return l;
}

const std::vector<std::string> kModelFilenames = {
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

void export_single_car(const std::string& cars_dir, int model_idx, const std::vector<uint8_t>& exe_img) {
    if (model_idx < 0 || model_idx >= kModelFilenames.size()) return;
    std::string base_name = kModelFilenames[model_idx];
    std::string bin_path = cars_dir + "/" + base_name + ".bin";

    if (!fs::exists(bin_path)) {
        std::cerr << "[-] .bin file not found: " << bin_path << std::endl;
        return;
    }

    std::ifstream f(bin_path, std::ios::binary);
    std::vector<uint8_t> pmt_bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    ByteView bv(pmt_bytes);
    PmtView pmt = inspect_pc_pmt(bv);
    CarLayout layout = extract_layout(exe_img, model_idx);

    // 1. Export DDS Textures
    std::string tex_dir = cars_dir + "/" + base_name + "_textures";
    fs::create_directories(tex_dir);
    for (size_t ti = 0; ti < pmt.textures.size(); ++ti) {
        const auto& tex = pmt.textures[ti];
        if (tex.is_dds && tex.available_bytes >= tex.dds.file_bytes) {
            std::string out_dds = tex_dir + "/tex_" + (ti < 10 ? "0" : "") + std::to_string(ti) + ".dds";
            std::ofstream df(out_dds, std::ios::binary);
            df.write(reinterpret_cast<const char*>(pmt.video.data + tex.data_offset), tex.dds.file_bytes);
        }
    }

    // 2. Export MTL file
    std::string mtl_path = cars_dir + "/" + base_name + ".mtl";
    std::ofstream mtl(mtl_path);
    mtl << "# Material library for " << base_name << "\n\n";

    std::map<std::pair<uint32_t, uint32_t>, std::string> mat_names;
    for (size_t oi = 0; oi < pmt.objects.size(); ++oi) {
        const auto& obj = pmt.objects[oi];
        for (size_t mi = 0; mi < obj.materials.size(); ++mi) {
            const auto& m = obj.materials[mi];
            std::string mname = "Mat_Obj" + std::to_string(oi) + "_M" + std::to_string(mi);
            mat_names[{uint32_t(oi), uint32_t(mi)}] = mname;
            mtl << "newmtl " << mname << "\n";
            mtl << "Kd " << m.diffuse[0] << " " << m.diffuse[1] << " " << m.diffuse[2] << "\n";
            mtl << "d " << m.diffuse[3] << "\n";
            mtl << "illum 2\n";
            if (m.texture_indices[0] >= 0 && m.texture_indices[0] < pmt.textures.size()) {
                int tid = m.texture_indices[0];
                mtl << "map_Kd " << base_name << "_textures/tex_" << (tid < 10 ? "0" : "") << tid << ".dds\n";
            }
            mtl << "\n";
        }
    }

    // 3. Export OBJ file
    std::string obj_path = cars_dir + "/" + base_name + ".obj";
    std::ofstream out(obj_path);
    out << "# OutRun 2006 Coast 2 Coast Model Exporter\n";
    out << "# Car: " << base_name << " (Model " << model_idx << ")\n";
    out << "mtllib " << base_name << ".mtl\n\n";

    std::map<uint32_t, CarLayout::Part> part_map;
    for (const auto& p : layout.parts) {
        part_map[p.obj_id] = p;
    }

    size_t v_offset = 1;
    for (size_t oi = 0; oi < pmt.objects.size(); ++oi) {
        if (layout.shadow_objs.count(oi) || layout.alt_light_objs.count(oi)) {
            continue;
        }

        std::vector<PreviewMesh> meshes;
        try {
            meshes = decode_static_object(pmt, oi);
        } catch (...) {
            continue;
        }
        if (meshes.empty()) continue;

        std::string obj_name;
        float tx = 0.f, ty = 0.f, tz = 0.f;
        float rx = 0.f;

        if (part_map.count(oi)) {
            const auto& pt = part_map[oi];
            obj_name = pt.name;
            tx = pt.tx; ty = pt.ty; tz = pt.tz;
            rx = pt.rx;
        } else if (oi == layout.body_obj) {
            obj_name = "Car_Body";
        } else if (oi == 4) {
            obj_name = "Interior_Cockpit";
        } else if (oi == 7) {
            obj_name = "Interior_Rollcage";
        } else if (oi == 14) {
            obj_name = "Glass_Windshield";
        } else if (oi == 15) {
            obj_name = "Glass_Rear";
        } else {
            obj_name = "Part_Obj" + std::to_string(oi);
        }

        out << "o " << obj_name << "\n";

        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const auto& m = meshes[mi];
            auto mat_it = mat_names.find({uint32_t(oi), m.material});
            if (mat_it != mat_names.end()) {
                out << "usemtl " << mat_it->second << "\n";
            }

            for (const auto& v : m.vertices) {
                float px = v.position[0];
                float py = v.position[1];
                float pz = v.position[2];

                if (std::abs(rx) > 1e-5f) {
                    float cos_rx = std::cos(rx);
                    float sin_rx = std::sin(rx);
                    float ny = py * cos_rx - pz * sin_rx;
                    float nz = py * sin_rx + pz * cos_rx;
                    py = ny; pz = nz;
                }

                px += tx; py += ty; pz += tz;
                out << "v " << std::fixed << std::setprecision(6) << px << " " << py << " " << pz << "\n";
            }
            for (const auto& v : m.vertices) {
                out << "vt " << std::fixed << std::setprecision(6) << v.uv[0] << " " << (1.0f - v.uv[1]) << "\n";
            }
            for (const auto& v : m.vertices) {
                out << "vn " << std::fixed << std::setprecision(6) << v.normal[0] << " " << v.normal[1] << " " << v.normal[2] << "\n";
            }
            for (size_t ti = 0; ti < m.indices.size(); ti += 3) {
                uint32_t i0 = v_offset + m.indices[ti];
                uint32_t i1 = v_offset + m.indices[ti+1];
                uint32_t i2 = v_offset + m.indices[ti+2];
                out << "f " << i0 << "/" << i0 << "/" << i0 << " "
                            << i1 << "/" << i1 << "/" << i1 << " "
                            << i2 << "/" << i2 << "/" << i2 << "\n";
            }
            v_offset += m.vertices.size();
        }
    }
    std::cout << "[+] Successfully exported " << base_name << " (" << v_offset - 1 << " vertices) to " << obj_path << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <OR2006C2C.EXE> <Cars_Directory> [model_idx]" << std::endl;
        return 1;
    }
    std::string exe_path = argv[1];
    std::string cars_dir = argv[2];

    std::ifstream ef(exe_path, std::ios::binary);
    std::vector<uint8_t> exe_bytes((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> exe_img;
    if (!outrun_pecompact::decompress(exe_bytes.data(), exe_bytes.size(), exe_img)) {
        std::cerr << "Failed to decompress EXE" << std::endl;
        return 1;
    }

    if (argc >= 4) {
        int m = std::stoi(argv[3]);
        export_single_car(cars_dir, m, exe_img);
    } else {
        std::cout << "Exporting all available cars in " << cars_dir << "..." << std::endl;
        for (int m = 0; m < 15; ++m) { // Base models 0 to 14
            export_single_car(cars_dir, m, exe_img);
        }
    }
    return 0;
}
