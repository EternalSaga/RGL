#!/usr/bin/env python3
"""meshlet_w_mixed.py —— 量「tangent.w 逐顶点近似」的误差面（簇化轴 ★1-a1ii 的判据工具）。

**为什么需要这个工具**（结论 → docs/mdi_plan.md §12）：UV 手性是**逐三角形**量
（`gltf_loader_impl.cppm::uvTriangleHandedness`，det<0 → -1），而 mesh shader 的**顶点**输出接口
装不下逐三角量——一个簇内的共享顶点被手性相反的两个三角形引用时，逐顶点只能存一个 w。
本工具精确数出这种顶点有多少，从而「要不要为它上 per-primitive 输出」是**量出来的**而不是猜的。

L0 判据 = `cluster.refined == -1`（引擎 `meshlet_ranges_impl.cppm::isL0Cluster` 同一条；
`tools/meshlet_pack/format.h` 旧版写的 `group.depth==0` 已被实测推翻，那里有记录）。

三条自检锚（任一不成立即退出码非 0，**不报数**）：
  A. L0 簇三角和 **恰等于** header.l0TriangleCount（闭式，切分口径唯一性判据）
  B. 负手性比例与既有两条锚点一致（82.00%，2,322,319 / 2,832,120）
  C. float32 忠实性：|det| 退化三角中两种精度手性相反的条数 **必须为 0**，
     否则本工具的 float64 结论对 shader（float32）不成立，读数作废

用法：tools/meshlet_w_mixed.py [产物路径]   （默认 build/rgl/models/bistro_exterior.glb.meshlets.bin）
"""
import mmap
import struct
import sys
from collections import Counter

DEFAULT = "build/rgl/models/bistro_exterior.glb.meshlets.bin"
NEAR = 1e-9
# 锚点 B（真产物 bistro，packer 在源 glb 上独立量过同一比例）
ANCHOR_NEG = (2322319, 2832120)

# FileHeader 字段偏移（tools/meshlet_pack/format.h 逐字段算出）
OFF_PRIMC, OFF_CLUSTERC = 40, 48
OFF_L0TRIS, OFF_POOL, OFF_CLUSTERS, OFF_TRIS, OFF_REMAP = 72, 96, 104, 112, 120
CL_STRIDE = 11  # DiskCluster 44B，以 4B 字计（索引进 `<I` 展平数组用）
V_BYTES = 48    # VertexPool 元素**字节**步长（ModelVertex 契约，别和上面的字步长混）


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def det64(uv, i0, i1, i2):
    s0, t0 = uv[i0]
    s1x, t1x = uv[i1]
    s2x, t2x = uv[i2]
    return (s1x - s0) * (t2x - t0) - (s2x - s0) * (t1x - t0)


def det32(uv, i0, i1, i2):
    s0, t0 = uv[i0]
    s1x, t1x = uv[i1]
    s2x, t2x = uv[i2]
    return f32(f32(f32(s1x - s0) * f32(t2x - t0)) - f32(f32(s2x - s0) * f32(t1x - t0)))


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    with open(path, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        magic = mm[0:8]
        if magic != b"RGLCLOD\x00":
            print(f"🔴 magic 不是 RGLCLOD：{magic!r}（不是本工具认的产物）")
            return 2
        primC, vertC, clustC, groupC = struct.unpack_from("<4I", mm, OFF_PRIMC)
        l0TrisHdr = struct.unpack_from("<Q", mm, OFF_L0TRIS)[0]
        offPool, offClusters = struct.unpack_from("<2Q", mm, OFF_POOL)
        offTris, offRemap = struct.unpack_from("<2Q", mm, OFF_TRIS)
        cl = struct.unpack_from(f"<{clustC * CL_STRIDE}I", mm, offClusters)
        refined = [cl[c * CL_STRIDE + 1] for c in range(clustC)]
        l0 = [c for c in range(clustC) if refined[c] == 0xFFFFFFFF]

        tot = neg = degen = flip = 0
        mixedSlots = mixedClusters = isolated = refs = 0
        mixedGlobal = set()
        perCluster = Counter()
        for c in l0:
            b = c * CL_STRIDE
            triC, vertC_, triOff, remapOff = cl[b + 2], cl[b + 3], cl[b + 4], cl[b + 5]
            tris = mm[offTris + triOff: offTris + triOff + triC * 3]
            remap = struct.unpack_from(f"<{vertC_}I", mm, offRemap + remapOff * 4)
            uv = [struct.unpack_from("<2f", mm, offPool + g * V_BYTES + 24) for g in remap]
            w = []
            for k in range(triC):
                i0, i1, i2 = tris[3 * k], tris[3 * k + 1], tris[3 * k + 2]
                d = det64(uv, i0, i1, i2)
                if -NEAR < d < NEAR:
                    degen += 1
                    d32 = det32(uv, i0, i1, i2)
                    if (d < 0.0) != (d32 < 0.0):
                        flip += 1
                    d = d32
                if d < 0.0:
                    neg += 1
                w.append(-1.0 if d < 0.0 else 1.0)
            tot += triC
            seen = [0] * vertC_  # 0=未引用 1=+1 2=-1 3=混合
            for k in range(triC):
                flag = 1 if w[k] > 0 else 2
                for slot in (tris[3 * k], tris[3 * k + 1], tris[3 * k + 2]):
                    refs += 1
                    if seen[slot] == 0:
                        seen[slot] = flag
                    elif seen[slot] != flag:
                        seen[slot] = 3
            bad = [s for s in range(vertC_) if seen[s] == 3]
            isolated += sum(1 for s in seen if s == 0)
            if bad:
                mixedClusters += 1
                mixedSlots += len(bad)
                perCluster[len(bad)] += 1
                mixedGlobal.update(remap[s] for s in bad)

    print(f"产物 = {path}")
    print(f"prims={primC} verts={vertC} clusters={clustC} groups={groupC}")
    print(f"L0 簇（refined==-1）= {len(l0)} / {clustC}")
    print(f"负手性三角 = {neg} / {tot} = {100.0*neg/tot:.2f}%")
    print(f"|det|<{NEAR:g} 退化三角 = {degen}，其中 float32/float64 手性相反 = {flip}")
    print(f"孤立顶点（簇内无三角引用）= {isolated}")
    print(f"🔴 混合手性顶点 = {mixedSlots} 个局部槽 / 去重全局顶点 {len(mixedGlobal)} 个 / "
          f"涉及簇 {mixedClusters} 个（占 L0 簇 {100.0*mixedClusters/len(l0):.2f}%）")
    print(f"   占 L0 顶点引用 {refs} 的 {100.0*mixedSlots/refs:.4f}%；"
          f"每簇混合数分布（前 6）= {perCluster.most_common(6)}")

    bad = []
    if tot != l0TrisHdr:
        bad.append(f"锚 A：L0 三角和 {tot} ≠ header.l0TriangleCount {l0TrisHdr}（切分口径不唯一）")
    if (neg, tot) != ANCHOR_NEG:
        bad.append(f"锚 B：负手性 {neg}/{tot} ≠ 既有锚点 {ANCHOR_NEG[0]}/{ANCHOR_NEG[1]}")
    if flip:
        bad.append(f"锚 C：{flip} 条退化三角两种精度手性相反 ⟹ float64 读数对 shader 不成立")
    if isolated:
        bad.append(f"锚 D：{isolated} 个孤立顶点（meshopt remap 应紧，出现即产物或本工具口径变了）")
    for m in bad:
        print(f"🔴 {m}")
    print("自检：0 条失败" if not bad else f"自检：{len(bad)} 条失败")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
