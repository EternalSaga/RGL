#!/usr/bin/env python3
# QA 合成场景生成器：为 qa/ 图像回归层（AGENTS.md 测试体系第二层，批 12.2+）程序化生产
# 「几何完全已知、无外部素材依赖、逐字节可复现」的 .glb + manifest。**stdlib-only**
# （struct/json/math/argparse/dataclasses/pathlib）——qa 场景是测试基准自身，基准不能
# 依赖第三方库的浮点实现/版本行为（numpy 不同版本的 sin 末位可差，逐字节复现即破）。
#
# ## 为什么需要这个文件（不是"示例模型导出"）
#
# bistro 这种真场景 2.8M 三角 / 395 纹理，图像一变只能答"变了"，答不了"变的是谁、该不该变"。
# commit 79fc778 把 picking objectID 通道扩成全分辨率 dump（r32_uint，值 = objectIndex+1），
# harness 于是能把"这块屏幕是谁"解码成精确整数——但 objectID = **运行时全局递增的 objectSlot
# 计数器**（跨模型全局，不是 per-model primitive 序号），离线文件无权断言它。所以分工是：
# 本文件产出「色块 ↔ 名字 ↔ 几何」的静态事实（每 piece 一个高饱和纯色 + manifest），
# harness 运行时渲染一次、从 object_id 通道反解「颜色 ↔ objectID」映射。
# **manifest 里没有 objectID 字段，这是设计而非遗漏。**
#
# ## piece = 独立 mesh（单 primitive）+ 独立 material + 独立 node
#
# 引擎加载器（rgl/gltf_loader_impl.cppm）把 mesh-major 展平所有 primitive，每 primitive
# 成为一个可拾取对象；本文件保持「1 piece = 1 mesh = 1 primitive = 1 material」，
# 则 manifest 里的 piece 序 = 展平后 globalPrimitiveIndex = node/mesh/material 下标，
# harness 拿名字反查任何一条都是同一个数。刻意**不**用「一个 mesh 多 primitive 靠材质区分」：
# 那会让"piece 序"多一层 mesh 内偏移，映射表没有收益只有出错面。
# piece 名同时写进 node.name / mesh.name / material.name（glTF primitive 无名机制）。
#
# ## 输出刻意不写 TEXCOORD_0 / TANGENT
#
# 加载器契约（gltf_loader_impl.cppm readVertexAttributes）：POSITION/NORMAL 必需，缺 = 整个
# 模型加载失败；TEXCOORD_0 可选（缺 → uv 填 0 走白纹理）、TANGENT 可选（缺 → Frisvad 解析
# 正交基兜底，纯色无 normal map 时切线基不参与任何可见计算）。QA 场景无纹理无 normal map，
# 两条都是纯负担——**不输出 = 少两条出错路径**（写错 stride 的机会都不给）。
#
# ## 确定性纪律（对齐 AGENTS.md「写测试的硬纪律」）
#
# 无真随机（连种子 LCG 都不需要——全部几何是解析式）；JSON 键序 = 插入序；float 只经
# struct "<f"（IEEE754 float32）落盘，min/max 从**已 round 成 float32 的字节**反读后计算，
# 保证 manifest/accessor min/max 与二进制数据精确相等（不是"约等于"）。
# 同一台机器上同一输入产出逐字节相同的 glb（自验收：连跑两次 md5sum 一致）。
#
# ## 自检内置（"不自检的生成器等于没测"）
#
# 每个 glb 先在内存里组装 → validate_glb_bytes() 全量校验（头/chunk 对齐/bufferView 与
# accessor 边界/POSITION min-max 与真实顶点**精确**相等/NORMAL 单位长/索引 < 顶点数/
# 绕序⇔顶点法线一致性/**朝外性**——星形件逐三角形 dot(几何法线,重心−中心)>0、
# 非星形封闭件（torus）缝合流形+方向一致+射线奇偶性种子（径向判据对 1 亏格曲面
# 不适用，管内侧 960/2304 本就该"朝中心"，2026-09-30 复盘分类）/primitive→material
# 引用存在且 baseColorFactor 与 manifest 精确相等/调色板最小两两距离与反灰跨度/
# shadow_rig 另有级联跨距自证与影包络覆盖断言）→ 全过才写盘。
# 任何一项失败 = 打错误列表、非零退出、坏文件不落盘。
#
# ## 三个场景的诚实定位
#
# - cornell_box：**只是 Cornell box 的几何**。真正的标志性特征是面光源 + 颜色溢出，
#   本引擎没有面光源（只有方向光 + 环境 cubemap，见 configs/scene.lua light 段），
#   所以它验证的是「几何正确性 + 双色墙 + 多材质 + AO 接触」，**不是**辐射度量学意义上的
#   Cornell box 基准，不要拿它跟经典 radiance 参考图对比。
# - shadow_rig：最重要。按 skill rgl-verify「阴影验收判据」硬要求 1-3 设计（阳性锚点 /
#   跨级联分布 / 远景取景），逐物体的失效模式在场景函数注释里。
# - d1_parity：forward / deferred 两路对拍（D1 指标）用的中等面数 + 材质混合 + 自发光面板。

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# 固定调色板（写死；禁止运行时生成颜色——运行时「色 → piece」反查依赖这张表与 glb
# 字节精确相等，运行时生成等于给反查表再引入一条随机性来源）。
#
# 选色判据（通道值只取 {0, 0.5, 1}，全表最小两两欧氏距离 = 0.5，内置自检会断言）：
#   1. 必须扛住整条后处理链：baseColor → 方向光 + IBL（env 以天光为主）→ SSAO → ACES
#      → gamma。ACES 对每通道单调，高饱和纯色位于色域顶点/棱中点，tonemap 压缩亮度
#      不改变色相维度上的分离度；
#   2. 避免灰 / 近黑 / 近白：灰的表观值完全由光照强度决定（AO 吃掉对比、曝光整体平移，
#      区分度随参数漂）；近黑 / 近白在 gamma 后向 0 / 255 塌缩，通道信息被裁掉；
#   3. 不用需要靠细微色差区分的相邻色（红 vs 橙红之类）：IBL 环境项逐通道加，
#      相邻色加完可能互换大小序。
# 运行时反查主要走 object_id 通道（整数、无损），颜色是人眼可读的冗余层 +
# object_id 附件缺席时的交叉校验，故最小距离 0.5 够用（同场景 10 piece 已是最优排布）。
# ---------------------------------------------------------------------------
PAL = {
    "red":       (1.0, 0.0, 0.0),
    "green":     (0.0, 1.0, 0.0),
    "blue":      (0.0, 0.0, 1.0),
    "yellow":    (1.0, 1.0, 0.0),
    "cyan":      (0.0, 1.0, 1.0),
    "magenta":   (1.0, 0.0, 1.0),
    "orange":    (1.0, 0.5, 0.0),
    "purple":    (0.5, 0.0, 1.0),
    "azure":     (0.0, 0.5, 1.0),
    "chartreuse": (0.5, 1.0, 0.0),
}
MIN_COLOR_DIST = 0.5  # 全调色板最小两两欧氏距离；同场景 piece 两两颜色取自此表

GAMMA_NOTE = (
    "baseColorFactor 是线性值；渲染链 = sun(方向光) + IBL(env) + SSAO + ACES tonemap + gamma。"
    "harness 反解颜色时对最终 sRGB 做最近邻匹配即可——纯色经此链后仍保持两两可分（选色判据），"
    "但匹配容差应基于实测渲染值而非这里的线性因子。"
)


# ---------------------------------------------------------------------------
# 几何基元：全部返回 (positions, normals, indices)，坐标为**世界坐标**（平移烘进顶点，
# node 恒 identity）。为什么烘：manifest 的 AABB = POSITION accessor 的 min/max =
# 二进制实测值，三者必须精确相等，任何经 node TRS 的间接层都让 harness 的交叉核对
# 多信一个引擎侧矩阵合成（loader 的 TRS 合成有环防御等分支，QA 基准没必要踩进去）。
# 绕序：三角形从法线一侧看为 CCW（引擎背面剔除生效，单面朝外）。
# ---------------------------------------------------------------------------

@dataclass
class Geom:
    positions: list          # [(x, y, z), ...] float（float32 可精确 round-trip）
    normals: list            # 等长单位向量
    indices: list            # len % 3 == 0


