"""文件/函数结构检查（CMake style_check 目标 / just style-check）。

三项阈值（代码行 = 去除注释与空行，token 级精确）：
- 文件代码行 > --file-threshold（默认 500）→ 提示拆分文件
- 函数代码行 > --func-threshold（默认 120）→ 提示拆分函数
- 函数嵌套深度 > --nesting-threshold（默认 5）→ 提示拆分函数
  （嵌套 = if/for/while/do/switch/catch/lambda 各 +1 层，函数体本身 0 层）

设计要点：
- 与 doccheck 同源：复用 build/compile_commands.json 的编译命令（含 C++20 modules 的
  .modmap BMI 依赖），libclang（ctypes）进程内解析，纯 Python 标准库零 pip 依赖。
- 坑（本机实测，勿回退）：
  1. libclang C API 不展开 @response file——模块参数全藏在 *.modmap 里，须内联展开
     （clang CLI 自动展开，doccheck 的 -ast-dump 路径不受影响）；
  2. 输入文件与驱动路径不能混进 argv（文件走 source_filename 参数；混入 → parse 返回 NULL）；
  3. Gentoo 布局 resource dir 在 /usr/lib/clang/22，与 libclang 默认推导不一致——
     须显式 -resource-dir（否则 stddef.h not found、AST 残缺）；
  4. `export module` + import + BMI 加载失败（缺失/标准不匹配）→ parse 返回 NULL 且
     无诊断（libclang 缺陷）——按"跳过该文件 + 警告"处理（BMI 陈旧/部分构建时发生）；
  5. LLVM 22 的 C API 结构体布局与网上旧例程全不同：CXCursor=32B {kind,xdata,data[3]}、
     CXSourceLocation/CXSourceRange=24B、CXToken=24B 值类型（tokenize 返回平铺数组，
     无 clang_getToken(array,i)）、CXString=16B 值类型——照旧例程写必段错误。
- 并发：ctypes 持 GIL，线程并行无效——ProcessPoolExecutor 一进程一文件（每进程加载
  一次 libclang），默认 min(cpu, 4) 个 worker，每进程 ~550MB 内存。
- 缓存 {build}/stylecheck_cache（key = sha256(源字节+命令)）：增量只重解析改动文件；
  全量 62 文件 4 worker ~20s，与编译并行不占关键路径。
- 只警告不阻断（exit 0）；--strict 有警告 exit 1。

用法：
    python3 tools/stylecheck/stylecheck.py [--build-dir build] [--workers N]
                                           [--file-threshold 500] [--func-threshold 120]
                                           [--nesting-threshold 5] [--strict]
"""

import argparse
import ctypes
import hashlib
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

EXCLUDE_SUBDIRS = ("/rgl/generated/",)
EXCLUDE_FILES = ("rgl/rlvk.cppm",)

DEFAULT_FILE_THRESHOLD = 500
DEFAULT_FUNC_THRESHOLD = 120
DEFAULT_NESTING_THRESHOLD = 5

FUNC_SPELLINGS = {"FunctionDecl", "CXXMethod", "Constructor", "Destructor",
                  "ConversionFunction"}
CTRL_SPELLINGS = {"IfStmt", "WhileStmt", "DoStmt", "ForStmt", "CXXForRangeStmt",
                  "SwitchStmt", "CXXCatchStmt", "LambdaExpr"}
TOK_COMMENT = 5

# ─── libclang ctypes 绑定（LLVM 22 API，结构体布局见文件头坑 5）────────────────


class CXCursorT(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("xdata", ctypes.c_int),
                ("data", ctypes.c_void_p * 3)]


class CXSourceLocationT(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2), ("int_data", ctypes.c_uint)]


class CXSourceRangeT(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2),
                ("begin_int_data", ctypes.c_uint), ("end_int_data", ctypes.c_uint)]


class CXTokenT(ctypes.Structure):
    _fields_ = [("int_data", ctypes.c_uint * 4), ("ptr_data", ctypes.c_void_p)]


class CXStringT(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("private_flags", ctypes.c_uint)]


