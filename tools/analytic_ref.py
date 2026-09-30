#!/usr/bin/env python3
"""闭式解着色参考（Phase 12 批 12.2d）—— 「某个像素**该**是多少」的独立第二实现。

## 它解决的问题

QA 的图像判据原本只有两类：与基线图像比（要维护 golden）、或统计量比较（直方图/比值，
松）。这一层给第三类：**解析锚点** —— 在受控配置下，某个像素的值可以由物理公式直接算出，
不需要基线。它能抓的是「整条链的数值系统性偏移」，而那正是跨日 ±1 LSB 那类噪声之外
真正贵的 bug 形态（阴影链 5a 系列里若干轮指标全绿而画面错）。

## 为什么不需要引擎侧加 `analytic = true` 开关（一条实测结论）

原计划要在引擎加一个 analytic 模式「跳过 IBL/SSAO 分支并导出 ACES 前线性值」。
实测**不需要动一行引擎代码**：链上每一项都已有配置门——

| 要关掉的东西 | 配置项 | shader 侧效果 |
|---|---|---|
| IBL 漫反射 + 镜面 | `environment.intensity = 0.0` | 两路统一乘 env.intensity |
| 天空盒背景 | `environment.skybox = false` | 不画背景 |
| 平环境光 | `light.ambient = {0,0,0}` | `light.ambient * albedo` 项归零 |
| SSAO | `ao.strength = 0.0` | `lerp(1,ao,0)` 位级 = 1.0 |
| 曝光 | `exposure = 1.0` | ACES 前线性倍率取中性 |
| 阴影 | `diagnostics.shadowPass = false` | `sampleShadow1Tap` 恒 1.0 |

剩下没门的只有 **specular**——但它在「平面 + 无纹理 + 常数 roughness/metallic」的探针面上
是 20 行可解析的闭式（就是本文件里的 `shade_direct`）。加一个把六个旋钮打包的引擎开关
只会多出一条与单旋钮并行、可能各自漂移的路径，**不换来任何表达能力**，故不做。

配置模板见 `qa/configs/analytic/`（探针场景 + 逐项注释「为什么这一项必须关」）。

## 探针几何为什么选「N ≡ V ≡ L」

把相机放在探针面正前方、光也垂直打在该面上，则屏幕中心像素处
N = V = L = (0,0,1)，于是 H = (0,0,1)、NdotH = VdotH = NdotL = NdotV = 1：

- D = a²/(π·a²² + 1e-7·a²) —— 只剩 roughness 的函数
- F = F0（Schlick 的 `(1-VdotH)⁵` 项整项消失）
- G = 1（两项各自分子分母相同）

**这是刻意挑的退化点**：任何 1/π 约定分歧、F0 取值分歧、G 项写法分歧，都会在没有
几何余量可藏的情况下直接体现在数值上。反过来，一个「随机机位」的对比会把这些偏差
混进掠射角与几何项里，误差方向不单调，出问题更难归因。

## 转录来源（改动这些文件就要同步本文件）

- `rgl/shaders/model_pbr.frag.slang` L25-52 / `model_pbr_ibl.frag.slang` 的 `shadeDirect`
  —— GGX(N) + Schlick-Fresnel + 简化 Smith-Schlick G，`diffuse = albedo(1-metal)/π`
- `rgl/shaders/tonemap.frag.slang` —— `acesTonemap`（Narkowicz 2015）+ 两张矩阵 +
  「AO 在曝光前乘、ACES 在曝光后」的顺序
- swapchain 是 `B8G8R8A8_SRGB`，**store 时硬件做 linear→sRGB 编码**，故 shader 不 pow
  （`tonemap.frag.slang` 头注明确写了「手动 gamma + 硬件编码 = 双重编码发白」）——
  本文件的 `srgb_encode` 就是那一步硬件编码的显式复刻。

## 已知不适用的范围（越界用会得到假失败）

- 只适用**无纹理**表面：albedo / roughness / metallic 必须是材质常数。贴了纹理的
  primitive 需要先解出该纹素（属 `tools/imgqa` 的活）。
- 未复刻 `normalFiltering`（几何法线导数估方差抬 roughness）。平面正视图下导数 ≈ 0
  ⟹ 该函数应返回原 roughness，但**这是一个前提，不是事实**；换非平面或非正视图
  必须先把它补进来，否则误差会被读成「着色链坏了」。
- 未复刻 `applyNormalMapTBN`：探针面必须无 normal map。
- 未复刻多灯循环与点光衰减：探针必须单方向光（`readLightCount() == 0` 走头灯路径）。
"""

