#!/usr/bin/env python3
"""
==============================================================================
 OutRun 2006: Coast 2 Coast - Preparador Automatico de Assets para PS Vita
==============================================================================
 Coloca este script en la raiz del juego (junto a Cars/, mv/ y OR2006C2C.EXE):
     python3 prepare_vita_assets.py                # Ejecuta ambos pasos
     python3 prepare_vita_assets.py --only-intro   # Solo pantalla de intro (ffmpeg)
     python3 prepare_vita_assets.py --only-cars    # Solo coches de jugador (Blender)

 Que genera (en 'vita_assets/' por defecto, sin tocar los archivos originales):
 1. Intro (mv/TitleScreen.bmp + mv/TitleScreen.ogg):
    Extrae 1 frame (segundo 4) y el audio Ogg Vorbis 48 kHz estereo de mv/TitleScreen.bik.
 2. Coches de jugador (Cars/obj_plcar*_pmt.sz):
    Reduce triangulos con Blender Decimate (COLLAPSE) protegiendo cristales/calcomanias
    (_Pass1) y reinyectando en formato nativo PMT + zlib nivel 9.

 Al terminar, arrastra las carpetas Cars/ y mv/ de dentro de 'vita_assets/'
 encima de ux0:data/outrun2006/ en tu PS Vita.
==============================================================================
"""

import argparse
import sys
import os
import subprocess
import glob
import shutil
import zlib
import struct
import datetime

# Rutas estándar donde suele estar instalado Blender en Windows
BLENDER_CANDIDATE_PATHS = [
    r"C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 5\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.3\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.1\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.0\blender.exe",
    r"C:\Program Files (x86)\Blender Foundation\Blender 5.0\blender.exe",
]

def find_blender():
    """Busca el ejecutable de Blender en las rutas habituales o en el PATH."""
    for path in BLENDER_CANDIDATE_PATHS:
        if os.path.exists(path):
            return path

    which_blender = shutil.which("blender")
    if which_blender:
        return which_blender
    return None

def decompress_sz(data):
    """Descomprime un archivo .sz probando offsets comunes."""
    if len(data) < 16:
        return None
    for offset in [0, 4, 8, 12, 16]:
        try:
            return zlib.decompress(data[offset:])
        except zlib.error:
            pass
    for offset in [0, 4, 8, 12, 16]:
        try:
            return zlib.decompress(data[offset:], -zlib.MAX_WBITS)
        except zlib.error:
            pass
    return None

