#!/usr/bin/env python3
"""Shader DEP 表检查（批 12.3d-D1 + D3；CMake shader_dep_check 目标 / 可独立跑）。

stdlib-only（AGENTS.md 工具语言纪律：留在 Python 的工具只准 stdlib）。

## 为什么存在（GUARD 债，护栏原则：能机制化的教训不进文档）

`rgl_shader()` 的 DEP 清单是**手工登记**的；漏登一个 import 的共享模块，ninja 就
判 SPV「未过期」——改 .slang 后构建"成功"而跑的是旧着色器（stale SPV 静默残留，
git:调试历史 2026-08-27 model.slang / 2026-09-28 批 10.4 / 2026-10-01 条目）。

## 规则

R1  每个 rgl_shader target：主源的 `import x;` / `import "x.slang";` /
    `#include "x"` 的**传递闭包** ⊆（主源 + 已登记 DEP）。漏登 = ERROR，
    点名 target、漏登文件与引入链（`a.slang:3 import scene`，传递引入带全链）。
    解析不到的 import（文件不存在）同样 ERROR。
R3  红线（2026-10-01 立"全禁"；2026-10-12 按 §8.4 第 1 条**窄化**）：任何 .slang 里
    `StructuredBuffer<T>`（含 RW/Append/Consume 变体）的元素类型 T **不在反射生成契约类型
    集合内** = ERROR。契约集合机械取自 `rgl/generated/*_types_generated.cppm`（**别手抄
    名单**——抄名单反模式，副本必然漂）。禁的是**无契约的裸 struct**：git:调试历史
    「灰屏」(2026-06-24)
    的成因是元素 stride 与 C++ sizeof 静默偏离，而契约类型的 sizeof 由反射生成物
    `static_assert(sizeof == N)` 钉住、SPIR-V `ArrayStride` 由 **R4** 逐次构建对账，
    两侧同源于一份声明 ⟹ 那条静默偏离在这里**有机器可验的等式**，不再是裸奔。
    标量/向量元素（uint/int/float/…）没有 C++ 侧对偶可漂，不在射程内（§2 目标形态本身
    就用 `StructuredBuffer<uint>`）。完整理由与实测 → `docs/constantbuffer_迁移计划.md` §8。
R4  StructuredBuffer 元素 stride 对账（**R3 能窄化的前提**，同一次构建里由
    `tools/check_spirv_stride.py` 跑，见 cmake/RglChecks.cmake）：对每个契约类型断言
    SPIR-V `ArrayStride` == 反射生成物 `sizeof`。R3 管"只准用有契约的类型"，
    R4 管"有契约的类型 stride 真的对"——两条合起来才闭合。
 R5  **裸字节偏移棘轮**（批 G，2026-10-12；判据 = docs/constantbuffer_迁移计划.md §5.2）：
    对 `ByteAddressBuffer` / `RWByteAddressBuffer` 的字节偏移读/写调用
    （`Load`/`Load2`/`Load3`/`Load4`/`Store`/`Store2`/`Store3`/`Store4`，含 `Load<T>()`
    泛型形态），其**第一个实参（字节偏移所在位）里出现裸十进制字面量** = ERROR。字节偏移 =
    类型在 CPU/GPU 边界被擦除后手工再物化布局，是本项目"双源"问题的制造者
    （`sceneData.Load4(721952u)` 与 C++ 侧 `kSceneData*Offset` 各写一份，改另一边谁都不出声）。
    判据是"**偏移实参里含裸数字字面量**"，不是"函数名叫 Load"——`buf.Load<T>(i * sizeof(T))`
    与 `sceneData.Load4(base)` 是**目标形态**，不在射程内（写成后者就把合法形态全禁了）。
    两处必要的收窄：① **接收者声明类型**必须是 ByteAddressBuffer——纹理也有
    `tex.Load(int3(p, 0))` / `tex.Store(gid, v)`，按函数名判会把它们全禁了；② 只看**第一个**
    实参——`Store` 的第 2 位是写入值，`Store(o, 0u)`（清零一个槽）的 `0u` 不是偏移，
    而 `Load` 只有一个实参，同一条规则天然覆盖。
    存量走**棘轮**（机制先例 = `tools/checks/tool_baseline.py`）：基线
    `tools/checks/byte_offset_baseline.json` **按文件记条数**（不是全局一个数——否则
    「A 文件迁掉 3 条、B 文件加 3 条」互相抵消，上升被掩盖），只准减不准增；
    上升 = ERROR（处置是迁移该区，**不是**改基线），下降 = 警告并打印收账命令。
    🔴 基线文件缺失 / JSON 坏 / 格式不符 ⟹ **ERROR 并声明"本次未做任何对账"**，绝不
    静默当 0 条放行（`tool_baseline.load_baseline` 的"缺失→空表按未登记"宽松形态在这里
    会被读成"零违规" = 假绿，故 R5 比它更严）。
    已知射程外（读数在汇总行出声，别读成"0 条违规"）：① `Interlocked*` 同样吃字节偏移，
    未纳入棘轮；② 偏移字面量若**先存进变量再传参**（`const uint o = 48u; buf.Load(o)`）
    本规则看不见——棘轮数的是"调用点内联字面量"，不是"字节偏移总量"。
 R6  **scene data 全区字节读永久退役**（批 N+1，2026-10-13；判据 =
    docs/constantbuffer_迁移计划.md §2 的成功判据「偏移写不出来了」的机器形态）：
    任何 `rgl/shaders/**.slang`（剥注释后）出现 `ByteAddressBuffer sceneData` 声明或
    `sceneData.Load*/Store*` 读写 = **ERROR**（不是警告——这条没有"存量"要棘轮，命中即回归）。
    与 R5 的分工要说清，否则后来人会以为 R5 已经够了：R5 是**棘轮**（别的 ByteAddressBuffer
    允许有裸偏移，只准减），R6 是**绝对禁令**（只认 `sceneData` 这一个名字，没有基线、
    没有豁免、没有下调命令）。scene data 的**物理 buffer 仍然存在**，R6 禁的是"绕过类型化
    载体直接按字节读它"这条旧路，不是禁 buffer。
    🔴 **自检先于判据**（形态先例 = R5 的「基线不可用 ⟹ 声明本次未做任何对账」）：一条
    "什么都没找到"的规则与一条"扫错了目录"的规则**输出一模一样**。故 R6 先要求扫描面里
    确实有 `scene.slang`（sceneData 曾经的唯一声明地、也是这条规则唯一可能咬人的文件），
    看不见它 ⟹ ERROR「本次未做任何对账」，绝不把"零命中"当成"已退役"。

## 输出形态与退出码（先例 = tools/checks/main.py）

ERROR 行红粗体醒目（强制 ANSI，ninja 捕获输出亦可见），但**默认不阻断构建**
（exit 0）；`--strict` 时有问题 exit 1（手动/CI 用）。每次 build 恒执行：
custom target 无 OUTPUT 产物 = ninja 判恒过期（与 reflection_check/source_checks 同形态）。

## 副作用：新鲜度清单（给 imgqa D2 守卫）

`--emit-manifest <path>` 写出 spv→源闭包清单（每 target 一行：
`<spv 名> <源 仓库相对路径>...`，含主源）。imgqa 的 checkShadersFresh() 据此
**逐 spv** 只比 mtime（轻版新鲜度守卫，防「改了 .slang 没构建就测量」）。
图真相源在此脚本一处，imgqa 不二次解析 CMakeLists（避免双实现漂移）。

    用法：python3 tools/check_shader_deps.py [--strict] [--emit-manifest PATH]
                [--update-byte-offset-baseline]
"""

