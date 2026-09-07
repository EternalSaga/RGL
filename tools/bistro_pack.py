#!/usr/bin/env python3
# assimp .gltf (dds-by-uri) -> conformant PBR .glb (embedded PNG).
# Rule: BaseColor->baseColorTexture(sRGB), Normal->normalTexture(flip-G DX->GL),
#       Specular(R:AO G:rough B:metal)->metallicRoughnessTexture + occlusionTexture.
import sys, os, json, struct, subprocess, tempfile, math

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
        p["roughnessFactor"] = 1.0
    if p.get("baseColorTexture"): referenced.add(p["baseColorTexture"]["index"])
    if m.get("normalTexture"): referenced.add(m["normalTexture"]["index"])
    if spec_idx is not None: referenced.add(spec_idx)

# 3. Transcode each referenced dds -> png, build new image entries + bin chunks.
new_images = []
old2new = {}
png_chunks = []   # bytes, each 4-byte padded
cur = len(geom)
cur = (cur + 3) & ~3   # pad geometry to 4 for appending pngs
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

done = 0
for i in sorted(referenced):
    src = uri_to_src(images[i]["uri"])
    if not os.path.exists(src):
        print("MISSING", src); continue
    png = os.path.join(tmp, f"t{i}.png")
    if not run_ff(src, png, kind[i] == "normal"):
        print("FFMPEG FAIL", src); continue
    blob = open(png, "rb").read()
    # new bufferView for this png
    pad = (-len(blob)) & 3
    bv = {"buffer": 0, "byteOffset": cur, "byteLength": len(blob)}
    g["bufferViews"].append(bv)
    cur += len(blob) + pad
    png_chunks.append(blob + b"\x00" * pad)
    new_images.append({"bufferView": len(g["bufferViews"]) - 1,
                       "mimeType": "image/png"})
    old2new[i] = len(new_images) - 1
    done += 1

# 4. Assemble BIN: geometry (padded) + png blobs.
geompad = (b"\x00" * ((-len(geom)) & 3))
bin_data = geom + geompad + b"".join(png_chunks)
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
    if m.get("occlusionTexture") and m["occlusionTexture"]["index"] in old2new:
        m["occlusionTexture"]["index"] = old2new[m["occlusionTexture"]["index"]]
g["images"] = new_images

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