def extract_pmt_to_obj(pmt_bytes, output_obj_path, extract_textures=False):
    """Extrae las mallas de un PMT a OBJ y MTL en coordenadas locales nativas (0,0,0)."""
    if len(pmt_bytes) < 16:
        return False

    system_size, video_size = struct.unpack('<II', pmt_bytes[8:16])
    if 16 + system_size + video_size > len(pmt_bytes):
        return False

    s = pmt_bytes[16:16+system_size]
    v = pmt_bytes[16+system_size:16+system_size+video_size]

    obj_count = struct.unpack('<I', s[4:8])[0]
    tex_count = struct.unpack('<I', s[8:12])[0]

    base_dir = os.path.dirname(output_obj_path) or "."
    base_name = os.path.splitext(os.path.basename(output_obj_path))[0]
    tex_dir = os.path.join(base_dir, f"{base_name}_textures")

    # Extraer texturas DDS solo si se solicita explícitamente
    if extract_textures and tex_count > 0:
        os.makedirs(tex_dir, exist_ok=True)
        header = 0x18 + obj_count * 0x3C
        tex_records = header + tex_count * 4 + 4 + 0xC

        for t in range(tex_count):
            rec = tex_records + t * 0x14
            if rec + 0x14 <= len(s):
                source = struct.unpack('<I', s[rec+4:rec+8])[0]
                size = struct.unpack('<I', s[rec+0x10:rec+0x14])[0] + 0x80
                if source + size <= len(v):
                    dds_bytes = v[source:source+size]
                    if dds_bytes[:4] == b'DDS ':
                        out_dds = os.path.join(tex_dir, f"tex_{t:02d}.dds")
                        with open(out_dds, 'wb') as df:
                            df.write(dds_bytes)

    # Materiales
    materials_dict = {}
    for oi in range(obj_count):
        start = 24 + oi * 60
        if start + 60 > len(s):
            break
        q = struct.unpack('<15I', s[start:start+60])
        if q[5] + 52 > len(s):
            continue
        h = struct.unpack('<13I', s[q[5]:q[5]+52])
        num_mats = h[11]
        num_colors = h[12]

        for mi in range(num_mats):
            off = q[13] + mi * 88
            if off + 88 > len(s):
                continue
            ci, attr = struct.unpack('<iI', s[off:off+8])
            tex_idx = struct.unpack('<i', s[off+24:off+28])[0]
            diffuse = [1.0, 1.0, 1.0, 1.0]
            if 0 <= ci < num_colors:
                coff = q[14] + ci * 72
                if coff + 16 <= len(s):
                    diffuse = list(struct.unpack('<4f', s[coff:coff+16]))

            mat_name = f"Mat_Obj{oi}_M{mi}"
            materials_dict[mat_name] = {'diffuse': diffuse, 'tex_idx': tex_idx}

    all_verts = []
    all_uvs = []
    all_norms = []
    objects_data = []

    # Extraer geometría de cada objeto
    for oi in range(obj_count):
        start = 24 + oi * 60
        if start + 60 > len(s):
            break
        q = struct.unpack('<15I', s[start:start+60])
        if q[5] + 52 > len(s):
            continue
        h = struct.unpack('<13I', s[q[5]:q[5]+52])
        num_formats = h[9]
        num_mat_groups = h[10]

        formats = []
        for fi in range(num_formats):
            off = q[12] + fi * 44
            if off + 44 > len(s):
                continue
            streams = struct.unpack('<I', s[off:off+4])[0]
            idx_b, vtx_b, fvf, stride = struct.unpack('<4I', s[off+24:off+40])
            ip = struct.unpack('<I', s[q[1]+fi*4:q[1]+fi*4+4])[0]
            idx_offset = struct.unpack('<I', s[ip+4:ip+8])[0]
            vp = struct.unpack('<I', s[q[2]+fi*16:q[2]+fi*16+4])[0]
            vert_offset = struct.unpack('<I', s[vp+4:vp+8])[0]
            formats.append({
                'stride': stride, 'fvf': fvf, 'vert_bytes': vtx_b,
                'idx_offset': idx_offset, 'vert_offset': vert_offset
            })

        mat_groups = []
        for mgi in range(num_mat_groups):
            off = q[10] + mgi * 32
            if off + 16 > len(s):
                continue
            base_v, mat, first, count = struct.unpack('<IIII', s[off:off+16])
            po = q[11] + first * 16
            prims = []
            for pi in range(count):
                proff = po + pi * 16
                if proff + 16 <= len(s):
                    ptype, pstart, pcount, ppoly = struct.unpack('<IIII', s[proff:proff+16])
                    prims.append({'type': ptype, 'start': pstart, 'count': pcount})
            mat_groups.append({'base_v': base_v, 'mat': mat, 'prims': prims})

        obj_batches_p0 = {}
        obj_batches_p1 = {}

        # Pre-cargar vertices una sola vez por formato (evita duplicar vertices en cada vertex group)
        fmt_v_start = {}
        for fi, f in enumerate(formats):
            stride = f['stride']
            fvf = f['fvf']
            if stride == 0:
                continue
            has_norm = bool(fvf & 0x10)
            has_color = bool(fvf & 0x40)
            num_tex = (fvf >> 8) & 0xF

            num_v = f['vert_bytes'] // stride
            fmt_v_start[fi] = len(all_verts)
            vb = v[f['vert_offset'] : f['vert_offset'] + f['vert_bytes']]

            for vi in range(num_v):
                p = vi * stride
                if p + 12 > len(vb):
                    break
                vx, vy, vz = struct.unpack('<fff', vb[p:p+12])
                all_verts.append((vx, vy, vz))

                cur_off = 12
                if has_norm and p + cur_off + 12 <= len(vb):
                    nx, ny, nz = struct.unpack('<fff', vb[p+cur_off:p+cur_off+12])
                    all_norms.append((nx, ny, nz))
                    cur_off += 12
                else:
                    all_norms.append((0.0, 1.0, 0.0))

                if has_color:
                    cur_off += 4

                if num_tex > 0 and p + cur_off + 8 <= len(vb):
                    tu, tv = struct.unpack('<ff', vb[p+cur_off:p+cur_off+8])
                    all_uvs.append((tu, 1.0 - tv))
                else:
                    all_uvs.append((0.0, 0.0))

        vg_count = (q[10] - q[9]) // 20
        for gi in range(vg_count):
            off = q[9] + gi * 20
            if off + 4 > len(s):
                continue
            fmt_idx = struct.unpack('<I', s[off:off+4])[0]
            if fmt_idx not in fmt_v_start:
                continue
            f = formats[fmt_idx]
            v_start = fmt_v_start[fmt_idx]

            for a in range(2):
                if off + 16 > len(s):
                    continue
                first_mg = struct.unpack('<I', s[off+4+a*4:off+8+a*4])[0]
                count_mg = struct.unpack('<I', s[off+12+a*4:off+16+a*4])[0]
                target_batches = obj_batches_p0 if a == 0 else obj_batches_p1

                for mgi in range(first_mg, first_mg + count_mg):
                    if mgi >= len(mat_groups):
                        continue
                    mg = mat_groups[mgi]
                    base_v = mg['base_v']
                    mat_idx = mg['mat']
                    mat_name = f"Mat_Obj{oi}_MG{mgi:02d}_M{mat_idx:02d}"
                    if mat_name not in target_batches:
                        target_batches[mat_name] = []

                    for prim in mg['prims']:
                        ptype = prim['type']
                        pstart = prim['start']
                        pcount = prim['count']
                        num_indices = pcount * 3 if ptype == 5 else (pcount + 2 if ptype == 6 else pcount * 4)

                        idx_byte_start = f['idx_offset'] + pstart * 2
                        idx_bytes = s[idx_byte_start : idx_byte_start + num_indices * 2]
                        if len(idx_bytes) < num_indices * 2:
                            continue
                        indices = struct.unpack(f'<{num_indices}H', idx_bytes)

                        if ptype == 6:  # Triangle Strip
                            for ti in range(len(indices) - 2):
                                i0, i1, i2 = indices[ti], indices[ti+1], indices[ti+2]
                                if i0 == i1 or i1 == i2 or i0 == i2:
                                    continue
                                if ti & 1:
                                    i0, i1 = i1, i0
                                target_batches[mat_name].append((v_start + i0 + base_v + 1,
                                                                 v_start + i1 + base_v + 1,
                                                                 v_start + i2 + base_v + 1))
                        elif ptype == 5:  # Triangle List
                            for ti in range(0, len(indices) - 2, 3):
                                i0, i1, i2 = indices[ti], indices[ti+1], indices[ti+2]
                                if i0 == i1 or i1 == i2 or i0 == i2:
                                    continue
                                target_batches[mat_name].append((v_start + i0 + base_v + 1,
                                                                 v_start + i1 + base_v + 1,
                                                                 v_start + i2 + base_v + 1))

        if any(len(faces) > 0 for faces in obj_batches_p0.values()):
            objects_data.append((f"Part_{oi:02d}", obj_batches_p0))
        if any(len(faces) > 0 for faces in obj_batches_p1.values()):
            objects_data.append((f"Part_{oi:02d}_Pass1", obj_batches_p1))

    # Escritura de MTL
    output_mtl_path = os.path.splitext(output_obj_path)[0] + ".mtl"
    mtl_filename = os.path.basename(output_mtl_path)
    with open(output_mtl_path, 'w', encoding='utf-8') as mtl_out:
        mtl_out.write(f"# OutRun 2006 Coast 2 Coast - Materials\n")
        seen_mtls = set()
        for obj_name, obj_batches in objects_data:
            for mat_name in obj_batches:
                if mat_name in seen_mtls:
                    continue
                seen_mtls.add(mat_name)
                # Extraer oi y mat_idx del nombre: Mat_Obj{oi}_MG{mgi}_M{mi}
                parts = mat_name.split('_')
                oi = int(parts[1][3:])
                mi = int(parts[3][1:])
                base_mat_key = f"Mat_Obj{oi}_M{mi}"
                minfo = materials_dict.get(base_mat_key, {'diffuse': [1.0, 1.0, 1.0, 1.0], 'tex_idx': -1})
                diff = minfo['diffuse']
                tex_idx = minfo['tex_idx']

                mtl_out.write(f"newmtl {mat_name}\n")
                mtl_out.write(f"Ka 1.000 1.000 1.000\n")
                mtl_out.write(f"Kd {diff[0]:.4f} {diff[1]:.4f} {diff[2]:.4f}\n")
                mtl_out.write(f"d {diff[3]:.4f}\n")
                mtl_out.write(f"illum 2\n")
                if tex_idx >= 0 and tex_idx < tex_count:
                    mtl_out.write(f"map_Kd {base_name}_textures/tex_{tex_idx:02d}.dds\n")
                mtl_out.write("\n")

    # Escritura de OBJ
    with open(output_obj_path, 'w', encoding='utf-8') as out:
        out.write(f"# OutRun 2006 Universal Geometry (Local Coordinates 0,0,0)\n")
        out.write(f"mtllib {mtl_filename}\n\n")
        for vx, vy, vz in all_verts:
            out.write(f"v {vx:.6f} {vy:.6f} {vz:.6f}\n")
        out.write("\n")
        for tu, tv in all_uvs:
            out.write(f"vt {tu:.6f} {tv:.6f}\n")
        out.write("\n")
        for nx, ny, nz in all_norms:
            out.write(f"vn {nx:.6f} {ny:.6f} {nz:.6f}\n")
        out.write("\n")
        for obj_name, obj_batches in objects_data:
            out.write(f"o {obj_name}\n")
            out.write(f"g {obj_name}\n")
            for mat_name, faces in obj_batches.items():
                if not faces:
                    continue
                out.write(f"usemtl {mat_name}\n")
                for i0, i1, i2 in faces:
                    out.write(f"f {i0}/{i0}/{i0} {i1}/{i1}/{i1} {i2}/{i2}/{i2}\n")
            out.write("\n")

    return True