import argparse
import json
import re
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = ROOT / "rgl" / "CMakeLists.txt"
SHADERS_DIR = ROOT / "rgl" / "shaders"

# slang 内建模块（编进 slangc，非 shaders/ 下文件）：glsl = GLSL 兼容层（本构建取
# gl_FragCoord 只能 import glsl，11.5/11.6 实测），不得算「未解析边」= 误报白名单。
SLANG_BUILTIN_MODULES = {"glsl"}

# rgl_shader 调用（先剥 # 注释——CMakeLists L14 的用法示例行本身就是个"调用"形态）。
# 实参形态（见 cmake/RglSlang.cmake）：rgl_shader(<src 短名> <OUT_VAR> [<DEP 短名>...])，
# 短名相对 shaders/、不带 .slang 扩展（可含 .frag/.mesh/.comp 等点段）。
RE_RGL_SHADER = re.compile(r"\brgl_shader\s*\(([^)]*)\)")

# 注释剥离：单遍扫描（见 strip_comments）。两条正则做不到正确——「先块后行」会让行注释里
# 出现的 `/*` 被当成块注释起点（`// 口径见 configs/*.lua` 实测吞掉 taa.frag.slang 224 行
# 真代码），「先行后块」则反过来让块注释里的 `//` 吃掉收尾的 `*/`。
RE_IMPORT = re.compile(
    r"^\s*(?:public\s+)?import\s+(?:\"([^\"]+)\""
    r"|([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*))\s*;")
RE_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

# R3 的元素类型判定（剥注释后按 `StructuredBuffer<` 前缀取尖括号里的标识符）。
# 一并覆盖 StructuredBuffer / RWStructuredBuffer / AppendStructuredBuffer /
# ConsumeStructuredBuffer。
RE_STRUCTURED_BUFFER = re.compile(
    r"\b(?:RW|Append|Consume)?StructuredBuffer\s*<\s*([A-Za-z_]\w*)")
