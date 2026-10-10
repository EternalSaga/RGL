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
# cornell_box / shadow_rig / d1_parity / izb_rig / env_band 五个场景**一个字节都没变**
# （改前改后 md5 逐字节比对是本次改动的硬验收项）。只有 nm_rig 需要纹理面，它显式写。
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
# ## 五个场景的诚实定位
#
# - cornell_box：**只是 Cornell box 的几何**。真正的标志性特征是面光源 + 颜色溢出，
#   本引擎没有面光源（只有方向光 + 环境 cubemap，见 configs/scene.lua light 段），
#   所以它验证的是「几何正确性 + 双色墙 + 多材质 + AO 接触」，**不是**辐射度量学意义上的
#   Cornell box 基准，不要拿它跟经典 radiance 参考图对比。
# - shadow_rig：最重要。按 skill rgl-verify「阴影验收判据」硬要求 1-3 设计（阳性锚点 /
#   跨级联分布 / 远景取景），逐物体的失效模式在场景函数注释里。
# - d1_parity：forward / deferred 两路对拍（D1 指标）用的中等面数 + 材质混合 + 自发光面板。
# - izb_rig：FTIZB/IZB 硬阴影 spike 装置（docs/hfts_izb_plan.md 批 1a）——厚度梯
#   0.4→0.02 m + 接触柱 + 穿插杆 + 叠影三分带；判据 = shadow.lua 的 izb 开关 A/B 对拍。
# - env_band：变体 1「带边」——metallic=1 平坦地面 + 四条并排 roughness 档，配程序化
#   env（纯黑 + 极亮窄带，tools/env_band_hdr.py），掠射机位让窄带镜面反射横贯画面；
#   判据 = speck / phase-lock / grain 三口径的受控实验表（TAA × env × roughness）。
# - nm_rig：**带 normal map 的受控靶**（本轮加）。单材质单 quad 平坦金属地面 + 程序化
#   normal map，扰动**幅度**与**频率**两轴独立可扫。存在的理由是一条构造性事实：
#   model_pbr_ibl.frag.slang:52-53 无 normal map 时 `geoN = worldNormal(...)`、`N = geoN`
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
        if self.emissive is None and ("Emissive" in self.name or "Foliage" in self.name):
            raise ValueError(f"piece {self.name}: name would be style-routed by "
                             f"material_styles pattern; rename it")
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
# 场景 5：env_band —— 变体 1「带边」：金属地面把极亮窄带的镜面反射扫过画面
# ---------------------------------------------------------------------------
# 要复现的现象（主 agent 已在 bistro 上定位）：静止机位下「相位锁定的高亮噪点」——某像素在
# 16 个 TAA 抖动相位里恰好 1 个相位冲到 255、其余相位只有 6~22。机制假设 = 低 roughness
# 金属面的 IBL 镜面项反射了高对比 env 的极亮窄带，被抖动切成相位锁定噪点。
#
# 为什么这个 rig 比 bistro 好归因：bistro 的 env（san_giuseppe 4k）里"亮区在哪、多宽、多亮"
# 全是素材巧合，改不动。本 rig 把 env 换成程序化生成的**纯黑 + 一条恒定仰角的极亮窄带**
# （tools/env_band_hdr.py），于是自变量只剩三个可写进文件名的数：带的仰角 / 角宽 / 亮度。
# 地面再给四档 roughness 并排 ⟹ **一次渲染同时看到单调性**，不必跑四次再跨会话比。
#
# ## 几何为什么这么摆（掠射 + 镜像线，全部闭式）
#
# 平面 N=+Y 的镜面反射方向 D_ref = D_in − 2(D_in·N)N ⟹ **D_ref 的仰角 = −视线仰角**。
# 于是"env 里仰角 θ_b 的水平带"在地面上的镜像 = 一条**恒定视线仰角 −θ_b 的水平线**，
# 与方位角无关（带子是整圈 ⟹ 镜像线横贯全画面）。相机俯角取 α = θ_b ⟹ 镜像线正好落在
# 画面主点行，且画面里"哪一行反射带子"是闭式可预测的（manifest.extra.probe）。
#
# 掠射角刻意取小（α = 3°）：① 镜像线离地平线近，地面在画面里铺得开；② 掠射端 Fresnel→1，
# 金属镜面项最强。地面必须铺到镜像线之外（闭式校验 A4），否则看到的是地面尽头不是反射。
#
# ## 四条带 = 四条 X 向等宽长条（1 piece = 1 mesh = 1 primitive = 1 material）
#
# 本文件的 piece 契约要求每 piece 独立材质，而**同场景 piece 颜色两两距离 ≥ 0.5**（见 PAL
# 注释与 validate_glb_bytes）⟹ 四条带不能同色。这是本 rig 唯一的已知混淆项：metallic=1 时
# baseColorFactor 就是金属染色，四档的**亮度标尺不完全相同**。两条缓解：
#   ① 挑调色板里 BT.601 亮度权重最接近的四色（yellow/chartreuse/cyan/green，权重
#      0.886/0.737/0.701/0.587，最大/最小 = 1.51×，而预测效应是"显著 → ~0"的量级差）；
#   ② harness 额外报"按实测平台亮度归一"的口径做稳健性对照。
# 权重差与 roughness 档的对应关系写进 manifest（extra.probe.bands[].lumaWeight601），
# 不藏。要彻底消掉混淆只能加中性色（改 PAL 固定表 = 动全表判据），刻意没做。
#
# ## 构建期断言（A1–A5）
#
# A1 镜像线在画面内且离上下边有裕量（否则"扫过画面"不成立）；A2 四条带在镜像线处**都可见**
# 且各自像素宽 ≥ 128（否则某一档根本没样本，单调性表是空的）；A3 roughness 严格递增
# （梯子公平性，同 izb_rig 的截面梯判据）；A4 地面在镜像线处铺满整个可见横向范围
# （否则画面边缘是"地面尽头"不是反射）；A5 四档同色同几何、**只有 roughness 不同**
# （单变量纪律：任何别的差异都会伪装成 roughness 效应）。