VISITOR = ctypes.CFUNCTYPE(ctypes.c_uint, CXCursorT, CXCursorT, ctypes.c_void_p)


def bind_libclang(lib):
    """声明全部用到的原型（每个 worker 进程加载一次）。"""
    lib.clang_createIndex.restype = ctypes.c_void_p
    lib.clang_createIndex.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.clang_parseTranslationUnit.restype = ctypes.c_void_p
    lib.clang_parseTranslationUnit.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
    lib.clang_getTranslationUnitCursor.restype = CXCursorT
    lib.clang_getTranslationUnitCursor.argtypes = [ctypes.c_void_p]
    lib.clang_visitChildren.restype = ctypes.c_uint
    lib.clang_visitChildren.argtypes = [CXCursorT, VISITOR, ctypes.c_void_p]
    lib.clang_getCursorKindSpelling.restype = ctypes.c_char_p
    lib.clang_getCursorKindSpelling.argtypes = [ctypes.c_int]
    lib.clang_getCursorDisplayName.restype = ctypes.c_char_p
    lib.clang_getCursorDisplayName.argtypes = [CXCursorT]
    lib.clang_getCursorLocation.restype = CXSourceLocationT
    lib.clang_getCursorLocation.argtypes = [CXCursorT]
    lib.clang_getCursorExtent.restype = CXSourceRangeT
    lib.clang_getCursorExtent.argtypes = [CXCursorT]
    lib.clang_getSpellingLocation.argtypes = [
        CXSourceLocationT, ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint)]
    lib.clang_getFileName.restype = ctypes.c_char_p
    lib.clang_getFileName.argtypes = [ctypes.c_void_p]
    lib.clang_getCursorUSR.restype = CXStringT
    lib.clang_getCursorUSR.argtypes = [CXCursorT]
    lib.clang_getCString.restype = ctypes.c_char_p
    lib.clang_getCString.argtypes = [CXStringT]
    lib.clang_disposeString.argtypes = [CXStringT]
    lib.clang_getCursorDefinition.restype = CXCursorT
    lib.clang_getCursorDefinition.argtypes = [CXCursorT]
    lib.clang_tokenize.argtypes = [
        ctypes.c_void_p, CXSourceRangeT,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint)]
    lib.clang_getTokenKind.restype = ctypes.c_int
    lib.clang_getTokenKind.argtypes = [CXTokenT]
    lib.clang_getTokenLocation.restype = CXSourceLocationT
    lib.clang_getTokenLocation.argtypes = [ctypes.c_void_p, CXTokenT]
    lib.clang_disposeTokens.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    lib.clang_disposeTranslationUnit.argtypes = [ctypes.c_void_p]


def find_libclang(driver_path):
    """按构建链驱动路径推导 libclang 位置（Linux lib64/lib、Windows bin 目录）。"""
    d = os.path.dirname(driver_path)
    cands = [
        os.path.join(d, "..", "lib64", "libclang.so"),
        os.path.join(d, "..", "lib", "libclang.so"),
        os.path.join(d, "libclang.so"),
    ]
    if os.name == "nt":
        base = os.path.basename(driver_path).split(".")[0]
        cands = [os.path.join(d, "libclang.dll"),
                 os.path.join(d, base + ".dll")] + cands
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def spelling_loc(lib, loc):
    f = ctypes.c_void_p()
    line = ctypes.c_uint()
    col = ctypes.c_uint()
    off = ctypes.c_uint()
    lib.clang_getSpellingLocation(loc, ctypes.byref(f), ctypes.byref(line),
                                  ctypes.byref(col), ctypes.byref(off))
    return (lib.clang_getFileName(f).decode() if f.value else "", line.value, col.value)


def usr_of(lib, cur):
    s = lib.clang_getCursorUSR(cur)
    t = lib.clang_getCString(s)
    out = t.decode() if t else ""
    lib.clang_disposeString(s)
    return out


def is_definition(lib, cur):
    d = lib.clang_getCursorDefinition(cur)
    if d.kind == 0:
        return False
    return (usr_of(lib, cur), spelling_loc(lib, lib.clang_getCursorLocation(cur))) == \
           (usr_of(lib, d), spelling_loc(lib, lib.clang_getCursorLocation(d)))


