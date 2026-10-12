#!/usr/bin/env python3
"""StructuredBuffer 元素 stride 对账护栏（R3 窄化的**配套保护**，构建期恒跑）。

stdlib-only（AGENTS.md 工具语言纪律：留在 Python 的工具只准 stdlib）。

## 要守的那条不变量

`StructuredBuffer<T>` 在 SPIR-V 里的元素步长（`ArrayStride`）**必须等于**反射生成物里
C++ 侧的 `sizeof(T)`。不等 ⟹ GPU 从错误的字节读元素 1、2、3……**不报错、不 device
lost、VVL 不报**，只把画面画错（git:调试历史「灰屏」）。

## 为什么需要它（R3 从"全禁"窄化成"只禁无契约裸 struct"的前提）

R3 原来一刀切禁 StructuredBuffer，理由是"stride ≥16B 对齐与 C++ sizeof 静默偏离"。
2026-10-12 实测推翻的正是这条理由的**普适性**：项目全量带 `-fvk-use-scalar-layout`
（`cmake/RglSlang.cmake` 的 SLANG_FLAGS 唯一声明点），scalar layout 下含 `float3` 的
struct 其 ArrayStride 就是 C++ `sizeof`，**不补到 16**。完整实测与结论 →
`docs/constantbuffer_迁移计划.md` §8（**本文件不抄那张表 = 不做第三副本**）。

放行"契约类型可用作 StructuredBuffer 元素"的代价是：这条不变量从"编译器不会给你错的机会"
降级成"有人守着才对"。本脚本就是那个守着的。**没有本脚本，R3 不许窄化。**

## 机制（三张脸对账，缺一面就抓不到）

    C++ 侧真相      rgl/generated/*_types_generated.cppm 的 static_assert(sizeof(T) == N)
    编译口径真相    cmake/RglSlang.cmake 的 SLANG_FLAGS（**现场解析，不内联重复 flag**）
    shader 侧声明   rgl/shaders/*.slang 里 `struct T` 的定义所在模块

对每个契约类型 T：按定义模块合成一个探针 .slang（`import <模块>;` + 每个 T 一个
`StructuredBuffer<T>`），用**构建期同一套 SLANG_FLAGS** 现场编成 SPV，解析二进制取该
binding 的 `ArrayStride`，与 `sizeof(T)` 对账。

为什么是**探针**而不是读已构建的 SPV：今天 `rgl/shaders/` 里零处 `StructuredBuffer<T>`
（★1-a2 已回退成 `ByteAddressBuffer`），"只查实际用到的类型"= 零覆盖 = 护栏恒绿但恒不
咬人。探针把覆盖面从"已用的类型"扩到"**R3 放行后任何可能被用的类型**"（= 全部契约类型），
顺带把本护栏变成对 SLANG_FLAGS 掉 flag 的常驻检测（掉 flag → 含 float3 的类型立刻红）。

为什么探针必须 `GetDimensions` 把 buffer 用一下：slang 的 DCE 会把"声明了但没读"的 buffer
连它的 `ArrayStride` 装饰一起删掉（实测），于是"没查到 stride"与"stride 不对"分不开。

## 失效必须出声（项目明文纪律：零警告会被读成"干净"）

slangc 找不到 / 探针编译失败 / binding 在 SPV 里解析不到 / 契约类型在 shaders 里没有
struct 定义 / 契约集合为空 —— 一律 ERROR 点名，并显式打印「本次**未检查** M 个类型」。
正常路径也打印「已检查 N 个」，让"检查跑过且覆盖面是多大"每次构建都看得见。

## 输出形态与退出码（与 tools/check_shader_deps.py 同型）

ERROR 行红粗体醒目，**默认不阻断构建**（exit 0）；`--strict` 时有问题 exit 1。
每次 build 恒执行（挂在 shader_dep_check 这个无 OUTPUT 的 custom target 上）。

    用法：python3 tools/check_spirv_stride.py [--strict] [--build-dir DIR]
                                              [--slangc PATH] [--probe-dir DIR]
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GENERATED_DIR = ROOT / "rgl" / "generated"
SHADERS_DIR = ROOT / "rgl" / "shaders"
SLANG_CMAKE = ROOT / "cmake" / "RglSlang.cmake"

# 反射生成物里 sizeof 的机械来源（**别手抄类型名单**——抄名单反模式，副本必然漂）。
# 只扫 *_types_generated.cppm：features_/param_semantics_ 两份生成物里没有 struct。
RE_SIZEOF = re.compile(r"static_assert\s*\(\s*sizeof\s*\(\s*([A-Za-z_]\w*)\s*\)\s*==\s*(\d+)")
RE_SLANG_STRUCT = re.compile(r"^\s*(?:export\s+)?struct\s+([A-Za-z_]\w*)\b", re.M)
RE_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
RE_LINE_COMMENT = re.compile(r"[ \t]*//[^\n]*")
# StructuredBuffer 及其变体的元素类型（剥注释后）。本脚本用它把"实际使用面"打进汇总，
# 并对**查不到 stride 的元素类型**（既不是契约类型、探针也就没覆盖它）出声，
# 避免"探针全绿但真用的那个没覆盖"被读成干净。
RE_SB_USE = re.compile(r"\b(?:RW|Append|Consume)?StructuredBuffer\s*<\s*([A-Za-z_]\w*)")

# SPIR-V 二进制常量（值取自 /usr/include/spirv/unified1/spirv.hpp：
# DecorationArrayStride=6 / DecorationBinding=33；OpName=5 / OpTypeRuntimeArray=29 /
# OpTypeStruct=30 / OpTypePointer=32 / OpVariable=59 / OpDecorate=71）。
# 自己解析而不调 spirv-dis：spirv-tools 不是本项目的构建依赖，护栏依赖它 = 没装就静默。
SPV_MAGIC = 0x07230203
OP_NAME, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT = 5, 29, 30
OP_TYPE_POINTER, OP_VARIABLE, OP_DECORATE = 32, 59, 71
DEC_ARRAY_STRIDE, DEC_BINDING = 6, 33

# 探针 binding 基址：避开被 import 模块自己的全局 binding（小数字段），
# 也让"探针的 binding"在报错信息里一眼可辨。
PROBE_BINDING_BASE = 100


def rel(p: Path) -> str:
    try:
        return p.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return p.as_posix()


def strip_comments(text: str) -> str:
    """剥块/行注释；块注释按换行数回填，保持行号与源文件一致（报错要点行）。"""
    text = RE_BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return RE_LINE_COMMENT.sub("", text)


# ─── 三张输入面 ──────────────────────────────────────────────────────────────

def contract_sizes(generated_dir: Path) -> tuple[dict[str, int], list[str]]:
    """反射生成物 → {契约类型: sizeof}。同类型两处 sizeof 不一致 = 问题（出声）。"""
    sizes: dict[str, int] = {}
    problems: list[str] = []
    for f in sorted(generated_dir.glob("*_types_generated.cppm")):
        for name, size in RE_SIZEOF.findall(f.read_text(encoding="utf-8", errors="replace")):
            got = int(size)
            if name in sizes and sizes[name] != got:
                problems.append(
                    f"{rel(f)}: 契约类型 {name} 的 sizeof 有两个值（{sizes[name]} 与 {got}）"
                    f"——反射生成物自相矛盾，本护栏的 C++ 侧真相不成立")
                continue
            sizes[name] = got
    return sizes, problems


def struct_defining_modules(shaders_dir: Path) -> dict[str, list[Path]]:
    """shaders/ 下 `struct T` 的定义位置：{类型: [定义它的 .slang...]}。"""
    defs: dict[str, list[Path]] = {}
    for p in sorted(shaders_dir.rglob("*.slang")):
        for name in RE_SLANG_STRUCT.findall(strip_comments(
                p.read_text(encoding="utf-8", errors="replace"))):
            defs.setdefault(name, []).append(p)
    return defs


def structured_buffer_uses(shaders_dir: Path) -> list[tuple[Path, int, str]]:
    """shaders/ 里实际的 StructuredBuffer 使用：[(文件, 行, 元素类型)]。"""
    out: list[tuple[Path, int, str]] = []
    for p in sorted(shaders_dir.rglob("*.slang")):
        for lineno, line in enumerate(strip_comments(
                p.read_text(encoding="utf-8", errors="replace")).splitlines(), 1):
            for m in RE_SB_USE.finditer(line):
                out.append((p, lineno, m.group(1)))
    return out


def slang_flags_from_cmake(cmake_file: Path) -> tuple[list[str], str]:
    """解析 cmake/RglSlang.cmake 的 SLANG_FLAGS（**唯一声明点**，不内联重复）。

    护栏必须用构建期同一套 flag 编探针：探针自带一份 flag 副本 = 构建掉 `-fvk-use-scalar-layout`
    而护栏仍然绿（护栏反过来给真正的错配背书，比没有护栏更糟）。
    """
    try:
        text = cmake_file.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [], f"读不到 {rel(cmake_file)}: {exc}"
    m = re.search(r"^[ \t]*set\s*\(\s*SLANG_FLAGS\b(.*?)\)", text, re.S | re.M)
    if not m:
        return [], f"{rel(cmake_file)} 里找不到 set(SLANG_FLAGS ...)——SLANG_FLAGS 唯一声明点漂移"
    body = re.sub(r"#[^\n]*", "", m.group(1))
    return body.split(), ""


# ─── 探针生成与编译 ──────────────────────────────────────────────────────────

def make_probe(module: str, types: list[str]) -> str:
    """一个定义模块 → 一个探针（该模块的全部契约类型各占一个 binding）。

    `GetDimensions` 是**保活**用的，不是判据：slang 的 DCE 会删掉没被读的 buffer 连同
    它的 ArrayStride 装饰（实测），探针就退化成"什么都没查到"。
    """
    lines = [
        f"// 自动生成的探针 —— tools/check_spirv_stride.py，勿手改、勿当契约读",
        f"import {module};",
    ]
    for i, t in enumerate(types):
        lines.append(f"[[vk::binding({PROBE_BINDING_BASE + i}, 0)]] StructuredBuffer<{t}> "
                     f"rgl_stride_probe_{i};")
    lines.append(f"[[vk::binding({PROBE_BINDING_BASE + len(types)}, 0)]] "
                 f"RWByteAddressBuffer rgl_stride_probe_sink;")
    lines += ["[shader(\"compute\")]", "[numthreads(1, 1, 1)]", "void main() {",
              "    uint s = 0u, n = 0u, t = 0u;"]
    for i in range(len(types)):
        lines.append(f"    rgl_stride_probe_{i}.GetDimensions(s, n); t += s + n;")
    lines += ["    rgl_stride_probe_sink.Store(0, t);", "}", ""]
    return "\n".join(lines)


def find_slangc(explicit: str, build_dir: Path) -> tuple[str, str]:
    """slangc 定位：--slangc（CMake 传 SLANGC_EXECUTABLE）→ RGL_SLANGC_PATH → 构建目录里找。"""
    if explicit:
        return (explicit, "") if Path(explicit).exists() else ("", f"--slangc 指定的路径不存在: {explicit}")
    env = os.environ.get("RGL_SLANGC_PATH", "")
    if env:
        return (env, "") if Path(env).exists() else ("", f"RGL_SLANGC_PATH 指向的文件不存在: {env}")
    for cand in sorted(build_dir.glob("_deps/slang-prebuilt*/bin/slangc")) if build_dir else []:
        return str(cand), ""
    return "", (f"找不到 slangc（--slangc / RGL_SLANGC_PATH / {build_dir}"
                f"/_deps/slang-prebuilt*/bin/slangc 全空）")


def compile_probe(slangc: str, flags: list[str], probe: Path, out_spv: Path,
                  include_dir: Path) -> tuple[bool, str]:
    cmd = [slangc, str(probe), *flags, "-I", str(include_dir), "-o", str(out_spv)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"调用 slangc 失败: {exc}"
    if r.returncode != 0:
        return False, (r.stderr or r.stdout or "").strip()
    return (True, "") if out_spv.exists() else (False, "slangc 退出码 0 但没有产出 SPV")


# ─── SPIR-V 二进制里取 ArrayStride ───────────────────────────────────────────

def strides_by_binding(spv: Path) -> tuple[dict[int, tuple[int, str]], str]:
    """SPV → {binding: (ArrayStride, 元素结构体名)}。解析失败返回原因（不出声的失败不算结论）。

    解析链（**按 binding 号定位**，不按类型名匹配）：
        OpVariable(+Binding 装饰) → OpTypePointer → [Slang 的 wrapper OpTypeStruct]
        → OpTypeRuntimeArray → OpTypeStruct(+OpName)。ArrayStride 装饰在 runtime array 上。

    两个自己踩过的编码细节（写下来免得下一个人重踩）：
      * `OpVariable` 的操作数序是 **Result Type 在前、Result <id> 在后**（`OpType*` 没有
        result type，第 1 个字就是 id）。读反了 = 一个变量都认不出。
      * slang 不直接 `OpTypePointer → OpTypeRuntimeArray`，而是先包一层
        `OpTypeStruct { OpTypeRuntimeArray }`，且 wrapper 的 OpName 恒为 `StructuredBuffer`
        （**不随元素类型变**）⟹ 按名字找元素类型这条路根本走不通。也再次说明判据不能建在
        mangled name 上：slang 把布局模式编进类型名（`T_natural` / `T_std430`）。

    自己解析而不调 spirv-dis：spirv-tools 不是本项目的构建依赖，护栏依赖它 = 没装就静默。
    """
    data = spv.read_bytes()
    if len(data) < 20:
        return {}, f"{rel(spv)}: 不足 20 字节，不是 SPV"
    endian = "<" if struct.unpack_from("<I", data, 0)[0] == SPV_MAGIC else ">"
    if struct.unpack_from(endian + "I", data, 0)[0] != SPV_MAGIC:
        return {}, f"{rel(spv)}: 魔数不是 {SPV_MAGIC:#x}，不是 SPV"
    nwords = len(data) // 4
    words = struct.unpack(endian + f"{nwords}I", data[: nwords * 4])

    names: dict[int, str] = {}
    struct_members: dict[int, list[int]] = {}
    runtime_elem: dict[int, int] = {}
    ptr_pointee: dict[int, int] = {}
    var_type: dict[int, int] = {}
    binding_of: dict[int, int] = {}
    stride_of: dict[int, int] = {}

    i = 5  # 前 5 个字是 header
    while i < nwords:
        head = words[i]
        wcount, opc = head >> 16, head & 0xFFFF
        if wcount == 0 or i + wcount > nwords:
            return {}, f"{rel(spv)}: 第 {i} 个字处指令长度非法（SPV 截断？）"
        if opc == OP_NAME and wcount >= 3:
            chars = bytearray()
            for widx in range(i + 2, i + wcount):
                chars += struct.pack(endian + "I", words[widx])
            names[words[i + 1]] = chars.split(b"\x00", 1)[0].decode("utf-8", "replace")
        elif opc == OP_TYPE_STRUCT and wcount >= 2:
            struct_members[words[i + 1]] = list(words[i + 2:i + wcount])
        elif opc == OP_TYPE_RUNTIME_ARRAY and wcount >= 3:
            runtime_elem[words[i + 1]] = words[i + 2]
        elif opc == OP_TYPE_POINTER and wcount >= 4:
            ptr_pointee[words[i + 1]] = words[i + 3]
        elif opc == OP_VARIABLE and wcount >= 3:
            var_type[words[i + 2]] = words[i + 1]  # 第 1 字 = Result Type，第 2 字 = Result <id>
        elif opc == OP_DECORATE and wcount >= 4:
            target, dec, lit = words[i + 1], words[i + 2], words[i + 3]
            if dec == DEC_ARRAY_STRIDE:
                stride_of[target] = lit
            elif dec == DEC_BINDING:
                binding_of[target] = lit
        i += wcount

    def runtime_array_of(type_id: int) -> int:
        """指针指向的类型 → runtime array 的 id（穿过 slang 的 wrapper struct）。"""
        if type_id in runtime_elem:
            return type_id
        for member in struct_members.get(type_id, []):
            if member in runtime_elem:
                return member
        return -1

    out: dict[int, tuple[int, str]] = {}
    for var, binding in binding_of.items():
        ptr = var_type.get(var, -1)
        arr = runtime_array_of(ptr_pointee.get(ptr, -1))
        if arr < 0:
            continue  # 非 runtime-array 形态的全局（push constant / scalar），不归本护栏
        elem = runtime_elem.get(arr, -1)
        stride = stride_of.get(arr, stride_of.get(ptr, -1))
        out[binding] = (stride if stride is not None else -1, names.get(elem, f"<id {elem}>"))
    if not out:
        return {}, (f"{rel(spv)}: 没解析到任何带 Binding 装饰的 runtime-array 变量"
                    f"（探针被 DCE 了？slangc 版本行为变了？）")
    return out, ""


# ─── 主流程 ──────────────────────────────────────────────────────────────────

def run(args) -> tuple[list[str], dict[str, str]]:
    """→ (ERROR 文案列表, 汇总读数)。"""
    errs: list[str] = []
    stats: dict[str, str] = {}

    sizes, problems = contract_sizes(GENERATED_DIR)
    errs += problems
    if not sizes:
        errs.append(f"{rel(GENERATED_DIR)}/\u002a_types_generated.cppm 里一个 sizeof 都没解析到"
                    f"（正则漂了？生成物改名了？）——本护栏本次**未检查任何类型**")
        return errs, stats

    defs = struct_defining_modules(SHADERS_DIR)
    flags, why = slang_flags_from_cmake(SLANG_CMAKE)
    if why:
        errs.append(f"{why}——拿不到构建期编译口径，本护栏本次**未检查任何类型**")
        return errs, stats
    slangc, why = find_slangc(args.slangc, Path(args.build_dir) if args.build_dir else ROOT / "build")
    if why:
        errs.append(f"{why}——无法现场编探针，本护栏本次**未检查 {len(sizes)} 个契约类型**")
        return errs, stats

    # 契约类型 → 定义模块（同类型在多个模块各定义一次 = 双源，先出声）
    by_module: dict[Path, list[str]] = {}
    for t in sorted(sizes):
        where = defs.get(t, [])
        if not where:
            errs.append(f"契约类型 {t}（sizeof={sizes[t]}）在 {rel(SHADERS_DIR)} 下找不到 "
                        f"`struct {t}` 定义——反射生成物与 shader 契约脱钩，本类型**未检查**")
            continue
        if len(where) > 1:
            errs.append(f"契约类型 {t} 在 {len(where)} 个 .slang 里各定义一次："
                        f"{'、'.join(rel(p) for p in where)}——布局双源，探针取第一个")
        by_module.setdefault(where[0], []).append(t)

    checked: dict[str, int] = {}
    tmp = tempfile.TemporaryDirectory(prefix="rgl-stride-probe-")
    tmpdir = Path(args.probe_dir) if args.probe_dir else Path(tmp.name)
    tmpdir.mkdir(parents=True, exist_ok=True)

    for module_src in sorted(by_module):
        types = sorted(by_module[module_src])
        module = module_src.stem  # slang 模块名 = 文件名（去 .slang；含 .comp/.mesh 点段）
        probe = tmpdir / f"stride_probe_{module}.slang"
        probe.write_text(make_probe(module, types), encoding="utf-8")
        out_spv = tmpdir / f"stride_probe_{module}.spv"
        ok, detail = compile_probe(slangc, flags, probe, out_spv, SHADERS_DIR)
        if not ok:
            errs.append(f"探针编译失败（{rel(module_src)} 的 {len(types)} 个契约类型"
                        f"{'、'.join(types)} **未检查**）: {detail.splitlines()[0] if detail else '无输出'}")
            continue
        strides, why = strides_by_binding(out_spv)
        if why:
            errs.append(f"探针 SPV 解析失败（{rel(module_src)} 的 {len(types)} 个契约类型"
                        f"**未检查**）: {why}")
            continue
        for i, t in enumerate(types):
            got = strides.get(PROBE_BINDING_BASE + i)
            if got is None:
                errs.append(f"{rel(module_src)}: 探针 binding {PROBE_BINDING_BASE + i}"
                            f"（类型 {t}）在 SPV 里找不到——探针没生成对，**未检查**")
                continue
            stride, spv_name = got
            if stride < 0:
                errs.append(f"{rel(module_src)}: 类型 {t} 的 runtime array 没有 ArrayStride 装饰"
                            f"（{spv_name}）——**未检查**")
                continue
            want = sizes[t]
            checked[t] = stride
            if stride != want:
                errs.append(
                    f"{rel(module_src)}: `StructuredBuffer<{t}>` 的 SPIR-V ArrayStride = {stride}"
                    f" ≠ 反射生成物 sizeof({t}) = {want}（SPV 类型名 {spv_name}）"
                    f"——GPU 从元素 1 起逐元素错位读，**静默**画错（git:调试历史「灰屏」）。"
                    f"口径 flags = {' '.join(flags)}；理由与实测 → docs/constantbuffer_迁移计划.md §8")

    stats["契约类型"] = str(len(sizes))
    stats["已检查"] = str(len(checked))
    stats["未检查"] = str(len(sizes) - len(checked))
    stats["探针模块"] = str(len(by_module))
    stats["flags"] = " ".join(flags)
    uses = structured_buffer_uses(SHADERS_DIR)
    stats["shaders 内实际使用"] = str(len(uses))
    for path, lineno, elem in uses:
        if elem in sizes:
            continue  # 契约类型：探针已覆盖
        errs.append(f"{rel(path)}:{lineno}: `StructuredBuffer<{elem}>` 的元素类型不在反射契约"
                    f"类型集合内——本护栏**查不到它的 stride**（无 C++ 侧 sizeof 可对账）。"
                    f"要么让它进 schema，要么走 ByteAddressBuffer；"
                    f"理由 → docs/constantbuffer_迁移计划.md §8")
    return errs, stats


def main() -> int:
    ap = argparse.ArgumentParser(
        description="StructuredBuffer 元素 stride 对账（SPIR-V ArrayStride == 反射 sizeof）")
    ap.add_argument("--strict", action="store_true",
                    help="有问题时 exit 1（默认只警告不阻断，先例 tools/check_shader_deps.py）")
    ap.add_argument("--build-dir", default="", metavar="DIR",
                    help="构建目录（用于自动定位 slangc；CMake 传 ${CMAKE_BINARY_DIR}）")
    ap.add_argument("--slangc", default="", metavar="PATH",
                    help="slangc 路径（CMake 传 ${SLANGC_EXECUTABLE}；缺省读 RGL_SLANGC_PATH 再自动找）")
    ap.add_argument("--probe-dir", default="", metavar="DIR",
                    help="探针 .slang/.spv 落盘目录（缺省用临时目录；排查时指到构建目录看产物）")
    args = ap.parse_args()

    errs, stats = run(args)
    for msg in errs:
        print(f"\033[1;31m[stride-check-ERROR] {msg}\033[0m")
    readout = " / ".join(f"{k} {v}" for k, v in stats.items())
    print(f"[stride-check] 汇总: {len(errs)} 问题（ERROR {len(errs)}）"
          + (f" / {readout}" if readout else " / **无读数 = 本次未做任何对账**"))
    return 1 if (args.strict and errs) else 0


if __name__ == "__main__":
    sys.exit(main())