from __future__ import annotations

import math
import struct
import sys
from dataclasses import dataclass

# ── 与 tonemap.frag.slang 逐元素一致的矩阵（转置存放，配 mul(x, M) 方向使用）──────
# 内层花括号 = 行；此约定下「中性输入不变」要求 M 的列和 = 1（下面有自检）。
K_ACES_INPUT = (
    (0.59719, 0.07600, 0.02840),
    (0.35458, 0.90834, 0.13383),
    (0.04823, 0.01566, 0.83777),
)
K_ACES_OUTPUT = (
    (1.60475, -0.10208, -0.00327),
    (-0.53108, 1.10813, -0.07276),
    (-0.07367, -0.00605, 1.07602),
)

PI = math.pi


# ═══════════════════════════════════════════════════════════════════════════
# 着色（shadeDirect 的独立复刻）
# ═══════════════════════════════════════════════════════════════════════════

def _mul_vec_mat(v: tuple[float, float, float], m) -> tuple[float, float, float]:
    """行向量 × 矩阵（与 shader 的 `mul(x, M)` 同向：结果第 j 分量 = Σ_i x_i·M[i][j]）。"""
    return (
        v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0],
        v[0] * m[0][1] + v[1] * m[1][1] + v[2] * m[2][1],
        v[0] * m[0][2] + v[1] * m[1][2] + v[2] * m[2][2],
    )


def shade_direct(
    n: tuple[float, float, float],
    v: tuple[float, float, float],
    l: tuple[float, float, float],
    albedo: tuple[float, float, float],
    roughness: float,
    metallic: float,
    light_color: tuple[float, float, float],
) -> tuple[float, float, float]:
    """直接光 BRDF 出射（线性、未曝光、未 tonemap）。

    逐式转录 `model_pbr.frag.slang`：GGX 法线分布（epsilon 相对化 1e-7·a2）+
    Schlick-Fresnel + 简化 Smith-Schlick G（k = a/2）+ `diffuse = albedo(1-metal)/π`。

    与 shader 的对应关系写成注释贴在每一行旁边，方便改动时逐条对账。

    @param n 世界空间法线（单位向量）
    @param v 片元→相机方向（单位向量）
    @param l 片元→光源方向（单位向量；shader 里是 `-light.direction`）
    @param albedo 基色（线性）
    @param roughness 粗糙度（**未经 normalFiltering 调整**，见模块头适用范围）
    @param metallic 金属度 0..1
    @param light_color 光色 × 强度（shader 里 `light.color` 已含 intensity）
    @return 线性 RGB 出射
    """
    f0 = tuple(0.04 + (a - 0.04) * metallic for a in albedo)  # lerp(float3(0.04), albedo, m)

    ndotl = max(0.0, _dot(n, l))          # saturate(dot(N, L))
    ndotv = max(_dot(n, v), 1e-4)         # max(dot(N, V), 1e-4)
    h = _norm(_add(v, l))                 # normalize(V + L)
    ndoth = max(0.0, _dot(n, h))          # saturate
    vdoth = max(0.0, _dot(v, h))          # saturate

    a = roughness * roughness
    a2 = a * a
    d = ndoth * ndoth * (a2 - 1.0) + 1.0
    dd = PI * d * d + 1e-7 * a2           # 相对 epsilon（绝对 epsilon 会削掉高光峰）
    dist = a2 / dd
    fres = tuple(f0c + (1.0 - f0c) * (1.0 - vdoth) ** 5 for f0c in f0)
    k = a * 0.5
    gspec = (ndotv / (ndotv * (1.0 - k) + k)) * (ndotl / (ndotl * (1.0 - k) + k))
    spec = tuple(dist * gspec * fc / (4.0 * ndotv * ndotl + 1e-4) for fc in fres)
    diff = tuple(ac * (1.0 - metallic) / PI for ac in albedo)

    term = tuple(dc + sc for dc, sc in zip(diff, spec))
    return tuple(cc * ndotl * tc for cc, tc in zip(light_color, term))