def box(cx: float, cy: float, cz: float, sx: float, sy: float, sz: float,
        yaw_deg: float = 0.0) -> Geom:
    """轴对齐盒（center = 几何中心，非底面心）；yaw_deg = 绕 +Y 旋转（右手，度）。

    24 顶点 / 12 三角形：面法线不共享（共享顶点 + 平均法线会让盒棱在光照上"圆"掉，
    QA 基准要的是硬棱——棱是几何错位最敏感的探针）。
    """
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    faces = [  # (normal, 4 顶点局部坐标)；每面绕序已核对 cross(e1,e2) ∥ normal
        ((1, 0, 0),  [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0, 1, 0),  [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
        ((0, 0, 1),  [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
    ]
    c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    pos, nrm, idx = [], [], []
    for n, verts in faces:
        base = len(pos)
        for (x, y, z) in verts:
            pos.append((cx + c * x + s * z, cy + y, cz - s * x + c * z))
            nrm.append((c * n[0] + s * n[2], n[1], -s * n[0] + c * n[2]))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return Geom(pos, nrm, idx)


def quad(p0, u, v) -> Geom:
    """平行四边形面片 p0 → p0+u → p0+u+v → p0+v；法线 = normalize(cross(u, v))。

    地板/墙/自发光面板的通用件。单面（背面剔除下从背面不可见）——Cornell 墙刻意只朝
    盒内，"墙没了"这类几何翻转 bug 从外面看应当是空的，不该被背面蒙混过关。
    """
    cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    n = _norm(cross)
    p1 = _add(p0, u)
    p2 = _add(p0, _add(u, v))
    p3 = _add(p0, v)
    return Geom([p0, p1, p2, p3], [n, n, n, n], [0, 1, 2, 0, 2, 3])


def cylinder(cx: float, base_y: float, cz: float, r: float, h: float, segs: int) -> Geom:
    """直立圆柱（+Y），底心 (cx, base_y, cz)。侧面法线光滑（跨面共享角度法线——
    曲面高光是光滑度类 bug 的探针，硬折法线会把它糊掉），盖/底硬法线 ±Y。
    三角形 = segs*4（侧 2/segs + 上下扇各 segs）。"""
    pos, nrm, idx = [], [], []
    for i in range(segs):
        a0 = 2.0 * math.pi * i / segs
        a1 = 2.0 * math.pi * (i + 1) / segs
        c0, s0 = math.cos(a0), math.sin(a0)
        c1, s1 = math.cos(a1), math.sin(a1)
        b = len(pos)
        # 侧面 4 顶点：[a0底, a1底, a1顶, a0顶]，各带自己的径向法线（光滑柱面）。
        # 绕序 (v0,v2,v1)：cross(v2-v0, v1-v0) 沿径向外向（数值核对过 a=0 处 = +X）——
        # (v0,v1,v2) 是内向的，背面剔除下柱面会从外面整个消失。
        pos += [(cx + r * c0, base_y, cz + r * s0),
                (cx + r * c1, base_y, cz + r * s1),
                (cx + r * c1, base_y + h, cz + r * s1),
                (cx + r * c0, base_y + h, cz + r * s0)]
        nrm += [(c0, 0.0, s0), (c1, 0.0, s1), (c1, 0.0, s1), (c0, 0.0, s0)]
        idx += [b, b + 2, b + 1, b, b + 3, b + 2]
        # 顶盖扇（法线 +Y；(center, rim(a1), rim(a0)) 为 +Y 侧 CCW——cross(e1,e2).y = sin(a0-a1) > 0）
        ct = len(pos)
        pos.append((cx, base_y + h, cz)); nrm.append((0.0, 1.0, 0.0))
        pos.append((cx + r * c1, base_y + h, cz + r * s1)); nrm.append((0.0, 1.0, 0.0))
        pos.append((cx + r * c0, base_y + h, cz + r * s0)); nrm.append((0.0, 1.0, 0.0))
        idx += [ct, ct + 1, ct + 2]
        # 底盖扇（法线 -Y）
        cb = len(pos)
        pos.append((cx, base_y, cz)); nrm.append((0.0, -1.0, 0.0))
        pos.append((cx + r * c0, base_y, cz + r * s0)); nrm.append((0.0, -1.0, 0.0))
        pos.append((cx + r * c1, base_y, cz + r * s1)); nrm.append((0.0, -1.0, 0.0))
        idx += [cb, cb + 1, cb + 2]
    return Geom(pos, nrm, idx)


def sphere(cx: float, cy: float, cz: float, r: float, lat: int, lon: int) -> Geom:
    """UV 球，光滑法线（= 径向）。极点三角退化为 3 顶点（不写出面积为 0 的退化三角——
    退化三角在背面剔除/光栅化上是未定义行为邻居，QA 基准不制造）。"""
    pos, nrm, idx = [], [], []
    for j in range(lat + 1):
        phi = math.pi * j / lat
        for i in range(lon):
            th = 2.0 * math.pi * i / lon
            nx = math.sin(phi) * math.cos(th)
            ny = math.cos(phi)
            nz = math.sin(phi) * math.sin(th)
            pos.append((cx + r * nx, cy + r * ny, cz + r * nz))
            nrm.append((nx, ny, nz))

    def vi(j, i):
        return j * lon + (i % lon)

    # 外向绕序（A=vi(j,i), B=vi(j,i+1), C=vi(j+1,i+1), D=vi(j+1,i) → (A,B,C)/(A,C,D)，
    # 数值核对 φ=90°/θ=0 处 cross = 径向外向）。顶带 A/B 同为北极点 → 只取 (A,C,D)
    # （即 (pole_i, ring1_{i+1}, ring1_i)）；底带 C/D 同点 → 只取 (A,B,C)。
    for j in range(lat):
        for i in range(lon):
            if j == 0:
                idx += [vi(j, i), vi(j + 1, i + 1), vi(j + 1, i)]
            elif j == lat - 1:
                idx += [vi(j, i), vi(j, i + 1), vi(j + 1, i + 1)]
            else:
                idx += [vi(j, i), vi(j, i + 1), vi(j + 1, i + 1),
                        vi(j, i), vi(j + 1, i + 1), vi(j + 1, i)]
    return Geom(pos, nrm, idx)


def torus(cx: float, cy: float, cz: float, ring_r: float, tube_r: float,
          maj: int, minu: int) -> Geom:
    """环面：主环在 XZ 平面（"平放"，洞朝 +Y），光滑法线。"""
    pos, nrm, idx = [], [], []
    for i in range(maj + 1):
        u = 2.0 * math.pi * i / maj
        cu, su = math.cos(u), math.sin(u)
        for k in range(minu + 1):
            v = 2.0 * math.pi * k / minu
            cv, sv = math.cos(v), math.sin(v)
            nrm.append((cv * cu, sv, cv * su))
            pos.append((cx + (ring_r + tube_r * cv) * cu, cy + tube_r * sv, cz + (ring_r + tube_r * cv) * su))

    def vi(i, k):
        return i * (minu + 1) + k

    # 外向绕序：A=(i,k), B=(i,k+1)（+v 方向）、D=(i+1,k)（+u 方向）；dP/du × dP/dv
    # 在 (u=0,v=0) 处 = ẑ×ŷ = −x̂ = 内向，故外向三角形为 (A,B,D) 与 (B,C,D)。
    for i in range(maj):
        for k in range(minu):
            idx += [vi(i, k), vi(i, k + 1), vi(i + 1, k),
                    vi(i, k + 1), vi(i + 1, k + 1), vi(i + 1, k)]
    return Geom(pos, nrm, idx)


def _merge_geoms(geoms: list) -> Geom:
    """多个 Geom 拼成一个（索引整体平移）。用途：shadow_rig 的地面 = 主平面 + 远端
    岛平台合并为**一个 primitive / 一个 objectID**——接收面是同一种材质（黄），
    影/光比度量要求"同一 piece 内对比"，拆成多个 piece 反而破坏度量口径。"""
    pos, nrm, idx = [], [], []
    for g in geoms:
        base = len(pos)
        pos += g.positions
        nrm += g.normals
        idx += [i + base for i in g.indices]
    return Geom(pos, nrm, idx)


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l)


# ---------------------------------------------------------------------------
# piece / 场景描述
# ---------------------------------------------------------------------------

@dataclass
class Piece:
    """一个可拾取对象 = 一个 mesh（单 primitive）+ 一个 material + 一个 node。"""
    name: str
    geom: Geom
    color: tuple                     # baseColorFactor 的 RGB（线性；alpha 恒 1）
    metallic: float = 0.0
    roughness: float = 0.5
    emissive: tuple | None = None    # 非 None → emissiveFactor（线性，strength 走默认 1.0）
    intent: str = ""                 # 设计意图（进 manifest；shadow_rig 逐物体必填）
    # 朝外性判据分类（2026-09-30 缺陷复盘加；「几何法线·顶点法线 > 0」的一致性断言
    # 只能抓两者不一致，抓不住"绕序与法线一起反向"的错误——故按几何类别补强判据）：
    #   radial=True  星形体（相对自身 bbox 中心星形）：逐三角形 dot(几何法线, 重心−中心)>0。
    #                盒/球/柱成立；torus 的管内侧按定义朝中心，**不适用**（实测会把
    #                正确绕序的 torus 标出 960/2304 "朝内"——v∈(110°,250°) 管内侧区域）。
    #   closed=True  封闭流形：位置缝合后每条无向边恰被 2 个三角形使用且方向相反
    #                （封闭 + 定向一致）；非星形者（torus）再补射线奇偶性种子判据
    #                （封闭 + 一致 + 连通 + 一个外向种子 ⟹ 全表面朝外）。
    radial: bool = True
    closed: bool = True

    def __post_init__(self):
        if self.color not in PAL.values():
            raise ValueError(f"piece {self.name}: color {self.color} not in fixed PAL")
        # 材质名连连看防御（configs/scene.lua material_styles：材质名含 "Emissive" →
        # light 风格、含 "Foliage" → 植被风格）：非自发光 piece 的名字若意外包含这两个
        # 子串，会被静默改路由、渲染到另一条管线上，piece 颜色对应的"预期值"整个作废。
        if self.emissive is None and ("Emissive" in self.name or "Foliage" in self.name):
            raise ValueError(f"piece {self.name}: name would be style-routed by "
                             f"material_styles pattern; rename it")


@dataclass
class Scene:
    name: str
    purpose: str
    honesty: str                     # 局限声明（进 manifest；不许假扮不是的东西）
    pieces: list = field(default_factory=list)
    cameras: dict = field(default_factory=dict)
    extra: dict = field(default_factory=dict)
    # 非 None = 构建期跑「级联跨距自证」（引擎公式复刻，见 cascade_self_proof）：
    # 断言遮挡体在预测边界下分处 ≥3 个不同级联，不满足 = 构建失败。
    cascade_camera: tuple | None = None


# ---------------------------------------------------------------------------
# 场景 1：cornell_box —— 只有几何是 Cornell 的
# ---------------------------------------------------------------------------
# 诚实声明（务必读）：经典 Cornell box 的标志性物理 = **面光源** + 墙间**颜色溢出**
# （color bleeding）。本引擎没有面光源（只有方向光 + 环境 cubemap，configs/scene.lua），
# 所以本场景实际验证的是：闭合几何正确性、红/绿双色墙 + 多材质、接触区 AO（SSAO
# 盒角/球底梯度）、以及「纯色块 ↔ objectID」反解链路本身。不要拿它和经典 radiance
# 参考图对比，那是在比一个本引擎没有的功能。
#
# 几何尺寸：内部 10×10×10 m（经典比例 ×5，贴合 bistro 的米制工作点，near/far 自适应
# 逻辑走与真场景相同的档位）。正面（z=+5）开放，推荐机位在 +Z 外向内看。
# 5 面墙全部单面朝内（背面剔除下从盒外看是空的——墙法线翻了应当立刻"墙没了"，
# 而不是被背面糊成一面颜色错误的墙）。

def scene_cornell_box() -> Scene:
    s = Scene(
        name="cornell_box",
        purpose="几何 + 双色墙 + 多材质 + 接触 AO 的纯色块基准（非辐射度量学 Cornell box）",
        honesty=("只借用了 Cornell box 的几何布局。引擎无面光源（只有方向光 + 环境 cubemap），"
                 "颜色溢出来不了也测不了；本场景的断言集不得包含 color bleeding。"),
        cameras={
            "front": {"position": [0.0, 5.0, 14.0], "target": [0.0, 4.0, 0.0],
                      "fovDegrees": 45.0},
        },
        extra={"layout": "内部 x/z ∈ [-5,5]、y ∈ [0,10]；左墙(-X)红 / 右墙(+X)绿 / 其余白位用纯色占位"},
    )
    P = s.pieces.append
    # 白墙位置（地板/天花板/后墙）也被赋高饱和纯色：规范要求 piece 两两可分，且近白
    # 本身会被 AO/曝光吃掉区分度——"经典配色"在这里让位于可识别性，见 PAL 注释。
    # 所有墙/地法线 = normalize(cross(u, v)) 且刻意朝盒内（单面朝内，见 quad() 注释）；
    # u/v 顺序即朝向，改动须经 cross 重算。
    P(Piece("floor", quad((-5.0, 0.0, -5.0), (0.0, 0.0, 10.0), (10.0, 0.0, 0.0)),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="阴影/AO 接收面 + 与盒底接触梯度；法线 +Y"))
    P(Piece("ceiling", quad((-5.0, 10.0, -5.0), (10.0, 0.0, 0.0), (0.0, 0.0, 10.0)),
            PAL["cyan"], roughness=0.9, radial=False, closed=False,
            intent="上界几何；法线 -Y 单面朝内"))
    P(Piece("back_wall", quad((-5.0, 0.0, -5.0), (10.0, 0.0, 0.0), (0.0, 10.0, 0.0)),
            PAL["blue"], roughness=0.9, radial=False, closed=False,
            intent="后墙（法线 +Z）；盒内深度基准面"))
    P(Piece("left_wall", quad((-5.0, 0.0, -5.0), (0.0, 10.0, 0.0), (0.0, 0.0, 10.0)),
            PAL["red"], roughness=0.9, radial=False, closed=False,
            intent="经典左墙红（法线 +X）；色溢出**不会**出现，见 honesty"))
    P(Piece("right_wall", quad((5.0, 0.0, -5.0), (0.0, 0.0, 10.0), (0.0, 10.0, 0.0)),
            PAL["green"], roughness=0.9, radial=False, closed=False,
            intent="经典右墙绿（法线 -X）"))
    P(Piece("box_tall", box(-1.8, 2.2, -1.2, 2.2, 4.4, 2.2, yaw_deg=25.0),
            PAL["orange"], roughness=0.7,
            intent="经典高方块（yaw 25°）：旋转 AABB vs 世界 AABB 的剔除/级联选级探针"))
    P(Piece("box_short", box(1.9, 1.3, 1.0, 2.6, 2.6, 2.6, yaw_deg=-18.0),
            PAL["purple"], roughness=0.7,
            intent="经典矮方块（yaw -18°）；顶面承球——接触区 = SSAO 阳性锚点"))
    P(Piece("sphere", sphere(1.9, 2.6 + 1.1, 1.0, 1.1, 24, 48),
            PAL["magenta"], roughness=0.5,
            intent="曲率连续性 + 与矮方块的接触 AO；光滑法线，棱线出现即顶点数据错"))
    return s


# ---------------------------------------------------------------------------
# 场景 2：shadow_rig —— 阴影验收专用装置（最重要）
# ---------------------------------------------------------------------------
# 按 skill rgl-verify「阴影验收判据」硬要求设计，逐条对应：
#
# 【要求 1 阳性锚点】anchor_cube_near：在默认主光（configs/scene.lua
#   direction = (0.5,-0.75,-0.45)，仰角 ≈ 48°）下**必然**投影。影位移精确值 =
#   高度 h × (d.x/|d.y|, d.z/|d.y|) = h × (2/3, -3/5)，即朝 (+X,-Z)；该象限内
#   所有遮挡体的影锥两两不相交、锚点影区内只有地面（构建期 assert，见下方校验环）。
#
# 【要求 2 跨级联分布 —— 设计对边界免疫，这是 2026-09-30 复盘后的定稿】
#   级联边界**不是常数**：near/far 由运行时自适应推导（rgl/ecs_impl.cppm projectionSystem：
#   near = max(0.05, dist−radius)，far = dist + radius，dist = 相机到场景包围球心、
#   radius = 各 primitive 包围球 unionSphere 合并），再经 cascadeSplitDistance
#   （practical split，count = kActiveShadowCascades = 4）切分。
#   教训（复盘）：上一版把地面做到 3400 m → radius=2543 → 实测边界 ≈ [342,685,1033,2738]，
#   放在 188 的"骑边界"物体实际稳稳落在 c0 内部——**按预测边界定点 = 会绿 = 无效断言**。
#   本版设计：
#     a) 地面缩到 ±210（radius 不再被地面撑爆；顺带治 yaw 扫掠满屏黄）；
#     b) 遮挡体距离用 ~×2 等比梯：≈12/24/48/96/192/384/768/1024 m，跨 ~2 个数量级。
#        只要梯子跨度大于任何一级级联的跨度，无论单调边界实际落在哪，都必然有
#        物体分处不同级联——**harness 不需要预测边界**，它从渲染色块反解
#        "哪个物体进了哪一级"即可；
#     c) 梯远端（384/768/1024）各有自己的岛平台（合并进 ground piece，同一材质），
#        远端物体的影落在自己岛上有承托，不落虚空；
#     d) 构建期跑 cascade_self_proof()：用引擎同款公式链（每 piece 去索引顶点
#        enclosingSphere → unionSphere 折叠 → near/far → practical split）预测本场景
#        在机位 A 下的级联占用，**断言 ≥3 个遮挡体分处 ≥3 个不同级联**，不满足 = 构建失败。
#        预测值只进 manifest 作参考，不是断言前提（它会随相机/场景内容漂移）。
#
# 【要求 3 远景取景】取景 B 与取景 A **同 position 纯转向**（顺带覆盖硬要求 2 的
#   "旋转分量独立自由度"）：只改 target 看 768/1024 m 档（far_tower / distant_marker）。
#
# 已知结构性细节：far = dist + radius 的球在"离相机最远顶点"处与远平面相切——本场景
# 最远顶点 = distant_marker 岛远端角，但 unionSphere 合并球比精确外接球松
# （半径由岛间距推得 ≈589 vs 相机到最远顶点 ≈1040 < far ≈1148），影关键区
# （影包络内）距远平面留 >100 m；岛外缘带才贴远平面，harness 采样不要越过影包络。
#
# 地面刻意取亮黄而非白：阴影验收的度量口径是「同材质阴影/受光比」（AGENTS.md 运行
# 要点——跨图全局百分位不可比），接收面需要亮、纯色、非近白。

SUN = (0.5, -0.75, -0.45)  # configs/scene.lua light.direction（锚点判据的前提）
# 单位高度影位移 = (d.x/|d.y|, d.z/|d.y|) = (2/3, -3/5)，与归一化无关（比例式）
SHADOW_OFF = (SUN[0] / -SUN[1], SUN[2] / -SUN[1])


def _rect_hull(rect, h: float):
    """影包络 = 遮挡体 footprint ∪ footprint+SHADOW_OFF·h；rect = (x0,x1,z0,z1)。"""
    ox, oz = SHADOW_OFF[0] * h, SHADOW_OFF[1] * h
    return (min(rect[0], rect[0] + ox), max(rect[1], rect[1] + ox),
            min(rect[2], rect[2] + oz), max(rect[3], rect[3] + oz))


def _rect_contains(outer, inner) -> bool:
    return outer[0] <= inner[0] and inner[1] <= outer[1] \
        and outer[2] <= inner[2] and inner[3] <= outer[3]


def _rect_overlap(a, b) -> bool:
    return a[0] < b[1] and b[0] < a[1] and a[2] < b[3] and b[2] < a[3]


def scene_shadow_rig() -> Scene:
    cam_a = (0.0, 4.0, 6.0)
    s = Scene(
        name="shadow_rig",
        purpose="阴影链验收装置：阳性锚点（要求1）+ 等比距离梯跨级联（要求2，边界免疫设计）"
                "+ 同位纯转向远景取景（要求3）",
        honesty=("锚点成立前提是光方向 = 默认值（scene.lua SUN）；harness 若经 RPC 改光，"
                 "须按新方向重算影位移再断言。级联边界是运行时自适应量（随相机距离与场景"
                 "包围球漂移），本 manifest 的 selfProof 只是构建期预测、不是契约；"
                 "跨级联断言的正确做法是从图像反解，见 cascadeReference.design。"),
        cameras={
            "near_anchor": {"position": list(cam_a), "target": [0.0, 1.5, -60.0],
                            "fovDegrees": 60.0,
                            "note": "取景 A：锚点断言 + 近中档遮挡体"},
            "far_sweep":   {"position": list(cam_a), "target": [0.0, 60.0, -762.0],
                            "fovDegrees": 60.0,
                            "note": "取景 B：与 A 同 position 纯转向（硬要求 2 的旋转分量）；"
                                    "far_tower/distant_marker 远景组"},
        },
        cascade_camera=cam_a,
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（锚点判据的前提）"},
            "shadowOffsetPerMeterOfHeight": [SHADOW_OFF[0], SHADOW_OFF[1]],
            "cascadeReference": {
                "runtimeAdaptive": True,
                "formula": "split_c = 0.5*near*(far/near)^(c/count) "
                           "+ 0.5*(near + (far-near)*(c+1)/count)，count=kActiveShadowCascades=4"
                           "（scene_context.cppm cascadeSplitDistance / scene_data.cppm）",
                "nearFarSource": "rgl/ecs_impl.cppm projectionSystem：near=max(0.05, dist-radius)、"
                                 "far=dist+radius；dist=相机→SceneBounds 球心；"
                                 "SceneBounds = 各 primitive 包围球 unionSphere 合并",
                "design": "遮挡体按 ~x2 等比梯布置（12→1024 m，跨 ~2 数量级）。边界不做预测、"
                          "由 harness 从图像反解（颜色→物体→哪一级有影）；梯子跨度大于任何"
                          "一级级联的跨度，故无论边界落在哪，必有物体分处不同级联。",
            },
            "note": "高度 h 的顶点影落点 = 基座 + h*(2/3, -3/5)（+X,-Z）；全部遮挡体的"
                    "影包络两两不相交、锚点影区内只有接收面——构建期 assert，不再是口头承诺",
        },
    )
    P = s.pieces.append
    # 地面 ±210 + 三块岛平台（far_cube / far_tower / distant_marker 的承托与影接收面）。
    # 岛尺寸 = 各自影包络 + 3 m 边距（下方 assert 兜底）。合并成单 primitive：
    # 接收面同材质 → 影/光比在同一片黄色内量。
    main_ground = (-210.0, 210.0, -210.0, 210.0)
    islands = {
        # far_cube hull x[-24,-4] z[-391.2,-372]
        "island_far_cube": (-27.0, -1.0, -394.5, -369.0),
        # far_tower hull x[-55,71.7] z[-868,-752]
        "island_far_tower": (-59.0, 76.0, -871.0, -749.0),
        # distant_marker hull x[20,36.7] z[-1029,-1013]
        "island_distant_marker": (17.0, 40.0, -1032.0, -1010.0),
    }

    def _flat_quad(rect):
        x0, x1, z0, z1 = rect
        return quad((x0, 0.0, z0), (0.0, 0.0, z1 - z0), (x1 - x0, 0.0, 0.0))

    P(Piece("ground",
            _merge_geoms([_flat_quad(r) for r in [main_ground, *islands.values()]]),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="影接收面（亮黄非近白）+ 三块岛平台（远端物体的影必须有承托，"
                   "不落虚空，否则远档锚点不可验证）；法线 +Y；同材质影/光比是主度量"))

    # 遮挡体：(name, geom, color, footprint rect, height, intent)
    # 距离梯（相机 A 到 bbox 心的实测近似）：12 / 24 / 49 / 97 / 192 / 384 / 771 / 1025
    occluders = [
        ("anchor_cube_near",
         box(0.0, 1.0, -6.0, 2.0, 2.0, 2.0), PAL["red"], (-1.0, 1.0, -7.0, -5.0), 2.0,
         "【硬要求 1】阳性锚点：默认主光下必投影到 (+X,-Z) 侧净空地面（梯首档，"
         "深居最小级联内部——任何路线任何级联下都必须看到影）"),
        ("near_post",
         box(2.0, 3.0, -18.0, 0.6, 6.0, 0.6), PAL["azure"], (1.7, 2.3, -18.3, -17.7), 6.0,
         "细长柱：影最窄、对亚像素错位最敏感；取景 A→B 同位纯转向时影向随 yaw 旋转"
         "——旋转分量探针（硬要求 2 的漏检面）"),
        ("near_cylinder",
         cylinder(7.0, 0.0, -42.0, 1.2, 3.0, 24), PAL["blue"], (5.8, 8.2, -43.2, -40.8), 3.0,
         "曲面遮挡体：影边界曲率 vs 级联 texel 尺寸；影包络与 near_post 刻意分离"),
        ("mid_cube",
         box(-10.0, 3.0, -90.0, 6.0, 6.0, 6.0), PAL["magenta"], (-13.0, -7.0, -93.0, -87.0), 6.0,
         "梯第 4 档（~97 m）：与锚点的远处一致性；最小级联深段"),
        ("step_cube",
         box(12.0, 4.0, -186.0, 8.0, 8.0, 8.0), PAL["cyan"], (8.0, 16.0, -190.0, -182.0), 8.0,
         "梯第 5 档（~192 m）：等比梯中段——边界无论落在哪，它与相邻档位分处不同跨距密度区"),
        ("far_cube",
         box(-18.0, 6.0, -378.0, 12.0, 12.0, 12.0), PAL["orange"], (-24.0, -12.0, -384.0, -372.0), 12.0,
         "最小级联盒外必须有影（『贴着相机全亮旁路』骗局的反面，要求 2）；影落自身岛台"),
        ("far_tower",
         box(-45.0, 80.0, -762.0, 20.0, 160.0, 20.0), PAL["green"], (-55.0, -35.0, -772.0, -752.0), 160.0,
         "远景高塔（~771 m）：硬要求 3 的取景目标；塔身自影（自身立面互遮）+ 160 m 投影"
         "落岛台，级间淡出带是否可见地连续"),
        ("distant_marker",
         box(25.0, 5.0, -1018.0, 10.0, 10.0, 10.0), PAL["purple"], (20.0, 30.0, -1023.0, -1013.0), 10.0,
         "梯最深档（~1025 m）：最远档仍必须有影；5a-7『半径恒 0 恒等阵』类退化数据"
         "在这档必然显形；影落岛台"),
    ]
    for name, geom, color, rect, h, intent in occluders:
        P(Piece(name, geom, color, roughness=0.7, intent=intent))

    # ---- 构建期几何断言（口头承诺不算数，坏了直接构建失败） ----
    hulls = {name: _rect_hull(rect, h) for name, _, _, rect, h, _ in occluders}
    ground_recs = {"main_ground": main_ground, **islands}
    for name, hull in hulls.items():
        assert any(_rect_contains(g, hull) for g in ground_recs.values()), \
            f"shadow_rig: {name} 的影包络 {hull} 不被任何接收面覆盖"
    rects = {name: rect for name, _, _, rect, _, _ in occluders}
    for a_name, hull in hulls.items():
        for b_name, rect in rects.items():
            if a_name != b_name:
                assert not _rect_overlap(hull, rect), \
                    f"shadow_rig: {a_name} 的影包络压上 {b_name}（锚点/对拍会被污染）"
    return s


# ---------------------------------------------------------------------------
# 场景 3：d1_parity —— forward / deferred 对拍（D1 指标）
# ---------------------------------------------------------------------------
# 目标：两条路线的差异**可量化**但不慢——~7k 三角形、10 个材质点（哑光介质 / 金属 /
# 混合粗糙 / 自发光），每个材质点独立 piece（独立颜色 → 独立 objectID），D1 差异可以
# 逐材质归因而非全图糊账。
#
# 自发光面板命名以 "Emissive" 开头是**故意的**：configs/scene.lua material_styles 的
# 「材质名含 Emissive → light 风格」连连看让两条路线都走批 11.0c 已修对等的发光路径
# （forward model_light 的 ×1/4 历史破口已消除，三处同值）。若用普通 pbr 材质携带
# emissiveFactor，deferred 的 G-buffer 路径对 emissive 的处理不在该对等修复覆盖内，
# 对拍结果会把「已知路线差异」混进 D1。
#
# 金属材质：metallicFactor=1 时无 baseColorTexture → 金属色 = baseColorFactor 染色反射；
# roughnessFactor 覆盖 0.25/0.4/0.6/0.75 四档，专测两路 GGX 与预滤波 env 采样差异。

def scene_d1_parity() -> Scene:
    s = Scene(
        name="d1_parity",
        purpose="forward/deferred D1 对拍：材质矩阵（哑光×3 + 金属×3 + 混合×1）+ 自发光×2 + 地面",
        honesty=("两路对拍前提 = 除 variant 外配置完全一致（含 env/曝光/ao.strength）；"
                 "SSAO 施加点在 tonemap 合成处连直接光一起压（AGENTS.md 已知边界），"
                 "逐材质 D1 应配合 ao.strength=0 的 BIT-IDENTICAL 基线各测一轮。"),
        cameras={
            "hero": {"position": [0.0, 4.5, 14.0], "target": [0.0, 1.2, -3.0],
                     "fovDegrees": 55.0},
        },
    )
    P = s.pieces.append
    P(Piece("ground", quad((-30.0, 0.0, -30.0), (0.0, 0.0, 60.0), (60.0, 0.0, 0.0)),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="公共接收面（法线 +Y）；两路地面差 = 光照模型差的地板"))
    P(Piece("matte_cube", box(-6.0, 1.25, -2.0, 2.5, 2.5, 2.5),
            PAL["red"], metallic=0.0, roughness=0.9, intent="哑光漫反射基准（两路必须几乎为零差）"))
    P(Piece("matte_sphere", sphere(-3.0, 1.2, -3.0, 1.2, 24, 48),
            PAL["blue"], metallic=0.0, roughness=0.5,
            intent="哑光介质中间粗糙度；光滑曲面 → 两路 GGX 高光的形状差"))
    P(Piece("metal_sphere", sphere(0.0, 1.2, -3.0, 1.2, 24, 48),
            PAL["green"], metallic=1.0, roughness=0.25,
            intent="高金属低粗糙：deferred 屏幕空间 G-buffer 的法线量化对锐反射最敏感（D1 主力探针）"))
    P(Piece("metal_box", box(3.0, 1.25, -2.0, 2.5, 2.5, 2.5),
            PAL["cyan"], metallic=1.0, roughness=0.6,
            intent="金属 + 中高粗糙：预滤波 env 高 mip 的两路采样一致性"))
    # radial=False：torus 的管内侧（管角 v∈(110°,250°)，即 960/2304 三角形）按定义
    # **朝中心**——「几何法线·(重心−bbox中心)>0」的径向判据对 1 亏格曲面不适用，
    # 正确绕序也会被打标（2026-09-30 复盘：外部独立解析器据此误报"960 朝内"，
    # 闭流形 + 方向一致 + 射线奇偶性三判据复核为全朝外）。本 piece 走
    # closed-manifold + parity 判据（见 validate_glb_bytes）。
    P(Piece("dielectric_torus", torus(6.0, 0.45, -3.0, 1.3, 0.45, 48, 24),
            PAL["magenta"], metallic=0.0, roughness=0.35, radial=False,
            intent="介质曲面 + 自遮挡（负 Gauss 区）：deferred 几何 pass 法线/深度重建的曲率压力点"))
    P(Piece("worn_cube", box(-4.5, 0.9, 1.5, 1.8, 1.8, 1.8),
            PAL["orange"], metallic=0.6, roughness=0.75,
            intent="非端点 metallic=0.6：两路 metallic 插值路径（diffuse 衰减 × 金属染色）逐通道差"))
    P(Piece("metal_cylinder", cylinder(4.5, 0.0, 1.5, 0.9, 2.4, 24),
            PAL["purple"], metallic=1.0, roughness=0.4,
            intent="金属曲面拉伸反射（柱面把 env 沿母线扫开）"))
    # 自发光面板：立式朝 +Z（hero 机位可见）；emissiveFactor = 纯色，strength 走默认 1.0
    # （loader CPU 端预乘 KHR_materials_emissive_strength，缺扩展 = 不加强，
    #  三路径同值语义见 bistro_pack.py 头注 2026-09-29 段——故意**不**写该扩展，
    #  省掉 extensionsUsed 声明面，两路读到同一字节）。
    P(Piece("Emissive_panel_a", quad((-2.5, 0.0, -6.0), (2.0, 0.0, 0.0), (0.0, 2.0, 0.0)),
            PAL["azure"], roughness=0.5, emissive=PAL["azure"], radial=False, closed=False,
            intent="自发光面板 A：light 风格连连看命中（名字含 Emissive）；两路发光亮度对等"))
    P(Piece("Emissive_strip", quad((0.5, 1.0, -6.0), (3.0, 0.0, 0.0), (0.0, 0.6, 0.0)),
            PAL["chartreuse"], roughness=0.5, emissive=PAL["chartreuse"], radial=False, closed=False,
            intent="自发光细长条：高频空间结构 → 两路 bloom 前的边缘差；与 A 颜色距离 0.87"))
    return s


SCENES = {
    "cornell_box": scene_cornell_box,
    "shadow_rig": scene_shadow_rig,
    "d1_parity": scene_d1_parity,
}


# ---------------------------------------------------------------------------
# glb 写出器（风格与 buffer 布局参照 tools/bistro_pack.py 的尾追加分配器，但此处
# 从零构造：无源文件、无贴图 → 单 buffer 纯几何）
# ---------------------------------------------------------------------------

COMP_F32 = 5126
COMP_U16 = 5123
_TYPE_NCOMP = {"SCALAR": 1, "VEC3": 3}


class GlbBuilder:
    """按 4 字节对齐尾追加 bufferView；每个 accessor 独占一个 bufferView（无交错无 stride
    ——引擎 locateAccessorStream 支持 stride，但 QA 数据不需要它，紧密排列让「accessor
    范围 ⊆ bufferView 范围 ⊆ buffer」这条自检链少一个自由度）。"""

    def __init__(self):
        self.bin = bytearray()
        self.buffer_views: list[dict] = []
        self.accessors: list[dict] = []
        self.materials: list[dict] = []
        self.meshes: list[dict] = []
        self.nodes: list[dict] = []

    def _append_bv(self, blob: bytes) -> int:
        # 对齐是**硬要求**：float32 accessor 的 byteOffset 必须是 4 的倍数（glTF 规范
        # 的 componentType 对齐），bistro_pack 同样按 4 对齐分配尾块。
        pad = (-len(self.bin)) & 3
        self.bin += b"\x00" * pad
        off = len(self.bin)
        self.bin += blob
        self.buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(blob)})
        return len(self.buffer_views) - 1

    def _add_accessor(self, blob: bytes, count: int, vtype: str, comp: int,
                      minmax: tuple | None) -> int:
        bv = self._append_bv(blob)
        acc: dict = {"bufferView": bv, "componentType": comp, "count": count, "type": vtype}
        if minmax is not None:
            # POSITION 的 min/max 是规范必需项（引擎与工具链依赖：剔除包围、级联包围）；
            # 值由调用方从**已 round 成 float32 的字节**反读后传入——这里不做二次舍入。
            lo, hi = minmax
            acc["min"] = list(lo)
            acc["max"] = list(hi)
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def add_piece(self, piece: Piece) -> None:
        g = piece.geom
        if len(g.positions) != len(g.normals):
            raise ValueError(f"piece {piece.name}: positions/normals count differ")
        if len(g.indices) % 3:
            raise ValueError(f"piece {piece.name}: index count not multiple of 3")
        # float32 量化**先于** min/max 统计（否则 manifest 里 0.1 的 min 和二进制里的
        # 0.10000000149011612 对不上；自检做的是精确相等，不是容差比较）。
        pos_bytes = struct.pack(f"<{3 * len(g.positions)}f", *(c for p in g.positions for c in p))
        pos_q = struct.unpack(f"<{3 * len(g.positions)}f", pos_bytes)
        nrm_bytes = struct.pack(f"<{3 * len(g.normals)}f", *(c for n in g.normals for c in n))
        use_u32 = len(g.positions) > 0xFFFF
        if use_u32:
            idx_bytes = struct.pack(f"<{len(g.indices)}I", *g.indices)
            idx_comp = 5125  # UNSIGNED_INT（loader 两者都收）
        else:
            idx_bytes = struct.pack(f"<{len(g.indices)}H", *g.indices)
            idx_comp = COMP_U16
        n = len(g.positions)
        pos_acc = self._add_accessor(pos_bytes, n, "VEC3", COMP_F32,
                                     (tuple(min(pos_q[i::3]) for i in range(3)),
                                      tuple(max(pos_q[i::3]) for i in range(3))))
        nrm_acc = self._add_accessor(nrm_bytes, n, "VEC3", COMP_F32, None)
        idx_acc = self._add_accessor(idx_bytes, len(g.indices), "SCALAR", idx_comp, None)

        mat: dict = {
            "name": piece.name,  # 引擎 material_styles 连连看的匹配字段（见 Piece 校验）
            "pbrMetallicRoughness": {
                # 三因子全部显式写出（含 metallic/roughness）：不吃 tinygltf / 规范默认值，
                # manifest 的 color 与 JSON 的 baseColorFactor 才能做精确相等校验。
                "baseColorFactor": [piece.color[0], piece.color[1], piece.color[2], 1.0],
                "metallicFactor": piece.metallic,
                "roughnessFactor": piece.roughness,
            },
        }
        if piece.emissive is not None:
            mat["emissiveFactor"] = [piece.emissive[0], piece.emissive[1], piece.emissive[2]]
        self.materials.append(mat)
        mat_i = len(self.materials) - 1
        mesh_i = len(self.meshes)
        self.meshes.append({
            "name": piece.name,
            "primitives": [{
                "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc},
                "indices": idx_acc,
                "material": mat_i,
                # 无 mode 字段 = 默认 4 (TRIANGLES)；无 TEXCOORD_0/TANGENT，见文件头
            }],
        })
        self.nodes.append({"name": piece.name, "mesh": mesh_i})

    def build(self) -> bytes:
        gltf = {
            "asset": {"version": "2.0", "generator": "tools/qa_scenes.py"},
            "scene": 0,
            "scenes": [{"name": "root", "nodes": list(range(len(self.nodes)))}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            # buffers[0].byteLength 必须 == BIN chunk 长度（含尾 0 填充），tinygltf 按
            # buffer 数据做边界校验，差一个字节 = 加载失败。
            "buffers": [{"byteLength": len(self.bin) + ((-len(self.bin)) & 3)}],
        }
        json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        json_bytes += b" " * ((-len(json_bytes)) & 3)  # JSON chunk 填充 = 空格 0x20（规范）
        bin_data = bytes(self.bin) + b"\x00" * ((-len(self.bin)) & 3)
        total = 12 + 8 + len(json_bytes) + 8 + len(bin_data)
        return (struct.pack("<4sII", b"glTF", 2, total)
                + struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes
                + struct.pack("<I4s", len(bin_data), b"BIN\x00") + bin_data)


# ---------------------------------------------------------------------------
# 自检（构建期，先于落盘）
# ---------------------------------------------------------------------------

def _cross(e1, e2):
    return (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0])


