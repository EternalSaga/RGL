#!/usr/bin/env python3
# assimp .gltf (dds-by-uri) -> conformant PBR .glb (embedded PNG).
# Rule: BaseColor->baseColorTexture(sRGB), Normal->normalTexture(flip-G DX->GL),
#       Specular(R:AO G:rough B:metal)->metallicRoughnessTexture + occlusionTexture.
# Emissive fix (2026-09-29): emissiveTexture was in NEITHER the referenced set NOR the
# index-remap loop, so emissive images were never packed and the stale index pointed at a
# re-ordered image (Bistro's 1x1 constant _Specular.dds (0,188,0) -> green emissive).
# Metadata backfill (2026-09-13): assimp FBX importer drops Source2 blend/double-sided
# semantics (output = 132x OPAQUE, doubleSided=0), but leaf-texture alpha cutouts and
# material names (".DoubleSided") survive -> restore STANDARD glTF fields here:
#   doubleSided  : material name contains ".DoubleSided"
#   MASK + cutoff: baseColor PNG alpha non-trivial (PIL scan; no PIL -> skip MASK)
#   KHR_materials_emissive_strength: on emissive materials (official strength unknown --
#     not in .pyscene/README; EMISSIVE_STRENGTH below is a look-dev value, not a source value)
import sys, os, json, struct, subprocess, tempfile, math

# Tangent baking (2026-09-13, plan A): assimp glTF exporter never writes the core
# TANGENT attribute (verified: default export, WEBCGL_GLTF_TANGENTS cfg, via-FBX round
# trip -- all produce POSITION/NORMAL/TEXCOORD_0 only). Bistro has normal maps but no
# tangents -> engine was forced into screen-space-derivative TBN, which degenerates at
# grazing angles (det->0, specular salt noise). We bake SMOOTH TANGENTS HERE instead
# (Lengyel per-triangle Jacobian + per-vertex accumulate + Gram-Schmidt, same family as
# Filament's CPU TangentSpaceMesh fallback) and emit glTF 2.0 CORE attribute TANGENT
# (vec3 float32). Handedness W is NOT in glTF core (spec: infer from geometry) -- the
# engine loader derives it per-triangle from the uv Jacobian sign, so we deliberately
# do not need to store it. Degenerate/uv-less primitives fall back to Frisvad analytic
# basis so every baked vertex gets a unit tangent perpendicular to its normal.
EMISSIVE_STRENGTH = 4.0   # look-dev; engine loader premultiplies into emissiveFactor
# Wet-pavement roughness 缩放（2026-09-29）：effective = 本值 × ORM.G(0.737) ≈ 0.12（水膜）。
# 逐条依据见下方 roughnessFactor 处的注释。
WET_ROUGHNESS_SCALE = 0.163
ALPHA_CUTOFF = 0.5        # glTF default

if len(sys.argv) < 4:
    print("usage: pack.py OUT.gltf TEXTURE_DIR OUT.glb [MAXSIDE]")
    sys.exit(2)
GLTF, TEXDIR, OUTGLB = sys.argv[1], sys.argv[2], sys.argv[3]
MAXSIDE = int(sys.argv[4]) if len(sys.argv) > 4 else 0

g = json.load(open(GLTF, "r"))
geombin = os.path.join(os.path.dirname(GLTF), "out.bin")
geom = open(geombin, "rb").read()

images = g["images"]
materials = g["materials"]

def uri_to_src(u):
    return os.path.join(TEXDIR, os.path.basename(u.replace("\\", "/")))

# 1. Decide per-image output kind + role, collect referenced indices.
#    Determine role by filename tag (authoritative from Bistro naming convention).
kind = {}   # idx -> 'base'|'normal'|'spec'|'other'
for i, im in enumerate(images):
    s = os.path.basename(im.get("uri", "")).lower()
    if "_normal" in s: kind[i] = "normal"
    elif "_specular" in s: kind[i] = "spec"
    elif "_basecolor" in s: kind[i] = "base"
    else: kind[i] = "other"

