#!/usr/bin/env python3
"""BIT-IDENTICAL 逐位回归工具（`just regress`，stdlib-only）。

**还债 债-09**（`docs/还债总账.md` §1 已结案索引），并顺带把时域线的**位比可用面**实测划清（债-46 因此否决下线）：

- **债-09**：本项目最高频的验收动作「改完确认画面逐位不变」一直靠手工 `git stash`
  双构建 + 手算 `sha256sum`。手工口径有两个结构性缺陷：① 慢且容易漏一半（只跑了改动
  侧）；② **只比哈希**——哈希不同既可能是本批改的，也可能是本机既有的 run-to-run
  漂移（2026-10-09 实测：默认配置连拍 14 次有 2 次出现 1 像素 ≤4 LSB 漂移，改动前后
  两个二进制各占 1 次 ⟹ 与任何改动无关）。本工具把「同构建地板」变成一条常驻读数，
  于是"差异"带**归因**而不只是带颜色。
- **债-46（已否决）**：时域滤波曾被记有三个位级出口（lag-N 同相位逐位相同 / 恒等式门 / 预热无关性）。
  `burst` 把「lag-N 逐位相同」做成了一条读数，且**周期是
  量出来的不是写死的**——TAA 的 Halton 周期 16（`rgl/taa_jitter.cppm`），而 FSR 的位数
  随上屏/渲染比变化（0.75 档实测 14，见 `configs/runtime.lua` fsr 块）。写死一个数就是
  给下一次换档埋雷。
  ⚠ **本工具第一次跑就把出口 ① 判红**：`aa=taa` 交付档同相位仍差 1-2 像素 / 恰 1 LSB
  （同进程 lag-16 与跨进程 `delay_frames` 差 16 的倍数两种问法都是），原"逐位相同"是从
  残差指标推断的。**带时域历史的档不做位比**（用户口径：对时域抗锯齿做 bit-identical 无意义）
  ⟹ 这条只更正账、不立案。工具因此自带**异相位对照**（`_lag_control`）把两种红分开：
  "周期不是 N" 与 "周期对但有 1 LSB 漂移"——周期照样量（它是统计判据的输入），只是不当闸门。

**判据口径（不许讨价还价的部分）**

1. **零容差**：判据 = 逐位相同。没有 PSNR/SSIM，没有「≤2 LSB 算通过」。差异大小只用于
   **归因**，绝不用于放行（红线方法学全文 → skill `rgl-verify`）。
2. **不存 golden**：本工具不在仓库里存任何参考图或参考哈希。`--expect` 只接受**你**从
   文档抄来的哈希前缀（红线 A 全文在 `docs/phase13_plan.md` §1.20），工具不替你记基线
   ——记了就会过期，过期就会被讨价还价。
3. **同会话双构建**：`ab` 的两个二进制在同一次调用里各自 `just build`、同一台机器、
   同一驱动下跑完。跨会话的"上次那张图"不算基线。
4. **文件哈希相等 ⟹ 像素相等**。这是**证明**不是假设：PNG 无损 ⟹ 文件是像素的函数，
   同一张文件不可能解出两幅不同的图。反向不成立（libpng 版本/设置变了会让同一幅图写出
   不同字节），所以**只在文件哈希不同时才解码**去分「像素变了」还是「编码变了」。
   这条懒解码让常见路径零解码开销（1280×720 纯 Python 解一张约 2s，`floor --runs 5`
   会省掉十次）。

**归因三态**（`ab` 的结论；注意第三态**仍然 FAIL**）

| 结论 | 含义 |
|---|---|
| 逐位相同 | 两侧各次运行同一哈希，且跨构建同一哈希 |
| 差异 ⊆ 既有地板 | 跨构建差异不超过同构建 run-to-run 地板 ⟹ 不是本批引入，**但红线零容差仍判 FAIL**，只是别去 git bisect 本批 |
| 差异 ⊄ 地板 | 本批引入。报告形态：几像素 / 最大 LSB / 位置 |

**采集层不另起炉灶**：staging 配置、跑引擎、error 行计数、旗标守卫全部 `import smoke`
复用（债-64 登记的就是"采集实现五份副本"，本工具是第六份的风险由这个 import 消掉）。
只有 RPC 连拍是新增能力——`smoke` 走 `llm_debug`（一进程一张图、自退），量不了连续帧。

用法:
    just regress selftest                     量具自检（任何判定前也会自动先跑一遍）
    just regress floor [--runs 5]             同一构建跑 N 次 = 噪声地板
    just regress ab    [--ref HEAD] [--runs 3]  同会话双构建（工作树 vs ref）+ 归因
    just regress burst [--n 32] [--lag 16]    连拍 N 帧，量最小同相位周期
    配置旗标与 just smoke 同源：--aa --size --frames --clip --render-scale …

退出码: 0 = 逐位相同（且自检过）· 1 = 有差异 / 自检不过 / 前置不满足。
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smoke  # 采集层复用（见模块 docstring 末段）

ROOT = smoke.ROOT
BUILD_RGL = smoke.BUILD_RGL
OUT = BUILD_RGL / ".regress"  # 本工具的产物目录（build/ 已 gitignore）

PNG_SIG = b"\x89PNG\r\n\x1a\n"
# color type → **每像素字节数**（PNG 里灰度+alpha 是 2 字节/像素，别按"1 个灰度通道"
# 记成 1——那会让 stride 少一半，解出来整幅错位。引擎只出 6 = RGBA，其余三种是为
# 自检的往返覆盖而支持的）
_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}
_FILTER_NAME = {0: "None", 1: "Sub", 2: "Up", 3: "Average", 4: "Paeth"}


class RegressError(RuntimeError):
    """可预期的失败（前置不满足 / 构建失败 / 恢复现场失败）。main 统一转退出码 1。"""


# ─────────────────────────────────────────────────────────────────────────────
# 1. PNG 读图（stdlib）
#
# 为什么自己写：AGENTS 工具语言纪律「留在 Python 的工具只准 stdlib-only」，而判据要
# 回答的是「差几个像素、差多少 LSB、差在哪」——只读 IHDR 拿不到像素。
# `tools/qa_scenes.py` 里那个自检解码器**故意**只支持 filter 0，这里不能复用：真实截图
# 的 libpng 自适应过滤根本不出 filter 0（1280×720 实测分布 {1:29, 2:4, 3:228, 4:459}），
# 只支持 0 的读图器对真实输入直接失效。
# ─────────────────────────────────────────────────────────────────────────────


@dataclasses.dataclass(frozen=True)
class Png:
    """解码后的紧凑像素缓冲（行距 = width × channels，无 filter 字节）。"""

    width: int
    height: int
    channels: int
    pixels: bytes
    filter_hist: tuple[tuple[int, int], ...]  # 诊断：各 filter 类型出现的行数

    @property
    def stride(self) -> int:
        return self.width * self.channels


class PngError(RuntimeError):
    """PNG 结构/数据不合法（截断、CRC 不符、不支持的形态）。"""


def _chunks(data: bytes):
    """逐 chunk 产出 (tag, payload)，**逐个验 CRC**。

    CRC 不是洁癖：截图是引擎写盘产物，写一半被杀 = 尾部 IDAT 截断。不验 CRC 的话，
    截断文件往往仍能 zlib 解出前若干行，于是"逐位比对"会拿半张图去比并判"相同"。
    """
    if not data.startswith(PNG_SIG):
        raise PngError("不是 PNG（签名不符）")
    i = 8
    while i + 8 <= len(data):
        ln = int.from_bytes(data[i:i + 4], "big")
        tag = data[i + 4:i + 8]
        if i + 12 + ln > len(data):
            raise PngError(f"chunk {tag[:4]!r} 声明 {ln} 字节但文件只剩 "
                           f"{max(0, len(data) - i - 8)}（写盘未完成 / 文件截断）")
        payload = data[i + 8:i + 8 + ln]
        want = int.from_bytes(data[i + 8 + ln:i + 12 + ln], "big")
        if zlib.crc32(tag + payload) & 0xFFFFFFFF != want:
            raise PngError(f"chunk {tag[:4]!r} CRC 不符（文件损坏）")
        yield tag.decode("latin1"), payload
        i += 12 + ln
        if tag == b"IEND":
            return


def png_decode_bytes(data: bytes) -> Png:
    """解 8bit 非隔行 PNG（color type 0/2/4/6），支持全部 5 种行过滤器。

    形态限制一律 **raise 而不是降级**：静默按 RGBA 解灰度图 = 通道错位 = 差异形态全错，
    那比直接失败坏得多（判据工具给出错误的"差在哪"比不给答案更害人）。
    """
    ihdr: bytes | None = None
    idat = bytearray()
    for tag, payload in _chunks(data):
        if tag == "IHDR":
            if ihdr is not None:
                raise PngError("多个 IHDR")
            ihdr = payload
        elif tag == "IDAT":
            idat += payload
    if ihdr is None:
        raise PngError("缺 IHDR")
    if not idat:
        raise PngError("缺 IDAT")
    w, h, depth, ctype, _comp, filt, interlace = struct.unpack(">IIBBBBB", ihdr[:13])
    if depth != 8:
        raise PngError(f"位深 {depth} 不支持（引擎经 libpng 简单 API 强制 8bit RGBA，"
                       f"见 rgl/image_codec_impl.cppm）")
    if interlace:
        raise PngError("隔行（Adam7）不支持")
    if filt != 0:
        raise PngError(f"filter method {filt} 不支持")
    if ctype not in _CHANNELS:
        raise PngError(f"color type {ctype} 不支持")
    nch = _CHANNELS[ctype]
    stride = w * nch
    raw = zlib.decompress(bytes(idat))
    need = h * (stride + 1)
    if len(raw) < need:
        raise PngError(f"IDAT 解压得 {len(raw)} 字节，按 {w}x{h}×{nch}ch 需要 {need}")

    out = bytearray(h * stride)
    prev = bytearray(stride)
    hist: dict[int, int] = {}
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f not in _FILTER_NAME:
            raise PngError(f"行 {y} filter {f} 非法")
        hist[f] = hist.get(f, 0) + 1
        if f == 0:
            pass
        elif f == 1:  # Sub: + 左
            for x in range(nch, stride):
                line[x] = (line[x] + line[x - nch]) & 0xFF
        elif f == 2:  # Up: + 上
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:  # Average: + (左+上)>>1
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        else:  # Paeth
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return Png(w, h, nch, bytes(out), tuple(sorted(hist.items())))


def png_decode_file(path: Path) -> Png:
    try:
        return png_decode_bytes(path.read_bytes())
    except PngError as exc:
        raise PngError(f"{rel(path)}: {exc}") from None


def png_encode(w: int, h: int, nch: int, pixels: bytes, filt: int) -> bytes:
    """按**指定** filter 编码（只为自检的往返覆盖 5 条分支而存在，不是通用编码器）。

    自检为什么不能只测解码器：解码器与"再编码一遍"若共享同一个错误理解（比如 bpp 取错），
    往返照样通过。所以往返之外另有两条独立判据：① 与 PNG 规范的过滤器定义逐字对照写的
    编码器；② 真实截图的 filter 分布必须覆盖到非平凡分支（见 selftest）。
    """
    stride = w * nch
    rows = bytearray()
    for y in range(h):
        cur = pixels[y * stride:(y + 1) * stride]
        prev = pixels[(y - 1) * stride:y * stride] if y else bytes(stride)
        out = bytearray(cur)
        if filt == 1:
            for x in range(stride - 1, nch - 1, -1):
                out[x] = (cur[x] - cur[x - nch]) & 0xFF
        elif filt == 2:
            for x in range(stride):
                out[x] = (cur[x] - prev[x]) & 0xFF
        elif filt == 3:
            for x in range(stride):
                a = cur[x - nch] if x >= nch else 0
                out[x] = (cur[x] - ((a + prev[x]) >> 1)) & 0xFF
        elif filt == 4:
            for x in range(stride - 1, -1, -1):
                a = cur[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                out[x] = (cur[x] - pred) & 0xFF
        rows += bytes([filt]) + bytes(out)

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ctype = {1: 0, 2: 4, 3: 2, 4: 6}[nch]
    body = struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)
    return (PNG_SIG + chunk(b"IHDR", body) + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


# ─────────────────────────────────────────────────────────────────────────────
# 2. 差异度量（只用于归因，不用于放行）
# ─────────────────────────────────────────────────────────────────────────────


@dataclasses.dataclass(frozen=True)
class Diff:
    pixels: int          # 有差异的像素数（任一通道不同即算）
    max_lsb: int         # 单通道最大绝对差
    bbox: tuple[int, int, int, int] | None
    samples: tuple[tuple[int, int], ...]  # 前几个差异像素坐标

    def __str__(self) -> str:
        if self.pixels == 0:
            return "0 像素差异"
        box = (f" @ bbox {self.bbox[0]},{self.bbox[1]}–{self.bbox[2]},{self.bbox[3]}"
               if self.bbox else "")
        where = "、".join(f"({x},{y})" for x, y in self.samples[:4])
        return (f"{self.pixels} 像素差异 / 最大 {self.max_lsb} LSB{box}"
                + (f"，样本 {where}" if where else ""))


def diff_pixels(a: Png, b: Png) -> Diff:
    """逐像素逐通道比对。形态不同直接算"整幅不同"（尺寸/通道变了不是"差异小"）。"""
    if (a.width, a.height, a.channels) != (b.width, b.height, b.channels):
        return Diff(-1, -1, None, ())
    stride = a.stride
    count = 0
    maxd = 0
    minx = miny = 1 << 30
    maxx = maxy = -1
    samples: list[tuple[int, int]] = []
    nch = a.channels
    for y in range(a.height):
        ra = a.pixels[y * stride:(y + 1) * stride]
        rb = b.pixels[y * stride:(y + 1) * stride]
        if ra == rb:  # 行级早退：真实差异通常只落在少数几行
            continue
        for x in range(a.width):
            i = x * nch
            ca, cb = ra[i:i + nch], rb[i:i + nch]
            if ca == cb:
                continue
            count += 1
            maxd = max(maxd, max(abs(p - q) for p, q in zip(ca, cb)))
            minx, miny = min(minx, x), min(miny, y)
            maxx, maxy = max(maxx, x), max(maxy, y)
            if len(samples) < 8:
                samples.append((x, y))
    return Diff(count, maxd, None if count == 0 else (minx, miny, maxx, maxy), tuple(samples))


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def pixel_sha256(img: Png) -> str:
    """像素内容哈希（与文件哈希**不是一回事**）。

    两个都要报：文档里的红线（`fdece9d0…`）是**文件** sha256，而真正判"画面变没变"的
    是像素。两者不一致只有一种解释——PNG 编码本身变了（libpng 版本 / chunk 集合），
    画面没变。这个区分本身就是本工具要给的归因之一。
    """
    h = hashlib.sha256()
    h.update(f"{img.width}x{img.height}x{img.channels}".encode())
    h.update(img.pixels)
    return h.hexdigest()


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


# ─────────────────────────────────────────────────────────────────────────────
# 3. 量具自检（判据工具自己要有自检 —— skill rgl-test-discipline 同一条纪律）
# ─────────────────────────────────────────────────────────────────────────────


def _lcg(seed: int):
    """确定性伪随机（写死种子；不用 random 全局态，也不用墙钟）。"""
    while True:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        yield seed >> 8


def selftest(quiet: bool = False) -> None:
    """跑不完就不许跑判据：读图器错一个字节，后面所有"逐位相同"都是假的。"""
    checks: list[str] = []

    def ok(cond: bool, what: str) -> None:
        if not cond:
            raise RegressError(f"自检失败：{what}")
        checks.append(what)

    # ① 编解码往返：5 种 filter × 4 种通道数，含全 0 / 全 255 边界行
    for nch in (1, 2, 3, 4):
        rnd = _lcg(0xC0FFEE + nch)
        w, h = 37, 11  # 故意取非整除宽度，逼出 stride/bpp 的 off-by-one
        px = bytes(next(rnd) & 0xFF for _ in range(w * h * nch))
        for filt in range(5):
            back = png_decode_bytes(png_encode(w, h, nch, px, filt))
            ok(back.width == w and back.height == h and back.channels == nch,
               f"往返 {nch}ch/{_FILTER_NAME[filt]} 尺寸")
            ok(back.pixels == px, f"往返 {nch}ch/{_FILTER_NAME[filt]} 像素")
    # ② 常量图（Paeth 预测器在 a==b==c 时的退化分支）
    w, h, nch = 16, 6, 4
    flat = bytes([128]) * (w * h * nch)
    for filt in range(5):
        ok(png_decode_bytes(png_encode(w, h, nch, flat, filt)).pixels == flat,
           f"常量图往返 {_FILTER_NAME[filt]}")
    # ③ 反向用例：改一个字节 → 差异必须恰好是 1 像素且位置正确
    src = bytes(next(_lcg(7)) & 0xFF for _ in range(w * h * nch))
    bad = bytearray(src)
    tx, ty = 5, 3
    off = (ty * w + tx) * nch
    bad[off] ^= 0xFF
    d = diff_pixels(png_decode_bytes(png_encode(w, h, nch, src, 4)),
                    png_decode_bytes(png_encode(w, h, nch, bytes(bad), 4)))
    ok(d.pixels == 1 and d.bbox == (tx, ty, tx, ty) and d.max_lsb > 0,
       f"单像素反向用例（实得 {d}）")
    ok(diff_pixels(png_decode_bytes(png_encode(w, h, nch, src, 3)),
                   png_decode_bytes(png_encode(w, h, nch, src, 3))).pixels == 0,
       "相同图差异为 0")
    # ④ 损坏必须出声：截断与 CRC 篡改都不许被当成"一张正常的图"
    whole = png_encode(w, h, nch, src, 4)
    for name, broken in (("截断", whole[: len(whole) // 2]),
                         ("CRC 篡改", whole[:-14] + bytes([whole[-14] ^ 0xFF]) + whole[-13:])):
        try:
            png_decode_bytes(broken)
        except PngError:
            checks.append(f"{name}的 PNG 被拒绝")
        else:
            raise RegressError(f"自检失败：{name}的 PNG 竟然解出来了")
    # ⑤ 真实截图（有就跑）：非平凡 filter 分支必须在真实输入上被覆盖过
    real = sorted((BUILD_RGL / "screenshots").glob("*.png"), key=lambda p: p.stat().st_mtime,
                  reverse=True) if (BUILD_RGL / "screenshots").is_dir() else []
    if real:
        img = png_decode_file(real[0])
        kinds = {f for f, _ in img.filter_hist}
        ok(img.width > 0 and len(img.pixels) == img.stride * img.height,
           f"真实截图可解（{real[0].name} {img.width}x{img.height}）")
        ok(len(kinds - {0}) >= 2,
           f"真实截图 filter 分布覆盖非平凡分支（实得 {img.filter_hist}）")
        again = png_decode_bytes(png_encode(img.width, img.height, img.channels, img.pixels, 0))
        ok(again.pixels == img.pixels, "真实截图重编码往返逐位相同")
        checks.append(f"真实截图 filter 分布 {dict(img.filter_hist)}")
    else:
        checks.append("真实截图：无（跳过；跑过 just shot 后会自动纳入自检）")

    # ⑥ 采集层的旗标守卫也在自检范围内：`--set` 若静默改坏配置，两侧量的都不是想要的档，
    # 而 ab/floor 全程绿（0 像素差照样判"逐位相同"）。smoke.selftest 的注释记着那次实测。
    # 包成 RegressError：量具自检的红要走本工具的报错口径，不要甩裸 traceback。
    try:
        checks.extend(smoke.selftest(quiet=True))
    except RuntimeError as err:
        raise RegressError(str(err)) from err

    if not quiet:
        print(f"[regress] 自检 {len(checks)} 项通过")
        for c in checks:
            print(f"          · {c}")


# ─────────────────────────────────────────────────────────────────────────────
# 4. 采集（复用 smoke 的 staging / 跑引擎 / error 计数）
# ─────────────────────────────────────────────────────────────────────────────


@dataclasses.dataclass
class Cfg:
    """一次比对两侧**必须完全相同**的东西（配置不同 = 比的不是同一件事）。"""

    aa: str | None = None
    size: tuple[int, int] | None = None
    frames: int = 8
    render_scale: float | None = None
    clip: int | None = None
    fsr_temporal: bool = False
    fsr_jitter: bool = False
    timeout: int = 120
    # 与 just smoke --set 同源的通用配置注入（两侧必须同源：配置不同 = 比的不是同一件事）。
    # 为什么要有它：门控特性（EVSSM / IZB / meshlet-cull）的 A/B 只能在对开的档上做，
    # 而"门关逐位不变"单独构成假绿——B1b 迁移 barrier 时门开侧才是被测路径。
    overrides: tuple[str, ...] = ()

    def describe(self) -> str:
        return (f"aa={self.aa or '(随配置)'}"
                + f" size={f'{self.size[0]}x{self.size[1]}' if self.size else '(随配置)'}"
                + f" frames={self.frames}"
                + f" render_scale={self.render_scale if self.render_scale is not None else '(随配置)'}"
                + (f" clip={self.clip}" if self.clip is not None else "")
                + (f" set={list(self.overrides)}" if self.overrides else ""))


@dataclasses.dataclass
class Shot:
    tag: str
    index: int
    path: Path
    sha: str
    errors: int
    warnings: int
    secs: float
    _img: Png | None = None

    def img(self) -> Png:
        """懒解码（理由见模块 docstring 判据 4）。"""
        if self._img is None:
            self._img = png_decode_file(self.path)
        return self._img


def stage(cfg: Cfg, shot_file: str) -> None:
    smoke.stage_configs(False, shot_file, cfg.frames, cfg.size,
                        cfg.render_scale, cfg.aa,
                        cfg.clip, cfg.fsr_temporal, cfg.fsr_jitter,
                        overrides=list(cfg.overrides))


def capture(tag: str, cfg: Cfg, runs: int, quiet: bool = False) -> list[Shot]:
    """同一构建连跑 runs 次，每次留一张 PNG + 文件哈希。

    为什么 runs > 1 而不是"跑一次比一次"：单次的哈希相同与否**不能**说明确定性
    （2026-10-09 那次 1 像素漂移要连拍 14 次才撞到 2 次）。地板必须同构建多次才量得出。
    """
    OUT.mkdir(parents=True, exist_ok=True)
    shot_file = f"{OUT.name}/{tag}.png"
    stage(cfg, shot_file)
    exe = smoke.check_prereq()
    shots: list[Shot] = []
    for i in range(runs):
        smoke.single_instance_or_die()
        code, log, secs = smoke.run_engine(exe, cfg.timeout)
        (OUT / f"{tag}_{i}.log").write_text(log, encoding="utf-8")
        err_lines, warn_lines = smoke.count_severities(log)
        errors, warnings = len(err_lines), len(warn_lines)
        src = BUILD_RGL / shot_file
        if code != 0 or not src.is_file():
            raise RegressError(f"[{tag}] 第 {i} 次没出图（退出码 {code}, errors={errors}）"
                               f"\n日志尾部：\n{smoke.tail(log)}")
        dst = OUT / f"{tag}_{i}.png"
        shutil.copyfile(src, dst)
        src.unlink()
        shots.append(Shot(tag, i, dst, file_sha256(dst), errors, warnings, secs))
        if not quiet:
            print(f"[regress] {tag}#{i}  {shots[-1].sha[:12]}  {secs:.1f}s"
                  f"  errors={errors}")
    return shots


def device_of(shots: list[Shot]) -> str:
    """从引擎启动日志抓实际渲染 GPU（三机轮换，读数必须绑机器）。"""
    log = OUT / f"{shots[0].tag}_0.log"
    return smoke.gpu_of(log.read_text(encoding="utf-8", errors="replace")) if log.is_file() else "(未知)"


def summarize(tag: str, shots: list[Shot]) -> dict[str, int]:
    groups: dict[str, int] = {}
    for s in shots:
        groups[s.sha] = groups.get(s.sha, 0) + 1
    return groups


def floor_diff(shots: list[Shot]) -> tuple[Diff, int]:
    """同构建地板：各不同哈希的代表两两比对，取最大差异。返回 (地板, 不同哈希数)。"""
    reps = {}
    for s in shots:
        reps.setdefault(s.sha, s)
    kinds = len(reps)
    if kinds <= 1:
        return Diff(0, 0, None, ()), kinds
    items = list(reps.values())
    worst = Diff(0, 0, None, ())
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            d = diff_pixels(items[i].img(), items[j].img())
            if d.pixels > worst.pixels or (d.pixels == worst.pixels and d.max_lsb > worst.max_lsb):
                worst = d
    return worst, kinds


def cross_diff(a: list[Shot], b: list[Shot]) -> tuple[Diff, int]:
    """跨构建最大差异（只在不同文件哈希之间解码——相同即相同，见判据 4）。"""
    ra, rb = {}, {}
    for s in a:
        ra.setdefault(s.sha, s)
    for s in b:
        rb.setdefault(s.sha, s)
    pairs = [(x, y) for x in ra.values() for y in rb.values() if x.sha != y.sha]
    if not pairs:
        return Diff(0, 0, None, ()), 0
    worst = Diff(0, 0, None, ())
    for x, y in pairs:
        d = diff_pixels(x.img(), y.img())
        if d.pixels > worst.pixels or (d.pixels == worst.pixels and d.max_lsb > worst.max_lsb):
            worst = d
    return worst, len(pairs)


def within(a: Diff, b: Diff) -> bool:
    """a 是否不超过 b（像素数与 LSB 双双不超过；-1 = 形态不同，视为超过）。"""
    if a.pixels == 0:
        return True
    if a.pixels < 0 or b.pixels < 0:
        return False
    return a.pixels <= b.pixels and a.max_lsb <= b.max_lsb


# ─────────────────────────────────────────────────────────────────────────────
# 5. git / 构建（ab 的双构建编排）
# ─────────────────────────────────────────────────────────────────────────────


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RegressError(f"git {' '.join(args)} 失败：{(r.stderr or r.stdout).strip()}")
    return r.stdout


def run_build(tag: str) -> float:
    t = time.monotonic()
    r = subprocess.run(["just", "build"], cwd=ROOT, capture_output=True, text=True)
    secs = time.monotonic() - t
    if r.returncode != 0:
        tail = "\n".join(((r.stdout or "") + (r.stderr or "")).splitlines()[-15:])
        raise RegressError(f"[{tag}] just build 失败：\n{tail}")
    return secs


def _disk_snapshot() -> str:
    """脏路径 → 磁盘内容 sha256 的排序快照（对 staged/unstaged 形态不敏感）。

    口径：`git status --porcelain -uall`（未跟踪目录展开到文件）逐行取路径——rename/copy
    行（`R `/`C `）取 `-> ` 右侧（磁盘上存在的是新路径）；磁盘不存在（删除）记哨兵。
    两遍 stash-pop 之间「形态可漂、内容不许丢」的核对只需要这一个视图。
    """
    entries = []
    for line in git("status", "--porcelain", "-uall").splitlines():
        xy, path = line[:2], line[3:]
        if "R" in xy or "C" in xy:
            path = path.split(" -> ", 1)[-1]
        p = ROOT / path
        digest = "<deleted>" if not p.exists() else hashlib.sha256(p.read_bytes()).hexdigest()
        entries.append(f"{path}\t{digest}")
    return "\n".join(sorted(entries))


@contextlib.contextmanager
def baseline_checkout(ref: str):
    """把工作树切到 ref（构建基线二进制用），**无论成败都恢复现场**。

    两条路径：
      · ref == HEAD 且工作树脏 → `git stash push -u`（不 checkout，避免动 HEAD）
      · ref != HEAD            → `git checkout --detach <ref>`（脏的话先 stash）
    恢复后**核对磁盘内容**与进场逐字节相同——不一致就点名，绝不留下
    "以为恢复了"的状态（丢用户改动是这个工具唯一不可接受的失败）。

    ⚠ 核对的是**内容**，不是 `git status` 形态（2026-10-11 修）：`stash pop` 不保留
    staged/unstaged 区分（`M ` 回场变 ` M`、暂存新文件 `A ` 回场变 `??`），按 status
    或 `git diff HEAD` 全文比对会把「内容完好、形态漂移」误报成「恢复失败」并吞掉
    后面的判定段（15.3B/C 连撞两次；后者对「暂存新文件回场变未跟踪」同样误报——
    `diff HEAD` 看不见未跟踪文件）。快照 = 每个脏路径的磁盘内容 sha256（删除记哨兵），
    形态怎么漂、内容不丢就同值。
    """
    orig = git("rev-parse", "HEAD").strip()
    before_snap = _disk_snapshot()
    dirty = bool(git("status", "--porcelain").strip())
    msg = f"regress-baseline-{time.strftime('%Y%m%dT%H%M%S')}"
    stashed = detached = False
    stash_sha = ""
    try:
        if dirty:
            git("stash", "push", "-u", "-m", msg)
            stashed = True
            # 立刻记下**这次 stash 的 commit SHA**（不是索引）：并发 stash 不会骗它。
            stash_sha = git("rev-parse", "stash@{0}").strip()
            if git("status", "--porcelain").strip():
                raise RegressError("stash 之后工作树仍不干净（多半是 skip-worktree/assume-unchanged）"
                                   "——已中止，构建没动")
        if ref != orig:
            git("checkout", "--quiet", "--detach", ref)
            detached = True
        yield
    finally:
        problems: list[str] = []
        if detached:
            if subprocess.run(["git", "checkout", "--quiet", "--detach", orig],
                              cwd=ROOT, capture_output=True, text=True).returncode != 0:
                problems.append(f"git checkout --detach {orig[:8]} 失败")
        if stashed:
            idx = _find_stash(stash_sha, msg)
            if idx is None:
                problems.append(f"找不到本次建的 stash（sha={stash_sha[:12]} msg={msg}）"
                                "——手工核对 `git stash list` 后 pop 它")
            elif subprocess.run(["git", "stash", "pop", f"stash@{{{idx}}}"],
                                cwd=ROOT, capture_output=True, text=True).returncode != 0:
                problems.append(f"git stash pop stash@{{{idx}}} 冲突 → 手工执行它")
        if _disk_snapshot() != before_snap:
            problems.append("恢复后磁盘内容与进场不一致（脏路径逐个 sha256 核对）")
        if problems:
            raise RegressError("现场恢复失败，**先别继续改代码**：\n  - "
                               + "\n  - ".join(problems)
                               + f"\n进场 HEAD={orig[:12]} stash={stashed} sha={stash_sha[:12]}")


def _find_stash(sha: str, msg: str) -> int | None:
    """按 **commit SHA** 找本次 stash 的索引（message 只作兜底）。

    踩过的坑（2026-10-09 第一次跑 `ab` 就撞上）：`%s` 对 stash 是**reflog 主题**
    `On master: <msg>`，不是 `<msg>` ⟹ 按 message 全等匹配永远找不到 ⟹ pop 不执行
    ⟹ 用户改动静静留在 stash 里，而工具只留下一句"找不到 stash"。所以主判据用 SHA：
    它同时免疫 message 变形与"期间用户又 stash 了一次"导致的索引漂移。
    `%x09` 是 git 自己的制表符转义——写 `\\t` 会被 pretty-format 吞成字母 t。
    """
    for line in git("stash", "list", "--format=%gd%x09%H%x09%s").splitlines():
        cols = line.split("\t")
        if len(cols) != 3:
            continue
        ref, entry_sha, subject = cols
        if entry_sha != sha and not subject.strip().endswith(msg):
            continue
        m = re.match(r"stash@\{(\d+)\}", ref)
        if m:
            return int(m.group(1))
    return None


# ─────────────────────────────────────────────────────────────────────────────
# 6. 子命令
# ─────────────────────────────────────────────────────────────────────────────


def banner(cfg: Cfg) -> None:
    print(f"[regress] 主机 {socket.gethostname()} · {platform.system()} {platform.release()}"
          f" · {smoke.git_state()}")
    print(f"[regress] 配置 {cfg.describe()}")
    for note in smoke.stale_shaders():
        print(f"[regress] 注意（陈旧 SPV）: {note}")
    if newer := smoke.stale_binary():
        print(f"[regress] 注意（二进制早于源）: {', '.join(newer)}")


def cmd_floor(args: argparse.Namespace) -> int:
    cfg = cfg_from(args)
    banner(cfg)
    shots = capture("floor", cfg, args.runs)
    floor, kinds = floor_diff(shots)
    print(f"\n[regress] 同构建地板（{args.runs} 次）：{kinds} 种文件哈希 "
          f"{ {k[:8]: v for k, v in summarize('floor', shots).items()} }")
    print(f"[regress]   最大差异 = {floor}")
    errs = sum(s.errors for s in shots)
    if errs:
        print(f"[regress]   error 行合计 {errs}（逐次见 {rel(OUT)}/floor_*.log）")
    if args.expect:
        _check_expect(shots, args.expect)
    if kinds > 1:
        print("[regress] 结论：**本机存在 run-to-run 逐位漂移**。任何位比判据的 FAIL 都要先"
              "与这个地板对照再归因（ab 子命令自动做这件事）")
        return 1
    print("[regress] 结论：本机构建在此配置下逐位稳定")
    return 0 if not errs else 1


def _check_expect(shots: list[Shot], expect: str) -> None:
    shas = {s.sha for s in shots}
    if len(shas) > 1:
        print(f"[regress] --expect 不作数：多次运行本身就有 {len(shas)} 种哈希")
        return
    sha = shas.pop()
    hit = sha.startswith(expect.lower())
    print(f"[regress] --expect {expect}… → 文件哈希 {sha[:16]}… {'命中' if hit else '不命中'}")
    if not hit:
        print("[regress]   先确认这条红线是哪台机器/哪个配置（红线绑机），再怀疑代码")


def cmd_ab(args: argparse.Namespace) -> int:
    cfg = cfg_from(args)
    banner(cfg)
    head = git("rev-parse", "HEAD").strip()
    ref = args.ref
    if ref == "HEAD":
        ref = head
    if not git("status", "--porcelain").strip() and ref == head:
        raise RegressError("工作树相对 HEAD 干净且 --ref 就是 HEAD ⟹ 两侧是同一份代码，"
                           "没有可比的东西。要量本机噪声地板用 `floor`；要比历史提交用 "
                           "`--ref <sha>`")

    print(f"[regress] A = 工作树现状 · B = {ref[:12]}")
    print(f"[regress] A 侧构建…")
    print(f"[regress]   just build {run_build('A'):.1f}s")
    a = capture("A_worktree", cfg, args.runs)

    with baseline_checkout(ref):
        print(f"[regress] B 侧构建（{ref[:12]}）…")
        print(f"[regress]   just build {run_build('B'):.1f}s")
        b = capture("B_baseline", cfg, args.runs)

    # 恢复后再构建一次：别把用户的 build 目录留在基线二进制上
    print(f"[regress] 恢复现场后重建工作树二进制…")
    print(f"[regress]   just build {run_build('A-restored'):.1f}s")

    ka = summarize("A", a)
    kb = summarize("B", b)
    floor_a, na = floor_diff(a)
    floor_b, nb = floor_diff(b)
    cross, pairs = cross_diff(a, b)
    print("\n[regress] ── 结果 ──")
    print(f"[regress] 设备        : {device_of(a)}")
    print(f"[regress] A 文件哈希  : { {k[:8]: v for k, v in ka.items()} }")
    print(f"[regress] B 文件哈希  : { {k[:8]: v for k, v in kb.items()} }")
    print(f"[regress] A 同构建地板: {na} 种哈希，最大 {floor_a}")
    print(f"[regress] B 同构建地板: {nb} 种哈希，最大 {floor_b}")
    print(f"[regress] A×B 跨构建  : {pairs} 对不同哈希，最大 {cross}")

    # 像素哈希（只在文件哈希已经不同时才有信息量，但这里给出来是为了能一眼看出
    # "PNG 编码变了而画面没变"这种罕见但会误导人的情形）
    if set(ka) | set(kb) != set() and len(set(ka) | set(kb)) > 1:
        pa = {pixel_sha256(s.img()) for s in a}
        pb = {pixel_sha256(s.img()) for s in b}
        print(f"[regress] A 像素哈希  : { {k[:8]: 1 for k in pa} }")
        print(f"[regress] B 像素哈希  : { {k[:8]: 1 for k in pb} }")
        if pa == pb:
            print("[regress]   ⚠ 文件哈希不同但**像素逐位相同** ⟹ PNG 编码本身变了"
                  "（libpng 版本 / chunk 集合），画面没变")
            return 1

    if cross.pixels == 0:
        print("[regress] 判定：**逐位相同** ✅（同构建内部若有漂移见上面地板）")
        return 0
    floor = max(floor_a, floor_b, key=lambda d: (d.pixels, d.max_lsb))
    if within(cross, floor):
        print(f"[regress] 判定：**差异 ⊆ 既有 run-to-run 地板** ⟹ 不是本批引入"
              f"（地板 {floor}）")
        print("[regress]   红线是零容差 ⟹ **仍判 FAIL**。别去 git bisect 本批，"
              "但要把这条漂移本身登记成债（本机 nondeterminism）")
        return 1
    print("[regress] 判定：**差异 ⊄ 地板** ⟹ 本批引入，逐位回归破了 ❌")
    print(f"[regress]   跨构建 {cross}")
    print(f"[regress]   地板     {floor}")
    return 1


def cmd_burst(args: argparse.Namespace) -> int:
    cfg = cfg_from(args)
    banner(cfg)
    warmup = args.warmup if args.warmup is not None else _default_warmup(args.aa)
    OUT.mkdir(parents=True, exist_ok=True)
    port = _free_port()
    stage(cfg, f"{OUT.name}/burst_unused.png")
    # llm_debug 与 /shotburst 抢同一个回读槽（引擎直接 409 拒收），RPC 门默认关
    smoke.patch_lua(smoke.STAGING / "runtime.lua", "llm_debug", "false")
    smoke.patch_lua(smoke.STAGING / "diagnostics.lua", "rpc", "true")
    smoke.patch_lua(smoke.STAGING / "diagnostics.lua", "rpc_port", str(port))
    print(f"[regress] RPC 127.0.0.1:{port}（llm_debug 已关，连拍走 /shotburst）")

    log_path = OUT / "burst.log"
    with open(log_path, "wb") as logf:
        proc = subprocess.Popen([f"./{smoke.EXE_NAME}"], cwd=BUILD_RGL,
                                env=dict(os.environ, RGL_CONFIG_DIR=str(smoke.STAGING)),
                                stdout=logf, stderr=subprocess.STDOUT)
        try:
            _wait_rpc(port, proc, args.timeout)
            frame = _wait_frame(port, warmup, proc, args.timeout)
            print(f"[regress] 预热到 frame={frame}，连拍 {args.n} 帧…")
            files = _rpc(port, "/shotburst", {"n": args.n, "dir": str(OUT),
                                              "prefix": "burst_"},
                         timeout=max(60, args.n * 5))["files"]
            if len(files) != args.n:
                raise RegressError(f"/shotburst 只交了 {len(files)}/{args.n} 张")
        finally:
            with contextlib.suppress(Exception):
                _rpc(port, "/quit", {}, timeout=10)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                raise RegressError("POST /quit 后引擎没退出——日志 "
                                   f"{rel(log_path)}（并行红线：先 pgrep -x rgl 清残留）")

    shots = [Shot("burst", i, BUILD_RGL / f, file_sha256(BUILD_RGL / f), 0, 0, 0.0)
             for i, f in enumerate(files)]
    errs, _ = smoke.count_severities(log_path.read_text(encoding="utf-8", errors="replace"))
    groups: dict[str, int] = {}
    for s in shots:
        groups[s.sha] = groups.get(s.sha, 0) + 1
    print(f"[regress] {args.n} 帧 → {len(groups)} 种文件哈希"
          + (f"，error 行 {len(errs)}" if errs else ""))

    period = _min_period([s.sha for s in shots])
    if period is None:
        print(f"[regress] 未在同相位意义下收敛：{args.n} 帧内不存在 lag ≤ {args.n // 2} 的逐位重复")
    else:
        print(f"[regress] **实测最小同相位周期 = {period} 帧**（lag-{period} 全部逐位相同）")
        if args.expect_period and period != args.expect_period:
            print(f"[regress] 与期望周期 {args.expect_period} 不符 ❌")
            return 1
    if args.lag:
        bad = [(i, i + args.lag) for i in range(args.n - args.lag)
               if shots[i].sha != shots[i + args.lag].sha]
        if not bad:
            print(f"[regress] lag-{args.lag} 同相位逐位相同 ✅（{args.n - args.lag} 对）")
            _lag_control(shots, args.lag)
            return 0
        # 逐对报形态，不只报第一对：本机 TAA 交付档的 lag-16 残余差异只落在**固定几个
        # 像素**上（2026-10-09 实测 4 个锚点、恰 1 LSB），"哪几对不等、差在哪"就是归因
        # 信息（只报第一对会把它降级成一句"有 9 对不等"）。
        print(f"[regress] lag-{args.lag} **不**逐位相同 ❌：{len(bad)}/{args.n - args.lag} 对不等")
        for i, j in bad[:12]:
            d = diff_pixels(shots[i].img(), shots[j].img())
            print(f"[regress]   frame {i:>3} vs {j:>3} → {d}")
        if len(bad) > 12:
            print(f"[regress]   …其余 {len(bad) - 12} 对略")
        _lag_control(shots, args.lag)
        return 1
    return 0


def _lag_control(shots: list[Shot], lag: int) -> None:
    """用**同一次采集内部**的相邻帧差当异相位对照，把"周期成立"与"残余漂移"分开。

    为什么要有这一行：lag-N 位比红了有两种完全不同的原因——① 周期根本不是 N（相位错位，
    差异是**全屏几万像素**）；② 周期对，但本机存在 1 LSB 级漂移（差异是**几个像素**）。
    只看 lag-N 的读数分不出这两者，而它们的处置完全相反（① 查渲染，② 归因后放行判断）。
    对照不需要任何外部基线或豁免清单：相邻帧（lag-1）在抖动型时域滤波下**必然**是异相位，
    它的差异就是"相位不同该差多少"的当场刻度。
    """
    if lag <= 1 or len(shots) < 2:
        return
    d1 = diff_pixels(shots[0].img(), shots[1].img())
    if d1.pixels == 0:
        print("[regress] 对照：lag-1 差异为 0 ⟹ 本档无时域状态，lag 判据退化为"
              "「帧间逐位相同」（周期 1）")
        return
    dn = diff_pixels(shots[0].img(), shots[lag].img()) if lag < len(shots) else None
    if dn is None:
        return
    print(f"[regress] 对照：lag-1（异相位）= {d1.pixels} 像素 / ≤{d1.max_lsb} LSB，"
          f"lag-{lag} = {dn.pixels} 像素 / ≤{dn.max_lsb} LSB")
    if dn.pixels * 100 <= d1.pixels:
        print(f"[regress]   ⟹ lag-{lag} 差异比异相位对照小 {d1.pixels // max(1, dn.pixels)} 倍"
              " ⟹ **周期成立，残余是 1 LSB 级漂移，不是相位错位**")
    else:
        print(f"[regress]   ⟹ lag-{lag} 差异与异相位对照同量级 ⟹ **周期不是 {lag}**，"
              "先查相位/周期本身，别去解释残余")


def _default_warmup(aa: str | None) -> int:
    """按抗锯齿态选预热帧数，**不许一个默认值通吃**。

    实测（本机 8060S，aa=taa 交付配置，lag-16 位比）：warmup=200 → 首对差 80838 像素
    / ≤6 LSB 且遍布全屏；warmup=1200 → 16 对里 14 对逐位相同、余下 2 对只差 1 像素
    / 1 LSB。根因 = 历史是 α_long=0.01 的指数累积（时间常数 ~100 帧），**位级**收敛
    要上千帧，而"lag-16 同相位逐位相同"这条出口只在收敛之后才成立。
    ⟹ 默认值取 48 会让 taa 档稳定读出一个假的"周期不成立"（2026-10-09 本工具第一次
       跑 taa 档就是这么撞上的）。
    """
    return 16 if aa in (None, "off") else 1200


def _min_period(shas: list[str]) -> int | None:
    """最小 L ≥ 1 使所有 i 有 shas[i] == shas[i+L]（i+L < n）。

    为什么量而不写死：TAA 的 16 是 Halton 周期，FSR 的相位数是 vendor 内部按
    上屏/渲染比算的（0.75 档实测 14）。把常数抄进工具 = 换档后工具"仍然通过"而判据
    早已不是那条判据。
    """
    n = len(shas)
    for lag in range(1, n // 2 + 1):
        if all(shas[i] == shas[i + lag] for i in range(n - lag)):
            return lag
    return None


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_rpc(port: int, proc: subprocess.Popen, timeout: int) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RegressError(f"引擎在 RPC 起来之前就退了（退出码 {proc.returncode}）"
                               f"——日志 {rel(OUT / 'burst.log')}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise RegressError(f"{timeout}s 内 RPC 端口 {port} 没起来（diagnostics.rpc 没生效？"
                       f"no_vvl/门被别处盖掉？）——日志 {rel(OUT / 'burst.log')}")


def _wait_frame(port: int, want: int, proc: subprocess.Popen, timeout: int) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st = _rpc(port, "/state", timeout=5)
        frame = int(st.get("frame", 0))
        if frame >= want:
            return frame
        if proc.poll() is not None:
            raise RegressError("引擎在预热期间退出")
        time.sleep(0.1)
    raise RegressError(f"{timeout}s 内没跑到 frame={want}")


def _rpc(port: int, path: str, body: dict | None = None, timeout: float = 10) -> dict:
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data,
                                 method="POST" if body is not None else "GET",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        raise RegressError(f"{path} → HTTP {exc.code}: {exc.read().decode('utf-8', 'replace')[:200]}")
    except (urllib.error.URLError, TimeoutError) as exc:
        raise RegressError(f"{path} 无响应：{exc}")


# ─────────────────────────────────────────────────────────────────────────────
# 7. CLI
# ─────────────────────────────────────────────────────────────────────────────


def cfg_from(args: argparse.Namespace) -> Cfg:
    # size 解析复用 smoke.parse_size：同一旗标有两种宽容度 = 一条路放行另一条路报错
    return Cfg(aa=args.aa, size=smoke.parse_size(args.size),
               frames=args.frames, render_scale=args.render_scale,
               clip=args.clip,
               fsr_temporal=args.fsr_temporal, fsr_jitter=args.fsr_jitter,
               timeout=args.timeout, overrides=tuple(args.overrides or ()))


def add_cfg_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--aa", choices=["off", "taa", "fsr"])
    p.add_argument("--size")
    p.add_argument("--frames", type=int, default=8, help="第 N 帧截图（默认 8，同 just smoke）")
    p.add_argument("--render-scale", type=float)
    p.add_argument("--clip", type=int)
    p.add_argument("--fsr-temporal", action="store_true")
    p.add_argument("--fsr-jitter", action="store_true")
    # 与 just smoke --set 完全同源（同一个 patch_lua 护栏：键命中 ≠ 1 即 FAIL）。
    # ⚠ 走 `just regress` 时引号会被 just 吃掉，字符串值要直连 `python3 tools/regress.py`。
    p.add_argument("--set", dest="overrides", action="append", default=None,
                   metavar="[FILE:]KEY=VALUE",
                   help="通用配置注入（可重复），作用在具名旗标之后；门控特性的 A/B 靠它开档")
    p.add_argument("--timeout", type=int, default=120)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="regress", description=__doc__.splitlines()[0])
    ap.add_argument("--no-selftest", action="store_true",
                    help="跳过量具自检（只在你正在改自检本身时用）")
    sub = ap.add_subparsers(dest="mode", required=True)

    sub.add_parser("selftest", help="量具自检")

    p = sub.add_parser("floor", help="同一构建跑 N 次 = 噪声地板")
    add_cfg_args(p)
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--expect", help="文件 sha256 前缀（红线自己从文档抄，工具不存基线）")

    p = sub.add_parser("ab", help="同会话双构建 + 归因")
    add_cfg_args(p)
    p.add_argument("--ref", default="HEAD", help="基线 ref（默认 HEAD = 工作树 vs 已提交）")
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--expect", help="文件 sha256 前缀（对 A 侧核对）")

    p = sub.add_parser("burst", help="RPC 连拍 N 帧，量最小同相位周期")
    add_cfg_args(p)
    p.add_argument("--n", type=int, default=32)
    p.add_argument("--warmup", type=int,
                   help="连拍前先跑到第 N 帧。缺省按 aa 选：off=16，taa/fsr=1200"
                        "（时域历史是 α_long=0.01 的指数累积，**位级**收敛要上千帧；"
                        "预热不够会读出一个假的「周期不成立」，见 _default_warmup 的实测）")
    p.add_argument("--lag", type=int, help="显式判 lag-N 同相位逐位相同（债-46 出口 ①）")
    p.add_argument("--expect-period", type=int, help="顺带核对实测最小周期")

    args = ap.parse_args(argv)
    if args.mode != "selftest":
        smoke.guard_flags(args.clip, args.aa, args.fsr_temporal, args.fsr_jitter)
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.mode == "selftest":
            selftest()
            return 0
        if not args.no_selftest:
            selftest(quiet=True)
        smoke.single_instance_or_die()
        if args.mode == "floor":
            return cmd_floor(args)
        if args.mode == "ab":
            return cmd_ab(args)
        if args.mode == "burst":
            return cmd_burst(args)
        raise RegressError(f"未知子命令 {args.mode}")
    except RegressError as exc:
        print(f"[regress] {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("[regress] 中断", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