def _tri_edges(vals, a, b, c):
    A = (vals[3 * a], vals[3 * a + 1], vals[3 * a + 2])
    B = (vals[3 * b], vals[3 * b + 1], vals[3 * b + 2])
    C = (vals[3 * c], vals[3 * c + 1], vals[3 * c + 2])
    return A, B, C


def _radial_outward_issues(name: str, vals, ixs, center) -> list[str]:
    """星形体朝外判据：逐三角形 dot(几何法线, 重心 − bbox中心) > 0。

    存在的理由（2026-09-30 缺陷复盘）：'几何法线·顶点法线>0' 的一致性断言抓不住
    "绕序与顶点法线**一起**反向"的错误（两者同向 ⇒ 一致性恒绿）。径向判据与顶点
    法线完全无关，钉死绝对朝外。仅对星形几何成立——torus/平面 piece 标 radial=False。
    全三角形逐个跑，不抽样。"""
    errs = []
    for t in range(0, len(ixs), 3):
        A, B, C = _tri_edges(vals, ixs[t], ixs[t + 1], ixs[t + 2])
        g = _cross((B[0] - A[0], B[1] - A[1], B[2] - A[2]),
                   (C[0] - A[0], C[1] - A[1], C[2] - A[2]))
        cx = (A[0] + B[0] + C[0]) / 3.0 - center[0]
        cy = (A[1] + B[1] + C[1]) / 3.0 - center[1]
        cz = (A[2] + B[2] + C[2]) / 3.0 - center[2]
        if g[0] * cx + g[1] * cy + g[2] * cz <= 0.0:
            errs.append(f"piece {name}: triangle {t // 3} not radially outward")
            break
    return errs