def parse_obj(obj_path):
    """Parsea geometría del OBJ exportado por Blender."""
    verts = []
    uvs = []
    norms = []
    objects = {}
    cur_obj = None
    cur_mat = None

    with open(obj_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            tag = parts[0]

            if tag == 'v':
                verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif tag == 'vt':
                uvs.append((float(parts[1]), float(parts[2])))
            elif tag == 'vn':
                norms.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif tag == 'o':
                cur_obj = parts[1]
                objects[cur_obj] = {}
                cur_mat = None
            elif tag == 'usemtl':
                cur_mat = parts[1]
                if cur_obj and cur_mat not in objects[cur_obj]:
                    objects[cur_obj][cur_mat] = []
            elif tag == 'f':
                if cur_obj and cur_mat:
                    fverts = []
                    for p in parts[1:]:
                        v_parts = p.split('/')
                        vi = int(v_parts[0]) - 1 if v_parts[0] else 0
                        ti = int(v_parts[1]) - 1 if len(v_parts) > 1 and v_parts[1] else 0
                        ni = int(v_parts[2]) - 1 if len(v_parts) > 2 and v_parts[2] else 0
                        fverts.append((vi, ti, ni))

                    if len(fverts) == 3:
                        objects[cur_obj][cur_mat].append(tuple(fverts))
                    elif len(fverts) == 4:
                        objects[cur_obj][cur_mat].append((fverts[0], fverts[1], fverts[2]))
                        objects[cur_obj][cur_mat].append((fverts[0], fverts[2], fverts[3]))
                    elif len(fverts) > 4:
                        for k in range(1, len(fverts) - 1):
                            objects[cur_obj][cur_mat].append((fverts[0], fverts[k], fverts[k+1]))

    return verts, uvs, norms, objects

def find_matching_objs(oi, parsed_objs):
    """Encuentra TODOS los objetos del OBJ correspondientes al objeto oi (incluyendo Pass0 y Pass1)."""
    exact = {
        f"Part_{oi:02d}", f"Part_{oi:02d}_Pass1",
        f"Part_{oi}", f"Part_{oi}_Pass1",
        f"Car_Part_{oi:02d}", f"Car_Part_{oi:02d}_Pass1",
    }
    prefixes = (f"Part_{oi:02d}_", f"Part_{oi:02d}.", f"Car_Part_{oi:02d}_", f"Car_Part_{oi:02d}.")
    matched = []
    for name in parsed_objs:
        if name in exact or name.startswith(prefixes):
            matched.append(name)
    if not matched and oi == 0:
        for name in parsed_objs:
            if 'body' in name.lower():
                matched.append(name)
    return matched

def build_triangle_strip(triangles):
    """Convierte una lista de triangulos en Triangle Strips encadenados (greedy) con puentes minimos."""
    if not triangles:
        return []

    # Eliminar triangulos degenerados de entrada
    tris = [t for t in triangles if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]]
    if not tris:
        return []

    # Mapa de arista dirigida (u, v) -> lista de (tid, tercer_vertice)
    edge_map = {}
    for tid, (a, b, c) in enumerate(tris):
        edge_map.setdefault((a, b), []).append((tid, c))
        edge_map.setdefault((b, c), []).append((tid, a))
        edge_map.setdefault((c, a), []).append((tid, b))

    used = [False] * len(tris)
    strip = []

    def pop_neighbor(u, v):
        lst = edge_map.get((u, v))
        if not lst:
            return None
        for tid, w in lst:
            if not used[tid]:
                return tid, w
        return None

    for start_tid in range(len(tris)):
        if used[start_tid]:
            continue
        used[start_tid] = True
        a, b, c = tris[start_tid]

        # Rotar el triangulo inicial para que su primera arista de salida tenga vecino si existe
        for ra, rb, rc in ((a, b, c), (b, c, a), (c, a, b)):
            if pop_neighbor(rc, rb) is not None:
                a, b, c = ra, rb, rc
                break

        if not strip:
            strip.extend([a, b, c])
        else:
            last = strip[-1]
            # Si len(strip) es par, el nuevo triangulo empieza en indice par (a, b, c);
            # si es impar, empieza en indice impar y D3D9 invierte los 2 primeros vertices -> (b, a, c)
            if len(strip) % 2 == 0:
                strip.extend([last, a, a, b, c])
            else:
                strip.extend([last, b, b, a, c])

        # Encadenar vecinos compartiendo arista (1 solo indice por triangulo)
        while True:
            next_i = len(strip) - 2
            if next_i & 1:
                u, v = strip[-1], strip[-2]
            else:
                u, v = strip[-2], strip[-1]
            nxt = pop_neighbor(u, v)
            if nxt is None:
                break
            ntid, nw = nxt
            used[ntid] = True
            strip.append(nw)

    return strip

