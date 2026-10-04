#!/usr/bin/env python3
"""程序化 Radiance .hdr 环境图生成器（**stdlib-only**）：纯黑背景 + 一条极窄极亮的水平带。

## 为什么需要它（不是"素材导出工具"）

变体 1「带边」要复现的机制 = **低 roughness 金属面把高对比 env 的极亮窄带切成走样**。
用真实 HDR（san_giuseppe_bridge_4k.hdr）做这个实验有两个致命不便：① 亮区的仰角/宽度/亮度
全是"素材里恰好有什么"，改不动也说不清；② 素材是 gitignore 的本地图，基准不可复现。
所以 env 必须**程序化生成**：带子的仰角、角宽、亮度三个数就是实验的自变量，写进文件名。

## 图像布局 = 等距柱状（latlong / equirectangular），不是 cube cross

**出处（不是猜的）**：`rgl/texture_pool.cppm::loadEnvCubeMapHdr` 头注释「Radiance .hdr
（等距柱状 float RGB，decodeHdr 产物）→ CPU 重投影 6 面 RGBA16F」，且「源非等距柱状
（W ≠ 2H）也按 u/v ∈ [0,1] 全图映射处理」。映射式逐式取自同文件 `sampleEquirect`：

    lon = atan2(x, −z)            u = (lon + π) / 2π      ← 图像中心列朝 −Z（GL 惯例）
    v = 0.5 − asin(y) / π         ← v=0 是天顶、v=0.5 是地平线、v=1 是天底

行序 top-down：`image_codec.cppm` 头注释「FreeImage 的 HDR 插件把文件第 y 行写入
scanline height−1−y，读回时反序还原」⟹ **DecodedImageF 第 0 行 = 文件第 0 行 = 天顶**。
Radiance 的 `-Y h +X w` 维度行正是"第一行在图像顶部"，两者一致。

**推论（本生成器的核心几何）**：一条"水平带" = 一段**恒定仰角**的纬度区间，落在若干**整行**上。
仰角 θ（度）→ 行号 y = (0.5 − θ/180) · H。θ>0 = 地平线以上。

## 编码 = Radiance RGBE（32-bit rle rgbe），刻意写**非压缩**扫描线

每像素 4 字节 r,g,b,e，值 = byte · 2^(e−136)（FreeImage 口径，见 `image_codec_impl.cppm`
的 FreeImage FIF_HDR 路径）。指数 e 取 `frexp` 的指数 ⟹ 最大通道落在 [128,256)，
**相对量化步长 = 1/256 ≈ 0.4%**（对 4000 的带子 = ±16，远小于实验要分辨的 9 倍亮度比）。
非压缩 = 每行直接写 4W 字节，**不写** `\\x02\\x02<w16><h16>` 的 new-RLE 行头；FreeImage/stb
都按"首字节不是 1/2 就是 flat 扫描线"分支，故合法。选非压缩的理由：少 30 行编解码器，
而 2 MB 的 env 文件没人关心体积。

## 确定性 + 自检（对齐 AGENTS.md「写测试的硬纪律」）

纯解析式，无随机、无浮点版本差异（只有 `math.sin/cos/frexp` 与整数化）。
`--selftest` 是**阳性锚点**：把刚写的字节**重新解析**回来（自带 ~15 行 RGBE 解码器），
断言 ① 带内像素 ≈ 请求亮度（容差 = RGBE 量化 0.8%）② 带外**逐字节全零**
③ 维度行与 W=2H 成立 ④ 亮度单调（抬 --intensity 必须抬读数）。
不自检的生成器等于没测——本文件曾把 v 的符号写反就会得到"带子在地平线下 6°"，
而渲染端只会表现为"什么都没有"，无从归因。
"""

import argparse
import math
import struct
import sys
import tempfile
from pathlib import Path

# ---------------------------------------------------------------------------
# RGBE 编解码（各 ~15 行；解码器只为自检存在，不参与正常产出）
# ---------------------------------------------------------------------------