def code_lines_in_range(lib, tu, rng, main_file):
    """extent 内主文件的非注释 token 行号集合。"""
    arr = ctypes.c_void_p()
    n = ctypes.c_uint()
    lib.clang_tokenize(tu, rng, ctypes.byref(arr), ctypes.byref(n))
    lines = set()
    if arr.value and n.value:
        toks = (CXTokenT * n.value).from_address(arr.value)
        for i in range(n.value):
            tok = toks[i]
            if lib.clang_getTokenKind(tok) == TOK_COMMENT:
                continue
            fn, ln, _ = spelling_loc(lib, lib.clang_getTokenLocation(tu, tok))
            if fn == main_file:
                lines.add(ln)
    lib.clang_disposeTokens(tu, arr, n.value)
    return lines


def collect_functions(lib, tu_cur, main_file):
    """主文件内全部函数定义 [(name, line, cursor)]。

    项目约定：主文件顶层 = imports + namespace RGL，函数都在 namespace 子树里；
    递归进入主文件位置的子树，非主文件子树整体剪枝（import 模块声明全在里面）。
    """
    funcs = []
    cb = None

    def walk(cur, parent, data):
        fn, ln, _ = spelling_loc(lib, lib.clang_getCursorLocation(cur))
        if fn != main_file:
            return 1
        sp = lib.clang_getCursorKindSpelling(cur.kind).decode()
        if sp in FUNC_SPELLINGS and is_definition(lib, cur):
            funcs.append((lib.clang_getCursorDisplayName(cur).decode(), ln, cur))
        lib.clang_visitChildren(cur, cb, None)
        return 1

    cb = VISITOR(walk)
    lib.clang_visitChildren(tu_cur, cb, None)
    return funcs


def nesting_depth(lib, func_cur):
    """控制结构嵌套深度（if/for/while/do/switch/catch/lambda 各 +1，函数体 0 层）。"""
    state = {"max": 0, "depth": 0}
    cb = None

    def walk(cur, parent, data):
        sp = lib.clang_getCursorKindSpelling(cur.kind).decode()
        if sp in CTRL_SPELLINGS:
            state["depth"] += 1
            if state["depth"] > state["max"]:
                state["max"] = state["depth"]
        lib.clang_visitChildren(cur, cb, None)
        if sp in CTRL_SPELLINGS:
            state["depth"] -= 1
        return 1

    cb = VISITOR(walk)
    lib.clang_visitChildren(func_cur, cb, None)
    return state["max"]


# ─── 编译命令清理（坑 1/2/3 全在这里）────────────────────────────────────────


def clean_cmd(entry, full, scratch_dir, resdir):
    """compile_commands 命令 → libclang 可用的纯 flag 列表。"""
    raw = []
    for a in shlex.split(entry["command"], posix=os.name != "nt"):
        if a.startswith("@"):
            rf = a[1:]
            if not os.path.isabs(rf):
                rf = os.path.join(entry["directory"], rf)
            raw.extend(shlex.split(open(rf, encoding="utf-8").read().strip()))
        else:
            raw.append(a)
    # -fmodule-file 的 BMI 相对路径按 CWD 解析（构建时 CWD=build 目录）——
    # 这里统一转绝对路径，工具自身 CWD 无所谓
    absed = []
    for a in raw:
        if a.startswith("-fmodule-file="):
            v = a[len("-fmodule-file="):]
            if "=" in v:
                name, path = v.split("=", 1)
            else:
                name, path = None, v
            if not os.path.isabs(path):
                path = os.path.abspath(os.path.join(entry["directory"], path))
            a = f"-fmodule-file={name}={path}" if name else f"-fmodule-file={path}"
        absed.append(a)
    raw = absed
    cmd = []
    skip = False
    for a in raw:
        if skip:
            skip = False
            continue
        if a == "-c" or a.startswith("-fmodule-output="):
            continue
        if a == "-o":
            skip = True
            continue
        if a == full:
            continue
        base = os.path.basename(a)
        if base in ("clang++", "clang", "clang++.exe", "clang.exe"):
            continue
        cmd.append(a)
    cmd += ["-fmodule-output=" + os.path.join(scratch_dir,
                                              os.path.basename(full) + ".pcm"),
            "-fsyntax-only", "-resource-dir", resdir]
    return cmd


