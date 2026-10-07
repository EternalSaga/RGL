#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FSR 3.1.4 Upscaler —— Vulkan/GLSL shader 排列产物生成器。

AMD 官方的排列编译器 `ffx_sc`（sdk/tools/ffx_shader_compiler）CMakeLists 是 MSVC-only，
还依赖 Windows 版 dxc / glslangValidator 预编译库，Linux 上不可用。但它的**产物格式**
（ffx_sc.cpp: WriteShaderPermutationsHeader / WriteBinaryHeader）只是

    static const uint32_t       g_<name>_size;
    static const unsigned char  g_<name>_data[];
    static const uint32_t       g_<name>_IndirectionTable[];   // key.index -> PermutationInfo 下标
    static const <name>_PermutationInfo g_<name>_PermutationInfo[];

外加从 SPIR-V 反射出来的资源绑定表——注意那份反射表**不是可选的**：`ffx_vk.cpp:3330+`
拿它逐条搭 VkPipelineLayout（`FfxShaderBlob::boundSRVTextures` 等）。

本脚本用 glslangValidator + 自带 SPIR-V 反射复现同一套符号，编译参数逐条对齐
`sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt` 与
`sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt`。

与 ffx_sc 的两处**有意**差异（都不影响符号契约）：
  1. 每个 (pass, variant) 的 SPIR-V 内联进它自己的 `*_permutations.h`，而不是拆成
     每排列一个头文件——拆文件只是为了让 ffx_sc 并行编译，我们不需要。
  2. 去重键仍是内容 MD5（与 ffx_sc 同键），但**跨 variant 共享**同一份字节：GLSL 分支下
     wave32/wave64 编译参数完全相同（CMakeCompileShaders.txt 里 HLSL_WAVE*_ARGS 只在
     .hlsl 分支赋值），不共享就是 40 份表存 2 套字节。

用法：  python3 tools/gen_ffx_shader_blobs.py [-j N] [--out DIR]
产物：  blobs/ffx_<pass>_pass[_wave64][_16bit]_permutations.h  ×40
        blobs/ffx_fsr3upscaler_blob_pool.h                     （共享字节池 + 反射表）
        blobs/manifest.txt                                     （生成参数 + 逐文件 SHA1）

工具语言纪律（AGENTS）：stdlib-only，不引入任何 pip 依赖。
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FSR_ROOT = os.path.dirname(HERE)
VENDOR = os.path.join(FSR_ROOT, "vendor")
GPU = os.path.join(VENDOR, "include", "FidelityFX", "gpu")
SHADER_DIR = os.path.join(VENDOR, "src", "backends", "vk", "shaders", "fsr3upscaler")

GLSLANG = os.environ.get("GLSLANG_VALIDATOR", "glslangValidator")

