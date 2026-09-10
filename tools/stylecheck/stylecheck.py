"""文件/函数结构检查（CMake style_check 目标 / just style-check）。

七类规则（前五项走 libclang 解析，后两项纯文本主进程）：
- 文件代码行 > --file-threshold（默认 500）→ 提示拆分文件
- 函数代码行 > --func-threshold（默认 120）→ 提示拆分函数
- 函数嵌套深度 > --nesting-threshold（默认 5）→ 提示拆分函数
  （嵌套 = if/for/while/do/switch/catch/lambda 各 +1 层，函数体本身 0 层）
- 接口瘦身：非 _impl 文件里函数定义 > --impl-threshold（默认 100）代码行
  → 提示迁入 xxx_impl.cppm（AGENTS.md"接口瘦身"，接口 BMI 不含函数体）
- 成员命名：成员变量名带 _/m_ 前缀或 _ 后缀 → 提示自然命名
- 分层/模块优先（文本）：底层零 entt（不得 import ecs、GMF 不得 include <entt>，
  名单 ZERO_ENTT_MODULES）；点名的禁止 import 边（FORBIDDEN_EDGES）；rgl/ 下
  不得有 .h/.hpp/.hh、.cpp 仅许 main.cpp；任何内部头 #include " 禁止
- GMF STL 白名单：GMF #include <STL 头> 只许 ranges/filesystem/chrono 三例外，
  其余走 import rlstd（AGENTS.md"优先使用 rlstd"）

代码行 = 去除注释与空行，token 级精确。

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
                                           [--nesting-threshold 5] [--impl-threshold 100]
                                           [--strict]
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

EXCLUDE_SUBDIRS = ("/rgl/generated/",)
EXCLUDE_FILES = ("rgl/rlvk.cppm",)

# 缓存版本：指标结构变化（如新增 fields）时 +1，自动失效旧缓存
CACHE_VERSION = "v2"

DEFAULT_FILE_THRESHOLD = 500
DEFAULT_FUNC_THRESHOLD = 120
DEFAULT_NESTING_THRESHOLD = 5

FUNC_SPELLINGS = {"FunctionDecl", "CXXMethod", "Constructor", "Destructor",
                  "ConversionFunction"}
CTRL_SPELLINGS = {"IfStmt", "WhileStmt", "DoStmt", "ForStmt", "CXXForRangeStmt",
                  "SwitchStmt", "CXXCatchStmt", "LambdaExpr"}
TOK_COMMENT = 5
FIELD_SPELLING = "FieldDecl"

# 接口瘦身（AGENTS.md"接口瘦身"）：非 _impl 接口单元里 >100 代码行的函数定义
# 应迁入 xxx_impl.cppm（接口 BMI 不含函数体，所有 importer 受益）
DEFAULT_IMPL_THRESHOLD = 100

# 分层依赖（AGENTS.md"构造依赖图"原则：只允许下层 import 上层）——
# 底层渲染层/纯配置零 entt：不得 import ecs，GMF 不得 #include <entt>
# （entt 经 GMF #include 暴露，import ecs 与 GMF include 都要拦）
ZERO_ENTT_MODULES = {
    "rlvk", "vkerror", "vulkan_utils", "features", "queue_family_manager",
    "VmaRaii", "vk_instance", "vk_device", "vk_swapchain", "vulkan_window",
    "vk_resources", "shader", "shader_registry", "state_kit", "gpu_cull",
    "vk_indirect_commands", "frame_session", "frame_capture", "cull_driver",
    "frame_draw_list", "render_graph", "render_graph_config", "diagnostics",
    "gpu_canary", "runtime_config", "camera_config", "scene_config",
}
# AGENTS.md 点名的假边（配置不依赖执行模块；截图经回调）
FORBIDDEN_EDGES = {
    "camera_config": {"llm_debug"},
    "ecs": {"llm_debug"},
}

# 模块优先：rgl/ 下除入口与单头库实现单元外不得有 .cpp，任何 .h/.hpp/.hh 都不允许
# （stb/tinygltf/VMA 是单头 C 库，实现宏只能放普通 .cpp——既有例外，勿删）
ALLOWED_CPP = {"main.cpp", "stb_impl.cpp", "tinygltf_impl.cpp", "vam_impl.cpp"}

# GMF 的 STL 白名单（AGENTS.md"优先使用 rlstd"）：STL 头只许三个例外，
# 其余一律走 import rlstd（缺名补入 rlstd.cppm）；第三方头不受此限
STL_HEADERS = {
    "algorithm", "any", "array", "atomic", "bit", "bitset", "charconv",
    "chrono", "compare", "complex", "concepts", "condition_variable",
    "coroutine", "deque", "exception", "execution", "expected",
    "filesystem", "forward_list", "fstream", "functional", "future",
    "generator", "iomanip", "ios", "iosfwd", "iostream", "istream", "latch",
    "limits", "list", "locale", "map", "memory", "mutex", "new", "numbers",
    "numeric", "optional", "ostream", "queue", "random", "ranges", "regex",
    "set", "semaphore", "source_location", "span", "sstream", "stack",
    "stdfloat", "stdexcept", "streambuf", "string", "string_view",
    "stride", "syncstream", "thread", "tuple", "type_traits", "typeindex",
    "typeinfo", "unordered_map", "unordered_set", "variant", "vector",
    "version",
    "cassert", "ccomplex", "cctype", "cerrno", "cfloat", "cfenv",
    "cinttypes", "climits", "clocale", "cmath", "csetjmp", "csignal",
    "cstdarg", "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib",
    "cstring", "ctgmath", "ctime", "cuchar", "cwchar", "cwctype",
}
STL_GMF_EXCEPTIONS = {"ranges", "filesystem", "chrono"}

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


def collect_decls(lib, tu_cur, main_file):
    """主文件内全部函数定义 + 成员字段 [(name, line, cursor)]。

    项目约定：主文件顶层 = imports + namespace RGL，声明都在 namespace 子树里；
    递归进入主文件位置的子树，非主文件子树整体剪枝（import 模块声明全在里面）。
    """
    funcs, fields = [], []
    cb = None

    def walk(cur, parent, data):
        fn, ln, _ = spelling_loc(lib, lib.clang_getCursorLocation(cur))
        if fn != main_file:
            return 1
        sp = lib.clang_getCursorKindSpelling(cur.kind).decode()
        name = lib.clang_getCursorDisplayName(cur).decode()
        if sp in FUNC_SPELLINGS and is_definition(lib, cur):
            funcs.append((name, ln, cur))
        elif sp == FIELD_SPELLING:
            fields.append((name, ln))
        lib.clang_visitChildren(cur, cb, None)
        return 1

    cb = VISITOR(walk)
    lib.clang_visitChildren(tu_cur, cb, None)
    return funcs, fields


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
        funcs, fields = collect_decls(lib, tu_cur, full)
        functions = []
        for name, ln, cur in funcs:
            flines = code_lines_in_range(lib, tu, lib.clang_getCursorExtent(cur), full)
            functions.append({"name": name, "line": ln,
                              "code_lines": len(flines),
                              "nesting": nesting_depth(lib, cur)})
        return {"total_lines": total_lines,
                "file_code_lines": len(file_lines),
                "functions": functions,
                "fields": [{"name": n, "line": l} for n, l in fields]}
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
        CACHE_VERSION.encode()
        + hashlib.sha256(src).hexdigest().encode()
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


# ─── 文本检查（纯主进程，覆盖全部 rgl/*.cppm 含 rlvk，排除 generated/）──────

MODULE_LINE_RE = re.compile(r"^\s*(?:export\s+)?module\s*([\w:]+)?\s*;")
IMPORT_RE = re.compile(r"^\s*import\s+([\w:]+)\s*;")
# 两组定界符各自闭合（不能用 \1 反引用：< 的闭合符是 >）
INCLUDE_RE = re.compile(r"^\s*#include\s*(?:<([^>]+)>|\"([^\"]+)\")")


def parse_module_file(src):
    """按行状态机解析：GMF = 裸 module; 之后、具名模块声明之前。

    返回 (模块名, {import: 行号}, [(include 路径, 行号, 定界符)])。
    """
    name, imps, gmf_incs = None, {}, []
    in_gmf = False
    for i, ln in enumerate(src.splitlines(), 1):
        m = MODULE_LINE_RE.match(ln)
        if m:
            if m.group(1) is None:
                in_gmf = True
            else:
                name = m.group(1)
                in_gmf = False
            continue
        if in_gmf:
            m = INCLUDE_RE.match(ln)
            if m:
                path = m.group(1) if m.group(1) is not None else m.group(2)
                delim = "<" if m.group(1) is not None else '"'
                gmf_incs.append((path, i, delim))
            continue
        m = IMPORT_RE.match(ln)
        if m and m.group(1) not in imps:
            imps[m.group(1)] = i
    return name, imps, gmf_incs


def rgl_cppm_files():
    """rgl/ 下全部 .cppm（含 rlvk，排除 generated/），仓库相对 POSIX 路径。"""
    out = []
    base = os.path.join(ROOT, "rgl")
    for dirpath, dirnames, filenames in os.walk(base):
        norm = dirpath.replace(os.sep, "/")
        if "/generated" in norm:
            dirnames[:] = []
            continue
        for fn in filenames:
            if fn.endswith(".cppm"):
                out.append(os.path.relpath(os.path.join(dirpath, fn), ROOT)
                           .replace(os.sep, "/"))
    return sorted(out)


def text_checks():
    """A3 分层依赖 + B1 模块优先 + B2 GMF STL 白名单。返回 [(rel, line, msg)]。"""
    warnings = []
    mods = {}
    for rel in rgl_cppm_files():
        src = open(os.path.join(ROOT, rel), encoding="utf-8").read()
        name, imps, gmf_incs = parse_module_file(src)
        if name is None:
            continue
        mods[name] = (rel, imps, gmf_incs)
        # B1：内部头 include（模块优先：模块间只走 import）
        for inc, i, delim in gmf_incs:
            if delim == '"':
                warnings.append((rel, i,
                    f'GMF #include "{inc}"——模块优先：模块间依赖只走 import，'
                    f"内部头文件不应存在"))
        # B2：GMF 的 STL 头白名单
        for inc, i, delim in gmf_incs:
            if delim != "<" or "/" in inc:
                continue
            if inc in STL_HEADERS and inc not in STL_GMF_EXCEPTIONS:
                warnings.append((rel, i,
                    f"GMF #include <{inc}>——STL 走 import rlstd"
                    f"（例外仅 ranges/filesystem/chrono；rlstd 缺名则补入 rlstd.cppm）"))
    # A3：底层零 entt + 点名禁止边
    for name, (rel, imps, gmf_incs) in mods.items():
        if name in ZERO_ENTT_MODULES:
            if "ecs" in imps:
                warnings.append((rel, imps["ecs"],
                    f"模块 '{name}' import ecs——底层渲染层不得依赖 ECS"
                    f"（entity/component/system 归场景层，见 AGENTS.md 分层）"))
            for inc, i, _ in gmf_incs:
                if inc.startswith("entt/"):
                    warnings.append((rel, i,
                        f"模块 '{name}' GMF #include <{inc}>"
                        f"——底层渲染层零 entt（见 AGENTS.md 分层）"))
        for target in sorted(FORBIDDEN_EDGES.get(name, ())):
            if target in imps:
                warnings.append((rel, imps[target],
                    f"模块 '{name}' import '{target}'——禁止依赖边"
                    f"（见 AGENTS.md 构造依赖图原则）"))
    # B1：rgl/ 下 .h/.hpp/.hh/.cpp 文件
    base = os.path.join(ROOT, "rgl")
    for dirpath, dirnames, filenames in os.walk(base):
        norm = dirpath.replace(os.sep, "/")
        if "/generated" in norm:
            dirnames[:] = []
            continue
        for fn in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, "/")
            if fn.endswith((".h", ".hpp", ".hh")):
                warnings.append((rel, 0,
                    "头文件——模块优先：所有新代码必须 C++20 modules (.cppm)"))
            elif fn.endswith(".cpp") and fn not in ALLOWED_CPP:
                warnings.append((rel, 0,
                    ".cpp 文件——模块优先：仅入口 main.cpp 允许 .cpp"))
    return warnings


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
    parser.add_argument("--impl-threshold", type=int, default=DEFAULT_IMPL_THRESHOLD,
                        help=f"接口单元函数体代码行阈值（默认 {DEFAULT_IMPL_THRESHOLD}）")
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
                if (not rel.endswith("_impl.cppm")
                        and fn["code_lines"] > args.impl_threshold):
                    target = os.path.basename(rel)[:-len(".cppm")] + "_impl.cppm"
                    warnings.append((rel, fn["line"],
                        f"函数 '{fn['name']}' 函数体代码行 {fn['code_lines']} > {args.impl_threshold}"
                        f"——接口瘦身：实现应迁入 {target}（接口 BMI 不含函数体）"))
            for fd in metrics["fields"]:
                n = fd["name"]
                if n.startswith("_") or n.startswith("m_") or n.endswith("_"):
                    warnings.append((rel, fd["line"],
                        f"成员变量 '{n}'——成员命名不加 _/m_ 前缀、不加 _ 后缀（自然命名）"))

    warnings.extend(text_checks())

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