def analyze_file(lib, entry, full, scratch_dir, resdir):
    """解析一个 .cppm 并算三项指标；parse 失败（NULL）抛 RuntimeError。"""
    cmd = clean_cmd(entry, full, scratch_dir, resdir)
    idx = lib.clang_createIndex(0, 0)
    argv = (ctypes.c_char_p * len(cmd))(*[a.encode() for a in cmd])
    tu = lib.clang_parseTranslationUnit(idx, full.encode(), argv, len(cmd), None, 0, 0)
    if not tu:
        raise RuntimeError("parse 返回 NULL（BMI 缺失/陈旧或 libclang 模块缺陷）")
    try:
        src = open(full, encoding="utf-8").read()
        total_lines = len(src.splitlines())
        tu_cur = lib.clang_getTranslationUnitCursor(tu)
        file_lines = code_lines_in_range(lib, tu, lib.clang_getCursorExtent(tu_cur), full)
        functions = []
        for name, ln, cur in collect_functions(lib, tu_cur, full):
            flines = code_lines_in_range(lib, tu, lib.clang_getCursorExtent(cur), full)
            functions.append({"name": name, "line": ln,
                              "code_lines": len(flines),
                              "nesting": nesting_depth(lib, cur)})
        return {"total_lines": total_lines,
                "file_code_lines": len(file_lines),
                "functions": functions}
    finally:
        lib.clang_disposeTranslationUnit(tu)


# ─── worker 进程（每进程加载一次 libclang）────────────────────────────────────

_W = {}


def _worker_init(ctx):
    path = ctx["libclang"]
    lib = ctypes.CDLL(path)
    bind_libclang(lib)
    _W["lib"] = lib
    _W["ctx"] = ctx


def _worker_task(rel_entry):
    rel, entry = rel_entry
    ctx = _W["ctx"]
    full = os.path.join(ctx["root"], rel)
    src = open(full, "rb").read()
    cmd = clean_cmd(entry, full, ctx["scratch_dir"], ctx["resdir"])
    key = hashlib.sha256(
        hashlib.sha256(src).hexdigest().encode()
        + json.dumps(cmd, sort_keys=True).encode()
    ).hexdigest()[:24]
    cache = os.path.join(ctx["cache_dir"], key + ".json")
    if os.path.exists(cache):
        return rel, json.load(open(cache, encoding="utf-8")), "cache"
    try:
        m = analyze_file(_W["lib"], entry, full, ctx["scratch_dir"], ctx["resdir"])
    except Exception as ex:
        return rel, None, "error:" + str(ex).splitlines()[0]
    os.makedirs(ctx["cache_dir"], exist_ok=True)
    with open(cache, "w", encoding="utf-8") as f:
        json.dump(m, f, ensure_ascii=False)
    return rel, m, "parsed"


# ─── 主进程 ───────────────────────────────────────────────────────────────────


def load_db(build_dir):
    db_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        raise SystemExit(
            f"error: 找不到 {db_path}\n  检查器复用 RGL 构建的编译命令（modules 依赖在里面）。\n"
            "  请先 just config && just build。")
    db = json.load(open(db_path, encoding="utf-8"))
    by_file = {}
    driver = None
    for e in db:
        full = os.path.abspath(os.path.join(e["directory"], e["file"]))
        by_file[full] = e
        if driver is None:
            first = shlex.split(e["command"], posix=os.name != "nt")[0]
            if "clang" in os.path.basename(first):
                driver = first
    return by_file, driver


