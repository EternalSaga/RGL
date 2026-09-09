"""Doxygen 注释检查（CMake doc_check 目标 / just doc-check）。

设计要点：
- 复用 build/compile_commands.json 里每个编译单元的精确命令（含 C++20 modules 的
  -fmodule-file BMI 依赖），剥离 -c/-o/-fmodule-output（防覆盖用户构建目录的 BMI），
  改指 scratch 目录 + -fsyntax-only -Xclang -ast-dump。机制与姊妹项目
  modern_vulkan_tutorial 的 tools/explore/rgl_env.py 同源（已全单元验证）。
- 纯 Python 标准库（json/re/hashlib/shlex/subprocess/os/sys/concurrent.futures），
  零 pip 依赖；clang 取构建链现成 LLVM，AST 文本解析用同目录 astparse.py
  （教学项目 vendored + 行号跟踪扩展）。
- 结果缓存 {build}/doccheck_cache（key = sha256(源字节+命令)）：增量构建只重 dump
  改动的 .cppm（每文件 ~2-10s），全量 ~1min，与编译并行不占关键路径。
- 检查规则：声明首行（AST loc 范围起点，含返回类型行）正上方须紧邻非空
  /** ... */ 块（默认不允许空行间隔，匹配项目注释规范；--max-gap 放宽）。
- 只警告不阻断（exit 0）；--strict 有警告 exit 1。

用法：
    python3 tools/doccheck/doccheck.py [--build-dir build] [--methods]
                                       [--max-gap N] [--strict]
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import astparse  # noqa: E402

# 检查范围：rgl/ 下全部 .cppm（含 _impl 单元）；排除生成物与纯 re-export 壳。
EXCLUDE_SUBDIRS = ("/rgl/generated/",)
EXCLUDE_FILES = ("rgl/rlvk.cppm",)


class Env:
    def __init__(self, rgl_root, build_dir):
        self.root = os.path.abspath(rgl_root)
        self.build = os.path.abspath(build_dir)
        self.cache_dir = os.path.join(self.build, "doccheck_cache")
        self.scratch_dir = os.path.join(self.build, "doccheck_scratch")
        db = os.path.join(self.build, "compile_commands.json")
        if not os.path.exists(db):
            raise SystemExit(
                f"error: 找不到 {db}\n  检查器复用 RGL 构建的编译命令（modules 依赖在里面）。\n"
                "  请先 just config && just build。")
        self.db = json.load(open(db))
        self._by_file = {}
        for e in self.db:
            self._by_file[os.path.abspath(os.path.join(e["directory"], e["file"]))] = e

    def cppm_units(self):
        out = []
        for full in sorted(self._by_file):
            if not full.endswith(".cppm"):
                continue
            norm = full.replace(os.sep, "/")
            if "/rgl/" not in norm:
                continue
            if any(x in norm for x in EXCLUDE_SUBDIRS):
                continue
            rel = os.path.relpath(full, self.root).replace(os.sep, "/")
            if rel in EXCLUDE_FILES:
                continue
            out.append(rel)
        return out

    def entry_for(self, rel):
        full = os.path.join(self.root, rel)
        e = self._by_file.get(full)
        if e is None:
            e = next((v for k, v in self._by_file.items() if k.replace(os.sep, "/").endswith("/" + rel)), None)
        if e is None:
            raise SystemExit(f"error: compile_commands.json 里没有 {rel}")
        return e

    def _dump_cmd(self, entry, rel):
        """剥 -c/-o/-fmodule-output（防写用户 build/），换文本 AST dump。"""
        clean, skip = [], False
        for a in shlex.split(entry["command"], posix=os.name != "nt"):
            if skip:
                skip = False
                continue
            if a == "-c" or a.startswith("-fmodule-output="):
                continue
            if a == "-o":
                skip = True
                continue
            clean.append(a)
        os.makedirs(self.scratch_dir, exist_ok=True)
        clean += [
            "-fmodule-output=" + os.path.join(self.scratch_dir, rel.replace("/", "_") + ".pcm"),
            "-fsyntax-only",
            "-Xclang", "-ast-dump",
        ]
        return clean

    def read(self, rel):
        with open(os.path.join(self.root, rel), encoding="utf-8") as f:
            return f.read()

    def ast_text(self, rel):
        """接口单元的文本 AST dump（按 源字节+命令 缓存）。"""
        entry = self.entry_for(rel)
        src = open(os.path.join(self.root, rel), "rb").read()
        cmd = self._dump_cmd(entry, rel)
        key = hashlib.sha256(
            hashlib.sha256(src).hexdigest().encode()
            + json.dumps(cmd, sort_keys=True).encode()
        ).hexdigest()[:24]
        os.makedirs(self.cache_dir, exist_ok=True)
        cache = os.path.join(self.cache_dir, key + ".txt")
        if os.path.exists(cache):
            return open(cache, encoding="utf-8").read()
        proc = subprocess.run(cmd, cwd=entry["directory"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"clang AST dump 失败 rc={proc.returncode}: {rel}\n{proc.stderr[:300]}")
        with open(cache, "w", encoding="utf-8") as f:
            f.write(proc.stdout)
        return proc.stdout


def module_name_of(src):
    m = re.search(r"^\s*(?:export\s+)?module\s+([\w.:]+)\s*;", src, re.M)
    if not m:
        raise SystemExit("error: 源文件里找不到模块声明行")
    return m.group(1)


def _unstar(t):
    """去掉 Doxygen 续行前导 * 后取内容。"""
    if t.startswith("*"):
        t = t[1:]
    return t.strip()


def _block_has_content(lines, start, end):
    """lines[start]=/** 行、lines[end]=块末行：是否有非空注释内容。

    末行两种形态：单独 */ 或 `* 文字 */`（*/ 尾随）；首行 `/** 文字` 同理。
    """
    ts = lines[start].strip()
    if ts.startswith("/**") and len(ts) > 3:
        return True
    te = lines[end].strip()
    if te.endswith("*/") and te != "*/" and _unstar(te[:-2]):
        return True
    for i in range(start + 1, end):
        t = lines[i].strip()
        if t != "*" and _unstar(t):
            return True
    return False


def _is_fwd_line(t, name):
    """同名实体的前向声明行（`export class X;` / 函数原型）——前向声明与
    定义之间可夹空行，注释块挂在前向声明上方（Renderer 先例）。"""
    if not t.endswith(";"):
        return False
    m = re.match(r"^(?:export\s+)?(?:class|struct|enum|union)\s+(\w+)\b", t)
    if m:
        return m.group(1) == name
    return bool(re.search(r"\b%s\s*\(" % re.escape(name), t))


def has_doxygen_above(lines, start_line, max_gap=0, name=None):
    """声明首行（1-based）正上方是否有非空 /** ... */ 块。

    规则：向上可跳过空行与同名前向声明行（合计至多 max_gap+2 行，
    覆盖"注释→前向声明→空行→定义"形态）；块首行须以 /** 起始
    （多行块末行为单独 */ 或 `* 文字 */`，或单行 /** ... */）；块内容非空。
    """
    i = start_line - 2
    skipped = 0
    budget = max_gap + 2
    while i >= 0:
        t = lines[i].strip()
        if t == "" or (name and _is_fwd_line(t, name)):
            skipped += 1
            if skipped > budget:
                return False
            i -= 1
            continue
        break
    if i < 0:
        return False
    s = lines[i].strip()
    if s == "*/" or (s.endswith("*/") and s.startswith("*")):
        j = i - 1
        while j >= 0:
            t = lines[j].strip()
            if t == "/**" or t.startswith("/**"):
                return _block_has_content(lines, j, i)
            j -= 1
        return False
    if s.startswith("/**") and s.endswith("*/"):
        return len(s) > len("/** */")
    return False


def iter_decls(unit, include_methods):
    """yield (kind, name, line1)。records 含嵌套类型；enums 含类内嵌。"""
    for r in unit["records"]:
        yield ("class", r["name"], r["line"])
        for ne in r["enums"]:
            yield ("enum", ne["name"], ne["line"])
        for n in r["nested"]:
            if n["kind"] in ("class", "struct", "union"):
                yield ("class", n["name"], n["line"])
            else:
                yield ("function", n["name"], n["line"])
        if include_methods:
            for m in r["methods"]:
                yield ("method", m["name"], m["line"])
    for e in unit["enums"]:
        yield ("enum", e["name"], e["line"])
    for f in unit["functions"]:
        yield ("function", f["name"], f["line"])


def declared_names_in(env, rel):
    """接口单元声明名集合（自由函数 + 类方法名）——impl 定义处跨查依据。

    注释单一真相源 = 接口声明处；impl 单元里接口已声明函数的定义
    不重复要求 /** 块（名字级匹配，同名 impl 本地函数会被误豁免——罕见，
    属可接受启发式）。
    """
    src = env.read(rel)
    unit = astparse.extract_unit(
        astparse.parse_ast_text(env.ast_text(rel), os.path.basename(rel),
                                module_name_of(src), prefilter=True)[0])
    names = set()
    for r in unit["records"]:
        for m in r["methods"]:
            names.add(m["name"])
    for f in unit["functions"]:
        names.add(f["name"])
    return names


def check_file(env, rel, include_methods, skip_names=None):
    """返回 (warnings, checked, unverified, skipped_iface)。"""
    src = env.read(rel)
    lines = src.splitlines()
    unit = astparse.extract_unit(
        astparse.parse_ast_text(env.ast_text(rel), os.path.basename(rel),
                                module_name_of(src), prefilter=True)[0])
    decls = [(k, n, l) for k, n, l in iter_decls(unit, include_methods)
             if n and 1 <= l <= len(lines)]
    # 重载容忍（Doxygen 惯例：一组重载共用最上方一个注释块）：
    # 同名相邻（≤4 行）上方声明有注释 → 本条视为已覆盖
    name_lines = {}
    for k, n, l in decls:
        if k in ("function", "method"):
            name_lines.setdefault(n, []).append(l)
    warnings, checked, unverified, skipped = [], 0, 0, 0
    for kind, name, line in decls:
        if (skip_names is not None and kind in ("function", "method")
                and name in skip_names):
            skipped += 1
            continue
        # 行号健全性：名字须出现在声明首行起的 3 行窗口内（多行声明容忍）
        window = " ".join(lines[line - 1:line + 2])
        if name not in window:
            unverified += 1
            continue
        checked += 1
        if has_doxygen_above(lines, line, max_gap=args.max_gap, name=name):
            continue
        covered = any(
            0 < line - other <= 4
            and has_doxygen_above(lines, other, max_gap=args.max_gap, name=name)
            for other in name_lines.get(name, []))
        if not covered:
            warnings.append((line, kind, name))
    return warnings, checked, unverified, skipped


_parser = argparse.ArgumentParser(description="RGL Doxygen 注释检查")
_parser.add_argument("--build-dir", default=None,
                     help="build 目录（默认 <repo>/build）")
_parser.add_argument("--methods", action="store_true",
                     help="同时检查成员函数（默认只查类/枚举/自由函数）")
_parser.add_argument("--max-gap", type=int, default=0,
                     help="注释块与声明之间允许的最大空行数（默认 0 = 紧贴）")
_parser.add_argument("--strict", action="store_true",
                     help="有警告时 exit 1（默认只警告不阻断）")
args = _parser.parse_args()
args.build_dir = os.path.abspath(args.build_dir) if args.build_dir else \
    os.path.join(os.path.dirname(HERE), "..", "build")
root = os.path.abspath(os.path.join(HERE, "..", ".."))

if __name__ == "__main__":
    env = Env(root, args.build_dir)
    units = env.cppm_units()
    ifaces = [u for u in units if not u.endswith("_impl.cppm")]
    all_warnings, total_checked, total_unverified, total_skipped = [], 0, 0, 0
    errors = []
    workers = os.cpu_count() or 4

    # 第一遍：接口单元声明名表（impl 定义处跨查用）
    iface_names = {}

    def iface_worker(rel):
        try:
            return rel, declared_names_in(env, rel), None
        except Exception as ex:
            return rel, set(), str(ex)

    def unit_worker(rel):
        try:
            skip = None
            if rel.endswith("_impl.cppm"):
                base = rel[: -len("_impl.cppm")] + ".cppm"
                skip = iface_names.get(base)
            return rel, *check_file(env, rel, args.methods, skip_names=skip), None
        except Exception as ex:  # 单文件失败不阻断（警告语义）
            return rel, [], 0, 0, 0, str(ex)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for rel, names, err in pool.map(iface_worker, ifaces):
            if err is not None:
                errors.append((rel, "接口名表: " + err))
            else:
                iface_names[rel] = names

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for rel, warns, checked, unverified, skipped, err in pool.map(unit_worker, units):
            if err is not None:
                errors.append((rel, err))
                continue
            all_warnings.extend((rel, w) for w in warns)
            total_checked += checked
            total_unverified += unverified
            total_skipped += skipped

    for rel, (line, kind, name) in sorted(all_warnings):
        label = {"class": "class", "enum": "enum", "function": "function",
                 "method": "method"}.get(kind, kind)
        print(f"[doc-check] {rel}:{line}: {label} '{name}' 缺 Doxygen 注释"
              f"（声明正上方需 /** ... */ 块）")
    for rel, err in errors:
        print(f"[doc-check] {rel}: 检查失败（跳过）: {err.splitlines()[0]}")
    print(f"[doc-check] 汇总: {len(all_warnings)} 警告 / {total_checked} 声明已检查"
          f"（{total_unverified} 行号未验证、{total_skipped} 接口已声明跳过），"
          f"{len(units)} 个 .cppm")
    sys.exit(1 if (args.strict and all_warnings) else 0)