def find_matching_mat(mgi, obj_mats):
    # Prioridad: busqueda por Material Group (MG) para evitar cruce de geometrias
    patterns = [f"_MG{mgi:02d}_", f"MG{mgi:02d}_", f"_MG{mgi}_", f"MG{mgi}_"]
    for p in patterns:
        for mk in obj_mats:
            if p in mk:
                return mk
    # Fallback solo para OBJs antiguos que no tenian prefijo MG
    if not any("MG" in mk for mk in obj_mats):
        for mk in obj_mats:
            if f"_M{mgi:02d}" in mk or f"_M{mgi}" in mk:
                return mk
    return None

def obj_to_pmt_bytes(template_bytes, obj_path):
    """Reinyecta el OBJ decimado en la estructura PMT original preservando texturas y metadatos."""
    pmt = template_bytes
    # Solo descomprimir si template_bytes no es ya un PMT descomprimido valido
    is_valid_raw_pmt = False
    if len(template_bytes) >= 16:
        _oc, _tc, _ss, _vs = struct.unpack('<IIII', template_bytes[:16])
        if _oc > 0 and _ss > 0 and (16 + _ss + _vs) <= len(template_bytes) and (16 + _ss + _vs) >= len(template_bytes) - 32:
            is_valid_raw_pmt = True
    if not is_valid_raw_pmt:
        decomp = decompress_sz(template_bytes)
        if decomp is not None and len(decomp) >= 16:
            pmt = decomp

    obj_count, tex_count, sys_sz, vid_sz = struct.unpack('<IIII', pmt[:16])
    s = bytearray(pmt[16 : 16 + sys_sz])
    v = bytearray(pmt[16 + sys_sz : 16 + sys_sz + vid_sz])

    header = 0x18 + obj_count * 0x3C
    tex_records = header + tex_count * 4 + 4 + 0xC
    tex_end = 0
    for t in range(tex_count):
        rec = tex_records + t * 0x14
        source = struct.unpack('<I', s[rec+4:rec+8])[0]
        size = struct.unpack('<I', s[rec+0x10:rec+0x14])[0] + 0x80
        tex_end = max(tex_end, source + size)

    new_v = bytearray(v[:tex_end])
    while len(new_v) % 16 != 0:
        new_v.append(0)

    min_idx_off = len(s)
    for oi in range(obj_count):
        start = 24 + oi * 60
        q = struct.unpack('<15I', s[start:start+60])
        h = struct.unpack('<13I', s[q[5]:q[5]+52])
        for fi in range(h[9]):
            ip = struct.unpack('<I', s[q[1]+fi*4:q[1]+fi*4+4])[0]
            io = struct.unpack('<I', s[ip+4:ip+8])[0]
            if io < min_idx_off:
                min_idx_off = io

    new_s_meta = bytearray(s[:min_idx_off])
    new_s_indices = bytearray()

    obj_verts, obj_uvs, obj_norms, parsed_objs = parse_obj(obj_path)

    for oi in range(obj_count):
        start = 24 + oi * 60
        q = struct.unpack('<15I', new_s_meta[start:start+60])
        h = struct.unpack('<13I', new_s_meta[q[5]:q[5]+52])
        num_formats = h[9]
        num_mat_groups = h[10]
        vg_count = (q[10] - q[9]) // 20

        mg_to_fmt = {}
        pass1_mgs = set()
        for gi in range(vg_count):
            vgoff = q[9] + gi * 20
            fmt = struct.unpack('<I', new_s_meta[vgoff:vgoff+4])[0]
            for a in range(2):
                fmg = struct.unpack('<I', new_s_meta[vgoff+4+a*4:vgoff+8+a*4])[0]
                cmg = struct.unpack('<I', new_s_meta[vgoff+12+a*4:vgoff+16+a*4])[0]
                if fmg < 0x80000000 and cmg < 0x80000000:
                    for mgi in range(fmg, fmg + cmg):
                        mg_to_fmt[mgi] = fmt
                        if a == 1:
                            pass1_mgs.add(mgi)

        matched_names = find_matching_objs(oi, parsed_objs)

        if not matched_names:
            for fi in range(num_formats):
                off = q[12] + fi * 44
                idx_b = struct.unpack('<I', new_s_meta[off+24:off+28])[0]
                vtx_b = struct.unpack('<I', new_s_meta[off+28:off+32])[0]

                ip = struct.unpack('<I', new_s_meta[q[1]+fi*4:q[1]+fi*4+4])[0]
                orig_io = struct.unpack('<I', s[ip+4:ip+8])[0]
                orig_idx_data = s[orig_io : orig_io + idx_b]

                while len(new_s_indices) % 2 != 0:
                    new_s_indices.append(0)
                cur_new_io = min_idx_off + len(new_s_indices)
                new_s_meta[ip+4:ip+8] = struct.pack('<I', cur_new_io)
                new_s_indices.extend(orig_idx_data)

                vp = struct.unpack('<I', new_s_meta[q[2]+fi*16:q[2]+fi*16+4])[0]
                orig_vo = struct.unpack('<I', s[vp+4:vp+8])[0]
                orig_vtx_data = v[orig_vo : orig_vo + vtx_b]

                while len(new_v) % 16 != 0:
                    new_v.append(0)
                cur_new_vo = len(new_v)
                new_s_meta[vp+4:vp+8] = struct.pack('<I', cur_new_vo)
                new_v.extend(orig_vtx_data)
            continue

        obj_mats = {}
        for mn in matched_names:
            for mk, tris_list in parsed_objs[mn].items():
                if mk not in obj_mats:
                    obj_mats[mk] = []
                obj_mats[mk].extend(tris_list)

        for fi in range(num_formats):
            off = q[12] + fi * 44
            streams, old_idx_b, old_vtx_b, fvf, stride, shader = struct.unpack('<6I', new_s_meta[off+20:off+44])
            has_norm = bool(fvf & 0x10)
            has_color = bool(fvf & 0x40)
            num_tex = (fvf >> 8) & 0xF

            vert_map = {}
            packed_verts = bytearray()
            fmt_indices = []

            for mgi in range(num_mat_groups):
                if mg_to_fmt.get(mgi, 0) != fi:
                    continue

                mg_off = q[10] + mgi * 32
                old_base_v, mat, first, count = struct.unpack('<IIII', new_s_meta[mg_off:mg_off+16])
                new_s_meta[mg_off:mg_off+4] = struct.pack('<I', 0)

                mat_key_match = find_matching_mat(mgi, obj_mats)
                tris = obj_mats.get(mat_key_match, []) if mat_key_match else []

                is_pass1 = (mgi in pass1_mgs)
                tri_indices = []
                for tri in tris:
                    mapped_tri = []
                    for vert in tri:
                        vi, ti, ni = vert
                        key = (vi, ti, ni, is_pass1)
                        if key not in vert_map:
                            new_idx = len(vert_map)
                            vert_map[key] = new_idx

                            vx, vy, vz = obj_verts[vi]
                            if has_norm:
                                nx, ny, nz = obj_norms[ni] if ni < len(obj_norms) else (0.0, 1.0, 0.0)
                            else:
                                nx, ny, nz = (0.0, 1.0, 0.0)

                            if is_pass1:
                                # Offset calcomanias/pass1 ligeramente sobre su normal (+3.5mm)
                                # para evitar que la carroceria decimada perfore la calcomania
                                vx += nx * 0.0035
                                vy += ny * 0.0035
                                vz += nz * 0.0035

                            packed_verts.extend(struct.pack('<fff', vx, vy, vz))

                            if has_norm:
                                packed_verts.extend(struct.pack('<fff', nx, ny, nz))

                            if has_color:
                                packed_verts.extend(b'\xff\xff\xff\xff')

                            for txi in range(num_tex):
                                tu, tv = obj_uvs[ti] if ti < len(obj_uvs) else (0.0, 0.0)
                                packed_verts.extend(struct.pack('<ff', tu, 1.0 - tv))

                        mapped_tri.append(vert_map[key])
                    tri_indices.append(tuple(mapped_tri))

                # Convertir a Triangle Strip (Type 6 nativo de OutRun 2006)
                strip = build_triangle_strip(tri_indices)
                pstart = len(fmt_indices)
                fmt_indices.extend(strip)

                pcount = len(strip) - 2 if len(strip) >= 3 else 0
                po = q[11] + first * 16
                new_s_meta[po:po+16] = struct.pack('<IIII', 6 if pcount > 0 else 0, pstart, pcount, len(tris))
                new_s_meta[mg_off+12:mg_off+16] = struct.pack('<I', 1 if pcount > 0 else 0)
                if count > 1:
                    for extra in range(1, count):
                        epo = po + extra * 16
                        if epo + 16 <= len(new_s_meta):
                            new_s_meta[epo:epo+16] = b'\x00' * 16

            idx_bytes = struct.pack(f'<{len(fmt_indices)}H', *fmt_indices)
            while len(new_s_indices) % 2 != 0:
                new_s_indices.append(0)
            cur_new_io = min_idx_off + len(new_s_indices)
            ip = struct.unpack('<I', new_s_meta[q[1]+fi*4:q[1]+fi*4+4])[0]
            new_s_meta[ip+4:ip+8] = struct.pack('<I', cur_new_io)
            new_s_meta[off+24:off+28] = struct.pack('<I', len(idx_bytes))
            new_s_indices.extend(idx_bytes)

            while len(new_v) % 16 != 0:
                new_v.append(0)
            cur_new_vo = len(new_v)
            vp = struct.unpack('<I', new_s_meta[q[2]+fi*16:q[2]+fi*16+4])[0]
            new_s_meta[vp+4:vp+8] = struct.pack('<I', cur_new_vo)
            new_s_meta[off+28:off+32] = struct.pack('<I', len(packed_verts))
            new_v.extend(packed_verts)

    final_s = new_s_meta + new_s_indices
    while len(final_s) % 16 != 0:
        final_s.append(0)
    while len(new_v) % 16 != 0:
        new_v.append(0)

    header = struct.pack('<IIII', obj_count, tex_count, len(final_s), len(new_v))
    return header + final_s + new_v

