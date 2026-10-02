#!/usr/bin/env python3
"""Shader DEP 表检查（批 12.3d-D1 + D3；CMake shader_dep_check 目标 / 可独立跑）。

stdlib-only（AGENTS.md 工具语言纪律：留在 Python 的工具只准 stdlib）。

## 为什么存在（GUARD 债，护栏原则：能机制化的教训不进文档）

`rgl_shader()` 的 DEP 清单是**手工登记**的；漏登一个 import 的共享模块，ninja 就
判 SPV「未过期」——改 .slang 后构建"成功"而跑的是旧着色器（stale SPV 静默残留，
调试历史 2026-08-27 model.slang / 2026-09-28 批 10.4 / 2026-10-01 条目）。

## 规则

R1  每个 rgl_shader target：主源的 `import x;` / `import "x.slang";` /
    `#include "x"` 的**传递闭包** ⊆（主源 + 已登记 DEP）。漏登 = ERROR，
    点名 target、漏登文件与引入链（`a.slang:3 import scene`，传递引入带全链）。
    解析不到的 import（文件不存在）同样 ERROR。
R3  红线（D3 裁决 2026-10-01）：任何 .slang 出现 `StructuredBuffer`
    （含 RW/Append/Consume 变体；剥注释后子串匹配）= ERROR。项目全面禁用而非
    只禁顶点/索引路径——调试历史「灰屏」：stride ≥16B 对齐与 C++ sizeof **静默**
    偏离；现行约定 ByteAddressBuffer + 手动 .Load()。全库现零使用，故规则不需要
    豁免清单（需要会漂移的豁免清单的规则宁缺毋滥——这里恰好不需要）。

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

# D3 红线 token：子串匹配，一并覆盖 StructuredBuffer / RWStructuredBuffer /
# AppendStructuredBuffer / ConsumeStructuredBuffer（剥注释后）。
BANNED_TOKEN = "StructuredBuffer"


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
        description="Shader DEP 表检查（import 闭包 ⊆ rgl_shader DEP + StructuredBuffer 红线）")
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

    # ── R3：StructuredBuffer 红线（D3 裁决：全库零使用 → 全禁、无豁免清单）──
    slang_files = sorted(SHADERS_DIR.rglob("*.slang"))
    for p in slang_files:
        for lineno, line in enumerate(read_lines(p), 1):
            if BANNED_TOKEN in line:
                err(f"{rel(p)}:{lineno}",
                    f"禁用 {BANNED_TOKEN}（含 RW/Append/Consume 变体）：stride ≥16B 对齐与 "
                    f"C++ sizeof 静默偏离（调试历史「灰屏」）——数据通路统一 "
                    f"ByteAddressBuffer + .Load()")

    for loc, msg in errors:
        print(f"\033[1;31m[shader-dep-check-ERROR] {loc}: {msg}\033[0m")
    print(f"[shader-dep-check] 汇总: {len(errors)} 问题（ERROR {len(errors)}） / "
          f"{len(calls)} 个 rgl_shader target、{len(slang_files)} 个 .slang")

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