ENV_BAND_H = 1.0            # 相机离地高度（m）
ENV_BAND_VIEW_ELEV = 3.0    # 俯角 = env 带子仰角（度）⟹ 镜像线落在主点行
ENV_BAND_FOV_V = 45.0       # 垂直 fov（glm::perspective 的 fovy，见 rgl/ecs_impl.cppm）
ENV_BAND_STRIP_W = 10.0     # 每条 roughness 带的 X 宽（m）
ENV_BAND_Z_NEAR = 30.0      # 地面向相机侧延伸（m）
ENV_BAND_Z_FAR = 300.0      # 地面向视线方向延伸（m）；远端落在黑色天穹里 = 边缘不可见
ENV_BAND_ROUGHNESS = (0.12, 0.25, 0.5, 0.75)
ENV_BAND_COLORS = ("yellow", "chartreuse", "cyan", "green")  # 见上「唯一混淆项」
# PIL/ITU-R BT.601 的 L 权重（度量脚本 convert("L") 用的就是这套，不是 BT.709）
K601 = (0.299, 0.587, 0.114)


def env_band_probe(width: int = 1280, height: int = 720) -> dict:
    """镜像线行号与四条带在镜像线处的像素边界（闭式，供 harness 定位 + 构建期断言）。

    逐式来源：视线仰角 −θ_b 的射线打到 y=0 的地面 ⟹ 距离 d = h/tan(θ_b)；
    地面点 (c, 0, −d) 的屏幕横坐标 = 中心 + (c/d)/tan(hfov/2) · (W/2)，
    其中 tan(hfov/2) = tan(fov_v/2)·(W/H)（垂直 fov + 宽高比，glm 约定）。
    """
    t = math.tan(math.radians(ENV_BAND_VIEW_ELEV))
    d = ENV_BAND_H / t
    tan_half_h = math.tan(math.radians(ENV_BAND_FOV_V / 2.0)) * (width / height)
    edges = []
    for i in range(len(ENV_BAND_ROUGHNESS) + 1):
        c = (i - len(ENV_BAND_ROUGHNESS) / 2.0) * ENV_BAND_STRIP_W
        edges.append(width / 2.0 + (c / d) / tan_half_h * (width / 2.0))
    return {
        "mirrorRow": height / 2.0,          # 俯角 = 带仰角 ⟹ 镜像线 = 主点行
        "mirrorDistanceM": d,
        "stripEdgesPxAtMirrorRow": edges,
        "pxPerDegElevation": height / ENV_BAND_FOV_V,
        "frameWidth": width,
        "frameHeight": height,
    }