def rgbe_encode(r: float, g: float, b: float) -> bytes:
    """线性 float RGB → 4 字节 RGBE（Radiance `LdRgbe` 标准：最大通道量化到 [128,256)）。

    全零必须编码成 4 个 0 字节（e=0 时解码端约定值为 0，而不是 0·2^-136 的"非零小数"）。
    """
    m = max(r, g, b)
    if m <= 0.0:
        return b"\x00\x00\x00\x00"
    _, e = math.frexp(m)            # m = mant · 2^e，mant ∈ [0.5, 1)
    scale = 256.0 / (2.0 ** e)      # 最大通道 → 256·mant ∈ [128, 256)
    to8 = lambda c: min(255, int(c * scale))
    return bytes((to8(r), to8(g), to8(b), e + 128))


def rgbe_decode(px: bytes) -> tuple:
    """4 字节 RGBE → 线性 float RGB（FreeImage 口径 value = byte/255 · 2^(e−128)）。"""
    r, g, b, e = px
    if e == 0:
        return (0.0, 0.0, 0.0)
    f = (2.0 ** (e - 128)) / 255.0
    return (r * f, g * f, b * f)


# ---------------------------------------------------------------------------
# 等距柱状几何（逐式对齐 rgl/texture_pool.cppm::sampleEquirect）
# ---------------------------------------------------------------------------

def row_of_elevation(elev_deg: float, height: int) -> float:
    """仰角（度，>0 = 地平线以上）→ 等距柱状行号（浮点，像素中心 = y + 0.5）。

    v = 0.5 − asin(y)/π 且 y = sin(θ) ⟹ asin(sin θ) = θ（θ ∈ [−90°,90°]）⟹ v = 0.5 − θ/π。
    行号 = v · H（`sampleEquirect` 的 cy = v·H − 0.5 是"像素中心减 0.5"，故行边界 = v·H）。
    """
    return (0.5 - math.radians(elev_deg) / math.pi) * height


def band_rows(elev_deg: float, width_deg: float, height: int) -> tuple:
    """带子的 [首行, 末行) 整行区间（带子中心在 elev_deg，角宽 width_deg）。

    刻意**按整行**取：亚像素的软边交给 `sampleEquirect` 的双线性去做，生成器不自己抗锯齿
    ——本实验要的恰恰是"硬边窄带"，软边是引擎那一侧的采样行为，不该在这里预先做掉。
    """
    y_hi = row_of_elevation(elev_deg + width_deg / 2.0, height)  # 仰角更大 = 行号更小
    y_lo = row_of_elevation(elev_deg - width_deg / 2.0, height)
    return max(0, int(math.floor(y_hi))), min(height, int(math.ceil(y_lo)))


def all_band_rows(elev: float, band_deg: float, height: int, count: int,
                  spacing: float) -> list:
    """N 条等间隔带的整行区间列表（`count=1` 时 = `[band_rows(...)]`，**字节与单带逐字相同**）。

    为什么要多条带：单带剖面只能靠"带宽"这一个量判 roughness 轴活不活，而带宽要在带边
    找半高、对掩膜与饱和都敏感。N 条带给出**起伏深度**（剖面 90/10 百分位之比）——
    低 roughness 分辨得出单条带 ⟹ 起伏深；高 roughness 平均成一摊 ⟹ 起伏平。
    它是比值型判据，不吃"整体多亮"，所以不吃 ACES 的 shoulder。
    """
    rows = [band_rows(elev + i * spacing, band_deg, height) for i in range(count)]
    for i, (y0, y1) in enumerate(rows):
        if y1 <= y0:
            raise SystemExit(f"第 {i} 条带 [{y0},{y1}) 为空：仰角 {elev + i * spacing}° "
                             f"宽 {band_deg}° 在 H={height} 下不足一个 texel"
                             f"（角分辨率 = {180.0 / height:.4f}°/行）")
    return rows