def main():
    parser = argparse.ArgumentParser(description="RGL 文件/函数结构检查")
    parser.add_argument("--build-dir", default=None, help="build 目录（默认 <repo>/build）")
    parser.add_argument("--workers", type=int, default=min(os.cpu_count() or 4, 4),
                        help="并行进程数（默认 min(cpu,4)，每进程 ~550MB 内存）")
    parser.add_argument("--file-threshold", type=int, default=DEFAULT_FILE_THRESHOLD,
                        help=f"文件代码行阈值（默认 {DEFAULT_FILE_THRESHOLD}）")
    parser.add_argument("--func-threshold", type=int, default=DEFAULT_FUNC_THRESHOLD,
                        help=f"函数代码行阈值（默认 {DEFAULT_FUNC_THRESHOLD}）")
    parser.add_argument("--nesting-threshold", type=int, default=DEFAULT_NESTING_THRESHOLD,
                        help=f"函数嵌套深度阈值（默认 {DEFAULT_NESTING_THRESHOLD}）")
    parser.add_argument("--strict", action="store_true",
                        help="有警告时 exit 1（默认只警告不阻断）")
    args = parser.parse_args()

    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(ROOT, "build")
    by_file, driver = load_db(build)
    if driver is None:
        raise SystemExit("error: compile_commands.json 里找不到 clang 驱动路径")
    libclang = find_libclang(driver)
    if libclang is None:
        print(f"[style-check] 警告: 找不到 libclang（驱动 {driver} 推导），跳过检查")
        return
    resdir = subprocess.run([driver, "-print-resource-dir"],
                            capture_output=True, text=True).stdout.strip()

    units = []
    for full in sorted(by_file):
        if not full.endswith(".cppm"):
            continue
        norm = full.replace(os.sep, "/")
        if "/rgl/" not in norm:
            continue
        if any(x in norm for x in EXCLUDE_SUBDIRS):
            continue
        rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
        if rel in EXCLUDE_FILES:
            continue
        units.append((rel, by_file[full]))

    ctx = {"root": ROOT, "build": build, "libclang": libclang,
           "cache_dir": os.path.join(build, "stylecheck_cache"),
           "scratch_dir": os.path.join(build, "stylecheck_scratch"),
           "resdir": resdir}
    os.makedirs(ctx["scratch_dir"], exist_ok=True)
    os.makedirs(ctx["cache_dir"], exist_ok=True)

    warnings, errors, hits, parsed = [], [], 0, 0
    with ProcessPoolExecutor(max_workers=args.workers, initializer=_worker_init,
                             initargs=(ctx,)) as pool:
        for rel, metrics, how in pool.map(_worker_task, units):
            if metrics is None:
                errors.append((rel, how[len("error:"):]))
                continue
            if how == "cache":
                hits += 1
            else:
                parsed += 1
            if metrics["file_code_lines"] > args.file_threshold:
                warnings.append((rel, 0,
                    f"文件代码行 {metrics['file_code_lines']} > {args.file_threshold}"
                    f"（去注释空行，总行数 {metrics['total_lines']}）——应拆分文件/模块"))
            for fn in metrics["functions"]:
                if fn["code_lines"] > args.func_threshold:
                    warnings.append((rel, fn["line"],
                        f"函数 '{fn['name']}' 代码行 {fn['code_lines']} > {args.func_threshold}"
                        f"（去注释空行）——应拆分为更小函数"))
                if fn["nesting"] > args.nesting_threshold:
                    warnings.append((rel, fn["line"],
                        f"函数 '{fn['name']}' 嵌套深度 {fn['nesting']} > {args.nesting_threshold}"
                        f"——应拆分为更小函数"))

    for rel, line, msg in sorted(warnings, key=lambda w: (w[0], w[1])):
        loc = f"{rel}:{line}" if line else rel
        print(f"[style-check] {loc}: {msg}")
    for rel, err in errors:
        print(f"[style-check] {rel}: 检查失败（跳过）: {err}")
    print(f"[style-check] 汇总: {len(warnings)} 警告 / {len(units)} 个 .cppm"
          f"（缓存命中 {hits}、解析 {parsed}、失败 {len(errors)}）")
    sys.exit(1 if (args.strict and (warnings or errors)) else 0)


if __name__ == "__main__":
    main()
