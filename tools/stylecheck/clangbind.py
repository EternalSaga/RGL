"""libclang ctypes 绑定与 AST helper（LLVM 22 API，纯标准库）。

坑 5（本机实测，勿回退）：LLVM 22 的 C API 结构体布局与网上旧例程全不同：
CXCursor=32B {kind,xdata,data[3]}、CXSourceLocation/CXSourceRange=24B、
CXToken=24B 值类型（tokenize 返回平铺数组，无 clang_getToken(array,i)）、
CXString=16B 值类型——照旧例程写必段错误。
另：clang_makeRange 在 LLVM 22 已更名 clang_getRange（CXSourceLocation.h，
旧名 ctypes 查找必 AttributeError）。

本模块只放"绑定 + 通用 AST helper + 认知复杂度"；编译命令清理在 cmdclean.py。
"""

import ctypes
import os

from config import (BINARY_OPERATOR_SPELLING, COGNITIVE_STRUCT_SPELLINGS,
                    ELSE_TOKEN, FUNC_SPELLINGS, IF_SPELLING, LAMBDA_SPELLING,
                    LOGICAL_OPERATOR_TOKENS, SWITCH_SPELLINGS, TOK_COMMENT,
                    FIELD_SPELLING)


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
    lib.clang_getTokenSpelling.restype = CXStringT
    lib.clang_getTokenSpelling.argtypes = [ctypes.c_void_p, CXTokenT]
    lib.clang_getTokenLocation.restype = CXSourceLocationT
    lib.clang_getTokenLocation.argtypes = [ctypes.c_void_p, CXTokenT]
    lib.clang_disposeTokens.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    lib.clang_disposeTranslationUnit.argtypes = [ctypes.c_void_p]
    # 认知复杂度用：range 拆/装（LLVM 22 里 makeRange 已更名 getRange）
    lib.clang_getRangeStart.restype = CXSourceLocationT
    lib.clang_getRangeStart.argtypes = [CXSourceRangeT]
    lib.clang_getRangeEnd.restype = CXSourceLocationT
    lib.clang_getRangeEnd.argtypes = [CXSourceRangeT]
    lib.clang_getRange.restype = CXSourceRangeT
    lib.clang_getRange.argtypes = [CXSourceLocationT, CXSourceLocationT]


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


def tokenize_spellings(lib, tu, rng):
    """tokenize 一段 range → [(token kind, spelling 文本)]（认知复杂度 gap 判定用）。"""
    arr = ctypes.c_void_p()
    n = ctypes.c_uint()
    lib.clang_tokenize(tu, rng, ctypes.byref(arr), ctypes.byref(n))
    out = []
    if arr.value and n.value:
        toks = (CXTokenT * n.value).from_address(arr.value)
        for i in range(n.value):
            s = lib.clang_getCString(lib.clang_getTokenSpelling(tu, toks[i]))
            out.append((lib.clang_getTokenKind(toks[i]), s.decode() if s else ""))
    lib.clang_disposeTokens(tu, arr, n.value)
    return out


def kind_spelling(lib, cur):
    """cursor kind 拼写串（如 'IfStmt'/'BinaryOperator'）。"""
    return lib.clang_getCursorKindSpelling(cur.kind).decode()


def children_of(lib, cur):
    """直接子 cursor 列表（浅取，不递归；回调生命周期仅覆盖本次调用）。"""
    kids = []
    cb = VISITOR(lambda k, p, d: (kids.append(k), 1)[1])
    lib.clang_visitChildren(cur, cb, None)
    return kids


def collect_decls(lib, tu_cur, main_file):
    """主文件内全部函数定义 + lambda + 成员字段 [(name, line, cursor)]。

    项目约定：主文件顶层 = imports + namespace RGL，声明都在 namespace 子树里；
    递归进入主文件位置的子树，非主文件子树整体剪枝（import 模块声明全在里面）。
    lambda 与函数定义一样收录为独立条目（独立计分行数/认知复杂度）；lambda 名字
    取其所在函数无法从 display name 稳定得到，用 "(anonymous lambda)" 占位 + 行号定位。
    注意：lambda 嵌在函数体内时，父函数 walk 不深入 lambda（见 cognitive_complexity
    的 lambda 边界），但 collect_decls 独立递归整棵 AST，二者互不影响。
    """
    funcs, fields = [], []
    cb = None

    def walk(cur, parent, data):
        fn, ln, _ = spelling_loc(lib, lib.clang_getCursorLocation(cur))
        if fn != main_file:
            return 1
        sp = kind_spelling(lib, cur)
        name = lib.clang_getCursorDisplayName(cur).decode()
        if sp in FUNC_SPELLINGS and is_definition(lib, cur):
            funcs.append((name, ln, cur))
        elif sp == LAMBDA_SPELLING:
            funcs.append(("(anonymous lambda)", ln, cur))
        elif sp == FIELD_SPELLING:
            fields.append((name, ln))
        lib.clang_visitChildren(cur, cb, None)
        return 1

    cb = VISITOR(walk)
    lib.clang_visitChildren(tu_cur, cb, None)
    return funcs, fields