# Script auxiliar que se inyecta en Blender 5.0 para decimar sin interfaz
BLENDER_WORKER_SCRIPT = """
import bpy
import sys

def main():
    args = sys.argv[sys.argv.index("--") + 1:]
    input_obj = args[0]
    output_obj = args[1]
    ratio = float(args[2])

    # Limpiar escena
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Importar OBJ en Blender 5.0 / 4.x
    if hasattr(bpy.ops.wm, "obj_import"):
        bpy.ops.wm.obj_import(filepath=input_obj)
    else:
        bpy.ops.import_scene.obj(filepath=input_obj)

    # Decimar cada objeto malla conservando UVs y materiales
    for obj in bpy.context.scene.objects:
        if obj.type == 'MESH':
            poly_count = len(obj.data.polygons)
            # Salvaguarda esencial: Proteger de la decimación partes transparentes, calcomanías,
            # cristales (Pass1) y piezas con pocos polígonos para evitar colapso a 0 o pérdida de transparencia
            if '_Pass1' in obj.name or poly_count <= 8:
                continue

            bpy.context.view_layer.objects.active = obj

            mod = obj.modifiers.new(name="Decimate_Auto", type='DECIMATE')
            mod.decimate_type = 'COLLAPSE'
            mod.ratio = ratio
            mod.use_symmetry = False
            try:
                bpy.ops.object.modifier_apply(modifier=mod.name)
            except Exception as e:
                print(f"Error aplicando modifier en {obj.name}: {e}")

    # Exportar OBJ optimizado
    if hasattr(bpy.ops.wm, "obj_export"):
        bpy.ops.wm.obj_export(filepath=output_obj, export_materials=True, export_uv=True, export_normals=True)
    else:
        bpy.ops.export_scene.obj(filepath=output_obj, use_materials=True, use_uvs=True, use_normals=True)

    print("[BLENDER_SUCCESS]")

if __name__ == "__main__":
    main()
"""