def generate_uniform(width: int, height: int, value: float) -> bytes:
    """生成 W×H **全方向等值**的 RGBE 字节流（每个 texel 都是 (v,v,v)）。

    ## 为什么需要它：GGX 预滤波 mip 链的**不动点判据**（B1，2026-10-05）

    预滤波是**归一化**加权平均（`env_prefilter.comp.slang:106-120`：`sum += L·NoL`、
    `weight += NoL`、`color = sum/weight`）⟹ 对**常值环境**，任意 roughness 的输出必须
    **恒等于该常数**。这条判据的强度在于它**不需要知道 GGX 长什么样**：常值输入的不动点
    是纯数学事实（凸组合的权重和为 1），任何"漏除采样数 / PDF 归一错 / 1−cosθ 处理错"
    都会立刻表现为"某几层 mip 偏离常数"。

    等距柱状下"全方向等值"= 整幅图同一个值（`sampleEquirect` 的经纬映射是满射，
    常值函数在任意重投影/双线性/卷积下仍是常值）⟹ 机位与反射方向**都不敏感**，
    这正是本判据比窄带靶便宜的地方。
    """
    px = rgbe_encode(value, value, value)
    if value <= 0.0:
        return b"\x00\x00\x00\x00" * (width * height)
    return px * (width * height)


def generate(width: int, height: int, elev: float, band_deg: float,
             intensity: float, count: int = 1, spacing: float = 0.0) -> bytes:
    """生成 W×H 等距柱状 RGBE 字节流：全黑 + 仰角 elev 起、角间隔 spacing 的 count 条白热水平带。"""
    bands = all_band_rows(elev, band_deg, height, count, spacing)
    on = rgbe_encode(intensity, intensity, intensity)
    off = b"\x00\x00\x00\x00"
    row_on = on * width
    row_off = off * width
    out = bytearray()
    for y in range(height):
        hit = any(a <= y < b for a, b in bands)
        out += row_on if hit else row_off
    return bytes(out)


