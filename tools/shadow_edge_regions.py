#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shadow_edge_regions.py —— 把「逐位不同像素」按**内容类别**拆开（相对坐标批 5 的诊断仪器）。

为什么需要它（一句话）：`imgqa shadow-edge-rebase` 报的「③ vs ① 逐位不同 = 91.3 %」是一个
**不分类的整幅差分**——天穹、地面、阴影项全混在同一个数里，于是它既证明不了「远点还有阴影之外
的系统性差异」，也证明不了「仪器取景坏了」。本脚本不新造判据、不改任何界，只把同一个数拆开：

    总量 = 阴影项贡献 + 场景非阴影贡献 + 非场景（天穹）贡献

三条口径（全部闭式，不引入可调阈值）：
  1. **阴影项** = 同一格内 `none` 臂与 `d5` 臂之差（两臂只差阴影项）。于是「把阴影项整个拿掉
     之后还剩多少差异」= `none` 臂 vs `none` 臂——这一刀**不需要任何掩码**；掩码只用来把剩下
     的部分再按内容分类。
  2. **场景 / 非场景**由靶的 manifest 闭式重建（9 个 piece 在 (u,v,y) 系里的真实长方体）。
     🔴 不能直接拿 manifest 的 `aabb` 当盒子：那是**旋转之后**的外接盒，比真实盒子大一圈
     （实测把场景面积从 38.7 % 抬到 78.7 %，见 `--selftest` 的闭式对照）。
  3. 严重度与「逐位不同」**分开报**：逐位是零容差算子，分不清「天穹糊成一团」与「天穹差
     1/4 个像素」——两者都是 ~100 % 逐位不同。所以同时给平均通道差、>32 LSB 面积、以及
     **配准无关的锐度**（天穹区梯度能量：糊了就是低，不需要跟任何一张图比）。