def process_pipeline(sz_files, ratio, output_dir, blender_exe, log_dir=None):
    os.makedirs(output_dir, exist_ok=True)
    temp_dir = os.path.join(output_dir, "_temp_work")
    os.makedirs(temp_dir, exist_ok=True)

    worker_py = os.path.join(temp_dir, "blender_worker.py")
    with open(worker_py, 'w', encoding='utf-8') as f:
        f.write(BLENDER_WORKER_SCRIPT)

    total_files = len(sz_files)
    conserve_pct = ratio * 100.0
    reduce_pct = (1.0 - ratio) * 100.0
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    print(f"\n=======================================================")
    print(f"[*] Iniciando optimizacion de {total_files} archivo(s)...")
    print(f"[*] Ratio de conservacion de poligonos: {conserve_pct:.1f}% (Reduccion: {reduce_pct:.1f}%)")
    print(f"[*] Carpeta destino: {output_dir}")
    print(f"=======================================================\n")

    log_path = os.path.join(log_dir or output_dir, "registro_optimizacion.txt")
    log_entries = []
    successful_count = 0

    for idx, sz_path in enumerate(sz_files, 1):
        filename = os.path.basename(sz_path)
        base_name = os.path.splitext(filename)[0]
        print(f"[{idx}/{total_files}] Procesando: {filename}...")

        # 1. Leer y descomprimir .sz original
        with open(sz_path, 'rb') as f:
            raw_sz = f.read()
        orig_sz_size = len(raw_sz)

        decomp = decompress_sz(raw_sz)
        pmt_data = decomp if decomp is not None else raw_sz

        # 2. Extraer a OBJ temporal
        raw_obj = os.path.join(temp_dir, f"{base_name}.obj")
        if not extract_pmt_to_obj(pmt_data, raw_obj, extract_textures=False):
            print(f"  [-] Error extrayendo PMT de {filename}. Saltando...")
            continue

        # 3. Decimar con Blender en background
        decimated_obj = os.path.join(temp_dir, f"{base_name}_decimado.obj")
        cmd = [
            blender_exe,
            "--background",
            "--python", worker_py,
            "--",
            raw_obj,
            decimated_obj,
            str(ratio)
        ]

        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
            if "[BLENDER_SUCCESS]" not in res.stdout:
                print(f"  [!] Aviso: Blender termino sin confirmacion explicita.")
        except subprocess.CalledProcessError as e:
            print(f"  [-] Error ejecutando Blender en {filename}: {e}")
            continue

        # 4. Reconstruir PMT binario a partir del OBJ decimado
        try:
            rebuilt_bin = obj_to_pmt_bytes(pmt_data, decimated_obj)
        except Exception as e:
            print(f"  [-] Error reconstruyendo PMT binario de {filename}: {e}")
            continue

        # 5. Comprimir con zlib nivel 9 y guardar como .sz final
        final_sz_path = os.path.join(output_dir, filename)
        rebuilt_sz = zlib.compress(rebuilt_bin, 9)
        with open(final_sz_path, 'wb') as sf:
            sf.write(rebuilt_sz)

        new_sz_size = len(rebuilt_sz)
        diff_pct = (1.0 - (new_sz_size / orig_sz_size)) * 100.0 if orig_sz_size > 0 else 0.0
        print(f"  [+] OPTIMIZADO: {orig_sz_size:,} bytes -> {new_sz_size:,} bytes (-{diff_pct:.1f}%)")
        print(f"  [+] Guardado: {final_sz_path}")
        successful_count += 1

        # Registrar linea para el archivo TXT
        log_entries.append(
            f"  - {filename:<35} | Original: {orig_sz_size:>9,} B -> Optimizado: {new_sz_size:>9,} B (-{diff_pct:5.1f}%)"
        )

        # Limpieza de archivos temporales individuales
        for tmp_file in glob.glob(os.path.join(temp_dir, f"{base_name}*")):
            try:
                if os.path.isdir(tmp_file):
                    shutil.rmtree(tmp_file, ignore_errors=True)
                else:
                    os.remove(tmp_file)
            except:
                pass

    # 6. Limpieza final de la carpeta temporal completa
    try:
        shutil.rmtree(temp_dir, ignore_errors=True)
    except:
        pass

    # 7. Guardar en registro_optimizacion.txt (modo 'a' para no borrar registros anteriores)
    try:
        file_already_exists = os.path.exists(log_path) and os.path.getsize(log_path) > 0
        with open(log_path, 'a', encoding='utf-8') as log_f:
            if not file_already_exists:
                log_f.write("========================================================================================\n")
                log_f.write("                  OUTRUN 2006: COAST 2 COAST - HISTORIAL DE OPTIMIZACIÓN                \n")
                log_f.write("========================================================================================\n\n")

            log_f.write("----------------------------------------------------------------------------------------\n")
            log_f.write(f"FECHA: {now_str} | CONSERVAR: {conserve_pct:.0f}% POLÍGONOS | REDUCCIÓN: {reduce_pct:.0f}%\n")
            log_f.write("----------------------------------------------------------------------------------------\n")
            if log_entries:
                for entry in log_entries:
                    log_f.write(entry + "\n")
            else:
                log_f.write("  (No se procesaron archivos con éxito en esta sesión)\n")
            log_f.write(f"RESULTADO: {successful_count} de {total_files} archivo(s) optimizados con éxito.\n\n")
        print(f"[+] Registro guardado en: {log_path}")
    except Exception as e:
        print(f"[-] No se pudo escribir en el archivo de registro TXT: {e}")

    # 8. Limpiar cualquier residuo en la carpeta final (protegiendo .sz y .txt)
    for stray in glob.glob(os.path.join(output_dir, "*")):
        stray_lower = stray.lower()
        if not stray_lower.endswith(".sz") and not stray_lower.endswith(".txt"):
            try:
                if os.path.isdir(stray):
                    shutil.rmtree(stray, ignore_errors=True)
                else:
                    os.remove(stray)
            except:
                pass

    print(f"\n=======================================================")
    print(f"[+] PROCESO COMPLETADO EXITOSAMENTE!")
    print(f"    Archivos optimizados: {successful_count}/{total_files}")
    print(f"    Archivos .sz finales guardados en: {output_dir}")
    print(f"    Historial actualizado en: {log_path}")
    print(f"    Todos los archivos temporales han sido eliminados.")
    print(f"=======================================================\n")
    return successful_count