def header(width: int, height: int) -> bytes:
    """Radiance 头：格式行 + 空行 + 维度行（`-Y` = 第一行在图像顶部）。"""
    return (f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n").encode("ascii")


def write_hdr(path: Path, width: int, height: int, elev: float, band_deg: float,
              intensity: float, count: int = 1, spacing: float = 0.0,
              uniform: bool = False) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    if uniform:
        data = header(width, height) + generate_uniform(width, height, intensity)
    else:
        data = header(width, height) + generate(width, height, elev, band_deg, intensity,
                                                count, spacing)
    path.write_bytes(data)
    if uniform:
        return {"path": str(path), "bytes": len(data), "width": width, "height": height,
                "uniform": True, "intensity": intensity,
                "lit_rows": f"全部 {height} 行", "deg_per_row": 180.0 / height}
    bands = all_band_rows(elev, band_deg, height, count, spacing)
    y0, y1 = bands[0]
    return {"path": str(path), "bytes": len(data), "width": width, "height": height,
            "elev_deg": elev, "band_deg": band_deg, "intensity": intensity,
            "count": count, "spacing_deg": spacing,
            "band_rows": [y0, y1], "band_texels": y1 - y0,
            "all_band_rows": [list(b) for b in bands],
            "deg_per_row": 180.0 / height}


# ---------------------------------------------------------------------------
# 自检（阳性锚点：重新解析自己刚写的字节）
# ---------------------------------------------------------------------------

def parse_hdr(data: bytes) -> tuple:
    """把 .hdr 字节拆成 (w, h, 扫描线字节)。只认本文件写的非压缩形态。

    Radiance 布局 = [格式/注释行…][空行][维度行][扫描线]——维度行在**空行之后**，
    不是头部块的最后一行（写反了会把 FORMAT 行当维度行，自检立刻抓到）。
    """
    head, sep, rest = data.partition(b"\n\n")
    if not sep:
        raise AssertionError("缺头部与扫描线之间的空行")
    lines = head.decode("ascii").splitlines()
    if not any("=" in ln for ln in lines):
        raise AssertionError("缺 FORMAT= 格式行")
    dimline, _, body = rest.partition(b"\n")
    dim = dimline.decode("ascii").split()
    if len(dim) != 4 or dim[0] != "-Y" or dim[2] != "+X":
        raise AssertionError(f"维度行不是 '-Y h +X w'：{dimline!r}")
    h, w = int(dim[1]), int(dim[3])
    if len(body) != w * h * 4:
        raise AssertionError(f"扫描线长度 {len(body)} ≠ {w}·{h}·4")
    return w, h, body


def selftest_canvas(band_deg: float) -> tuple:
    """自检画布尺寸 (W, H)：H 要尽量小（纯 Python 逐像素循环），又要让**请求的带宽
    ≥ 1 texel**——否则 `all_band_rows` 直接抛「不足一个 texel」，自检连被测对象都没有。

    旧实现写死 64×32（5.625°/行），于是 `--band 0.6` 的自检必炸；而「带子落在请求仰角
    上」这条断言，恰恰只在带子真能落到整行上时才有意义。上限 512 行 = 0.35°/行，
    再窄的带就交给生产尺寸（≥1024×512）生成，自检不假装能验。
    """
    h = 32
    while h < 512 and 180.0 / h > max(band_deg, 1e-6):
        h *= 2
    return 2 * h, h


def selftest(args) -> int:
    """阳性锚点自检入口。**写盘全程在临时目录**，`--out` 一个字节都不碰。

    旧实现直接覆写 `--out`（帮助文本里也这么写），跑完 `--out` 留下的是自检画布那张
    小图；而生产 env 常常就指着同一个路径 —— 2026-10-05 实测踩过：1024×512 的 0.6°
    窄带 env 被 `--selftest` 换成 64×32 的 11.25° 宽带，角分辨率差 16 倍，整轮受控
    实验数字作废（且画面看起来"完全合理"，只是亮区宽到不随 roughness 变）。
    """
    with tempfile.TemporaryDirectory(prefix="env_band_selftest_") as td:
        return _selftest(args, Path(td) / (Path(args.out).name or "band.hdr"))


def _selftest(args, out: Path) -> int:
    errs = []
    w, h = selftest_canvas(args.band)
    info = write_hdr(out, w, h, args.elev, args.band, args.intensity)
    data = out.read_bytes()
    pw, ph, body = parse_hdr(data)
    if (pw, ph) != (w, h):
        errs.append(f"维度回读 {(pw, ph)} ≠ {(w, h)}")
    if pw != 2 * ph:
        errs.append(f"W={pw} 非 2H（引擎按 u/v∈[0,1] 全图映射，非等距柱状会横向拉伸）")

    # ① 带内亮度 + ② 带外全零
    y0, y1 = info["band_rows"]
    tol = 0.008  # RGBE 相对量化 1/256，两侧各留一点
    bad_in = bad_out = 0
    for y in range(ph):
        for x in range(pw):
            v = rgbe_decode(body[(y * pw + x) * 4:(y * pw + x) * 4 + 4])
            if y0 <= y < y1:
                if abs(v[0] - args.intensity) > args.intensity * tol or v[0] <= 0:
                    bad_in += 1
            elif v != (0.0, 0.0, 0.0) or body[(y * pw + x) * 4:(y * pw + x) * 4 + 4] != b"\0\0\0\0":
                bad_out += 1
    if bad_in:
        errs.append(f"带内 {bad_in} 个像素亮度偏离 > {tol * 100:.1f}%")
    if bad_out:
        errs.append(f"带外 {bad_out} 个像素非零（'纯黑背景'不成立）")

    # ③ 带子确实落在请求的仰角上（用行号反解仰角，容差 = 半行）
    got = math.degrees(math.pi * (0.5 - (y0 + y1) / 2.0 / ph))
    if abs(got - args.elev) > 180.0 / ph:
        errs.append(f"带子中心仰角实测 {got:.4f}° ≠ 请求 {args.elev}°")

    # ④ 单调性（反向对照：抬亮度必须抬读数，否则"亮度自变量"是假的）
    hi = write_hdr(out, w, h, args.elev, args.band, args.intensity * 2.0)
    _, _, body2 = parse_hdr(out.read_bytes())
    y0b = hi["band_rows"][0]
    v2 = rgbe_decode(body2[(y0b * pw + 0) * 4:(y0b * pw + 0) * 4 + 4])
    if not (v2[0] > args.intensity * 1.5):
        errs.append(f"亮度翻倍后读数 {v2[0]} 未随之上升（自变量失效）")

    # ⑤ 逐字节可复现（同参数两次写盘必须同哈希）
    import hashlib
    write_hdr(out, w, h, args.elev, args.band, args.intensity)
    a = hashlib.md5(out.read_bytes()).hexdigest()
    write_hdr(out, w, h, args.elev, args.band, args.intensity)
    b = hashlib.md5(out.read_bytes()).hexdigest()
    if a != b:
        errs.append("同参数两次生成字节不同（确定性破了）")

    # ⑥ 多带：count=1 必须与不传 count **逐字节相同**（否则一次加参数就把第一轮所有
    #    单带基准的哈希作废），count=3 必须恰好亮 3 段且段间全黑、中心仰角对位
    #
    #    间隔不能写死 40°：三条带要都落在天顶以下，`--elev 25` 时 25+80 = 105° 会把第三条
    #    顶出天空 → `all_band_rows` 抛「为空」→ 自检在仰角靶参数下必炸（2026-10-05 实测）。
    #    取「40° 与装得进天空的最大间隔」的较小者；下限 2×带宽保证三条带不糊成一段。
    #    掠射靶（--elev 3）下算出来仍是 40°，既有基准字节不变。
    sp = min(40.0, max(2.0 * args.band, (88.0 - args.elev) / 2.0))
    single = write_hdr(out, w, h, args.elev, args.band, args.intensity, 1, 0.0)
    if hashlib.md5(out.read_bytes()).hexdigest() != a:
        errs.append("count=1 的产物与单带旧路径字节不同（既有基准会集体失效）")
    del single
    multi = write_hdr(out, w, h, args.elev, args.band, args.intensity, 3, sp)
    _, _, body3 = parse_hdr(out.read_bytes())
    lit = [y for y in range(ph)
           if rgbe_decode(body3[(y * pw + 0) * 4:(y * pw + 0) * 4 + 4])[0] > 0]
    segs, prev = [], None
    for y in lit:
        if prev is None or y != prev + 1:
            segs.append([y, y])
        else:
            segs[-1][1] = y
        prev = y
    if len(segs) != 3:
        errs.append(f"count=3 只亮出 {len(segs)} 段（须 3 段）：{segs}")
    # 段按 y 升序 = 仰角**降**序（行 0 是天顶），故第 i 段对应第 (n-1-i) 个请求仰角
    n = len(segs) if segs else 1
    for i, (s0, s1) in enumerate(segs):
        got = math.degrees(math.pi * (0.5 - (s0 + s1 + 1) / 2.0 / ph))
        want = args.elev + (n - 1 - i) * sp
        if abs(got - want) > 180.0 / ph:
            errs.append(f"第 {i} 段中心仰角实测 {got:.3f}° ≠ 请求 {want}°")
    del multi

    # ⑦ uniform 模式（B1 不动点判据的生成端）：**逐 texel** 必须等于请求值，且
    #    一个都不能漏（"全方向等值"若有一行没写，重投影后就是某个方向上的暗缝，
    #    而 B1 的判据恰恰依赖"处处相等"）
    uw, uh = 32, 16
    uinfo = write_hdr(out, uw, uh, 0.0, 0.0, args.intensity, uniform=True)
    _, _, ubody = parse_hdr(out.read_bytes())
    bad_u = sum(1 for i in range(uw * uh)
                if abs(rgbe_decode(ubody[i * 4:i * 4 + 4])[0] - args.intensity)
                > args.intensity * tol)
    if bad_u:
        errs.append(f"uniform 模式 {bad_u}/{uw * uh} 个 texel 偏离 > {tol * 100:.1f}%")
    if uinfo.get("uniform") is not True:
        errs.append("uniform 模式回显缺 uniform=True（调用方无法区分两种产物）")
    # 反向对照：uniform=0 必须全零（否则"常值 0"这个对照样本是假的）
    write_hdr(out, uw, uh, 0.0, 0.0, 0.0, uniform=True)
    _, _, zbody = parse_hdr(out.read_bytes())
    if any(zbody[i * 4:i * 4 + 4] != b"\0\0\0\0" for i in range(uw * uh)):
        errs.append("uniform=0 未产出全零字节（常值 0 对照失效）")

    if errs:
        for e in errs:
            print(f"SELFTEST FAIL: {e}", file=sys.stderr)
        return 1
    # 自检全程写的是临时目录里那张（收尾即删），--out 保持原样 —— 生产 env 与自检
    # 画布从此不可能互相覆盖（旧实现在这里覆写 --out，见 selftest() 的踩坑记录）。
    print(f"selftest OK  画布 {info['width']}x{info['height']}（临时目录，--out "
          f"'{args.out}' 未写入）  "
          f"带行 [{y0},{y1}) = {info['band_texels']} texel  "
          f"仰角 {args.elev}°±{180.0 / ph:.3f}°  亮度 {args.intensity}  "
          f"角分辨率 {info['deg_per_row']:.4f}°/行")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="程序化 Radiance .hdr（纯黑 + 极窄极亮水平带，等距柱状，stdlib-only）")
    ap.add_argument("--out", default="build/rgl/env/band.hdr", help="输出 .hdr 路径")
    ap.add_argument("--width", type=int, default=1024, help="等距柱状宽（须 = 2×高）")
    ap.add_argument("--height", type=int, default=512, help="等距柱状高")
    ap.add_argument("--elev", type=float, default=3.0,
                    help="带子中心仰角（度，>0 = 地平线以上）")
    ap.add_argument("--band", type=float, default=0.6,
                    help="带子角宽（度）；0.6° 在 H=512 下 = 2 texel")
    ap.add_argument("--intensity", type=float, default=4000.0,
                    help="带子亮度（线性 radiance，须 >1 才算'超过 1'；引擎重投影端钳到 6e4）")
    ap.add_argument("--count", type=int, default=1,
                    help="带的条数（默认 1 = 与单带产物逐字节相同）；>1 时从 --elev 起等间隔排开")
    ap.add_argument("--spacing", type=float, default=3.0,
                    help="多带角间隔（度）；8 条 × 3° 覆盖仰角 1.5°..22.5°，"
                         "掠射机位下正好铺满画面下半（反射仰角 0°..27°）")
    ap.add_argument("--uniform", action="store_true",
                    help="全方向等值 env（每个 texel 都是 --intensity）——GGX 预滤波 mip 链的"
                         "不动点判据用：归一化卷积对常值环境的输出必须恒等于该常数。"
                         "带参数（--elev/--band/--count/--spacing）在此模式下被忽略")
    ap.add_argument("--selftest", action="store_true",
                    help="跑阳性锚点自检（在临时目录写盘，**不碰 --out**）")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(args)
    if args.width != 2 * args.height:
        raise SystemExit("--width 须等于 2×--height（等距柱状；见文件头 loadEnvCubeMapHdr 出处）")
    info = write_hdr(Path(args.out), args.width, args.height, args.elev, args.band,
                     args.intensity, args.count, args.spacing, args.uniform)
    print(" ".join(f"{k}={v}" for k, v in info.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