def shade_surface(
    n, v, l, albedo, roughness, metallic,
    light_color, ambient=(0.0, 0.0, 0.0), exposure=1.0, shadow=1.0,
) -> tuple[float, float, float]:
    """完整线性链：直接光 + 平环境光（其余项由配置归零），阴影只乘直接光。

    @param shadow `sampleShadow1Tap` 的返回值（探针配置下恒 1.0）
    @return ACES **之前**的线性 HDR 值
    """
    direct = shade_direct(n, v, l, albedo, roughness, metallic, light_color)
    direct = tuple(c * shadow for c in direct)     # shader：shadowFactor 只乘直接光
    amb = tuple(ac * ac2 * (1.0 - metallic) for ac, ac2 in zip(ambient, albedo))
    return tuple(a + b for a, b in zip(amb, direct))


# ═══════════════════════════════════════════════════════════════════════════
# 显示链（tonemap.frag.slang + 硬件 sRGB store 的独立复刻）
# ═══════════════════════════════════════════════════════════════════════════

def aces_tonemap(x: tuple[float, float, float]) -> tuple[float, float, float]:
    """Narkowicz 2015 ACES 近似（per-channel 有理二次式），输出仍在线性显示域。

    顺序与 shader 相同：先 ×曝光，再 ACES；saturate 在乘输出矩阵**之后**。
    """
    vv = _mul_vec_mat(x, K_ACES_INPUT)
    a = tuple(v * (v + 0.0245786) - 0.000090537 for v in vv)
    b = tuple(v * (0.983729 * v + 0.432951) + 0.238081 for v in vv)
    out = _mul_vec_mat(tuple(ac / bc for ac, bc in zip(a, b)), K_ACES_OUTPUT)
    return tuple(min(1.0, max(0.0, c)) for c in out)


def srgb_encode(c: float) -> float:
    """硬件 linear→sRGB 传输函数（swapchain 是 *_SRGB 格式，store 时自动做这一步）。

    注意：shader **不做** pow(1/2.2)。这里显式写出来，是因为回读拿到的是编码后的
    字节，而闭式解算的是线性值——不补这一步，两者天生差一个传输函数。
    """
    c = min(1.0, max(0.0, c))
    return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055


def to_bytes(linear: tuple[float, float, float], exposure: float = 1.0) -> tuple[int, int, int]:
    """线性 HDR → 屏幕字节（曝光 → ACES → sRGB 编码 → 8bit）。

    这是「该是多少」的最终答案，可直接与 PNG 像素比。
    """
    mapped = aces_tonemap(tuple(c * exposure for c in linear))
    return tuple(round(srgb_encode(c) * 255.0) for c in mapped)


# ═══════════════════════════════════════════════════════════════════════════
# 小向量工具（不引 numpy：本文件要能被无依赖环境直接跑）
# ═══════════════════════════════════════════════════════════════════════════

def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def _norm(a):
    m = math.sqrt(_dot(a, a))
    return (a[0] / m, a[1] / m, a[2] / m) if m > 0.0 else (0.0, 0.0, 0.0)


# ═══════════════════════════════════════════════════════════════════════════
# 自检：先证明这套公式本身没抄错，再拿它去判引擎
# ═══════════════════════════════════════════════════════════════════════════