# 反射生成物 = 契约类型集合的**唯一机械来源**（R3 放行名单，不是手抄豁免清单：
# 名单随 schema 自动长，加类型的人不需要知道 R3 存在）。
GENERATED_DIR = ROOT / "rgl" / "generated"
RE_GENERATED_STRUCT = re.compile(r"^\s*export\s+struct\s+([A-Za-z_]\w*)\b", re.M)
# 标量/向量元素类型：没有 C++ struct 对偶，stride 由类型本身定死（uint→4），
# 灰屏那条静默偏离在这里**不存在发生地**。这是语言的类型词表，不是会漂的项目名单。
SCALAR_ELEMENT_TYPES = frozenset({
    "bool", "int", "uint", "dword", "float", "half",
    "bool2", "bool3", "bool4", "int2", "int3", "int4",
    "uint2", "uint3", "uint4", "float2", "float3", "float4",
    "half2", "half3", "half4",
})

# ─── R5：ByteAddressBuffer 裸字节偏移棘轮（批 G，2026-10-12）─────────────────
# 基线文件与收账命令（形态先例 = config.TOOL_BASELINE_FILE / main.py 的
# --update-tool-baseline）。路径写死在代码里 = 唯一真相源，文档只指针不抄。
BYTE_OFFSET_BASELINE_FILE = ROOT / "tools" / "checks" / "byte_offset_baseline.json"
BYTE_OFFSET_UPDATE_CMD = ("python3 tools/check_shader_deps.py "
                          "--update-byte-offset-baseline")
BYTE_OFFSET_BASELINE_COMMENT = (
    "R5 裸字节偏移棘轮基线（rgl/shaders/**：ByteAddressBuffer 的 Load*/Store* **第一个实参**"
    "（字节偏移位）含裸十进制字面量的调用点数，按文件记）。只准降不准升——上升的处置是把该区"
    "迁到类型化载体（ConstantBuffer / 反射契约 StructuredBuffer），**不是**改这里的数字。"
    f"下调后用 `{BYTE_OFFSET_UPDATE_CMD}` 重写本文件。判据与已知射程外 → "
    "docs/constantbuffer_迁移计划.md §5.2 与本脚本 docstring R5 节。")

# ByteAddressBuffer 声明（全局 / 形参 / 局部同形；`readonly` 与 `[[vk::binding()]]`
# 前缀不影响 `类型 名` 的相邻性）。一并吃 `a, b` 逗号声明形态。
RE_BYTE_ADDRESS_BUFFER = re.compile(
    r"\b(?:RW)?ByteAddressBuffer\s*\*?\s*"
    r"([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)")
# 「任意 `类型 名` 声明」探测——只为把**非** ByteAddressBuffer 的接收者排除掉：纹理也有
# `tex.Load(int3(p, 0))` / `tex.Store(gid, v)`，实参里同样有裸字面量，按名字判会全误报。
# 接收者既不在 BAB 集合、也查不到任何声明时 → 上报「无法判定」（R5 自曝盲区，不静默）。
RE_DECLARED_TYPE = re.compile(
    r"([A-Za-z_]\w*(?:\s*<[^<>]*>)?\s*\*?)\s+([A-Za-z_]\w*)\s*(?=[\[;=,)])")
# 字节偏移读/写调用点（长名在前，避免 Load4 被 Load 分支吃掉后靠回溯兜）。
RE_BAB_CALL = re.compile(
    r"\b([A-Za-z_]\w*)\s*\.\s*"
    r"(Load4|Load3|Load2|Load|Store4|Store3|Store2|Store)\b")
# 裸十进制字面量：数字且左侧不是标识符字符/点（`kVertexStride2` 的 2、`float4x4` 的 4
# 都不算；`48u` / `24` / `0x10` 都算）。
RE_BARE_DECIMAL = re.compile(r"(?<![A-Za-z0-9_.])\d")


# ─── R6：scene data 全区字节读**永久退役**（批 N+1，2026-10-13）───────────────
# 判据 = docs/constantbuffer_迁移计划.md §2 的成功判据「偏移写不出来了」的机器形态。
# 迁移期各区逐批从 `ByteAddressBuffer sceneData` + 手写字节偏移迁到类型化载体；批 N+1 删掉
# 声明本身之后，"再写回去"必须是**构建期出声**的事，而不是靠 code review 记着。
# 与 R5 的分工：R5 是**棘轮**（允许别的 ByteAddressBuffer 有裸偏移，只准减），R6 是**绝对
# 禁令**（只针对 sceneData 这一个名字，没有基线、没有豁免、没有下调）。
SCENE_DATA_BUFFER_NAME = "sceneData"
RE_SCENE_DATA_DECL = re.compile(r"\bByteAddressBuffer\s*\*?\s*sceneData\b")
RE_SCENE_DATA_READ = re.compile(r"\bsceneData\s*\.\s*(?:Load|Store)[0-9]?\b")
# R6 的自检锚：扫描面里**必须**看到 scene.slang —— 它曾是 sceneData 的唯一声明地，
# 也是这条规则唯一真正可能咬人的文件。看不见它 = 规则扫错了目录 = 静默失效。
SCENE_DATA_SELF_CHECK_FILE = "scene.slang"


