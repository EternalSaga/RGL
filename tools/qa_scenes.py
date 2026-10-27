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
# ## TEXCOORD_0 / TANGENT / normalTexture：默认仍不写，nm_rig 显式开
#
# 加载器契约（gltf_loader_impl.cppm readVertexAttributes）：POSITION/NORMAL 必需，缺 = 整个
# 模型加载失败；TEXCOORD_0 可选（缺 → uv 填 0 走白纹理，**且 normal 槽整体回落中性纹理**：
# texture_pool.cppm uploadModelTextures 的 `if (!prim.hasUv) continue;`）、TANGENT 可选
# （缺 → Frisvad 解析正交基兜底）。历史理由仍然成立：**不输出 = 少两条出错路径**，所以
# cornell_box / shadow_rig / izb_rig 等早期场景**一个字节都没变**
# （改前改后 md5 逐字节比对是当时的硬验收项；d1_parity / env_band 两场景已随图变体
# 收敛刀②.2 删除，见下）。只有 nm_rig 需要纹理面，它显式写。
#
# 三条**非显然**的加载器契约（本轮实测查实，写错任何一条 = 纹理静默不生效、画面纹丝不动）：
#   ① `material.normalTexture.index` 是 **images[] 下标**，不是 glTF 标准的 textures[] 下标
#      （gltf_loader_impl.cppm:269 拿它与 gltf.images.size() 比；bistro_pack.py:292 的
#      index remap 同样直接映射到 images）。写标准形态 → 下标越界 → 静默回落中性纹理。
#   ② 图片**必须内嵌**（images[i].bufferView + mimeType），URI 外部文件不支持
#      （collectImages 只从 bufferView 取字节；无数据 → warn + 回落）。
#   ③ normal/ORM 走 **linear 上传**（禁 sRGB 解码），且 UNORM 纹理**会生成 mip 链**
#      （texture_upload_impl.cppm recordMipZeroAndChain → mipGen.recordChain）。
#      ⟹ 高频 normal 图在掠射区会被 mip 平均掉，"频率"轴必须连 world/texel 一起扫。
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
# ## 各场景的诚实定位
#
# - cornell_box：**只是 Cornell box 的几何**。真正的标志性特征是面光源 + 颜色溢出，
#   本引擎没有面光源（只有方向光 + 环境 cubemap，见 configs/scene.lua light 段），
#   所以它验证的是「几何正确性 + 双色墙 + 多材质 + AO 接触」，**不是**辐射度量学意义上的
#   Cornell box 基准，不要拿它跟经典 radiance 参考图对比。
# - shadow_rig：最重要。按 skill rgl-verify「阴影验收判据」硬要求 1-3 设计（阳性锚点 /
#   跨级联分布 / 远景取景），逐物体的失效模式在场景函数注释里。
# - d1_parity / env_band：已随图变体收敛刀②.2 删除（docs/archive/图变体收敛_plan.md §6.1-3：
#   d1_parity = forward/deferred 两路对拍，forward 已不存在；env_band = 实验已结案的
#   「带边」靶，零自动化消费者，冻结归档，现场见 git history @ < 2026-10-18）。
# - izb_rig：FTIZB/IZB 硬阴影 spike 装置（docs/hfts_izb_plan.md 批 1a）——厚度梯
#   0.4→0.02 m + 接触柱 + 穿插杆 + 叠影三分带；判据 = shadow.lua 的 izb 开关 A/B 对拍。
# - nm_rig：**带 normal map 的受控靶**（本轮加）。单材质单 quad 平坦金属地面 + 程序化
#   normal map，扰动**幅度**与**频率**两轴独立可扫。存在的理由是一条构造性事实：
#   deferred_geometry.frag.slang 无 normal map 时 `geoN = worldNormal(...)`、`N = geoN`
#   （该式承自已删除的 forward model_pbr_ibl.frag.slang:52-53，历史对偶）
#   ⟹ shading 法线与 specular AA 的输入是同一个量，**不带 normal map 在构造上就复现不出**
#   「法线高频变化而 specular AA 不察觉」那一类噪点（env_band 的 honesty 已把这条写成局限）。

import argparse
import json
import math
import struct
import sys
import zlib
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


def _f32(v: float) -> float:
    """float64 → float32 → float64（本文件的「先量化后统计」纪律：manifest 里的数必须与
    二进制里的**同一个** float32 相等，自检做的是精确相等而不是容差比较）。"""
    return struct.unpack("<f", struct.pack("<f", v))[0]


# ---------------------------------------------------------------------------
# stdlib PNG 写出（normal map 用）。**不许引入 PIL / numpy**（AGENTS.md 工具语言纪律：
# 进仓库的 Python 只准 stdlib-only）。约 40 行：signature + IHDR + IDAT + IEND，
# 每 chunk = 长度(4BE) + 类型(4) + 数据 + CRC32(4BE, 覆盖 类型+数据)。
#
# 为什么只写 **8bit / color type 2 (RGB) / filter 0 (None) / 非隔行**：
#   · 位深不是我们能选的——引擎 image_codec_impl.cppm:33-36 用 libpng 简单 API 并**强制
#     请求 PNG_FORMAT_RGBA**，16bit 源会被 libpng 自己降到 8bit。写 16bit 换不来精度，
#     只换一条"以为自己是 16bit"的错觉。所以量化风险必须正面处理，见 _normal_rows 注释。
#   · filter 0 = 每行原样（不 Sub/Up/Average/Paeth）：编码器与自检解码器都少一个自由度，
#     且 normal map 的相邻 texel 本来就不相关（分块常量），过滤省不了几个字节。
#   · RGB 而非 RGBA：引擎对无 alpha 源显式补 255（image_codec_impl.cppm:38-40），
#     而 normal 的 A 通道根本不进着色（decodeNormalDx 只读 r/g，z 由 sqrt 重建）。
# ---------------------------------------------------------------------------

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def png_encode_rgb(w: int, h: int, rows: list[bytes]) -> bytes:
    """RGB8 行（每行 3w 字节）→ PNG 字节。纯解析式，同输入同输出（逐字节可复现）。"""
    if len(rows) != h or any(len(r) != 3 * w for r in rows):
        raise ValueError(f"png_encode_rgb: 行尺寸不符（期望 {h}×{3 * w}）")
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8bit / RGB / 无过滤 / 非隔行
    raw = b"".join(b"\x00" + r for r in rows)             # 每行前缀 filter type 0
    return (PNG_SIG + _png_chunk(b"IHDR", ihdr)
            + _png_chunk(b"IDAT", zlib.compress(raw, 9)) + _png_chunk(b"IEND", b""))