def _selftest() -> int:
    """公式自检。每条都是「如果这行从未生效，这条会红」的写法。"""
    bad = 0

    def check(name, got, want, tol=1e-9):
        nonlocal bad
        if abs(got - want) > tol:
            print(f"  FAIL {name}: got {got!r} want {want!r}")
            bad += 1

    # ① 矩阵约定自检：列和 = 1 ⟹ 中性白 (1,1,1) 过输入矩阵不变。
    #    这正是 tonemap.frag.slang 注释里那条「2026-09-15 勘误」的守门条件：
    #    旧值抄错版时列和不为 1，白输入会灰化 + 通道串扰方向反了。
    for name, m in (("input", K_ACES_INPUT), ("output", K_ACES_OUTPUT)):
        for j in range(3):
            col = sum(m[i][j] for i in range(3))
            check(f"{name} 列 {j} 之和 = 1", col, 1.0, tol=5e-5)

    # ② 退化点手算对账：N=V=L=(0,0,1)、metal=0、rough=0.9、albedo=(0,0,1)、光=(1,1,1)
    #    a=0.81, a2=0.6561, d=a2, D=1/(π·a2+1e-7)=0.48513…, F=0.04, G=1
    #    spec = D·0.04/(4+1e-4) = 0.0048512…，diff_B = 1/π = 0.3183098…
    n = v = l = (0.0, 0.0, 1.0)
    got = shade_direct(n, v, l, (0.0, 0.0, 1.0), 0.9, 0.0, (1.0, 1.0, 1.0))
    a = 0.9 * 0.9
    a2 = a * a
    d_expect = 1.0 / (PI * a2 + 1e-7)
    check("D(退化)", d_expect, 0.485127, tol=1e-4)
    spec_expect = d_expect * 0.04 / (4.0 + 1e-4)
    check("R = 纯 specular（metal=0 时漫反在 R 上为 0）", got[0], spec_expect, tol=1e-12)
    check("B = 1/π + specular", got[2], 1.0 / PI + spec_expect, tol=1e-12)
    # **反向锚点**：R 必须严格 > 0（= specular 项真的在链里）。若有人把 spec 漏乘，
    # 或 albedo 通道串了，这里第一个红。
    check("specular 项非零（阳性锚点）", 1 if got[0] > 0 else 0, 1)
    check("R ≠ B（纯蓝面 specular 不该带颜色）", 1 if abs(got[0] - got[2]) > 1e-3 else 0, 1)

    # ③ 背光的反面：N 与 L 相反 ⟹ NdotL 饱和到 0 ⟹ 出射恰为 0
    dark = shade_direct((0.0, 0.0, 1.0), (0.0, 0.0, 1.0), (0.0, 0.0, -1.0),
        (1.0, 1.0, 1.0), 0.9, 0.0, (12.0, 12.0, 12.0))
    check("背光面恰为 0（不是接近 0）", sum(abs(c) for c in dark), 0.0)

    # ④ sRGB 传输函数的两个已知点 + 连续性（12.92 的线性段与 pow 段在拐点相等）
    check("sRGB(0) = 0", srgb_encode(0.0), 0.0)
    check("sRGB(1) = 1", srgb_encode(1.0), 1.0, tol=1e-12)
    lo, hi = 12.92 * 0.0031308, 1.055 * (0.0031308 ** (1 / 2.4)) - 0.055
    check("sRGB 在拐点连续", lo, hi, tol=1e-6)
    # **反向锚点**：中灰线性 0.5 编码后必须明显变亮（≈0.735）。若有人漏了传输函数，
    # 这里会拿到 0.5。
    check("sRGB(0.5) ≈ 0.735", srgb_encode(0.5), 0.73536, tol=1e-4)

    # ⑤ ACES 的三条真实性质（写成断言而不是一句「压缩」——我第一版把这条写成
    #    「输出增幅 < 10×」，实测直接错：filmic 曲线在暗端 out/in 增益从 0.105 起
    #    **先升后降**（0.01→0.5 区间增益 0.105→0.749），所以「增幅小于输入倍数」
    #    根本不成立。下面三条才是这条曲线真正的形状特征。）
    lo_v = aces_tonemap((0.05, 0.05, 0.05))[0]
    hi_v = aces_tonemap((0.5, 0.5, 0.5))[0]
    check("ACES 单调递增", 1 if hi_v > lo_v else 0, 1)
    # 白点被压：输入 1.0 映到 ≈0.619，不是 1.0（这就是 "filmic" 的 roll-off）
    check("白点 roll-off（out(1) ≈ 0.619 < 1）", aces_tonemap((1.0, 1.0, 1.0))[0], 0.619115,
        tol=1e-5)
    # 高光压缩：高输入端的增益（out/in）必须低于中间调，否则亮部会糊成一片
    gain_mid = aces_tonemap((1.0, 1.0, 1.0))[0] / 1.0
    gain_hi = aces_tonemap((4.0, 4.0, 4.0))[0] / 4.0
    check("高光端增益低于中间调（亮部压缩）", 1 if gain_hi < gain_mid else 0, 1)
    check("ACES 上限 saturate 到 1", aces_tonemap((1e3, 1e3, 1e3))[0], 1.0, tol=1e-12)

    # ⑥ 读图器自检（**判据自己也要有判据**）：内存里造一张 4×3 的已知图，
    #    逐行用 filter 0/1/2/3/4 编一遍，再读回来逐像素比对。不测这一段，
    #    一旦编码器写出非 0 filter，读图器会静默读到错像素，而「闭式解不符」
    #    会被一路归因到着色链上——那是本项目最贵的一种假线索。
    bad += _selftest_png_reader()

    print(f"{'自检失败' if bad else '自检通过'}：{bad} 条失败")
    return 1 if bad else 0