def find_title_bik(root):
    mv_dir = None
    for entry in os.listdir(root):
        if entry.lower() == "mv" and os.path.isdir(os.path.join(root, entry)):
            mv_dir = entry
            break
    if not mv_dir:
        return None, None
    mv_path = os.path.join(root, mv_dir)
    for entry in os.listdir(mv_path):
        if entry.lower() == "titlescreen.bik" and ":" not in entry:
            return mv_dir, os.path.join(mv_path, entry)
    return mv_dir, None


def build_intro_cmds(ffmpeg_bin, in_path, out_bmp, out_ogg, seconds):
    fcmd = [
        ffmpeg_bin, "-y", "-hide_banner",
        "-ss", str(seconds),
        "-i", in_path,
        "-frames:v", "1",
        "-pix_fmt", "bgr24",
        out_bmp,
    ]
    acmd = [
        ffmpeg_bin, "-y", "-hide_banner",
        "-i", in_path,
        "-vn",
        "-c:a", "libvorbis",
        "-ar", "48000",
        "-ac", "2",
        "-q:a", "4",
        out_ogg,
    ]
    return fcmd, acmd


def fmt_cmd(cmd):
    return " ".join(f'"{c}"' if " " in c else c for c in cmd)


def process_intro(root, out_root, seconds, ffmpeg_arg, dry_run):
    mv_dir, bik_path = find_title_bik(root)
    if not mv_dir or not bik_path:
        print(f"[-] Intro: no se encontro mv/TitleScreen.bik en {root}", file=sys.stderr)
        return False

    out_bmp = os.path.join(out_root, mv_dir, "TitleScreen.bmp")
    out_ogg = os.path.join(out_root, mv_dir, "TitleScreen.ogg")
    ffmpeg_bin = ffmpeg_arg or shutil.which("ffmpeg")
    if not ffmpeg_bin and not dry_run:
        print("[-] Intro: no se encontro 'ffmpeg' en el PATH.", file=sys.stderr)
        print("    Instala ffmpeg (Windows: 'winget install Gyan.FFmpeg') o usa --ffmpeg /ruta/a/ffmpeg", file=sys.stderr)
        return False

    fcmd, acmd = build_intro_cmds(ffmpeg_bin or "ffmpeg", bik_path, out_bmp, out_ogg, seconds)
    if dry_run:
        print(f"[*] Intro detectada: {bik_path} -> {os.path.join(out_root, mv_dir)}/")
        print("    Frame:", fmt_cmd(fcmd))
        print("    Audio:", fmt_cmd(acmd))
        return True

    os.makedirs(os.path.dirname(out_bmp), exist_ok=True)
    print(f"[*] Intro [1/2] Extrayendo frame (t={seconds:g}s) -> {out_bmp}")
    if subprocess.call(fcmd) != 0:
        print("[-] Intro: ffmpeg fallo al extraer el frame.", file=sys.stderr)
        return False
    print(f"[*] Intro [2/2] Extrayendo audio (48 kHz Ogg) -> {out_ogg}")
    if subprocess.call(acmd) != 0:
        print("[-] Intro: ffmpeg fallo al extraer el audio.", file=sys.stderr)
        return False

    bmp_kb = os.path.getsize(out_bmp) / 1024.0
    ogg_kb = os.path.getsize(out_ogg) / 1024.0
    print(f"[+] Intro lista: TitleScreen.bmp ({bmp_kb:.1f} KB) + TitleScreen.ogg ({ogg_kb:.1f} KB)")
    return True