def png_decode_rgb(blob: bytes) -> tuple[int, int, list[bytes]]:
    """**自检专用**最小 PNG 读回（校验 chunk CRC + inflate + 去过滤 → RGB 行）。

    存在的理由（AGENTS.md「判据工具自己要有自检」）：写出器一旦把 chunk 长度或 CRC 写错，
    引擎侧的症状是"纹理解码失败 → 回落中性纹理"，而回落是**静默**的（只一条 warn），
    实验会跑完并给出一个漂亮的"normal map 无影响"负结果。自检必须在生成期就把这种
    "写了但没人读到"的失败抓出来。只支持本文件写出的形态（filter 0），刻意不做通用解码器。
    """
    if blob[:8] != PNG_SIG:
        raise ValueError("png: 缺 signature")
    pos, idat, ihdr = 8, b"", None
    while pos < len(blob):
        (ln,) = struct.unpack(">I", blob[pos:pos + 4])
        tag = blob[pos + 4:pos + 8]
        data = blob[pos + 8:pos + 8 + ln]
        (crc,) = struct.unpack(">I", blob[pos + 8 + ln:pos + 12 + ln])
        if zlib.crc32(tag + data) & 0xFFFFFFFF != crc:
            raise ValueError(f"png: chunk {tag!r} CRC 不符（写出器算错）")
        if tag == b"IHDR":
            ihdr = data
        elif tag == b"IDAT":
            idat += data
        elif tag == b"IEND":
            break
        pos += 12 + ln
    if ihdr is None:
        raise ValueError("png: 缺 IHDR")
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", ihdr)
    if (depth, ctype, interlace) != (8, 2, 0):
        raise ValueError(f"png: IHDR 非 8bit/RGB/非隔行：{(depth, ctype, interlace)}")
    raw = zlib.decompress(idat)
    stride = 3 * w
    rows = []
    for y in range(h):
        ft = raw[y * (stride + 1)]
        if ft != 0:
            raise ValueError(f"png: 行 {y} 过滤类型 {ft} ≠ 0（写出器不该产生）")
        rows.append(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    return w, h, rows


# ---------------------------------------------------------------------------
# 程序化 normal map
#
# ## 约定：写 **DirectX 约定**（G = +Y 朝下），与引擎 decodeNormalDx 精确互逆
#
# 引擎（rgl/shaders/normalmap.slang）：`x = r*2-1; y = -(g*2-1); z = sqrt(saturate(1-x²-y²))`
# ⟹ 想让解码后的切线法线等于设计值 (X, Y)，必须写 `R = (1+X)/2`、`G = (1-Y)/2`（B 随意，
# 被 sqrt 重建忽略）。写反 G 的后果不是报错而是**图案上下镜像**——本 rig 的图案 Y 恒 0，
# 镜像不可见，但 sine 图案一旦引入 Y 分量就会静默反向，故此处按互逆写死并自检。
#
# ## 8bit 量化会不会污染结论（本任务最大技术风险）——三条正面回答
#
# 1. **量化步长是可算的常数**：R 通道 1 LSB = 2/255 = 0.007843（切线 x 分量），
#    在 x=0 附近折成 tilt = 0.4488°、反射方向偏 0.8976°。它**与图案幅度无关**（每个码字
#    的步长都一样），所以**一条与幅度无关的误差造不出"随幅度单调上升"的曲线**——
#    而"幅度单调上升"正是本次要复现的判据。这是主论证。
# 2. **幅度 0 是天然负对照**：flat 图案整张图只有一个码字（R=G=128），逐 texel 完全相同
#    ⟹ 量化误差是**全域恒定**的 0.225° tilt（与引擎自己的中性纹理 ensureNeutralNormalTexture
#    的 (128,128,255) 同一个值），恒定 tilt 只把整条反射带整体平移，**造不出逐像素差异**。
#    若 flat 的 speck ≈ 0 而大 amp 的 speck 显著，量化不可能是驱动源。
# 3. **图案刻意取"码字即设计"**：分块常量的 tilt 直接以 8bit 码字为设计对象（设计值先量化、
#    再由量化值反算真实 tilt 写进 manifest），所以"量化误差"在这里不是误差，是设计本身。
#    manifest 里同时记 `designedTiltDeg`（请求值）与 `quantizedTiltDeg`（码字反算值）与
#    二者之差，让 harness 用**量化后**的角度做闭式预测，而不是拿请求值去对。
#
# 缓解不了的那部分如实记：8bit 下 tilt 的**最小非零档 ≈ 0.45°**，所以幅度轴扫不到
# 「比量化还小」的区间；那个区间本来也不是 bistro 湿面的工作点（bistro 的 normal map
# 也是 8bit，同样受这条下限约束），不影响复现的等价性。
# ---------------------------------------------------------------------------

NM_PATTERNS = ("flat", "uniform", "blocks", "checker", "stripes", "noise", "sine")


def _lcg_unit(count: int, seed: int = 0x2545F491) -> list[float]:
    """确定性 [-1,1) 均匀序列（Knuth MMIX LCG，写死种子）。**不许用 random**——它的实现
    与版本相关，逐字节可复现即破（本文件头「确定性纪律」）。"""
    out, s = [], seed & 0xFFFFFFFFFFFFFFFF
    for _ in range(count):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out.append(((s >> 32) / 2.0 ** 32) * 2.0 - 1.0)
    return out


def nm_tangent_xy(px: int, py: int, amp_deg: float, block: int, pattern: str,
                  rand: list[float], texels: int) -> tuple[float, float]:
    """texel (px, py) 的**设计**切线法线 xy 分量（z 由引擎 sqrt 重建，不写）。

    tilt 只沿**切线 x**，且本场景把切线 x 摆成世界 +Z（= 视线水平投影方向 = 入射面内）。
    这不是审美选择而是**一阶敏感性**要求：N=+Y 的平面上，tilt 若垂直入射面（沿世界 +X），
    反射方向只改方位角、仰角变化是 θ 的**二阶**小量（R_y = V_y·cos2θ）；而水平窄带对
    方位角**完全不敏感**（整圈等值）⟹ 垂直入射面的 tilt 在窄带靶上几乎不产生任何信号。
    摆进入射面才有 Δ仰角 = 2θ 的一阶响应。

    图案对「切线基符号 / 手性 w」不变：blocks/noise 的 tilt 正负各半、checker/stripes 同理
    ⟹ T 反向或 w 取反只把图案镜像，统计形态不变（loader 的 uvTriangleHandedness 是从
    uv Jacobian 推 w 的，不把结论押在那条推断的方向上）。
    """
    if pattern == "flat" or amp_deg == 0.0:
        return (0.0, 0.0)
    s = math.sin(math.radians(amp_deg))
    if pattern == "uniform":       # 全图同一 tilt —— 「normal map 真的接上了吗」的阳性锚点
        return (s, 0.0)
    bx, by = px // block, py // block
    nb = texels // block
    if pattern == "blocks":        # 每 block 一个独立均匀 tilt（幅度轴 + 频率轴都干净，见场景注释）
        return (s * rand[by * nb + bx], 0.0)
    if pattern == "checker":
        return ((s if (bx + by) % 2 == 0 else -s), 0.0)
    if pattern == "stripes":
        return ((s if bx % 2 == 0 else -s), 0.0)
    if pattern == "noise":         # 逐 texel 独立（block 无意义 = 频率上限）
        return (s * rand[py * texels + px], 0.0)
    if pattern == "sine":          # 连续正弦：低频率下顶点网格也表达得出来 ⟹ 可与
        return (s * math.sin(2.0 * math.pi * px / (2.0 * block)), 0.0)
    raise ValueError(f"未知 normal map 图案 {pattern}（合法：{NM_PATTERNS}）")


def nm_encode(x: float, y: float) -> tuple[int, int, int]:
    """设计切线法线 (x, y) → DirectX 约定 8bit RGB 码字（与 decodeNormalDx 互逆）。"""
    return (max(0, min(255, int(round(127.5 * (1.0 + x))))),
            max(0, min(255, int(round(127.5 * (1.0 - y))))), 255)


def nm_decode(r8: int, g8: int) -> tuple[float, float, float]:
    """引擎 decodeNormalDx 的逐式复刻（自检用，**不是**第二套实现——它就是照 shader 抄的）。"""
    x = r8 / 255.0 * 2.0 - 1.0
    y = -(g8 / 255.0 * 2.0 - 1.0)
    z = math.sqrt(max(0.0, 1.0 - x * x - y * y))
    return (x, y, z)


def _normal_rows(texels: int, amp_deg: float, block: int, pattern: str) -> tuple[list[bytes], dict]:
    """生成 normal map 像素行 + 该图案的**量化后**事实（进 manifest，供闭式预测用）。"""
    if pattern not in NM_PATTERNS:
        raise ValueError(f"未知图案 {pattern}")
    if block < 1 or texels % block:
        raise ValueError(f"block={block} 必须整除 texels={texels}（否则边缘块不对称）")
    rand = _lcg_unit((texels // block) * (texels // block) if pattern == "blocks"
                     else texels * texels)
    rows, seen = [], {}
    for py in range(texels):
        buf = bytearray()
        for px in range(texels):
            x, y = nm_tangent_xy(px, py, amp_deg, block, pattern, rand, texels)
            c = nm_encode(x, y)
            seen[c] = seen.get(c, 0) + 1
            buf += bytes(c)
        rows.append(bytes(buf))
    # 码字 → tilt 直方图（**量化后真值**，不是请求值；见文件头量化三条回答第 3 条）。
    # 用 asin(dx)：dx 就是切线 x 分量 = **入射面内**的那一个，窄带靶只对它有一阶响应
    # （垂直入射面的分量只改方位角，二阶）。刻意**不**用 atan2(dy,dx)——tilt→0 时 dx→0，
    # atan2 会把 ±0.2° 的码字读成 ±180°（本轮实测踩过）。
    # dy 不写死为 0 而是断言它 = 中性残差：G=128 解码出的是 −0.003922 而非 0
    # （128/255·2−1 ≠ 0），引擎自己的中性纹理 ensureNeutralNormalTexture 也是这个值。
    hist: dict[float, int] = {}
    for (r, g, _b), n in seen.items():
        dx, dy, _dz = nm_decode(r, g)
        if abs(dy) > 0.004 + 1e-9:
            raise ValueError(f"图案含 y≠0 分量（dy={dy}）——本生成器不产，切线基手性会"
                             f"改变其符号，见 nm_tangent_xy")
        tilt = math.degrees(math.asin(max(-1.0, min(1.0, dx))))
        hist[round(tilt, 6)] = hist.get(round(tilt, 6), 0) + n
    tot = sum(hist.values())
    mean_sq = sum(a * a * n for a, n in hist.items()) / tot
    return rows, {
        "pattern": pattern,
        "texels": texels,
        "blockTexels": block,
        "requestedTiltDeg": amp_deg,
        "tiltDegRange": [min(hist), max(hist)],
        # rms tilt = 该图给 shading 法线带来的真实扰动强度（闭式预测用它，不用请求值）
        "tiltRmsDeg": round(math.sqrt(mean_sq), 6),
        # 量化后的 tilt 档位 → texel 数。**闭式预测必须用这张表而不是请求幅度**：
        # 8bit 下 amp=2° 只剩 ~10 档、amp=0.5° 只剩 3 档，"设计分布"与"实际分布"
        # 在小幅度端不是一回事（这条就是量化风险的全部可见形态，故如实入账）。
        "tiltLevelHistDeg": {f"{a:+.6f}": hist[a] for a in sorted(hist)},
        "distinctCodeWords": len(seen),
        # 8bit 量化的**与幅度无关**的步长（主论证：常数误差不可能造出随幅度单调的曲线）
        "quantStepDeg": round(math.degrees(math.atan2(2.0 / 255.0, 1.0)), 6),
        "encodingConvention": "DirectX (G=+Y down); exact inverse of decodeNormalDx "
                              "(rgl/shaders/normalmap.slang)",
    }


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
    # ── 透明混合管线刀3（docs/透明混合管线_plan.md §3）加的三个字段 ──────────────
    # 默认值 = 不写任何新键 ⟹ 既有七个场景的 JSON 字节全等（图像基准不破）。
    alpha: float = 1.0               # baseColorFactor[3]；< 1 才写 alphaMode="BLEND"
    unlit: bool = False              # True → 写 KHR_materials_unlit（引擎按扩展**存在性**路由）
    # node 平移。🔴 透明排序判据**必须**用它：renderListSystem 的排序代理是 entity 世界
    # 矩阵的平移列（tf.world[3]），而 loader 把 node.translation 合成进 prim.nodeWorld
    # （gltf_loader_impl 的 nodeLocalTRS）⟹ 所有 node 都 identity 时**全部图元 distanceSq
    # 相等**，stable_sort 视为已序、比较子 > 与 < 给出同一个序列 = 排序判据结构性失效。
    # 几何仍按"绕自身原点"作者，落位只靠这个平移（aabb 因此是局部量，见 manifest 注）。
    translation: tuple | None = None
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
    # ── blend_lit（债-134）加的一枚：材质名**故意**含 "Emissive"/"Foliage" 以走 material_styles
    # 连连看（透明/延迟的 vegetation、light 类别变体）。默认 False = 下面那条"名字会被静默改路由"
    # 的防御照旧生效，既有九个场景一个字节不变。
    # 🔴 为什么需要这枚旗：那条防御保护的是**闭式颜色预测**（名字改路由 ⟹ 预期值作废），而
    # blend_lit 按台账裁决**不写闭式**（透明光照的闭式要复刻 BRDF+IBL+CSM，成本 > 收益），
    # 防御的前提在这里不成立，但防御本身不能拆（blend_sort 还靠它）。⟹ 加显式 opt-in，
    # 而不是放宽条件。开了这枚 = 作者声明"我知道这个名字会换管线，且本靶不预测像素值"。
    # ⚠ 它**不**豁免 BLEND+emissive 那条（债-131）：本靶的 light 件 emissive 恒 None，
    # 走的是"名字换类别变体、自发光电平为 0"的形态，×1/4 那个未决系数根本不进算式。
    style_routed: bool = False
    # ── 纹理面（默认全关 = 既有五个场景一个字节都不变）────────────────────────
    # 三条加载器契约见文件头「TEXCOORD_0 / TANGENT / normalTexture」段（①index 是 images[]
    # 下标 ②图片必须内嵌 ③normal 走 linear 上传且会生成 mip 链）。
    uv: list | None = None           # [(u,v), ...] 与 positions 等长；None = 不写 TEXCOORD_0
    tangent: list | None = None      # [(tx,ty,tz), ...] 单位切线；None = 不写 TANGENT
    normal_png: bytes | None = None  # 内嵌 PNG（DirectX 约定）；None = 不写 normalTexture
    normal_info: dict | None = None  # 该 normal map 的设计事实（进 manifest，供闭式预测）
    normal_rows: list | None = None  # 生成期像素行（自检/直方图用，**不进 manifest**）

    def __post_init__(self):
        if self.color not in PAL.values():
            raise ValueError(f"piece {self.name}: color {self.color} not in fixed PAL")
        # 材质名连连看防御（configs/scene.lua material_styles：材质名含 "Emissive" →
        # light 风格、含 "Foliage" → 植被风格）：非自发光 piece 的名字若意外包含这两个
        # 子串，会被静默改路由、渲染到另一条管线上，piece 颜色对应的"预期值"整个作废。
        # style_routed=True = 作者显式声明这次路由是设计（见字段注）。
        if (self.emissive is None and not self.style_routed
                and ("Emissive" in self.name or "Foliage" in self.name)):
            raise ValueError(f"piece {self.name}: name would be style-routed by "
                             f"material_styles pattern; rename it")
        # 反向对账（阳性锚点，缺则 style_routed 变成"关掉防御的万能钥匙"）：开了旗却名字
        # 不含任何 pattern ⟹ 防御被白关了一次而路由没换，作者意图与产物不符。
        if self.style_routed and not ("Emissive" in self.name or "Foliage" in self.name):
            raise ValueError(f"piece {self.name}: style_routed=True 但名字不含 "
                             f"Emissive/Foliage —— 这枚旗的唯一作用是声明"
                             f"「这次静默改路由是设计」，名字不命中 pattern 就是白关防御")
        # 透明混合管线刀3 加的两条构建期闸门（默认值下两条恒不触发 = 既有场景零影响）：
        if not 0.0 < self.alpha <= 1.0:
            raise ValueError(f"piece {self.name}: alpha {self.alpha} 不在 (0,1]")
        if self.translation is not None and len(self.translation) != 3:
            raise ValueError(f"piece {self.name}: translation 须是 3 个数")
        # 🔴 自发光电平口径未定案（债-131：光源类 ×1/4 的补偿前提已失效）⟹ 带 emissive 的
        # BLEND piece 的闭式预测会吃这个未决系数，等于把靶盖在一个待裁的公式上。要用就先裁债。
        if self.alpha < 1.0 and self.emissive is not None:
            raise ValueError(f"piece {self.name}: BLEND + emissive 的闭式预测依赖光源类 ×1/4，"
                             f"而该系数的口径待裁（债-131）——先结案再造这个靶")
        # 纹理面自洽（**阳性锚点**：normal_png 而 uv=None 时引擎 uploadModelTextures 会
        # `if (!prim.hasUv) continue;` 静默回落中性纹理，实验跑完给出假的"normal 无影响"）
        n = len(self.geom.positions)
        if self.uv is not None and len(self.uv) != n:
            raise ValueError(f"piece {self.name}: uv count {len(self.uv)} != vertex count {n}")
        if self.tangent is not None and len(self.tangent) != n:
            raise ValueError(f"piece {self.name}: tangent count != vertex count")
        if self.normal_png is not None and self.uv is None:
            raise ValueError(f"piece {self.name}: normal_png 但无 uv —— 引擎会静默回落"
                             f"中性纹理（uploadModelTextures 的 !hasUv 早退），拒绝生成")


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
    # 参数化场景（nm_rig 一族）用同一 Scene 类产出多个文件；None = 用 name 当文件名。
    # 刻意与 name 分开：manifest 里的 scene 字段要能带上参数（自描述），文件名也要，
    # 但 SCENES 注册表要求 name 是稳定键。
    out_name: str | None = None


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
# 场景 4：izb_rig —— FTIZB/IZB 硬阴影 spike 装置（docs/hfts_izb_plan.md 批 1a）
# ---------------------------------------------------------------------------
# 设计哲学与 shadow_rig 同构：判据「边界免疫」。CSM 级联 texel 尺寸是运行时自适应量
# （near/far 随相机与包围球漂移），manifest 里预测"这根杆必亚 texel"= 会绿 = 无效断言。
# 所以本 rig 给的是**四组探针 + A/B 对拍**（shadow.lua 的 izb 关=CSM / 开=IZB，同机位同帧），
# 让图像回答「细影去哪了」，构建期只断言几何事实（包络/相交/三分区存在性）。
#
# 四组探针（逐一对应 FTIZB 能力点）：
#   a) 厚度梯 bar_thick/mid/thin/hair（截面 0.4/0.15/0.05/0.02 m，均 6 m 长、均 y=2.0）：
#      同高同长只差截面。粗档 = 参照（任何路线都对），细档 = CSM 走样/丢失区，
#      IZB 卖点 = 任意距离画锐。ladder 机位一排同框对比。
#   b) 接触探柱 post_contact：截面 0.04 m 细柱**立在地面**（底面 y 精确 = 0），影从
#      根部连续过渡到 h·off 偏移端——peter-panning / 漏光缝 / 双黑带的检测器。
#   c) 穿插探杆 bar_mounted：X 向 0.3 m 方杆在 y=4.5 穿过主立柱，其影带横穿立柱影带
#      ——交叠线 = 两独立遮挡体逐 texel 求交的探针（IZB 链表 ≥2 节点 + 可见性 AND）。
#   d) 叠影探杆 stack_pair_lo/hi（平行 X 向 0.1 m 杆，y=1.0 / 2.5）：两条影带在地面
#      错落成「受光 | 单叠 | 双叠」三分带。硬阴影下双叠=单叠（二值），该区现在的价值
#      是 IZB 链表长度 ≥2 的**代码路径覆盖阳性锚点**；PCSS 时代（阶段 3）它变成半影
#      过渡的视觉判据。两杆互不遮挡（构建期断言：互相不在对方的 toward-light 推导图内）。
#
# 浮动杆（无物理支撑）——QA 装置非场景美术，honesty 声明。
# 构建期断言集（A1–A8，见函数体内注释）：包络被地面覆盖 / 包络两两不相交（除声明对，
# 且声明对**必须真相交**——排除集不许是摆设）/ 影不落他人身上 / 穿插为真 / 触地为真 /
# 梯子公平性（同高同长、截面严格递减）/ 三分区各 ≥4cm 可测 / 叠影杆自身受光。

GROUND_HALF_IZB = 60.0

def _slab_shadow_band_z(rect, y_bottom: float, y_top: float):
    """水平 X 向薄杆（rect = XZ footprint，底 y_bottom 顶 y_top）的地面影带 z 区间。
    方向光投影：地面 z = z_obj + SHADOW_OFF.z · y_obj（z_obj ∈ rect z 区间，取两端）。"""
    return (rect[2] + SHADOW_OFF[1] * y_top, rect[3] + SHADOW_OFF[1] * y_bottom)


def _slab_shadow_band_x(rect, y_bottom: float, y_top: float):
    """X 向薄杆地面影带 x 区间（与 z 版同一投影式：地面 x = x_obj + SHADOW_OFF.x · y_obj）。"""
    return (rect[0] + SHADOW_OFF[0] * y_bottom, rect[1] + SHADOW_OFF[0] * y_top)


def _toward_light_hull(rect, dy: float):
    """rect 沿「朝光方向」抬升 dy 后的 XZ 位置（遮挡测试用：A 顶向光推 dy 会不会够到 B）。
    光源方向 (-2/3, +1, +3/5) 每单位 y：x 减 2/3、z 加 3/5。"""
    dx, dz = -SHADOW_OFF[0] * dy, -SHADOW_OFF[1] * dy
    return (rect[0] + dx, rect[1] + dx, rect[2] + dz, rect[3] + dz)


def scene_izb_rig() -> Scene:
    s = Scene(
        name="izb_rig",
        purpose="FTIZB/IZB 硬阴影 spike 装置：厚度梯（0.4→0.02 m）+ 接触阴影 + 穿插影线 + 叠影链表（docs/hfts_izb_plan.md 批 1a）",
        honesty=("浮动杆无物理支撑（QA 装置，非美术）。影带坐标是按默认光向的**几何设计值**，"
                 "不是渲染逐像素契约——CSM 级联 texel 尺寸运行时自适应，正确用法 = shadow.lua "
                 "的 izb 开关 A/B 对拍 + imgqa 影响域（域外恰为 0），不是拿预测坐标逐像素比。"
                 "锚点前提 = 默认光向（scene.lua SUN）；RPC 改光后一切坐标须按新光向重算。"),
        cameras={
            "hero": {"position": [0.0, 8.0, 34.0], "target": [0.0, 1.0, 0.0],
                     "fovDegrees": 55.0,
                     "note": "立柱组全景：穿插杆 + 叠影对 + 接触柱 + 梯子前两档"},
            "ladder": {"position": [4.0, 5.0, 20.0], "target": [40.0, 0.5, 6.0],
                       "fovDegrees": 60.0,
                       "note": "厚度梯对比取景：4 根同长同高、截面 0.4→0.02，影全落受光地面"},
            "contact": {"position": [-13.5, 1.0, 9.5], "target": [-14.0, 0.4, 6.0],
                        "fovDegrees": 40.0,
                        "note": "接触阴影特写：柱根影连续性（peter-panning / 漏光缝探针）"},
            "stack_zoom": {"position": [14.0, 4.0, 12.5], "target": [14.0, 0.0, 9.45],
                           "fovDegrees": 45.0,
                           "note": "叠影三分带俯视（带坐标设计值见 stackDesign）"},
        },
        cascade_camera=None,  # 场景仅数十米，跨级联不是本 rig 的断言对象（那是 shadow_rig 职责）
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（全部影位移判据的前提）"},
            "shadowOffsetPerMeterOfHeight": [SHADOW_OFF[0], SHADOW_OFF[1]],
            "thicknessLadder": [
                {"name": "bar_thick", "sectionMeters": 0.4, "heightMeters": 2.0},
                {"name": "bar_mid", "sectionMeters": 0.15, "heightMeters": 2.0},
                {"name": "bar_thin", "sectionMeters": 0.05, "heightMeters": 2.0},
                {"name": "bar_hair", "sectionMeters": 0.02, "heightMeters": 2.0},
            ],
            "stackDesign": {
                "note": ("两平行杆影带的 z 区间设计值（构建期精确算出，进 manifest 供 harness "
                         "定位三分带；运行时 CSM 会量化/走样它们——那正是要 A/B 看的）"),
                "bandLo": None,   # 构建期回填
                "bandHi": None,   # 构建期回填
            },
            "declaredOverlaps": [["pillar", "bar_mounted"], ["stack_pair_lo", "stack_pair_hi"]],
            "note": "高度 h 的顶点影落点 = 基座 + h*(2/3, -3/5)（+X,-Z）；浮动杆影 = footprint 平移，"
                    "立柱/触地柱影 = 包络（footprint ∪ 平移 footprint）",
        },
    )
    P = s.pieces.append
    gh = GROUND_HALF_IZB

    P(Piece("ground", quad((-gh, 0.0, -gh), (0.0, 0.0, 2 * gh), (2 * gh, 0.0, 0.0)),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="公共影接收面（亮黄非近白，同材质影/光比口径）；四组探针的影全部落在其上"))
    P(Piece("pillar", cylinder(0.0, 0.0, 0.0, 0.5, 6.0, 24),
            PAL["red"], roughness=0.7,
            intent="主遮挡柱 r=0.5 h=6：曲面遮挡体 + 大影包络基线；bar_mounted 的穿插对象"))
    P(Piece("bar_mounted", box(0.0, 4.5, 0.0, 4.0, 0.3, 0.3),
            PAL["blue"], roughness=0.6,
            intent="穿插探杆：X 向 4 m 方杆 y=4.5 穿过主柱（构建期断言真相交）；"
                   "其影带横穿立柱影带 = 两遮挡体影交叠线（IZB 逐 texel 求交探针）"))

    # 厚度梯：同 y=2.0 同 6 m 长，截面 0.4→0.02 严格递减（构建期断言公平性）。
    # 间距 ≥6 m：影包络两两分离（构建期断言），ladder 机位一眼排开。
    ladder = [
        ("bar_thick", 12.0, 0.4, PAL["orange"], "X"),
        ("bar_mid", 22.0, 0.15, PAL["green"], "Z"),
        ("bar_thin", 32.0, 0.05, PAL["cyan"], "X"),
        ("bar_hair", 42.0, 0.02, PAL["magenta"], "X"),
    ]
    for name, xc, sec, color, axis in ladder:
        sx, sz = (6.0, sec) if axis == "X" else (sec, 6.0)
        P(Piece(name, box(xc, 2.0, 6.0, sx, sec, sz), color, roughness=0.6,
                intent=f"厚度梯档 截面={sec} m（{'沿X' if axis == 'X' else '沿Z'}）：影宽 = 截面，"
                       f"细档 CSM 走样/丢失区，IZB 必须画锐；粗档=参照档"))

    P(Piece("post_contact", box(-14.0, 1.25, 6.0, 0.04, 2.5, 0.04),
            PAL["azure"], roughness=0.6,
            intent="接触探柱：截面 0.04 m 立在地面（底 y 精确=0，构建期断言）；根部影必须"
                   "与柱连续——peter-panning / 漏光缝 / 接触双黑带的阳性探针"))

    # 叠影对：0.1 m 截面，y=1.0 / 2.5，z 错开使两影带部分重叠成三分带。
    P(Piece("stack_pair_lo", box(14.0, 1.0, 10.0, 6.0, 0.1, 0.1),
            PAL["purple"], roughness=0.6,
            intent="叠影探杆·低（y=1.0）：与 high 的影带组成 受光|单叠|双叠 三分带；"
                   "双叠区 = IZB 链表长度 ≥2 的代码路径覆盖锚点（构建期断言两杆互不遮挡）"))
    P(Piece("stack_pair_hi", box(14.0, 2.5, 11.0, 6.0, 0.1, 0.1),
            PAL["chartreuse"], roughness=0.6,
            intent="叠影探杆·高（y=2.5）：高杆影带前移 1.5 m 与低杆影带部分重叠（三分带）"))

    # ---- 构建期断言（A1–A8）：口头承诺不算数，坏了构建失败 ----------------
    # footprint / 包络高度全部**从几何本体推导**（不手抄坐标表——手抄表与几何是双份
    # 真相源，改几何忘改表 = 断言静默空转）。包络 = footprint ∪ footprint+off·h_top
    # （保守上界：立柱/触地柱的真实影即此；浮动杆真实影更窄，用包络判「不相交」只会
    # 过于严格不会漏报，安全方向正确）。
    ground_rect = (-gh, gh, -gh, gh)
    declared = {frozenset(d) for d in s.extra["declaredOverlaps"]}
    rects, hull_h, yext = {}, {}, {}
    for p in s.pieces:
        if p.name == "ground":
            continue
        pos = p.geom.positions
        rects[p.name] = (min(v[0] for v in pos), max(v[0] for v in pos),
                         min(v[2] for v in pos), max(v[2] for v in pos))
        yext[p.name] = (min(v[1] for v in pos), max(v[1] for v in pos))
        hull_h[p.name] = yext[p.name][1]
    hulls = {k: _rect_hull(rects[k], hull_h[k]) for k in rects}

    # A1 全部保守包络被地面覆盖
    for k, hull in hulls.items():
        assert _rect_contains(ground_rect, hull), f"izb_rig: {k} 影包络 {hull} 出地面"
    # A2 包络两两不相交，除声明重叠对；且声明对必须真相交（排除集不许空转）
    names = sorted(rects)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            overlap = _rect_overlap(hulls[a], hulls[b])
            if frozenset((a, b)) in declared:
                assert overlap, f"izb_rig: 声明重叠对 ({a},{b}) 实际不相交——探针设计失效"
            else:
                assert not overlap, f"izb_rig: ({a},{b}) 影包络意外相交（对拍会被污染）"
    # A3 任何遮挡体 footprint 不落他人影包络内（声明对豁免）——影只落在裸地面
    for a in names:
        for b in names:
            if a != b and frozenset((a, b)) not in declared:
                assert not _rect_overlap(hulls[a], rects[b]), \
                    f"izb_rig: {a} 的影包络压上 {b} 本体"
    # A4 穿插为真：bar_mounted footprint 与柱 footprint 相交，且 y 区间真在柱身内（几何实测）
    assert _rect_overlap(rects["pillar"], rects["bar_mounted"]), "izb_rig: bar_mounted 没穿柱"
    barys = [v[1] for v in next(p for p in s.pieces if p.name == "bar_mounted").geom.positions]
    assert 0.0 < min(barys) and max(barys) < 6.0, "izb_rig: bar_mounted 高度超出柱身"
    # A5 触地为真：post_contact 底面 y 精确 = 0
    post = next(p for p in s.pieces if p.name == "post_contact")
    ymin = min(v[1] for v in post.geom.positions)
    assert ymin == 0.0, f"izb_rig: post_contact 底面 y={ymin} 非精确接地"
    # A6 梯子公平性：同高（bbox y 中心 = 2.0）+ 截面严格递减（从几何实测，不是口头常量）
    secs = [sec for _, _, sec, _, _ in ladder]
    assert all(secs[i] > secs[i + 1] for i in range(len(secs) - 1)), "izb_rig: 截面非严格递减"
    for name, _, _, _, _ in ladder:
        ys = [v[1] for v in next(p for p in s.pieces if p.name == name).geom.positions]
        assert abs((min(ys) + max(ys)) / 2.0 - 2.0) < 1e-9, f"izb_rig: {name} 不在 y=2.0 平面"
    # A7 叠影三分带存在且各 ≥4 cm（可测性下限；<4 cm 时 1080p 特写机位 <10 px，判据噪声吞信号）
    band_lo = _slab_shadow_band_z(rects["stack_pair_lo"], *yext["stack_pair_lo"])
    band_hi = _slab_shadow_band_z(rects["stack_pair_hi"], *yext["stack_pair_hi"])
    lo_only = (band_lo[0], min(band_hi[0], band_lo[1]))
    both = (max(band_lo[0], band_hi[0]), min(band_lo[1], band_hi[1]))
    hi_only = (max(band_lo[1], band_hi[0]), band_hi[1])
    for label, (z0, z1) in (("lo_only", lo_only), ("both", both), ("hi_only", hi_only)):
        assert z1 - z0 >= 0.04, \
            f"izb_rig: 三分带 {label} 宽 {z1 - z0:.3f} m < 0.04 m（不可测）"
    # A8 叠影杆自身受光：两杆互相不在对方的 toward-light 推导图内；柱也够不到它们
    assert not _rect_overlap(_toward_light_hull(
        rects["stack_pair_lo"],
        yext["stack_pair_hi"][0] - yext["stack_pair_lo"][1]),
        rects["stack_pair_hi"]), "izb_rig: lo 遮住了 hi（三分区不成立）"
    for k in ("stack_pair_lo", "stack_pair_hi"):
        pillar_reach = _toward_light_hull(rects[k], yext["pillar"][1] - hull_h[k])
        assert not _rect_overlap(pillar_reach, rects["pillar"]), \
            f"izb_rig: {k} 在柱影里（叠影区不在受光地面上）"

    s.extra["stackDesign"]["bandLo"] = [band_lo[0], band_lo[1]]
    s.extra["stackDesign"]["bandHi"] = [band_hi[0], band_hi[1]]
    xband_lo = _slab_shadow_band_x(rects["stack_pair_lo"], *yext["stack_pair_lo"])
    xband_hi = _slab_shadow_band_x(rects["stack_pair_hi"], *yext["stack_pair_hi"])
    x_span = (max(xband_lo[0], xband_hi[0]), min(xband_lo[1], xband_hi[1]))
    assert x_span[1] - x_span[0] >= 1.0, \
        f"izb_rig: 双叠 cell x 交叠宽 {x_span[1] - x_span[0]:.2f} m < 1 m（样本不足）"
    s.extra["stackDesign"]["zones"] = {
        "lo_only_z": [lo_only[0], lo_only[1]],
        "double_stack_z": [both[0], both[1]],
        "hi_only_z": [hi_only[0], hi_only[1]],
        "x_span": [x_span[0], x_span[1]],  # 两杆影带 x 区间之交（构建期由几何计算）
    }
    return s


# ---------------------------------------------------------------------------
# 场景 6：nm_rig —— 带 normal map 的受控靶（本轮加，复现 bistro 湿面高亮噪点）
# ---------------------------------------------------------------------------
# ## 为什么必须新造一个靶，而不是在 env_band 上加纹理
#
# env_band 的 honesty 已把那条构造性局限写进文件：本引擎无 normal map 时
# `geoN = worldNormal(M, normalize(input.normal))`、`N = geoN`
# （rgl/shaders/deferred_geometry.frag.slang，原 forward 的 model_pbr_ibl.frag 已随刀②.2 删除）
# ⟹ **shading 法线与 specular AA 的输入
# 是同一个量**。而 specular AA（normalFiltering）在平坦面上 ddx/ddy(geoN) ≈ 0 ⟹ 零增强。
# 于是"法线高频变化而 specular AA 完全不察觉"这一类噪点**在没有 normal map 的靶上
# 构造不出来**——不是没测到，是不存在。nm_rig 的唯一职责就是把这条轴变成可扫的自变量。
#
# ## 形态：单材质单 quad 整块地面，一个配置一个进程
#
# 刻意**不**学 env_band 的四条并排 roughness 带。三条理由：
#   ① 带边界是地面上 x=常数的直线、平行于视线 ⟹ 画面里收敛到灭点，矩形窗必然串带
#      （env_band 那一轮为此专门写了 band_mask，可用像素减半）；
#   ② 四档颜色不同而 metallic=1 时 baseColor 就是金属染色 ⟹ 四档亮度标尺天然不同
#      （env_band 的头注把这条列为"唯一已知混淆项"）；
#   ③ 单材质整块 ⟹ 平坦面 N=+Y 的反射仰角只依赖视线仰角、视线仰角只依赖画面行号
#      ⟹ **amp=0 时画面逐行不变**，整窗都是可用像素，"该是多少"有闭式解。
# 代价如实记：一次进程只出一个数，扫 6 档幅度要起 6 个进程（约 6×40 s）。可接受。
#
# ## 相机为什么是俯角 25°（而不是 env_band 的掠射 3°）
#
# 俯角 25° + fov_v 45° ⟹ 画面仰角范围 [−2.5°, −47.5°]，**全画幅皆地面、地平线在画外**。
# 三条硬收益：
#   ① deferred 路线可用（画幅里没有天穹像素 ⟹ light.frag 的天穹分支根本不被触发，
#      它与 env.intensity 的口径关系怎么定都不影响本靶；2026-10-18 起 intensity=0 连天穹
#      一起关，定案见 docs/archive/图变体收敛_plan.md §6.2）；
#   ② 可用像素从 env_band 的 49 行涨到整窗；
#   ③ 不需要掩膜，也不需要原 forward 的背景天穹开关（environment.skybox 键已随刀②.2 删除）。
# 窄带的镜像线 = 反射仰角等于带仰角的那一行；带仰角取 25°（= 俯角）⟹ 镜像线落在主点行。
#
# ## 两条自变量为什么"干净"（这是本 rig 全部的设计价值）
#
# · **幅度 amp**：每 texel 的 tilt 幅度。amp=0 时整张图只有一个码字（R=G=128）
#   ⟹ 逐 texel 完全相同 ⟹ 量化误差是全域恒定的 0.225° tilt（与引擎自己的中性纹理
#   ensureNeutralNormalTexture 同值），恒定 tilt 只把反射带整体平移，**造不出逐像素差异**
#   ⟹ 天然负对照。
# · **频率 block**：tilt 场的块大小（texel）。关键在 **mip level 只由 UV 的屏幕导数决定、
#   与纹理内容无关** ⟹ 固定 uv_scale 扫 block 时，纹理过滤强度**逐位不变**，变的只有
#   内容频率。这是"频率"这条轴能做到单变量的唯一原因（若改 uv_scale 则同时改了过滤，
#   那是另一条轴，见 probe.texelDensityAtMirrorRow 的说明）。
# · 图案取 **blocks**（每块一个独立均匀 tilt，均匀分布）而不是 ±amp 两态：两态图案的
#   "亮像素总数"与 amp 无关（每个 tilt 值都恰好映射到一条亮行），只有亮行的**位置**随
#   amp 分开 ⟹ speck 对 amp 近似平坦，测不出单调性。宽分布才有 0 → 饱和的单调上升。
#
# ## 构建期断言（A1–A5）
# A1 全画幅皆地面（地平线在画外）；A2 窄带镜像线在画面内且离上下边有裕量；
# A3 地面在镜像线处铺满整个可见横向范围；A4 幅度/频率两轴的取值合法性（block 整除
#    texels、amp 在 8bit 可表达范围内）；A5 单变量纪律（单 piece、metallic=1、法线恒 +Y、
#    几何严格平坦）。

NM_GROUND_HALF = 100.0      # 地面半边长（m）
NM_UV_SCALE = 0.30          # 每纹理重复 = 0.30 m（texels=64 ⟹ texel = 4.69 mm）
NM_TEXELS = 64
NM_CAM_H = 1.0              # 相机离地高度（m）
NM_PITCH_DEG = 25.0         # 俯角 = env 带子仰角（度）
NM_FOV_V = 45.0


def nm_ray_dir(px: int, py: int, w: int, h: int, fov_v: float, pitch_deg: float):
    """像素 (px,py) 的世界空间视线方向（glm::perspective 约定 + 零 roll 俯角）。

    ndc_y = 1 − 2(y+0.5)/H（y=0 是顶行）；tan θ_v = ndc_y·tan(fov/2)、
    tan θ_h = ndc_x·tan(fov/2)·(W/H)（垂直 fov + 宽高比，与已归档的 env_band_probe 同一约定）。
    dir = cos θ_v·(cos θ_h·f̂ + sin θ_h·r̂) + sin θ_v·û，
    f̂=(0,−sin p,−cos p)、r̂=(1,0,0)、û=(0,cos p,−sin p)。
    """
    ndc_x = 2.0 * (px + 0.5) / w - 1.0
    ndc_y = 1.0 - 2.0 * (py + 0.5) / h
    t = math.tan(math.radians(fov_v / 2.0))
    th = math.atan(ndc_x * t * (w / h))
    tv = math.atan(ndc_y * t)
    p = math.radians(pitch_deg)
    ct, st = math.cos(tv), math.sin(tv)
    ch, sh = math.cos(th), math.sin(th)
    sp, cp = math.sin(p), math.cos(p)
    vx = ct * sh
    vy = ct * ch * (-sp) + st * cp
    vz = ct * ch * (-cp) + st * (-sp)
    n = math.sqrt(vx * vx + vy * vy + vz * vz)
    return (vx / n, vy / n, vz / n)


def nm_ground_uv(px: int, py: int, w: int, h: int, fov_v: float, pitch_deg: float,
                 cam_h: float, half: float, scale: float):
    """像素 → 地面 UV（None = 该像素打不到地面）。UV 约定：u 沿世界 +Z、v 沿世界 +X。"""
    d = nm_ray_dir(px, py, w, h, fov_v, pitch_deg)
    if d[1] >= -1e-12:
        return None
    t = cam_h / -d[1]
    x, z = d[0] * t, d[2] * t
    return ((z + half) / scale, (x + half) / scale)


def nm_rig_probe(w: int = 1280, h: int = 720, fov_v: float = NM_FOV_V,
                 pitch_deg: float = NM_PITCH_DEG, cam_h: float = NM_CAM_H,
                 half: float = NM_GROUND_HALF, scale: float = NM_UV_SCALE,
                 texels: int = NM_TEXELS) -> dict:
    """闭式取景事实：仰角范围、镜像行、镜像行处的 texel 屏幕密度与 mip 估计。

    texel 密度是**数值差分**算的（∂UV/∂pixel），不抄解析式——解析式在"俯仰 + 透视 +
    宽高比"三处任一抄错都会让 mip 预测偏一档，而 mip 档位恰恰是"高频被平均掉了没有"
    这条判据的命门。
    """
    top = nm_ray_dir(w // 2, 0, w, h, fov_v, pitch_deg)
    bot = nm_ray_dir(w // 2, h - 1, w, h, fov_v, pitch_deg)
    elev = lambda d: math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
    mirror = h / 2.0 - 0.5                       # 反射仰角 = −视线仰角 = 带仰角 ⟹ 主点行
    row = int(round(mirror))
    u0 = nm_ground_uv(w // 2, row, w, h, fov_v, pitch_deg, cam_h, half, scale)
    ur = nm_ground_uv(w // 2 + 1, row, w, h, fov_v, pitch_deg, cam_h, half, scale)
    ud = nm_ground_uv(w // 2, row + 1, w, h, fov_v, pitch_deg, cam_h, half, scale)
    rho_x = max(abs(ur[0] - u0[0]), abs(ur[1] - u0[1])) * texels
    rho_y = max(abs(ud[0] - u0[0]), abs(ud[1] - u0[1])) * texels
    rho = max(rho_x, rho_y)
    d_mirror = cam_h / math.tan(math.radians(pitch_deg))
    tan_h = math.tan(math.radians(fov_v / 2.0)) * (w / h)
    return {
        "frameWidth": w, "frameHeight": h,
        "viewElevDegTop": round(elev(top), 4), "viewElevDegBottom": round(elev(bot), 4),
        "horizonInFrame": elev(top) > 0.0,
        "mirrorRow": mirror,
        "mirrorDistanceM": round(d_mirror, 4),
        "pxPerDegElevation": round(h / fov_v, 4),
        "texelsPerPixelAtMirrorRow": round(rho, 4),
        "mipAtMirrorRow": round(math.log2(max(rho, 1e-9)), 3),
        "texelWorldSizeM": round(scale / texels, 6),
        "visibleHalfWidthAtMirrorRowM": round(d_mirror * tan_h, 3),
    }


def scene_nm_rig(amp_deg: float = 2.0, block: int = 1, pattern: str = "blocks",
                 texels: int = NM_TEXELS, uv_scale: float = NM_UV_SCALE,
                 roughness: float = 0.12, pitch_deg: float = NM_PITCH_DEG) -> Scene:
    """单材质整块金属地面 + 程序化 normal map。参数即自变量，其余全部归零（见配置）。"""
    g = NM_GROUND_HALF
    # 顶点序与 env_band 的 quad() 同构，但这里要逐顶点带 uv/tangent，故手搓 4 顶点。
    # 绕序：cross(u_vec, v_vec) = cross(+Z, +X) = +Y（与 quad() 同一判据），
    # 顶点序 p0 → p0+u → p0+u+v → p0+v、索引 [0,1,2, 0,2,3]。
    p0, uv_, tan_ = (-g, 0.0, -g), [], []
    verts = [(-g, 0.0, -g), (-g, 0.0, g), (g, 0.0, g), (g, 0.0, -g)]
    for (x, _y, z) in verts:
        uv_.append(((z + g) / uv_scale, (x + g) / uv_scale))   # u 沿 +Z = 入射面内
        tan_.append((0.0, 0.0, 1.0))                          # T = +Z（单位，自检查）
    geom = Geom(verts, [(0.0, 1.0, 0.0)] * 4, [0, 1, 2, 0, 2, 3])

    rows, info = _normal_rows(texels, amp_deg, block, pattern)
    png = png_encode_rgb(texels, texels, rows)
    probe = nm_rig_probe(scale=uv_scale, texels=texels, pitch_deg=pitch_deg)

    tag = (f"a{amp_deg:05.2f}_b{block:03d}_r{roughness:.2f}_{pattern}"
           f"_t{texels}_s{uv_scale:05.3f}")
    s = Scene(
        name=f"nm_rig_{tag}",
        purpose=(f"normal map 受控靶：metallic=1 平坦地面 + {pattern} 图案"
                 f"（幅度 {amp_deg}°、块 {block} texel、roughness {roughness}），"
                 f"配程序化窄带 env（仰角 {pitch_deg}°）"),
        honesty=("地面几何**完全平坦**（法线恒 +Y）是刻意的：specular AA（normalFiltering）"
                 "喂的是几何法线，平坦面上 ddx/ddy ≈ 0 ⟹ 零增强，这正是本靶要利用的性质"
                 "而不是要修的 bug。因此本靶的结论只覆盖『法线高频来自 normal map 而几何法线"
                 "恒定』这一类；几何本身起伏（geoN 也高频）那一类由 A5 断言排除在外。"
                 "另：baseColor 取调色板 yellow（PAL 固定表里没有中性灰，改 PAL = 动全表判据），"
                 "metallic=1 时它是金属染色 ⟹ 亮度标尺带 yellow 的 BT.601 权重 0.886，"
                 "跨颜色对比时须按实测平台归一。"),
        cameras={
            "topdown25": {"position": [0.0, NM_CAM_H, 0.0],
                          "target": [0.0, NM_CAM_H - 10.0 * math.sin(math.radians(pitch_deg)),
                                     -10.0 * math.cos(math.radians(pitch_deg))],
                          "fovDegrees": NM_FOV_V},
        },
        out_name=f"nm_rig_{tag}",
    )
    s.pieces.append(Piece(
        "metal_ground", geom, PAL["yellow"], metallic=1.0, roughness=roughness,
        radial=False, closed=False,
        intent=(f"唯一 piece：metallic=1 平坦地面（法线恒 +Y ⟹ specular AA 零增强）+ "
                f"normal map {pattern} amp={amp_deg}° block={block}"),
        uv=uv_, tangent=tan_, normal_png=png, normal_info=info, normal_rows=rows))

    s.extra["probe"] = probe
    s.extra["normalMap"] = info
    s.extra["uvScaleMetersPerRepeat"] = uv_scale
    s.extra["tangentBasis"] = {
        "tangentXYZ": [0.0, 0.0, 1.0],
        "why": "T 必须落在**入射面内**（世界 +Z = 视线水平投影）。垂直入射面的 tilt 只改"
               "反射方位角、仰角只随 θ 二阶变化（R_y = V_y·cos2θ），而水平窄带对方位角"
               "不敏感 ⟹ 摆错方向则窄带靶对 tilt 几乎无响应（一阶信号为零）。",
        "handedness": "uvTriangleHandedness 从 uv Jacobian 推 w；本布局 det>0 ⟹ w=+1，"
                      "B = cross(N,T)·w = +X = ∂P/∂v（自洽右基）",
    }
    s.extra["envRecipe"] = {
        # 生成脚本已随图变体收敛刀②.2 归档（git history @ < 2026-10-18 的该文件）
        "tool": "tools/env_band_hdr.py（已归档）",
        "args": f"--elev {pitch_deg} --band <W> --intensity <RAD>",
        "note": f"带子仰角必须 == 相机俯角 {pitch_deg}°，否则镜像线跑出主点行 "
                f"（harness 仍按实测行定位）",
    }
    # ── 构建期断言 ────────────────────────────────────────────────────────
    # A1 全画幅皆地面：顶行视线仰角必须 < 0（地平线在画外），否则 deferred 的天穹
    #    会进来污染（它不乘 env.intensity，"env 关掉归零"那行实验永远不归零）。
    assert not probe["horizonInFrame"], \
        f"A1 地平线在画内（顶行仰角 {probe['viewElevDegTop']}°）——换更陡的俯角"
    # A2 镜像线在画面内且离上下边 ≥ 15% 画高
    hh = probe["frameHeight"]
    assert 0.15 * hh <= probe["mirrorRow"] <= 0.85 * hh, f"A2 镜像行 {probe['mirrorRow']} 出画"
    # A3 地面在镜像线处铺满整个可见横向范围
    assert g > probe["visibleHalfWidthAtMirrorRowM"], \
        f"A3 地面半宽 {g} m < 镜像线处可见半宽 {probe['visibleHalfWidthAtMirrorRowM']} m"
    # A4 两轴取值合法
    assert texels % block == 0 and block >= 1, f"A4 block={block} 不整除 texels={texels}"
    assert 0.0 <= amp_deg < 89.0, f"A4 amp={amp_deg}° 越界"
    assert info["distinctCodeWords"] >= 1
    # 负对照判据按**是否真的平坦**分档（amp=0 时任何图案都退化成单码字，判据同 flat；
    # 按图案名分档会让 amp=0 的 blocks 落进"必须有 tilt"那一支而必然误炸）
    if amp_deg == 0.0 or pattern == "flat":
        assert info["distinctCodeWords"] == 1, "A4 负对照必须只有一个码字（负对照的前提）"
        assert abs(info["tiltRmsDeg"]) < 0.3, \
            f"A4 负对照 tilt rms {info['tiltRmsDeg']}° 过大（造不出逐像素差异这一条不成立）"
    else:
        assert info["tiltRmsDeg"] > 0.3, \
            (f"A4 {pattern} amp={amp_deg}° 的 tilt rms 仅 {info['tiltRmsDeg']}°——"
             f"8bit 量化把图案吃掉了（码字数 {info['distinctCodeWords']}），"
             f"这一档不可用，把 amp 抬到 rms ≥ 0.5° 以上再测")
    # A5 单变量纪律：单 piece、metallic=1、几何严格平坦、法线恒 +Y
    assert len(s.pieces) == 1
    p = s.pieces[0]
    assert p.metallic == 1.0 and {v[1] for v in geom.positions} == {0.0}
    assert {tuple(round(c, 3) for c in n) for n in geom.normals} == {(0.0, 1.0, 0.0)}
    assert all(abs(t[0]) < 1e-9 and abs(t[1]) < 1e-9 and abs(t[2] - 1.0) < 1e-9
               for t in p.tangent), "A5 切线必须恒 +Z"
    return s


# ---------------------------------------------------------------------------
# 场景 6：penumbra_ladder —— 半影宽度随「遮挡体↔接收面间隙 h」的变化曲线
# ---------------------------------------------------------------------------
# 为什么需要它（债-17 PCSS 冻结件处置缺的那个数）：PCSS 相对 CSM 唯一**结构性**的能力差异
# = 半影随间隙变宽（接触硬化：贴地的接触影硬、高处的投影软）。CSM 的 4 抽头 PCF 核宽绑的是
# **阴影贴图纹素**，与 h 无关 ⟹ 它在原理上给不出这条曲线。
#
# 为什么现有靶都量不出来：shadow_rig 的遮挡体**全坐在地上**（h≈0）⟹ 半影恒等于核宽，梯子
# 退化成一点；bistro 上高处投影确实有，但**间隙与级联同时变**（越高的遮挡体离相机越远 ⟹ 落进
# 越粗的级联），两个自变量搅在一起，测出来分不清是 PCSS 的功劳还是 texel 变粗。本场景的设计
# 约束就一条：**只让 h 变，其余全固定**——所有板同尺寸、同 u 位（沿光的水平行进方向）、只在
# 横向 v 上排开，接收面是同一块无限远的平地同材质。
#
# 闭式解（写进 manifest，测量脚本据此对拍）：太阳是角直径 α 的圆盘；接收面水平、遮挡边缘
# **垂直于光的水平行进方向**时，半影宽度
#     W(h) = h · [cot(e − α/2) − cot(e + α/2)]        e = 太阳仰角
# 一阶 = α·h/sin²e。本场景 e = asin(0.75/|SUN|) = 48.1108°（sin²e = 0.554187），
# 于是 W/h = 0.016692（α=0.53° 出厂）/ 0.157809（α=5°）。**线性于 h** 是判据本体。
#
# 判据用**比值** W(h_max)/W(h_min)：PCSS 应 ≈ h_max/h_min，CSM 应 ≈ 1。选比值不选绝对值 =
# 透视缩放、像素尺度、相机标定全部约掉 ⟹ 量具自身不引入误差（绝对值那列仍报，用来查
# max_penumbra 软夹取的介入点，那是出厂配置的真实行为，不是测量误差）。
#
# 三条让"边缘垂直于光"成立的机关（写错任何一条，测到的就是方位角展宽而非仰角展宽）：
#   ① 板的 yaw = atan(0.9) = 41.9872°，把板的局部 +x 精确转到光的水平行进方向
#      u = normalize(2/3, −3/5)（= SHADOW_OFF 归一化，见下）；
#   ② 扫描沿 u 走，穿过的是板的**局部 ±x 边**（不是世界 x 边）；
#   ③ 接收面是**单一平地**（不是阶梯/坡面），间隙 = 板底高度，逐点相同。
#
# ⚠ **只量下游缘**（离开影的那条），上游缘不可测且这是几何事实不是疏忽：上游缘位于
# u = 0.897h − 8，板 footprint 是 u ∈ [−8, 8] ⟹ h < 17.8 m 时上游缘就在板**正下方**，
# 被不透明的板本身挡住（任何俯角都躲不开，俯角越平被遮得越多）。下游缘 u = 0.897h + 8 > 8
# 恒在板外，但板仍会在它下游投下"相机视线遮挡带"，其边界 = 视线擦过板下游缘：
#     u_hidden(h) = (8·Y − h·u_cam)/(Y − h)      （Y = 相机高、u_cam = 相机沿 u 的坐标）
# 俯角 65° / 距离 170 m 时四档余量 = 4.3 / 8.3 / 14.8 / 19.8 m（h=8/16/32/64），全部 > 半影
# 半宽 ⟹ 下游半影完整可见。**换机位前必须重算这条**，否则测到的是"板挡住了"而不是"没半影"。
#
# 自校验锚点（阳性）：下游缘 50% 交叉的**位置**有闭式解 = 0.897h + 8（以板心沿 u 计）。
# 位置对得上 ⟹ 相机投影复刻正确、扫描线落在正确的地上；宽度读数才有意义。

LADDER_HEIGHTS = (8.0, 16.0, 32.0, 64.0)   # 间隙梯（米）；等比 ×2 ⟹ 判据比值 = 8
LADDER_SIDE = 16.0                         # 板边长（米）；同时是图内自定标的已知长度
LADDER_THICK = 0.5
LADDER_SPACING = 44.0                      # 沿 v 的间距；影包络两两不相交（下方 assert）
LADDER_YAW = math.degrees(math.atan(0.9))  # 41.9872°：板局部 +x → 光的水平行进方向 u
LADDER_U = (SHADOW_OFF[0] / math.hypot(*SHADOW_OFF), SHADOW_OFF[1] / math.hypot(*SHADOW_OFF))
LADDER_V = (-LADDER_U[1], LADDER_U[0])     # 横向（垂直于光的水平行进方向）
LADDER_SUN_ELEV = math.asin(-SUN[1] / math.sqrt(sum(v * v for v in SUN)))

# 机位由（俯角, 距离）**反解**，不手填坐标 —— 手填过一次就把净空算错了（h=64 档净空只剩
# 0.7 m，被下面的构建期 assert 当场抓住）。场景心 = 四片影的均值位置（沿 u 26.9 m）。
LADDER_TARGET_U = 0.896908 * sum(LADDER_HEIGHTS) / len(LADDER_HEIGHTS)   # = 26.9
LADDER_CAM_ELEV_DEG = 75.0
LADDER_CAM_DIST = 200.0


def ladder_target():
    return (LADDER_TARGET_U * LADDER_U[0], 0.0, LADDER_TARGET_U * LADDER_U[1])


def ladder_camera():
    e = math.radians(LADDER_CAM_ELEV_DEG)
    tx, _, tz = ladder_target()
    back = LADDER_CAM_DIST * math.cos(e)
    return (tx - back * LADDER_U[0], LADDER_CAM_DIST * math.sin(e),
            tz - back * LADDER_U[1])


LADDER_CAM = {"position": list(ladder_camera()), "target": list(ladder_target()),
              "fovDegrees": 40.0}


def ladder_camera_clearance():
    """每档下游缘的**视线净空**（米）：> 0 才说明板没挡住自己要测的那条影缘。

    板是不透明的。视线擦过板下游缘（u = 边长/2, y = h）时落地的 u 就是遮挡带边界：
        u_hidden(h) = (边长/2 · Y − h · u_cam) / (Y − h)
    下游缘在 u_edge(h) = |SHADOW_OFF|·h + 边长/2。返回 [(h, u_hidden, u_edge, margin)]。
    """
    cx, y, cz = ladder_camera()
    u_cam = cx * LADDER_U[0] + cz * LADDER_U[1]
    half = LADDER_SIDE / 2.0
    offlen = math.hypot(*SHADOW_OFF)
    out = []
    for h in LADDER_HEIGHTS:
        u_hidden = (half * y - h * u_cam) / (y - h)
        u_edge = offlen * h + half
        out.append((h, u_hidden, u_edge, u_edge - u_hidden))
    return out


def ladder_penumbra_per_meter(alpha_deg: float) -> float:
    """闭式半影宽度对间隙的斜率 dW/dh（米/米）；边缘垂直于光的水平行进方向、接收面水平。"""
    half = math.radians(alpha_deg) / 2.0
    e = LADDER_SUN_ELEV
    return 1.0 / math.tan(e - half) - 1.0 / math.tan(e + half)


def scene_penumbra_ladder() -> Scene:
    s = Scene(
        name="penumbra_ladder",
        purpose="半影宽度↔间隙曲线：PCSS 接触硬化的能力判据（CSM 原理上给不出斜率）",
        honesty=("半影宽度的闭式解只在**边缘垂直于光的水平行进方向 + 接收面水平**时成立，"
                 "本场景靠 yaw/平地两个机关把这两个前提钉死（见源码头注）；换机位扫描方向就"
                 "不再适用该闭式解。max_penumbra 是**图集纹素域**的 soft clamp，高 h 档会被"
                 "夹住——那是出厂配置的真实行为，读数里必须区分'物理该多宽'与'配置允许多宽'。"
                 "级联边界随相机漂移（运行期自适应），本场景刻意压小横向跨度让四档尽量落在"
                 "同一条 texel 尺度上，但**不承诺**它们真在同一级联——CSM 那一列若出现台阶，"
                 "先查级联再查 PCSS。"),
        cameras={
            "ladder": {"position": list(LADDER_CAM["position"]),
                       "target": list(LADDER_CAM["target"]),
                       "fovDegrees": LADDER_CAM["fovDegrees"],
                       "note": "沿 u 上游、俯角 65°、距场景心 170 m。俯角是**算出来的**："
                               "再平一点，板就会挡住自己的下游影缘（净空见 selfProof，"
                               "构建期 assert 非负）"},
        },
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（闭式解的前提）"},
            "sunElevationDeg": math.degrees(LADDER_SUN_ELEV),
            "penumbraPerMeterOfGap": {
                "formula": "W/h = cot(e-a/2) - cot(e+a/2)，a = pcss.sun_degrees",
                "at_0.53": ladder_penumbra_per_meter(0.53),
                "at_5": ladder_penumbra_per_meter(5.0),
            },
            "csmExpectation": "W 与 h 无关（PCF 核宽 = 阴影贴图纹素数 × texelWorld），"
                              "判据比值 W(64)/W(16) 应 ≈ 1；PCSS 应 ≈ 4",
            "measureOnly": "下游缘（离开影的那条）。上游缘在 h<17.8 m 时位于板正下方、被板本身"
                           "挡住，任何俯角都测不到 —— 这是几何事实，不是配置没调好",
            "edgePositionAnchor": "下游缘 50% 交叉沿 u 的位置 = |SHADOW_OFF|·h + 边长/2"
                                  "（闭式、独立于宽度）⟹ 位置对得上才证明投影复刻没错",
            # ── 测量接口（imgqa 判据读本块，**不在 C++ 里重抄几何常量** = 不造第二真相）──
            # 扫描 = 从 shadowCenter 起沿 u 走 s 米；下游缘在 s = edgeS；半影宽度取该缘上
            # 阴影因子 S=(none−arm)/none 的 10↔90 交叉间距。
            "measure": {
                "u": [LADDER_U[0], LADDER_U[1]],
                "v": [LADDER_V[0], LADDER_V[1]],
                "shadowOffsetPerMeter": list(SHADOW_OFF),
                "side": LADDER_SIDE,
                "spacing": LADDER_SPACING,
                "edgeS": LADDER_SIDE / 2.0,
                "scanHalfSpan": LADDER_SIDE / 2.0,
                "step": 0.05,
                "plates": [
                    {"name": f"plate_h{int(h):02d}", "h": h,
                     "center": [off * LADDER_V[0], off * LADDER_V[1]],
                     "shadowCenter": [off * LADDER_V[0] + h * SHADOW_OFF[0],
                                      off * LADDER_V[1] + h * SHADOW_OFF[1]],
                     "edgeS": LADDER_SIDE / 2.0}
                    for h, off in zip(LADDER_HEIGHTS,
                                      [(i - (len(LADDER_HEIGHTS) - 1) / 2.0) * LADDER_SPACING
                                       for i in range(len(LADDER_HEIGHTS))])
                ],
            },
        },
    )
    P = s.pieces.append

    half = 200.0
    P(Piece("ground", quad((-half, 0.0, -half), (0.0, 0.0, 2 * half), (2 * half, 0.0, 0.0)),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="唯一接收面（同材质同粗糙度 ⟹ 四档半影的明暗基准完全一致，"
                   "明暗跳变全部归因于半影本身而不是材质差异）；法线 +Y"))

    plates = [
        (f"plate_h{int(h):02d}", h, c,
         f"间隙 h={h:g} m 档：底面离地 {h:g} m，板面水平 ⟹ 影内每一点的遮挡体↔接收面间隙"
         f"恒等于 h（这是本场景唯一的自变量）")
        for h, c in zip(LADDER_HEIGHTS,
                        [PAL["red"], PAL["magenta"], PAL["cyan"], PAL["orange"]])
    ]
    n = len(plates)
    footprints = []
    for i, (name, h, color, intent) in enumerate(plates):
        off = (i - (n - 1) / 2.0) * LADDER_SPACING
        cx, cz = off * LADDER_V[0], off * LADDER_V[1]
        P(Piece(name, box(cx, h + LADDER_THICK / 2.0, cz, LADDER_SIDE, LADDER_THICK,
                          LADDER_SIDE, yaw_deg=LADDER_YAW),
                color, roughness=0.7, intent=intent))
        r = LADDER_SIDE / 2.0  # 旋转不改变沿自身局部轴的跨度；世界 AABB 见下
        ext = LADDER_SIDE * (abs(math.cos(math.radians(LADDER_YAW)))
                             + abs(math.sin(math.radians(LADDER_YAW)))) / 2.0
        footprints.append((name, (cx - ext, cx + ext, cz - ext, cz + ext), h))

    # ---- 构建期几何断言（口头承诺不算数，坏了直接构建失败）----
    # 视线净空：板是不透明的，俯角不够就会挡住**自己要测的那条下游影缘**，症状是"半影量不出来"
    # 而不是"没影"——最难归因的一种失效，所以钉在构建期。
    for h, u_hidden, u_edge, margin in ladder_camera_clearance():
        half_pen = ladder_penumbra_per_meter(5.0) * h / 2.0   # 最宽档（α=5°）的半影半宽
        assert margin > half_pen, \
            f"penumbra_ladder: h={h} 档板体遮挡带边界 u={u_hidden:.1f} 吃掉下游影缘 " \
            f"u={u_edge:.1f}（净空 {margin:.1f} m < 半影半宽 {half_pen:.1f} m）——抬高相机"
    ground = (-half, half, -half, half)
    for name, rect, h in footprints:
        assert _rect_contains(ground, _rect_hull(rect, h)), \
            f"penumbra_ladder: {name} 的影包络落出接收面"
    for i, (na, ra, ha) in enumerate(footprints):
        for nb, rb, hb in footprints[i + 1:]:
            assert not _rect_overlap(_rect_hull(ra, ha), rb), \
                f"penumbra_ladder: {na} 的影压上 {nb}（半影会被邻居污染）"
            assert not _rect_overlap(_rect_hull(rb, hb), ra), \
                f"penumbra_ladder: {nb} 的影压上 {na}（半影会被邻居污染）"
    return s


# ---------------------------------------------------------------------------
# 场景 8：ssr_rig —— 屏幕空间反射的**闭式**靶（Phase 15 批 15.0）
# ---------------------------------------------------------------------------
# ## 为什么必须新造靶，而不是在 env_band 上加一根柱
#
# env_band 的 A5 断言是**单变量纪律**（四 piece 同色板位、同几何、metallic 全 1，只有
# roughness 不同）。加柱会直接违反它自己的断言；nm_rig 当年也是同一个理由另造的靶
# （见本文件「为什么必须新造一个靶，而不是在 env_band 上加纹理」）。
#
# ## 判据为什么是闭式的（本 rig 的全部价值在这一段）
#
# 平面 y=0、N=+Y 的镜面：眼 E=(0,h,0) 看向地面点 P=(x,0,z) 的入射方向 D=(x,−h,z)，
# 反射方向 R = D − 2(D·N)N = (x, +h, z)。沿 R 前进：Q(t) = (x(1+t), ht, z(1+t))。
# 令 s = 1+t ≥ 1，则**水平位置 = s·(x,z)、高度 = h(s−1)**。于是命中竖直柱
# （水平截面 C、y ∈ [0,Hc]）的条件是
#       ∃ s ∈ [1, 1 + Hc/h] : s·(x,z) ∈ C
# 换 λ = 1/s 写成"倒影区 = 柱底面关于相机原点的缩放并集"：
#       R_refl = ⋃_{λ ∈ [λmin, 1]} λ·C,     λmin = h / (h + Hc)
# **与方位角无关、与柱的形状无关**——C 是矩形时该并集就是 8 个角点（λmin·四角 ∪ 1·四角）
# 的凸包。所以"哪块屏幕该出现倒影"是纯几何闭式解，不需要任何渲染器参与，
# 这正是本项目判据体系里最高的一档（闭式解 > 统计）。
#
# ## 判据算子 = SSR 开/关**差分**，不是绝对亮度（务必读）
#
# 地面是 metallic=1，方向光在它上面本来就有 specular 高光；env 也非全黑。所以
# 「倒影区变亮」这种绝对判据会把太阳高光算进来。正确算子 = **因果泄漏恒等式**
# （skill rgl-verify）：SSR 关时，闭式掩码**之外**的像素交付图必须逐位不变；
# 掩码**之内**必须出现显著变化（阳性锚点）。共同项在差分里天然抵消。
#
# ## 两条诚实声明（harness 必须处理，否则会得出假结论）
#
# ① **柱身与倒影在屏幕上部分重叠**（几何必然：倒影区远边界 = λ=1 的柱底面，它的屏幕行
#    比柱底近端更高）。所以判据必须用 objectID 可见集把「柱自己的像素」摘出去，
#    不能拿屏幕 bbox 当掩码。
# ② 地面**完全平坦且无纹理**（同 env_band 的取舍）——本 rig 只测「反射**位置**对不对」
#    这一条轴，测不了粗糙度模糊与水膜法线扰动（那是 15.3 的事，届时另加参数）。
#
# ## 变异红线（改坏哪里会让本靶立刻构建失败）
#
# 下表**逐条实测验红**（2026-10-10，7/7 变红、无一存活；复跑 = 本文件头部纪律）。
# "红在" = **实际**首先触发的断言。断言顺序刻意排成「闭式几何 A1/A2 → 屏幕投影 A3/A4 →
# 场景装配 A5–A7」，让最具体的一条先报——第一版把屏幕区间放在前面，结果 λmin 写倒数
# 只报出"出画"这种粗判据，归因要重做一遍。
#
# ⚠ 写这张表时按记忆填"该红哪条"，**四条里错了三条**（纪律「断言要照实测写，不照记忆写」
# 就是这么来的）。另外第一版的 A0 **测不到**「投影函数自己会归一化」：`nm_ray_dir` 返回的
# 本就是单位向量，只拿它做往返，去掉归一化时 A0 原样绿——补了"喂非单位方向"的第二段才红。
#
# | 变异 | 红在 |
# |---|---|
# | 投影函数去掉归一化 | **A0** 第二段（未归一化方向往返误差 1.4e19 px） |
# | `λmin` 写成倒数 `(h+Hc)/h` | **A1**（倒影点半径 12.79 越过柱底最远角 6.63） |
# | `λmin` 写死成常数 | **A2**（面积 3.870/3.870/3.870 不随柱高单调） |
# | `λ` 区间退化成单点（忘柱高贡献） | **A2**（面积 1.440/1.440/1.440） |
# | 相机俯角写错（8°→0°） | **A3**（倒影行区间 [569, 855] 出画） |
# | 负对照忘了传 `hc=0` | **A5**（无柱变体的凸包非空） |
# | 地面半宽缩到盖不住凸包 | **A6**（倒影落出接收面） |

SSR_CAM_H = 1.6        # 相机离地高度（m），人眼工作点
SSR_PITCH_DEG = 8.0    # 相机俯角（度）
SSR_FOV_V = 45.0       # 垂直 fov（与 nm_rig 同约定：垂直 fov + 宽高比推水平）
SSR_RHO = 6.0          # 柱底心到相机原点的水平距离（沿 −Z），m
SSR_HALF = 0.6         # 柱截面半宽 = 半深（正方形），m
SSR_HC = 1.5           # 柱高（m）
SSR_GROUND_HALF_X = 25.0
SSR_GROUND_Z_NEAR = 5.0     # 地面向相机身后延伸（避免画面下缘看到"地面尽头"）
SSR_GROUND_Z_FAR = 25.0
SSR_GROUND_ROUGHNESS = 0.12  # 与 nm_rig 交付档一致；越接近镜面判据越锐
SSR_PROBE_W, SSR_PROBE_H = 1280, 720   # = llm_shot 默认取景，断言与 probe 共用同一份


def ssr_dir_to_pixel(d, w: int = SSR_PROBE_W, h: int = SSR_PROBE_H,
                     fov_v: float = SSR_FOV_V, pitch_deg: float = SSR_PITCH_DEG):
    """世界方向 → 亚像素坐标（`nm_ray_dir` 的闭式逆，同一套 glm::perspective 约定）。

    ⚠ 必须先归一化：下面 asin(dot(d,û)) 的定义域是**单位**向量，喂未归一化方向会让
    asin 撞上 clamp 边界 → tan(90°) 发散出 1e16 量级的假行号（本 rig 开发时实测踩过，
    症状是"倒影跑到画面外"，而闭式代码看起来完全正确）。
    """
    n = math.sqrt(sum(c * c for c in d))
    dx, dy, dz = d[0] / n, d[1] / n, d[2] / n
    p = math.radians(pitch_deg)
    sp, cp = math.sin(p), math.cos(p)
    # f̂=(0,−sin p,−cos p)、r̂=(1,0,0)、û=(0,cos p,−sin p)（与 nm_ray_dir 逐字同式）
    tv = math.asin(max(-1.0, min(1.0, dy * cp - dz * sp)))
    th = math.atan2(dx, -(dy * sp + dz * cp))
    t = math.tan(math.radians(fov_v / 2.0))
    ndc_x = math.tan(th) / (t * (w / h))
    ndc_y = math.tan(tv) / t
    return ((ndc_x + 1.0) / 2.0 * w - 0.5, (1.0 - ndc_y) / 2.0 * h - 0.5)


def ssr_ground_point(px: float, py: float, w: int = SSR_PROBE_W, h: int = SSR_PROBE_H,
                     fov_v: float = SSR_FOV_V, pitch_deg: float = SSR_PITCH_DEG,
                     cam_h: float = SSR_CAM_H):
    """像素 → 地面点 (x, 0, z)；None = 该像素打不到 y=0（朝上）。复用 nm_ray_dir。"""
    d = nm_ray_dir(int(round(px)), int(round(py)), w, h, fov_v, pitch_deg)
    if d[1] >= -1e-12:
        return None
    t = cam_h / -d[1]
    return (d[0] * t, 0.0, d[2] * t)


def _convex_hull_2d(pts):
    """Andrew monotone chain → 逆时针凸包（去共线）。排序先于扫描 ⟹ 输出与输入序无关
    （确定性纪律：同一输入两次生成必须逐字节相同）。"""
    ps = sorted(set(pts))
    if len(ps) <= 2:
        return ps

    def cr(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower: list = []
    for p in ps:
        while len(lower) >= 2 and cr(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper: list = []
    for p in reversed(ps):
        while len(upper) >= 2 and cr(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def ssr_reflection_hull(half: float = SSR_HALF, rho: float = SSR_RHO, hc: float = SSR_HC,
                        cam_h: float = SSR_CAM_H):
    """倒影区（世界 XZ 平面）= ⋃_{λ∈[λmin,1]} λ·柱底面 的凸包；柱高 ≤ 0 → 空集。

    返回 [(x, z), ...] 逆时针。λmin = h/(h+Hc) 来自"反射射线在柱高范围内"这一约束
    （推导见本段头注释），**不是**调出来的旋钮。
    """
    if hc <= 0.0:
        return []
    lam_min = cam_h / (cam_h + hc)
    corners = [(sx, sz) for sx in (-half, half) for sz in (-half, half)]
    pts = [(lam * sx, lam * (-rho + sz)) for lam in (lam_min, 1.0) for sx, sz in corners]
    return _convex_hull_2d(pts)


def ssr_probe(w: int = SSR_PROBE_W, h: int = SSR_PROBE_H, hc: float = SSR_HC) -> dict:
    """闭式取景事实（进 manifest，供 harness 定位与构建期断言）。

    `hc = 0` = **无反射源**（负对照变体用）⟹ 凸包空集、行/列区间为 None。倒影区间的
    存在性是几何属性而非渲染结果，所以负对照的"区间必须为空"能在构建期就断言掉。
    """
    hull = ssr_reflection_hull(hc=hc)
    lam_min = SSR_CAM_H / (SSR_CAM_H + hc) if hc > 0.0 else None
    proj = [ssr_dir_to_pixel((x, -SSR_CAM_H, z), w, h) for x, z in hull]
    pillar_rows = [] if hc <= 0.0 else [
        ssr_dir_to_pixel((sx, y - SSR_CAM_H, -SSR_RHO + sz), w, h)[1]
        for y in (0.0, hc) for sx in (-SSR_HALF, SSR_HALF) for sz in (-SSR_HALF, SSR_HALF)]
    # 柱身的屏幕凸包：harness 用它把「柱自己的像素」从倒影判据里摘出去（诚实声明 ②：
    # 倒影远边界与柱底在屏幕上重叠，拿 bbox 或行区间当掩码会把柱身算成倒影）。
    pillar_hull = [] if hc <= 0.0 else _convex_hull_2d([
        ssr_dir_to_pixel((sx, y - SSR_CAM_H, -SSR_RHO + sz), w, h)
        for y in (0.0, hc) for sx in (-SSR_HALF, SSR_HALF) for sz in (-SSR_HALF, SSR_HALF)])
    return {
        "frameWidth": w, "frameHeight": h,
        "lambdaMin": round(lam_min, 6) if lam_min is not None else None,
        "mirrorRow": h / 2.0 - 0.5,          # env 反射的镜像线 = 主点行
        "reflectionHullWorldXZ": [[round(x, 6), round(z, 6)] for x, z in hull],
        "reflectionHullPx": [[round(px, 4), round(py, 4)] for px, py in proj],
        "reflectionRowRangePx": ([round(min(p[1] for p in proj), 3),
                                  round(max(p[1] for p in proj), 3)] if proj else None),
        "reflectionColRangePx": ([round(min(p[0] for p in proj), 3),
                                  round(max(p[0] for p in proj), 3)] if proj else None),
        "pillarRowRangePx": ([round(min(pillar_rows), 3), round(max(pillar_rows), 3)]
                             if pillar_rows else None),
        "pillarHullPx": [[round(px, 4), round(py, 4)] for px, py in pillar_hull],
        "reflectionAreaM2": None,            # 由调用方按凸包鞋带公式填
    }


def scene_ssr_rig(pillar: bool = True) -> Scene:
    """`pillar=False` = 负对照（无柱）：闭式倒影凸包必须为空集，判据的"反向用例"。"""
    hc = SSR_HC if pillar else 0.0
    probe = ssr_probe(hc=hc)
    hull = ssr_reflection_hull(hc=hc)
    name = "ssr_rig" if pillar else "ssr_rig_empty"
    probe["reflectionAreaM2"] = round(_hull_area(hull), 6)

    lam_txt = (f"λ∈[{probe['lambdaMin']:.4f},1]" if pillar
               else "（无柱 ⟹ 倒影区 = ∅）")
    s = Scene(
        name=name,
        purpose=("SSR 闭式靶：metallic=1 平坦镜面地面 + 一根几何完全已知的竖直柱，"
                 f"倒影区 = 柱底面关于相机原点的缩放并集 {lam_txt}，"
                 "其屏幕位置是纯几何闭式解（不依赖渲染器）"
                 + ("" if pillar else "；本变体**无柱** = 负对照")),
        honesty=("① 判据算子必须是 SSR 开/关**差分**（因果泄漏恒等式），不是绝对亮度——"
                 "金属地面本来就有方向光 specular 与 env 项，绝对判据会把它们算进倒影。"
                 "② 柱身与倒影在屏幕上**部分重叠**（倒影远边界 = λ=1 的柱底面，其屏幕行"
                 "比柱底近端更高），所以必须用 objectID 可见集摘出柱自身像素，"
                 "拿屏幕 bbox 当掩码会把柱身算成倒影。③ 地面完全平坦无纹理，本 rig 只测"
                 "「反射位置对不对」，测不了粗糙度模糊与水膜法线扰动（15.3 另加参数）。"
                 "④ 无柱变体的地面与有柱变体逐字节同材质同几何，唯一差别是少一个 piece。"),
        cameras={
            "grazing": {"position": [0.0, SSR_CAM_H, 0.0],
                        "target": [0.0,
                                   SSR_CAM_H - 10.0 * math.sin(math.radians(SSR_PITCH_DEG)),
                                   -10.0 * math.cos(math.radians(SSR_PITCH_DEG))],
                        "fovDegrees": SSR_FOV_V},
        },
    )
    P = s.pieces.append
    P(Piece("ground_metal",
            # ⚠ u/v 顺序**不可随意对调**：quad 的法线 = cross(u,v)，本 rig 要 +Y。全项目地面
            #   quad 的约定是 (0,0,+L),(L,0,0)（见 shadow_rig / nm_rig 的 ground）。写成
            #   (0,0,−L),(L,0,0) 会得到 −Y 法线，画面靠 deferred_geometry 的 `!frontFace → N=−N`
            #   背面翻向救回来，于是 N 要先过一遍 TBN 再翻号——实测带 ~5° 偏差，SSR 的反射方向
            #   被 2× 放大成 ~10°，靶上表现为「倒影只剩中央窄带 + 两侧地面假命中」（2026-10-10）。
            quad((-SSR_GROUND_HALF_X, 0.0, SSR_GROUND_Z_NEAR),
                 (2.0 * SSR_GROUND_HALF_X, 0.0, 0.0),
                 (0.0, 0.0, -(SSR_GROUND_Z_NEAR + SSR_GROUND_Z_FAR))),
            PAL["yellow"], metallic=1.0, roughness=SSR_GROUND_ROUGHNESS,
            radial=False, closed=False,
            intent=("metallic=1 镜面接收面；baseColor 是金属的 F0 调制色，"
                    "所以它**不能**与柱色正交（cyan×red=0 会让倒影恒黑）——"
                    "yellow×red = red，倒影可辨而地面自身在纯黑 env 下近黑")))
    if pillar:
        P(Piece("pillar_red",
                box(0.0, SSR_HC / 2.0, -SSR_RHO, 2.0 * SSR_HALF, SSR_HC, 2.0 * SSR_HALF),
                PAL["red"], metallic=0.0, roughness=0.4,
                intent=(f"反射源：底心 (0,0,−{SSR_RHO})、高 {SSR_HC} m、截面 "
                        f"{2 * SSR_HALF} m 方柱；倒影凸包 = 其底面缩放并集")))

    s.extra["probe"] = probe
    s.extra["closedForm"] = {
        "relation": "R_refl = ⋃_{λ∈[h/(h+Hc),1]} λ·C，C = 柱底面，h=相机离地高，Hc=柱高",
        "derivation": ("平面 N=+Y：E=(0,h,0) 看 P=(x,0,z) 的反射方向 = (x,h,z)，"
                       "沿它走 s=1+t 步 ⟹ 水平 = s·(x,z)、高度 = h(s−1)；"
                       "命中竖直柱 ⟺ ∃s∈[1,1+Hc/h] 使 s·(x,z) ∈ C"),
        "cameraHeightM": SSR_CAM_H, "pillarHeightM": SSR_HC,
        "pillarCenterZ": -SSR_RHO, "pillarHalfM": SSR_HALF,
        "operator": "SSR 开/关差分（因果泄漏恒等式）；绝对亮度判据无效，见 honesty ①",
    }

    # ── 构建期断言 ──────────────────────────────────────────────────────
    # A0 **闭式实现自身的阳性锚点**：ssr_dir_to_pixel 必须是 nm_ray_dir 的逆。
    #    这条不成立时后面所有断言都在自欺（投影写错 → 凸包"在画面内"也毫无意义）。
    #    ⚠ 第二段**不是冗余**：nm_ray_dir 返回的本就是单位向量，只测它等于**没测**
    #    「函数自己会归一化」这条契约（变异验红实测：去掉归一化时 A0 原样绿，
    #    只有喂非单位方向才红）。归一化一旦丢，凸包会发散到 1e19 量级的假行号。
    err = 0.0
    for px in range(0, SSR_PROBE_W, 97):
        for py in range(0, SSR_PROBE_H, 71):
            d = nm_ray_dir(px, py, SSR_PROBE_W, SSR_PROBE_H, SSR_FOV_V, SSR_PITCH_DEG)
            qx, qy = ssr_dir_to_pixel(d, SSR_PROBE_W, SSR_PROBE_H)
            err = max(err, abs(qx - px), abs(qy - py))
    assert err < 1e-6, f"A0 投影逆映射往返误差 {err:.2e} px（应 ≪ 1e-6）"
    err_scaled = 0.0
    for px in range(0, SSR_PROBE_W, 131):
        for py in range(0, SSR_PROBE_H, 97):
            d = nm_ray_dir(px, py, SSR_PROBE_W, SSR_PROBE_H, SSR_FOV_V, SSR_PITCH_DEG)
            for scale in (0.001, 7.0, 1e5):    # 世界点→方向那一步喂进来的就是未归一化向量
                qx, qy = ssr_dir_to_pixel(tuple(c * scale for c in d), SSR_PROBE_W, SSR_PROBE_H)
                err_scaled = max(err_scaled, abs(qx - px), abs(qy - py))
    assert err_scaled < 1e-6, \
        f"A0 未归一化方向往返误差 {err_scaled:.2e} px（函数必须自己归一化）"
    # ── 断言顺序 = 先闭式几何（A1/A2）、再屏幕投影（A3/A4）、最后场景装配（A5–A7）。
    #    理由：闭式几何那条最具体、最先报出"是推导错了还是取景错了"；反过来把屏幕区间
    #    放前面，任何几何错都只会报"出画"这条粗判据，归因要重做一遍（实测踩过：
    #    λmin 写倒数被 A「出画」遮蔽，看不出是几何问题）。
    # A1 **倒影区不许越过柱底**（出处 = 闭式契约本身，不是本文件代码）：
    #    R_refl = ⋃_{λ∈(0,1]} λ·C ⟹ 每个凸包点都是 C 内某点的**不超过 1 倍**的缩放，
    #    于是 |p| ≤ max_{c∈C}|c|（倒影只会比柱更靠近相机，绝不会跑到柱后面去）。
    #    专门抓 λmin 写反（误写成 (h+Hc)/h > 1）：那种错不会让面积单调性变号，
    #    但会让倒影区整体翻到柱的**远侧**——画面上一切照常、判据量的是空气。
    # A2 倒影区随柱高**单调增**（越高 ⟹ 反射射线能在柱高范围内走越远 ⟹ 近端越靠近相机）。
    #    抓 λmin 与 Hc 的耦合被写死成常数（λ 区间退化成单点也一并红）。
    if hull:
        r_farthest = max(math.hypot(sx, -SSR_RHO + sz)
                         for sx in (-SSR_HALF, SSR_HALF) for sz in (-SSR_HALF, SSR_HALF))
        for x, z in hull:
            assert math.hypot(x, z) <= r_farthest + 1e-9, \
                f"A1 倒影点 ({x:.3f},{z:.3f}) 半径超过柱底最远角 {r_farthest:.3f} ⟹ 倒影跑到柱后面"
        a_lo = _hull_area(ssr_reflection_hull(hc=SSR_HC * 0.5))
        a_hi = _hull_area(ssr_reflection_hull(hc=SSR_HC * 2.0))
        assert a_lo < probe["reflectionAreaM2"] < a_hi, \
            f"A2 倒影面积不随柱高单调：{a_lo:.3f} / {probe['reflectionAreaM2']:.3f} / {a_hi:.3f}"
        # A3 倒影凸包完全在画面内（出画 = 判据区域不完整，差分只能量到一半）
        rr, rc = probe["reflectionRowRangePx"], probe["reflectionColRangePx"]
        assert 0.0 < rr[0] and rr[1] < SSR_PROBE_H - 1.0, f"A3 倒影行区间 {rr} 出画"
        assert 0.0 < rc[0] and rc[1] < SSR_PROBE_W - 1.0, f"A3 倒影列区间 {rc} 出画"
        # A4 倒影区与 env 镜像线（主点行）分得开——否则「env 反射」与「SSR 反射」两件事
        #    在同一批像素上混在一起，差分归不了因（env_band 的窄带就落在这一行）。
        assert rr[0] - probe["mirrorRow"] >= 60.0, \
            f"A4 倒影上边界 {rr[0]:.0f} 离镜像线 {probe['mirrorRow']:.0f} 不足 60px"
    # A5 柱身必须在画面内（柱不在屏幕上 ⟹ 没有任何像素能 ray march 到它，靶是空的）；
    #    负对照走 else：闭式凸包必须是空集（判据的**反向用例**，区分"没倒影"与"算错了"）
    if pillar:
        pr = probe["pillarRowRangePx"]
        assert -1.0 < pr[0] and pr[1] < SSR_PROBE_H, f"A5 柱身行区间 {pr} 出画"
        assert probe["reflectionAreaM2"] > 0.5, \
            f"A5 倒影面积 {probe['reflectionAreaM2']} m² 过小"
    else:
        assert hull == [], "A5 负对照（无柱）的倒影凸包必须为空集"
    # A6 地面必须覆盖整个倒影凸包（凸包落在地面外 = 看到的是"地面尽头"不是倒影）
    for x, z in hull:
        assert abs(x) <= SSR_GROUND_HALF_X, f"A6 倒影凸包 x={x} 落出地面"
        assert -SSR_GROUND_Z_FAR <= z <= SSR_GROUND_Z_NEAR, f"A6 倒影凸包 z={z} 落出地面"
    # A7 柱几何非退化（柱底贴地但不穿进地面，穿模会让 objectID 可见集出现 z-fighting）
    if pillar:
        assert SSR_HC > 0.0 and SSR_RHO - SSR_HALF > 0.0, "A7 柱几何退化"
    return s


def _hull_area(hull) -> float:
    """鞋带公式面积（probe 的 reflectionAreaM2 与 A7 单调性判据共用一处实现）。"""
    n = len(hull)
    if n < 3:
        return 0.0
    return abs(sum(hull[i][0] * hull[(i + 1) % n][1] - hull[(i + 1) % n][0] * hull[i][1]
                   for i in range(n))) / 2.0


def scene_ssr_rig_empty() -> Scene:
    """负对照：同地面、无柱 ⟹ 闭式倒影凸包为空，判据区域必须拿不到任何变化。"""
    return scene_ssr_rig(pillar=False)


# ---------------------------------------------------------------------------
# 场景 8：blend_sort —— 透明排序正确性的靶（透明混合管线刀3）
# ---------------------------------------------------------------------------
# 设计要点（每条都在堵一条"靶立不起来却看不出来"的路）：
#
# 1. **三层全用 unlit**：unlit 分支在 transparent.frag / light.frag 里都是**早退**
#    （return float4(albedo, alpha) / return float4(albedo, 1.0)），不采光照、不采阴影、
#    不采 env、不加自发光 ⟹ 闭式里只剩「baseColor + 混合 + 显示链」三项，不必复刻
#    BRDF/IBL/阴影，也不必碰债-131 那个未决的自发光电平。
# 2. **颜色只取 PAL 的纯 0/1 三原色**：① 透明段写 RGBA16F，0/1 与 α∈{0.5,0.25} 的混合结果
#    全是 2 的幂的有限和 ⟹ half 精度**逐位精确**，闭式与实机之间没有舍入差可争；② 不透明侧
#    albedo 要过 G-buffer 8bit sRGB 往返，而 0.0/1.0 是这条往返的**不动点**（srgbEncode
#    两端点原样返回）⟹ 背景值不需要模型化量化。（选 0.5 这类中间值会把"量化往返"混进
#    判据，红的时候分不清是谁。）
# 3. **每层一个 node 平移**：排序代理 = entity 世界矩阵平移列，node 全 identity 时所有图元
#    distanceSq 相等 ⟹ 比较子 > 与 < 给出同一序列 = 靶结构性不红（详见 Piece.translation
#    的注）。这是本靶唯一真正的新机制，也是它必须先于判据存在的原因。
# 4. **四个判据区各由几何算出**（harness 侧做投影，不在图里找边界）：far-only / overlap /
#    near-only / bg-only。前三个各自锚住"α、c、显示链"，于是 overlap 区新增的**唯一未知
#    就是合成次序** —— 红的时候能一口咬定是排序，而不是"整条链都可疑"。
BLEND_SORT_CAM = {"position": (0.0, 0.0, 6.0), "target": (0.0, 0.0, 0.0), "fovDegrees": 45.0}
BLEND_SORT_HALF_TAN = 0.41421356237309503  # tan(22.5°)，与 fovDegrees=45 同源（harness 复算用）


def _xy_quad(size: float) -> Geom:
    """绕自身原点、朝 +Z 的正方形面片（相机在 +Z 正视 ⟹ 前向面 = 正面，透明段剔背面）。"""
    h = size / 2.0
    return quad((-h, -h, 0.0), (size, 0.0, 0.0), (0.0, size, 0.0))


def scene_blend_sort() -> Scene:
    s = Scene(
        name="blend_sort",
        purpose="透明混合排序正确性的靶：两层 unlit 半透面 + 一层 unlit 背景，"
                "期望像素由闭式混合式给出，翻转 renderListSystem 的排序比较子必须让它红",
        honesty="只测**合成次序**这一件事。三层全 unlit ⟹ 不覆盖：透明体的光照/阴影/自发光、"
                "MASK discard、velocity、picking（透明不入 picking）、GPU culling 候选（透明不入）。"
                "且 alpha 混合是非预乘 SRC_ALPHA/ONE_MINUS_SRC_ALPHA（rgl/shader.cppm 的 "
                "colorBlend 分支），换混合方程则本闭式作废。",
        cameras={"sort": dict(BLEND_SORT_CAM)},
    )
    s.pieces.append(Piece(
        "backdrop", _xy_quad(16.0), PAL["blue"], unlit=True, translation=(0.0, 0.0, -2.0),
        radial=False, closed=False, roughness=0.9,
        intent="OPAQUE unlit 背景：给混合提供 dst。z=-2 在最远处，两层都盖在它上面"))
    s.pieces.append(Piece(
        "layer_far", _xy_quad(2.0), PAL["green"], unlit=True, alpha=0.25,
        translation=(-0.4, 0.0, -1.0), radial=False, closed=False, roughness=0.9,
        intent="BLEND unlit 远层 α=0.25：node 原点 (-0.4,0,-1) ⟹ distanceSq=49.16"))
    s.pieces.append(Piece(
        "layer_near", _xy_quad(2.0), PAL["red"], unlit=True, alpha=0.5,
        translation=(0.4, 0.0, 0.0), radial=False, closed=False, roughness=0.9,
        intent="BLEND unlit 近层 α=0.5：node 原点 (0.4,0,0) ⟹ distanceSq=36.16（更小=更近）"))
    # 闭式设计事实（**只记事实，不记结论**）：harness 从这里吃 α / 颜色 / 落位与相机，
    # 自己算投影与混合式；它不在脚本里抄第二份颜色表，也不在图里找区域边界。
    s.extra["closedForm"] = {
        "camera": dict(BLEND_SORT_CAM),
        "halfTanHalfFov": BLEND_SORT_HALF_TAN,
        "aspect": [1280, 720],
        "blend": "non-premultiplied over: out = a*src + (1-a)*dst (rgl/shader.cppm colorBlend)",
        "regions": ["far_only", "overlap", "near_only", "bg_only"],
        "orderCorrect": ["layer_far", "layer_near"],
        "orderFlipped": ["layer_near", "layer_far"],
        "note": ("期望字节 = 显示链(线性混合值 × exposure)；线性混合值按 orderCorrect 逐层"
                 "over 合成，orderFlipped 只用于变异对照"),
    }
    return s


# ---------------------------------------------------------------------------
# 场景 9：blend_lit —— 透明段「lit 类别变体」的同源双臂靶（债-134 的先决输入）
# ---------------------------------------------------------------------------
# ## 它存在的唯一理由
# 出厂场景 0 个 BLEND 图元 ⟹ 每天跑的 just smoke / just regress 里透明段是个**空 pass**，
# 而唯一常驻的透明检查 blend_sort 三层全 unlit（frag 早退，不采光照/阴影/env）⟹
# transparent.frag 的 pbr / vegetation / light 三支 lit 变体在「有人会自动跑到」这个意义上
# **零像素覆盖**（账 → docs/还债总账.md 债-134；边界全文 → docs/透明混合管线_plan.md §4 末行）。
# 台账规定的修法**不是**再写一条闭式判据（透明光照的闭式要复刻 BRDF + IBL + CSM，成本 > 收益），
# 先决 = 一份可重放的透明冻结配置 + 一条弱判据。本场景 = 那份弱判据的**单变量输入**。
#
# ## 双臂 = 同一份场景生成两次，只差 alphaMode（这是整台仪器的立命处）
# 判据要证「透明段对交付图真的有贡献」，所以两臂必须只差「这几面是不是 BLEND」。
# 实现 = 同一个 `blend_lit_panes(blend=...)`，同一个几何/落位/材质，只有 `alpha` 取
# 1.0（不写 alphaMode 键 = OPAQUE）还是 0.25/0.5（写 alphaMode="BLEND"）之分。
# ⚠ **同源是构造性的，不是注释里的**：两臂共用同一个 `_BLEND_LIT_PANES` 表与同一个投影，
# 任何"只给一臂改几何"的写法都会让另一臂跟着变，所以想不同源都难。
#
# ## 为什么是三支 lit 变体、为什么 α 取两档
# 三个 pane 的**材质名**分别命中 configs/scene.lua material_styles 的两条 pattern（"Foliage"
# → vegetation、"Emissive" → light），第三条不命中 = pbr 回落。⟹ 一次运行把
# transparent / transparent_vegetation / transparent_light 三支 SPV 全打上像素。
# （OPAQUE 臂同名字走 deferred_geometry 那三格 —— 名字换的是**族**，两臂各得其所。）
# α 取 0.25 与 0.5 两档：blend_sort 的教训是单个 α 能被「某种取整恰好蒙对」糊过去，
# 两个互不相同的 α 各命中一次才堵得掉。本靶不预测像素值，但同一枚纪律不花钱。
#
# ## 三条刻意的设计（每条都在堵一条"两臂差异不来自透明段"的旁路）
# 1. **shadow.lua mode="none"**：`collectShadowCasters` 排 blend ⟹ 开着阴影时 OPAQUE 臂的
#    pane 会投影、BLEND 臂的不投。那会让「双臂像素不同」在**透明段一条都没画**时也成立
#    （影子没了也是不同）= 一条恒真的判据。关掉阴影把这个旁路**结构性**摘掉。
#    ⚠ 代价：transparent.frag 的 sampleShadow1Tap 只走「因子恒 1.0」那一支 → 写进 honesty。
# 2. **pane 互不重叠、共面（同 z）**：每个 pane 独占一块屏幕区域，"这块像素是谁画的"没有二义，
#    反向用例（透明段条数 = 0）与像素判据的归因都是一一对应。多层叠加是 blend_sort 的活。
# 3. **env/light 都不归零**（与 blend_sort 正相反，那边是双保险、这边是被测对象本身）：
#    env 段还是**硬前置**（含 BLEND 图元而无 environment ⟹ 装配期 validateBlendSupport
#    直接抛，原因见 docs/透明混合管线_plan.md §4 倒数第二行）。
#
# ## 不覆盖什么（honesty 字段的展开，两处必须同步）
# 不预测任何像素值（弱判据）；不覆盖阴影遮挡（见上 1）、MASK discard、velocity/TAA
# （透明段不写深度）、picking（透明不入 picking 段）、GPU culling 候选（透明不入候选）、
# 多层叠加排序（那是 blend_sort）、自发光电平（债-131，本靶 emissive 恒 0 故根本不进算式）。
BLEND_LIT_CAM = {"position": (0.0, 0.0, 6.0), "target": (0.0, 0.0, 0.0), "fovDegrees": 45.0}
BLEND_LIT_SIZE = [1280, 720]
BLEND_LIT_Z = -1.0            # 三个 pane 共面（同 z）：屏幕 footprint 同尺度，区域无二义
BLEND_LIT_PANE = 2.0          # pane 边长（米）
BLEND_LIT_XS = (-3.0, 0.0, 3.0)  # 三个 pane 的 node 原点 x（互不重叠：2.0 宽、间距 3.0）
# 背景板：半尺寸 16 世界单位 @ z=-2（距相机 8）⟹ 该深度视锥半宽 2.95 / 半高 1.66，
# 铺满整屏且余量极大（改画幅也不穿帮）。**"背景板可见竖条"那条区域归因依赖它铺满**，
# 故写成常量并由 manifest 带出去给 imgqa 侧对账，不在 C++ 里重抄。
BLEND_LIT_BACK = 16.0
BLEND_LIT_BACK_Z = -2.0
# (piece 名, PAL 色, α, 设计意图)。名字里的 Foliage/Emissive 是**载荷**，不是手滑：
# material_styles 靠它把类别变体路由出来（见头注），故 style_routed 由名字派生（见下面构造处）。
_BLEND_LIT_PANES = (
    ("pane_pbr", "red", 0.25,
     "不命中 material_styles 任何 pattern ⟹ 回落 transparent 本体（pbr 前向）；α=0.25"),
    ("pane_Foliage", "green", 0.5,
     "名字含 Foliage ⟹ style=vegetation ⟹ BLEND 臂走 transparent_vegetation、"
     "OPAQUE 臂走 deferred_geometry_vegetation；α=0.5"),
    ("pane_Emissive", "yellow", 0.25,
     "名字含 Emissive ⟹ style=light ⟹ BLEND 臂走 transparent_light、OPAQUE 臂走 "
     "deferred_geometry_light；emissive 恒 None（债-131 的 ×1/4 不进算式）；α=0.25"),
)


def blend_lit_projection(z: float):
    """pane 所在深度 → (半高(米), 每米像素)。

    前提与 blend_sort 同一条：机位沿 −Z **精确**正视（无俯角无偏航）⟹ 世界 x/y → 屏幕 px
    是纯线性映射。前提不成立时本函数抛，而不是给个"看起来对"的近似 —— 采样点量错几何
    而闭式/判据照旧自洽，是这类仪器最贵的假绿。
    """
    pos = BLEND_LIT_CAM["position"]
    tgt = BLEND_LIT_CAM["target"]
    if abs(pos[0] - tgt[0]) > 1e-12 or abs(pos[1] - tgt[1]) > 1e-12 or pos[2] <= tgt[2]:
        raise ValueError("blend_lit 投影只覆盖「沿 −Z 精确正视」的机位")
    depth = pos[2] - z
    if depth <= 0.0:
        raise ValueError(f"blend_lit: pane 平面 z={z} 不在机位之前（depth={depth}）")
    half_tan = math.tan(math.radians(BLEND_LIT_CAM["fovDegrees"] / 2.0))
    half_h = half_tan * depth
    return half_h, (BLEND_LIT_SIZE[1] / 2.0) / half_h


def scene_blend_lit(blend: bool = True) -> Scene:
    arm = "blend" if blend else "opaque"
    s = Scene(
        name=f"blend_lit{'' if blend else '_opaque'}",
        purpose="透明段 lit 类别变体的常驻像素覆盖靶（债-134 先决）：与 blend_lit_opaque "
                "同源、只差 alphaMode，三支 lit 变体（pbr/vegetation/light）各占一块不重叠"
                "屏幕区域；配 imgqa blend-lit 用",
        honesty=(
            "弱判据输入：**不预测任何像素值**（透明光照的闭式要复刻 BRDF+IBL+CSM，台账裁"
            "「成本 > 收益」）。只证三件事——透明段有 draw、VVL 干净、交付图相对 OPAQUE 臂"
            "变了。⟹ 它证「透明段对交付图有贡献」，**不证「透明画对了」**（画对了归 "
            "blend-sort 的闭式与将来的强判据）。逐条不覆盖：① 阴影（shadow mode=none 是"
            "刻意的——collectShadowCasters 排 blend，开着阴影会让「双臂不同」在透明段一条"
            "没画时也成立），故 transparent.frag 的 sampleShadow1Tap 只走因子恒 1.0 那一支；"
            "② MASK discard（透明段没有 MASK 图元）；③ velocity/TAA（透明段不写深度，§4）；"
            "④ picking / object_id（透明不入 picking 段，§4）；⑤ GPU culling 候选（透明不"
            "入候选，§4）；⑥ 多层叠加与排序（pane 共面不重叠，那是 blend_sort 的射程）；"
            "⑦ 自发光电平（pane 的 emissive 恒 None，债-131 的 ×1/4 不进算式）；"
            "⑧ 法线贴图 / ORM / 湿面（无纹理面，且湿面调制只住在 deferred 路）。"),
        cameras={"lit": dict(BLEND_LIT_CAM)},
    )
    s.pieces.append(Piece(
        "backdrop", _xy_quad(BLEND_LIT_BACK), PAL["blue"], translation=(0.0, 0.0, BLEND_LIT_BACK_Z),
        radial=False, closed=False, roughness=0.9,
        intent="OPAQUE PBR 背景（**两臂都 OPAQUE**）：给混合提供 dst，也是「无 pane 参考点」"
               "所在。α 恒 1 = 两臂同值，不参与单变量"))
    for (name, color, alpha, intent), x in zip(_BLEND_LIT_PANES, BLEND_LIT_XS):
        # 旗由名字派生而不是逐枚手写：命中 pattern 的那两枚才是"设计过的路由"，
        # 手写会漂（改名字忘了改旗 → 要么白关防御、要么被反向检查打回）
        s.pieces.append(Piece(
            name, _xy_quad(BLEND_LIT_PANE), PAL[color],
            alpha=(alpha if blend else 1.0),
            style_routed=("Emissive" in name or "Foliage" in name),
            translation=(x, 0.0, BLEND_LIT_Z), radial=False, closed=False, roughness=0.35,
            intent=intent + ("" if blend else "（OPAQUE 臂：α 归 1，本件走 deferred 族）")))
    # ── 自检：背景板必须在自身深度上铺满整屏（"背景板可见竖条"的区域归因依赖它）────────
    # 不写死数字：用同一个投影算子在 backdropZ 处算视锥半尺寸，与 BLEND_LIT_BACK 比。
    bh, _ = blend_lit_projection(BLEND_LIT_BACK_Z)
    if BLEND_LIT_BACK <= bh * (BLEND_LIT_SIZE[0] / BLEND_LIT_SIZE[1]):
        raise ValueError(
            f"blend_lit: 背景板半尺寸 {BLEND_LIT_BACK} 盖不住 z={BLEND_LIT_BACK_Z} 处的视锥"
            f"（半宽 {bh * (BLEND_LIT_SIZE[0] / BLEND_LIT_SIZE[1]):.2f}）——"
            "「pane 之外的像素都是背景板」这条归因会失效")
    # ── 测量接口（harness 吃这里，不在 C++ 里重抄颜色/α/落位 = 第二真相）──────────
    # 只记**事实**：机位、画幅、pane 的世界落位与 α、期望的类别变体名。屏幕区域由 harness
    # 侧的投影算子算（与 blend_sort 同一条缝：口径住 imgqa，selftest 直接打）。
    s.extra["blendLit"] = {
        "arm": arm,
        "twin": "blend_lit_opaque" if blend else "blend_lit",
        "camera": dict(BLEND_LIT_CAM),
        "aspect": list(BLEND_LIT_SIZE),
        "paneZ": BLEND_LIT_Z,
        "paneSize": BLEND_LIT_PANE,
        "paneXs": list(BLEND_LIT_XS),
        "backdropHalf": BLEND_LIT_BACK,
        "backdropZ": BLEND_LIT_BACK_Z,
        "expectTransparentDraws": sum(1 for p in s.pieces if p.alpha < 1.0),
        "litVariants": ["transparent", "transparent_vegetation", "transparent_light"],
        "shadowMode": "none",   # 承重（不是省事）：见头注「刻意的设计」第 1 条
        "note": ("双臂同源：本文件同一个 scene_blend_lit(blend=) 生成，只差 alphaMode。"
                 "harness 必须先对账两臂 manifest 的几何/落位/颜色全等，再谈像素差。"),
    }
    return s


def scene_blend_lit_opaque() -> Scene:
    return scene_blend_lit(blend=False)


# ---------------------------------------------------------------------------
# 场景 10：shadow_grain —— 影场颗粒度的**槽影带**靶（imgqa shadow-grain 的常驻靶）
# ---------------------------------------------------------------------------
# ## 为什么必须新造靶（penumbra_ladder 教训的复刻，别删这段）
# 「影子随相机游动」修了九轮而指标全绿：`imgqa swim` 测的是**世界不变性**（同一个世界点的
# 影值变没变）——它测的性质是对的，且确实该绿（纹素吸附工作正常）。用户看到的是另一件事：
# 门板上的自阴影痤疮**钉死在世界表面**，相机一动屏幕像素就扫过它。现有判据网里没有任何
# 一条测得到「影场在世界空间上的高频程度」，本靶补的就是这个洞（判据 = `imgqa shadow-grain`，
# 口径与三条判据的理由住 tools/imgqa/shadow_grain.cpp 头注释）。
# bistro 门板是**发现地**但不是合格靶：固定屏幕矩形不可常驻（区域必须来自几何）。本靶把
# 发现地读图确认的几何形态（见下「机制」）**程序化重造**：槽的数量/深度/周期全部闭式，
# 判据窗口的密度上界有闭式刻度（2·count/(cols−1)），阳性锚点才有资格当闸门。
#
# ## 机制（2026-10-18 读图定案；更早的「掠射+normal map 椒盐」模型已被实测证伪）
# bistro 门板窗口的 72 个二值翻转**全部单向**（slope=0 遮 / slope=16 受光），列分布集中在
# 两列 = 门板的**竖直拼缝槽**（concave moldings）。槽机制闭式：前墙（朝光）+ 后退 d 的背板，
# 竖直缝槽内背板被前墙右缘遮光，影带宽度 s = d·du/dn（du = 光沿墙 u 轴分量、dn = 朝光分量），
# 带与受光区的**光深度差 = d·dn**。三股偏置里：
#   · 采样端常数 bias（**债-150 起住在世界光深域**：`kShadowDepthBiasWorld = 5.854 mm`，由
#     `shadowDepthBiasNdc` 除以本帧级 0 跨度换算成 NDC 后下发；shadow.slang 仍是
#     `ref = ndc.z − params.x`）与 normal offset（1.5 texel 沿法线，②）都把比较推向**受光**
#     ——带要活过 slope=0 档，要求 d·dn > 世界 bias + 1.5 texel 的补偿；
#   · 光栅端斜率偏置（`csm.slope_bias`，16·dZ/dpixel）把前墙的存储深度**推远**——带要在
#     slope=16 档被抹掉，要求 d·dn < 16·dn/1024 NDC + 上项。
# 合并成分离窗（span = 级联光空间跨度，texel = span/1024，W = 世界偏置 5.854 mm）：
#   W/dn + 1.5·span/1024 < d < W/dn + 17.5·span/1024
# 🔴 **债-150 改的就是这一行的第一项**：改前它是 `0.0015·span/dn`（跟着 span 线性漂），改后是
#   `W/dn`（世界常数，与 span 无关）。窗宽仍是 16·texel（dn 约掉），但**窗心的缩放律变了**
#   ——旧窗随机位距离整体缩放自相似，新窗的 bias 项不缩。本靶的槽深 d 是**按旧窗**排的
#   （dn=0.6 时旧窗 d ∈ (0.0040, 0.0196)·span），2026-10-26 随债-150 重跑 `imgqa shadow-grain`
#   三条判决线仍全绿（J1 最严一条存活率 100 %，下限 50 %）⟹ 靶仍然合格，**没有重排几何**；
#   若哪天要按新窗重排，那是另一次改动，得连 J1 的下限一起重立。
# 投影系统按 SceneBounds 自适应 near/far ⟹ span ∝ 机位距离 ⟹ texel 项仍随 D 缩放自相似。
# ⚠ 已证伪、别再试的两条（各烧过一轮实机）：
#   ① 「完美平面 + normal map 掠射椒盐」：平面上法线偏移**数学上永不**自遮
#     （stored − ref = δ·cosθ/dn > 0 恒受光），γ=8°/amp=20° 实测两档 cov ≡ 0；
#   ② 「与墙平行的薄板遮挡体」：薄板与墙同掠射，它的影被**自己的** slope 偏置抹掉
#     （b=3 实测影侧探针 0.000）。遮挡体若用「朝日竖板」则影缘噪声带（normal map × 杠杆臂）
#     淹没信号（T=30 m 实测两档同 0.0338）。
#
# ## 几何机关（每条都有构建期 assert，改任何一条 assert 会响）
#   ① 墙法线 N 水平且 N·L̂ = GRAIN_DN = 0.6（与 bistro 门同量级；太阳仰角 48° 限死
#      dn ≤ 0.667，「掠射」在机制里根本不需要——见上「已证伪①」）。方位分支取 du > 0
#      的那个（影带落在**每个缝槽的右缘**，闭式可算，assert ②）。
#   ② 机位沿 +N 正对墙心（视轴 = −N，up = Ŷ）⟹ 墙面 ⊥ 视轴 ⟹ 墙上映射是**精确的相似
#      变换**（px/m 各向同性），region 投出来是轴对齐屏幕矩形——区域来自几何（manifest
#      世界坐标角点 + 相机投影），不是手填屏幕矩形。
#   ③ 前墙 = 共面 quad 块拼成的**单 piece**（缝槽 = 相邻块之间的竖直缺口；合并是因为
#      调色板只有 10 色，墙块各自成 piece 会撞「piece 两两可分」校验——颜色不进判据，
#      区域来自几何）；背板 = **每缝深一块**比前墙**小**的 quad，各退本组 d 沿 −N。
#      region 内唯一影结构 = 全部缝的竖直带，每条 2 个二值跳变 ⟹ 闭式密度 = 2·count/(cols−1)。
#   ⑥ 背板覆盖 = 「穿过本缝的视线打在背板平面的落点区间」± 裕量，落点 = u·(D+d)/D（背板
#      比墙远 d，同一条视线打得更朝外）。只盖「缝本身」的背板会在外侧缝露出黑背景
#      （off=0 ⟹ cov=0 = 死缝）——2026-10-19 首版加密靶实机翻车记录；assert ⑤/⑤′ 钉住
#      「盖住落点区间」且「相邻背板不重叠」（重叠 = 谁可见变成深度竞争的二义）。
#   ④ 光线**向上**爬 d·Lv/dn（太阳仰角 48° > 45° ⟹ 该值 > d）：带在背板上存在的前提是
#      入射光线被**墙顶**放走之前先穿过缺口 ⟹ 墙顶必须高出 region 上界 ≥ d·Lv/dn + 裕量
#      （assert ④；漏了这条 = region 上部两档同受光、密度掉档）。
#   ⑤ 光 = 交付默认 SUN（ProfileOpts::lightDir 同值），机位/帧号/尺寸都是量具的组成部分。
#
# ## 靶子加密：从单缝深到多缝深（2026-10-19 本批，用户点名）
# 首版只有 0.30 m 一个缝深——出厂 slope_bias 定档 8 的实测表（configs/shadow.lua）只证明了
# 「d=0.30 的真影带在 8 之下存活率 100%」，而 `rgl/shadow_config.cppm::slopeBias` 注释自己
# 登记了盲区：「更细的缝会不会在 8 之下就被提前抹掉，现在没有东西在守」。本靶把缝深做成
# **多档交错排在同一行**（每档一条缝；**不是**每组四条横向铺开——铺到 5 m 宽会撑大
# SceneBounds、推高级联 span、分离窗整体右移，d=0.30 在 slope=0 下都被 PCF 糊死，
# 实机翻车记录写在 GRAIN_GROOVES 常量注释里）。每条缝独立出密度/存活率读数、独立过
# slope=0 在场闸门——画不出带的死缝由闸门抛（rc=3），绝不许静默进判据
# （penumbra_ladder 的「靶瞎了与带被正确抹除给出同一个绿」教训）。
# 缝深选点判据（分离窗见上「机制」；span 由引擎级联决定、生成器不可知）：
#   · 下界：d > W/dn + 1.5·span/1024（slope=0 存活；W = 世界偏置 5.854 mm，债-150 起不再随
#     span 缩放）。本靶（宽 ≈1.29 m）实测定锚：
#     d=0.10 在 slope=0 仍**在场**（探针 0.834、存活率 1.000）⟹ 下界 < 0.10（旧窄靶上
#     「d=0.12 cov 0.345」的锚点随 span 标定作废，别照抄）。再细的缝先撞 assert ⑧
#     （带宽 < 4 px 探针放不下），进不了靶。
#   · 带宽下限：band·s_px ≥ 4 px（assert ⑧）——在场探针取闭式带中央 50%，< 4 px 连 2 px
#     探针都放不下。
#   · 上界（slope=16 抹除）与「8 之下提前抹除」正是本靶要**测**的，不是选点约束。
# 定稿缝深 [0.30, 0.24, 0.20, 0.17, 0.14]（2026-10-19 实机扫描：五条全部 slope=0 在场；
# slope=8 出厂档下 d≤0.20 被整条抹光、d=0.24 带缘抖动——J1 因此**红**，那是本靶要守的
# 新发现，读数表住 tools/imgqa/shadow_grain.cpp 头注「跑起来该看到什么」）。


GRAIN_DN = 0.6                 # 墙朝光分量 N·L̂（分离窗下界含 W/dn 项，见头注「机制」）
GRAIN_CENTER = (0.0, 2.0, 0.0)  # 墙心（离地 2 m 纯取景考虑，场景无地面）
GRAIN_D = 6.0                  # 机位沿 +N 距墙心（米）；span ∝ D，整套几何随它自相似
GRAIN_FOV = 70.0               # 竖直 fov（度）；s_px = 360/(D·tan(fov/2))，密度闭式的 px 刻度
GRAIN_CAPTURE = (1280, 720)    # 采集尺寸（imgqa 仪器写死同值；assert ⑥ 按它算取景）
# 缝深组（米）：**分离窗里唯一没被 assert 钉死的量**——span 由引擎级联决定，生成器不可知。
# 每个缝深一条缝、按深度**交错排在同一行**（不是每组四条横向铺开——实测横向铺到 5 m 宽
# 会把 SceneBounds 撑大 → 级联 span 变大 → 分离窗整体右移，d=0.30 的带在 slope=0 下都被
# PCF 糊到 cov 0.36，2026-10-19 首版加密靶实机翻车记录）。总宽必须保持 ≈ 首版 1.15 m 量级
# 才不漂 span。选点判据与实测锚点见头注「靶子加密」；改它必须重跑 `imgqa shadow-grain`
# 确认每条缝 slope=0 在场（画不出带的死缝会被仪器闸门抛，不许静默进判据）。
GRAIN_GROOVES = [0.30, 0.24, 0.20, 0.17, 0.14]
GRAIN_SEG = 0.10               # 相邻缝之间的共面墙块宽（litProbe 取其中一块的中心）
GRAIN_SLIVER = 0.04            # 带左缘到缝左缘的受光窄条（带在缝右缘，见机关①）
GRAIN_BACK_MARGIN = 0.05       # 背板在「缝的视线落点区间」外的覆盖裕量（米，见机关⑥）
GRAIN_REGION_END = 0.04        # 最外缝的 cell 缘到 region 缘的裕量（米）
GRAIN_REGION_H = 0.50          # 判据窗口高（米）


def grain_frame() -> dict:
    """靶的闭式几何帧（墙正交标架 + 槽/带/region 尺寸）。纯函数，assert 与生成共用。"""
    L = _norm((-SUN[0], -SUN[1], -SUN[2]))          # 指向光源
    Lv = L[1]
    dn = GRAIN_DN
    hL = math.sqrt(1.0 - Lv * Lv)                   # |L̂ 的水平分量|
    a = dn / hL                                     # N 在太阳方位上的投影系数
    assert a < 1.0, f"shadow_grain: ① dn={dn} 超过太阳方位上限 {hL:.3f}（墙做不出来）"
    h = _norm((L[0], 0.0, L[2]))
    p = (h[2], 0.0, -h[0])                          # 水平 ⊥ h
    b = math.sqrt(max(0.0, 1.0 - a * a))
    N = U = None
    du = 0.0
    for bb in (b, -b):                              # 两个方位解，取 du > 0 的（带在缝右缘）
        N = (a * h[0] + bb * p[0], 0.0, a * h[2] + bb * p[2])
        U = (N[2], 0.0, -N[0])                      # u = Ŷ × N
        du = U[0] * L[0] + U[1] * L[1] + U[2] * L[2]
        if du > 0:
            break
    groups = []
    for d in GRAIN_GROOVES:                       # 每个缝深一条缝（交错排行，见常量注释）
        band = d * du / dn                        # 槽底影带宽 s（头注「机制」闭式）
        gapW = band + GRAIN_SLIVER                # 缝宽
        groups.append({"depth": d, "band": band, "gapW": gapW, "cell": gapW + GRAIN_SEG})
    totalW = sum(g["cell"] for g in groups)
    u = -totalW / 2.0
    for g in groups:                              # 缝的 u 区间（墙心为原点）
        g["gapLo"], g["gapHi"] = u, u + g["gapW"]
        g["cellLo"], g["cellHi"] = u, u + g["cell"]
        g["bandU"] = (g["gapHi"] - g["band"], g["gapHi"])   # 带 = 缝右缘往左 s（背板坐标）
        # 机关⑥：相机穿过缝 [gapLo,gapHi] 的视线打在**退 d** 的背板上，落点 u·(D+d)/D
        # （比缝本身更朝外）⟹ 背板必须盖住落点区间，否则缝里看到的是黑背景（cov 恒 0 =
        # 死缝；2026-10-19 首版加密靶「背板只盖组跨度」实机翻车记录）。
        k = (GRAIN_D + g["depth"]) / GRAIN_D
        hits = sorted((g["gapLo"] * k, g["gapHi"] * k))
        g["backLo"], g["backHi"] = hits[0] - GRAIN_BACK_MARGIN, hits[1] + GRAIN_BACK_MARGIN
        u = g["cellHi"]
    regionW = totalW + 2.0 * GRAIN_REGION_END
    regionH = GRAIN_REGION_H
    climb = max(g["depth"] for g in groups) * Lv / dn   # 机关④（最深缝决定墙顶余量）
    wallW = regionW + 0.30
    wallH = regionH + 2.0 * (climb + 0.05)
    s_px = (GRAIN_CAPTURE[1] / 2.0) / (GRAIN_D * math.tan(math.radians(GRAIN_FOV / 2.0)))
    return {"L": L, "N": N, "U": U, "dn": dn, "du": du, "Lv": Lv, "groups": groups,
            "totalW": totalW, "climb": climb,
            "regionW": regionW, "regionH": regionH, "wallW": wallW, "wallH": wallH,
            "pxPerMeter": s_px}


def scene_shadow_grain() -> Scene:
    gf = grain_frame()
    N, U = gf["N"], gf["U"]
    C = GRAIN_CENTER
    groups = gf["groups"]
    rw, rh = gf["regionW"], gf["regionH"]
    wallW, wallH = gf["wallW"], gf["wallH"]

    def wall_point(u: float, v: float) -> tuple:
        return (C[0] + u * U[0], C[1] + v, C[2] + u * U[2])

    def back_point(u: float, v: float, d: float) -> tuple:   # 背板（退 d 沿 −N）
        return (C[0] + u * U[0] - d * N[0], C[1] + v, C[2] + u * U[2] - d * N[2])

    allGaps = [(g["gapLo"], g["gapHi"]) for g in groups]
    # 前墙块 = 全部缺口在墙上的补集。
    edges = [-wallW / 2.0]
    for lo, hi in allGaps:
        edges += [lo, hi]
    edges += [wallW / 2.0]
    segs = [(edges[2 * i], edges[2 * i + 1]) for i in range(len(allGaps) + 1)]
    # litProbe = 中间墙块中心（恒受光参照；离一切带缘 ≥ 3 px，assert ⑩）。
    joints = [(groups[i]["gapHi"] + groups[i + 1]["gapLo"]) / 2.0 for i in range(len(groups) - 1)]
    assert joints, "shadow_grain: 缝数 < 2，litProbe（墙块中心）无处可放"
    probeU = joints[len(joints) // 2]

    s = Scene(
        name="shadow_grain",
        purpose="影场颗粒度靶：朝光墙 + 多缝深交错缝槽（每个缝深一条竖直缝、背板各退本组 d），"
                "槽底闭式影带；逐带存活率闭式 = 跳变/(2·rows)（imgqa shadow-grain）",
        honesty=("前墙几何严格共面，唯一的深度结构 = 缝槽里的背板（缝深组 GRAIN_GROOVES 是"
                 "分离窗里唯一没被 assert 钉死的量，选点判据与实测锚点见源码头注「靶子加密」；"
                 "更细的缝画不出带，会被仪器闸门抛掉，不许静默进判据。总宽保持 ≈ 首版量级是"
                 "刻意的——横向铺开会撑大 SceneBounds、推高级联 span、把分离窗整体右移，"
                 "2026-10-19 实机翻车记录写在常量注释里））。级联 texel 尺度随相机自适应"
                 "漂移，本靶的机位/尺寸/帧号都是量具的组成部分（imgqa 仪器写死同值）；"
                 "换机位 = 换靶，闭式刻度与全部 assert 都要重推。带在缝的**右缘**"
                 "（du > 0 的方位分支，构建期钉），左右不对称是设计不是漂移。"),
        cameras={"grain": {"position": [_f32(C[i] + GRAIN_D * N[i]) for i in range(3)],
                           "target": list(C), "fovDegrees": GRAIN_FOV}},
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（= ProfileOpts::lightDir 同值）"},
            "wallDn": GRAIN_DN,
            "grooves": [{"depthM": _f32(g["depth"]), "bandM": _f32(g["band"]),
                         "gapWM": _f32(g["gapW"]), "cellM": _f32(g["cell"]),
                         "segM": GRAIN_SEG, "sliverM": GRAIN_SLIVER,
                         "backUM": [_f32(g["backLo"]), _f32(g["backHi"])]} for g in groups],
            "closedForm": {
                "bandU": [[_f32(x) for x in g["bandU"]] for g in groups],
                "pxPerMeter": gf["pxPerMeter"],
                "densityRegion": f"{len(groups)} 缝 × 2 缘 ⟹ region 每行 {2 * len(groups)} "
                                 "次二值跳变 ⟹ density = 2·count/(cols−1)",
                "survival": "带存活率 = 闭式带±裕量内的水平跳变数 / (2·rows)"
                            "（带在场闭式 = 1、被抹 = 0；分母是闭式刻度，不抄实测）。"
                            "存活窗/探针窗的列区间由仪器从 bands 角点投影导出（±3 px 裕量"
                            "与中央 50%），生成器 assert ⑪ 钉住「邻带存活窗互不重叠」",
                "separationWindow": "W/dn + span·(1.5/1024, 17.5/1024)，W = 世界偏置 5.854 mm"
                                    "（slope=0 存活下界 / slope=16 抹除上界，头注「机制」；"
                                    "债-150 前第一项写作 0.0015·span/dn，随 span 线性漂）",
            },
            # ── 测量接口（imgqa shadow-grain 读本块，**不在 C++ 里重抄几何** = 不造第二真相）──
            "measure": {
                "regionCorners": [[_f32(x) for x in wall_point(u, v)] for u, v in
                                  ((-rw / 2, -rh / 2), (-rw / 2, rh / 2),
                                   (rw / 2, rh / 2), (rw / 2, -rh / 2))],
                # 每个缝深组 = 本组各带（bands = 背板深度上的两个世界角点，v 下缘 / v 上缘；
                # 仪器投影成列区间，存活窗 = 列区间 ± 裕量、探针 = 中央 50%）。
                "groups": [{
                    "depthM": _f32(g["depth"]),
                    "bands": [[[_f32(x) for x in back_point(g["bandU"][0], -rh / 2.0, g["depth"])],
                               [_f32(x) for x in back_point(g["bandU"][1], rh / 2.0, g["depth"])]]],
                } for g in groups],
                "litProbe": [_f32(x) for x in wall_point(probeU, 0.0)],
                "captureSize": list(GRAIN_CAPTURE),
            },
        },
    )
    P = s.pieces.append

    def piece(quads, name, intent, color):
        """多 quad 一 piece（前墙 = 全部墙块合并：调色板只有 10 色，墙块各自成 piece
        会撞「piece 两两可分」校验；颜色不进判据——区域来自几何、cov 同像素约掉 albedo）。"""
        pos, idx = [], []
        for q in quads:
            b = len(pos)
            pos += [tuple(_f32(x) for x in p) for p in q]
            idx += [b, b + 1, b + 2, b, b + 2, b + 3]
        P(Piece(name, Geom(pos, [tuple(_f32(x) for x in N) for _ in pos], idx),
                PAL[color], roughness=0.9, radial=False, closed=False, intent=intent))

    # 前墙（绕序：p(u0,v0)→p(u1,v0)→p(u1,v1)→p(u0,v1)，cross ∥ +N，assert ⑨ 兜底）。
    piece([[wall_point(u0, -wallH / 2.0), wall_point(u1, -wallH / 2.0),
            wall_point(u1, wallH / 2.0), wall_point(u0, wallH / 2.0)] for u0, u1 in segs],
          "wall_front",
          f"前墙（共面，朝光 dn={GRAIN_DN}，{len(segs)} 块合并一 piece）：恒受光 = 密度地板"
          f"与受光参照；缺口 = {len(allGaps)} 条缝（缝深 {GRAIN_GROOVES} 交错排）",
          color="red")
    # 各缝背板：盖住「穿过本缝的视线在背板上的落点区间」± 裕量（机关⑥），且互不重叠
    # （assert ⑤）——只在**本缝**里可见。
    back_colors = ["yellow", "orange", "chartreuse", "green", "cyan", "azure", "magenta",
                   "purple"]
    for gi, g in enumerate(groups):
        piece([[back_point(g["backLo"], -(rh + 0.10) / 2.0, g["depth"]),
                back_point(g["backHi"], -(rh + 0.10) / 2.0, g["depth"]),
                back_point(g["backHi"], (rh + 0.10) / 2.0, g["depth"]),
                back_point(g["backLo"], (rh + 0.10) / 2.0, g["depth"])]],
              f"groove_back_{gi}",
              f"缝深 {g['depth']:.2f} m 的背板（退 d 沿 −N，盖住视线落点区间，机关⑥）："
              "slope=0 档带被前墙右缘遮光、大 slope 档被斜率偏置抹除——出厂档存活率判据"
              "就发生在它的像素上",
              color=back_colors[gi % len(back_colors)])

    # ---- 构建期断言（口头承诺不算数；编号对应源码头注「几何机关」+「靶子加密」）----
    s_px = gf["pxPerMeter"]
    assert gf["du"] > 0.05, f"shadow_grain: ② du={gf['du']:.3f} ≤ 0.05，带侧分支失稳"
    assert GRAIN_SLIVER >= 0.02 and GRAIN_SEG * s_px >= 6.0, \
        "shadow_grain: ③ sliver < 2 cm 或墙块 < 6 px（带贴缝左缘 / litProbe 邻域跨线）"
    assert wallH / 2.0 >= rh / 2.0 + gf["climb"] + 0.04, \
        f"shadow_grain: ④ 墙顶余量不足（climb={gf['climb']:.3f} m，region 上部的带会被" \
        "入射光从墙顶放走 = 两档同受光）"
    assert all(abs(g["backLo"]) < wallW / 2.0 and abs(g["backHi"]) < wallW / 2.0
               for g in groups), \
        "shadow_grain: ⑤ 背板没被前墙包住（墙缘外会露背板）"
    assert all(groups[i]["backHi"] < groups[i + 1]["backLo"] for i in range(len(groups) - 1)), \
        "shadow_grain: ⑤′ 相邻背板重叠（不同深度的板在同一视线区间 = 谁可见变成深度竞争的" \
        "二义，闭式带不再成立——缝太密或背板裕量太大）"
    assert rw / 2.0 * s_px <= GRAIN_CAPTURE[0] / 2 - 16 \
        and rh / 2.0 * s_px <= GRAIN_CAPTURE[1] / 2 - 16, \
        f"shadow_grain: ⑥ region 投出画外（px/m={s_px:.1f}，裕量 16 px）"
    assert rw * s_px >= 48.0 and rh * s_px >= 40.0, \
        "shadow_grain: ⑦ 判据窗口 < 48×40 px（分母太小，闭式刻度失稳）"
    assert all(g["band"] * s_px >= 4.0 for g in groups), \
        "shadow_grain: ⑧ 某缝带宽 < 4 px（PCF ±0.5 texel 抹平带不可见，且中央 50% 探针放不下" \
        " 2 px——该缝深超出本靶可测范围，删组或换机位重推，不许留死缝）"
    cross_un = (U[1] * 0.0 - U[2] * 1.0, U[2] * 0.0 - U[0] * 0.0, U[0] * 1.0 - U[1] * 0.0)
    assert sum(cross_un[i] * N[i] for i in range(3)) > 0.99, \
        "shadow_grain: ⑨ 绕序/法线核对失败（cross(U,Ŷ) ≠ N）"
    bandEdges = [e for g in groups for e in g["bandU"]]
    assert min(abs(probeU - e) for e in bandEdges) * 0.95 * s_px >= 3.0, \
        "shadow_grain: ⑩ litProbe（墙块中心）离某带缘 < 3 px（含 5% 背板透视收缩），探针邻域会跨线"
    assert (GRAIN_SEG + GRAIN_SLIVER) * 0.9 * s_px >= 10.0, \
        "shadow_grain: ⑪ 邻带存活窗重叠（带缘到邻带缘 < 10 px，±3 px 存活窗会互吃跳变）"
    assert GRAIN_BACK_MARGIN >= 0.03, \
        "shadow_grain: ⑫ 背板覆盖裕量 < 3 cm（机关⑥ 落点区间的容差没了）"
    return s


# ---------------------------------------------------------------------------
# 场景 10：shadow_edge_rig —— 影边世界位置的**绝对尺**（真值 = 算术，不是参照渲染）
# ---------------------------------------------------------------------------
# ## 为什么必须有它（主 agent 立项原文，别删）
# 本会话已建的两台阴影判据（`shadow-cascade-seam` / `shadow-caster-survival`）**都是
# 「拿一次渲染比另一次渲染」**（级联比级联、D=5 比 D=-1），结构上看不见**共模错误**。
# 已被实测证实的两次翻车：
#   ① 差分仪器对「四级全走 LOD」（一个肉眼可见在丢接触阴影的形态）给出 **满分 0.000**
#     ——四级一起变差时级间没有分歧。
#   ② 绝对仪器的**本影面积存活率是反向误导的**：那个坏形态读 **1.0022（比参照还"好"）**
#     ——粗簇轮廓比原始几何略胖，把丢掉的影子补回来还倒赚。
# **根因是参照本身也是渲染。** 本靶把真值换成算术：本文件上方
# `SUN = (0.5,-0.75,-0.45)`（= configs/scene.lua light.direction，仰角 48.11°）与
# `SHADOW_OFF = (SUN.x/−SUN.y, SUN.z/−SUN.y) = (0.6667,-0.6)`（单位高度的影位移），
# 于是**高度 H 的竖直挡板，其远侧影边落在 u = 板远侧面 + |SHADOW_OFF|·H**
# （u = 光的水平行进方向单位线；|SHADOW_OFF| = 0.90140）。闭式，无参照渲染。
# 仪器 = `imgqa shadow-edge-ruler`（算子层 shadow_edge_metric.*，臂表与闸门住仪器层）。
#
# ## 几何机关（编号 = 构建期 assert 的编号，改任何一条 assert 会响）
#   ① 零起伏平地 y=0（**不许任何起伏**——顺带结掉债-139 那个卡在「鹅卵石起伏」上的
#      2.73pp 残差）：唯一接收面、同材质。闭式解的前提「接收面水平」由几何本身钉死。
#   ② 一排竖直挡板：长边 ⊥ 光的水平行进方向（把影边钉成直线、且垂直于扫描线 → 精度
#      最高）；**厚度 = 本靶自变量**；高度 H 全同。每板 = 1 piece = 1 primitive =
#      独立簇 DAG——阴影簇 LOD 的选级是**逐图元** `min(D, 自身最深级)`
#      （rgl/main_meshlet_tables.cppm 启动对账行），若把八块板合成一个 primitive，
#      它们共享同一张 DAG、在同一级一起塌，自变量在物理上失去意义。
#      每板 ~1920 三角（大面 36×12 网格）：>128 三角才有 ≥2 个 L0 簇、DAG 才有深度
#      （1 簇的 primitive 恒走 L0 = 全量几何 = 恒绿仪器）；cornell_box 2242 三角出
#      6 级、penumbra_ladder 50 三角只出 1 级——**靶必须有足够三角数才会真的走 LOD**。
#   ③ 板沿 u（= 光的水平行进方向 = 机位深度方向）排开 ⟹ 各板影边落在不同级联，
#      「误差随板序」曲线天然就是「误差随距离/随级联」曲线，级联交界 = 曲线台阶。
#   ④ 机位在**下游**（影侧）低位朝上游看：某板影边的视线**不经过该板自己**（板在影边
#      之后），只可能被更靠近相机的板的上缘挡住——净空约束比上游机位松一个量级
#      （上游机位要求相机高 ≈ 2× 排长才能从板顶上方看到自己的影边）。
#      机位 = 离线网格搜索的解（搜索空间：H × 间距 × fov × 相机下游距 B × 高 Y × 目标
#      分位，步长 0.05；判据 = assert ⑤–⑩ 全部通过 + 净空/纹素/px 刻度排序取优），
#      常数抄录在下面；改厚度档/H/间距 = 机位作废，构建期 assert 当场抓红，重搜再改。
#
# ## 闭式真值与扫描线（manifest 的 measure 块 = 仪器唯一读数接口）
#   板 i 远侧面 u_far,i = u_i + t_i/2；**真值影边 u_e,i = u_far,i + |SHADOW_OFF|·H**。
#   扫描线沿 u：[u_e − s_up, u_e + s_dn]，s_up = 0.45·|SHADOW_OFF|·H（上游不吞接触影缘）、
#   s_dn = 0.45·gap（下游不吞邻板影，gap = 本影边到邻板近侧面）。cov 沿扫描线的
#   **50% 过零点** = 实测影边；窗口内多个过零点 = 前提破了（吞进接触缘/邻影）⟹ 仪器抛。
#   带宽（接触缘到远影缘 = |SHADOW_OFF|·H）≥ 6× 最粗级纹素：窄带被 PCF 糊成矮包后
#   50% 交叉点不再是影边位置（闭式解在「半影 ≳ 带宽」处失效）——assert ⑩。
#
# ## 级联自证（机位不靠肉眼看图，全部构建期 assert；公式链 = 引擎复刻，同源
#   cascade_self_proof + render_list_impl/scene_context 的现实现）
#   SceneBounds = 逐 primitive 去索引顶点 enclosingSphere（质心+最远点）的 unionSphere
#   → near = max(0.05, dist−R)、far = dist+R（projectionSystem）
#   → **向上取整到 2 的幂**（quantizeShadowDistance，债-139）
#   → practical split（log/uni 各半，count=4）→ 各级远截止 splits[c]
#   → 级 c 外接球半径 r_c = frustumBoundingSphere(nearC, farC)（姿态无关闭式）
#   → **texel_c = 2·r_c/1024**（图集 2048、4 级 2×2 分块 → 块边长 1024）。
#   断言：⑤ 全部影边视深 < 0.85·shadowFar（末级淡出从 t=0.866 起吃 cov——5a-9 的
#   atten，落进淡出带的读数会系统性偏「受光」）；⑥ 影边跨 ≥3 枚级联（自然渲染的误差
#   曲线必须有台阶，否则仪器没有自变量）；⑦ 视线净空：每个扫描窗点到相机之间的一切
#   板顶缘之上 ≥ 3 mm；⑧ 窗口隔离：窗口不含他板实体、不含他影边（±20 mm 界外）；
#   ⑨ 取景：全部窗口角点在画内且离画边 ≥ 30 px（按 captureSize 算）；
#   ⑩ 带宽 ≥ 6×texel_max 且 s_up ≥ 2.5×texel_max。
#
# ## 自变量定义域 = 实测扫描（不是猜的）——🔴 扫描已跑完，结论 = 本对组合对 LOD 免疫
# 厚度扫描（8 档 3…300 mm，`EDGE_THICKNESSES_SCAN`）在 15 臂下的原始读数全录于
# `EDGE_SCAN_READINGS`（并随 manifest selfProof.thicknessScan 落盘）。两条推翻设计前提：
#   ① **影边对 D 结构性不动**：D 六臂 PNG 逐位全等；引擎成本表显示 D=4 已塌到 1 簇/prim
#     （6.2% 三角）而影不变 ⟹ 凸平板簇化只删面内部三角、轮廓恒存活（剪影不变性）。
#     「DAG 有深度」≠「影边可动」——本靶的自变量在物理上不存在。
#   ② **地面影浅**：太阳只占地面受光 ~30%（env 1.0 天光补满）⟹ cov 峰值 0.303（字节域），
#     50% 过零点结构上不存在（112 个 (臂, 板) 组合全被闸门 C 点名）。bistro 的墙影深度
#     （太阳占比 >50%）不可外推到地面。
# 定稿档位（EDGE_THICKNESSES）因此**不存在合法取值**——修法 = 换接收面（朝日立墙）+
# 换遮挡体形态（板+细鳍，鳍在粗级死亡才让影边动），属靶重设计，裁决权在主 agent（F4：
# 两臂同形即停，不 tune 判据）。
#
# ## 变异红线（改坏哪里会让本靶立刻构建失败；逐条实跑记录见交付说明）
#   · 把某板 u 挪出 shadowFar 射程 → assert ⑤ 红（该档将永远量不到，闸门 ③ 是运行期
#     兜底，构建期就该拦）。
#   · 把机位 Y 压低 5 cm → assert ⑦ 红（视线擦板顶，影边被板体挡一半 = 读数系统性
#     偏上游而曲线形状正常——最难归因的一种假数）。
#   · 把某板间距压到 band+t+2·s 以下 → assert ⑧ 红（窗口吞邻影 = 双过零点）。
#   · 把 H 改矮到带宽 < 6×texel → assert ⑩ 红（PCF 把窄带糊成矮包，50% 点不再是边）。
#   · 把地面加起伏 → 本靶**写不出**起伏（quad 平面）——前提「接收面水平」是几何事实。
#
# ## 远工作点 `shadow_edge_rig_far`（2026-10-25 相对坐标批 5 第 2 刀加）
# 出厂点 |camera| ≈ 8 m 时门关态精度本来就够 ⟹ 只验「rebase 开不破坏出厂点」会放行一个
# 「线接完了但一分精度都没买到」的实现。远工作点 = 整靶沿世界 +X 平移 1e6 m（float32 的
# ULP = 0.0625 m = 123 枚最粗级纹素），门关态在那里**应当**是坏的 —— 「开比关好」才是
# rebase 的存在理由。仪器 = `imgqa shadow-edge-rebase`（四格对拍：{出厂, 远} × {门关, 门开}）。
#   ⑪ 纯平移自证（构建期）：工作帧的 eye/tgt/sceneBounds/逐板 far·edge 必须恰等于零帧 +
#      world_offset，且视深/预测级联/splits/texel 必须与零帧**相同**（它们是相对量）。
#      前一半拦「真值漏挪」（漏挪 = 影边误差凭空多出 1e6 mm，而 ⑤–⑩ 一条都不红）；
#      后一半拦「闭式解里藏着未声明的绝对坐标假设」（那才是靶撑不住平移，当场红）。
#   · 变异红线：把 measure 里任意一处 `edge_u2w(..., off)` 的 `off` 删掉 → ⑪ 红；
#     把 `EDGE_FAR_OFFSET` 改成非水平（动 y）→ ⑪ 的 far/edge 半边红（地面接收关系破了）。
#   🔴 大坐标住在 **entity 的 position**，不烘进 GLB 顶点（顶点是 float32，烘进去 = 资产
#      自己带 ±31 mm 误差，而 `toRelative` 减的是变换平移列、碰不到顶点 ⟹ 那 31 mm 任何
#      原点方案都消不掉）。实测两种形态在 1e6 下都「阴影整块消失」，所以这不是绕开失效。

EDGE_H = 0.17                              # 板高（米）；带宽 = 0.9014·H = 153 mm
EDGE_SPACING_K = 1.6                       # 间距 = K·(带宽 + t_max + 40mm)（窗口隔离的闭式下界）
EDGE_SPACING_PAD = 0.04                    # 间距余量（米）
EDGE_LV_K = 4.0                            # 板长边 = max(K·H, 0.4)（≥4H 让窗口 v 线不越板缘）
EDGE_CAPTURE = (1600, 900)                 # 采集尺寸（imgqa 仪器写死同值；assert ⑨ 按它算）
EDGE_FOV = 40.0                            # 竖直 fov（度）
EDGE_CAM_B = 2.9                           # 相机在最后板下游 B 米（沿 u）
EDGE_CAM_Y = 4.6                           # 相机高（米）
EDGE_CAM_TGT_FRAC = 0.65                   # 目标 u = 首影边 + frac·(末影边 − 首影边)
EDGE_S_UP_K = 0.45                         # 窗口上游半宽 = K·带宽
EDGE_S_DN_K = 0.45                         # 窗口下游半宽 = K·gap
EDGE_SCAN_B = 2.9                          # 扫描版机位（搜索解，见机关④）
EDGE_SCAN_Y = 4.6
EDGE_SCAN_TGT_FRAC = 0.65
EDGE_SCAN_FOV = 40.0
# 厚度扫描档（米）：跨 3mm…300mm 等比 ×2 —— 自变量定义域的**探测集**，不是定稿。
EDGE_THICKNESSES_SCAN = [0.003, 0.006, 0.012, 0.025, 0.05, 0.1, 0.2, 0.3]
# 定稿档位（米）：跨越扫描拐点（原始读数见 selfProof.thicknessScan）。
EDGE_THICKNESSES = [0.003, 0.006, 0.012, 0.025, 0.05, 0.1, 0.2, 0.3]
# ── 扫描原始读数（2026-10-20 首跑，`shadow-edge-ruler` 15 臂，逐字记录，不做美化）──────
# 🔴 扫描的结论 = **本靶设计前提被实测推翻**（F4 停调口径红线：两臂同形即停，不 tune 判据）：
#  ① D 逐位无差异：d-1/d0/d3/d5/d7/d11 六臂 PNG md5 全等（9b78bac4…），f1d5 == f1d-1
#     （ab390d2c…）。引擎成本表给出机理：D=4 时全资产只剩 9 簇 / 658 三角（占 L0 6.2%，
#     每 prim 恰 1 枚根簇）而影**逐位不变** ⟹ 凸平板的簇化只删 ±u 面内部三角、**轮廓
#     （rim 边界）恒存活** ⟹ 影边在世界坐标下对 D 结构性不动。DAG 有深度（5 级）≠ 剪影可变。
#  ② 影浅：地面受光里太阳只占 ~30%（env 1.0 桥景天光补满）⟹ cov 峰值 0.303（字节域）/
#     ≈0.56（线性域），50% 过零点贴着平台、结构上时有时无 ⟹ 112 个 (臂, 板) 组合全部
#     找不到过零点（闸门 C 如实抛，逐条点名）。bistro 墙面影深（太阳占墙面 >50%）不可
#     外推到地面——定口径时就是这么错估出「1–2% 覆盖」的。
#  ③ 逐板 umbra（cov>0.5，线性域）面积：自然臂 0.17%、f0 0.11%、f1/f2 0.17%、f3 0.01%。
EDGE_SCAN_READINGS: dict = {
    "date": "2026-10-20",
    "arms": "none + 自然 D∈{-1,0,3,5,7,11} + 钉级 force∈{0,1,2,3}×D∈{5,-1}（15 臂）",
    "perPlateErrorMm": {f"t{int(t*1000):03d}": "no-crossing(全部 14 条 csm 臂)"
                        for t in EDGE_THICKNESSES_SCAN},
    "dArmsBitIdentical": {"md5": "9b78bac498bc915ce85772fe80841d61",
                          "arms": ["d-1", "d0", "d3", "d5", "d7", "d11"]},
    "pinnedD5eqDm1": {"md5": "ab390d2c4a474486cf31763a9efc5fc5", "cascade": 1},
    "covMaxByte": 0.303,
    # 线性域（主 agent 独立复算修正——首报把字节域 0.303 当结论、又手推了个 0.56，域引错了）：
    "covMaxLinear": 0.510, "covP99Linear": 0.367, "covMeanLinear": 0.004,
    "umbraAreaPct": {"natural": 0.17, "f0": 0.11, "f1": 0.17, "f2": 0.17, "f3": 0.01},
    "engineCostTable": "D=0:173簇/10626三角 · D=1:87/5310 · D=2:41/2654 · D=3:24/1326 · "
                       "D=4:9/658(6.19%) · D=5: 回落自身最深级=4（9 prim 全回落）",
    "verdict": "F4 停：影边对 D 结构性不动（剪影不变）+ 地面影浅（50% 阈值不可达）。"
               "厚度拐点不存在——不是阈值要调，是「地面 + 凸平板」这对接收面/遮挡体组合"
               "对 shadow LOD 免疫。",
    # ── 终跑（E1/E2 改版后：相对阈值 + 10 臂，2026-10-20，rc=0 五闸门全过）──────────────
    # 上面各键 = 首跑（15 臂绝对 0.5）的负结果记录，保留不删（honesty ⑤ 的证据链）。
    "finalRun": {
        "arms": "none + 自然 D=5 + 钉级 force∈{0,1,2,3}×D∈{5,-1}（10 臂）",
        "seamMm": {"c0-c1": {"median": 9.82, "max": 13.68},
                   "c1-c2": {"median": 10.32, "max": 16.61},
                   "c2-c3": {"median": 9.99, "max": 9.99},
                   "all": {"median": 9.99, "max": 16.61}},
        "note": "缝是 (板,交界) 二元性质：同交界 t100=0 / t200=16.61mm。t003×级3 = 影缘落"
                "近平面 11mm（<1 纹素）精度带 → 闭式预判预期不可量（⊘）；t100 的 c1/c2 过渡"
                "括号样本逐位相同 → seam=0 是真测量（闸门 5 显式警告不抛）。",
    },
}
# meshlet_pack --stats 实际输出（scan 靶，sha1 b8c0db69…；全文 = /tmp/opencode/pack_scan1.log）
EDGE_PACK_STATS: dict = {
    "levels": 5,
    "perLevelTris": [10626, 5308, 2652, 1324, 656],
    "perLevelClusters": [173, 86, 40, 23, 8],
    "clusters": 330, "groups": 45, "nodes": 52,
    "l0Shape": "avg 61.4 tris/cluster, median fill 50.0%",
    "note": "DAG 深度 5 级满足「≥3 级」验收，但引擎成本表证明塌到 1 簇/prim 后影逐位不变"
            "（剪影不变性），深度 ≠ 可测性。",
}
EDGE_N_PLATES = 8               # 板数 = 自变量档数（调色板 10 色 − 地面 = 上限 9）

# 🔴 远工作点的世界平移（相对坐标批 5 第 2 刀）：**沿世界 +X 平移 1e6 m**。
# 为什么必须有第二个工作点：rebase 的全部意义 = 「场景离世界原点很远时 float32 还够用」，
# 而出厂点 |camera| ≈ 8 m 时门关态精度本来就够 ⟹ 只验「门开不坏」会放行一个
# 「线接完了但一分精度都没买到」的实现。判据表的两条（不坏 / 变好）分别钉这两个工作点。
#
# 🔴 平移量住在 **entity 的 position（模型根变换）**，**不烘进 GLB 顶点**。理由不是省事：
# 顶点缓冲是 float32，`float32(1e6 + 局部)` 的 ULP 比纹素还粗 ⟹ **资产自己**就带位置误差，
# 而原点对它一无所知（`toRelative` 减的是变换的平移列，不是顶点）⟹ 烘进顶点的那部分误差
# **任何原点方案都消不掉**，「变好」的读数会被它污染到与信号同量级。
# 真实引擎也不会把大坐标烘进顶点 —— 瓦片顶点住在瓦片局部空间，大坐标只住在 tile 原点。
# 实测两条路都走了一遍：两种形态在远点下都「阴影整块消失」（见下方 honesty ⑥），
# 所以这条选择不是绕开失效，只是不让资产量化污染掉本该由 rebase 买单的那部分。
#
# 🔴🔴 债-159（2026-10-26）：原值 `1e6` 是**错的工作点**，本靶因此对「大坐标精度」整类缺陷
# 结构性失明。原因不是「小数没挑好」，是**两件事叠加**，逐条都在下面：
#
# ⓪ 为什么原来失明：`1e6 = 2⁶·5⁶` 是 float32 **恰可精确表示**的数，而各 piece 的 node 矩阵
#    是单位阵 ⟹ 「把绝对层从 float 影子改成真 double」这类回归在图像上**根本不存在**
#    （圆与不圆是同一个数）。第二刀实测：绝对位置保真度 0.0331 m → 2.3e-8 m（1.4e6 倍），
#    `shadow-swim-flat` 四格与 `material-swim` 八条读数**逐数不变**。
#
# ① 光「网格外」还不够，必须**大到绝对尺量得出来**（四条本机实测的数，2026-10-26，
#    `just imgqa shadow-edge-rebase` 打印）：
#    · 误差住在世界 X 上，而绝对尺量的是**沿 u（光的水平行进方向）的影边**，投影系数
#      u_x = (2/3) / |(2/3, −3/5)| = **0.7433** ⟹ 可诱导误差先打 0.7433 折；
#    · 最紧的逐板界（`plateBoundMm` = 1 枚本级纹素 + 2 枚像素刻度）**实测 = 18.57 mm**
#      （plate_t300）；
#    · 🔴 而未注入任何变异时，出厂基准与远点门开的 |err④ − err①| **实测已达 12.86 mm**
#      （= 界的 69 %，共模偏置与 texel 吸附相位差）⟹ 留给「信号」的余量只有 **5.7 mm**；
#    · 1e6 量级 float32 的 ULP = 0.0625 m ⟹ 最大可诱导误差 = ULP/2 = 31.25 mm，投到 u 最多
#      **23.2 mm = 界的 1.25 倍**。
#    ⟹ **在 1e6 量级上无论怎么挑小数都不可判**：最好的情形信号只比界大 25 %，而它与既有的
#      12.86 mm 偏差可能同号相加也可能相消（相消就落到界以下读绿）。任务书建议的 `1e6 + 0.3`
#      更差：诱导 12.5 mm、投到 u 9.3 mm = 界的 0.50 倍，**必然读绿**。
#      ⟹ 本批把 assert ⑫ 的第四查与仪器的 G1b 都定成「诱导量 ≥ 2 × 最紧界」，把「改了但白改」
#        当场拦下来（两条都实跑验过会抛，见交付说明）。
#
# ② 量级抬到 **4e6**（ULP = 0.25 m）：本靶取 entity position = 4000000.1175 ⟹ float32 圆到
#    **4000000.0**，诱导误差 **−117.5 mm**，投到 u **−87.3 mm = 最紧界 18.57 mm 的 4.7 倍**
#    （仪器打印的实测倍数）。构建期 assert ⑫ 与仪器 G1b 把「圆走了、且圆掉的量量得出来」钉成
#    判据，改回网格上的值 / 改回 1e6 量级都直接建不出来或抛出来。
#    ⚠ 一枚副作用（如实报）：4e6 处 ULP = 0.25 m **大于本靶扫描窗的上游半宽 68.6 mm** ⟹ 注入
#      该变异后远点门开态的影边被挪**出窗**，J-R2 的红表现为「门开可量 0 板」而不是「误差超界
#      N 板」。红是确定的、可归因的（出厂两格逐数不变），但绝对尺在 4e6 上**饱和**了；要拿到
#      「超界 N 板」那种更锐的读数，得把量级降到 2e6（ULP 0.125 m ⟹ 最大诱导 46.4 mm，恰在窗内
#      且 = 界的 2.5 倍）或把窗口放宽（会撞 assert ⑧ 的单过零保证）。留给下一批裁。
#
# ③ 量级**不能任意抬**（这是上界，不是偏好）：`Bounds::center` 仍是 float（批 3 第二刀明令
#    本刀不动它），而级联的 near/far 由它派生（`projectionSystem`：dist = |camera.position −
#    dvec3(center)|）⟹ |off| 处的 float32 ULP/2 直接进 nearRaw。出厂 nearRaw = 1.4686，量化到
#    2，离下界 1.0 只有 **0.4686 m** ⟹ 要求 ULP/2 < 0.4686 ⟺ **|off| < 2²³ = 8.39e6**。
#    取 4e6 时该项 ≤ 0.125 m ⟹ nearRaw ∈ [1.34, 1.59] 仍量化到 2、farRaw ∈ [10.87, 11.12]
#    仍量化到 16。assert ⑫ 的第三查按「包围球心被 float32 圆走后 near/far 还在不在同一桶」
#    求值，越界即建不出来（不是注释，是断言）。
def _f32_ulp(x: float) -> float:
    """|x| 所在二进制指数区间的 float32 ULP（= 2^(e−23)，e = floor(log2|x|)）。

    复用本文件既有的 float32 量化算子 `_f32`（「先量化后统计」纪律那条），**不另写一份**
    struct 往返——两份 float32 语义的实现在同一文件里 = 项目哲学第 2 条点名的第二份真相。
    """
    return math.ldexp(1.0, math.floor(math.log2(abs(x))) - 23)


EDGE_FAR_OFFSET = (4000000.18, 0.0, 0.0)   # 整靶的**世界**平移：真值 / 机位 / sceneBounds 吃它
# 🔴 每个 piece 的 **node translation**（债-159 的第二半：让「逐图元复合链」也有亚格小数可丢）。
# 出厂靶各 piece 的 node 矩阵是单位阵 ⟹ `absolute = root · I = root` 在**任何**数值域都成立
# ⟹ 第二刀那条「复合改在 double 域乘」的链在图像上不可见。取 **0.0625 m = ULP(4e6)/4**：
# 它单独在 float32 里精确可表示（所以 `worldOffset − nodeTranslation + nodeTranslation` 在
# double 域**逐位回到** worldOffset，不给真值掺进任何误差），但**加到工作点上就落到网格外**
# （4000000.0625 不可表示，float32 圆回 4000000.0）⟹ 一旦有人把复合改回 float 域乘，这 62.5 mm
# 整个消失。为什么写进 GLB 而不是并进 worldOffset：并进 root 就等于只改工作点，逐图元那一步
# 仍然是乘单位阵（还是没被量过）。
EDGE_FAR_NODE_T = (_f32_ulp(EDGE_FAR_OFFSET[0]) / 4.0, 0.0, 0.0)
ZERO3 = (0.0, 0.0, 0.0)


# 沿 u 的投影系数（世界 X 上的误差投到「光的水平行进方向」上打这个折）：绝对尺量的是沿 u 的
# 影边落点，不是世界 X。推导见 EDGE_FAR_OFFSET 头注 ①。
EDGE_U_PROJ_X = (SHADOW_OFF[0] / math.hypot(*SHADOW_OFF))


def edge_u2w(u: float, v: float, y: float = 0.0, off: tuple = ZERO3) -> tuple:
    """(沿光水平行进方向的坐标 u, 横向 v, 高 y) → 世界 (x, z 平面 + y)。

    `off` = 整靶的世界平移（默认零）。🔴 **零偏移时一个算术都不多做**（不是 `+ 0.0`）：
    加 0.0 会把 `-0.0` 归一成 `+0.0`，而 manifest 是逐字节比对的回归对象（默认零偏移
    必须让现有靶逐字节不变）。
    """
    offlen = math.hypot(*SHADOW_OFF)
    ux, uz = SHADOW_OFF[0] / offlen, SHADOW_OFF[1] / offlen
    x, zz = u * ux + v * (-uz), u * uz + v * ux
    if off != ZERO3:
        x, y, zz = x + off[0], y + off[1], zz + off[2]
    return (x, y, zz)


def edge_frame(thicknesses: list, cam_b: float, cam_y: float, tgt_frac: float,
               fov: float, off: tuple = ZERO3) -> dict:
    """靶的闭式几何帧：板布局 + 地面 + 机位 + 引擎级联公式链复刻。纯函数。

    `off` = 整靶世界平移（只作用于世界坐标；u/v 帧与全部几何关系平移不变，
    所以级联闭式（near/far/splits/texel）与 `off` 无关 —— 这正是本靶能整体平移的依据）。
    """
    offlen = math.hypot(*SHADOW_OFF)
    band = offlen * EDGE_H                       # 带宽 = 接触缘→远影缘（沿 u）
    lv = max(EDGE_LV_K * EDGE_H, 0.4)            # 长边
    n = len(thicknesses)
    t_max = max(thicknesses)
    spacing = EDGE_SPACING_K * (band + t_max + EDGE_SPACING_PAD)
    plates = []
    for i, t in enumerate(thicknesses):
        uc = i * spacing
        far = uc + t / 2.0
        edge = far + band
        plates.append({"t": t, "uc": uc, "near": uc - t / 2.0, "far": far, "edge": edge})
    gap_min = min(plates[i + 1]["near"] - plates[i]["edge"] for i in range(n - 1))
    s_up, s_dn = EDGE_S_UP_K * band, EDGE_S_DN_K * gap_min
    cu = plates[-1]["uc"] + cam_b                # 相机 u（下游）
    u0 = plates[0]["uc"] - max(0.5, 2 * EDGE_H)
    u1 = cu + 0.5
    v_half = lv / 2 + max(0.4, 2 * EDGE_H)
    e0, e7 = plates[0]["edge"], plates[-1]["edge"]
    ut = e0 + tgt_frac * (e7 - e0)
    eye = edge_u2w(cu, 0.0, cam_y, off)
    tgt = edge_u2w(ut, 0.0, 0.0, off)
    # ── 引擎公式链复刻（见头注「级联自证」；unionSphere = ecs_impl 同款）──
    def _union(a, b):
        ca, ra = a
        cb, rb = b
        d = math.dist(ca, cb)
        if d == 0.0:
            return (ca, max(ra, rb))
        if d + ra <= rb:
            return (cb, rb)
        if d + rb <= ra:
            return (ca, ra)
        k = (d + ra - rb) / (2 * d)
        return tuple(ca[j] + (cb[j] - ca[j]) * k for j in range(3)), (d + ra + rb) / 2.0
    cu_, cv_ = (u0 + u1) / 2.0, 0.0
    gc = edge_u2w(cu_, cv_, 0.0, off)
    acc = (gc[0], gc[1], gc[2]), math.hypot(u1 - cu_, v_half)
    for p in plates:
        c = edge_u2w(p["uc"], 0.0, EDGE_H / 2.0, off)
        r = math.sqrt((p["t"] / 2) ** 2 + (EDGE_H / 2) ** 2 + (lv / 2) ** 2)
        acc = _union(acc, (c, r))
    sb_c, sb_r = acc
    dist = math.dist(eye, sb_c)
    near = max(0.05, dist - sb_r)
    far = dist + sb_r

    def _quant(d):   # quantizeShadowDistance：向上取整到 2 的幂
        m, e = math.frexp(d)
        return math.ldexp(1.0, e - 1 if m == 0.5 else e)

    def _split(near_, far_, c, count=4):   # cascadeSplitDistance（mix=0.5）
        if c + 1 >= count:
            return far_
        log_s = near_ * (far_ / near_) ** (c / count)
        uni = near_ + (far_ - near_) * ((c + 1) / count)
        return 0.5 * log_s + 0.5 * uni

    def _frustum_r(n_, f_):   # frustumBoundingSphere 半径（姿态无关闭式，×1.001 余量同引擎）
        th = math.tan(math.radians(fov) / 2.0)
        aspect = EDGE_CAPTURE[0] / EDGE_CAPTURE[1]
        cs = th * math.sqrt(aspect * aspect + 1.0)
        rn, rf = n_ * cs, f_ * cs
        zc = (f_ * f_ - n_ * n_ + rf * rf - rn * rn) / (2.0 * (f_ - n_))
        return math.sqrt((f_ - zc) ** 2 + rf * rf) * 1.001

    sn, sf = _quant(near), _quant(far)
    splits = [_split(sn, sf, c) for c in range(4)]
    radii = [_frustum_r(0.02 if c == 0 else splits[c - 1], splits[c]) for c in range(4)]
    texels = [2.0 * r / 1024.0 for r in radii]   # 块边长 = 2048/2 = 1024
    # 视深（沿视轴）与预测级联
    tv = (tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2])
    tl = math.sqrt(sum(x * x for x in tv))
    fwd = (tv[0] / tl, tv[1] / tl, tv[2] / tl)
    for p in plates:
        e = edge_u2w(p["edge"], 0.0, 0.0, off)
        p["depth"] = sum((e[k] - eye[k]) * fwd[k] for k in range(3))
        p["cascade"] = sum(1 for c in range(3) if p["depth"] > splits[c])
    return {"plates": plates, "band": band, "lv": lv, "spacing": spacing,
            "s_up": s_up, "s_dn": s_dn, "gap_min": gap_min,
            "ground": (u0, u1, -v_half, v_half), "eye": eye, "tgt": tgt,
            "fov": fov, "near": near, "far": far, "sn": sn, "sf": sf,
            "splits": splits, "radii": radii, "texels": texels,
            "sceneBounds": (sb_c, sb_r), "cam_u": cu}


def _edge_grid_face(p0, pu, pv, nu: int, nv: int) -> Geom:
    """平行四边形面 p0 + a·pu + b·pv 的 nu×nv 网格（每 quad 独立顶点；法线 =
    normalize(cross(pu, pv))，绕序由 quad() 保证从法线侧看 CCW）。"""
    geoms = []
    for i in range(nu):
        for j in range(nv):
            a0, a1 = i / nu, (i + 1) / nu
            b0, b1 = j / nv, (j + 1) / nv
            corner = _add(p0, _add((pu[0] * a0, pu[1] * a0, pu[2] * a0),
                                   (pv[0] * b0, pv[1] * b0, pv[2] * b0)))
            du = (pu[0] * (a1 - a0), pu[1] * (a1 - a0), pu[2] * (a1 - a0))
            dv = (pv[0] * (b1 - b0), pv[1] * (b1 - b0), pv[2] * (b1 - b0))
            geoms.append(quad(corner, du, dv))
    return _merge_geoms(geoms)


def _edge_slab(uc: float, t: float, h: float, lv: float, nv: int = 36, ny: int = 8) -> Geom:
    """竖直板 = 沿 u 厚 t、沿 v 长 lv、高 h 的封闭长方体，底坐 y=0、中心在 (uc, ·, 0)。
    大面（±u 面）细分 v̂ 向 nv × ŷ 向 ny；±v 面 ŷ 向 ny（与顶底 v̂ 向 nv 沿共享棱对齐，
    流形缝合判据要求共享棱两侧细分一致）。>128 三角才有 ≥2 个 L0 簇、簇 DAG 才有深度
    （机关②）。细分是**共面**的：轮廓与 12 三角盒逐点相同，只喂给打包器可简化的内部
    自由度。六面绕序逐面核对：cross(pu, pv) = 外法线（û×v̂=−ŷ、ŷ×û=v̂ 等，算过）。"""
    offlen = math.hypot(*SHADOW_OFF)
    ux, uz = SHADOW_OFF[0] / offlen, SHADOW_OFF[1] / offlen
    vx, vz = -uz, ux

    def W(u, v, y):
        return (u * ux + v * vx, y, u * uz + v * vz)
    Y = (0.0, h, 0.0)
    Vv = (lv * vx, 0.0, lv * vz)
    Uu = (t * ux, 0.0, t * uz)
    faces = [
        _edge_grid_face(W(uc + t / 2, -lv / 2, 0), Y, Vv, ny, nv),   # +u 面：ŷ×v̂=+û
        _edge_grid_face(W(uc - t / 2, -lv / 2, 0), Vv, Y, nv, ny),   # −u 面：v̂×ŷ=−û
        _edge_grid_face(W(uc - t / 2, lv / 2, 0), Uu, Y, 1, ny),     # +v 面：û×ŷ=+v̂
        _edge_grid_face(W(uc - t / 2, -lv / 2, 0), Y, Uu, ny, 1),    # −v 面：ŷ×û=−v̂
        _edge_grid_face(W(uc - t / 2, -lv / 2, h), Vv, Uu, nv, 1),   # 顶面：v̂×û=+ŷ
        _edge_grid_face(W(uc - t / 2, -lv / 2, 0), Uu, Vv, 1, nv),   # 底面：û×v̂=−ŷ
    ]
    return _merge_geoms(faces)


def scene_shadow_edge_rig(thicknesses: list | None = None, scan: bool = False,
                          world_offset: tuple = ZERO3) -> Scene:
    """影边绝对尺靶。`world_offset` ≠ 0 = **远工作点**（`shadow_edge_rig_far`）：
    整靶（板 + 地面 + 相机 + 目标点）沿世界 +X 平移，闭式真值跟着平移并写进 manifest。

    🔴 两帧分工（本函数的全部复杂度都在这一条上）：
      · `fr`  = **工作帧**（带偏移）→ 构建期断言 ⑤–⑩ 与 manifest 的 measure 块都吃它。
        断言跑在偏移帧上才有意义：靶的闭式解里若藏着任何未声明的「世界坐标必须小」假设，
        平移会当场把它抓红（而不是等运行期读出一堆假数）。
      · `fr0` = **零帧**（不带偏移）→ 写进 GLB 的几何吃它。理由见 EDGE_FAR_OFFSET 头注：
        大坐标只准住在 entity 的 position（模型根变换），不许烘进 float32 顶点缓冲。
    `world_offset = 0` 时两帧是同一个对象 ⟹ 现有靶逐字节不变（回归护栏，cmp 证）。
    """
    th = list(thicknesses if thicknesses is not None else EDGE_THICKNESSES)
    cb, cy, tgf, fov = (EDGE_SCAN_B if scan else EDGE_CAM_B,
                        EDGE_SCAN_Y if scan else EDGE_CAM_Y,
                        EDGE_SCAN_TGT_FRAC if scan else EDGE_CAM_TGT_FRAC,
                        EDGE_SCAN_FOV if scan else EDGE_FOV)
    off = tuple(float(x) for x in world_offset)
    moved = off != ZERO3
    fr = edge_frame(th, cb, cy, tgf, fov, off)
    fr0 = fr if not moved else edge_frame(th, cb, cy, tgf, fov)
    plates, band = fr["plates"], fr["band"]
    offlen = math.hypot(*SHADOW_OFF)
    texel_max = max(fr["texels"])
    colors = ["red", "magenta", "cyan", "orange", "green", "blue", "purple", "azure"]

    stem = ("shadow_edge_scan" if scan else
            "shadow_edge_rig_far" if moved else "shadow_edge_rig")
    s = Scene(
        name=stem,
        out_name=stem,
        purpose="影边世界位置绝对尺：竖直板远侧影边 = 板远侧面 + |SHADOW_OFF|·H（闭式），"
                "误差随板序 = 误差随距离/随级联，台阶 = 级联交界（imgqa shadow-edge-ruler）"
                + (f"；**远工作点** = 整靶沿世界 +X 平移 {off[0]!r} m（rebase 的「变好」"
                   "工作点，imgqa shadow-edge-rebase）" if moved else ""),
        honesty=("闭式解成立的前提（破任何一条读到的是假数，方向注明）：① 接收面水平 "
                 "y=0（本靶唯一平地，无起伏）——接收面一斜，影边在世界 u 上的落点带 "
                 "Δz·cot 项，误差随板位系统漂移；② 板的远侧影缘**竖直且垂直于光的水平"
                 "行进方向**（长边 ⊥ u 由几何钉死）——板转个角，影边成斜线，扫描线读到"
                 "的是混合位置；③ 光方向 = SUN=(0.5,−0.75,−0.45)（configs/scene.lua "
                 "light 段）——harness/RPC 改光后一切真值坐标必须按新光向重算，本 manifest "
                 "全部作废；④ 板是**不透明竖直板**，材质非 doubleSided（双面条目不进簇 "
                 "LOD 路径 = 判据恒绿的静默失效，loader 契约见 qa_scenes 头注）。"
                 "末级淡出（shadow.slang ⑦，t>0.866 起吃 cov）由 assert ⑤ 把全部影边"
                 "挡在射程内；PCF 把带宽 < 数纹素的窄带糊成矮包、50% 点不再是影边，"
                 "由 assert ⑩ 把带宽钉在 6×texel 之上。级联 texel/边界是**闭式预测**"
                 "（公式链 = 引擎复刻，见源码头注），运行期若相机/内容变了会漂——"
                 "仪器按 manifest 机位采集，漂移即对账红。"
                  "🔴 ⑤ 本靶的**负结果**（2026-10-20 实测，别把它读成在测 LOD）：凸平板的"
                  "簇 DAG **保轮廓**——塌到 1 簇/prim（6.2% 三角）影边仍逐位不动（D=-1…11 "
                  "六臂 PNG md5 全等，主 agent 复算），所以本靶对「LOD 是否毁剪影」结构性"
                  "免疫，它的自变量 = **级间影边分歧**（force_cascade 钉级，四臂 PNG 互不"
                  "相同）。bistro 上真正被 LOD 丢掉的是**贴在更大主体上的细附属结构**"
                  "（花箱/牛腿/窗棂：四级全 LOD 时近场 186 个小阴影丢 24 个），那是下一台"
                  "「细附属结构」靶的自变量，不在本靶上找。"
                  + ("🔴 ⑥ **本靶是远工作点（`shadow_edge_rig_far`）**：整靶沿世界 +X 平移 "
                     f"{off[0]!r} m，平移量住在 entity 的 position（**不烘进 GLB 顶点**，"
                     "理由见 qa_scenes.py 的 EDGE_FAR_OFFSET 头注）。GLB 与出厂靶的**顶点缓冲"
                     "逐字节相同**，差的是每个 node 多一枚 `translation` = "
                     f"{EDGE_FAR_NODE_T[0]!r} m（债-159：让「逐图元复合链」也有亚格小数可丢，"
                     "理由见 EDGE_FAR_NODE_T 头注）——所以仪器写进 entities 的平移量是 "
                     f"{off[0] - EDGE_FAR_NODE_T[0]!r} = worldOffset − nodeTranslation。"
                     "本 manifest 的 measure 块（真值 / 机位 / sceneBounds）已跟着平移，"
                     "并额外给 `worldOffset` 与 `nodeTranslation` 两枚键。"
                     "🔴 工作点取在**float32 网格之外**（该 entity position 的 float32 圆后值 = "
                     f"{_f32(off[0] - EDGE_FAR_NODE_T[0])!r}，圆走 "
                     f"{(_f32(off[0] - EDGE_FAR_NODE_T[0]) - (off[0] - EDGE_FAR_NODE_T[0])) * 1000:.1f} mm"
                     "）——网格上的工作点（原值 1e6）没有亚格小数可丢，「大坐标精度」整类缺陷"
                     "对图像判据结构性不可见（债-159 的成因，构建期 assert ⑫ 拦）。"
                     "闭式解的全部前提（①–④）与平移无关：级联 near/far/splits/texel 只吃"
                     "相机到 SceneBounds 的**相对**距离，实测两帧的 cascadeModel 逐数相同。"
                     if moved else "")),
        cameras={
            "edge": {"position": list(fr["eye"]), "target": list(fr["tgt"]),
                     "fovDegrees": fr["fov"],
                     "note": "下游低位朝上游看（机关④）：影边视线不经过自己的板；"
                             "机位 = 离线网格搜索解，构建期 assert ⑤–⑩ 验全部约束"},
        },
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（闭式解的前提③）"},
            "sunElevationDeg": math.degrees(math.asin(-SUN[1] / math.sqrt(sum(v * v for v in SUN)))),
            "shadowOffsetPerMeterOfHeight": [SHADOW_OFF[0], SHADOW_OFF[1]],
            "edgeOffsetPerPlate": band,
            # ── 测量接口（imgqa shadow-edge-ruler 读本块，**不在 C++ 里重抄几何**）──
            "measure": {
                # 🔴 只在远工作点写这一枚键：默认零偏移必须让现有靶的 manifest **逐字节不变**
                # （它是回归对象），而多写一个键就多了 40 来个字节。仪器侧的读法 = 缺键即零。
                # 🔴 `worldOffset` = 整靶的**世界**平移（真值 / 机位 / sceneBounds 都吃它），
                # **不是**仪器写进 scene.lua 的那个数。后者 = `worldOffset − nodeTranslation`
                # （node 已经替 piece 自己挪掉了 nodeTranslation，见 EDGE_FAR_NODE_T 头注）。
                # 两份数都写出来，仪器只做一次减法；只写一份 = 逼另一侧抄常数。
                **({} if not moved else {"worldOffset": list(off)}),
                **({} if not moved else {"nodeTranslation": list(EDGE_FAR_NODE_T)}),
                "u": [SHADOW_OFF[0] / offlen, SHADOW_OFF[1] / offlen],
                "v": [-SHADOW_OFF[1] / offlen, SHADOW_OFF[0] / offlen],
                "H": EDGE_H,
                "bandM": band,
                "plateLengthM": fr["lv"],
                "captureSize": list(EDGE_CAPTURE),
                "fovDegrees": fr["fov"],
                "camera": {"position": list(fr["eye"]), "target": list(fr["tgt"])},
                "sceneBounds": {"center": list(fr["sceneBounds"][0]),
                                "radius": fr["sceneBounds"][1]},
                "stepM": 0.004,
                "vLines": [-fr["lv"] / 4.0, 0.0, fr["lv"] / 4.0],
                "plates": [{
                    "name": f"plate_t{int(round(p['t'] * 1000)):03d}",
                    "thicknessM": p["t"],
                    "uCenter": p["uc"],
                    "farFaceX": edge_u2w(p["far"], 0.0, 0.0, off)[0],
                    "farFaceZ": edge_u2w(p["far"], 0.0, 0.0, off)[2],
                    "trueEdgeX": edge_u2w(p["edge"], 0.0, 0.0, off)[0],
                    "trueEdgeZ": edge_u2w(p["edge"], 0.0, 0.0, off)[2],
                    "scanFromX": edge_u2w(p["edge"] - fr["s_up"], 0.0, 0.0, off)[0],
                    "scanFromZ": edge_u2w(p["edge"] - fr["s_up"], 0.0, 0.0, off)[2],
                    "scanToX": edge_u2w(p["edge"] + fr["s_dn"], 0.0, 0.0, off)[0],
                    "scanToZ": edge_u2w(p["edge"] + fr["s_dn"], 0.0, 0.0, off)[2],
                    "depthM": p["depth"],
                    "cascadePredicted": p["cascade"],
                } for p in plates],
                "cascadeModel": {
                    "formula": ("SceneBounds(unionSphere 逐 prim 质心+最远点) → near=max(0.05,"
                                "dist−R)/far=dist+R → 向上取整到 2 的幂(quantizeShadowDistance) →"
                                " practical split(mix=0.5,count=4) → r_c=frustumBoundingSphere →"
                                " texel=2r/1024（图集 2048、2×2 分块）"),
                    "nearRaw": fr["near"], "farRaw": fr["far"],
                    "nearQuantized": fr["sn"], "farQuantized": fr["sf"],
                    "splitsPredicted": fr["splits"],
                    "texelMmPredicted": [t * 1000.0 for t in fr["texels"]],
                },
            },
            "selfProof": {
                "cascade": {
                    "note": ("generator-side closed-form replica of the engine chain; the "
                             "instrument RE-DERIVES it in C++ from camera+sceneBounds above "
                             "and cross-checks (drift = throw). See header 级联自证"),
                    "occupiedCascades": sorted({p["cascade"] for p in plates}),
                    "perPlate": [{"name": f"plate_t{int(round(p['t'] * 1000)):03d}",
                                  "depthM": p["depth"], "cascade": p["cascade"]}
                                 for p in plates],
                },
                "clearance": {
                    "note": "min line-of-sight clearance over occluding plate tops (m)",
                    "minM": None,  # 由构建期计算回填（下方 assert ⑦ 就地求值）
                },
                "thicknessScan": EDGE_SCAN_READINGS,
                "meshletPackStats": EDGE_PACK_STATS,
            },
        },
    )
    P = s.pieces.append
    g = fr0["ground"]  # 🔴 零帧：GLB 顶点缓冲里**不许**出现 1e6（见 EDGE_FAR_OFFSET 头注）
    # 🔴 远工作点的每个 piece 都带同一枚 node translation（债-159 第二半）。出厂靶 nodeT=None
    # ⟹ 不写这个键 ⟹ 既有靶的 GLB 与 manifest 一个字节都不变。
    nt = EDGE_FAR_NODE_T if moved else None
    P(Piece("ground", quad(edge_u2w(g[0], g[2]),
                           (edge_u2w(g[1], g[2])[0] - edge_u2w(g[0], g[2])[0], 0.0,
                            edge_u2w(g[1], g[2])[2] - edge_u2w(g[0], g[2])[2]),
                           (edge_u2w(g[1], g[3])[0] - edge_u2w(g[1], g[2])[0], 0.0,
                            edge_u2w(g[1], g[3])[2] - edge_u2w(g[1], g[2])[2])),
            PAL["yellow"], roughness=0.9, radial=False, closed=False, translation=nt,
            intent="唯一接收面（零起伏平地 y=0，闭式前提①；顺带结掉债-139 卡在鹅卵石"
                   "起伏上的 2.73pp 残差）；法线 +Y"))
    for i, p in enumerate(plates):
        P(Piece(f"plate_t{int(round(p['t'] * 1000)):03d}",
                _edge_slab(p["uc"], p["t"], EDGE_H, fr["lv"]),
                PAL[colors[i % len(colors)]], roughness=0.7, translation=nt,
                intent=f"竖直挡板：厚度 t={p['t']*1000:g} mm（本靶自变量）、高 {EDGE_H} m、"
                       f"长边 ⊥ 光的水平行进方向；每板独立 primitive = 独立簇 DAG"
                       f"（选级逐图元 min(D, 自身最深级)）"))

    # ---- 构建期断言（编号对应头注「级联自证」；口头承诺不算数）----
    tag = stem
    assert all(p["depth"] < fr["sf"] * 0.85 for p in plates), \
        f"{tag}: ⑤ 有影边视深落进末级淡出带（≥0.85·shadowFar={fr['sf']:.2f}）——" \
        f"depths={[round(p['depth'],2) for p in plates]}，cov 被 atten 吃掉读数偏受光"
    assert len({p["cascade"] for p in plates}) >= 3, \
        f"{tag}: ⑥ 影边只跨 {len({p['cascade'] for p in plates})} 枚级联（<3）——误差曲线" \
        f"没有台阶，仪器没有自变量；cascs={[p['cascade'] for p in plates]}"
    # ⑦ 视线净空：窗口点 wu 对相机与它之间的板 j（u_j > wu）近侧顶缘的净空
    clr_min = 1e30
    for p in plates:
        for w in (-fr["s_up"], 0.0, fr["s_dn"]):
            wu = p["edge"] + w
            for q in plates:
                if q["uc"] <= p["uc"]:
                    continue  # 相机在下游：只有 u 更大的板在视线与窗口点之间
                T = fr["cam_u"] - wu
                Pp = fr["cam_u"] - q["near"]
                if T > Pp > 0:
                    clr_min = min(clr_min, fr["eye"][1] * (1 - Pp / T) - EDGE_H)
    assert clr_min > 0.003, \
        f"{tag}: ⑦ 视线净空 {clr_min:.4f} m ≤ 3 mm——窗口点被板顶缘遮挡（读数系统性" \
        f"偏上游而曲线形状正常，最难归因的假数）；加大 cam_y 重搜机位"
    s.extra["selfProof"]["clearance"]["minM"] = clr_min
    for i, p in enumerate(plates):
        lo, hi = p["edge"] - fr["s_up"], p["edge"] + fr["s_dn"]
        for j, q in enumerate(plates):
            if i == j:
                continue
            assert not (lo < q["near"] < hi or lo < q["far"] < hi), \
                f"{tag}: ⑧ {p['name'] if 'name' in p else i} 窗口吞进邻板实体"
            assert not (lo - 0.02 < q["edge"] < hi + 0.02), \
                f"{tag}: ⑧ 窗口吞进邻影边（双过零点，仪器必抛）"
    # ⑨ 取景：窗口角点投影在画内 + 30 px（针孔：竖直 fov + aspect 压横轴）
    tv = tuple(fr["tgt"][k] - fr["eye"][k] for k in range(3))
    tl = math.sqrt(sum(x * x for x in tv))
    f_ = tuple(x / tl for x in tv)
    up = (0.0, 1.0, 0.0)
    s_ = _norm((f_[1] * up[2] - f_[2] * up[1], f_[2] * up[0] - f_[0] * up[2],
                f_[0] * up[1] - f_[1] * up[0]))
    u_ = (s_[1] * f_[2] - s_[2] * f_[1], s_[2] * f_[0] - s_[0] * f_[2],
          s_[0] * f_[1] - s_[1] * f_[0])
    tanh = math.tan(math.radians(fr["fov"]) / 2.0)
    asp = EDGE_CAPTURE[0] / EDGE_CAPTURE[1]

    def _px(pt):
        d = tuple(pt[k] - fr["eye"][k] for k in range(3))
        z = sum(d[k] * f_[k] for k in range(3))
        if z <= 1e-6:
            return None
        x = sum(d[k] * s_[k] for k in range(3))
        y = sum(d[k] * u_[k] for k in range(3))
        nx = x / (z * tanh * asp)
        ny_ = y / (z * tanh)
        return ((nx + 1) / 2 * EDGE_CAPTURE[0] - 0.5, (1 - ny_) / 2 * EDGE_CAPTURE[1] - 0.5)
    for p in plates:
        for wu in (p["edge"] - fr["s_up"], p["edge"] + fr["s_dn"]):
            for vv in (-fr["lv"] / 4, fr["lv"] / 4):
                # 🔴 必须带 `off`：`_px` 里的 `fr["eye"]` 是**工作帧**，这里给零帧点 = 两点
                # 差着 1e6 m ⟹ 投影直接飞出画外。远工作点首跑就是这条抓红的（assert ⑨
                # 本来管的就是「窗口在不在画内」，它没坏，是靶的取景代码漏了帧口径）。
                pr = _px(edge_u2w(wu, vv, 0.0, off))
                assert pr is not None and 30 < pr[0] < EDGE_CAPTURE[0] - 30 \
                    and 30 < pr[1] < EDGE_CAPTURE[1] - 30, \
                    f"{tag}: ⑨ 窗口角点 (u={wu:.2f}, v={vv:.2f}) 投出画外/贴边——换机位重推"
    assert band >= 6 * texel_max, \
        f"{tag}: ⑩ 带宽 {band*1000:.0f} mm < 6×最粗纹素 {texel_max*1000:.1f} mm——窄带被" \
        f" PCF 糊成矮包，50% 交叉点不再是影边位置；加高 H 或拉近相机重搜"
    assert fr["s_up"] >= 2.5 * texel_max, \
        f"{tag}: ⑩ 上游窗口 {fr['s_up']*1000:.0f} mm < 2.5×纹素（影边上游移一档就出窗）"
    assert fr["s_dn"] >= 2.5 * texel_max, \
        f"{tag}: ⑩ 下游窗口 {fr['s_dn']*1000:.0f} mm < 2.5×纹素（影边下游移一档就出窗）"
    assert len(th) == EDGE_N_PLATES, f"{tag}: 板数必须 = {EDGE_N_PLATES}（调色板/判据窗口按它排）"
    assert all(0.001 <= t <= 0.5 for t in th), f"{tag}: 厚度档越出 1mm…500mm 物理量程"
    assert all(b > a for a, b in zip(th, th[1:])), f"{tag}: 厚度档必须严格升序（板序=自变量序）"
    # ⑪ 远工作点的**纯平移自证**（只在 `world_offset` ≠ 0 时有内容可查）。
    # 为什么必须有它，而不是「反正断言 ⑤–⑩ 都跑在工作帧上」：⑤–⑩ 全是**平移不变**的几何
    # 关系，它们照旧绿只说明「靶的形状没变」，**说明不了「manifest 报的真值真的跟着挪了」**。
    # 而仪器只信 manifest（manifest = 唯一读数接口）——真值漏挪 = 影边误差凭空多出 1e6 mm，
    # 而 ⑤–⑩ 一条都不会红。这条断言把「平移是纯几何平移」从口头承诺变成构建期判据。
    # 反向半边（最后一条）同样承重：级联闭式若真随世界平移漂了，说明公式链里藏着未声明的
    # 绝对坐标假设 —— 那才是「靶的闭式解撑不住平移」，必须当场红，不许换仪器蒙过去。
    if moved:
        # 🔴 读回**已写进 manifest 的那些数**（`s.extra["measure"]`），不是重算一遍工作帧：
        # 只比 fr 与 fr0 是恒真自检（两边都由同一个 edge_u2w 算出），实测抓不到「measure 块
        # 里某一处 edge_u2w 漏传 off」这种真破口——变异「trueEdge 不随平移」在 fr 对账下全绿，
        # 只有读回 manifest 才红。仪器的 G1 也抓它，但构建期就该拦（别把判据押在跑得到运行期）。
        ms = s.extra["measure"]
        for key, mk in (("eye", "position"), ("tgt", "target")):
            w, mp = fr0[key], ms["camera"][mk]
            assert all(abs(mp[i] - (w[i] + off[i])) < 1e-9 for i in range(3)), \
                f"{tag}: ⑪ manifest 的 camera.{mk} 不是零帧 + world_offset（真值漏挪）"
        bc0, mbc = fr0["sceneBounds"][0], ms["sceneBounds"]["center"]
        assert all(abs(mbc[i] - (bc0[i] + off[i])) < 1e-9 for i in range(3)), \
            f"{tag}: ⑪ manifest 的 sceneBounds.center 不是零帧 + world_offset"
        for i, (mp, p0) in enumerate(zip(ms["plates"], fr0["plates"])):
            for kx, kz, uq in (("farFaceX", "farFaceZ", p0["far"]),
                               ("trueEdgeX", "trueEdgeZ", p0["edge"]),
                               ("scanFromX", "scanFromZ", p0["edge"] - fr0["s_up"]),
                               ("scanToX", "scanToZ", p0["edge"] + fr0["s_dn"])):
                w = edge_u2w(uq, 0.0)
                assert abs(mp[kx] - (w[0] + off[0])) < 1e-9 \
                    and abs(mp[kz] - (w[2] + off[2])) < 1e-9, \
                    f"{tag}: ⑪ manifest 的第 {i} 块板 {kx}/{kz} 不是零帧 + world_offset" \
                       "（真值漏挪 = 仪器会读出 1e6 mm 的假误差）"
        for i, (p0, p1) in enumerate(zip(fr0["plates"], plates)):
            assert abs(p1["depth"] - p0["depth"]) < 1e-9 \
                and p1["cascade"] == p0["cascade"], \
                f"{tag}: ⑪ 第 {i} 块板的视深/预测级联随平移变了（视深是相机到影边的**相对**" \
                "量，平移不变才对；变了 = 公式链里藏着未声明的绝对坐标假设）"
        for c in range(4):
            assert abs(fr["splits"][c] - fr0["splits"][c]) <= 1e-9 * abs(fr0["splits"][c]) \
                and abs(fr["texels"][c] - fr0["texels"][c]) <= 1e-9 * abs(fr0["texels"][c]), \
                f"{tag}: ⑪ 级联 splits/texel 随世界平移漂了（相对距离量，本应与 off 无关）"
    # ⑫ 🔴 远工作点必须**离开 float32 网格**，且圆走的量必须大到绝对尺量得出来（债-159）。
    # 这一组断言就是本债的机器判据：把 EDGE_FAR_OFFSET 改回网格上的值（1e6 那类）、或把
    # node translation 改成 0 / 改成大于半个 ULP，**直接建不出靶**——不再靠「下次记得」。
    # 为什么判据必须长在这里而不是只写在注释里：本债的整个成因就是「工作点看起来对、读数
    # 很漂亮、全绿，而它结构上量不到」（同形先例 = 债-153 的 swim 从不应用 worldOffset）。
    if moved:
        # 唯一进 `rootTransformOf` 的那枚数是 **entity position**（= worldOffset − nodeTranslation），
        # 所以「在不在网格上」问的是它，不是 worldOffset 本身。
        ent = off[0] - EDGE_FAR_NODE_T[0]
        assert all(v == 0.0 for v in EDGE_FAR_NODE_T[1:]), \
            f"{tag}: ⑫ node translation 只准动 X（动 y 破地面接收关系 = assert ⑪ 的前提③，" \
            "动 z 会让「远点 = 出厂靶 + worldOffset」这条自证多出一项待对账的偏移）"
        ulp = _f32_ulp(ent)
        # (a) node translation 必须**亚格**：小于工作点处 ULP 的一半 ⟹ 在 float32 域把它加到
        #     大坐标上会被**整个吃掉**，这正是「逐图元复合链有亚格小数可丢」的定义。
        assert 0.0 < abs(EDGE_FAR_NODE_T[0]) < ulp / 2.0, \
            f"{tag}: ⑫ node translation = {EDGE_FAR_NODE_T[0]!r} m，不在 (0, ULP/2 = " \
            f"{ulp / 2.0!r}) 内 ⟹ 它在 float 域复合下不会消失，逐图元那条链又没有可丢的精度了"
        # (b) double 域往返必须**逐位**回到 worldOffset：node 平移不许给闭式真值掺进误差
        #     （取 2 的幂就是为了这一条；换成 0.06 这类小数会留 ~1e-9 m 的真值偏差）。
        assert ent + EDGE_FAR_NODE_T[0] == off[0], \
            f"{tag}: ⑫ entity + nodeTranslation 不逐位等于 worldOffset ⟹ 闭式真值与落位分家"
        # (c) entity position 在网格外（债-159 的一句话本体：圆与不圆必须是**两个数**）。
        rounded = _f32(ent)
        assert rounded != ent, \
            f"{tag}: ⑫ entity position {ent!r} 恰可 float32 精确表示 ⟹ 「绝对层多过一遍 float」" \
            "这类回归在图像上根本不存在（圆走 0 mm），本靶又变回债-159 描述的那台全绿仪器"
        # (d) 圆走的量投到**测量轴 u** 上必须 ≥ 8 枚最细级联纹素。为什么是 8：绝对尺最紧的逐板
        #     界（plateBoundMm = 1 枚本级纹素 + 2 枚像素刻度）**实测 18.57 mm = 3.27 枚最细纹素**，
        #     取 8 枚 = 该界的 2.4 倍，与仪器侧 G1b 的「≥ 2 倍」同向且略严。
        #     ⚠ 这条就是「为什么量级要从 1e6 抬到 4e6」：1e6 处最大可诱导 ULP/2 = 31.25 mm，
        #     投到 u 只有 23.2 mm = 界的 1.25 倍，而基准自身的偏差已占掉界的 69 % ⟹ 不可判。
        #     实跑：把 EDGE_FAR_OFFSET 改成 1000000.3（任务书建议的形态）本条当场抛。
        induced = abs(rounded - ent)
        projected = induced * EDGE_U_PROJ_X
        assert projected >= 8.0 * min(fr["texels"]), \
            f"{tag}: ⑫ 网格外工作点诱导的位置误差 = {induced * 1000:.1f} mm，投到测量轴 u 只有 " \
            f"{projected * 1000:.1f} mm < 8 × 最细纹素 {8 * min(fr['texels']) * 1000:.1f} mm ⟹ " \
            "绝对尺按构造量不出来，这台仪器仍然是全绿的假仪器"
        # (e) `Bounds::center` 仍是 float，而级联 near/far 由它派生（projectionSystem:
        #     dist = |camera.position − dvec3(center)|）⟹ 球心被 float32 圆走后 near/far
        #     **必须还落在同一个量化桶**里，否则 assert ⑪ 的「级联与平移无关」在引擎侧就是
        #     假的，闭式预测的 texel 刻度全部作废。|Δdist| ≤ |Δcenter|（Lipschitz 1）⟹ 拿
        #     球心分量的最大圆走量当上界来比，是**保守**判据（不需要复刻引擎的 dist 公式）。
        bc = ms["sceneBounds"]["center"]
        dq = max(abs(_f32(c) - c) for c in bc)
        for raw, q, what in ((fr["near"], fr["sn"], "near"), (fr["far"], fr["sf"], "far")):
            margin = min(abs(raw - q), abs(raw - q / 2.0))
            assert dq < margin, \
                f"{tag}: ⑫ 包围球心的 float32 圆走量 {dq:.3f} m ≥ {what}Raw={raw:.4f} 到量化桶" \
                f"边界的距离 {margin:.4f} m ⟹ 远工作点会把级联 near/far 挤出同一桶（工作点量级" \
                "太靠上，2²³ = 8.39e6 是本靶的上界）"
        # 事实落盘（仪器/报告读它，不在别处重抄一遍这些数）。
        s.extra["selfProof"]["farGrid"] = {
            "note": "债-159：远工作点的 float32 网格事实（构建期 assert ⑫ 求值，不是手填）",
            "entityPositionX": ent,
            "entityPositionXFloat32": rounded,
            "inducedErrorMm": (rounded - ent) * 1000.0,
            "projectedOnLightAxisMm": (rounded - ent) * 1000.0 * EDGE_U_PROJ_X,
            "float32UlpM": ulp,
            "nodeTranslationXM": EDGE_FAR_NODE_T[0],
            "uProjX": EDGE_U_PROJ_X,
            "boundsCenterQuantizeM": dq,
            "nearRaw": fr["near"], "nearQuantized": fr["sn"],
            "farRaw": fr["far"], "farQuantized": fr["sf"],
            "finestTexelMm": min(fr["texels"]) * 1000.0,
        }
    return s


# ---------------------------------------------------------------------------
# 场景 11：shadow_wall_rig —— **垂直接收面**上影场的世界不变性 + 级联接缝因果实验
# ---------------------------------------------------------------------------
# ## 为什么必须有它（债-139 的第二半，别删）
# 第 7 刀的 `shadow-swim-flat` 把「平地残差是不是测量地板」答死了（恰 0.0000 %），但结案时明确
# 留了一句没验的话：**本仪器量的是水平地面，而债-139 的原始症状在垂直门面上**——门面不是 y=0
# 平面，那套闭式重建吃不下它。「门上那条影为什么随相机动」仍未答。本靶把接收面换成**竖直平面**，
# 把同一个问题在正确的几何上重做一遍。
#
# ## 三条可分离机制（用户的猜想只点名了第一条）
#   S 接缝     = 级联切分距离投到门面上的那条边界（两侧纹素尺寸 / 法线外推不同）；相机动 ⟹
#                边界扫过门面
#   T 纹素吸附 = 单级联内光空间盒随相机自适应；门关态已被世界锚定吸附消掉（债-139 ①）
#   C 坐标精度 = 大坐标 float32 量化（rebase 治的那个）
#   ⟹ 判据必须能把三者分开，否则「门面上有随相机动的影」证实不了任何一件事。
#
# ## 几何机关（编号 = 构建期 assert 的编号，改坏任何一条直接建不出靶）
#   ① **门面法线 = 太阳的水平分量方向**（竖直面能拿到的最大日照 = |水平(L)| = 0.6677，地面是
#      0.7444）。影深 = 太阳在受光里的占比；拿 edge rig 的地面实测（~30 %）折算，门面约 27 %
#      ⟹ cov 峰值 ≈ 0.45（线性域），够判据吃。法线取反 = 门面背对太阳 = 全场本影 = 判据恒真。
#   ② **遮挡体是竖直细杆，且整根在画外**。两条性质同时成立才有得测：
#      · 影缘竖直：e1（沿墙水平）⊥ 光的水平行进方向 ⟹ e1·L = 0 ⟹ 杆上每一点的影都落在
#        u = 杆的 u 处，杆影就是竖直线段，与杆的高度范围无关。
#      · 杆画外：杆沿 +n 前移 HBAR，影在墙上**下移** K_V·HBAR（K_V = −L.y/(−n·L) = 1.1149）
#        ⟹ 把杆抬到 K_V·HBAR 以上，它的影照旧盖住整块门面，而相机一个像素都看不见它。
#        🔴 这条不是省事：仪器的门面掩码是**闭式**的（「射线命中门面矩形」），前提是画内除了
#        门面没有别的东西。杆入画 ⟹ 掩码把杆的像素当成门面 ⟹ 对拍读的是两回事。
#        构建期 assert ⑤ 逐角点投影核对（含 60 px 余量），不是口头承诺。
#      · 接缝必须**穿过**门面（assert ②'：splits[force] ∈ 门面视深区间），否则仪器没有自变量。
#   ③ **接缝近水平**：机位在门面正前方 ⟹ 视深梯度 gU = e1·f ≈ 0 ⟹ 等视深线（= 接缝在门面上的
#      轨迹）是水平线。梯度方向 = 接缝的法线 ⟹ 相机沿 n 平移时接缝**横切**竖直影缘。反过来
#      （接缝平行于影缘）受影响面积会塌成一条线，判据没有统计量。
#   ④ **末级淡出**（shadow.slang ⑦，t>0.866 吃 cov）把全部档挡在射程内。
#   ⑤⑥⑦⑧ 见下面 assert 本体。
#
#   ④' **推拉序列 = 因果实验的自变量**：接缝在门面上的位置 = 「视深 == splits[c]」的轨迹，而
#      splits 只吃 (nearQ, farQ) = 量化后的 dist(相机, SceneBounds 球心) ± R。沿门面法线 n 推拉
#      相机 ⟹ 换桶时 splits **跳**、不换桶时 splits **恒**。⟹ 一条可证伪的预测：
#        · 同桶内挪相机：接缝在门面上**扫过**，但它的**视深逐字不变**；
#        · 跨桶：接缝视深**跳**到新的 splits 值，跳量 = 闭式已知的 Δsplits。
#      「拧它它就动、不拧它不动」= 因果；只报「不连续位置与预测接缝位置重合」= 相关性，不算。
#      WALL_CAUSAL_DOLLY 就是这条序列，WALL_CAUSAL_PAIRS 是「同桶对 / 跨桶对」的配对表。
#
# ## 闭式真值（manifest 的 measure 块 = 仪器唯一读数接口，C++ 一个几何常量都不抄）
#   门面平面：点 O = (0,0,0)（世界原点，n·P = 0）、法线 n、局部轴 e1（水平）/ e2 = +Y。
#   视深场：depth(u, y) = c0 + gU·u + gY·y，其中 (gU, gY, c0) 只由机位与朝向决定。
#   接缝轨迹：depth = splits[c] ⟹ y = (splits[c] − c0 − gU·u)/gY（gY ≠ 0 由 assert ③ 钉）。
#   杆影：u = 杆的 u（机关 ②），y 覆盖 [杆底 − K_V·HBAR, 杆顶 − K_V·HBAR] ⊇ [0, WALL_H]（⑥）。
#   级联闭式 = 引擎公式链复刻（同 edge_frame：unionSphere → near/far → 2 的幂 → practical
#   split(mix=0.5,count=4) → frustumBoundingSphere → texel = 2r/1024）。
WALL_W = 5.0                                 # 门面宽（沿 e1，米）
WALL_H = 3.5                                 # 门面高（沿 +Y，米）
WALL_HBAR = 5.0                              # 遮挡杆沿门面法线 n 的前移量（米）
WALL_NBAR = 9                                # 杆数（= 门面上的竖直影缘对数）；+1 门面
#   = 10 件，正好等于固定调色板的容量（构建期「同场景 piece 两两色距 ≥ 0.5」是反查
#   可靠性的地基，见 PAL 注释；加杆数要么换调色板要么并件，别绕那条断言）
WALL_PERIOD = 0.50                           # 杆间距（沿 e1，米）；9 根 × 0.5 = 4.0 m 跨门面
WALL_BAR_W = 0.05                            # 杆宽（沿 e1）
WALL_BAR_D = 0.10                            # 杆厚（沿 n）
WALL_BAR_PAD = 0.6                           # 杆影覆盖相对门面的上下余量（米）
WALL_CAPTURE = (1600, 900)                   # 采集尺寸（仪器写死同值；assert ⑧ 按它算）
WALL_FOV = 40.0                              # 竖直 fov（度）
WALL_Y_E = 0.0                               # 机位高度（沿 e2，米）
WALL_Y_TGT = WALL_H / 2.0                    # 目标点高度（= 门面中线）
WALL_FORCE_CASCADE = 3                       # 钉级臂的级号（接缝 = splits[2]，钉在远侧那一级）

# 激励：B = A 沿门面法线 n 纯平移（target 不动 ⟹ 姿态逐字相同）。🔴 两枚机位必须**同桶**
# （assert ⑦ 逐数对账），否则「阴影随相机变」里有一半是级联本身变了，不是本判据的射程。
WALL_CAM_A = 8.75
WALL_CAM_B = 9.00
WALL_STIMULUS_M = WALL_CAM_B - WALL_CAM_A    # = 0.25 m，闭式已知，仪器不另算

# 因果序列：沿 n 的相机距离。同桶的两档成对（接缝视深必须逐字相同）、跨桶的两档成对
# （必须差一个闭式已知的 Δsplits）。三档桶位 = (1,16) / (2,16) / (4,16)。
WALL_CAUSAL_DOLLY = {
    # ── 第一组：本批开工前按闭式模型选的档（模型说接缝穿门面）────────────────────────
    "d1": 8.75, "d2": 9.00,      # 模型桶 (2, 16)  模型 splits[2] = 9.0784
    "d3": 10.25, "d4": 10.50,    # 模型桶 (4, 16)  模型 splits[2] = 10.5
}
WALL_CAUSAL_PAIRS = [
    ("d1", "d2", "same"),   # 同桶：模型说接缝在门面上扫过，但**视深逐字不变**
    ("d3", "d4", "same"),
    ("d2", "d3", "jump"),   # 跨桶：模型说接缝视深跳到新的 splits（Δ = +1.4216 m）
    ("d4", "d1", "jump"),   # 反着拧：Δ = −1.4216 m（主 agent 的独立验红形态）
]


def wall_basis():
    """门面局部标架 + 影位移常数（纯函数；assert 与 manifest 共用同一份，不抄第二份）。"""
    ln = math.sqrt(sum(v * v for v in SUN))
    lx, ly, lz = SUN[0] / ln, SUN[1] / ln, SUN[2] / ln      # 光传播方向（单位）
    nn = math.hypot(-lx, -lz)
    nv = (-lx / nn, 0.0, -lz / nn)                          # 竖直面能拿到的最大日照法线
    e1v = (nv[2], 0.0, -nv[0])                              # 沿墙水平；e1 × e2 = n
    kh = -(nv[0] * lx + nv[2] * lz)                         # = −n·L > 0
    kv = -ly / kh                                           # 前移 1 m → 影下移 kv 米
    return {"L": (lx, ly, lz), "n": nv, "e1": e1v, "e2": (0.0, 1.0, 0.0),
            "kH": kh, "kV": kv, "sunFrac": kh}


def wall_u2w(u: float, y: float, h: float = 0.0, b=None) -> tuple:
    """门面局部 (u, y, 沿 n 的前距 h) → 世界 (x, y, z)。"""
    b = b or wall_basis()
    e1, e2, nv = b["e1"], b["e2"], b["n"]
    return (u * e1[0] + y * e2[0] + h * nv[0],
            u * e1[1] + y * e2[1] + h * nv[1],
            u * e1[2] + y * e2[2] + h * nv[2])


def wall_yaw_deg(b) -> float:
    """`box()` 的 yaw：局部 +x → e1、局部 +z → n（两者都水平且正交，yaw 就够了）。"""
    return math.degrees(math.atan2(b["n"][0], b["n"][2]))


def wall_frame(a: float, b=None) -> dict:
    """沿 n 距离为 a 的一枚机位的闭式量（视深梯度）。纯函数。"""
    b = b or wall_basis()
    eye = wall_u2w(0.0, WALL_Y_E, a, b)
    tgt = wall_u2w(0.0, WALL_Y_TGT, 0.0, b)
    tv = tuple(tgt[k] - eye[k] for k in range(3))
    tl = math.sqrt(sum(x * x for x in tv))
    f = tuple(x / tl for x in tv)
    gU = sum(b["e1"][k] * f[k] for k in range(3))
    gY = sum(b["e2"][k] * f[k] for k in range(3))
    o = wall_u2w(0.0, 0.0, 0.0, b)
    c0 = -sum((eye[k] - o[k]) * f[k] for k in range(3))
    return {"eye": eye, "tgt": tgt, "f": f, "gU": gU, "gY": gY, "c0": c0, "a": a}


def wall_bars(b=None) -> list:
    """15 根竖直杆的 (u, y 起, y 止)。影覆盖 [y0 − kV·h, y1 − kV·h] ⊇ [0, WALL_H]。"""
    b = b or wall_basis()
    sh = b["kV"] * WALL_HBAR
    return [((i - (WALL_NBAR - 1) / 2.0) * WALL_PERIOD, sh - WALL_BAR_PAD,
             sh + WALL_H + WALL_BAR_PAD) for i in range(WALL_NBAR)]


def wall_cascade(eye: tuple, sb_center: tuple, sb_r: float) -> dict:
    """引擎级联公式链复刻（同 edge_frame；口径注释在那边，两份公式各自独立实现会漂）。"""
    def _quant(d):
        m, e = math.frexp(d)
        return math.ldexp(1.0, e - 1 if m == 0.5 else e)
    def _split(near_, far_, c, count=4):
        if c + 1 >= count:
            return far_
        return 0.5 * near_ * (far_ / near_) ** (c / count) + 0.5 * (
            near_ + (far_ - near_) * ((c + 1) / count))
    def _frustum_r(n_, f_):
        th = math.tan(math.radians(WALL_FOV) / 2.0)
        aspect = WALL_CAPTURE[0] / WALL_CAPTURE[1]
        cs = th * math.sqrt(aspect * aspect + 1.0)
        rn, rf = n_ * cs, f_ * cs
        zc = (f_ * f_ - n_ * n_ + rf * rf - rn * rn) / (2.0 * (f_ - n_))
        return math.sqrt((f_ - zc) ** 2 + rf * rf) * 1.001
    dist = math.dist(eye, sb_center)
    near, far = max(0.05, dist - sb_r), dist + sb_r
    sn, sf = _quant(near), _quant(far)
    sp = [_split(sn, sf, c) for c in range(4)]
    radii = [_frustum_r(0.02 if c == 0 else sp[c - 1], sp[c]) for c in range(4)]
    return {"dist": dist, "nearRaw": near, "farRaw": far, "nearQ": sn, "farQ": sf,
            "splits": sp, "radii": radii, "texels": [2.0 * r / 1024.0 for r in radii]}


def wall_corners(b=None) -> list:
    b = b or wall_basis()
    return [wall_u2w(u, y, 0.0, b) for u in (-WALL_W / 2, WALL_W / 2)
            for y in (0.0, WALL_H)]


def wall_bar_verts(uc: float, y0: float, y1: float, b) -> list:
    """一根杆的 8 角（构建期 assert ⑤⑥ 与 SceneBounds 都吃它；口径 = box 的局部 ±半宽）。"""
    yc = (y0 + y1) / 2.0
    return [wall_u2w(uc + sx * WALL_BAR_W / 2, yc + sy * (y1 - y0) / 2,
                     WALL_HBAR + sz * WALL_BAR_D / 2, b)
            for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]


def wall_scene_bounds(b=None) -> tuple:
    """SceneBounds（引擎口径：逐 primitive 质心+最远点球，再 unionSphere 逐个合并）。"""
    b = b or wall_basis()
    def _sph(pts):
        c = tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
        return c, max(math.dist(c, p) for p in pts)
    def _uni(x, y):
        ca, ra = x; cb, rb = y
        d = math.dist(ca, cb)
        if d == 0.0:
            return (ca, max(ra, rb))
        if d + ra <= rb:
            return (cb, rb)
        if d + rb <= ra:
            return (ca, ra)
        k = (d + ra - rb) / (2 * d)
        return tuple(ca[j] + (cb[j] - ca[j]) * k for j in range(3)), (d + ra + rb) / 2.0
    acc = _sph(wall_corners(b))
    for uc, y0, y1 in wall_bars(b):
        acc = _uni(acc, _sph(wall_bar_verts(uc, y0, y1, b)))
    return acc


def wall_camera_px(fr: dict, pt: tuple) -> tuple | None:
    """世界点 → 像素（针孔：竖直 fov + aspect 压横轴；与 imgqa projectPoint3 同口径）。"""
    d = tuple(pt[k] - fr["eye"][k] for k in range(3))
    z = sum(d[k] * fr["f"][k] for k in range(3))
    if z <= 1e-6:
        return None
    r_ = _norm((fr["f"][1] * 0.0 - fr["f"][2] * 1.0, fr["f"][2] * 0.0 - fr["f"][0] * 0.0,
                fr["f"][0] * 1.0 - fr["f"][1] * 0.0))
    u_ = (r_[1] * fr["f"][2] - r_[2] * fr["f"][1],
          r_[2] * fr["f"][0] - r_[0] * fr["f"][2],
          r_[0] * fr["f"][1] - r_[1] * fr["f"][0])
    th = math.tan(math.radians(WALL_FOV) / 2.0)
    asp = WALL_CAPTURE[0] / WALL_CAPTURE[1]
    nx = sum(d[k] * r_[k] for k in range(3)) / (z * th * asp)
    ny = sum(d[k] * u_[k] for k in range(3)) / (z * th)
    return ((nx + 1) / 2 * WALL_CAPTURE[0] - 0.5, (1 - ny) / 2 * WALL_CAPTURE[1] - 0.5)


def scene_shadow_wall_rig() -> Scene:
    """竖直门面靶（本文件场景 11；判据 = imgqa shadow-swim-wall）。"""
    b = wall_basis()
    bars = wall_bars(b)
    corners = wall_corners(b)
    sb_c, sb_r = wall_scene_bounds(b)
    names = list(WALL_CAUSAL_DOLLY) + ["A", "B"]
    dist_of = dict(WALL_CAUSAL_DOLLY)
    dist_of["A"] = WALL_CAM_A
    dist_of["B"] = WALL_CAM_B
    models = {}
    for nm in names:
        fr = wall_frame(dist_of[nm], b)
        cm = wall_cascade(fr["eye"], sb_c, sb_r)
        models[nm] = (fr, cm)

    cams = {nm: {"position": list(models[nm][0]["eye"]), "target": list(models[nm][0]["tgt"]),
                 "fovDegrees": WALL_FOV,
                 "note": f"沿 n 距离 {dist_of[nm]} m（因果推拉序列；同桶对/跨桶对见 measure.causalPairs）"}
            for nm in names}
    cams["factory"] = dict(cams["d3"], note="仪器默认机位 = d3（接缝穿门面中段）")

    s = Scene(
        name="shadow_wall_rig",
        purpose="垂直接收面（门面）上影场的世界不变性 + 级联接缝因果实验：15 根**画外**竖直杆在"
                "墙上投出 15 对竖直影缘；接缝近水平 ⟹ 相机沿墙法线平移时横切影缘"
                "（imgqa shadow-swim-wall）",
        honesty=("闭式解成立的前提（破任何一条读到的是假数）：① 门面是**平面**且法线 = 太阳水平"
                 "分量方向（竖直面最大日照）——门面一弯或转个角，影深与视深梯度全变；"
                 "② 遮挡杆**整根在画外**（构建期 assert ⑤ 逐角点投影核对）——仪器的门面掩码是"
                 "「射线命中门面矩形」这条闭式判据，画内混进别的几何它认不出来；"
                 "③ 杆影是竖直线段（e1·L = 0，由法线选型钉死）——杆影一旦斜了，接缝与影缘的夹角"
                 "就小，受影响的像素面积塌成一条线；④ 光方向 = SUN=(0.5,−0.75,−0.45)"
                 "（configs/scene.lua light 段），改光后本 manifest 全部作废；⑤ 末级淡出"
                 "（shadow.slang ⑦，t>0.866 吃 cov）由 assert ④ 把全部档挡在射程内；"
                 "⑥ 级联 splits/texel 是**闭式预测**（公式链 = 引擎复刻），运行期相机或内容变了"
                 "会漂——仪器按 manifest 机位采集并逐数对账，漂移即红。"
                 "⚠ 本靶**只有**门面与画外的杆：门面上的影只来自那 15 根杆，所以它量的是"
                 "「垂直接收面 + CSM 固有机制」这一类缺陷，**不含** bistro 那种逐图元剔除 / "
                 "逐图元 LOD 造成的级间内容差异（那条不在本靶上，别拿本靶的绿去回答它）。"),
        cameras=cams,
        extra={
            "defaultLight": {"direction": list(SUN), "intensity": 12.0,
                             "source": "configs/scene.lua light 段（闭式解的前提④）"},
            "measure": {
                "captureSize": list(WALL_CAPTURE),
                "fovDegrees": WALL_FOV,
                "plane": {"origin": [0.0, 0.0, 0.0], "normal": list(b["n"]),
                          "e1": list(b["e1"]), "e2": list(b["e2"]),
                          "uMin": -WALL_W / 2.0, "uMax": WALL_W / 2.0,
                          "yMin": 0.0, "yMax": WALL_H},
                "sunFracVertical": b["sunFrac"],
                "shadowDropPerMeterForward": b["kV"],
                "barDistanceM": WALL_HBAR,
                "bars": [{"name": f"bar_{i:02d}", "u": uc, "yBottom": y0, "yTop": y1,
                          "shadowU": uc, "shadowYBottom": y0 - b["kV"] * WALL_HBAR,
                          "shadowYTop": y1 - b["kV"] * WALL_HBAR}
                         for i, (uc, y0, y1) in enumerate(bars)],
                "sceneBounds": {"center": list(sb_c), "radius": sb_r},
                "stimulus": {"from": "A", "to": "B", "axis": "n",
                             "meters": WALL_STIMULUS_M,
                             "note": "B = A 沿门面法线 n 纯平移（target 不动 = 姿态逐字相同）"},
                "forceCascade": WALL_FORCE_CASCADE,
                "seamSplitIndex": WALL_FORCE_CASCADE - 1,
                "cameras": {nm: {
                    "distanceN": dist_of[nm],
                    "position": list(models[nm][0]["eye"]),
                    "target": list(models[nm][0]["tgt"]),
                    "gU": models[nm][0]["gU"], "gY": models[nm][0]["gY"],
                    "c0": models[nm][0]["c0"],
                    "nearRaw": models[nm][1]["nearRaw"], "farRaw": models[nm][1]["farRaw"],
                    "nearQ": models[nm][1]["nearQ"], "farQ": models[nm][1]["farQ"],
                    "splits": models[nm][1]["splits"],
                    "texelM": models[nm][1]["texels"],
                    "seamYAtU0": [((sp - models[nm][0]["c0"]) / models[nm][0]["gY"])
                                  for sp in models[nm][1]["splits"][:3]],
                    "depthLo": min(models[nm][0]["c0"] + models[nm][0]["gU"] * u
                                   + models[nm][0]["gY"] * y
                                   for u in (-WALL_W / 2, WALL_W / 2) for y in (0.0, WALL_H)),
                    "depthHi": max(models[nm][0]["c0"] + models[nm][0]["gU"] * u
                                   + models[nm][0]["gY"] * y
                                   for u in (-WALL_W / 2, WALL_W / 2) for y in (0.0, WALL_H)),
                } for nm in names},
                "causalPairs": [{"a": p[0], "b": p[1], "kind": p[2],
                                 "splitA": models[p[0]][1]["splits"][WALL_FORCE_CASCADE - 1],
                                 "splitB": models[p[1]][1]["splits"][WALL_FORCE_CASCADE - 1]}
                                for p in WALL_CAUSAL_PAIRS],
            },
        },
    )
    P = s.pieces.append
    e1 = b["e1"]
    P(Piece("facade", quad(corners[0], (WALL_W * e1[0], 0.0, WALL_W * e1[2]),
                           (0.0, WALL_H, 0.0)),
            PAL["yellow"], roughness=0.9, radial=False, closed=False,
            intent="**唯一接收面**：竖直平面 n·P = 0，u∈[−2.5,2.5]、y∈[0,3.5]；法线 = 太阳水平"
                   "分量方向（竖直面最大日照，前提①）；单面（背面剔除下从背面看是空的）"))
    yaw = wall_yaw_deg(b)
    # 杆的颜色 = 调色板里除门面（yellow）之外的前 WALL_NBAR 个（构建期色距断言要求两两不同；
    # 杆整根在画外，颜色对图像零影响，只是 manifest ↔ objectID 反查的静态事实）。
    bar_colors = [k for k in PAL if k != "yellow"][:WALL_NBAR]
    assert len(bar_colors) == WALL_NBAR, f"调色板不够 {WALL_NBAR} 根杆用"
    for i, (uc, y0, y1) in enumerate(bars):
        c = wall_u2w(uc, (y0 + y1) / 2.0, WALL_HBAR, b)
        P(Piece(f"bar_{i:02d}", box(c[0], c[1], c[2], WALL_BAR_W, y1 - y0, WALL_BAR_D, yaw),
                PAL[bar_colors[i]], roughness=0.6,
                intent=f"竖直遮挡杆：u={uc:+.2f}、沿 n 前移 {WALL_HBAR} m、y∈[{y0:.2f},{y1:.2f}]"
                       f"（**整根在画外**，assert ⑤）；在墙上投出 u={uc:+.2f} 的竖直影缘"))

    # ---- 构建期断言（编号对应头注「几何机关」；口头承诺不算数）----
    tag = "shadow_wall_rig"
    assert abs(b["e2"][1] - 1.0) < 1e-12, f"{tag}: ① e2 必须是 +Y（门面竖直的前提）"
    assert abs(b["n"][1]) < 1e-12, f"{tag}: ① 门面法线必须水平（接收面竖直）"
    best_n = math.hypot(b["L"][0], b["L"][2])
    assert abs(b["sunFrac"] - best_n) < 1e-12, \
        f"{tag}: ① 门面法线不是最大日照方向（{b['sunFrac']:.6f} vs {best_n:.6f}）"
    cross_e1e2 = (-b["e1"][2], 0.0, b["e1"][0])   # e1 × (+Y)
    assert all(abs(cross_e1e2[k] - b["n"][k]) < 1e-12 for k in range(3)), \
        f"{tag}: ① e1 × e2 ≠ n（quad 的法线就是错的，门面会朝反方向）"
    frA = models["A"][0]
    assert abs(frA["gU"]) < 0.02 * abs(frA["gY"]), \
        f"{tag}: ③ 接缝不近水平（gU/gY = {frA['gU']/frA['gY']:.4f}）——接缝与竖直影缘夹角太小"
    assert abs(frA["gY"]) > 1e-6, f"{tag}: ③ 视深沿门面高度无梯度（gY=0）——接缝根本不上墙"
    for nm, (fr, cm) in models.items():
        for c in corners:
            p = wall_camera_px(fr, c)
            assert p is not None and 30 < p[0] < WALL_CAPTURE[0] - 30 \
                and 30 < p[1] < WALL_CAPTURE[1] - 30, \
                f"{tag}: ⑧ 档 {nm} 的门面角点投出画外/贴边"
            for uc, y0, y1 in bars:
                for v in wall_bar_verts(uc, y0, y1, b):
                    pv = wall_camera_px(fr, v)
                    assert pv is None or not (-60 < pv[0] < WALL_CAPTURE[0] + 60
                                              and -60 < pv[1] < WALL_CAPTURE[1] + 60), \
                        f"{tag}: ⑤ 档 {nm} 的遮挡杆角点落进画面（px={tuple(round(x) for x in pv)}）" \
                        f"——闭式门面掩码的前提破了"
        dlo = min(fr["c0"] + fr["gU"] * u + fr["gY"] * y
                  for u in (-WALL_W / 2, WALL_W / 2) for y in (0.0, WALL_H))
        dhi = max(fr["c0"] + fr["gU"] * u + fr["gY"] * y
                  for u in (-WALL_W / 2, WALL_W / 2) for y in (0.0, WALL_H))
        assert dhi < 0.86 * cm["farQ"], \
            f"{tag}: ④ 档 {nm} 门面最远视深 {dhi:.3f} 进末级淡出带（0.86·farQ=" \
            f"{0.86*cm['farQ']:.3f}）——cov 被 atten 吃掉，读数系统性偏受光"
        # ②' 🔴 **不再**要求「模型算出的接缝穿过门面」。本批实机逐位读出的结果是：闭式级联
        # 模型（quantize → cascadeSplitDistance）在 a = 11…16 段与引擎实际选级不符，而
        # 「接缝在不在门面上」恰恰是**被测命题**——把它写成构建期硬 assert 等于用假设堵死结论，
        # 也等于让靶永远只能生成「模型说接缝在门面上」的那几档（实机却不在）。
        # 换成两条与模型无关、且判据真正依赖的前提：
        assert dhi > dlo + 1e-9, f"{tag}: ②' 档 {nm} 门面视深区间退化（区间宽度 0）"
        # 以及「模型接缝位置」必须**写进** manifest 供仪器对账（缺键即抛，见下面的 measure 装配）。
        _ = cm["splits"][WALL_FORCE_CASCADE - 1]
    for uc, y0, y1 in bars:
        assert y0 - b["kV"] * WALL_HBAR <= 0.0 and y1 - b["kV"] * WALL_HBAR >= WALL_H, \
            f"{tag}: ⑥ 杆 u={uc:+.2f} 的影盖不住整块门面"
    for a_nm, b_nm, kind in WALL_CAUSAL_PAIRS:
        sa = models[a_nm][1]["splits"][WALL_FORCE_CASCADE - 1]
        sb = models[b_nm][1]["splits"][WALL_FORCE_CASCADE - 1]
        if kind == "same":
            assert (models[a_nm][1]["nearQ"], models[a_nm][1]["farQ"]) == \
                   (models[b_nm][1]["nearQ"], models[b_nm][1]["farQ"]), \
                f"{tag}: ⑦ 同桶对 {a_nm}/{b_nm} 其实不同桶"
            assert abs(sa - sb) < 1e-12, \
                f"{tag}: ⑦ 同桶对 {a_nm}/{b_nm} 的 splits 不等（{sa} vs {sb}）——「同桶内挪相机" \
                f"接缝视深不变」这条预测根本不成立"
        else:
            assert abs(sa - sb) > 0.5, \
                f"{tag}: ⑦ 跨桶对 {a_nm}/{b_nm} 的 splits 只差 {abs(sa-sb):.4f} m（要 > 0.5 m）" \
                f"——「拧切分距离」的幅度不够，因果实验没有区分力"
    assert 0.0 < WALL_STIMULUS_M < 0.5, f"{tag}: 激励量 {WALL_STIMULUS_M} 越出同桶窗口"
    return s


SCENES = {
    "cornell_box": scene_cornell_box,
    "shadow_rig": scene_shadow_rig,
    "izb_rig": scene_izb_rig,
    "nm_rig": scene_nm_rig,
    "penumbra_ladder": scene_penumbra_ladder,
    "ssr_rig": scene_ssr_rig,
    "ssr_rig_empty": scene_ssr_rig_empty,
    "blend_sort": scene_blend_sort,
    # 债-134 前置：BLEND 臂 + 反向臂（同函数、只差 alphaMode），两臂必须都生成
    "blend_lit": scene_blend_lit,
    "blend_lit_opaque": scene_blend_lit_opaque,
    "shadow_grain": scene_shadow_grain,
    "shadow_edge_rig": scene_shadow_edge_rig,
    "shadow_wall_rig": scene_shadow_wall_rig,
    # 远工作点（相对坐标批 5 第 2 刀）：同一台靶整体沿世界 +X 平移 1e6 m，闭式真值跟着平移
    # 并写进 manifest 的 measure 块。GLB 与出厂靶逐字节相同（大坐标只住 entity position）。
    "shadow_edge_rig_far": lambda: scene_shadow_edge_rig(world_offset=EDGE_FAR_OFFSET),
}


# ---------------------------------------------------------------------------
# glb 写出器（风格与 buffer 布局参照 tools/bistro_pack.py 的尾追加分配器，但此处
# 从零构造：无源文件、无贴图 → 单 buffer 纯几何）
# ---------------------------------------------------------------------------

COMP_F32 = 5126
COMP_U16 = 5123
_TYPE_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}


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
        self.images: list[dict] = []      # 内嵌 PNG（normal map）；空 = JSON 不写 images 键

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

    def _add_image(self, png: bytes) -> int:
        """内嵌 PNG → images[] 下标（= material.normalTexture.index 的取值空间）。

        ⚠ 这里**刻意不建 glTF 的 textures[]/sampler[] 间接层**：引擎把 normalTexture.index
        当 images[] 下标用（gltf_loader_impl.cppm:269 与 gltf.images.size() 比、
        texture_pool.cppm uploadImageSlot 直接 model.images[imageIndex]），bistro_pack.py:292
        的 remap 也是直接映射到 images。写标准形态 = 下标越界 = **静默**回落中性纹理。
        """
        self.images.append({"bufferView": self._append_bv(png), "mimeType": "image/png"})
        return len(self.images) - 1

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
        attrs = {"POSITION": pos_acc, "NORMAL": nrm_acc}
        if piece.uv is not None:
            uv_bytes = struct.pack(f"<{2 * n}f", *(c for u in piece.uv for c in u))
            attrs["TEXCOORD_0"] = self._add_accessor(uv_bytes, n, "VEC2", COMP_F32, None)
        if piece.tangent is not None:
            tan_bytes = struct.pack(f"<{3 * n}f", *(c for t in piece.tangent for c in t))
            attrs["TANGENT"] = self._add_accessor(tan_bytes, n, "VEC3", COMP_F32, None)

        mat: dict = {
            "name": piece.name,  # 引擎 material_styles 连连看的匹配字段（见 Piece 校验）
            "pbrMetallicRoughness": {
                # 三因子全部显式写出（含 metallic/roughness）：不吃 tinygltf / 规范默认值，
                # manifest 的 color 与 JSON 的 baseColorFactor 才能做精确相等校验。
                "baseColorFactor": [piece.color[0], piece.color[1], piece.color[2], piece.alpha],
                "metallicFactor": piece.metallic,
                "roughnessFactor": piece.roughness,
            },
        }
        if piece.normal_png is not None:
            # index = images[] 下标（见 _add_image 的 ⚠）；texCoord 显式写 0，
            # 不吃"缺省即 0"的默认值（与三因子同一纪律）。
            mat["normalTexture"] = {"index": self._add_image(piece.normal_png), "texCoord": 0}
        if piece.emissive is not None:
            mat["emissiveFactor"] = [piece.emissive[0], piece.emissive[1], piece.emissive[2]]
        # alphaMode 只在**真半透**时写（写 "OPAQUE" 会改既有场景的 JSON 字节）。引擎侧
        # 判据 = mat.alphaMode == "BLEND" 字符串相等（gltf_loader_impl 的 alphaModeBlend），
        # 缺键 = 规范默认 "OPAQUE"。
        if piece.alpha < 1.0:
            mat["alphaMode"] = "BLEND"
        if piece.unlit:
            # 引擎只读**存在性**（extensions.count(...) > 0），空对象即合法（glTF 规范同义）。
            mat["extensions"] = {"KHR_materials_unlit": {}}
        self.materials.append(mat)
        mat_i = len(self.materials) - 1
        mesh_i = len(self.meshes)
        self.meshes.append({
            "name": piece.name,
            "primitives": [{
                "attributes": attrs,
                "indices": idx_acc,
                "material": mat_i,
                # 无 mode 字段 = 默认 4 (TRIANGLES)；TEXCOORD_0/TANGENT 仅在 piece 带时写
            }],
        })
        self.nodes.append({"name": piece.name, "mesh": mesh_i})
        if piece.translation is not None:
            # 键序 = name, mesh, translation（既有场景不写这个键 = 字节不变）。
            self.nodes[-1]["translation"] = [piece.translation[0], piece.translation[1],
                                             piece.translation[2]]

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
        # images 键**只在有内嵌图时写**：既有五个场景没有图，多一个空数组键就改了 JSON
        # 字节 = 改了 md5 = 所有既有图像基准全部失效（本次改动的硬验收项）。
        if self.images:
            gltf["images"] = self.images
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
        want_alpha = entry.get("alpha", 1.0)
        if bcf != [entry["color"][0], entry["color"][1], entry["color"][2], want_alpha]:
            errs.append(f"piece {entry['name']}: baseColorFactor != manifest color/alpha")
        # ── 透明与 unlit 路由的三方对账（JSON ↔ manifest ↔ 引擎判据口径）──────────
        # 堵的是三条各自静默的路：① 写了 alphaMode 但 manifest 没说（harness 按 OPAQUE
        # 预测 → 靶自己骗自己）② manifest 说了但 JSON 没写（引擎走 opaque，透明段零 draw，
        # 画面"看着对"因为回落路径恰好也像混合）③ unlit 扩展漏写（材质被静默当 PBR 打光，
        # 正是 2026-10-18 aa-coverage 电平自检抓出的那类 D1 破口）。
        json_blend = mat.get("alphaMode", "OPAQUE") == "BLEND"
        if json_blend != (want_alpha < 1.0):
            errs.append(f"piece {entry['name']}: alphaMode={mat.get('alphaMode')!r} 与 "
                        f"baseColorFactor.a={want_alpha} 不自洽")
        if json_blend != (entry.get("alphaMode") == "BLEND"):
            errs.append(f"piece {entry['name']}: JSON alphaMode 与 manifest alphaMode 不符")
        json_unlit = "KHR_materials_unlit" in mat.get("extensions", {})
        if json_unlit != bool(entry.get("unlit")):
            errs.append(f"piece {entry['name']}: KHR_materials_unlit 存在性与 manifest "
                        f"unlit 字段不符（存在性 = 引擎路由的唯一判据）")
        want_tr = entry.get("translation")
        if node.get("translation") != want_tr:
            errs.append(f"piece {entry['name']}: node.translation={node.get('translation')} "
                        f"!= manifest {want_tr}（node 原点 = 透明排序代理，错一位 = 判据量错层）")
        if mat.get("name") != entry["name"]:
            errs.append(f"piece {entry['name']}: material name mismatch")
        # ── 纹理面深校验（manifest 声明了才查；既有五场景声明里没有 → 整段跳过）──────
        # 每条断言都在堵一条**静默回落**路径：引擎侧纹理缺失从不报错，只 warn 一句然后绑
        # 白/中性纹理，实验照跑、指标照出，最后得到一个"normal map 无影响"的假负结果。
        nrm_tex = entry.get("normalTexture")
        if nrm_tex is None:
            if "normalTexture" in mat:
                errs.append(f"piece {entry['name']}: JSON 有 normalTexture 但 manifest 未申报")
            if "TEXCOORD_0" in prim["attributes"] or "TANGENT" in prim["attributes"]:
                errs.append(f"piece {entry['name']}: JSON 有 TEXCOORD_0/TANGENT 但 manifest 未申报")
        else:
            # ① TEXCOORD_0 必须存在（缺 → uploadModelTextures 的 !hasUv 早退，normal 槽回落）
            if "TEXCOORD_0" not in prim["attributes"]:
                errs.append(f"piece {entry['name']}: 带 normalTexture 却无 TEXCOORD_0 "
                            f"—— 引擎会静默回落中性纹理")
            else:
                uv = root["accessors"][prim["attributes"]["TEXCOORD_0"]]
                if uv["count"] != pos["count"] or uv["type"] != "VEC2":
                    errs.append(f"piece {entry['name']}: TEXCOORD_0 count/type 与顶点不符")
                else:
                    uoff = (root["bufferViews"][uv["bufferView"]]["byteOffset"]
                            + uv.get("byteOffset", 0))
                    uvals = struct.unpack(f"<{2 * uv['count']}f",
                                          bin_data[uoff:uoff + 8 * uv["count"]])
                    if any(not math.isfinite(v) for v in uvals):
                        errs.append(f"piece {entry['name']}: TEXCOORD_0 含非有限值")
                    lo, hi = nrm_tex.get("uvRange", [None, None])
                    got = (min(uvals[0::2]), min(uvals[1::2]))
                    got_hi = (max(uvals[0::2]), max(uvals[1::2]))
                    if (round(got[0], 5), round(got[1], 5)) != (round(lo[0], 5), round(lo[1], 5)) \
                            or (round(got_hi[0], 5), round(got_hi[1], 5)) != (round(hi[0], 5),
                                                                              round(hi[1], 5)):
                        errs.append(f"piece {entry['name']}: uv 范围 {got}..{got_hi} "
                                    f"!= manifest {lo}..{hi}（world/texel 是频率轴的真值）")
            # ② TANGENT 单位长（非单位 = 切线基被缩放，tilt 角度不再是设计值）
            if "TANGENT" in prim["attributes"]:
                ta = root["accessors"][prim["attributes"]["TANGENT"]]
                toff = (root["bufferViews"][ta["bufferView"]]["byteOffset"]
                        + ta.get("byteOffset", 0))
                tvals = struct.unpack(f"<{3 * ta['count']}f",
                                      bin_data[toff:toff + 12 * ta["count"]])
                for k in range(0, 3 * ta["count"], 3):
                    l2 = tvals[k] ** 2 + tvals[k + 1] ** 2 + tvals[k + 2] ** 2
                    if abs(l2 - 1.0) > 1e-4:
                        errs.append(f"piece {entry['name']}: tangent #{k // 3} len^2={l2} 非单位")
                        break
            # ③ normalTexture.index 必须落在 images[]（引擎就是按 images 下标取的）
            imgs = root.get("images", [])
            ti = mat.get("normalTexture", {}).get("index", -1)
            if not (0 <= ti < len(imgs)):
                errs.append(f"piece {entry['name']}: normalTexture.index {ti} 出 images[] "
                            f"（引擎按 images 下标取，越界 = 静默回落）")
                continue
            img = imgs[ti]
            if img.get("mimeType") != "image/png" or "bufferView" not in img:
                errs.append(f"piece {entry['name']}: image {ti} 不是内嵌 PNG"
                            f"（引擎 collectImages 只认 bufferView，URI 外部文件不支持）")
                continue
            ibv = root["bufferViews"][img["bufferView"]]
            png = bin_data[ibv["byteOffset"]:ibv["byteOffset"] + ibv["byteLength"]]
            # ④ PNG 结构 + CRC + 像素逐字节回环（写出器写错 chunk 的失败必须在这里炸，
            #     而不是在引擎侧变成一条 warn + 一张中性纹理）
            try:
                pw, ph, prows = png_decode_rgb(png)
            except Exception as e:  # noqa: BLE001 — 报错转文本进 errs
                errs.append(f"piece {entry['name']}: 内嵌 PNG 自检解码失败：{e}")
                continue
            if [pw, ph] != list(nrm_tex["size"]):
                errs.append(f"piece {entry['name']}: PNG 尺寸 {pw}×{ph} != manifest "
                            f"{nrm_tex['size']}")
            # ⑤ 码字直方图逐字节回环（图案设计值的唯一真相源 = 生成函数，
            #     与 PNG 写出器互验：两者任一处写错这里就红）
            want = nrm_tex["codeWordHistogram"]
            got_hist: dict[str, int] = {}
            for row in prows:
                for k in range(0, len(row), 3):
                    key = f"{row[k]},{row[k + 1]},{row[k + 2]}"
                    got_hist[key] = got_hist.get(key, 0) + 1
            if got_hist != want:
                errs.append(f"piece {entry['name']}: PNG 码字直方图 != manifest（差异 "
                            f"{sorted(got_hist.items())[:3]} vs {sorted(want.items())[:3]}）")
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
    # 「nodes identity」这句对带平移的场景是**谎言**，就地改准（既有场景不进这个分支 = 字节不变）。
    if any(p.translation is not None for p in scene.pieces):
        manifest["coordinateSystem"] = (
            "Y-up, meters; piece geometry baked in coords relative to its own node origin, "
            "placement via node translation (per-piece 'translation'). 🔴 node origin is "
            "also the renderListSystem transparent sort proxy (tf.world[3]) -- that is why "
            "a sorting target MUST translate its nodes, not just its vertices")
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
        # ── 透明/平移事实（默认值不写 = 既有七场景 manifest 字节不变）─────────────
        # 闭式混合判据要**精确**的 alpha 与 unlit 路由，不能靠"看名字猜"：harness 读这里，
        # 不读 configs/scene.lua，也不在脚本里再抄一份颜色/alpha（那正是本项目反复踩的
        # 「访问路径没吃单一来源」）。
        if p.alpha != 1.0:
            entry["alpha"] = p.alpha
            entry["alphaMode"] = "BLEND"
        if p.unlit:
            entry["unlit"] = True
        if p.translation is not None:
            entry["translation"] = [p.translation[0], p.translation[1], p.translation[2]]
            # aabb 是**局部**量（顶点没烘平移）；世界落位 = translation。
            entry["aabbSpace"] = "local (node translation carries placement)"
        # 纹理面事实（只在带 normal map 时写 = 既有五场景 manifest 字节不变）。
        # 这里记的是**量化后**的事实（码字直方图 + 反算 tilt），harness 做闭式预测时
        # 必须用它、而不是用"请求角度"——见 _normal_rows 的量化三条回答第 3 条。
        if p.normal_png is not None:
            hist: dict[str, int] = {}
            for row in p.normal_rows:
                for k in range(0, len(row), 3):
                    key = f"{row[k]},{row[k + 1]},{row[k + 2]}"
                    hist[key] = hist.get(key, 0) + 1
            uvs = p.uv
            entry["normalTexture"] = {
                "size": [p.normal_info["texels"], p.normal_info["texels"]],
                "uvRange": [[_f32(min(u[0] for u in uvs)), _f32(min(u[1] for u in uvs))],
                            [_f32(max(u[0] for u in uvs)), _f32(max(u[1] for u in uvs))]],
                "codeWordHistogram": {k: hist[k] for k in sorted(hist)},
                **p.normal_info,
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
        description="程序化 QA 场景生成器（场景清单 = --only 的取值，即 SCENES 注册表键，"
                    "此处不抄名单）。输出默认落 build/qa_scenes/（gitignore 内；引擎 "
                    "CWD=build/rgl/，harness 用绝对路径或拷入 build/rgl/models/ 加载）。")
    ap.add_argument("--out", default="build/qa_scenes", help="输出目录（默认 build/qa_scenes）")
    ap.add_argument("--only", choices=sorted(SCENES), help="只生成指定场景")
    # ── nm_rig 的自变量（仅 --only nm_rig 时生效；文件名自带参数，可一族并存）──────
    ap.add_argument("--nm-amp", type=float, default=2.0,
                    help="normal map tilt 幅度（度）。0 = 负对照（整图单码字）")
    ap.add_argument("--nm-block", type=int, default=1,
                    help="tilt 场块大小（texel）= 频率轴。固定 uv_scale 扫它才是单变量"
                         "（mip level 只由 UV 屏幕导数决定、与内容无关）")
    ap.add_argument("--nm-pattern", choices=NM_PATTERNS, default="blocks")
    ap.add_argument("--nm-texels", type=int, default=NM_TEXELS)
    ap.add_argument("--nm-uv-scale", type=float, default=NM_UV_SCALE,
                    help="每纹理重复的世界米数（texel 屏幕尺寸轴，会同时改 mip 过滤强度）")
    ap.add_argument("--nm-roughness", type=float, default=0.12)
    ap.add_argument("--nm-pitch", type=float, default=NM_PITCH_DEG,
                    help="相机俯角（度），必须 == env 窄带仰角")
    args = ap.parse_args(argv)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    grand_bytes = 0
    for name in (sorted(SCENES) if not args.only else [args.only]):
        if name == "nm_rig":
            scene = scene_nm_rig(amp_deg=args.nm_amp, block=args.nm_block,
                                 pattern=args.nm_pattern, texels=args.nm_texels,
                                 uv_scale=args.nm_uv_scale, roughness=args.nm_roughness,
                                 pitch_deg=args.nm_pitch)
        else:
            scene = SCENES[name]()
        glb, manifest_json, stats = build_scene(scene)
        stem = scene.out_name or scene.name
        (out / f"{stem}.glb").write_bytes(glb)
        (out / f"{stem}.manifest.json").write_text(manifest_json, encoding="ascii")
        grand_bytes += stats["bytes"] + stats["manifestBytes"]
        print(f"{stem:<12} pieces={stats['pieces']:>2}  tris={stats['tris']:>5}  "
              f"glb={stats['bytes']:>7} B  manifest={stats['manifestBytes']:>6} B  "
              f"self-check=OK"
              + (f"  cascade-self-proof: {stats['cascade']} 预测级联（occupied）"
                 if stats.get("cascade") else "")
              + f"  -> {out}/{stem}.glb")
    print(f"{'TOTAL':<12} {'':>10} {'':>10} out={out}  totalBytes={grand_bytes}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