def scan_scene_data_retired(texts: dict[Path, str]) -> list[tuple[Path, int, str]]:
    """R6 扫描（输入 = 已剥注释的 {文件: 文本}）→ [(文件, 行号, 命中行文本)]。

    剥注释是判据的一部分：`// sceneData.Load4(64)` 是历史散文不是调用（同 R5 的教训，
    本仓库的注释里全是这种句子，不剥 = 满屏误报 = 第二天就被加豁免名单）。
    """
    hits: list[tuple[Path, int, str]] = []
    for path, text in texts.items():
        for lineno, line in enumerate(text.splitlines(), 1):
            if RE_SCENE_DATA_DECL.search(line) or RE_SCENE_DATA_READ.search(line):
                hits.append((path, lineno, line.strip()))
    return hits


def contract_types(generated_dir: Path) -> set[str]:
    """反射生成物里的契约 struct 名（R3 的放行面）。空 = 规则失效，调用处必须出声。"""
    out: set[str] = set()
    if generated_dir.is_dir():
        for f in sorted(generated_dir.glob("*_types_generated.cppm")):
            out |= set(RE_GENERATED_STRUCT.findall(
                f.read_text(encoding="utf-8", errors="replace")))
    return out


def rel(p: Path) -> str:
    try:
        return p.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return p.as_posix()