def _selftest_png_reader() -> int:
    """PNG 读路径往返自检：写（5 种 filter 各一行）→ 读 → 逐像素比对。"""
    import struct as _s
    import zlib as _z

    w, h, ch = 4, 5, 3
    px = [[(i * 17 + j * 61 + k * 3) & 0xFF for k in range(ch)] for j in range(h) for i in range(w)]
    px = [[tuple(px[j * w + i]) for i in range(w)] for j in range(h)]

    raw = bytearray()
    prev = bytes(ch * w)
    for j in range(h):
        line = bytes(v for p in px[j] for v in p)
        f = j % 5                      # 每行用不同 filter，把 5 条逆变换全踩一遍
        if f == 0:
            enc = line
        elif f == 1:
            enc = bytes((line[i] - (line[i - ch] if i >= ch else 0)) & 0xFF for i in range(len(line)))
        elif f == 2:
            enc = bytes((line[i] - prev[i]) & 0xFF for i in range(len(line)))
        elif f == 3:
            enc = bytes((line[i] - (((line[i - ch] if i >= ch else 0) + prev[i]) >> 1)) & 0xFF
                for i in range(len(line)))
        else:
            def paeth(i):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            enc = bytes((line[i] - paeth(i)) & 0xFF for i in range(len(line)))
        raw.append(f)
        raw += enc
        prev = line

    def chunk(typ, body):
        c = typ + body
        return _s.pack(">I", len(body)) + c + _s.pack(">I", _z.crc32(c) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", _s.pack(">II", w, h) + bytes([8, 2, 0, 0, 0]))
        + chunk(b"IDAT", _z.compress(bytes(raw)))
        + chunk(b"IEND", b""))

    import os
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "rgl_analytic_ref_selftest.png")
    with open(path, "wb") as fh:
        fh.write(png)
    bad = 0
    for j in range(h):
        for i in range(w):
            got = _read_png_pixel(path, i, j)
            if got != px[j][i]:
                print(f"  FAIL PNG 读回 ({i},{j}) filter={j % 5}: got {got} want {px[j][i]}")
                bad += 1
    os.unlink(path)
    return bad


# ═══════════════════════════════════════════════════════════════════════════
# 探针：算出预测值 + 与实际截图像素对比
# ═══════════════════════════════════════════════════════════════════════════

def _read_png_pixel(path: str, x: int, y: int) -> tuple[int, int, int]:
    """读 PNG 的一个像素（stdlib-only：zlib + struct 手解，不引 Pillow）。

    支持 8bit RGB/RGBA、非隔行；够用即可（llm_debug 产的就是这种）。
    """
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} 不是 PNG")
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            # IHDR = width(4) height(4) depth(1) colorType(1) compression(1) filter(1)
            # interlace(1) 共 13 字节。字段逐个数取，不用一个大 format（>IIBBB 只有 11
            # 字节，切片给 13 会直接 struct.error——踩过）。
            w = struct.unpack(">I", body[0:4])[0]
            depth, color, interlace = body[8], body[9], body[12]
            if depth != 8 or interlace != 0:
                raise ValueError(f"只支持 8bit 非隔行 PNG，实得 depth={depth} interlace={interlace}")
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib_decompress(idat)
    stride = w * channels + 1  # 每行前 1 字节 filter type
    if y * stride + 1 + x * channels + channels > len(raw):
        raise ValueError(f"像素 ({x},{y}) 越界或 PNG 截断")
    # 逐行解 filter（llm_debug 写的通常是 filter 0，但必须真解，否则 filter≠0 时读错）
    prev = bytearray(w * channels)
    row = bytearray(w * channels)
    for yy in range(y + 1):
        base = yy * stride
        ftype = raw[base]
        line = bytearray(raw[base + 1:base + 1 + w * channels])
        _unfilter(ftype, line, prev, channels)
        prev, row = line, line
    return (row[x * channels], row[x * channels + 1], row[x * channels + 2])


def zlib_decompress(idat: bytes) -> bytes:
    import zlib
    return zlib.decompress(idat)


def _unfilter(ftype: int, line: bytearray, prev: bytearray, ch: int) -> None:
    """PNG 5 种行内 filter 的逆变换（不实现 = filter≠0 时静默读到错像素）。"""
    if ftype == 0:
        return
    n = len(line)
    if ftype == 1:
        for i in range(n):
            line[i] = (line[i] + (line[i - ch] if i >= ch else 0)) & 0xFF
    elif ftype == 2:
        for i in range(n):
            line[i] = (line[i] + prev[i]) & 0xFF
    elif ftype == 3:
        for i in range(n):
            left = line[i - ch] if i >= ch else 0
            line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
    elif ftype == 4:
        for i in range(n):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = (line[i] + pred) & 0xFF
    else:
        raise ValueError(f"未知 PNG filter type {ftype}")