def find_plcar_files(root):
    cars_dir = None
    for entry in os.listdir(root):
        if entry.lower() == "cars" and os.path.isdir(os.path.join(root, entry)):
            cars_dir = entry
            break
    if not cars_dir:
        return None, []
    src_cars = os.path.join(root, cars_dir)
    targets = sorted(
        os.path.join(src_cars, f)
        for f in os.listdir(src_cars)
        if f.lower().startswith("obj_plcar")
        and f.lower().endswith("_pmt.sz")
        and ":" not in f
    )
    return cars_dir, targets


def self_check():
    strip = build_triangle_strip([(0, 1, 2), (2, 1, 3), (4, 5, 6)])
    assert len(strip) >= 5, "build_triangle_strip produjo strip invalido"
    assert find_matching_mat(3, ["Mat_Obj0_MG03_M01"]) == "Mat_Obj0_MG03_M01"
    fcmd, acmd = build_intro_cmds("ffmpeg", "mv/TitleScreen.bik", "out/mv/TitleScreen.bmp", "out/mv/TitleScreen.ogg", 4)
    assert "-ss" in fcmd and fcmd[-1].endswith("TitleScreen.bmp")
    assert "libvorbis" in acmd and "48000" in acmd and acmd[-1].endswith("TitleScreen.ogg")


def main():
    self_check()
    ap = argparse.ArgumentParser(
        description="Prepara assets opcionales de OutRun 2006 para PS Vita (intro BMP+OGG y coches de jugador)."
    )
    ap.add_argument("--root", default=".", help="Raiz del juego (por defecto: directorio actual)")
    ap.add_argument("--out", default="vita_assets", help="Carpeta de salida (por defecto: vita_assets)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--only-intro", action="store_true", help="Ejecuta solo la extraccion del frame+audio de la intro (ffmpeg)")
    mode.add_argument("--only-cars", action="store_true", help="Ejecuta solo la reduccion de coches de jugador (Blender)")
    ap.add_argument("--ratio", type=float, default=0.50, help="Ratio de poligonos a conservar en coches 0.15..0.95 (por defecto: 0.50)")
    ap.add_argument("--time", type=float, default=4.0, help="Segundo del video de intro a capturar (por defecto: 4.0)")
    ap.add_argument("--blender", default=None, help="Ruta manual al ejecutable de Blender")
    ap.add_argument("--ffmpeg", default=None, help="Ruta manual al ejecutable de ffmpeg")
    ap.add_argument("--dry-run", action="store_true", help="Muestra que archivos y comandos se ejecutarian sin modificar nada")
    args = ap.parse_args()

    if not (0.15 <= args.ratio <= 0.95):
        print("[-] Error: --ratio debe estar entre 0.15 y 0.95.", file=sys.stderr)
        return 1
    if args.time < 0.0:
        print("[-] Error: --time debe ser >= 0.", file=sys.stderr)
        return 1

    root = os.path.abspath(args.root)
    out_root = os.path.abspath(args.out) if os.path.isabs(args.out) else os.path.join(root, args.out)
    run_intro = not args.only_cars
    run_cars = not args.only_intro

    ok_intro = True
    ok_cars = True

    if run_intro:
        ok_intro = process_intro(root, out_root, args.time, args.ffmpeg, args.dry_run)

    if run_cars:
        cars_dir, sz_files = find_plcar_files(root)
        if not cars_dir or not sz_files:
            print(f"[-] Coches: no se encontraron archivos Cars/obj_plcar*_pmt.sz en {root}", file=sys.stderr)
            ok_cars = False
        else:
            out_cars = os.path.join(out_root, cars_dir)
            if args.dry_run:
                print(f"[*] Coches detectados: {len(sz_files)} en {os.path.join(root, cars_dir)} (ratio={args.ratio:.2f}) -> {out_cars}/")
                for p in sz_files[:5]:
                    print(f"    - {os.path.basename(p)}")
                if len(sz_files) > 5:
                    print(f"    ... (+{len(sz_files) - 5} mas)")
            else:
                blender_exe = args.blender or find_blender()
                if not blender_exe or not os.path.exists(blender_exe):
                    print("[-] Coches: no se encontro Blender automaticamente.", file=sys.stderr)
                    print("    Instala Blender o pasa su ruta con --blender /ruta/a/blender", file=sys.stderr)
                    ok_cars = False
                else:
                    print(f"[+] Blender detectado: {blender_exe}")
                    print(f"[+] Coches de jugador detectados: {len(sz_files)} -> {out_cars}/")
                    ok_cars = process_pipeline(sz_files, args.ratio, out_cars, blender_exe, log_dir=out_root) > 0

    if not args.dry_run and (ok_intro or ok_cars):
        print(f"\n[+] Listo. Arrastra las carpetas generadas dentro de '{out_root}' a ux0:data/outrun2006/ en tu PS Vita.")
    return 0 if (ok_intro and ok_cars) else 1


if __name__ == "__main__":
    raise SystemExit(main())