def scene_env_band() -> Scene:
    n = len(ENV_BAND_ROUGHNESS)
    x0 = -n * ENV_BAND_STRIP_W / 2.0
    probe = env_band_probe()

    s = Scene(
        name="env_band",
        purpose=("变体 1「带边」：metallic=1 平坦地面 + 四条并排 roughness 档 "
                 f"{list(ENV_BAND_ROUGHNESS)}，配程序化 env（纯黑 + 仰角 "
                 f"{ENV_BAND_VIEW_ELEV}° 极亮窄带），掠射机位让窄带的镜面反射横贯画面"),
        honesty=("地面**完全平坦且无纹理**（无 TEXCOORD_0/TANGENT/normal map）——这是刻意的，"
                 "变体 1 只测「env 窄带 × roughness」这一条轴。代价如实写在这里：本引擎的 "
                 "shading 法线 = 插值顶点法线（rgl/shaders/model_pbr_ibl.frag.slang:52-53 "
                 "geoN = worldNormal(M, normalize(input.normal))，N = geoN），无 normal map "
                 "时**逐像素法线变化与 specular AA 的输入是同一个量**，所以本 rig 造不出"
                 "「法线高频变化而 specular AA 不察觉」那一类噪点。bistro 湿面有 normal map，"
                 "这一轴本 rig 覆盖不到，不要拿本 rig 的绿/红去否证 bistro 的机制。"),
        cameras={
            "grazing": {"position": [0.0, ENV_BAND_H, 0.0],
                        "target": [0.0,
                                   ENV_BAND_H - 10.0 * math.sin(math.radians(ENV_BAND_VIEW_ELEV)),
                                   -10.0 * math.cos(math.radians(ENV_BAND_VIEW_ELEV))],
                        "fovDegrees": ENV_BAND_FOV_V},
        },
    )
    P = s.pieces.append
    for i, r in enumerate(ENV_BAND_ROUGHNESS):
        x = x0 + i * ENV_BAND_STRIP_W
        P(Piece(f"rough_{r:.2f}",
                quad((x, 0.0, -ENV_BAND_Z_FAR), (0.0, 0.0, ENV_BAND_Z_FAR + ENV_BAND_Z_NEAR),
                     (ENV_BAND_STRIP_W, 0.0, 0.0)),
                PAL[ENV_BAND_COLORS[i]], metallic=1.0, roughness=r,
                radial=False, closed=False,
                intent=(f"roughness={r} 档（metallic=1，N=+Y 平坦）；"
                        f"镜像线处像素 x ∈ [{probe['stripEdgesPxAtMirrorRow'][i]:.0f},"
                        f"{probe['stripEdgesPxAtMirrorRow'][i + 1]:.0f})")))

    s.extra["probe"] = dict(probe)
    s.extra["probe"]["bands"] = [
        {"piece": f"rough_{r:.2f}", "roughness": r,
         "color": list(PAL[ENV_BAND_COLORS[i]]),
         "lumaWeight601": round(sum(a * b for a, b in zip(K601, PAL[ENV_BAND_COLORS[i]])), 6),
         "xRangePx": [round(probe["stripEdgesPxAtMirrorRow"][i], 3),
                      round(probe["stripEdgesPxAtMirrorRow"][i + 1], 3)]}
        for i, r in enumerate(ENV_BAND_ROUGHNESS)]
    s.extra["envRecipe"] = {
        "tool": "tools/env_band_hdr.py",
        "layout": "equirect (latlong), W=2H, row0=天顶, v=0.5-elev/180"
                  "（出处 rgl/texture_pool.cppm loadEnvCubeMapHdr/sampleEquirect）",
        "elevDeg": ENV_BAND_VIEW_ELEV,
        "note": "带子仰角必须 == 相机俯角，否则镜像线跑出主点行（harness 仍按实测行定位）",
    }

    # ── 构建期断言 ──────────────────────────────────────────────────────
    # A1 镜像线在画面内且离上下边 ≥ 15% 画高（离地平线也要有裕量：太贴地平线时
    #    地面远端/侧端会跑进测量窗，把"地面尽头"的边缘当成反射来量）
    h = probe["frameHeight"]
    assert 0.15 * h <= probe["mirrorRow"] <= 0.85 * h, f"A1 镜像线行 {probe['mirrorRow']} 出画"
    horizon_row = probe["mirrorRow"] - ENV_BAND_VIEW_ELEV * probe["pxPerDegElevation"]
    assert horizon_row >= 0.15 * h, f"A1 地平线行 {horizon_row:.0f} 太贴画顶"
    # A2 四条带在镜像线处都可见，且各自像素宽 ≥ 128
    e = probe["stripEdgesPxAtMirrorRow"]
    for i, r in enumerate(ENV_BAND_ROUGHNESS):
        lo, hi = max(e[i], 0.0), min(e[i + 1], float(probe["frameWidth"]))
        assert hi - lo >= 128.0, f"A2 roughness={r} 档在镜像线处仅 {hi - lo:.0f}px（<128）"
    # A3 梯子公平性：roughness 严格递增
    assert all(b > a for a, b in zip(ENV_BAND_ROUGHNESS, ENV_BAND_ROUGHNESS[1:])), \
        "A3 roughness 档非严格递增"
    # A4 地面在镜像线处铺满整个可见横向范围（画面左右边缘对应的地面 x 必须在地面内）
    d = probe["mirrorDistanceM"]
    tan_h = math.tan(math.radians(ENV_BAND_FOV_V / 2.0)) * (probe["frameWidth"] / h)
    x_visible = d * tan_h
    x_ground = n * ENV_BAND_STRIP_W / 2.0
    assert x_ground > x_visible, \
        f"A4 地面半宽 {x_ground} m < 镜像线处可见半宽 {x_visible:.2f} m（画面边缘是地面尽头）"
    # A5 单变量纪律：四 piece 同色板位、同几何尺寸、metallic 全 1，只有 roughness 不同
    assert len(s.pieces) == n
    for p in s.pieces:
        assert p.metallic == 1.0, f"A5 {p.name} metallic ≠ 1"
        assert len(p.geom.positions) == 4, f"A5 {p.name} 不是单 quad"
        ys = {v[1] for v in p.geom.positions}
        assert ys == {0.0}, f"A5 {p.name} 地面不平整（y 出现 {ys}）"
        assert {tuple(round(c, 3) for c in v) for v in p.geom.normals} == {(0.0, 1.0, 0.0)}, \
            f"A5 {p.name} 法线不是恒定 +Y"
    return s


