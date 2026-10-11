#!/usr/bin/env python3
"""SSR 闭式反射靶的判据算子（批 15.1；判据的**几何来源** = `ssr_rig` manifest）。

为什么需要它：`just ssr-target` 只生成场景与一张 SSR 关的图，**判据本身**（开/关差分
在闭式凸包内外的行为）得有人算。区域来自**几何**（manifest 里的凸包），不来自图像——
这是本项目区域归因的一贯做法（同 `tools/imgqa/`）。

两条判据（都必须是 SSR **开/关差分**，绝对亮度判据无效，理由见 manifest 的 honesty ①）：

  **A 阳性锚点** = `reflectionHullPx \\ pillarHullPx` 必须**变亮**，且覆盖率
  ≥ `--min-coverage`。覆盖率是主判据：SSR 的四种典型缺陷（起点错 / 方向错 / 行进
  方向错 / 穿越判据状态机失效）都表现为**部分像素整条射线没命中**，而不是整体偏移，
  所以"平均亮度变了"抓不住，必须数像素。实测覆盖率对缺陷极敏感：状态机留死区时
  74%（漏的按行交替 = 横条纹），修完 97.2%。

  **B 因果泄漏恒等式** = 凸包**外**（去柱身）必须**逐位不变**。SSR 只可能在闭式预测
  的倒影区产生作用，区外任何改动都是假命中。⚠ 判据带 `--edge-px`（默认 4）的**边界
  豁免带**：闭式多边形是解析投影，实际反射边由逐像素射线求交决定，两者差 ~1px 是
  **量化**不是错误（15.1 全分辨率单 pass 实测泄漏 115 px 全部距边界 ≤1.18px）。
  **批 15.3 子步 B 起豁免带 = 4px**（按实测重设）：apply 的粗糙度感知螺旋模糊把影响
  域外推，理论 = 半分辨率双线性 footprint（~2px）+ 模糊半径（roughness × blur_scale）；
  靶 roughness 恒 0.122、出厂 blur_scale=8 ⟹ 实测泄漏 639 px 全部 ≤3.37px
  （分布 [0,1)=334 / [1,2)=223 / [2,3)=72 / [3,4)=10），消融臂逐条对上公式：
  blur_scale=0 → 最远 2.4px（纯 footprint）、=16 → 最远 4.4px（半径翻倍）。
  豁免带只作用于 B；真正的"逐位"红线是 `just regress ab`（SSR 门默认关 ⟹
  默认配置必须逐位不变）。

柱身从两侧都摘掉：它自己也能反射地面（合法作用），而它与倒影在屏幕上**部分重叠**
（honesty ②），拿屏幕 bbox 当掩码会把柱身算成倒影。

用法：
    just ssr-check                 # 自己跑 SSR 开/关两张图，再判
    just ssr-check -- --off a.png --on b.png   # 用现成的两张图（调试现场）
"""

import argparse
import json
import math
import os
import struct
import subprocess
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, "build", "rgl", "models", "ssr_rig.manifest.json")


def png_decode_rgb(blob):
    """最小 PNG 解码（8bit RGB/RGBA、非隔行）——stdlib only（工具语言纪律）。"""
    pos, w, h, ch = 8, 0, 0, 4
    idat = bytearray()
    while pos < len(blob):
        ln, tag = struct.unpack(">I4s", blob[pos:pos + 8])
        pos += 8
        data = blob[pos:pos + ln]
        pos += ln + 4
        if tag == b"IHDR":
            w, h, _depth, ctype, _comp, _filt, _inter = struct.unpack_from(">IIBBBBB", data, 0)
            ch = 4 if ctype == 6 else 3
        elif tag == b"IDAT":
            idat += data
        elif tag == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = w * ch
    rows, prev, out = [], bytes(stride), []
    p = 0
    for _ in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line))
        prev = bytes(line)
        out.append(line)
    return w, h, out, ch