用法：python3 tools/shadow_edge_regions.py [--root build/rgl] [--selftest]
只读 build/rgl/screenshots/imgqa_shadow_edge_rebase_*.png 与 build/rgl/models/*.manifest.json；
零写入、零依赖（stdlib）、不启引擎。
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import zlib

# ── 与量具同源的常数（改动 = 对不上仪器；出处 = tools/imgqa/shadow_edge_ruler.h）──────
WIDTH, HEIGHT = 1600, 900
STEM_BASE, STEM_FAR = "shadow_edge_rig", "shadow_edge_rig_far"
ARMS = ("none", "d5")
CELLS = (  # (标签, manifest stem, rebase 开关) —— 与 shadow-edge-rebase 的 ①②③④ 同序
    ("①出厂·门关", STEM_BASE, "off"),
    ("②出厂·门开", STEM_BASE, "on"),
    ("③远点·门关", STEM_FAR, "off"),
    ("④远点·门开", STEM_FAR, "on"),
)
NEAR_EPS = 1e-3  # 与 projectPoint3 的「相机背后」判据同值


# ══════════════════════════════════ PNG 读取 ═══════════════════════════════════
def png_read_rgb(path: str):
    """读 8 位非隔行 PNG（灰度/RGB/RGBA）→ (rows, cols, 每像素字节数, 像素字节串)。

    RGBA → 丢 alpha，与 imgqa 的 `readPngBgr`（cv::IMREAD_COLOR）同口径——🔴 这条必须一致，
    否则本脚本数的「逐位不同」与仪器数的不是同一件事。
    """
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG: " + path)
    pos, idat, hdr = 8, [], None
    while pos + 8 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body[:13])
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
        pos += 12 + ln
    cols, rows, depth, ctype, comp, _filt, inter = hdr
    if depth != 8 or inter != 0 or comp != 0:
        raise ValueError(f"只支持 8 位非隔行无交错 PNG，收到 depth={depth} color={ctype} "
                         f"interlace={inter}: {path}")
    nch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = cols * nch
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(rows):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:  # Sub
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:  # Average
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:  # Paeth
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError("未知行滤波 %d: %s" % (ft, path))
        out += line
        prev = line
    if nch == 4:
        rgb = bytearray()
        for i in range(0, len(out), 4):
            rgb += out[i:i + 3]
        return rows, cols, 3, bytes(rgb)
    return rows, cols, nch, bytes(out)


def png_write_rgba_filtered(path: str, rows: int, cols: int, px: bytes, filt: int) -> None:
    """每行都用同一种滤波写 PNG（0..4）。

    为什么必须能写 5 种滤波：真截图的滤波是 libpng 自适应选的，本脚本的解码器要五种全通；
    只测滤波 0 = 解码器 4/5 的代码从没被跑过，而它一旦写错，**所有**读数都建在错像素上。
    """
    stride = cols * 4
    raw = bytearray()
    prev = bytearray(stride)
    for y in range(rows):
        line = bytearray(px[y * stride:(y + 1) * stride])
        enc = bytearray(stride)
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if filt == 0:
                p = 0
            elif filt == 1:
                p = a
            elif filt == 2:
                p = b
            elif filt == 3:
                p = (a + b) >> 1
            else:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                p = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            enc[i] = (line[i] - p) & 0xFF
        raw.append(filt)
        raw += enc
        prev = line
    body = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", cols, rows, 8, 6, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(struct.pack(">I", 13) + b"IHDR" + ihdr + struct.pack(">I", zlib.crc32(b"IHDR" + ihdr)))
        fh.write(struct.pack(">I", len(body)) + b"IDAT" + body + struct.pack(">I", zlib.crc32(b"IDAT" + body)))
        fh.write(struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND")))


# ════════════════════════ 靶的闭式几何（manifest = 唯一真相源）═══════════════════
def _basis(man):
    u, v = man["measure"]["u"], man["measure"]["v"]
    if abs(math.hypot(*u) - 1.0) > 1e-9 or abs(u[0] * v[0] + u[1] * v[1]) > 1e-9:
        raise ValueError("manifest 的 measure.u/v 不是单位正交基（生成器漂了）")
    return (u[0], 0.0, u[1]), (v[0], 0.0, v[1])


def _solve3(rows, rhs):
    """3×3 线性方程组（列主元高斯消元，stdlib）。奇异返回 None，由调用方换一组方程。"""
    a = [list(r) + [b] for r, b in zip(rows, rhs)]
    for col in range(3):
        piv = max(range(col, 3), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-15:
            return None
        a[col], a[piv] = a[piv], a[col]
        pv = a[col][col]
        a[col] = [x / pv for x in a[col]]
        for r in range(3):
            if r != col and a[r][col]:
                f = a[r][col]
                a[r] = [x - f * y for x, y in zip(a[r], a[col])]
    return [a[i][3] for i in range(3)]


def ground_rect(man):
    """从 ground piece 的 AABB + u/v 基反解 (u0, u1, v_half)，四条边全部参与对账。

    地面在 (u,v) 里是轴对齐矩形（v 关于 0 对称），但靶的 u 轴相对世界 x 轴转了 ~42°，于是
    manifest 里的 AABB 是**转完之后**的外接盒——直接拿它当盒子会把场景面积读大一倍。
    反解 = 按基的符号写出四条边的线性方程、取三条解 3×3、第四条做冗余校验：解错会立刻
    对不上，不会静默给出一个胖一圈的矩形（`--selftest` ⑩ 就是打这条校验的）。
    """
    u3, v3 = _basis(man)
    pc = [p for p in man["pieces"] if p["name"] == "ground"][0]
    lo, hi = pc["aabb"]["min"], pc["aabb"]["max"]
    if lo[1] != 0.0 or hi[1] != 0.0:
        raise ValueError("ground 的 aabb 不在 y=0 平面上（靶前提① 破了）")
    ux, uz, vx, vz = u3[0], u3[2], v3[0], v3[2]

    def rows_for(axis):
        """返回 (max 边方程, min 边方程)，每条 = (u0,u1,vh 的系数, 右端)。

        max 边取「u1 还是 u0」由该轴 u 分量的符号决定（系数为 0 的那一侧自动消失）；
        v 分量在 max 边恒取 +|·|、min 边恒取 −|·|（矩形关于 v=0 对称）。
        """
        pa, qa = (ux, vx) if axis == 0 else (uz, vz)
        hi_, lo_ = (hi[0], lo[0]) if axis == 0 else (hi[2], lo[2])
        rmax = [pa if pa < 0 else 0.0, pa if pa > 0 else 0.0, abs(qa)]
        rmin = [pa if pa > 0 else 0.0, pa if pa < 0 else 0.0, -abs(qa)]
        return (rmax, hi_), (rmin, lo_)

    e0max, e0min = rows_for(0)
    e1max, e1min = rows_for(1)
    eqs = [e0max, e0min, e1max, e1min]
    sol = None
    for pick in ((0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)):
        sol = _solve3([eqs[i][0] for i in pick], [eqs[i][1] for i in pick])
        if sol is not None:
            break
    if sol is None:
        raise ValueError("地面矩形反解：四条边方程组奇异（u/v 基退化）")
    u0, u1, v_half = sol[0], sol[1], sol[2]
    if v_half <= 0 or u1 <= u0:
        raise ValueError(f"地面矩形反解给出退化矩形 u0={u0} u1={u1} vh={v_half}")
    checks = [(max(u0 * ux, u1 * ux) + v_half * abs(vx), hi[0], "max_x"),
              (min(u0 * ux, u1 * ux) - v_half * abs(vx), lo[0], "min_x"),
              (max(u0 * uz, u1 * uz) + v_half * abs(vz), hi[2], "max_z"),
              (min(u0 * uz, u1 * uz) - v_half * abs(vz), lo[2], "min_z")]
    for got, want, what in checks:
        # 容差 1e-5 m：manifest 的 aabb 是 **float32** 落盘的顶点极值，ULP(6.5 m) = 4.8e-7 m，
        # 反解再精确也对不到 double 的 0。1e-5 = 20 倍 ULP，同时比最细级联纹素（0.5 mm）
        # 小 50 倍 ⟹ 抓得住「aabb 被改胖」这类真错（`--selftest` ⑩ 打的就是它），
        # 又不会被浮点表示本身误报。
        if abs(got - want) > 1e-5:
            raise ValueError(f"地面矩形反解对不上 {what}: {got} vs {want}")
    return u0, u1, v_half


def boxes(man):
    """piece 在 (u,v,y) 系里的真实长方体：(name, u0,u1, v0,v1, y0,y1)。

    地面 = y 退化的那一个（`pieces` 里没有名为 ground 的 piece 时不硬造——`--selftest` 的
    反向用例要的就是「只有板、没有地面」的清单）。
    """
    me = man["measure"]
    lv, h = me["plateLengthM"], me["H"]
    out = []
    if any(p["name"] == "ground" for p in man["pieces"]):
        g0, g1, vh = ground_rect(man)
        out.append(("ground", g0, g1, -vh, vh, 0.0, 0.0))
    for p in me["plates"]:
        uc, t = p["uCenter"], p["thicknessM"]
        out.append((p["name"], uc - t / 2.0, uc + t / 2.0, -lv / 2.0, lv / 2.0, 0.0, h))
    return out


def aabb_roundtrip_error(man) -> float:
    """重建长方体的 8 角点转回世界，与 manifest 的 aabb 比；返回最大偏差（米）。

    🔴 `pieces[].aabb` 是**资产局部系**（GLB 顶点极值，两枚靶逐字节相同），`measure.*` 才是
    世界系（远点靶的 farFaceX = 1e6 + 局部）——对账必须给 aabb 补上 `measure.worldOffset`，
    否则远点靶凭空差 1e6 m（本函数第一次跑就是这么抓出来的）。
    """
    u3, v3 = _basis(man)
    off = man["measure"].get("worldOffset", [0.0, 0.0, 0.0])
    pcs = {p["name"]: p["aabb"] for p in man["pieces"]}
    worst = 0.0
    for (nm, bu0, bu1, bv0, bv1, by0, by1) in boxes(man):
        pts = [[uu * u3[0] + vv * v3[0] + off[0], yy + off[1], uu * u3[2] + vv * v3[2] + off[2]]
               for uu in (bu0, bu1) for vv in (bv0, bv1) for yy in (by0, by1)]
        mn = [min(p[k] for p in pts) for k in range(3)]
        mx = [max(p[k] for p in pts) for k in range(3)]
        lo = [c + o for c, o in zip(pcs[nm]["min"], off)]
        hi = [c + o for c, o in zip(pcs[nm]["max"], off)]
        worst = max(worst, max(abs(a - b) for a, b in zip(mn, lo)),
                    max(abs(a - b) for a, b in zip(mx, hi)))
    return worst


def aabb_mask_area_fraction(man, proj) -> float:
    """把 manifest 的 aabb **直接**当盒子时的场景面积占比（只用来给「为什么不能用 aabb」出数）。"""
    off = man["measure"].get("worldOffset", [0.0, 0.0, 0.0])
    bs = [(p["aabb"]["min"][0] + off[0], p["aabb"]["max"][0] + off[0],
           p["aabb"]["min"][1] + off[1], p["aabb"]["max"][1] + off[1],
           p["aabb"]["min"][2] + off[2], p["aabb"]["max"][2] + off[2]) for p in man["pieces"]]
    n = span_mask(proj, bs).count(1)
    return n / (WIDTH * HEIGHT)


# ══════════════════════ 投影（与 imgqa shadow_kernel_metric 逐式同）══════════════
def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _norm(a):
    n = math.sqrt(_dot(a, a))
    return [x / n for x in a]


class Projector:
    """竖直 fov 的针孔相机（aspect 只压横轴）= `makeProjector` / `projectPoint3` 的 Python 版。

    🔴 逐式同 = 本脚本的掩码与仪器的扫描窗落在同一批像素上；抄错一条 = 分类与判据各说各话。
    """

    def __init__(self, cam, fov_deg, cols=WIDTH, rows=HEIGHT):
        self.pos = [float(x) for x in cam["position"]]
        f = [float(cam["target"][k]) - float(cam["position"][k]) for k in range(3)]
        self.f = _norm(f)
        self.r = _norm(_cross(self.f, [0.0, 1.0, 0.0]))
        self.up = _cross(self.r, self.f)
        self.tan_half = math.tan(math.radians(float(fov_deg)) / 2.0)
        self.cols, self.rows = cols, rows
        self.aspect = cols / rows

    def view(self, w):
        d = [w[k] - self.pos[k] for k in range(3)]
        return (_dot(d, self.r), _dot(d, self.up), _dot(d, self.f))

    def pixel(self, xv, yv, zv):
        xd = xv / (self.tan_half * self.aspect) / zv
        yd = yv / self.tan_half / zv
        if not (-1.0 <= xd <= 1.0 and -1.0 <= yd <= 1.0):
            return None
        return ((xd * 0.5 + 0.5) * self.cols, (0.5 - yd * 0.5) * self.rows)

    def point3(self, w):
        """世界点 → (px, py)；相机背后或屏外 = None（与 projectPoint3 同：不夹到边缘）。"""
        xv, yv, zv = self.view(w)
        if zv <= NEAR_EPS:
            return None
        return self.pixel(xv, yv, zv)


def _clip_half(poly, n, d, eps):
    """Sutherland–Hodgman：把凸多边形裁到半空间 n·p + d ≥ 0（poly = [(x,y,z)…]）。"""
    out = []
    m = len(poly)
    for i in range(m):
        a, b = poly[i], poly[(i + 1) % m]
        fa = _dot3(n, a) + d
        fb = _dot3(n, b) + d
        if fb >= 0:
            if fa < 0:
                t = -fa / (fb - fa)
                out.append([a[k] + t * (b[k] - a[k]) for k in range(3)])
            out.append(b)
        elif fa >= 0:
            t = -fa / (fb - fa)
            out.append([a[k] + t * (b[k] - a[k]) for k in range(3)])
    return out


def _dot3(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _box_faces(b):
    (x0, x1, y0, y1, z0, z1) = b
    c = [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
         [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]]
    return [[c[0], c[3], c[2], c[1]], [c[4], c[5], c[6], c[7]], [c[0], c[1], c[5], c[4]],
            [c[2], c[3], c[7], c[6]], [c[1], c[2], c[6], c[5]], [c[0], c[4], c[7], c[3]]]


def span_mask(proj, world_boxes):
    """世界轴对齐长方体集合 → 逐像素掩码（bytearray，1 = 被任一盒挡住）。

    做法：每个盒的 6 个面投到视空间、**先按视锥体五个半空间裁好**（近平面 + 四侧；不裁就
    会出现「一个顶点投出画幅 ⟹ 整面被弃 ⟹ 大地面被读成零像素」这种静默失效），再投到像素
    平面（凸多边形），按扫描行取 x 区间并集。
    为什么不是逐像素 slab：1.44 M 像素 × 9 个盒的纯 Python 循环要几分钟；扫描行是 900 行 ×
    54 个多边形，秒级。凸体在相机前方的投影 = 其各面投影的并 ⟹ 两者等价（`--selftest` ⑥
    拿一个闭式地平面对它求值：相机高 1、水平看向 +z、y=0 的无限大地面必须**恰好**盖住下半幅）。
    """
    th, aspect = proj.tan_half, proj.aspect
    planes = [((0.0, 0.0, 1.0), -NEAR_EPS), ((-1.0, 0.0, th * aspect), 0.0),
              ((1.0, 0.0, th * aspect), 0.0), ((0.0, -1.0, th), 0.0), ((0.0, 1.0, th), 0.0)]
    polys = []
    for b in world_boxes:
        for face in _box_faces(b):
            vs = [proj.view(p) for p in face]
            for (n, d) in planes:
                vs = _clip_half(vs, n, d, NEAR_EPS)
                if len(vs) < 3:
                    break
            if len(vs) < 3:
                continue
            pts = []
            for v in vs:
                xv, yv, zv = v
                xd = xv / (th * aspect) / zv
                yd = yv / th / zv
                # 已经裁进视锥，残差的越界只可能是浮点舍入（clip 落在面上的顶点）；
                # 真越界（>1e-6）说明裁剪面写错，必须响，不能静默夹掉。
                if abs(xd) > 1.0 + 1e-6 or abs(yd) > 1.0 + 1e-6:
                    raise RuntimeError(f"视锥裁剪后顶点仍投出画幅（xd={xd} yd={yd}）——裁剪面写错了")
                xd = min(1.0, max(-1.0, xd))
                yd = min(1.0, max(-1.0, yd))
                pts.append(((xd * 0.5 + 0.5) * proj.cols, (0.5 - yd * 0.5) * proj.rows))
            polys.append(pts)
    mask = bytearray(proj.rows * proj.cols)
    for pts in polys:
        ys = [p[1] for p in pts]
        # 像素 i 覆盖连续区间 [i, i+1)（projectPoint3 的 px = (xd·0.5+0.5)·cols 口径）
        r0 = max(0, int(math.floor(min(ys))))
        r1 = min(proj.rows - 1, int(math.ceil(max(ys))) - 1)
        ne = len(pts)
        for r in range(r0, r1 + 1):
            yc = r + 0.5
            xs = []
            for i in range(ne):
                a, b = pts[i], pts[(i + 1) % ne]
                if a[1] == b[1]:
                    if abs(yc - a[1]) < 1e-12:
                        xs += [a[0], b[0]]
                    continue
                t = (yc - a[1]) / (b[1] - a[1])
                if 0.0 <= t <= 1.0:
                    xs.append(a[0] + t * (b[0] - a[0]))
            if not xs:
                continue
            i0 = max(0, int(math.floor(min(xs))))
            i1 = min(proj.cols - 1, int(math.ceil(max(xs))) - 1)
            base = r * proj.cols
            for i in range(i0, i1 + 1):
                mask[base + i] = 1
    return mask


def scene_mask(man, proj):
    """逐像素：任一 piece 的真实长方体挡住这条视线 = 场景像素。"""
    u3, v3 = _basis(man)
    off = man["measure"].get("worldOffset", [0.0, 0.0, 0.0])
    wb = []
    for (_nm, bu0, bu1, bv0, bv1, by0, by1) in boxes(man):
        xs = [bu0 * u3[0] + bv0 * v3[0] + off[0], bu1 * u3[0] + bv1 * v3[0] + off[0]]
        zs = [bu0 * u3[2] + bv0 * v3[2] + off[2], bu1 * u3[2] + bv1 * v3[2] + off[2]]
        wb.append((min(xs), max(xs), by0 + off[1], by1 + off[1], min(zs), max(zs)))
    return span_mask(proj, wb)


# ══════════════════════════════════ 图像算子 ═══════════════════════════════════
def diff_mask(a: bytes, b: bytes, nch: int):
    """逐位不同掩码（任一通道不等）+ 逐像素最大通道差。与 `anyChannelDiff` 同口径。"""
    n = len(a) // nch
    m = bytearray(n)
    mx = bytearray(n)
    for i in range(n):
        o = i * nch
        d0 = a[o] - b[o]
        if d0 < 0:
            d0 = -d0
        d1 = a[o + 1] - b[o + 1]
        if d1 < 0:
            d1 = -d1
        d2 = a[o + 2] - b[o + 2]
        if d2 < 0:
            d2 = -d2
        d = d0 if d0 >= d1 else d1
        if d2 > d:
            d = d2
        mx[i] = d
        m[i] = 1 if d else 0
    return m, mx


def dilate(mask, w, h, r):
    """矩形结构元膨胀（先横后纵两趟，O(w·h·r)；r=0 → 原样）。"""
    tmp = bytearray(w * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            if mask[row + x]:
                for xx in range(max(0, x - r), min(w, x + r + 1)):
                    tmp[row + xx] = 1
    out = bytearray(w * h)
    for x in range(w):
        col = [tmp[y * w + x] for y in range(h)]
        for y in range(h):
            if col[y]:
                for yy in range(max(0, y - r), min(h, y + r + 1)):
                    out[yy * w + x] = 1
    return out


def erode_square(mask, w, h, r):
    """方形结构元腐蚀（积分图求「窗口内未置位的个数」，O(w·h)）。"""
    integ = [0] * ((h + 1) * (w + 1))
    for y in range(h):
        row_sum = 0
        base, nbase = y * w, (y + 1) * (w + 1)
        pbase = y * (w + 1)
        for x in range(w):
            row_sum += 0 if mask[base + x] else 1
            integ[nbase + x + 1] = integ[pbase + x + 1] + row_sum
    out = bytearray(w * h)
    for y in range(h):
        y0, y1 = max(0, y - r), min(h, y + r + 1)
        base = y * w
        for x in range(w):
            if not mask[base + x]:
                continue
            x0, x1 = max(0, x - r), min(w, x + r + 1)
            n = integ[y1 * (w + 1) + x1] - integ[y0 * (w + 1) + x1] \
                - integ[y1 * (w + 1) + x0] + integ[y0 * (w + 1) + x0]
            if n == 0:
                out[base + x] = 1
    return out


def measure_displacement(ref, arm, inner, rad=8, stride=4):
    """在 `inner`（已按 rad 内缩）区域内求「arm ≈ ref 平移多少」——同一批像素、整数位移、
    抛物线亚像素细化。返回 (dx, dy, 最佳处 MAD, 零位移 MAD)；样本不足返回 None。

    🔴 为什么必须先内缩：不内缩时，位移会把**求值集本身**平移，而天空/场景轮廓处的差异最大，
    大位移靠「把轮廓像素滚出求值集」就能假降 MAD——第一版就是这么读出「整张位移场贴在 ±rad
    边界上」的假结论。内缩保证每个候选位移比较的是同一批像素。
    """
    pts = [i for i in range(len(inner)) if inner[i] and i % WIDTH % stride == 0
           and (i // WIDTH) % stride == 0
           # 内缩只保证不越**掩码**边界；图像边界要另外让出 rad（否则位移后取到画幅外）
           and rad <= i % WIDTH < WIDTH - rad and rad <= i // WIDTH < HEIGHT - rad]
    if len(pts) < 2000:
        return None
    surf = {}
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            s = 0
            for i in pts:
                x, y = i % WIDTH, i // WIDTH
                j = (y + dy) * WIDTH + (x + dx)
                o, q = i * 3, j * 3
                d = abs(ref[o] - arm[q])
                d1 = abs(ref[o + 1] - arm[q + 1])
                d2 = abs(ref[o + 2] - arm[q + 2])
                if d1 > d:
                    d = d1
                if d2 > d:
                    d = d2
                s += d
            surf[(dx, dy)] = s / len(pts)

    def f(k):
        return surf.get(k, float("inf"))

    (bx, by), bv = min(surf.items(), key=lambda kv: kv[1])
    if bv == 0.0:  # 完全重合：细化没有意义（MAD 曲面在谷底是平台，抛物线会给出无意义的斜率）
        return bx, by, bv, surf[(0, 0)]
    ex = ey = 0.0
    den = f((bx - 1, by)) - 2 * bv + f((bx + 1, by))
    if den:
        ex = 0.5 * (f((bx - 1, by)) - f((bx + 1, by))) / den
    den = f((bx, by - 1)) - 2 * bv + f((bx, by + 1))
    if den:
        ey = 0.5 * (f((bx, by - 1)) - f((bx, by + 1))) / den
    # 抛物线细化在「曲面是平的」（= 没有任何位移能解释差异）时会给出 NaN/±∞；
    # 那不是读数，是「刚性位移模型不适用」的信号，退回整数峰并让调用方看降幅。
    if not (-0.5 <= ex <= 0.5) or ex != ex:
        ex = 0.0
    if not (-0.5 <= ey <= 0.5) or ey != ey:
        ey = 0.0
    return bx + ex, by + ey, bv, surf[(0, 0)]


def _f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


def fps_pose_displacement(man_base, man_far):
    """闭式预测：出厂 fps 机位在 1e6 处被 float32 收窄后，画面要平移多少像素。

    代码事实（rgl/camera_config_impl.cppm::readCameraPlacement，configs/camera.lua 出厂
    `mode = "fps"`）：朝向由
        glm::vec3 fwd = glm::vec3(camera.target) - glm::vec3(camera.position);
        pitch = asin(fwd.y/|fwd|);  yaw = atan2(-fwd.x, -fwd.z);
    反推，而 fps 分支的 view 只吃 yaw/pitch ⟹ **有效视向 = 绝对机位各分量先过 float32
    之后**再算出来的那条。|机位| = 1e6 时 ULP = 0.0625 m，而 eye/target 的 x 差只有 3.4 m
    ⟹ 视向被改掉 ~1 %。收窄发生在配置读取期（渲染原点还不存在）⟹ 门关/门开**两态同样中招**。

    返回 (视向夹角换算的像素数, disp)；`disp(ndc_x, ndc_y)` 给同一世界方向在两套相机下的
    落点差（米制像素，正 = 收窄后落点更靠右下）。
    """
    fov = float(man_base["measure"]["fovDegrees"])

    def basis(cam, narrow):
        e = [float(x) for x in cam["position"]]
        c = [float(x) for x in cam["target"]]
        if narrow:
            e = [_f32(x) for x in e]
            c = [_f32(x) for x in c]
        f = _norm([c[k] - e[k] for k in range(3)])
        r = _norm(_cross(f, [0.0, 1.0, 0.0]))
        return (f, r, _cross(r, f))

    th = math.tan(math.radians(fov) / 2.0)
    asp = WIDTH / HEIGHT
    b0 = basis(man_base["measure"]["camera"], False)
    b1 = basis(man_far["measure"]["camera"], True)
    cosang = max(-1.0, min(1.0, _dot(b0[0], b1[0])))
    ang_px = math.degrees(math.acos(cosang)) / fov * HEIGHT

    def px_of(w, bb):
        zc = _dot(w, bb[0])
        xd = _dot(w, bb[1]) / (th * asp) / zc
        yd = _dot(w, bb[2]) / th / zc
        return ((xd * 0.5 + 0.5) * WIDTH, (0.5 - yd * 0.5) * HEIGHT)

    def disp(ndc_x, ndc_y):
        w = _norm([b0[0][k] + ndc_x * th * asp * b0[1][k] + ndc_y * th * b0[2][k] for k in range(3)])
        p0, p1 = px_of(w, b0), px_of(w, b1)
        return p1[0] - p0[0], p1[1] - p0[1]

    return ang_px, disp


def scan_direction_mm_per_px(man, plate):
    """该板扫描线的像素方向 + 每像素沿扫描向的毫米当量（闭式，manifest 给的真值坐标）。"""
    proj = Projector(man["measure"]["camera"], man["measure"]["fovDegrees"])
    a = proj.point3([plate["scanFromX"], 0.0, plate["scanFromZ"]])
    b = proj.point3([plate["scanToX"], 0.0, plate["scanToZ"]])
    e = proj.point3([plate["trueEdgeX"], 0.0, plate["trueEdgeZ"]])
    if not (a and b and e):
        return None
    dx, dy = b[0] - a[0], b[1] - a[1]
    plen = math.hypot(dx, dy)
    if plen < 1e-9:
        return None
    mlen = math.hypot(plate["scanToX"] - plate["scanFromX"], plate["scanToZ"] - plate["scanFromZ"])
    return (dx / plen, dy / plen, 1000.0 * mlen / plen, e)


def field_removal_residual(ref, arm, sky, disp):
    """把**闭式预测的位移场**从 arm 里减掉后，与 ref 在天穹区的平均通道差（双线性重采样）。

    这是机制主张的决定性互证：如果 arm 与 ref 的差异**就是**那条视向偏转，减掉预测场之后
    残差应当塌到「同一次渲染的两臂之差」那个量级；减不动 = 还有第二个来源，机制只算部分解释。
    返回 (原始 MAD, 减场后 MAD)。
    """
    n = WIDTH * HEIGHT
    idx = [i for i in range(n) if sky[i]]
    raw = res = 0.0
    m = 0
    for i in idx:
        x, y = i % WIDTH, i // WIDTH
        dx, dy = disp((x + 0.5) / WIDTH * 2 - 1, 1 - (y + 0.5) / HEIGHT * 2)
        sx, sy = x + dx, y + dy
        x0, y0 = int(math.floor(sx)), int(math.floor(sy))
        if x0 < 0 or y0 < 0 or x0 + 1 >= WIDTH or y0 + 1 >= HEIGHT:
            continue
        tx, ty = sx - x0, sy - y0
        w00, w01 = (1 - tx) * (1 - ty), tx * (1 - ty)
        w10, w11 = (1 - tx) * ty, tx * ty
        o = i * 3
        a00 = (y0 * WIDTH + x0) * 3
        a10 = ((y0 + 1) * WIDTH + x0) * 3
        dr = da = 0
        for c in range(3):
            v = (w00 * arm[a00 + c] + w01 * arm[a00 + 3 + c]
                 + w10 * arm[a10 + c] + w11 * arm[a10 + 3 + c])
            d0 = abs(ref[o + c] - arm[o + c])
            d1 = abs(ref[o + c] - v)
            if d1 > dr:
                dr = d1
            if d0 > da:
                da = d0
        raw += da
        res += dr
        m += 1
    return raw / max(m, 1), res / max(m, 1)


def silhouette_rows(px, nch, sky, win=12):
    """每列的「天空→场景」轮廓行的**图像直读**（闭式边界 ±win 内最强垂直梯度 + 抛物线细化）。

    🔴 只用于**格间相对**读数：直读值本身带一个仪器口径的系统量（梯度峰未必正好落在抗锯齿
    覆盖的那条线上），把它当成「对闭式真值的偏离」会读出凭空 ~10 px 的假偏置；而四格共用同一
    个估计器，**两两相减**把这个系统量抵消掉，剩下的才是取景位移。闭式边界在这里只当搜索窗
    的中心用，不参与读数。
    """
    n = WIDTH * HEIGHT
    L = luma(px, nch, n)
    out = {}
    for x in range(WIDTH):
        col = [y for y in range(HEIGHT) if sky[y * WIDTH + x]]
        if not col:
            continue
        b = col[-1] + 0.5
        if b < win + 2 or b > HEIGHT - win - 3:
            continue
        best, brow = -1.0, None
        for y in range(int(b) - win, int(b) + win):
            g = abs(L[(y + 1) * WIDTH + x] - L[y * WIDTH + x])
            if g > best:
                best, brow = g, y
        if brow is None or brow <= int(b) - win or brow >= int(b) + win - 1:
            continue
        a = abs(L[brow * WIDTH + x] - L[(brow - 1) * WIDTH + x])
        c = abs(L[(brow + 2) * WIDTH + x] - L[(brow + 1) * WIDTH + x])
        den = a - 2 * best + c
        d = 0.5 * (a - c) / den if den else 0.0
        if abs(d) > 1.0:
            d = 0.0
        out[x] = brow + 0.5 + d
    return out


def luma(px, nch, n):
    out = [0.0] * n
    for i in range(n):
        o = i * nch
        out[i] = 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2]
    return out


def sky_gradient_energy(px, nch, sky, n):
    """天穹区的梯度能量（配准无关的锐度）。只统计右邻与下邻都在天穹内的像素。"""
    gx = gy = 0.0
    cnt = 0
    L = luma(px, nch, n)
    for y in range(HEIGHT - 1):
        base = y * WIDTH
        for x in range(WIDTH - 1):
            i = base + x
            if sky[i] and sky[i + 1] and sky[i + WIDTH]:
                gx += abs(L[i + 1] - L[i])
                gy += abs(L[i + WIDTH] - L[i])
                cnt += 1
    return (gx / cnt if cnt else 0.0), (gy / cnt if cnt else 0.0), cnt


# ══════════════════════════════════ 主流程 ═════════════════════════════════════
def run(root: str) -> int:
    models = os.path.join(root, "models")
    shots = os.path.join(root, "screenshots")
    absent = [os.path.join(shots, f"imgqa_shadow_edge_rebase_{sw}_{stem}_{arm}.png")
              for (_t, stem, sw) in CELLS for arm in ARMS
              if not os.path.exists(os.path.join(shots, f"imgqa_shadow_edge_rebase_{sw}_{stem}_{arm}.png"))]
    if absent:
        print("🔴 缺截图（先跑 `just imgqa shadow-edge-rebase` 采集；本脚本不启引擎）：")
        for p in absent:
            print("   ", p)
        return 2

    print("shadow-edge-regions —— 把「逐位不同像素」按内容类别拆开（只读诊断，不改任何界）")
    print(f"  输入 = {shots}/imgqa_shadow_edge_rebase_*.png（{WIDTH}×{HEIGHT}）+ "
          f"{models}/{'、'.join(s + '.manifest.json' for s in (STEM_BASE, STEM_FAR))}")

    npx = WIDTH * HEIGHT
    tot = float(npx)

    def pct(x):
        return 100.0 * x / tot

    # ── 闭式几何 + 掩码 ───────────────────────────────────────────────────────
    mans, masks = {}, {}
    for stem in (STEM_BASE, STEM_FAR):
        mans[stem] = json.load(open(os.path.join(models, stem + ".manifest.json")))
        err = aabb_roundtrip_error(mans[stem])
        if err > 1e-5:  # 容差理由见 ground_rect：aabb 是 float32 落盘量，别拿 double 的 0 去要
            print(f"🔴 {stem}: 长方体重建与 manifest aabb 对不上（最大 {err:.3e} m）——掩码不可信")
            return 2
        proj = Projector(mans[stem]["measure"]["camera"], mans[stem]["measure"]["fovDegrees"])
        masks[stem] = scene_mask(mans[stem], proj)
        n_sc = masks[stem].count(1)
        print(f"  {stem}: 9 个 piece 闭式重建与 manifest aabb 逐位吻合（最大偏差 {err:.1e} m）；"
              f"场景 {pct(n_sc):.4f}% / 天穹 {pct(npx - n_sc):.4f}%"
              f"   （直接拿 aabb 当盒子会读成 {pct(npx * aabb_mask_area_fraction(mans[stem], proj)):.2f}% 场景）")
    same = masks[STEM_BASE] == masks[STEM_FAR]
    print(f"  两靶场景掩码{'逐位相同 ✓（纯平移下取景不变 = 闭式侧证据）' if same else '不同 🔴'}")

    # ── 读图 ──────────────────────────────────────────────────────────────────
    imgs = {}
    for (tag, stem, sw) in CELLS:
        for arm in ARMS:
            p = os.path.join(shots, f"imgqa_shadow_edge_rebase_{sw}_{stem}_{arm}.png")
            rows, cols, nch, px = png_read_rgb(p)
            if (rows, cols) != (HEIGHT, WIDTH):
                print(f"🔴 {p} 尺寸 {cols}×{rows} ≠ {WIDTH}×{HEIGHT}")
                return 2
            imgs[(tag, arm)] = (nch, px)
    nch = 3  # png_read_rgb 已把 RGBA 收成 RGB（与 imgqa 的 IMREAD_COLOR 同口径）

    def K(tag, arm):
        return imgs[(tag, arm)][1]

    # ── 阴影项（格内 none↔d5）─────────────────────────────────────────────────
    print("\n  ── 阴影项（格内 none↔d5 逐位不同 = 阴影对交付图的贡献；分母 = 整幅）──")
    shadow = {}
    for (tag, stem, _sw) in CELLS:
        m, mx = diff_mask(K(tag, "none"), K(tag, "d5"), nch)
        shadow[tag] = (m, mx)
        sc = masks[stem]
        n = m.count(1)
        n_sky = sum(1 for i in range(npx) if m[i] and not sc[i])
        print(f"   {tag}: 阴影像素 {n} = {pct(n):.4f}%   最大通道差 {max(mx)}   "
              f"落在闭式天穹区 {n_sky}")

    # ── Q1 分解 ───────────────────────────────────────────────────────────────
    print("\n  ── Q1：③ vs ①（门关，纯平移激励）逐位不同的内容分解 ──")
    D, DMX = diff_mask(K("③远点·门关", "d5"), K("①出厂·门关", "d5"), nch)
    N, NMX = diff_mask(K("③远点·门关", "none"), K("①出厂·门关", "none"), nch)
    nD, nN = D.count(1), N.count(1)
    only_d = sum(1 for i in range(npx) if D[i] and not N[i])
    only_n = sum(1 for i in range(npx) if N[i] and not D[i])
    print(f"   总逐位不同             = {nD} = {pct(nD):.4f}%   ← 仪器报的那个数")
    print(f"   阴影项整个拿掉后仍不同 = {nN} = {pct(nN):.4f}%   ← 与阴影**无关**的部分（none↔none）")
    print(f"   只在阴影项开着才不同   = {only_d} = {pct(only_d):.4f}%   ← 阴影项贡献")
    print(f"   阴影项关掉反而不同     = {only_n}（阴影与其余项在个别像素上互相抵消，不是新类别）")

    sc1, sc3 = masks[STEM_BASE], masks[STEM_FAR]
    sky = bytearray(1 if (not sc1[i] and not sc3[i]) else 0 for i in range(npx))
    sh3, sh1 = shadow["③远点·门关"][0], shadow["①出厂·门关"][0]
    shu = bytearray(1 if (sh3[i] or sh1[i]) else 0 for i in range(npx))
    shd = dilate(shu, WIDTH, HEIGHT, 2)
    # 三类互斥：阴影项（膨胀后）优先，其余按闭式天穹 / 场景二分
    in_sh = shd
    in_sky = bytearray(1 if (sky[i] and not in_sh[i]) else 0 for i in range(npx))
    in_scn = bytearray(1 if (not sky[i] and not in_sh[i]) else 0 for i in range(npx))
    print("   ── 按内容类别拆开（三类互斥；优先级：阴影项 → 天穹 → 场景非阴影。"
          "阴影掩码做 2px 膨胀：几何轮廓的抗锯齿像素不该算进天穹）──")
    tot_c = 0
    for nm, msk in (("阴影项（csm 与 none 之差非零，含 2px 膨胀）", in_sh),
                    ("天穹（闭式非场景，阴影掩码外）", in_sky),
                    ("场景非阴影（地面 + 板，阴影掩码外）", in_scn)):
        a = msk.count(1)
        c = sum(1 for i in range(npx) if D[i] and msk[i])
        tot_c += c
        print(f"     {nm:<44} 面积 {a:>7} = {pct(a):6.3f}%   其中逐位不同 {c:>7} = "
              f"{pct(c):6.3f}pp   命中率 {100.0 * c / max(a, 1):6.2f}%")
    print(f"     {'三类合计':<44} {'':>22}   其中逐位不同 {tot_c:>7} = {pct(tot_c):.3f}pp"
          f"（总数 {nD}，差 {nD - tot_c}）")
    print("   ── 同一口径再给门开态（④ vs ②）作对照 ──")
    D42, DMX42 = diff_mask(K("④远点·门开", "d5"), K("②出厂·门开", "d5"), nch)
    nD42 = D42.count(1)
    print(f"     总逐位不同 = {nD42} = {pct(nD42):.4f}%")
    for nm, msk in (("阴影项", in_sh), ("天穹", in_sky), ("场景非阴影", in_scn)):
        a = msk.count(1)
        c = sum(1 for i in range(npx) if D42[i] and msk[i])
        print(f"     {nm:<12} 其中逐位不同 {c:>7} = {pct(c):6.3f}pp   命中率 {100.0 * c / max(a, 1):6.2f}%")

    # ── Q2 天穹 ───────────────────────────────────────────────────────────────
    print("\n  ── Q2：天穹在 1e6 坏没坏、门开修不修（区域 = 闭式非场景）──")
    idx = [i for i in range(npx) if sky[i]]
    nsky = len(idx)
    for nm, dm, mx in (("门关 ③vs①", D, DMX), ("门开 ④vs②", D42, DMX42)):
        nd = sum(dm[i] for i in idx)
        s = sum(mx[i] for i in idx)
        big = sum(1 for i in idx if mx[i] > 32)
        print(f"   {nm}: 逐位不同 {nd}/{nsky} = {100.0 * nd / nsky:.3f}%   平均通道差 {s / nsky:.3f}   "
              f">32 LSB {100.0 * big / nsky:.3f}%   最大 {max(mx[i] for i in idx)}")
    print("   ── 配准无关的锐度（天穹区梯度能量；糊了就是低，不需要跟任何一张图比）──")
    for (tag, _stem, _sw) in CELLS:
        gx, gy, cnt = sky_gradient_energy(K(tag, "d5"), nch, sky, npx)
        print(f"     {tag}: 水平 {gx:.4f}   垂直 {gy:.4f}   样本 {cnt}")
    print("   ── 场景区同口径（对照：是不是只有天穹在动）──")
    sc_any = bytearray(1 - v for v in sky)
    for nm, dm, mx in (("门关 ③vs①", D, DMX), ("门开 ④vs②", D42, DMX42)):
        j = [i for i in range(npx) if sc_any[i]]
        nd = sum(dm[i] for i in j)
        print(f"     {nm}: 场景逐位不同 {100.0 * nd / len(j):.3f}%   平均通道差 "
              f"{sum(mx[i] for i in j) / len(j):.3f}   >32 LSB {100.0 * sum(1 for i in j if mx[i] > 32) / len(j):.3f}%")

    # ── Q3 ────────────────────────────────────────────────────────────────────
    print("\n  ── Q3：阴影项自己的差分有没有漏进天穹 / 被取景污染 ──")
    sc_dil = dilate(sc1, WIDTH, HEIGHT, 3)
    for (tag, stem, _sw) in CELLS:
        m = shadow[tag][0]
        sc = masks[stem]
        in_sky = sum(1 for i in range(npx) if m[i] and not sc[i])
        near = sum(1 for i in range(npx) if m[i] and not sc[i] and sc_dil[i])
        print(f"   {tag}: 阴影像素落在天穹 = {in_sky}（其中紧贴场景轮廓 3px 内 = {near}，"
              f"轮廓外 = {in_sky - near}）")
    print("   阴影项是**格内**两臂之差：同一相机、同一几何、同一取景 ⟹ 结构上不含跨格的取景差；"
          "上面是在核它有没有漏进天穹。")

    # ── Q4 取景：远点的画面到底被平移了多少（闭式预测 vs 实测互证）────────────────
    print("\n  ── Q4：远点取景位移（影边毫米读数的公共分母；闭式预测与实测互证）──")
    ang_px, disp = fps_pose_displacement(mans[STEM_BASE], mans[STEM_FAR])
    fov = float(mans[STEM_BASE]["measure"]["fovDegrees"])
    print(f"   闭式预测：出厂 fps 机位过 float32 后有效视向偏 {ang_px:.2f} px"
          f"（= {ang_px * fov / HEIGHT:.4f}°）。同一世界方向的落点差：")
    for (a, b, lbl) in ((0.0, 0.0, "中心"), (-0.8, 0.8, "左上"), (0.8, 0.8, "右上"),
                        (-0.8, -0.8, "左下"), (0.8, -0.8, "右下")):
        ddx, ddy = disp(a, b)
        print(f"      {lbl} NDC ({a:+.1f},{b:+.1f}) → dx {ddx:+6.2f} px  dy {ddy:+6.2f} px")

    inner = erode_square(sky, WIDTH, HEIGHT, 9)
    print(f"   实测（天空区内缩 9 px 后做整数位移互相关 + 抛物线亚像素；样本 {sum(inner)}）：")
    for nm, (ta, tb) in (("④vs② 门开", ("④远点·门开", "②出厂·门开")),
                         ("③vs① 门关", ("③远点·门关", "①出厂·门关")),
                         ("②vs① 出厂点门开 vs 门关", ("②出厂·门开", "①出厂·门关"))):
        r = measure_displacement(K(tb, "d5"), K(ta, "d5"), inner)
        if r is None:
            print(f"      {nm}: 样本不足")
            continue
        dx, dy, v, v0 = r
        print(f"      {nm}: 最佳位移 ({dx:+.2f},{dy:+.2f}) px   该处 MAD {v:.3f}   "
              f"零位移 MAD {v0:.3f}   位移解释掉 {100.0 * (1 - v / v0):5.1f}%")

    print("   ── 几何直读第三路互证：天空/场景轮廓行的格间差（闭式边界只当搜索窗中心，不参与读数）──")
    sil = {tag: silhouette_rows(K(tag, "d5"), 3, sky) for (tag, _s, _w) in CELLS}
    for nm, (ta, tb) in (("④−② 门开", ("④远点·门开", "②出厂·门开")),
                         ("③−① 门关", ("③远点·门关", "①出厂·门关")),
                         ("②−① 出厂点两态（对照，应为 0）", ("②出厂·门开", "①出厂·门关"))):
        cols = sorted(set(sil[ta]) & set(sil[tb]))
        d = sorted(sil[ta][c] - sil[tb][c] for c in cols)
        med = d[len(d) // 2]
        print(f"      {nm}: 中位 {med:+6.2f} px   p25/p75 {d[len(d)//4]:+.2f}/{d[len(d)*3//4]:+.2f}   列数 {len(cols)}")
    print("   ── 决定性互证：把**闭式预测的位移场**整张减掉，看天空残差塌不塌 ──")
    for nm, (ta, tb) in (("④vs② 门开", ("④远点·门开", "②出厂·门开")),
                         ("③vs① 门关", ("③远点·门关", "①出厂·门关")),
                         ("②vs① 出厂点两态（对照）", ("②出厂·门开", "①出厂·门关"))):
        r0, r1 = field_removal_residual(K(tb, "d5"), K(ta, "d5"), sky, disp)
        note = '（对照：出厂点本来就没有位移，减掉一张非零的场**只会变差**——这条是算子的反向用例）' if r0 < 1.0 else ''
        print(f"      {nm}: 原始 MAD {r0:6.3f} → 减场后 {r1:6.3f}"
              + (f"   解释掉 {100.0 * (1 - r1 / r0):5.1f}%" if not note else "   " + note))
    print("   ── 该取景位移投到逐板扫描线上的毫米当量（= 影边世界位置读数的偏置，闭式预测）──")
    for p in mans[STEM_BASE]["measure"]["plates"]:
        sd = scan_direction_mm_per_px(mans[STEM_BASE], p)
        if sd is None:
            print(f"     {p['name']:<12} 扫描窗投出画幅，跳过")
            continue
        ux, uy, mmpp, e = sd
        ddx, ddy = disp(e[0] / WIDTH * 2 - 1, 1 - e[1] / HEIGHT * 2)
        print(f"     {p['name']:<12} 位移 ({ddx:+5.2f},{ddy:+5.2f}) px × 每像素 {mmpp:5.2f} mm"
              f" → 沿扫描向偏置 {(ddx * ux + ddy * uy) * mmpp:+7.2f} mm")
    return 0


# ══════════════════════════════════ 自检（闭式输入）═════════════════════════════
def selftest() -> int:
    """判据工具自己要有自检：每条都用**闭式已知**的输入，不依赖引擎、不依赖 GPU。"""
    print("selftest —— 闭式输入自检（不读引擎产物）")
    fails = []

    def ck(name, cond, extra=""):
        print(f"  [{'ok ' if cond else 'FAIL'}] {name}{('  ' + extra) if extra else ''}")
        if not cond:
            fails.append(name)

    # ① PNG 编解码往返 × 5 种滤波（RGBA → RGB，alpha 丢弃口径同 imgqa）
    import tempfile
    rows, cols = 5, 7
    src = bytes((i * 37 + 11) % 256 for i in range(rows * cols * 4))
    want = b"".join(src[i * 4:i * 4 + 3] for i in range(rows * cols))
    with tempfile.TemporaryDirectory() as td:
        for ft in range(5):
            p = os.path.join(td, f"t{ft}.png")
            png_write_rgba_filtered(p, rows, cols, src, ft)
            r2, c2, nch2, px2 = png_read_rgb(p)
            ck(f"PNG 编解码往返（滤波 {ft}）", (r2, c2, nch2) == (rows, cols, 3) and px2 == want)

    # ② 逐位不同掩码：闭式两图，已知哪 4 个像素该红、最大差 = 1
    a = bytes([0] * 12 + [255] * 12)
    b = bytes([0] * 12 + [254] * 12)
    m, mx = diff_mask(a, b, 3)
    ck("diff_mask 只数不等的那 4 个像素、最大差 = 1", sum(m) == 4 and max(mx) == 1)

    # ③ 反向：同一张图 ⟹ 掩码恰为 0（恒真/恒假掩码 = 分类全瞎的两种形态）
    ck("diff_mask 反向用例（同图 → 恰 0）", sum(diff_mask(a, a, 3)[0]) == 0)

    # ④ 膨胀：单点 → (2r+1)² 方块；且不越界
    w = h = 9
    d0 = bytearray(w * h)
    d0[4 * w + 4] = 1
    ck("dilate r=1 把单点变成 3×3", sum(dilate(d0, w, h, 1)) == 9)
    d1 = bytearray(w * h)
    d1[0] = 1
    ck("dilate 在角上不越界（r=1 → 4 像素）", sum(dilate(d1, w, h, 1)) == 4)

    # ⑤ 投影闭式：光轴点落正中心；上边缘落第 0 行；相机背后 = None
    pr = Projector({"position": [0.0, 0.0, 0.0], "target": [0.0, 0.0, 1.0], "fovDegrees": 45.0},
                   45.0, 16, 16)
    q = pr.point3([0.0, 0.0, 10.0])
    ck("Projector 光轴点 → 画幅正中心", q is not None and abs(q[0] - 8.0) < 1e-9 and abs(q[1] - 8.0) < 1e-9, str(q))
    th = math.tan(math.radians(45.0) / 2.0)
    q2 = pr.point3([0.0, 10.0 * th, 10.0])
    ck("Projector 视野上边缘 → py=0，相机背后 → None",
       q2 is not None and abs(q2[1]) < 1e-9 and pr.point3([0.0, 0.0, -1.0]) is None, str(q2))

    # ⑥ 掩码闭式：相机高 1、水平看向 +z、y=0 的「无限大」地面 ⟹ 地平线**恰好**是画幅
    #    水平中线（零滚相机的针孔投影性质）⟹ 掩码必须精确等于下半幅，一行不多一行不少。
    #    这一条同时打三处：视锥裁剪（近侧被裁的地面不能漏）、扫描行填充的像素区间口径
    #    （[i, i+1) 还是中心）、以及「相机背后的面不能投进画里」。
    man = {"measure": {"u": [1.0, 0.0], "v": [0.0, 1.0], "H": 1.0, "plateLengthM": 1.0,
                       "worldOffset": [0.0, 0.0, 0.0], "fovDegrees": 45.0,
                       "camera": {"position": [0.0, 1.0, 0.0], "target": [0.0, 1.0, 10.0]},
                       "plates": []},
           "pieces": [{"name": "ground", "aabb": {"min": [-1000.0, 0.0, -1000.0],
                                                  "max": [1000.0, 0.0, 1000.0]}}]}
    pr6 = Projector(man["measure"]["camera"], 45.0, 100, 100)
    mk = scene_mask(man, pr6)
    want = bytearray(1 if (i // 100) >= 50 else 0 for i in range(100 * 100))
    ck("scene_mask 闭式地平线（下半幅恰满、上半幅恰空）", bytes(mk) == bytes(want),
       f"实得 {mk.count(1)} / 闭式 5000")

    # ⑦ 反向用例：盒整个在相机背后 ⟹ 掩码必须恰为 0（恒真掩码 = 分类全瞎的形态）
    man7 = json.loads(json.dumps(man))
    man7["measure"]["u"] = [0.0, 1.0]
    man7["measure"]["v"] = [1.0, 0.0]
    man7["pieces"] = [{"name": "plate_x", "aabb": {"min": [-0.5, 0.0, -50.5],
                                                   "max": [0.5, 1.0, -49.5]}}]
    man7["measure"]["plates"] = [{"name": "plate_x", "uCenter": -50.0, "thicknessM": 1.0}]
    ck("scene_mask 反向用例（盒在相机背后 → 恰 0）", scene_mask(man7, pr6).count(1) == 0)

    # ⑧ 反向用例：盒在画幅外（侧向 50 m、纵深 ±0.5 m）⟹ 掩码恰为 0（不能把屏外读成边缘）
    man8 = json.loads(json.dumps(man7))
    man8["measure"]["u"] = [1.0, 0.0]
    man8["measure"]["v"] = [0.0, 1.0]
    man8["pieces"] = [{"name": "plate_x", "aabb": {"min": [49.5, 0.0, -0.5],
                                                   "max": [50.5, 1.0, 0.5]}}]
    man8["measure"]["plates"] = [{"name": "plate_x", "uCenter": 50.0, "thicknessM": 1.0}]
    ck("scene_mask 反向用例（盒在画幅外 → 恰 0）", scene_mask(man8, pr6).count(1) == 0)

    # ⑨ 地面矩形反解：造一个转了 42° 的闭式矩形，反解必须回到原始 (u0,u1,vh)
    ang = math.radians(42.0)
    uu = (math.cos(ang), math.sin(ang))
    vv = (-math.sin(ang), math.cos(ang))
    u0, u1, vh = -0.5, 8.9, 0.74
    cs = [(u * uu[0] + v * vv[0], u * uu[1] + v * vv[1]) for u in (u0, u1) for v in (-vh, vh)]
    man8 = {"measure": {"u": list(uu), "v": list(vv), "worldOffset": [0.0, 0.0, 0.0]},
            "pieces": [{"name": "ground",
                        "aabb": {"min": [min(c[0] for c in cs), 0.0, min(c[1] for c in cs)],
                                 "max": [max(c[0] for c in cs), 0.0, max(c[1] for c in cs)]}}]}
    g0, g1, vh8 = ground_rect(man8)
    ck("ground_rect 反解（转 42° 的闭式矩形）",
       max(abs(g0 - u0), abs(g1 - u1), abs(vh8 - vh)) < 1e-12,
       f"({g0:.6f},{g1:.6f},{vh8:.6f}) vs ({u0},{u1},{vh})")

    # ⑩ 反解失败必须出声：把 aabb 改胖 1 mm（模拟「直接拿 aabb 当盒子」）→ 校验应抛
    bad = json.loads(json.dumps(man8))
    bad["pieces"][0]["aabb"]["max"][0] += 1e-3
    try:
        ground_rect(bad)
        ck("aabb 被改胖 → ground_rect 抛", False, "没抛 = 校验形同虚设")
    except ValueError:
        ck("aabb 被改胖 → ground_rect 抛", True)

    # ⑪⑫ 位移算子：闭式已知位移必须被读回（小画幅，纯合成输入，不碰引擎）
    global WIDTH, HEIGHT
    w0, h0 = WIDTH, HEIGHT
    WIDTH, HEIGHT = 80, 60
    try:
        n = WIDTH * HEIGHT
        pat = [(i * 7919 + (i // WIDTH) * 131) % 251 for i in range(n)]  # 高频、非周期
        ref = bytearray()
        for i in range(n):
            ref += bytes((pat[i], pat[(i * 3) % n], pat[(i * 5) % n]))

        def shifted(src, dx, dy):
            out = bytearray(len(src))
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    sx = min(max(x - dx, 0), WIDTH - 1)
                    sy = min(max(y - dy, 0), HEIGHT - 1)
                    o, q = (y * WIDTH + x) * 3, (sy * WIDTH + sx) * 3
                    out[o:o + 3] = src[q:q + 3]
            return bytes(out)

        arm = shifted(ref, 3, -2)
        inner = bytearray(1 if (3 <= x < WIDTH - 3 and 3 <= y < HEIGHT - 3) else 0
                          for y in range(HEIGHT) for x in range(WIDTH))
        r = measure_displacement(bytes(ref), arm, inner, rad=4, stride=1)
        ck("measure_displacement 读回闭式位移 (3,−2)",
           r is not None and abs(r[0] - 3) < 0.6 and abs(r[1] + 2) < 0.6, str(None if not r else r[:2]))
        r0 = measure_displacement(bytes(ref), bytes(ref), inner, rad=4, stride=1)
        ck("measure_displacement 反向用例（同图 → (0,0) 且 MAD 恰 0）",
           r0 is not None and r0[0] == 0 and r0[1] == 0 and r0[2] == 0.0, str(r0))
        raw, res = field_removal_residual(bytes(ref), arm, inner, lambda a, b: (3.0, -2.0))
        ck("field_removal_residual：常数场完全解释（残差 < 0.5 且远小于原始）",
           res < 0.5 < raw, f"原始 {raw:.2f} → 减场后 {res:.2f}")
        raw2, res2 = field_removal_residual(bytes(ref), arm, inner, lambda a, b: (0.0, 0.0))
        ck("field_removal_residual 反向用例（减错场 → 残差 ≈ 原始）",
           abs(res2 - raw2) < 1.0, f"原始 {raw2:.2f} → 减场后 {res2:.2f}")
    finally:
        WIDTH, HEIGHT = w0, h0

    print(f"自检通过：{len(fails)} 条失败")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="把逐位不同像素按内容类别拆开（只读诊断）")
    ap.add_argument("--root", default="build/rgl", help="构建产物根（默认 build/rgl）")
    ap.add_argument("--selftest", action="store_true", help="只跑闭式输入自检（不读引擎产物）")
    a = ap.parse_args()
    return selftest() if a.selftest else run(a.root)


if __name__ == "__main__":
    sys.exit(main())