def probe(
    png: str, x: int, y: int,
    albedo=(0.0, 0.0, 1.0), roughness=0.9, metallic=0.0,
    light_color=(12.0, 12.0, 12.0), ambient=(0.0, 0.0, 0.0),
    exposure=1.0, n=(0.0, 0.0, 1.0), v=(0.0, 0.0, 1.0), l=(0.0, 0.0, 1.0),
    tol=2,
) -> int:
    """把「该是多少」与「实际是多少」摆在一起，并解释差多少。

    @param tol 允许的最大单通道字节差（默认 2：留 ACES 多项式 float32 vs float64
               与 8bit 量化取整的余量；再大就该当失败看待，不能靠放宽阈值混过去）
    @return 0 = 通过
    """
    linear = shade_surface(n, v, l, albedo, roughness, metallic, light_color,
        ambient=ambient, exposure=exposure, shadow=1.0)
    want = to_bytes(linear, exposure)
    got = _read_png_pixel(png, x, y)
    print(f"探针 ({x},{y})：")
    print(f"  线性预测 = ({linear[0]:.6f}, {linear[1]:.6f}, {linear[2]:.6f})")
    print(f"  预测字节 = {want}")
    print(f"  实际字节 = {got}")
    diff = tuple(abs(int(a) - int(b)) for a, b in zip(got, want))
    print(f"  差       = {diff}   （阈值 {tol}）")
    ok = all(d <= tol for d in diff)
    print("  => " + ("PASS" if ok else "FAIL"))
    if not ok:
        print("  归因顺序（先怀疑前提，再怀疑引擎）：")
        print("    ① 探针面是否真的无纹理 / 无 normal map（模块头适用范围）")
        print("    ② N/V/L 是否真如假设（机位与光向是否被相机模式改写过）")
        print("    ③ normalFiltering 是否抬了 roughness（平面正视图应为 0 方差）")
        print("    ④ 六个配置门是否都真的关上了（少关一项就少一个已知量）")
        print("    ⑤ 以上都排除了，才轮到怀疑着色链")
    return 0 if ok else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] == "--selftest":
        return _selftest()
    if argv[0] != "--probe":
        print(__doc__)
        return 0

    import argparse
    ap = argparse.ArgumentParser(
        prog="analytic_ref.py --probe", description="闭式解探针：预测值 vs 实机像素")
    ap.add_argument("png")
    ap.add_argument("x", type=int)
    ap.add_argument("y", type=int)
    ap.add_argument("--albedo", type=float, nargs=3, metavar=("R", "G", "B"),
        help="基色（线性）；缺省 = cornell_box back_wall 的纯蓝")
    ap.add_argument("--roughness", type=float, default=0.9)
    ap.add_argument("--metallic", type=float, default=0.0)
    ap.add_argument("--intensity", type=float, default=12.0, help="光强（scene.lua light.intensity）")
    ap.add_argument("--light", type=float, nargs=3, metavar=("X", "Y", "Z"),
        help="**指向光源**的单位向量 L。注意：scene.lua 里的 direction 是**照射方向**，"
             "传之前要先取反。缺省 = (0,0,1)（垂直正入射的退化情形）")
    ap.add_argument("--ambient", type=float, nargs=3, default=(0.0, 0.0, 0.0))
    ap.add_argument("--exposure", type=float, default=1.0)
    ap.add_argument("--tol", type=int, default=2, help="单通道允许的最大字节差")
    a = ap.parse_args(argv[1:])

    l = _norm(tuple(a.light)) if a.light else (0.0, 0.0, 1.0)
    return probe(a.png, a.x, a.y,
        albedo=tuple(a.albedo) if a.albedo else (0.0, 0.0, 1.0),
        roughness=a.roughness, metallic=a.metallic,
        light_color=tuple(c * a.intensity for c in (1.0, 1.0, 1.0)),
        ambient=tuple(a.ambient), exposure=a.exposure, n=(0.0, 0.0, 1.0),
        v=(0.0, 0.0, 1.0), l=l, tol=a.tol)


if __name__ == "__main__":
    raise SystemExit(main())