# ─── 认知复杂度（SonarSource 风格）────────────────────────────────────────────
# 打分规则（函数体入口 nesting=0，逐节点）：
# - 结构 +1+nesting 且子树 nesting+1：IfStmt/ForStmt/CXXForRangeStmt/WhileStmt/
#   DoStmt/CXXCatchStmt/三元 ConditionalOperator。
# - +1 但子树 nesting 不变：SwitchStmt；else / else if 分支（else 子节点若又是
#   IfStmt = else if：只 +1 一次、内层 if 不再重复结构计分——skip 集实现）。
# - 二元逻辑运算符序列：一段连续的 &&/|| 整段记 +1（flat）。链起点 = 本节点是
#   BinaryOperator 且运算符为 &&/||、父节点不是 &&/|| 的 BinaryOperator；
#   序列子树 nesting+1。运算符 opcode 拿 kind spelling 拿不到（恒为
#   "BinaryOperator"），用左右操作数 extent 之间 gap 的 token 文本判定。
# - 独立的嵌套单元不深入、各自单独计分（collect_decls 已逐一收录为条目）：
#   FunctionDecl/CXXMethod 等命名函数定义 + **LambdaExpr**（lambda = "函数中的
#   函数"，其体不计入宿主、从自身起独立算 nesting=0）。宿主与 lambda 认知复杂度
#   互不叠加（2026-09-12 修正：原实现把 lambda 体叠加进宿主，虚高宿主复杂度）。

def _logical_op(lib, tu, cur):
    """cur 为 &&/|| 的 BinaryOperator 时返回运算符串，否则 None。

    opcode 判定 = 左右操作数（两子 cursor）extent 之间 gap 的 token 文本
    （clang_getCursorKindSpelling 只给 'BinaryOperator'，不含 opcode）。
    精确匹配 '&&'/'||'：复合赋值 '&&=' 不误报；lhs/rhs 是表达式，gap 之外
    不可能出现逻辑运算符 token。"""
    kids = children_of(lib, cur)
    if len(kids) != 2:
        return None
    gap = lib.clang_getRange(lib.clang_getRangeEnd(lib.clang_getCursorExtent(kids[0])),
                             lib.clang_getRangeStart(lib.clang_getCursorExtent(kids[1])))
    for kind, text in tokenize_spellings(lib, tu, gap):
        if kind != TOK_COMMENT and text in LOGICAL_OPERATOR_TOKENS:
            return text
    return None


def _if_else_child(lib, tu, if_cur):
    """IfStmt 的 else 分支 cursor（无则 None）。

    libclang 的 IfStmt 子节点 = [init?, cond, then, else?]——带 init-statement 的
    if 无 else 也是 3 子（坑），不能以子节点数判 else；判据 = 倒数第二子 extent
    末与末子 extent 首之间的 gap 含 'else' 关键字 token（实测 LLVM 22 精确）。"""
    kids = children_of(lib, if_cur)
    if len(kids) < 3:
        return None
    a, b = kids[-2], kids[-1]
    gap = lib.clang_getRange(lib.clang_getRangeEnd(lib.clang_getCursorExtent(a)),
                             lib.clang_getRangeStart(lib.clang_getCursorExtent(b)))
    for kind, text in tokenize_spellings(lib, tu, gap):
        if kind != TOK_COMMENT and text == ELSE_TOKEN:
            return b
    return None


def cognitive_complexity(lib, tu, func_cursor):
    """单函数认知复杂度（规则见上方块注释）。

    nesting 经 visitChildren 的 client_data 指针逐层传 nesting+1（入口 1 表 0，
    避开 0/None 混淆）。游标身份键 = (kind, xdata, data[3]) 值组（cursor 是值
    语义结构体，同一节点重建值相同）——用于 else if 的"内层 if 不重复计分"。
    遍历体内异常不穿越 ctypes 边界（记录后跳过），由调用方见 err 抛错。"""
    state = {"total": 0, "skip": set(), "err": None}
    cb = None

    def key_of(cur):
        return (cur.kind, cur.xdata, tuple(int(x or 0) for x in cur.data))

    def walk(cur, parent, data):
        if state["err"] is not None:
            return 1
        try:
            nest = (int(data) - 1) if data else 0
            sp = kind_spelling(lib, cur)
            if (sp in FUNC_SPELLINGS and is_definition(lib, cur)) or sp == LAMBDA_SPELLING:
                return 1  # 独立嵌套单元（命名函数 / lambda）：各自单独计分，不叠加宿主
            child_nest = nest
            key = key_of(cur)
            if key in state["skip"]:
                state["skip"].discard(key)  # else if 的内层 if：+1 已在外层计入
            elif sp in COGNITIVE_STRUCT_SPELLINGS:
                state["total"] += 1 + nest
                child_nest = nest + 1
            elif sp in SWITCH_SPELLINGS:
                state["total"] += 1 + nest  # 子树 nesting 不变
            elif sp == BINARY_OPERATOR_SPELLING and _logical_op(lib, tu, cur):
                if not (kind_spelling(lib, parent) == BINARY_OPERATOR_SPELLING
                        and _logical_op(lib, tu, parent)):
                    state["total"] += 1  # 链起点整段 +1（flat）
                    child_nest = nest + 1  # 该序列递增 nesting
            if sp == IF_SPELLING:
                ec = _if_else_child(lib, tu, cur)
                if ec is not None:
                    state["total"] += 1  # else / else if：+1，不递增 nesting
                    if kind_spelling(lib, ec) == IF_SPELLING:
                        state["skip"].add(key_of(ec))
            lib.clang_visitChildren(cur, cb, ctypes.c_void_p(child_nest + 1))
        except Exception as ex:  # ctypes 回调内异常不外逃（防 UB），记录后本文件判失败
            state["err"] = repr(ex)
        return 1

    cb = VISITOR(walk)
    lib.clang_visitChildren(func_cursor, cb, ctypes.c_void_p(1))
    if state["err"] is not None:
        raise RuntimeError(f"认知复杂度遍历异常: {state['err']}")
    return state["total"]