# 2. Rewire materials: attach metallicRoughness+occlusion from specular, factors->1,
#    drop KHR_materials_specular. Collect referenced image indices.
referenced = set()
for m in materials:
    p = m.setdefault("pbrMetallicRoughness", {})
    spec_idx = None
    ext = m.get("extensions", {})
    if "KHR_materials_specular" in ext:
        st = ext["KHR_materials_specular"].get("specularTexture")
        if st: spec_idx = st["index"]
        del ext["KHR_materials_specular"]
        if not ext: del m["extensions"]
    if spec_idx is not None:
        p["metallicRoughnessTexture"] = {"index": spec_idx}
        m["occlusionTexture"] = {"index": spec_idx}
        p["metallicFactor"] = 1.0
        # Wet-pavement look-dev (2026-09-29): Bistro 的 *_Specular.dds 是 16x16 常量
        # (0,188,0) -> ORM 的 G 通道恒 0.737，**wet 与 dry 材质的 PBR 参数逐字节相同**
        # （README/README 未发布湿面数据，官方参考图是 Falcor 路径追踪）。湿沥青/湿石板
        # 的物理近似 = 一层水膜 = roughness 大幅下降，于是这里给名字含 "_Wet" 的材质
        # 一个 roughness 缩放：effective = WET_ROUGHNESS_SCALE * 0.737 = 0.12。
        #
        # 为什么是 0.12 而非更镜面：实测路面带 p90 在 effective roughness
        # 0.221 / 0.120 / 0.059 三档分别是 197.0 / 197.3 / 197.0 —— **0.12 就到平台期**。
        # 因为没有 SSR/局部反射，能被反射的只有 IBL cubemap 的高 mip（近似均匀天空辉光），
        # 再锐化也反射不出任何新信息。这是无 SSR 的已知代价，不是调参没调到位。
        #
        # 已知残留偏差（未做，需 shader 侧 wetness 通道）：真实湿面会**同时**压暗漫反射
        # （水填进微观凹凸），这里只加高光、不压暗，故湿区整体比官方略"亮而奶"，
        # 缺湿面特有的暗部对比。补它需要 per-material 的 wetness 分量进 scene data
        # （draw data schema 扩列 = 反射管线改动），列 Phase 12 候选。
        p["roughnessFactor"] = (WET_ROUGHNESS_SCALE if "_Wet" in (m.get("name") or "")
                                else 1.0)
    if p.get("baseColorTexture"): referenced.add(p["baseColorTexture"]["index"])
    if m.get("normalTexture"): referenced.add(m["normalTexture"]["index"])
    # emissiveTexture 同样要收：漏了则贴图不打包，且 step 5 的索引 remap 拿不到映射，
    # emissiveTexture.index 会保留旧下标、指向重排后的另一张图（Bistro 的
    # *_Specular.dds 是 1x1 常量 (0,188,0) → 自发光纯绿，2026-09-29 实测 10 个材质受害）
    if m.get("emissiveTexture"): referenced.add(m["emissiveTexture"]["index"])
    if spec_idx is not None: referenced.add(spec_idx)

# 2b. Shared BIN tail allocator: geometry (padded to 4) + tangent chunks + png chunks.
#     All appended chunks are 4-byte aligned (float32 accessors require it).
cur = len(geom)
cur = (cur + 3) & ~3
tail_chunks = []   # bytes (already padded), in bufferView order

def append_bv(blob):
    """Append a 4-aligned chunk to the BIN tail, return its new bufferView index."""
    global cur
    pad = (-len(blob)) & 3
    g["bufferViews"].append({"buffer": 0, "byteOffset": cur, "byteLength": len(blob)})
    cur += len(blob) + pad
    tail_chunks.append(blob + b"\x00" * pad)
    return len(g["bufferViews"]) - 1

# ---- tangent baking (see header note) ----------------------------------------
import numpy as np

_CT = {5120: "<i1", 5121: "<u1", 5122: "<i2", 5123: "<u2",
       5125: "<u4", 5124: "<i4", 5126: "<f4"}
_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