def _closed_manifold_parity_issues(name: str, vals, ixs, parity: bool) -> list[str]:
    """封闭曲面判据：
      1) 位置缝合（round 1e-5，兜住 float32 噪声与 seam 重复顶点/极点重复顶点）后
         每条无向边恰被 2 个三角形使用、且两个使用方向相反（封闭 + 定向一致）；
      2) parity=True 时追加：三角形邻接图连通 + 射线奇偶性种子（≤12 个）——
         种子三角形重心沿几何法线推出 ε，向 +n 打射线数命中（剔除与本三角形共顶点的
         邻接件），偶数 = 推出点在实体外 = 绕序朝外。
    封闭 + 定向一致 + 连通 + 一个外向种子 ⟹ 全表面一致朝外；奇偶性与顶点法线、
    与 bbox 中心均无关，正好补上一致性断言漏掉的「一起反向」错误类。
    星形件（radial）跑 1) 不跑 2)（朝外由径向判据钉死，缝合检查是白赚的顶点级兜底）；
    torus 这类非星形封闭件跑 1)+2)（径向判据不适用，见 Piece docstring）。"""
    errs = []
    canon = {}

    def cid(v):
        k = (round(vals[3 * v] * 1e5), round(vals[3 * v + 1] * 1e5), round(vals[3 * v + 2] * 1e5))
        if k not in canon:
            canon[k] = len(canon)
        return canon[k]

    edge_users = {}     # 有向边 → 使用三角形数（封闭+一致 ⇔ 每条有向边恰 1 次）
    for t in range(0, len(ixs), 3):
        ca, cb, cc = cid(ixs[t]), cid(ixs[t + 1]), cid(ixs[t + 2])
        for e in ((ca, cb), (cb, cc), (cc, ca)):
            edge_users[e] = edge_users.get(e, 0) + 1
    bad_edges = [e for e, n in edge_users.items()
                 if n != 1 or edge_users.get((e[1], e[0]), 0) != 1]
    if bad_edges:
        return [f"piece {name}: not a closed consistently-oriented manifold "
                f"({len(bad_edges)} 条有向边未与反向边一一配对)"]
    # 连通性（union-find over 三角形，按共享边合并）
    tri_of_edge = {}
    n_tri = len(ixs) // 3
    parent = list(range(n_tri))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for ti, t in enumerate(range(0, len(ixs), 3)):
        ca, cb, cc = cid(ixs[t]), cid(ixs[t + 1]), cid(ixs[t + 2])
        for e in ((ca, cb), (cb, cc), (cc, ca)):
            u = (e[1], e[0])
            if u in tri_of_edge:
                ra, rb = find(ti), find(tri_of_edge[u])
                parent[ra] = rb
            else:
                tri_of_edge[e] = ti
    if parity and len({find(i) for i in range(n_tri)}) > 1:
        errs.append(f"piece {name}: closed manifold but disconnected (奇偶性种子无法外推全表面)")
    if not parity:
        return errs
    # 射线奇偶性种子
    xs = [vals[3 * v] for v in set(ixs)]
    ys = [vals[3 * v + 1] for v in set(ixs)]
    zs = [vals[3 * v + 2] for v in set(ixs)]
    diag = math.sqrt((max(xs) - min(xs)) ** 2 + (max(ys) - min(ys)) ** 2
                     + (max(zs) - min(zs)) ** 2)
    eps = 1e-3 * diag

    def ray_tri(o, d, A, B, C):
        e1 = (B[0] - A[0], B[1] - A[1], B[2] - A[2])
        e2 = (C[0] - A[0], C[1] - A[1], C[2] - A[2])
        p = _cross(d, e2)
        det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2]
        if abs(det) < 1e-12:
            return False
        inv = 1.0 / det
        s0 = (o[0] - A[0], o[1] - A[1], o[2] - A[2])
        u = (s0[0] * p[0] + s0[1] * p[1] + s0[2] * p[2]) * inv
        if u < -1e-9 or u > 1 + 1e-9:
            return False
        q = _cross(s0, e1)
        v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv
        if v < -1e-9 or u + v > 1 + 1e-9:
            return False
        s = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv
        return s > 1e-7

    n_seeds = min(12, n_tri)
    for si in range(n_seeds):
        t = (si * n_tri // n_seeds) * 3
        a, b, c = ixs[t], ixs[t + 1], ixs[t + 2]
        A, B, C = _tri_edges(vals, a, b, c)
        n = _cross((B[0] - A[0], B[1] - A[1], B[2] - A[2]),
                   (C[0] - A[0], C[1] - A[1], C[2] - A[2]))
        nl = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if nl == 0.0:
            errs.append(f"piece {name}: degenerate seed triangle {t // 3}")
            break
        n = (n[0] / nl, n[1] / nl, n[2] / nl)
        o = ((A[0] + B[0] + C[0]) / 3.0 + n[0] * eps,
             (A[1] + B[1] + C[1]) / 3.0 + n[1] * eps,
             (A[2] + B[2] + C[2]) / 3.0 + n[2] * eps)
        nbr = {a, b, c}
        hits = 0
        for t2 in range(0, len(ixs), 3):
            if t2 == t or nbr & {ixs[t2], ixs[t2 + 1], ixs[t2 + 2]}:
                continue
            A2, B2, C2 = _tri_edges(vals, ixs[t2], ixs[t2 + 1], ixs[t2 + 2])
            if ray_tri(o, n, A2, B2, C2):
                hits += 1
        if hits % 2 == 1:
            errs.append(f"piece {name}: seed triangle {t // 3} 射线奇偶性=奇 ⇒ 绕序朝内"
                        f"（closed-manifold 朝外判据失败）")
            break
    return errs

def validate_glb_bytes(glb: bytes, manifest: dict) -> list[str]:
    """结构自洽校验；返回错误列表（空 = 通过）。

    阳性锚点思路（AGENTS.md「写测试的硬纪律」）：每条断言都问过"如果生成器这里坏了
    这条会绿吗"——min/max 与二进制**精确**相等（不是容差比较，能抓 float 舍入分叉）、
    baseColorFactor 与 manifest **精确**相等（抓调色板/写盘分叉）、索引 < 顶点数
    （抓 mesh 局部索引表越界，loader 同样校验、它拒载比这里报错更难查）。
    """
    errs: list[str] = []
    if len(glb) < 20 or glb[0:4] != b"glTF":
        return ["bad magic"]
    version, total = struct.unpack("<II", glb[4:12])
    if version != 2:
        errs.append(f"version {version} != 2")
    if total != len(glb):
        errs.append(f"header total {total} != file size {len(glb)}")
    jlen, jtype = struct.unpack("<I4s", glb[12:20])
    if jtype != b"JSON" or jlen % 4:
        errs.append("first chunk is not 4-aligned JSON")
    try:
        root = json.loads(glb[20:20 + jlen].decode("utf-8"))
    except Exception as e:  # noqa: BLE001 — 报错转文本
        return errs + [f"JSON chunk parse fail: {e}"]
    blen, btype = struct.unpack("<I4s", glb[20 + jlen:28 + jlen])
    if btype != b"BIN\x00" or blen % 4:
        errs.append("second chunk is not 4-aligned BIN")
    if 28 + jlen + blen != len(glb):
        errs.append(f"chunks {28 + jlen + blen} != file size {len(glb)}")
    bin_data = glb[28 + jlen:28 + jlen + blen]
    # BIN 尾部 0 填充检查：最后一个 bufferView 末尾之后的字节必须全 0
    bv_pad_from = max(bv["byteOffset"] + bv["byteLength"] for bv in root["bufferViews"])
    if any(bin_data[bv_pad_from:]):
        errs.append("BIN padding after last bufferView is not zero")

    bufs = root["buffers"]
    if len(bufs) != 1 or bufs[0]["byteLength"] != blen:
        errs.append(f"buffers[0].byteLength {bufs[0].get('byteLength')} != BIN len {blen}")
    if "uri" in bufs[0]:
        errs.append("embedded glb must not declare buffer uri")

    # bufferView ⊆ buffer
    for i, bv in enumerate(root["bufferViews"]):
        if bv["buffer"] != 0 or bv["byteOffset"] % 4 or bv["byteOffset"] + bv["byteLength"] > blen:
            errs.append(f"bufferView {i} out of bounds/misaligned")
    comp_size = {COMP_F32: 4, COMP_U16: 2, 5125: 4}

    # accessor ⊆ bufferView
    for i, acc in enumerate(root["accessors"]):
        cs = comp_size.get(acc["componentType"])
        nc = _TYPE_NCOMP.get(acc["type"])
        if cs is None or nc is None:
            errs.append(f"accessor {i}: bad componentType/type")
            continue
        bv = root["bufferViews"][acc["bufferView"]]
        need = acc.get("byteOffset", 0) + (acc["count"] - 1) * acc.get("byteStride", 0) + nc * cs
        if need > bv["byteLength"]:
            errs.append(f"accessor {i} range {need} > bufferView len {bv['byteLength']}")

    # 逐 piece 深校验（对照 manifest）
    pieces = manifest["pieces"]
    if len(root["meshes"]) != len(pieces) or len(root["nodes"]) != len(pieces):
        errs.append(f"mesh/node count != manifest piece count")
    for pi, (entry, mesh) in enumerate(zip(pieces, root["meshes"])):
        prim = mesh["primitives"][0]
        node = root["nodes"][pi]
        if mesh["name"] != entry["name"] or node["name"] != entry["name"]:
            errs.append(f"piece {pi}: name mismatch with manifest")
        pos = root["accessors"][prim["attributes"]["POSITION"]]
        nrm = root["accessors"][prim["attributes"]["NORMAL"]]
        idx = root["accessors"][prim["indices"]]
        # POSITION min/max vs 二进制实测（精确相等）
        poff = root["bufferViews"][pos["bufferView"]]["byteOffset"] + pos.get("byteOffset", 0)
        vals = struct.unpack(f"<{3 * pos['count']}f", bin_data[poff:poff + 12 * pos["count"]])
        lo = tuple(min(vals[k::3]) for k in range(3))
        hi = tuple(max(vals[k::3]) for k in range(3))
        if tuple(pos.get("min", ())) != lo or tuple(pos.get("max", ())) != hi:
            errs.append(f"piece {entry['name']}: POSITION min/max != binary")
        if tuple(entry["aabb"]["min"]) != lo or tuple(entry["aabb"]["max"]) != hi:
            errs.append(f"piece {entry['name']}: manifest AABB != binary POSITION range")
        # NORMAL 单位长（1e-4 容差；法线是解析式算出来的，正常应为 1±1e-7 量级，
        # 容差只兜 float32 化，兜不住"忘归一化"——归一化前的长度差远大于此）
        noff = root["bufferViews"][nrm["bufferView"]]["byteOffset"] + nrm.get("byteOffset", 0)
        nvals = struct.unpack(f"<{3 * nrm['count']}f", bin_data[noff:noff + 12 * nrm["count"]])
        for k in range(0, 3 * nrm["count"], 3):
            l2 = nvals[k] ** 2 + nvals[k + 1] ** 2 + nvals[k + 2] ** 2
            if abs(l2 - 1.0) > 1e-4:
                errs.append(f"piece {entry['name']}: normal #{k // 3} len^2 = {l2} not unit")
                break
        # 索引读取 + 界 + 三角形数（先于绕序检查：绕序检查消费 ixs）
        ibyte = (root["bufferViews"][idx["bufferView"]]["byteOffset"]
                 + idx.get("byteOffset", 0))
        esz = comp_size[idx["componentType"]]
        fmt = "H" if esz == 2 else "I"
        ixs = struct.unpack(f"<{idx['count']}{fmt}",
                            bin_data[ibyte:ibyte + esz * idx["count"]])
        idx_out_of_range = any(ix >= pos["count"] for ix in ixs)
        if idx_out_of_range:
            errs.append(f"piece {entry['name']}: index >= vertex count")
        if idx["count"] // 3 != entry["triangleCount"]:
            errs.append(f"piece {entry['name']}: triangleCount mismatch")
        # 绕序 ⇔ 法线一致性（阳性锚点：本生成器初版 sphere/torus/柱侧面与 4 个 quad 的
        # 绕序/法线错全是既有"范围类"断言抓不到的——背面剔除下症状是"物体消失"而非报错。
        # 每三角形几何法线（绕序决定）· 三顶点法线之和 > 0，抓一整类翻绕错误；
        # 本场景所有图元都满足"外向绕序 = 外向顶点法线"，双背面几何不适用）。
        # 越界索引时跳过（变异测试补：否则校验器自己踩越界崩溃，报错被 IndexError 吞掉）。
        for t in range(0, len(ixs) - 2, 3) if not idx_out_of_range else ():
            a, b_, c_ = ixs[t], ixs[t + 1], ixs[t + 2]
            e1 = [vals[3 * b_ + q] - vals[3 * a + q] for q in range(3)]
            e2 = [vals[3 * c_ + q] - vals[3 * a + q] for q in range(3)]
            g = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0]]
            nsum = [nvals[3 * a + q] + nvals[3 * b_ + q] + nvals[3 * c_ + q] for q in range(3)]
            if g[0] * nsum[0] + g[1] * nsum[1] + g[2] * nsum[2] <= 0.0:
                errs.append(f"piece {entry['name']}: triangle {t // 3} winding vs normals disagree")
                break
        # 朝外性判据（2026-09-30 缺陷复盘加，分类理由见 Piece docstring）：一致性断言
        # 只能抓"几何法线与顶点法线不一致"，抓不住"两者一起反向"；这里按件型补上
        # 与顶点法线无关的绝对朝外断言。逐三角形（radial）/ 缝合+奇偶性（closed）。
        if not idx_out_of_range:
            test = entry.get("outwardTest")
            if test == "radial":
                center = tuple((entry["aabb"]["min"][k] + entry["aabb"]["max"][k]) / 2.0
                               for k in range(3))
                errs += _radial_outward_issues(entry["name"], vals, ixs, center)
                errs += _closed_manifold_parity_issues(entry["name"], vals, ixs, parity=False)
            elif test == "closed-manifold":
                errs += _closed_manifold_parity_issues(entry["name"], vals, ixs, parity=True)
        # 材质存在且 baseColorFactor 与 manifest 精确相等
        if not (0 <= prim["material"] < len(root["materials"])):
            errs.append(f"piece {entry['name']}: material index dangling")
            continue
        mat = root["materials"][prim["material"]]
        bcf = mat["pbrMetallicRoughness"]["baseColorFactor"]
        if bcf != [entry["color"][0], entry["color"][1], entry["color"][2], 1.0]:
            errs.append(f"piece {entry['name']}: baseColorFactor != manifest color")
        if mat.get("name") != entry["name"]:
            errs.append(f"piece {entry['name']}: material name mismatch")
    # 同场景调色板距离下限（反查可靠性的地基，见 PAL 注释）
    colors = [tuple(p["color"]) for p in pieces]
    for i, c in enumerate(colors):
        # 通道跨度 ≥ 0.5 = 反灰/反近黑/近白断言（变异测试补：注入灰 (0.5,0.48,0.52) 与
        # 各原色距离 ≥ 0.84，仅靠两两距离抓不住——「避免灰」必须单独成断言，
        # 灰的表观值由光照强度决定，AO/曝光直接吃掉区分度，见 PAL 注释判据 2）
        if max(c) - min(c) < 0.5:
            errs.append(f"piece {pieces[i]['name']}: color {c} channel spread < 0.5 (gray-ish)")
    for i in range(len(colors)):
        for j in range(i + 1, len(colors)):
            d = math.dist(colors[i], colors[j])
            if d < MIN_COLOR_DIST - 1e-9:
                errs.append(f"palette distance {d:.3f} < {MIN_COLOR_DIST} "
                            f"(pieces {i},{j})")
    return errs