def inside(poly, x, y):
    """射线法（偶数交叉）——凸包也用它，省得为靶特化一套。"""
    hit = False
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        if (y0 > y) != (y1 > y):
            if x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                hit = not hit
    return hit


def dist_to_polygon(poly, x, y):
    """到多边形边界的最近距离（判据 B 的边界豁免带用）。"""
    best, n = 1e30, len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        dx, dy = x1 - x0, y1 - y0
        l2 = dx * dx + dy * dy
        t = 0.0 if l2 == 0.0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / l2))
        best = min(best, math.hypot(x - (x0 + t * dx), y - (y0 + t * dy)))
    return best


def luma(px):
    """sRGB → 线性 → Rec.709 亮度。判据比较的是**线性**亮度：截图是 sRGB 编码的，
    直接比字节会把 0.5 处的等量变化当成比 0.9 处大得多。"""
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin(px[0]) + 0.7152 * lin(px[1]) + 0.0722 * lin(px[2])


def shoot(name, ssr):
    """跑一次 headless 截图（走 RGL_CONFIG_DIR staging，不碰 configs 副本）。"""
    cmd = [sys.executable, os.path.join("tools", "smoke.py"), "shot",
           "--config-base", "qa/configs/ssr_rig", "--name", name]
    if ssr:
        cmd.append("--ssr")
    subprocess.run(cmd, cwd=REPO, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(REPO, "build", "rgl", "screenshots", f"{name}.png")


def main():
    ap = argparse.ArgumentParser(description="SSR 闭式反射靶判据算子")
    ap.add_argument("--off", help="SSR 关的 PNG（不给 = 自己跑）")
    ap.add_argument("--on", help="SSR 开的 PNG（不给 = 自己跑）")
    ap.add_argument("--min-coverage", type=float, default=0.90,
                    help="判据 A：凸包内必须变亮的像素占比下限（默认 0.90）")
    ap.add_argument("--edge-px", type=float, default=4.0,
                    help="判据 B：距凸包边界多少像素以内豁免（默认 4 = 15.3B 半分辨率 footprint"
                         " + 出厂模糊半径的实测上界 3.37px，理由见模块 docstring）")
    ap.add_argument("--row-bands", action="store_true",
                    help="追加判据 A 区域的**逐行带漏分布**（40px 带，批 15.3 子步 C 结案 §3.2③ 用："
                         "15.1 时代 2.8%% 漏集中在两端行带 480–519/680–719，要看的是**分布形状**"
                         "不是总数）")
    a = ap.parse_args()

    if not os.path.exists(MANIFEST):
        print(f"✗ 找不到靶的 manifest：{MANIFEST}\n  先跑 `just ssr-target` 生成场景。", file=sys.stderr)
        return 2

    off = a.off or shoot("ssr_hull_off", ssr=False)
    on = a.on or shoot("ssr_hull_on", ssr=True)

    man = json.load(open(MANIFEST))["probe"]
    hull, pillar = man["reflectionHullPx"], man["pillarHullPx"]
    w, h, off_rows, ch = png_decode_rgb(open(off, "rb").read())
    _, _, on_rows, _ = png_decode_rgb(open(on, "rb").read())
    if (w, h) != (man.get("width"), man.get("height")) and man.get("width"):
        print(f"⚠ 图尺寸 {w}x{h} 与 manifest 记录不一致——凸包是按生成时的分辨率算的")

    band_a = band_b = brighter = darker = leak = 0
    leak_far = 0.0
    deltas = []
    miss_by_row = [0] * h  # --row-bands：判据 A 区域内每行「未变亮」像素数（③ 结案用）
    region_by_row = [0] * h
    for y in range(h):
        ob, nb = off_rows[y], on_rows[y]
        for x in range(w):
            opx, npx = ob[ch * x:ch * x + 3], nb[ch * x:ch * x + 3]
            cx, cy = x + 0.5, y + 0.5
            if inside(pillar, cx, cy):
                continue
            if inside(hull, cx, cy):
                band_a += 1
                region_by_row[y] += 1
                d = luma(npx) - luma(opx)
                deltas.append(d)
                if d > 1e-6:
                    brighter += 1
                elif d < -1e-6:
                    darker += 1
                else:
                    miss_by_row[y] += 1
            else:
                band_b += 1
                if opx != npx:
                    d = dist_to_polygon(hull, cx, cy)
                    if d > a.edge_px:
                        leak += 1
                        leak_far = max(leak_far, d)

    deltas.sort()
    n = len(deltas)
    q = lambda f: deltas[min(n - 1, int(f * n))] if n else 0.0
    cov = brighter / band_a if band_a else 0.0
    print(f"图 {w}x{h}   凸包顶点 {len(hull)} / 柱身凸包顶点 {len(pillar)}")
    print(f"\nA 闭式倒影凸包内（去柱身）{band_a} px = 画面 {100.0 * band_a / (w * h):.2f}%")
    print(f"    变亮 {brighter} ({100.0 * cov:.1f}%) · 变暗 {darker} · "
          f"Δ线性亮度 p50={q(.5):+.5f} p90={q(.9):+.5f} max={deltas[-1]:+.5f}" if n else "    （区域为空）")
    print(f"B 凸包外（去柱身）{band_b} px，边界 ±{a.edge_px:g}px 豁免后泄漏 {leak} px"
          + (f"，最远 {leak_far:.1f}px" if leak else ""))

    if a.row_bands:
        # 逐行带漏分布（40px 带 = 15.1 时代 ③ 记录的行带宽，直接可比）。两端带 = 掠射端与
        # 近端（15.1 实测 2.8% 漏集中处）；结案判据 = 两端带漏率与中部**同量级**，不是总数。
        print("\nA 区域逐行带漏分布（40px 带；漏 = 区域内未变亮且未变暗）：")
        bands = [(y0, min(y0 + 40, h)) for y0 in range(0, h, 40)]
        rows_out = []
        for y0, y1 in bands:
            reg = sum(region_by_row[y0:y1])
            mis = sum(miss_by_row[y0:y1])
            if reg:
                rows_out.append((y0, y1, reg, mis))
        for y0, y1, reg, mis in rows_out:
            mark = " ← 15.1 ③ 两端带" if (y0, y1) in [(480, 520), (680, 720)] else ""
            print(f"    rows {y0:3d}–{y1 - 1:3d}: 区域 {reg:6d}  漏 {mis:5d}  "
                  f"漏率 {100.0 * mis / reg:5.2f}%{mark}")
        edge = [(r, m) for y0, y1, r, m in rows_out if (y0, y1) in [(480, 520), (680, 720)]]
        mid = [(r, m) for y0, y1, r, m in rows_out if (y0, y1) not in [(480, 520), (680, 720)]]
        if edge and mid and all(r for r, _ in edge):
            er = sum(m for r, m in edge) / sum(r for r, _ in edge)
            mr = sum(m for r, m in mid) / sum(r for r, _ in mid)
            print(f"    两端带漏率 {100.0 * er:.2f}% vs 其余带 {100.0 * mr:.2f}%"
                  f"（比值 {er / mr if mr else float('inf'):.2f}×；≈1 = 已收敛到同量级）")

    ok_a = n > 0 and cov >= a.min_coverage and darker == 0
    ok_b = leak == 0
    print(f"\n判据 A 阳性锚点（凸包内变亮 ≥{100.0 * a.min_coverage:.0f}% 且无变暗）: "
          + ("PASS" if ok_a else f"FAIL（覆盖率 {100.0 * cov:.1f}%）"))
    print("判据 B 因果泄漏恒等式（凸包外逐位不变）: " + ("PASS" if ok_b else f"FAIL（{leak} px）"))
    print("\n注：两条判据都只在 SSR 门**开**时成立；SSR 门**关**时的逐位红线是 "
          "`just regress ab --expect <红线A>`。")
    return 0 if (ok_a and ok_b) else 1


if __name__ == "__main__":
    sys.exit(main())