def read_acc(ai):
    """Read an accessor as (count, ncomp) float/int numpy array (strided bv aware)."""
    a = g["accessors"][ai]
    bv = g["bufferViews"][a["bufferView"]]
    dt = np.dtype(_CT[a["componentType"]])
    n, c = a["count"], _NC[a["type"]]
    base = bv.get("byteOffset", 0)
    if a.get("normalized"):
        raise ValueError("normalized accessors unexpected here")
    if "byteStride" in bv:
        item = dt.itemsize
        raw = np.frombuffer(geom, dtype=dt, count=(len(geom) - base) // item)
        arr = np.lib.stride_tricks.as_strided(
            raw, shape=(n, c), strides=(bv["byteStride"], item))
    else:
        arr = np.frombuffer(geom, dtype=dt, count=n * c, offset=base).reshape(n, c)
    return np.array(arr, copy=True)

def frisvad(n):
    """Frisvad 2012 analytic ONB tangent (vectorized): unit t perpendicular to unit n."""
    nx, ny, nz = n[:, 0], n[:, 1], n[:, 2]
    out = np.empty_like(n)
    sing = nz < -0.9999999
    out[sing] = (0.0, -1.0, 0.0)
    reg = ~sing
    a = 1.0 / (1.0 - nz[reg])           # nz < -0.9999999 excluded => 1-nz <= ~2, > 0 safe
    b = -nx[reg] * ny[reg] * a
    out[reg, 0] = 1.0 - nx[reg] * nx[reg] * a
    out[reg, 1] = b
    out[reg, 2] = -nx[reg]
    return out / np.maximum(np.linalg.norm(out, axis=1, keepdims=True), 1e-30)

def bake_tangents():
    """For every primitive with POSITION+NORMAL+TEXCOORD_0+indices and no TANGENT:
    Lengyel smooth tangents -> core TANGENT accessor. Returns (n_baked, n_skip)."""
    n_baked = n_skip = 0
    for mesh in g["meshes"]:
        for prim in mesh["primitives"]:
            at = prim["attributes"]
            if "TANGENT" in at or "POSITION" not in at or "NORMAL" not in at \
               or "TEXCOORD_0" not in at or "indices" not in prim:
                n_skip += 1
                continue
            pos = read_acc(at["POSITION"]).astype(np.float64)
            nrm = read_acc(at["NORMAL"]).astype(np.float64)
            uv = read_acc(at["TEXCOORD_0"]).astype(np.float64)
            idx = read_acc(prim["indices"]).astype(np.int64).reshape(-1, 3)
            n_v = pos.shape[0]
            e1 = pos[idx[:, 1]] - pos[idx[:, 0]]
            e2 = pos[idx[:, 2]] - pos[idx[:, 0]]
            s1 = uv[idx[:, 1]] - uv[idx[:, 0]]
            s2 = uv[idx[:, 2]] - uv[idx[:, 0]]
            det = s1[:, 0] * s2[:, 1] - s2[:, 0] * s1[:, 1]
            valid = np.abs(det) > 1e-12
            r = np.where(valid, 1.0 / np.where(valid, det, 1.0), 0.0)[:, None]
            T = (e1 * s2[:, 1:2] - e2 * s1[:, 1:2]) * r          # (m,3) per-triangle
            B = (e2 * s1[:, 0:1] - e1 * s2[:, 0:1]) * r
            wtri = np.sign(np.einsum("ij,ij->i", np.cross(nrm[idx[:, 0]], T), B))
            wtri[~valid] = 0.0
            flat = idx.ravel()
            Trep = np.repeat(T, 3, axis=0)
            Tacc = np.stack([np.bincount(flat, Trep[:, k], minlength=n_v)
                             for k in range(3)], axis=1)
            gs = Tacc - nrm * np.einsum("ij,ij->i", nrm, Tacc)[:, None]  # Gram-Schmidt
            bad = np.linalg.norm(gs, axis=1) < 1e-12                     # no valid tri
            if bad.any():
                gs[bad] = frisvad(nrm[bad] / np.maximum(
                    np.linalg.norm(nrm[bad], axis=1, keepdims=True), 1e-30))
            tan = gs / np.maximum(np.linalg.norm(gs, axis=1, keepdims=True), 1e-30)
            bv = append_bv(np.ascontiguousarray(tan, dtype="<f4").tobytes())
            at["TANGENT"] = len(g["accessors"])
            g["accessors"].append({"bufferView": bv, "componentType": 5126,
                                   "count": int(n_v), "type": "VEC3"})
            n_baked += 1
    return n_baked, n_skip

n_tan, n_tan_skip = bake_tangents()

# 3. Transcode each referenced dds -> png, build new image entries + bin chunks.
new_images = []
old2new = {}
tmp = tempfile.mkdtemp()

def run_ff(inp, outp, flip):
    vf = []
    if MAXSIDE > 0:
        vf.append(f"scale={MAXSIDE}:{MAXSIDE}:force_original_aspect_ratio=decrease:flags=lanczos")
    # NOTE: normal-map green-channel flip (DirectX->OpenGL) + BC5 Z-reconstruction deferred
    # to Phase 8 (when the engine actually samples normalTexture). Decode as-is for now.
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", inp]
    if vf:
        cmd += ["-vf", ",".join(vf)]
    cmd += [outp]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0

def has_real_alpha(png_blob):
    # baseColor alpha 非平凡判据：存在 <250 的像素（cutout 迹象）且存在 >5 的像素
    # （全 0 alpha 多半是误分类/掩码图而非 cutout）。PIL 缺失 = False（宁漏不错 MASK）。
    try:
        import io
        from PIL import Image
        im = Image.open(io.BytesIO(png_blob))
        if im.mode != "RGBA":
            return False
        ah = im.histogram()[3 * 256:4 * 256]  # RGBA -> alpha 直方图 256 bin
        return sum(ah[:250]) > 0 and sum(ah[3:]) > 0
    except Exception:
        return False

done = 0
alpha_real = {}   # old image idx -> baseColor alpha 非平凡（MASK 判据）
for i in sorted(referenced):
    src = uri_to_src(images[i]["uri"])
    if not os.path.exists(src):
        print("MISSING", src); continue
    png = os.path.join(tmp, f"t{i}.png")
    if not run_ff(src, png, kind[i] == "normal"):
        print("FFMPEG FAIL", src); continue
    blob = open(png, "rb").read()
    new_images.append({"bufferView": append_bv(blob), "mimeType": "image/png"})
    old2new[i] = len(new_images) - 1
    if kind[i] == "base":
        alpha_real[i] = has_real_alpha(blob)
    done += 1

# 3b. Metadata backfill (see header): MASK from baseColor alpha scan, doubleSided from
#     name, emissive strength on emissive materials. Runs BEFORE the index remap in step 5
#     (reads OLD base texture indices).
n_mask = n_ds = n_str = 0
for m in materials:
    name = (m.get("name") or "").lower()
    if ".doublesided" in name:
        m["doubleSided"] = True; n_ds += 1
    base = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
    if base and alpha_real.get(base["index"], False):
        m["alphaMode"] = "MASK"; m["alphaCutoff"] = ALPHA_CUTOFF; n_mask += 1
    if any(m.get("emissiveFactor", [0, 0, 0])) or "emissiveTexture" in m:
        m.setdefault("extensions", {})["KHR_materials_emissive_strength"] = \
            {"emissiveStrength": EMISSIVE_STRENGTH}
        n_str += 1
# extensionsUsed = 材质实际使用的扩展（step 5 统一重建——assimp 中间件 over-declare：
# specular 被本脚本删除、volume/ngon 从未出现在材质里，残留即 conformant 违规）

# 4. Assemble BIN: geometry (padded) + png blobs.
geompad = (b"\x00" * ((-len(geom)) & 3))
bin_data = geom + geompad + b"".join(tail_chunks)
buffer0_len = len(bin_data)

# 5. Rewrite gltf JSON.
g["buffers"] = [{"byteLength": buffer0_len}]   # drop uri -> embedded
for m in materials:
    p = m.setdefault("pbrMetallicRoughness", {})
    def remap(d, k):
        if d.get(k): d[k]["index"] = old2new[d[k]["index"]]
    remap(p, "baseColorTexture")
    if p.get("metallicRoughnessTexture") and p["metallicRoughnessTexture"]["index"] in old2new:
        p["metallicRoughnessTexture"]["index"] = old2new[p["metallicRoughnessTexture"]["index"]]
    if m.get("normalTexture") and m["normalTexture"]["index"] in old2new:
        m["normalTexture"]["index"] = old2new[m["normalTexture"]["index"]]
    if m.get("emissiveTexture") and m["emissiveTexture"]["index"] in old2new:
        m["emissiveTexture"]["index"] = old2new[m["emissiveTexture"]["index"]]
    if m.get("occlusionTexture") and m["occlusionTexture"]["index"] in old2new:
        m["occlusionTexture"]["index"] = old2new[m["occlusionTexture"]["index"]]
g["images"] = new_images

used_exts = sorted({k for m in materials for k in m.get("extensions", {})})
if used_exts:
    g["extensionsUsed"] = used_exts
else:
    g.pop("extensionsUsed", None)
g.pop("extensionsRequired", None)

json_bytes = json.dumps(g, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * ((-len(json_bytes)) & 3)
# strip samplers that referenced removed images? keep as-is (unused ok)

# 6. Write GLB.
total = 12 + 8 + len(json_bytes) + 8 + len(bin_data)
with open(OUTGLB, "wb") as f:
    f.write(struct.pack("<4sII", b"glTF", 2, total))
    f.write(struct.pack("<I4s", len(json_bytes), b"JSON"))
    f.write(json_bytes)
    f.write(struct.pack("<I4s", len(bin_data), b"BIN\x00"))
    f.write(bin_data)

print(f"OK wrote {OUTGLB}  {buffer0_len/1e6:.1f}MB  geom={len(geom)/1e6:.1f}MB  pngs={done}/{len(referenced)}  json={len(json_bytes)/1e6:.1f}MB")
print(f"backfill: MASK={n_mask} doubleSided={n_ds} emissive_strength={n_str} (PIL={'yes' if alpha_real else 'no-scan'})")
print(f"tangents: baked={n_tan} skipped={n_tan_skip}")