# ---------------------------------------------------------------------------
# 场景 → glb + manifest
# ---------------------------------------------------------------------------

def _engine_union_sphere(a, b):
    """rgl/ecs_impl.cppm unionSphere 的逐式复刻（SceneBounds 聚合方式）。"""
    (ac, ar), (bc, br) = a, b
    if ar == 0.0:
        return b
    if br == 0.0:
        return a
    d = math.dist(ac, bc)
    if ar >= d + br:
        return a
    if br >= d + ar:
        return b
    r = 0.5 * (ar + br + d)
    t = ar / (d + r) if (d + r) > 0.0 else 0.0
    return (tuple(ac[i] + (bc[i] - ac[i]) * t for i in range(3)), r)


def cascade_self_proof(scene: "Scene") -> dict:
    """构建期级联跨距自证：复刻引擎公式链——
      每 primitive 去索引顶点 enclosingSphere（loader computeBounds：质心 + 最远点）
      → SceneBounds = unionSphere 折叠（ecs_impl，primitive 序）
      → near = max(0.05, dist−radius)、far = dist + radius（projectionSystem）
      → practical split 边界（cascadeSplitDistance，count = kActiveShadowCascades = 4）。
    断言：遮挡体（radial 件；地面等接收面不算）分处 ≥3 个不同预测级联，
    不满足 = 梯子设计失效 = 构建失败。预测值进 manifest 仅供参考——边界随
    相机/场景内容漂移，**harness 的运行时断言必须从图像反解而非信任这里的数字**
    （上一版正是把预测边界当常数排布，radius 被 3400m 地面撑到 2543 后
    "骑边界"物体实际什么都没跨，2026-09-30 复盘）。"""
    acc = ((0.0, 0.0, 0.0), 0.0)
    for p in scene.pieces:
        pts = [p.geom.positions[i] for i in p.geom.indices]  # 引擎按 index 去索引展开
        n = len(pts)
        if n == 0:
            continue
        c = tuple(sum(pt[k] for pt in pts) / n for k in range(3))
        r = max(math.dist(pt, c) for pt in pts)
        acc = _engine_union_sphere(acc, (c, r))
    center, radius = acc
    cam = scene.cascade_camera
    d = math.dist(cam, center)
    near = max(0.05, d - radius)
    far = d + radius
    count = 4
    bounds = []
    for c in range(count):
        if c + 1 >= count:
            bounds.append(far)
        else:
            log_s = near * (far / near) ** (c / count)
            uni = near + (far - near) * ((c + 1) / count)
            bounds.append(0.5 * log_s + 0.5 * uni)
    occupancy = [[] for _ in range(count)]
    ladders = []
    for p in scene.pieces:
        if not p.radial:  # 接收面（地面）不进梯子
            continue
        pos = p.geom.positions
        c_p = tuple((min(v[k] for v in pos) + max(v[k] for v in pos)) / 2.0 for k in range(3))
        dist = math.dist(cam, c_p)
        cascade = next(i for i, b in enumerate(bounds) if dist <= b)
        occupancy[cascade].append(p.name)
        ladders.append({"name": p.name, "eyeDistanceApprox": round(dist, 2),
                        "predictedCascade": cascade})
    occupied = sum(1 for o in occupancy if o)
    if occupied < 3:
        raise SystemExit(
            f"[cascade-self-proof FAILED] {scene.name}: 遮挡体仅占 {occupied} 个预测级联 "
            f"(boundaries≈{[round(b, 1) for b in bounds]}, occupancy={occupancy}) "
            f"—— 等比梯失效，调整地面/梯子后再交")
    return {
        "note": ("generator-side PREDICTION via engine formula replicas (unionSphere / "
                 "projectionSystem / cascadeSplitDistance, count=4); boundaries drift with "
                 "runtime camera & scene bounds — harness must reverse-solve cascade "
                 "membership from rendered images, not trust these numbers"),
        "camera": list(cam),
        "sceneBoundsCenter": [center[0], center[1], center[2]],
        "sceneBoundsRadius": radius,
        "nearPredicted": near,
        "farPredicted": far,
        "boundariesPredictedApprox": [round(b, 1) for b in bounds],
        "occupancyPredicted": {f"cascade{i}": occupancy[i] for i in range(count)},
        "ladder": ladders,
        "occupiedCascadeCount": occupied,
    }


