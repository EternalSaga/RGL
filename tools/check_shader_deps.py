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
"""

import argparse
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

RE_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
RE_LINE_COMMENT = re.compile(r"[ \t]*//[^\n]*")
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
    """剥块/行注释；块注释按换行数回填，保持行号与源文件一致（报错要点行）。"""
    text = RE_BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return RE_LINE_COMMENT.sub("", text)


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


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Shader DEP 表检查（import 闭包 ⊆ rgl_shader DEP + StructuredBuffer "
                    "元素类型须走反射契约）")
    ap.add_argument("--strict", action="store_true",
                    help="有问题时 exit 1（默认只警告不阻断，先例 tools/checks/main.py）")
    ap.add_argument("--emit-manifest", default=None, metavar="PATH",
                    help="写出 spv→源闭包清单（供 imgqa checkShadersFresh 逐 spv 比 mtime）")
    args = ap.parse_args()

    errors: list[tuple[str, str]] = []

    def err(loc: str, msg: str) -> None:
        errors.append((loc, msg))

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

    for loc, msg in errors:
        print(f"\033[1;31m[shader-dep-check-ERROR] {loc}: {msg}\033[0m")
    print(f"[shader-dep-check] 汇总: {len(errors)} 问题（ERROR {len(errors)}） / "
          f"{len(calls)} 个 rgl_shader target、{len(slang_files)} 个 .slang、"
          f"StructuredBuffer 使用 {len(sb_uses)} 处（契约类型 {len(contracts)} 个放行面）")

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