def strip_comments(text: str) -> str:
    """剥块/行注释，**逐字符等长**（注释字符换成空格、换行原样保留）——行列偏移与源
    文件完全对齐，报错要点行、R5 还要从命中位置回切实参文本。

    为什么不是两条正则（旧实现，2026-10-12 批 G 换掉）：`//` 与 `/* */` 互相嵌套出现时
    两种先后顺序都会误吞真代码——本项目实测踩的是「先块后行」：行注释里写 `configs/*.lua`
    含 `/*`，被当成块注释起点一路吞到下一个 `*/`，taa.frag.slang 一次少扫 224 行
    （R5 变异实验拿 taa.frag.slang 对账时暴露）。字符串字面量里的 `//` 同样要跳过。
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " "
                               for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_lines(path: Path) -> list[str]:
    return strip_comments(
        path.read_text(encoding="utf-8", errors="replace")).splitlines()


def resolve_import(ref: str, quoted: bool, importer: Path) -> Path | None:
    """slang 的 import 解析：dotted 名走 include 路径（-I shaders/）找 <路径>.slang；
    带引号形态按相对本文件优先、shaders/ 兜底。解析不到 = None（R1 报 ERROR）。"""
    if quoted:
        p = importer.parent / ref
        if p.suffix == "":
            p = p.with_name(p.name + ".slang")
        if p.exists():
            return p
        p = SHADERS_DIR / ref
        return p if p.exists() else None
    p = SHADERS_DIR.joinpath(*ref.split(".")).with_suffix(".slang")
    return p if p.exists() else None


def import_refs(path: Path) -> list[tuple[str, bool, int]]:
    """一个 shader 源文件的直接 import 引语 [(ref, quoted, line)]（剥注释后）。"""
    out = []
    for lineno, line in enumerate(read_lines(path), 1):
        m = RE_IMPORT.match(line)
        if m:
            quoted, dotted = m.group(1), m.group(2)
            out.append((quoted if quoted else dotted, bool(quoted), lineno))
            continue
        m = RE_INCLUDE.match(line)
        if m:
            out.append((m.group(1), True, lineno))
    return out


def closure_of(main: Path) -> tuple[set[Path], dict[Path, tuple[Path, str, int]],
                                     list[tuple[Path, int, str]]]:
    """主源 → (闭包含主源, 文件→(引入者, 引语, 行) 用于点名, 未解析边列表)。"""
    seen = {main}
    parent: dict[Path, tuple[Path, str, int]] = {}
    unresolved: list[tuple[Path, int, str]] = []
    queue = deque([main])
    while queue:
        cur = queue.popleft()
        for ref, quoted, line in import_refs(cur):
            if not quoted and ref in SLANG_BUILTIN_MODULES:
                continue
            tgt = resolve_import(ref, quoted, cur)
            if tgt is None:
                unresolved.append((cur, line, ref))
                continue
            if tgt not in seen:
                seen.add(tgt)
                parent[tgt] = (cur, ref, line)
                queue.append(tgt)
    return seen, parent, unresolved


def chain_str(target_file: Path, f: Path, parent: dict) -> str:
    """缺失文件 → 引入链（自最近一跳起向主源拼接，仅一跳时与直接 import 同形）。"""
    parts = []
    cur = f
    while cur in parent:
        importer, ref, line = parent[cur]
        parts.append(f"{rel(importer)}:{line} import {ref}")
        cur = importer
    if not parts:
        return f"{rel(target_file)} 主源本身"
    return " ← ".join(parts)


# ─── R5：ByteAddressBuffer 裸字节偏移棘轮 ────────────────────────────────────

def _skip_ws(text: str, i: int) -> int:
    while i < len(text) and text[i].isspace():
        i += 1
    return i


def call_arg_text(text: str, i: int) -> str | None:
    """方法名末位下标 → 实参串（跳过可选 `<T>` 泛型实参）；不是调用 → None。

    括号按深度配对（`Load(min(a + 4u, b))` 里的嵌套括号不会截早）；泛型实参里遇到
    `;` / `}` 视为「这不是 `<T>`」直接放弃（防 `x.Load < y` 之类把扫描放羊）。
    """
    i = _skip_ws(text, i)
    if i < len(text) and text[i] == "<":
        depth = 0
        while i < len(text):
            c = text[i]
            if c in ";}":
                return None
            if c == "<":
                depth += 1
            elif c == ">":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            i += 1
        else:
            return None
        i = _skip_ws(text, i)
    if i >= len(text) or text[i] != "(":
        return None
    depth, start = 0, i
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return None


def first_top_level_arg(args: str) -> str:
    """实参串 → **第一个**实参（字节偏移所在位）。顶层逗号才分参数，
    `Store(min(a, 4u), 0u)` 的第一个实参是 `min(a, 4u)`。

    为什么只看第一个实参：`Store` 的字节偏移在第 1 位，第 2 位是**写入值**——
    `buf.Store(o, 0u)`（清零一个槽）里那个 `0u` 与"手写字节偏移"毫无关系，按整串
    实参判就是误报。`Load` 只有一个实参，同一条规则天然覆盖。
    """
    depth = 0
    for i, c in enumerate(args):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            return args[:i]
    return args


def scan_byte_offsets(texts: dict[Path, str]) -> tuple[dict[Path, list[tuple[int, str]]],
                                                       set[str]]:
    """R5 扫描（输入 = 已剥注释的 {文件: 文本}）。

    两步判据：**① 接收者是 ByteAddressBuffer**（纹理的 `tex.Load(int3(p, 0))` 实参里
    也有裸字面量，不按类型过滤 = 大量误报）；**② 第一个实参（字节偏移所在位）含裸十进制
    字面量**（`Store` 的第 2 个实参是写入值，`Store(o, 0u)` 的 `0u` 不是偏移，见
    `first_top_level_arg`）。
    返回 ({文件: [(行号, 偏移实参串)]}, 无法判定类型的接收者名集合)。

    接收者类型取自**全体 .slang 的并集**而非单文件：shader 全局量经 `import` 跨文件可见
    （`scene.slang` 声明 `sceneData`，`light.frag.slang` 里读它），按单文件判会漏掉
    全部跨文件读取。代价 = 同名跨类型会误判成 BAB（当前无此碰撞；且误判方向是**误报**，
    吵在明处，比静默漏检可接受）。
    """
    bab_names: set[str] = set()
    declared: set[str] = set()
    for text in texts.values():
        for m in RE_BYTE_ADDRESS_BUFFER.finditer(text):
            bab_names |= {n for n in re.split(r"\s*,\s*", m.group(1)) if n}
        declared |= {m.group(2) for m in RE_DECLARED_TYPE.finditer(text)}

    hits: dict[Path, list[tuple[int, str]]] = {}
    unknown: set[str] = set()
    for path, text in texts.items():
        for m in RE_BAB_CALL.finditer(text):
            name = m.group(1)
            if name in bab_names:
                pass
            elif name in declared:
                continue          # 纹理 / 其它类型的 Load/Store：不在射程内
            else:
                unknown.add(name)  # R5 自曝盲区：判不出类型 → 不静默跳过
                continue
            args = call_arg_text(text, m.end())
            if args is None:
                continue
            offset = first_top_level_arg(args)
            if not RE_BARE_DECIMAL.search(offset):
                continue
            hits.setdefault(path, []).append(
                (text.count("\n", 0, m.start()) + 1, " ".join(offset.split())))
    return hits, unknown


def load_byte_offset_baseline(path: Path) -> tuple[dict[str, int], str | None]:
    """读 R5 基线 → ({rel: 允许条数}, 不可用原因)。

    🔴 与 `tool_baseline.load_baseline`（缺失/坏 → 空表按未登记）**故意不同**：R5 的
    空表会被下游读成「零违规」，那是"失效不出声 = 假绿"的教科书形态。这里缺失 / 坏 JSON /
    格式不符一律返回原因串，调用处必须 ERROR 出声并声明本次未对账。
    """
    if not path.is_file():
        return {}, f"基线文件不存在: {rel(path)}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {}, f"基线文件读不出来: {rel(path)}（{type(e).__name__}: {e}）"
    if not isinstance(data, dict):
        return {}, f"基线顶层不是 JSON 对象: {rel(path)}"
    files = data.get("files")
    if not isinstance(files, dict):
        return {}, f"基线缺 `files` 对象（格式不符）: {rel(path)}"
    bad = sorted(str(k) for k, v in files.items()
                 if isinstance(v, bool) or not isinstance(v, int))
    if bad:
        return {}, f"基线 `files` 有非整数条数（格式不符）: {rel(path)} 的 {bad[:3]}"
    return {str(k): int(v) for k, v in files.items()}, None


def write_byte_offset_baseline(counts: dict[str, int], path: Path) -> str:
    """按当前条数重写 R5 基线（收账）；返回写入路径。"""
    files = {k: int(v) for k, v in sorted(counts.items())}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(
        {"_comment": BYTE_OFFSET_BASELINE_COMMENT,
         "total": sum(files.values()), "files": files},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return rel(path)


def compare_byte_offsets(counts: dict[str, int], baseline: dict[str, int]
                         ) -> tuple[list[tuple[str, str]], list[tuple[str, str]],
                                    int, int]:
    """逐文件棘轮比对 → (ERROR 行, WARNING 行, 当前总条数, 基线总条数)。

    当前 > 基线（或不在基线里且 > 0）= ERROR；当前 < 基线 = WARNING + 收账命令。
    总数只统计**磁盘上还在**的文件（已删除文件的基线条数原样保留，等收账时一并归零，
    否则一次误删就把基线悄悄清零）。
    """
    errs: list[tuple[str, str]] = []
    warns: list[tuple[str, str]] = []
    cur_total = base_total = 0
    for name in sorted(set(counts) | set(baseline)):
        cur, base = counts.get(name, 0), baseline.get(name, 0)
        if name not in counts:
            if base > 0:
                warns.append((name,
                    f"R5 基线仍记 {base} 条但该文件已不在磁盘上（或已无任何命中）——"
                    f"用 `{BYTE_OFFSET_UPDATE_CMD}` 收账"))
            continue
        cur_total += cur
        base_total += base
        if name not in baseline:
            if cur > 0:
                errs.append((name,
                    f"R5 裸字节偏移基线未登记：当前 {cur} 条（新文件必须显式登记进 "
                    f"{rel(BYTE_OFFSET_BASELINE_FILE)}——但**先确认**这些偏移本该存在，"
                    f"新增裸偏移的处置是走类型化载体，不是登记额度）"))
            continue
        if cur > base:
            errs.append((name,
                f"R5 裸字节偏移基线上升 {cur} > {base}——处置是把该区迁到类型化载体"
                f"（ConstantBuffer / 反射契约 StructuredBuffer），**不是**改基线"
                f"（基线只准降；确实降了才用 `{BYTE_OFFSET_UPDATE_CMD}` 收账）"))
        elif cur < base:
            warns.append((name,
                f"R5 裸字节偏移可下调 {cur} < {base}——用 `{BYTE_OFFSET_UPDATE_CMD}` "
                f"收账（悄悄漂移等于没有基线）"))
    return errs, warns, cur_total, base_total


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Shader DEP 表检查（import 闭包 ⊆ rgl_shader DEP + StructuredBuffer "
                    "元素类型须走反射契约）")
    ap.add_argument("--strict", action="store_true",
                    help="有问题时 exit 1（默认只警告不阻断，先例 tools/checks/main.py）")
    ap.add_argument("--emit-manifest", default=None, metavar="PATH",
                    help="写出 spv→源闭包清单（供 imgqa checkShadersFresh 逐 spv 比 mtime）")
    ap.add_argument("--update-byte-offset-baseline", action="store_true",
                    help="用当前 R5 裸字节偏移条数重写 byte_offset_baseline.json"
                         "（迁移掉一批偏移后收账用；基线只准降，上升的处置是迁移不是改这里）")
    args = ap.parse_args()

    errors: list[tuple[str, str]] = []
    warnings: list[tuple[str, str]] = []

    def err(loc: str, msg: str) -> None:
        errors.append((loc, msg))

    def warn(loc: str, msg: str) -> None:
        warnings.append((loc, msg))

    if not CMAKE_FILE.exists():
        print(f"\033[1;31m[shader-dep-check-ERROR] {rel(CMAKE_FILE)}: 找不到 CMakeLists，"
              f"本检查未执行（规则失效必须出声）\033[0m")
        return 1
    if not SHADERS_DIR.is_dir():
        print(f"\033[1;31m[shader-dep-check-ERROR] {rel(SHADERS_DIR)}: shaders 目录不存在，"
              f"本检查未执行\033[0m")
        return 1

    # ── R1：rgl_shader target 的 import 闭包 ⊆ 主源 + DEP ──────────────────
    text = re.sub(r"#[^\n]*", "", CMAKE_FILE.read_text(encoding="utf-8"))
    calls = [m for m in RE_RGL_SHADER.finditer(text)]
    manifest_rows: list[tuple[str, set[Path]]] = []
    for m in calls:
        where = f"{rel(CMAKE_FILE)}:{text.count(chr(10), 0, m.start()) + 1}"
        raw = m.group(1).split()
        if len(raw) < 2:
            err(where, f"rgl_shader 调用参数不足（需 <src> <OUT_VAR>）: ({' '.join(raw)})")
            continue
        src, deps = raw[0], raw[2:]
        main_src = SHADERS_DIR / f"{src}.slang"
        if not main_src.exists():
            err(where, f"rgl_shader({src}) 主源不存在: {rel(main_src)}")
            continue
        allowed = {main_src}
        for d in deps:
            dep = SHADERS_DIR / f"{d}.slang"
            if not dep.exists():
                err(where, f"rgl_shader({src}) 登记了不存在的 DEP: {rel(dep)}")
                continue
            allowed.add(dep)
        files, parent, unresolved = closure_of(main_src)
        for importer, line, ref in unresolved:
            err(f"{rel(importer)}:{line}",
                f"rgl_shader({src}) 的 import 无法解析: '{ref}'（shaders/ 下无对应 .slang）")
        for f in sorted(files - allowed):
            err(rel(main_src),
                f"rgl_shader({src}) DEP 漏登 {rel(f)}"
                f"（由 {chain_str(main_src, f, parent)}——改该文件不会触发本 SPV 重编译）")
        manifest_rows.append((src + ".spv", files))

    # ── R3：StructuredBuffer 元素类型必须是反射契约类型（2026-10-12 窄化，见 §8）──
    slang_files = sorted(SHADERS_DIR.rglob("*.slang"))
    contracts = contract_types(GENERATED_DIR)
    if not contracts:
        err(rel(GENERATED_DIR),
            "反射生成物里一个契约 struct 都没解析到（生成物被清空？正则漂了？）——"
            "R3 的放行面为空，本次**任何 StructuredBuffer 都会被误判成违规**，"
            "规则已不可信，请勿把「零警告」读成「干净」")
    sb_uses: list[tuple[Path, int, str]] = []
    for p in slang_files:
        for lineno, line in enumerate(read_lines(p), 1):
            for m in RE_STRUCTURED_BUFFER.finditer(line):
                elem = m.group(1)
                sb_uses.append((p, lineno, elem))
                if elem in SCALAR_ELEMENT_TYPES or elem in contracts:
                    continue
                err(f"{rel(p)}:{lineno}",
                    f"`StructuredBuffer<{elem}>` 的元素类型 {elem} 不在反射契约类型集合内"
                    f"（集合 = {rel(GENERATED_DIR)}/*_types_generated.cppm 机械提取，"
                    f"现 {len(contracts)} 个）——无契约的裸 struct 正是 stride 静默错位的"
                    f"发生地（git:调试历史「灰屏」(2026-06-24)）。要么把 {elem} 进 schema 走反射，"
                    f"要么走 ByteAddressBuffer + .Load()；理由与实测 → "
                    f"docs/constantbuffer_迁移计划.md §8")

    # ── R5：ByteAddressBuffer 裸字节偏移棘轮（批 G，判据 §5.2）────────────────
    # 扫描面复用 R3 的 slang_files；注释必须先剥（`// Load4(64)` 是散文不是调用，
    # 不剥 = 大量误报，见 strip_comments：块注释按换行回填，行号仍与源文件一致）。
    shader_texts = {p: strip_comments(p.read_text(encoding="utf-8", errors="replace"))
                    for p in slang_files}
    bo_hits, bo_unknown = scan_byte_offsets(shader_texts)
    bo_counts = {rel(p): len(bo_hits.get(p, [])) for p in slang_files}
    bo_now = sum(bo_counts.values())
    if args.update_byte_offset_baseline:
        print(f"[shader-dep-check] R5 基线已重写: {bo_now} 条 / "
              f"{sum(1 for v in bo_counts.values() if v)} 个文件 → "
              f"{write_byte_offset_baseline(bo_counts, BYTE_OFFSET_BASELINE_FILE)}")
    bo_baseline, bo_bad = load_byte_offset_baseline(BYTE_OFFSET_BASELINE_FILE)
    if bo_bad:
        err(rel(BYTE_OFFSET_BASELINE_FILE),
            f"R5 基线不可用（{bo_bad}）——本次**未做任何对账**，磁盘上现 {bo_now} 条裸字节"
            f"偏移**一条都没被检查**；请勿把「零警告」读成「干净」。基线是已提交产物："
            f"恢复它，而不是删掉规则")
        bo_allowed = -1
    else:
        bo_errs, bo_warns, bo_cur, bo_allowed = compare_byte_offsets(
            bo_counts, bo_baseline)
        errors.extend(bo_errs)
        warnings.extend(bo_warns)
    for name in sorted(bo_unknown):
        warn(f"{rel(SHADERS_DIR)}",
             f"R5 判不出 `{name}` 的类型（既不是 ByteAddressBuffer，也查不到任何声明）——"
             f"若它其实是 ByteAddressBuffer，**它的裸偏移本次漏检了**（R5 自曝盲区）")

    # ── R6：scene data 全区字节读永久退役（批 N+1，判据 §2「偏移写不出来了」）────
    # 🔴 自检先于判据（形态先例 = R5 的「基线不可用 ⟹ 声明本次未做任何对账」）：
    # 一条「什么都没找到」的规则，与一条「扫错了目录」的规则，输出**一模一样**。所以先证明
    # 扫描面里确实有 scene.slang（sceneData 曾经的唯一声明地），再谈"零命中 = 干净"。
    sd_scanned = {p.name for p in shader_texts}
    if SCENE_DATA_SELF_CHECK_FILE not in sd_scanned:
        sd_ok = False
        err(f"{rel(SHADERS_DIR)}",
            f"R6 本次**未做任何对账**：扫描面（现 {len(shader_texts)} 个 .slang）里没有 "
            f"{SCENE_DATA_SELF_CHECK_FILE}——sceneData 曾经的唯一声明地不在射程内，"
            f"「零命中」是扫错目录的产物，不是干净；请勿把「零警告」读成「已退役」")
    else:
        sd_ok = True
        for path, lineno, line in scan_scene_data_retired(shader_texts):
            err(f"{rel(path)}:{lineno}",
                f"R6 禁止 scene data 全区字节读：`{SCENE_DATA_BUFFER_NAME}` 声明/读写已随批 N+1 "
                f"永久退役（各区走类型化子范围载体，binding 表见 scene.slang 头注释）。"
                f"要加新区 → 加契约 struct + schema 登记 + SceneDataService 子范围槽，"
                f"**不是**把字节读写回来。命中 = `{line}`")
    if sd_ok:
        sd_hits = len(scan_scene_data_retired(shader_texts))
    else:
        sd_hits = -1

    for loc, msg in errors:
        print(f"\033[1;31m[shader-dep-check-ERROR] {loc}: {msg}\033[0m")
    for loc, msg in warnings:
        print(f"\033[1;33m[shader-dep-check-WARN] {loc}: {msg}\033[0m")
    print(f"[shader-dep-check] 汇总: {len(errors)} 问题（ERROR {len(errors)}、"
          f"WARNING {len(warnings)}） / "
          f"{len(calls)} 个 rgl_shader target、{len(slang_files)} 个 .slang、"
          f"StructuredBuffer 使用 {len(sb_uses)} 处（契约类型 {len(contracts)} 个放行面）、"
          f"R5 裸字节偏移 "
          f"{'**无读数 = 本次未做任何对账**' if bo_allowed < 0 else f'当前 {bo_cur} 条 / 基线 {bo_allowed} 条（{len(bo_counts)} 个 .slang 参与）'}、"
          f"R6 sceneData 全区读 "
          f"{'**本次未做任何对账**' if sd_hits < 0 else f'{sd_hits} 处命中（必须 0）'}）")

    if args.emit_manifest and manifest_rows:
        out = Path(args.emit_manifest)
        out.parent.mkdir(parents=True, exist_ok=True)
        lines = [
            "# shader spv→源闭包清单 —— tools/check_shader_deps.py 每次构建生成，勿手改。",
            "# 格式: <spv 名> <源 仓库相对路径>...（含主源，行内含全部传递 import；",
            "#       供 imgqa checkShadersFresh 逐 spv 比 mtime 防 stale SPV）。",
        ]
        for spv_name, files in manifest_rows:
            lines.append(spv_name + " " + " ".join(rel(f) for f in sorted(files)))
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    return 1 if (args.strict and errors) else 0


if __name__ == "__main__":
    sys.exit(main())