# ── 编译参数：逐条抄自 AMD 的 CMake（取值必须一致，顺序无关） ────────────────────
API_ARGS = ["-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os"]
BASE_DEFINES = [
    ("FFX_GLSL", 1),
    ("FFX_GPU", 1),
    ("FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF", 0),
    ("FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF", 0),
    ("FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF", 1),
    ("FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF", 0),
    ("FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE", 2),
]
# 排列维度。**位域顺序 = 本表顺序 = POPULATE_PERMUTATION_KEY 的赋值顺序**，
# 三者不一致会让 IndirectionTable 查错排列（不崩，只是画面用错 shader）。
PERM_OPTIONS = [
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
]
# variant = (名字后缀, FFX_HALF)。GLSL 下 wave64 与 wave32 字节相同，见文件头注释。
VARIANTS = [("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)]

# 反射分类 → (PermutationInfo 的 count 字段名, 字段前缀, 池里数组名后缀)
CATEGORIES = [
    ("numConstantBuffers", "constantBuffer", "CBV"),
    ("numSRVTextures", "srvTexture", "TextureSRV"),
    ("numUAVTextures", "uavTexture", "TextureUAV"),
    ("numSRVBuffers", "srvBuffer", "BufferSRV"),
    ("numUAVBuffers", "uavBuffer", "BufferUAV"),
    ("numSamplers", "sampler", "Sampler"),
    ("numRTAccelerationStructures", "rtAccelerationStructure", "RTAccelerationStructure"),
]
CATEGORY_KEYS = [c[1] for c in CATEGORIES]

# ─────────────────────────────── SPIR-V 反射 ────────────────────────────────
# 只需回答「这个 descriptor 变量属于哪一类、绑在哪个 binding / set、几个元素」。
# 分类规则对齐 ffx_sc 的 GLSLCompiler::ExtractReflectionData（spirv-reflect 版）。
_DEC_BINDING, _DEC_SET, _DEC_NONWRITABLE = 33, 34, 2
_SC_UNIFORM_CONSTANT, _SC_UNIFORM, _SC_STORAGE_BUFFER = 0, 2, 3
_OP_TYPE_IMAGE, _OP_TYPE_SAMPLER, _OP_TYPE_STRUCT = 25, 26, 30
_OP_TYPE_ARRAY, _OP_TYPE_RUNTIME_ARRAY, _OP_TYPE_POINTER = 28, 29, 32
_OP_TYPE_ACCEL = 4184
_OP_CONSTANT, _OP_VARIABLE, _OP_DECORATE = 43, 59, 71
_OP_NAME = 5


def reflect_spirv(blob: bytes):
    """→ {category_key: [(binding, count, set), ...]}，按 (set, binding) 升序。

    排序是必须的：spirv-reflect 的 bindings 按 binding 号升序枚举，ffx_vk.cpp 按该
    顺序搭 pipeline layout 与 descriptor 写入顺序。
    """
    if len(blob) < 20:
        raise ValueError("SPIR-V too small")
    magic = struct.unpack_from("<I", blob, 0)[0]
    if magic != 0x07230203:
        raise ValueError("not SPIR-V")

    decos: dict[int, dict] = {}
    types: dict[int, tuple] = {}
    ptrs: dict[int, tuple] = {}
    consts: dict[int, int] = {}
    variables: dict[int, tuple] = {}
    names: dict[int, str] = {}

    off, end = 20, len(blob)
    while off + 4 <= end:
        word0 = struct.unpack_from("<I", blob, off)[0]
        opcode, length = word0 & 0xFFFF, word0 >> 16
        if length == 0:
            raise ValueError("malformed SPIR-V")
        w = struct.unpack_from("<%dI" % length, blob, off)
        if opcode == _OP_DECORATE:
            decos.setdefault(w[1], {})[w[2]] = w[3] if length > 3 else True
        elif opcode == _OP_NAME:
            raw = blob[off + 8: off + length * 4]
            names[w[1]] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
        elif opcode == _OP_CONSTANT and length >= 4:
            consts[w[2]] = struct.unpack_from("<i", blob, off + 12)[0]
        elif opcode == _OP_TYPE_IMAGE:
            # OpTypeImage 操作数：sampled_type dim depth arrayed multisampled sampled fmt acc
            # → sampled 是第 6 个操作数（w[7]）。写成 w[6]（multisampled，恒 0）会把
            # SRV 全判成 UAV：不崩、不报错，只是所有 r_* 采样图都进了 uav 表。
            types[w[1]] = ("image", w[7])
        elif opcode == _OP_TYPE_SAMPLER:
            types[w[1]] = ("sampler",)
        elif opcode == _OP_TYPE_STRUCT:
            types[w[1]] = ("struct",)
        elif opcode == _OP_TYPE_ACCEL:
            types[w[1]] = ("rtas",)
        elif opcode == _OP_TYPE_ARRAY:
            types[w[1]] = ("array", w[2], max(int(consts.get(w[3], 1)), 1))
        elif opcode == _OP_TYPE_RUNTIME_ARRAY:
            types[w[1]] = ("runtime", w[2])
        elif opcode == _OP_TYPE_POINTER:
            ptrs[w[1]] = (w[2], w[3])              # (storage class, pointee)
        elif opcode == _OP_VARIABLE:
            sc, pointee = ptrs.get(w[1], (None, None))
            variables[w[2]] = (sc, pointee)
        off += length * 4

    def unwrap(tid):
        count = 1
        while True:
            t = types.get(tid)
            if t is None:
                return tid, count
            if t[0] == "array":
                count, tid = t[2], t[1]
            elif t[0] == "runtime":
                count, tid = 0, t[1]
            else:
                return tid, count

    def classify(sc, tid):
        base, count = unwrap(tid)
        t = types.get(base)
        if t is None:
            return None
        if t[0] == "struct":
            if sc == _SC_UNIFORM:
                return ("constantBuffer", count)
            if sc == _SC_STORAGE_BUFFER:
                d = decos.get(base, {})
                return ("srvBuffer" if _DEC_NONWRITABLE in d else "uavBuffer", count)
            return None
        if t[0] == "image":
            return ("srvTexture" if t[1] == 1 else "uavTexture", count)
        if t[0] == "sampler":
            return ("sampler", count)
        if t[0] == "rtas":
            return ("rtAccelerationStructure", count)
        return None

    found = []
    for vid, (sc, pointee) in variables.items():
        if sc not in (_SC_UNIFORM_CONSTANT, _SC_UNIFORM, _SC_STORAGE_BUFFER):
            continue
        d = decos.get(vid, {})
        if _DEC_BINDING not in d or _DEC_SET not in d:
            continue
        c = classify(sc, pointee)
        if c is None:
            continue
        # 名字**不是**调试装饰：ffx_fsr3upscaler.cpp 的 RegisterPipelineResourceBindings
        # 拿 wcscmp 把 binding 名对进 srvTextureBindingTable / uavTextureBindingTable 来
        # 填 resourceIdentifier，对不上直接 FFX_ERROR_INVALID_ARGUMENT。glslang 的 -Os
        # 保留 OpName（实测 8 条变量名），所以这里取得到 r_xxx / cbXXX / s_xxx。
        found.append((int(d[_DEC_SET]), int(d[_DEC_BINDING]), c[0], c[1], names.get(vid, "")))
    found.sort(key=lambda t: (t[0], t[1]))
    out = {k: [] for k in CATEGORY_KEYS}
    for s, b, kind, count, name in found:
        out[kind].append((b, count, s, name))
    return out


# ─────────────────────────────── 编译 ───────────────────────────────────────
def compile_one(shader: str, defines, out_path: str) -> bytes:
    args = [GLSLANG] + API_ARGS + ["-o", out_path]
    args += ["-D%s=%d" % (k, v) for k, v in defines]
    args += ["-I" + GPU, "-I" + os.path.join(GPU, "fsr3upscaler"), shader]
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(out_path):
        raise RuntimeError("glslang failed (rc=%d) for %s\n%s\n%s" % (p.returncode, shader, p.stdout, p.stderr))
    with open(out_path, "rb") as f:
        return f.read()


def job(spec):
    shader, suffix, half, perm_index, tmpdir = spec
    defines = list(BASE_DEFINES)
    for i, name in enumerate(PERM_OPTIONS):
        defines.append((name, (perm_index >> i) & 1))
    defines.append(("FFX_HALF", half))
    out = os.path.join(tmpdir, "%s%s_%d.spv"
                       % (os.path.splitext(os.path.basename(shader))[0],
                          suffix and "_" + suffix.strip("_") or "", perm_index))
    blob = compile_one(shader, defines, out)
    return shader, suffix, perm_index, blob, reflect_spirv(blob)


# ─────────────────────────────── 产物写出 ───────────────────────────────────
COMPONENT_SRC = os.path.join(VENDOR, "src/components/fsr3upscaler/ffx_fsr3upscaler.cpp")


def load_binding_tables():
    """从 component 源码里抠出 srv/uavTextureBindingTable 的名字集合。

    为什么要在生成期对这张表：ffx_fsr3upscaler.cpp:322-344 的
    RegisterPipelineResourceBindings 用 **wcscmp(binding 名)** 把 shader 资源对进
    resourceIdentifier，对不上就 FFX_ERROR_INVALID_ARGUMENT。也就是说 SPIR-V 反射出来的
    变量名是**功能正确性的一部分**，不是调试装饰——名字全空的话库能编、能链、能创建
    context，第一次 dispatch 才炸。这里把它变成生成期的硬失败。
    """
    src = open(COMPONENT_SRC, "r", encoding="utf-8", errors="replace").read()
    tables = {}
    for kind in ("srvTexture", "uavTexture"):
        m = re.search(kind + r"BindingTable\[\]\s*=\s*\{(.*?)\};", src, re.S)
        if not m:
            raise RuntimeError("在 %s 里找不到 %sBindingTable（上游改了结构？）"
                               % (COMPONENT_SRC, kind))
        tables[kind] = set(re.findall(r'L"([^"]*)"', m.group(1)))
    return tables


def validate_names(blobs):
    """阳性锚点：每个 (pass, variant) 反射出的 SRV/UAV 名字必须能在 component 的表里查到。"""
    tables = load_binding_tables()
    bad = []
    checked = 0
    for md5, (_data, refl) in sorted(blobs.items()):
        for kind in ("srvTexture", "uavTexture"):
            for _b, _c, _s, nm in refl.get(kind, []):
                checked += 1
                if nm not in tables[kind]:
                    bad.append("%s: %s 不在 %sBindingTable 里" % (md5[:12], nm or "(空名)", kind))
    if bad:
        print("反射名对账 **失败**（%d/%d 条对不上）：" % (len(bad), checked), file=sys.stderr)
        for b in bad[:20]:
            print("  - " + b, file=sys.stderr)
        raise SystemExit(1)
    print("反射名对账：OK（%d 条 SRV/UAV 名全部命中 component 绑定表）" % checked)


# ── typed image 格式对账（护栏，见 PATCHES.md P11 / 债-73）───────────────────────
# GLSL 侧 `layout(..., <格式>) uniform image2D` 的格式限定符**必须**与 host 侧给该资源
# 选的 FfxSurfaceFormat 一致，否则 VVL 判「整个 image 的读写值未定义」（实测 RADV 上
# rw_luma_history 就是这么炸的：shader 写 rgba8、资源建成 R16G16B16A16_FLOAT）。
# DX12 侧 UAV 格式取自资源、限定符被忽略 ⟹ **Windows 永远看不见这类 bug**，只能靠对账。
# HLSL 回调头是这里的「参考实现」：那边写 RWTexture2D<FfxFloat32x4> 的，格式就取资源格式。
GLSL_CALLBACKS = os.path.join(GPU, "fsr3upscaler", "ffx_fsr3upscaler_callbacks_glsl.h")

# GLSL 限定符 → FFX 自己的格式拼写（注意 FFX 用 _FLOAT，Vulkan 头里才叫 _SFLOAT）
_GLSL_FMT_TO_FFX = {
    "r8": "R8_UNORM", "r16f": "R16_FLOAT", "rg16f": "R16G16_FLOAT",
    "rgba8": "R8G8B8A8_UNORM", "rgba16f": "R16G16B16A16_FLOAT",
    "rgba32f": "R32G32B32A32_FLOAT", "r32f": "R32_FLOAT",
    "r11f_g11f_b10f": "B10G11R11_UFLOAT_PACK32",
}


def load_resource_formats():
    """resourceIdentifier → FfxSurfaceFormat（host 侧建资源用的那张表）。"""
    src = open(COMPONENT_SRC, "r", encoding="utf-8", errors="replace").read()
    fmt_of = {}
    for m in re.finditer(r"(FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_\w+),\s*L\"[^\"]*\"\s*,[^;]*?FFX_SURFACE_FORMAT_(\w+)", src):
        fmt_of.setdefault(m.group(1), m.group(2))
    if not fmt_of:
        raise RuntimeError("在 %s 里解析不到任何 FFX_SURFACE_FORMAT_*（上游改了资源表结构？）" % COMPONENT_SRC)
    return fmt_of


def validate_image_formats():
    """typed image2D 的格式限定符 vs host 侧资源格式。

    只比对**两边都查得到**的：app 提供格式的资源（color / output / depth / 各 reactive）
    没有 host 侧格式项，跳过。ping-pong 双缓冲（LUMA_HISTORY → _1/_2）按前缀解析，
    但要求前缀下的格式**唯一**，否则不猜。

    防「空集合假绿」：解析不到 typed image、或可比对数为 0，一律当失败——上游重构了
    写法却没人发现，比报错贵得多。
    """
    glsl = open(GLSL_CALLBACKS, "r", encoding="utf-8", errors="replace").read()
    typed = {}
    for m in re.finditer(r"layout\s*\(\s*set\s*=\s*0,\s*binding\s*=\s*([A-Z_0-9]+)([^)]*)\)\s*"
                         r"(?:writeonly\s+)?uniform\s+image2D\s+(rw_\w+)\s*;", glsl):
        attrs = m.group(2).strip().rstrip(",").strip()
        fmt = attrs.split(",")[-1].strip() if "," in attrs else None
        if fmt:
            typed[m.group(3)] = fmt
    if not typed:
        raise RuntimeError("%s 里解析不到任何带格式限定符的 image2D（上游改了声明写法？"
                           "护栏不能靠『没解析到』通过）" % GLSL_CALLBACKS)

    tables = load_binding_tables()
    src = open(COMPONENT_SRC, "r", encoding="utf-8", errors="replace").read()
    mu = re.search(r"uavTextureBindingTable\[\]\s*=\s*\{(.*?)\};", src, re.S)
    name2id = {m.group(2): m.group(1)
               for m in re.finditer(r"\{\s*(FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_\w+)\s*,\s*L\"(\w+)\"\s*\}", mu.group(1))}
    fmt_of = load_resource_formats()

    bad, checked = [], 0
    for name, fmt in sorted(typed.items()):
        if name not in tables["uavTexture"]:
            continue                                   # 不在绑定表 = app 侧资源，无从比对
        ident = name2id.get(name)
        if not ident:
            continue
        want = fmt_of.get(ident)
        if want is None:                                 # ping-pong 别名：前缀下格式须唯一
            cands = {v for k, v in fmt_of.items() if k.startswith(ident + "_")}
            if len(cands) != 1:
                continue
            want = cands.pop()
        checked += 1
        if fmt not in _GLSL_FMT_TO_FFX:
            bad.append("%s: 未知 GLSL 格式限定符 %r（先补 _GLSL_FMT_TO_FFX）" % (name, fmt))
        elif _GLSL_FMT_TO_FFX[fmt] != want:
            bad.append("%s: shader 写 %s（=%s），host 侧资源是 FFX_SURFACE_FORMAT_%s"
                       % (name, fmt, _GLSL_FMT_TO_FFX[fmt], want))
    if checked == 0:
        raise RuntimeError("typed image 格式对账一条都没比对上（绑定表/资源表解析漂了？）")
    if bad:
        print("typed image 格式对账 **失败**（%d 条不匹配 / 共比对 %d 条）：" % (len(bad), checked), file=sys.stderr)
        for b in bad:
            print("  - " + b, file=sys.stderr)
        print("  这类不一致在 DX12 上不可见（UAV 格式取自资源），在 Vulkan 上 VVL 判"
              "「整个 image 读写值未定义」。改 shader 的格式限定符去对齐 host 侧资源格式"
              "（= HLSL 侧 RWTexture2D<> 的语义），别反过来降资源精度。", file=sys.stderr)
        raise SystemExit(1)
    print("typed image 格式对账：OK（%d/%d 条与 host 侧资源格式一致）" % (checked, len(typed)))


def c_escape(s):
    """反射出来的名字来自 OpName，正常就是标识符；真撞上引号/反斜杠也不要把头文件写坏。"""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def write_pool(path, blobs):
    """blobs: md5 -> (bytes, reflection)。所有 (pass, variant) 共享一份字节。"""
    out = [
        "// 自动生成，请勿手改。生成器：third_party/fsr/tools/gen_ffx_shader_blobs.py",
        "// 被 40 个 *_permutations.h 共同 include，#pragma once 去重。",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for md5 in sorted(blobs):
        data, refl = blobs[md5]
        n = "ffx_fsr3upscaler_blob_" + md5
        out.append("static const uint32_t g_%s_size = %d;" % (n, len(data)))
        out.append("static const unsigned char g_%s_data[] = {" % n)
        for i in range(0, len(data), 16):
            out.append(",".join("0x%02x" % b for b in data[i:i + 16]) + ",")
        out.append("};")
        out.append("")
        for _cnt, key, sfx in CATEGORIES:
            res = refl.get(key, [])
            if not res:
                continue
            out.append('static const char* g_%s_%sResourceNames[] = { %s };'
                       % (n, sfx, ", ".join('"%s"' % c_escape(nm) for *_x, nm in res)))
            out.append("static const uint32_t g_%s_%sResourceBindings[] = { %s };"
                       % (n, sfx, ", ".join(str(b) for b, _c, _s, _nm in res)))
            out.append("static const uint32_t g_%s_%sResourceCounts[] = { %s };"
                       % (n, sfx, ", ".join(str(c) for _b, c, _s, _nm in res)))
            out.append("static const uint32_t g_%s_%sResourceSets[] = { %s };"
                       % (n, sfx, ", ".join(str(s) for _b, _c, s, _nm in res)))
            out.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def write_permutations(path, shader_name, blobs, blob_order, key_to_index, total_keys):
    out = [
        "// 自动生成，请勿手改。生成器：third_party/fsr/tools/gen_ffx_shader_blobs.py",
        "// 符号契约 = AMD ffx_sc 的 <%s_permutations.h>。" % shader_name,
        "#pragma once",
        "#include <stdint.h>",
        '#include "ffx_fsr3upscaler_blob_pool.h"',
        "",
        "typedef union %s_PermutationKey {" % shader_name,
        "    struct {",
    ]
    for name in PERM_OPTIONS:
        out.append("        uint32_t %s : 1;" % name)
    out += ["    };", "    uint32_t index;", "} %s_PermutationKey;" % shader_name, ""]
    out += ["typedef struct %s_PermutationInfo {" % shader_name,
            "    const uint32_t       blobSize;",
            "    const unsigned char* blobData;"]
    for cnt, key, _sfx in CATEGORIES:
        out += ["",
                "    const uint32_t  %s;" % cnt,
                "    const char**    %sNames;" % key,
                "    const uint32_t* %sBindings;" % key,
                "    const uint32_t* %sCounts;" % key,
                "    const uint32_t* %sSpaces;" % key]
    out += ["} %s_PermutationInfo;" % shader_name, "",
            "static const uint32_t g_%s_IndirectionTable[] = {" % shader_name]
    for i in range(total_keys):
        out.append("    %d," % key_to_index.get(i, 0))
    out += ["};", "",
            "static const %s_PermutationInfo g_%s_PermutationInfo[] = {" % (shader_name, shader_name)]
    for md5 in blob_order:
        n = "ffx_fsr3upscaler_blob_" + md5
        refl = blobs[md5][1]
        fields = ["g_%s_size" % n, "g_%s_data" % n]
        for _cnt, key, sfx in CATEGORIES:
            res = refl.get(key, [])
            if res:
                fields += [str(len(res)),
                           "g_%s_%sResourceNames" % (n, sfx),
                           "g_%s_%sResourceBindings" % (n, sfx),
                           "g_%s_%sResourceCounts" % (n, sfx),
                           "g_%s_%sResourceSets" % (n, sfx)]
            else:
                fields += ["0", "0", "0", "0", "0"]
        out.append("    { " + ", ".join(fields) + " },")
    out += ["};", ""]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_inputs():
    """护栏的比对对象 = 产物的**输入**。生成器脚本自身也在内：编译参数（BASE_DEFINES /
    PERM_OPTIONS / VARIANTS）就写死在脚本里，改一个 define 不改 shader，产物同样会变。"""
    inputs = [os.path.abspath(__file__)]
    for root, _dirs, files in os.walk(SHADER_DIR):
        inputs += [os.path.join(root, f) for f in files if f.endswith(".glsl")]
    for root, _dirs, files in os.walk(GPU):
        inputs += [os.path.join(root, f) for f in files if f.endswith(".h")]
    return sorted(inputs)


def read_manifest(path):
    inputs, outputs = {}, {}
    if not os.path.isfile(path):
        return inputs, outputs
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if len(p) == 3 and p[0] == "INPUT":
                inputs[p[2]] = p[1]
            elif len(p) == 3 and p[0] == "SHA1":
                outputs[p[2]] = p[1]
    return inputs, outputs


def check(out_dir):
    """护栏：输入漂移（改了 shader/生成器却没重跑）或产物被手改 → rc=1 并点名。"""
    # 先做纯文本对账：格式不一致时 SHA1 可能完全正确（上游就是这么签进来的），
    # 所以它必须在哈希比对之前、且不受哈希结果影响。
    validate_image_formats()
    mpath = os.path.join(out_dir, "manifest.txt")
    if not os.path.isfile(mpath):
        print("FSR blobs: manifest.txt 缺失 → 跑 `just fsr-blobs`", file=sys.stderr)
        return 1
    want_in, want_out = read_manifest(mpath)
    bad = []
    for p in collect_inputs():
        rel = "fsr/" + os.path.relpath(p, FSR_ROOT).replace(os.sep, "/")
        if rel not in want_in or sha1(p) != want_in[rel]:
            bad.append("输入已变、产物未重生成: " + rel)
    for fn, want in sorted(want_out.items()):
        fp = os.path.join(out_dir, fn)
        if not os.path.isfile(fp):
            bad.append("产物缺失: " + fn)
        elif sha1(fp) != want:
            bad.append("产物与 manifest 不符（被手改？）: " + fn)
    if bad:
        print("FSR shader 排列产物护栏 **失败**：", file=sys.stderr)
        for b in bad:
            print("  - " + b, file=sys.stderr)
        print("修法：python3 third_party/fsr/tools/gen_ffx_shader_blobs.py 然后随改动提交 blobs/",
              file=sys.stderr)
        return 1
    print("FSR shader 排列产物护栏：OK（%d 输入 / %d 产物）" % (len(want_in), len(want_out)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(FSR_ROOT, "blobs"))
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 4)))
    ap.add_argument("--check", action="store_true",
                    help="不生成，只按 manifest.txt 比对输入/产物 SHA1（构建护栏用，毫秒级）")
    args = ap.parse_args()

    if args.check:
        return check(args.out)

    shaders = sorted(glob.glob(os.path.join(SHADER_DIR, "*.glsl")))
    if not shaders:
        print("no shaders under " + SHADER_DIR, file=sys.stderr)
        return 1
    total_keys = 1 << len(PERM_OPTIONS)
    specs = [(s, suffix, half, p, None)
             for s in shaders for (suffix, half) in VARIANTS for p in range(total_keys)]
    print("passes=%d variants=%d perms=%d → %d glslang invocations"
          % (len(shaders), len(VARIANTS), total_keys, len(specs)))

    blobs = {}                       # md5 -> (bytes, reflection)
    hashes = {}                      # (shader, suffix) -> {perm_index: md5}
    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        specs = [(s, suf, half, p, tmp) for (s, suf, half, p, _) in specs]
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futures = [ex.submit(job, s) for s in specs]
            # 注意：as_completed 的到达顺序是**不确定的**，所以这里只收集，
            # 去重顺序在下面按 (pass, variant, perm_index) 升序重放——否则重跑
            # 一遍产物字节就变，SHA1 护栏会假报警。
            for fut in concurrent.futures.as_completed(futures):
                shader, suffix, perm_index, blob, refl = fut.result()
                md5 = hashlib.md5(blob).hexdigest()
                blobs.setdefault(md5, (blob, refl))
                hashes.setdefault((shader, suffix), {})[perm_index] = md5
                done += 1
                if done % 400 == 0:
                    print("  %d/%d" % (done, len(specs)), flush=True)

    per_variant = {}
    for key in sorted(hashes):
        order, k2i = [], {}
        for p in range(total_keys):
            md5 = hashes[key][p]
            if md5 not in order:
                order.append(md5)
            k2i[p] = order.index(md5)
        per_variant[key] = (order, k2i)

    validate_names(blobs)
    validate_image_formats()

    write_pool(os.path.join(args.out, "ffx_fsr3upscaler_blob_pool.h"), blobs)
    produced = ["ffx_fsr3upscaler_blob_pool.h"]
    for shader in shaders:
        base = os.path.splitext(os.path.basename(shader))[0]
        for suffix, _half in VARIANTS:
            name = base + suffix
            order, k2i = per_variant[(shader, suffix)]
            fn = "%s_permutations.h" % name
            write_permutations(os.path.join(args.out, fn), name, blobs, order, k2i, total_keys)
            produced.append(fn)

    total_bytes = sum(len(v[0]) for v in blobs.values())
    with open(os.path.join(args.out, "manifest.txt"), "w", newline="\n") as f:
        f.write("# 由 tools/gen_ffx_shader_blobs.py 生成（内容相同的重跑结果一致）\n")
        f.write("glslang=%s\n" % " ".join([GLSLANG] + API_ARGS))
        f.write("defines=%s\n" % ",".join("%s=%d" % kv for kv in BASE_DEFINES))
        f.write("permutations=%s\n" % ",".join(PERM_OPTIONS))
        f.write("variants=%s\n" % ",".join((sfx or "w32") + ":FFX_HALF=%d" % h for sfx, h in VARIANTS))
        f.write("unique_blobs=%d\n" % len(blobs))
        f.write("spirv_bytes=%d\n" % total_bytes)
        for p in collect_inputs():
            f.write("INPUT %s fsr/%s\n" % (sha1(p), os.path.relpath(p, FSR_ROOT).replace(os.sep, "/")))
        for fn in sorted(produced):
            f.write("SHA1 %s %s\n" % (sha1(os.path.join(args.out, fn)), fn))
    print("unique SPIR-V modules = %d (%.1f MiB), headers = %d"
          % (len(blobs), total_bytes / 1048576.0, len(produced)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
