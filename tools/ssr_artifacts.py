#!/usr/bin/env python3
"""SSR 伪影度量算子（Phase 15 批 15.3 子步 C，stdlib-only）：firefly / 深度间隙断裂带 / 屏幕边缘环带。

**为什么存在**：批 15.3 子步 C 的三条伪影（firefly、遮挡轮廓处的反射断裂、屏幕边界淡出）
都要「度量先行」——判"要不要修"与收反向用例的账都必须有可复跑的数。区域来自**差分图自身
的结构**（同 `tools/imgqa/` 的"区域来自几何不来自图像"精神：这里的"几何"是 Δ 图的活跃集）。
输入 = 同机位 SSR 开/关两张 PNG（Δ = lum(on) − lum(off)，线性 Rec.709，与 diffstruct 同口径）。

三个度量（口径全部钉在本文件，改口径 = 改判据）：

1. **firefly**（债-40 那条 HDR firefly 对 SSR 的形态：孤立像素的反射贡献 ≫ 邻域）：
   `|Δ(p)| > firefly-floor ∧ |Δ(p)| > k × p90(|Δ| ∈ 3×3 邻域去自身)`。
   · 8 枚邻值的 p90 按 nearest-rank（`sorted[min(n-1, int(f·n))]`，同 ssr_hull_check 的 q()）
     = 8 枚里的**最大值**——写 p90 不写 max 是为了与 spec 对齐，n=8 时两者重合。
   · floor（默认 0.01 线性亮度）把近黑处的 1 LSB 噪声排除在"亮斑"之外——没有 floor，
     全黑区域 0.00002 vs 0.000002 也算 10× 比值 = 度量自嗨。
   · 同时报**比值分布** |Δ|/邻域p90（活跃集 p50/p90/p99/max）——k 的定标依据，先量后定。
   · 计数用 |Δ|（双向）：变暗尖峰（减 iblSpec 的镜像面）与变亮尖峰同为"孤立像素异常"形态。

2. **深度间隙断裂带**（反射在遮挡轮廓处断裂/被啃的镜像签名 = 「差分消失带」）：
   `|Δ(p)| ≤ 1e-6（该像素 SSR 完全没作用）∧ 3×3 半径 3 的切比雪夫邻域内存在活跃像素
   （|Δ| > gap-act，默认 0.02）`；给 `--gbuf`（dump_gbuf=3）时再限定 **0<rough<gate**
   （门控域 = 本该反射的光滑面；不加会把湿→干材质过渡的正确零 Δ 也算成缝）。
   宽度 = 每个 gap 像素到**最近活跃像素**的欧氏距离（多源 BFS 精确距离变换）：
   dist>1.0 = 缝宽 ≥2px（单像素量化缝到 1.0 为止），dist>1.5 = ≥3px。
   🔴 **缝性（seam）= 核心区分**：gap 像素沿某轴双向走 ≤6 步，**两侧**的第一个非零像素都
   活跃才算「侵蚀缝」；只有一侧活跃 = 活跃域的自然边界（M1 出屏区与命中区的分界——零区是
   宽的不是被啃的），不是伪影。「成片」判据 = {seam ∧ dist>1.0} 的 8-连通分量：报分量数、
   ≥20px 的分量数与最大分量；缝宽分布 = seam 像素的 dist p50/p90/max。
   ⚠ 区域条件用「邻域活跃」而不是「深度梯度大」——深度梯度带是这些缝的**成因位置**，
   邻域活跃 + 双侧缝性是它的**可测代理**（不需要额外深度输入，判据只依赖交付图 + gbuf）。

3. **屏幕边缘环带**（现状 `kEdgeFadePx=24` 只作用 hitUv，起点贴屏缘的像素不淡出）：
   · 环带 R0 = 距屏幕边界 < ring（默认 24 = kEdgeFadePx）的像素；邻带 R1 = [ring, 2·ring)。
   · 「边缘一圈反射突然全亮」判据 = R0 的活跃占比 / 活跃均值 显著高于 R1（起点侧无淡出
     ⟹ 环带贡献与画面内部同量级甚至更高；若反而更低则无此现象）。
   · 「可见硬切」判据 = **硬边密度**：水平/垂直相邻像素对里 `|Δ(a)| > 0.02 ∧ |Δ(b)| ≤ 1e-6`
     的对数占像素数比，R0 内 vs 深内部（距屏缘 ≥2ring，"画面典型值"参照）。fade 的硬切会在
     环带内留下显著高于画面典型值的 active→零 硬邻接；与深内部同量级 = 没有额外的边缘硬切。

用法:
    python3 tools/ssr_artifacts.py <off.png> <on.png> [--firefly-k 3] [--firefly-floor 0.01]
                                   [--gap-act 0.02] [--ring 24] [--json]
    python3 tools/ssr_artifacts.py --selftest    # 合成图阳性锚点（无需引擎/真图）
退出码: 0 = 出数（本算子是**度量**不是判据闸门，红绿由调用方的验收条件定）· 1 = 输入不合法。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ssr_hull_check import png_decode_rgb  # 共享最小 PNG 解码（含行过滤器展开，15.3B 教训）

EPS = 1e-6  # 「Δ≈0」判据线：与 diffstruct 的差分集合门槛同值（PNG 字节有任何不同都 > 它）


def luma_grid(path: Path) -> list[list[float]]:
    """整图线性 Rec.709 亮度网格（sRGB 解码 → 0.2126/0.7152/0.0722，同 ssr_hull_check.luma）。"""
    w, h, rows, ch = png_decode_rgb(path.read_bytes())

    def lin(c: int) -> float:
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    return [[0.2126 * lin(r[ch * x]) + 0.7152 * lin(r[ch * x + 1]) + 0.0722 * lin(r[ch * x + 2])
             for x in range(w)] for r in rows]


def abs_delta(off: Path, on: Path) -> tuple[list[list[float]], int, int]:
    """|Δ| 网格 + 宽高。Δ = lum(on) − lum(off)，取绝对值（三个度量都吃 |Δ|）。"""
    a = luma_grid(off)
    b = luma_grid(on)
    if len(a) != len(b) or (a and len(a[0]) != len(b[0])):
        raise SystemExit(f"ssr_artifacts: 两图尺寸不同 {len(a[0])}x{len(a)} vs {len(b[0])}x{len(b)}")
    h, w = len(a), len(a[0])
    return [[abs(b[y][x] - a[y][x]) for x in range(w)] for y in range(h)], w, h


def p90(vals: list[float]) -> float:
    """nearest-rank p90（同 ssr_hull_check 的 q()：sorted[min(n-1, int(0.9n))]）。空表返回 0。"""
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(0.9 * len(s)))]


def firefly(d: list[list[float]], w: int, h: int, k: float, floor: float) -> dict:
    """firefly 计数 + 比值分布（口径 → 模块 docstring 1）。"""
    count = 0
    ratios: list[float] = []
    for y in range(h):
        row = d[y]
        for x in range(w):
            v = row[x]
            if v <= floor:
                continue
            nb = []
            for dy in (-1, 0, 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                nrow = d[yy]
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    xx = x + dx
                    if 0 <= xx < w:
                        nb.append(nrow[xx])
            n90 = p90(nb)
            ratio = v / max(n90, EPS)
            ratios.append(ratio)
            if v > k * max(n90, EPS):
                count += 1
    ratios.sort()
    q = lambda f: ratios[min(len(ratios) - 1, int(f * len(ratios)))] if ratios else 0.0
    return {"count_k": count, "k": k, "floor": floor, "active": len(ratios),
            "ratio_p50": q(0.5), "ratio_p90": q(0.9), "ratio_p99": q(0.99),
            "ratio_max": ratios[-1] if ratios else 0.0}


def gap_band(d: list[list[float]], w: int, h: int, act: float,
             rough: list[list[float]] | None = None, gate: float = 0.40) -> dict:
    """差分消失带（口径 → 模块 docstring 2）。距离 = 多源 BFS（4-连通步长 1 的精确网格距离
    与欧氏差 ≤0.41；判据线 1.0/1.5 在两种度量下语义一致：单像素缝两者都 ≤1.0）。

    `rough`（dump_gbuf=3 的 g/255 网格）在场时 gap 像素额外要求 **0 < rough < gate**
    （= 过门控的光滑面）：深度间隙伪影的受害面是「本该反射却没反射」的光滑像素，
    不加这个区域条件会把**门控域边界**（湿→干材质过渡，零 Δ 是正确行为）也算成缝。"""
    active = [[v > act for v in row] for row in d]
    # 多源 BFS：dist[y][x] = 到最近活跃像素的 4-连通步数；-1 = 不可达（全图无活跃）
    dist = [[-1] * w for _ in range(h)]
    dq = deque()
    for y in range(h):
        for x in range(w):
            if active[y][x]:
                dist[y][x] = 0
                dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        nd = dist[y][x] + 1
        for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= yy < h and 0 <= xx < w and dist[yy][xx] == -1:
                dist[yy][xx] = nd
                dq.append((yy, xx))
    gap = [[False] * w for _ in range(h)]
    n_gap = n_w1 = n_w15 = 0
    for y in range(h):
        for x in range(w):
            if d[y][x] > EPS or dist[y][x] <= 0:
                continue
            if rough is not None:
                r = rough[y][x]
                if not (0.0 < r < gate):
                    continue
            near = False
            for dy in range(-3, 4):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                arow = active[yy]
                for dx in range(-3, 4):
                    xx = x + dx
                    if 0 <= xx < w and arow[xx]:
                        near = True
                        break
                if near:
                    break
            if not near:
                continue
            gap[y][x] = True
            n_gap += 1
            if dist[y][x] > 1:
                n_w1 += 1
                if dist[y][x] > 1.5:
                    n_w15 += 1
    # 缝性（seam）：零 Δ 带的两侧都有活跃 = 侵蚀缝；只有一侧有活跃 = 活跃域的自然边界
    # （如 M1 出屏区与命中区的分界——零区是**宽**的，不是被啃出来的缝）。
    R_SEAM = 6

    def side_active(y: int, x: int, dy: int, dx: int) -> bool:
        for s in range(1, R_SEAM + 1):
            yy, xx = y + dy * s, x + dx * s
            if not (0 <= yy < h and 0 <= xx < w):
                return False
            v = d[yy][xx]
            if v > EPS:  # 走出零区：第一个非零像素必须活跃才算一侧
                return v > act
        return False

    n_seam = n_seam_w1 = 0
    seam = [[False] * w for _ in range(h)]
    seam_dists: list[int] = []
    for y in range(h):
        for x in range(w):
            if not gap[y][x]:
                continue
            horiz = side_active(y, x, 0, -1) and side_active(y, x, 0, 1)
            vert = side_active(y, x, -1, 0) and side_active(y, x, 1, 0)
            if horiz or vert:
                seam[y][x] = True
                n_seam += 1
                seam_dists.append(dist[y][x])
                if dist[y][x] > 1:
                    n_seam_w1 += 1
    seam_dists.sort()
    q = lambda f: seam_dists[min(len(seam_dists) - 1, int(f * len(seam_dists)))] if seam_dists else 0

    # 8-连通分量（{seam ∧ dist>1} = 「缝宽 ≥2px」的侵蚀带像素）
    seen = [[False] * w for _ in range(h)]
    comps: list[int] = []
    for y in range(h):
        for x in range(w):
            if seen[y][x] or not seam[y][x] or dist[y][x] <= 1:
                continue
            size = 0
            dq = deque([(y, x)])
            seen[y][x] = True
            while dq:
                cy, cx = dq.popleft()
                size += 1
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = cy + dy, cx + dx
                        if 0 <= yy < h and 0 <= xx < w and not seen[yy][xx] and seam[yy][xx] and dist[yy][xx] > 1:
                            seen[yy][xx] = True
                            dq.append((yy, xx))
            comps.append(size)
    return {"gap_px": n_gap, "wide_gt1px": n_w1, "wide_gt1_5px": n_w15,
            "seam_px": n_seam, "seam_wide_gt1px": n_seam_w1,
            "seam_dist_p50": q(0.5), "seam_dist_p90": q(0.9), "seam_dist_max": q(1.0),
            "components": len(comps), "components_ge20": sum(1 for c in comps if c >= 20),
            "max_component": max(comps) if comps else 0, "act": act}


def edge_ring(d: list[list[float]], w: int, h: int, ring: int) -> dict:
    """屏幕边缘环带 + 硬边密度（口径 → 模块 docstring 3）。

    R0 = 距屏缘 [0,ring)；R1 = [ring,2ring)；interior_ref = 距屏缘 ≥2ring 的深内部
    （硬边密度的"画面典型值"参照——fade 硬切若存在应显著高于它）。
    """
    def band_stats(pred) -> dict:
        n = act_n = hard = 0
        s_act = 0.0
        for y in range(h):
            for x in range(w):
                if not pred(x, y):
                    continue
                n += 1
                v = d[y][x]
                if v > 0.01:
                    act_n += 1
                    s_act += v
                    # 硬边：与四邻中任一「零 Δ」像素相邻（active→零 邻接，只数带内一侧防重复）
                    for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                        if 0 <= yy < h and 0 <= xx < w and d[yy][xx] <= EPS:
                            hard += 1
                            break
        return {"px": n, "active": act_n,
                "active_pct": 100.0 * act_n / n if n else 0.0,
                "mean_active_luma": s_act / act_n if act_n else 0.0,
                "hard_edge_per_px": hard / n if n else 0.0}

    def edge_dist(x: int, y: int) -> int:
        return min(x, y, w - 1 - x, h - 1 - y)

    return {"ring_px": ring,
            "R0": band_stats(lambda x, y: edge_dist(x, y) < ring),
            "R1": band_stats(lambda x, y: ring <= edge_dist(x, y) < 2 * ring),
            "interior_ref": band_stats(lambda x, y: edge_dist(x, y) >= 2 * ring)}


def rough_grid(path: Path) -> list[list[float]]:
    """dump_gbuf=3 的 roughness 网格（.g 通道 / 255，与 diffstruct --gbuf 同口径）。"""
    w, h, rows, ch = png_decode_rgb(path.read_bytes())
    return [[r[ch * x + 1] / 255.0 for x in range(w)] for r in rows]


def measure(off: Path, on: Path, k: float, floor: float, act: float, ring: int,
            gbuf: Path | None = None, gate: float = 0.40) -> dict:
    d, w, h = abs_delta(off, on)
    rough = rough_grid(gbuf) if gbuf else None
    return {"image": f"{w}x{h}", "off": str(off), "on": str(on),
            "firefly": firefly(d, w, h, k, floor),
            "gap_band": gap_band(d, w, h, act, rough, gate),
            "edge_ring": edge_ring(d, w, h, ring)}


# ── selftest：合成图阳性锚点（口径即判据，正向/反向都要有）──────────────────────────

def _selftest() -> int:
    fails: list[str] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}")
        if not ok:
            fails.append(name)

    W, H = 40, 40
    # 1a. firefly 阳性锚点：均匀活跃场 0.10 中一枚 0.90 孤立尖峰 → k=3 计数恰 1
    d = [[0.10] * W for _ in range(H)]
    d[20][20] = 0.90
    r = firefly(d, W, H, 3.0, 0.01)
    check("firefly 阳性（孤立尖峰 → 恰 1）", r["count_k"] == 1, f"count={r['count_k']}")
    # 1b. firefly 反向：全场均匀 → 0
    d = [[0.10] * W for _ in range(H)]
    r = firefly(d, W, H, 3.0, 0.01)
    check("firefly 反向（均匀场 → 0）", r["count_k"] == 0, f"count={r['count_k']}")
    # 1c. firefly floor：0.008 的孤立尖峰在 floor=0.01 下不进活跃集
    d = [[0.0008] * W for _ in range(H)]
    d[20][20] = 0.008
    r = firefly(d, W, H, 3.0, 0.01)
    check("firefly floor（暗尖峰不进活跃集）", r["active"] == 0 and r["count_k"] == 0,
          f"active={r['active']}")
    # 2. gap band：行 0–2 活跃、3–5 零缝、6+ 全活跃 → gap = 3 行 ×W；
    #    dist：行 3/5 = 1、行 4 = 2 ⟹ wide>1 与 >1.5 都恰一行（行 4）；分量 = 1 条横带
    d = [[0.1 if (y < 3 or y > 5) else 0.0 for _ in range(W)] for y in range(H)]
    g = gap_band(d, W, H, 0.02)
    check("gap 带宽度（3px 缝 → dist>1 与 >1.5 各恰一行，dist=2）",
          g["gap_px"] == 3 * W and g["wide_gt1px"] == W and g["wide_gt1_5px"] == W,
          f"gap={g['gap_px']} w1={g['wide_gt1px']} w15={g['wide_gt1_5px']}")
    check("gap 带成片（1 个 ≥20px 分量）",
          g["components"] == 1 and g["components_ge20"] == 1, f"comps={g['components']}")
    # 2b. gap 反向：活跃紧贴零（无邻域活跃）→ 0
    d = [[0.0] * W for _ in range(H)]
    g = gap_band(d, W, H, 0.02)
    check("gap 反向（全零图无活跃 → 0）", g["gap_px"] == 0, f"gap={g['gap_px']}")
    # 2c. gap 门控域限定：同一几何，缝行 rough=0.12（湿）→ 全算；rough=0.80（干）→ 全不算
    d = [[0.1 if (y < 3 or y > 5) else 0.0 for _ in range(W)] for y in range(H)]
    wet = [[0.12] * W for _ in range(H)]
    dry = [[0.80] * W for _ in range(H)]
    gw = gap_band(d, W, H, 0.02, wet)
    gd = gap_band(d, W, H, 0.02, dry)
    check("gap 门控域（湿缝全算 / 干缝全不算）",
          gw["gap_px"] == 3 * W and gd["gap_px"] == 0,
          f"wet={gw['gap_px']} dry={gd['gap_px']}")
    # 2d. 缝性判据的**反向用例**：单侧边界（行 0–2 活跃、其余全零，M1 型自然边界）
    #     → gap>0（邻域活跃成立）但 seam=0、分量=0（另一侧永远没有活跃）
    d = [[0.1 if y < 3 else 0.0 for _ in range(W)] for y in range(H)]
    g1 = gap_band(d, W, H, 0.02)
    check("缝性反向（单侧边界 gap>0 但 seam=0）",
          g1["gap_px"] > 0 and g1["seam_px"] == 0 and g1["components"] == 0,
          f"gap={g1['gap_px']} seam={g1['seam_px']} comps={g1['components']}")
    # 2e. 缝性阳性：双侧夹缝的 seam 计数与宽度（行 3–5 全为 seam，dist>1 恰行 4）
    d = [[0.1 if (y < 3 or y > 5) else 0.0 for _ in range(W)] for y in range(H)]
    g2 = gap_band(d, W, H, 0.02)
    check("缝性阳性（3px 缝 → seam=3W、seam宽>1 恰一行、缝宽 p90=2）",
          g2["seam_px"] == 3 * W and g2["seam_wide_gt1px"] == W and g2["seam_dist_p90"] == 2,
          f"seam={g2['seam_px']} w1={g2['seam_wide_gt1px']} p90={g2['seam_dist_p90']}")
    # 3. edge ring（120×120，ring=24）：仅外圈 24px 环带有贡献 → R0 亮、R1/深内部不亮；
    #    环带在 e=23/24 处留下 active→零 硬邻接，深内部没有
    RING = 24
    W = H = 120
    d = [[0.5 if min(x, y, W - 1 - x, H - 1 - y) < RING else 0.0 for x in range(W)] for y in range(H)]
    e = edge_ring(d, W, H, RING)
    check("edge ring 阳性（环带亮、邻带/深内部不亮）",
          e["R0"]["active"] > 0 and e["R1"]["active"] == 0 and e["interior_ref"]["active"] == 0,
          f"R0={e['R0']['active']} R1={e['R1']['active']} int={e['interior_ref']['active']}")
    check("edge ring 硬边（环带有 active→零 硬邻接、深内部无）",
          e["R0"]["hard_edge_per_px"] > 0 and e["interior_ref"]["hard_edge_per_px"] == 0.0,
          f"hardR0={e['R0']['hard_edge_per_px']:.3f}")
    # 3b. edge ring 反向：全零 → 各带都无活跃、无硬边
    d = [[0.0] * W for _ in range(H)]
    e = edge_ring(d, W, H, RING)
    check("edge ring 反向（全零）",
          e["R0"]["active"] == 0 and e["R0"]["hard_edge_per_px"] == 0.0)

    print(f"selftest: {'PASS' if not fails else 'FAIL: ' + ', '.join(fails)}")
    return 0 if not fails else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="SSR 伪影度量（firefly / 深度间隙断裂带 / 屏幕边缘环带）")
    ap.add_argument("off", nargs="?", help="SSR 关 PNG")
    ap.add_argument("on", nargs="?", help="SSR 开 PNG")
    ap.add_argument("--firefly-k", type=float, default=3.0, help="firefly 比值阈值 k（默认 3）")
    ap.add_argument("--firefly-floor", type=float, default=0.01,
                    help="firefly 活跃门槛（线性亮度，默认 0.01）")
    ap.add_argument("--gap-act", type=float, default=0.02,
                    help="断裂带的『邻域活跃』门槛（默认 0.02）")
    ap.add_argument("--ring", type=int, default=24, help="边缘环带宽度 px（默认 24 = kEdgeFadePx）")
    ap.add_argument("--gbuf", help="dump_gbuf=3 的 metalrough PNG ⟹ gap 像素限定过门控域（0<rough<gate）")
    ap.add_argument("--gate", type=float, default=0.40, help="门控上界（g/255 口径，默认 0.40 同 diffstruct）")
    ap.add_argument("--json", action="store_true", help="输出 JSON（喂表格/台账）")
    ap.add_argument("--selftest", action="store_true", help="合成图阳性锚点（不需要输入图）")
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if not a.off or not a.on:
        print("ssr_artifacts: 需要 <off.png> <on.png>（或 --selftest）", file=sys.stderr)
        return 1
    r = measure(Path(a.off), Path(a.on), a.firefly_k, a.firefly_floor, a.gap_act, a.ring,
                Path(a.gbuf) if a.gbuf else None, a.gate)
    if a.json:
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    f, g, e = r["firefly"], r["gap_band"], r["edge_ring"]
    print(f"图 {r['image']}   off={Path(a.off).name} on={Path(a.on).name}")
    print(f"\n1) firefly（|Δ|>{a.firefly_floor:g} ∧ |Δ| > {a.firefly_k:g}×邻域3×3 p90）")
    print(f"   计数 = {f['count_k']}   活跃集(|Δ|>floor) = {f['active']}")
    print(f"   比值 |Δ|/邻域p90 分布: p50={f['ratio_p50']:.2f} p90={f['ratio_p90']:.2f} "
          f"p99={f['ratio_p99']:.2f} max={f['ratio_max']:.2f}")
    gate_note = f" ∧ 0<rough<{a.gate:g}（门控域）" if a.gbuf else "（未给 --gbuf ⟹ 无门控域限定）"
    print(f"\n2) 深度间隙断裂带（|Δ|≤ε ∧ 半径3邻域有 |Δ|>{a.gap_act:g}{gate_note}）")
    print(f"   gap 像素 = {g['gap_px']}   缝宽>1px = {g['wide_gt1px']}   >1.5px = {g['wide_gt1_5px']}")
    print(f"   侵蚀缝（双侧活跃）= {g['seam_px']}   其中缝宽>1px = {g['seam_wide_gt1px']}   "
          f"缝宽 dist p50/p90/max = {g['seam_dist_p50']}/{g['seam_dist_p90']}/{g['seam_dist_max']}")
    print(f"   缝分量（seam∧宽>1，8-连通）= {g['components']}（≥20px 的 {g['components_ge20']} 个，"
          f"最大 {g['max_component']} px）")
    print(f"\n3) 屏幕边缘环带（ring = {e['ring_px']}px = kEdgeFadePx）")
    for tag in ("R0", "R1", "interior_ref"):
        b = e[tag]
        name = {"R0": "外圈 [0,ring)", "R1": "邻带 [ring,2ring)", "interior_ref": "深内部（≥2ring）"}[tag]
        print(f"   {name:28s} px={b['px']:7d} 活跃={b['active']:6d} ({b['active_pct']:5.2f}%) "
              f"活跃均值={b['mean_active_luma']:.4f} 硬边密度={b['hard_edge_per_px']:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