def build_scene(scene: Scene) -> tuple[bytes, str, dict]:
    builder = GlbBuilder()
    names = [p.name for p in scene.pieces]
    if len(set(names)) != len(names):
        raise ValueError(f"{scene.name}: duplicate piece names {names}")
    for p in scene.pieces:
        builder.add_piece(p)
    glb = builder.build()

    # manifest：静态事实（名字/颜色/几何/下标）。**没有 objectID**——它是运行时全局
    # objectSlot 计数器值（跨模型递增，79fc778），离线断言它会制造一个必然过期的谎言；
    # harness 从 object_id 通道 + 本文件的颜色表在运行时建立映射。
    manifest = {
        "scene": scene.name,
        "purpose": scene.purpose,
        "honesty": scene.honesty,
        "generator": "tools/qa_scenes.py",
        "colorSpace": "linear (glTF baseColorFactor) — see GAMMA_NOTE",
        "gammaNote": GAMMA_NOTE,
        "coordinateSystem": "Y-up, meters, world coords baked into vertices (nodes identity)",
        "mapping": ("piece order == flattened globalPrimitiveIndex == meshIndex == nodeIndex "
                    "== materialIndex (1 piece = 1 mesh = 1 primitive = 1 material)"),
        "objectIds": ("deliberately ABSENT: objectID = runtime global objectSlot counter. "
                      "imgqa names ids by mapping order (id == primitiveIndex + 1, verified "
                      "2026-09-30: ids 1..9 contiguous) and uses closed-form predicted chroma "
                      "as a WEAK validator only -- raw color nearest-neighbor is NOT reliable: "
                      "ACES saturation shifts chroma by 0.1-0.15, same magnitude as inter-piece "
                      "chroma distance (far_cube mis-mapped to ground in first attempt)"),
        "cameras": scene.cameras,
        "pieces": [],
    }
    manifest.update(scene.extra)
    total_tris = 0
    for i, p in enumerate(scene.pieces):
        pos = p.geom.positions
        lo = [min(c[k] for c in pos) for k in range(3)]
        hi = [max(c[k] for c in pos) for k in range(3)]
        # float32 化的 AABB（与 accessor min/max 同值同理由：先量化后统计）
        lo = [struct.unpack("<f", struct.pack("<f", v))[0] for v in lo]
        hi = [struct.unpack("<f", struct.pack("<f", v))[0] for v in hi]
        tri = len(p.geom.indices) // 3
        total_tris += tri
        entry = {
            "name": p.name,
            "color": list(p.color),
            "metallic": p.metallic,
            "roughness": p.roughness,
            "emissive": list(p.emissive) if p.emissive else None,
            "primitiveIndex": i,
            "meshIndex": i,
            "materialIndex": i,
            "vertexCount": len(pos),
            "triangleCount": tri,
            # 朝外性判据类别（harness 独立复核时用同类判据；分类理由见 Piece docstring：
            # 径向判据对非星形几何——torus 管内侧、平面——数学上不适用，误用会把正确
            # 绕序标成"朝内"）："radial" | "closed-manifold" | "none"
            "outwardTest": "radial" if p.radial else ("closed-manifold" if p.closed else "none"),
            "aabb": {"min": lo, "max": hi},
            "intent": p.intent,
        }
        manifest["pieces"].append(entry)
    manifest["totalTriangles"] = total_tris
    proof = cascade_self_proof(scene) if scene.cascade_camera is not None else None
    if proof is not None:
        manifest["cascadeSelfProof"] = proof
    manifest_json = json.dumps(manifest, indent=2, ensure_ascii=True) + "\n"

    # 自检先于落盘：失败 = 不落坏文件（AGENTS.md：不自检的生成器等于没测）
    errs = validate_glb_bytes(glb, json.loads(manifest_json))
    if errs:
        raise SystemExit(f"[self-check FAILED] {scene.name}:\n  " + "\n  ".join(errs))
    return glb, manifest_json, {"pieces": len(scene.pieces), "tris": total_tris,
                                "bytes": len(glb), "manifestBytes": len(manifest_json.encode()),
                                "cascade": proof["occupiedCascadeCount"] if proof else None}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="程序化 QA 场景生成器（cornell_box / shadow_rig / d1_parity）。"
                    "输出默认落 build/qa_scenes/（gitignore 内；引擎 CWD=build/rgl/，"
                    "harness 用绝对路径或拷入 build/rgl/models/ 加载）。")
    ap.add_argument("--out", default="build/qa_scenes", help="输出目录（默认 build/qa_scenes）")
    ap.add_argument("--only", choices=sorted(SCENES), help="只生成指定场景")
    args = ap.parse_args(argv)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    grand_bytes = 0
    for name in (sorted(SCENES) if not args.only else [args.only]):
        scene = SCENES[name]()
        glb, manifest_json, stats = build_scene(scene)
        (out / f"{name}.glb").write_bytes(glb)
        (out / f"{name}.manifest.json").write_text(manifest_json, encoding="ascii")
        grand_bytes += stats["bytes"] + stats["manifestBytes"]
        print(f"{name:<12} pieces={stats['pieces']:>2}  tris={stats['tris']:>5}  "
              f"glb={stats['bytes']:>7} B  manifest={stats['manifestBytes']:>6} B  "
              f"self-check=OK"
              + (f"  cascade-self-proof: {stats['cascade']} 预测级联（occupied）"
                 if stats.get("cascade") else "")
              + f"  -> {out}/{name}.glb")
    print(f"{'TOTAL':<12} {'':>10} {'':>10} out={out}  totalBytes={grand_bytes}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