# ---------------------------------------------------------------------------
# 场景 6：nm_rig —— 带 normal map 的受控靶（本轮加，复现 bistro 湿面高亮噪点）
# ---------------------------------------------------------------------------
# ## 为什么必须新造一个靶，而不是在 env_band 上加纹理
#
# env_band 的 honesty 已把那条构造性局限写进文件：本引擎无 normal map 时
# `geoN = worldNormal(M, normalize(input.normal))`、`N = geoN`
# （rgl/shaders/model_pbr_ibl.frag.slang:52-53）⟹ **shading 法线与 specular AA 的输入
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
#   ① deferred 路线可用（deferred 的天穹不乘 env.intensity 也不读 skybox 标志，
#      light.frag.slang:193-203；画幅里没有天穹像素 = 那条不对称根本不被触发）；
#   ② 可用像素从 env_band 的 49 行涨到整窗；
#   ③ 不需要掩膜，也不需要 skybox=false 这条 forward 专属开关。
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
    tan θ_h = ndc_x·tan(fov/2)·(W/H)（垂直 fov + 宽高比，与 env_band_probe 同一约定）。
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
                 roughness: float = 0.12, pitch_deg: float = NM_PITCH_DEG,
                 variant: str = "forward") -> Scene:
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
        "tool": "tools/env_band_hdr.py",
        "args": f"--elev {pitch_deg} --band <W> --intensity <RAD>",
        "note": f"带子仰角必须 == 相机俯角 {pitch_deg}°，否则镜像线跑出主点行 "
                f"（harness 仍按实测行定位）",
    }
    s.extra["variant"] = variant

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


SCENES = {
    "cornell_box": scene_cornell_box,
    "shadow_rig": scene_shadow_rig,
    "d1_parity": scene_d1_parity,
    "izb_rig": scene_izb_rig,
    "env_band": scene_env_band,
    "nm_rig": scene_nm_rig,
    "penumbra_ladder": scene_penumbra_ladder,
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
                "baseColorFactor": [piece.color[0], piece.color[1], piece.color[2], 1.0],
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
        if bcf != [entry["color"][0], entry["color"][1], entry["color"][2], 1.0]:
            errs.append(f"piece {entry['name']}: baseColorFactor != manifest color")
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
        description="程序化 QA 场景生成器（cornell_box / shadow_rig / d1_parity / izb_rig / "
                    "env_band / nm_rig）。输出默认落 build/qa_scenes/（gitignore 内；引擎 CWD=build/rgl/，"
                    "harness 用绝对路径或拷入 build/rgl/models/ 加载）。")
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
    ap.add_argument("--nm-variant", default="forward", choices=("forward", "deferred"))
    args = ap.parse_args(argv)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    grand_bytes = 0
    for name in (sorted(SCENES) if not args.only else [args.only]):
        if name == "nm_rig":
            scene = scene_nm_rig(amp_deg=args.nm_amp, block=args.nm_block,
                                 pattern=args.nm_pattern, texels=args.nm_texels,
                                 uv_scale=args.nm_uv_scale, roughness=args.nm_roughness,
                                 pitch_deg=args.nm_pitch, variant=args.nm_variant)
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
