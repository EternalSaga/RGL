"""单文件分析：一次 libclang parse + 同一 TU 上跑 Indexer，产出样式指标 + doc 声明表。

缓存 JSON schema（CACHE_VERSION 随结构变化递增）：
    {total_lines, file_code_lines,
     functions: [{name, line, code_lines, cognitive,
                  returns_expected, throws}],  # 含 lambda 独立条目；v7 增两字段
     fields:    [{name, line}],
     decls:     [{k: class|enum|function|method, n, l}]}  # doc 规则消费
代码行 = 去注释与空行，token 级精确（libclang tokenize）。

为什么声明/函数指标走 Indexer 而不是 cursor 递归遍历（坑 7，2026-09-23 实测）：
`-std=gnu++23` + `-x c++-module` 下 export 声明在游标树里是 0 子的
UnexposedDecl——旧 collect_decls 对接口文件的类内方法/字段**整体不可见**
（state_kit funcs=0/fields=0），doxygen 检查只能靠文本 -ast-dump 第二通路。
Indexer 穿透 export，且回调内可重入 visitChildren/tokenize（实测），函数体
复杂度就地计算——**一个 TU、两套规则共享同一份 AST** 由此成立。

decls 过滤（与旧 doccheck 文本管线行为对齐）：
- class/enum：定义记录，record 祖先 ≤1（嵌套一层记录 emit，更深吞掉）；
  记录要求 isDefinition（前置声明不 emit），枚举放宽（罕见，出现即检）；
- function（ekind=Function）：仅顶层（容器非 record）；
- method（CXXStatic/InstanceMethod/Constructor/Destructor/Conversion）：
  容器是**本文件已知记录**且该记录是顶层类——出类定义（容器 record 不在本
  文件，如 impl 里的 Foo::bar）不 emit（旧文本管线同样不 emit）；
- 隐式声明（编译器生成 ctor/dtor 等）全部剔除；同名同行去重（redeclaration
  折叠，emit 取首个 = 声明处，与旧一致）；
- 行号统一用 extent range start（含返回类型行，对齐旧 doccheck 的声明首行
  语义；样式警告行号随之从标识符行改为声明起始行，更准）。
"""

import ctypes

from clangbind import (EKIND_ENUM, EKIND_FIELD, EKIND_FUNCTION, EKIND_FUNCTIONISH,
                       EKIND_RECORD, children_of,
                       code_lines_in_range, cognitive_complexity, index_decls,
                       kind_spelling, tokenize_spellings)
from cmdclean import clean_cmd

_RECORD_CURSOR_SPELLINGS = {"ClassDecl", "StructDecl", "UnionDecl", "EnumDecl",
                            "ClassTemplate"}
_LAMBDA_SPELLING = "LambdaExpr"
_THROW_SPELLING = "ThrowStmt"
# export 区的 semanticContainer cursor 是 UnexposedDecl（坑 7 同源），非 record ✓


def _returns_expected(lib, tu, cur):
    """签名（extent 首个 `{` 之前）是否出现 `expected` token = 返回 std::expected。

    tokenize 全 extent 后截到首个 `{`（函数体起点）——覆盖多行签名与
    `auto f() -> std::expected<...>` 尾置返回类型；构造/析构无 expected 返回，恒 False。
    """
    saw_open = False
    for kind, text in tokenize_spellings(lib, tu, lib.clang_getCursorExtent(cur)):
        if text == "{":
            saw_open = True
            break
        if kind != 4 and text == "expected":  # TOK_COMMENT=4
            return True
    return False if saw_open else False


def _body_throws(lib, cur, _depth=0):
    """函数体（含嵌套 lambda）是否含 ThrowStmt 游标。深度护栏防病态 AST。"""
    if _depth > 40:
        return False
    for c in children_of(lib, cur):
        if kind_spelling(lib, c) == _THROW_SPELLING:
            return True
        if _body_throws(lib, c, _depth + 1):
            return True
    return False


def _file_line(lib, cur, main_file):
    """(是否在主文件, range-start 行号)。"""
    rng = lib.clang_getCursorExtent(cur)
    f = ctypes.c_void_p()
    ln = ctypes.c_uint()
    col = ctypes.c_uint()
    off = ctypes.c_uint()
    lib.clang_getSpellingLocation(lib.clang_getRangeStart(rng), ctypes.byref(f),
                                  ctypes.byref(ln), ctypes.byref(col), ctypes.byref(off))
    fn = lib.clang_getFileName(f).decode() if f.value else ""
    return (fn == main_file, ln.value)


def analyze_file(lib, cxindex, entry, full, scratch_dir, resdir):
    """解析一个 .cppm（parse + index 同一 TU）；parse 失败（NULL）抛 RuntimeError。

    cxindex = worker 进程共享的 CXIndex。**绝不调 clang_disposeIndex**（本机实测：
    modules + Indexer 之后 disposeIndex 必段错误，坑 7 附赠；worker 随进程退出回收）。
    """
    cmd = clean_cmd(entry, full, scratch_dir, resdir)
    argv = (ctypes.c_char_p * len(cmd))(*[a.encode() for a in cmd])
    tu = lib.clang_parseTranslationUnit(cxindex, full.encode(), argv, len(cmd),
                                        None, 0, 0)
    if not tu:
        raise RuntimeError("parse 返回 NULL（BMI 缺失/陈旧或 libclang 模块缺陷）")
    try:
        src = open(full, encoding="utf-8", errors="replace").read()  # 仅供 total_lines
        total_lines = len(src.splitlines())
        tu_cur = lib.clang_getTranslationUnitCursor(tu)
        file_lines = code_lines_in_range(
            lib, tu, lib.clang_getCursorExtent(tu_cur), full)

        functions = []
        fields = []
        decls = []
        # record 容器链：cursor 身份组 → 该 record 的 record 祖先数（含自身）。
        # 容器实体先于成员回调，链自洽。export 区域的容器 cursor 是
        # UnexposedDecl（非 record）天然穿透，namespace/TU 同理。
        record_depth = {}    # id → int（顶层类 = 1）
        seen_fn = set()        # functions 指标去重 (name, line)
        seen_decl = set()      # doc decls 去重 (k, n, line)
        pure_decl_names = set()  # 已 emit 的同文件纯声明 (k, n)（decl+def 折叠）

        def record_children(cur):
            """函数定义 cursor 内联扫 lambda（独立条目，含嵌套 lambda）。"""
            stack = list(children_of(lib, cur))
            while stack:
                c = stack.pop()
                # lambda 独立条目：无返回类型契约，两字段按非 expected 记 False
                if kind_spelling(lib, c) == _LAMBDA_SPELLING:
                    ok, ln = _file_line(lib, c, full)
                    if ok:
                        lam_lines = code_lines_in_range(
                            lib, tu, lib.clang_getCursorExtent(c), full)
                        functions.append({"name": "(anonymous lambda)", "line": ln,
                                          "code_lines": len(lam_lines),
                                          "cognitive": cognitive_complexity(lib, tu, c),
                                          "returns_expected": False,
                                          "throws": False})
                stack.extend(children_of(lib, c))

        def cont_chain_depth(cont_id):
            """容器 cursor 的 record 祖先数（含容器自身；容器非 record = 0）。
            record 容器不在登记表 = 跨文件（如 impl 里的 Foo::bar，Foo 在接口
            文件）→ 99：其 method 一律不 emit（旧文本管线同样不 emit）。"""
            if cont_id in record_depth:
                return record_depth[cont_id]
            if cont_id is None:
                return 0
            ck = lib.clang_getCursorKindSpelling(cont_id[0]).decode()
            return 99 if ck in _RECORD_CURSOR_SPELLINGS else 0

        def on_decl(entity, cur):
            if entity["isImplicit"]:
                return
            ok, ln = _file_line(lib, cur, full)
            eid = (cur.kind, cur.xdata, tuple(int(x or 0) for x in cur.data))
            ek = entity["ekind"]
            name = entity["name"]
            if ok and ek in EKIND_RECORD:
                record_depth[eid] = cont_chain_depth(entity["cont_id"]) + 1
            if not ok:
                return

            # 样式指标：函数定义（display name 含签名）+ 其体内 lambda
            if ek in EKIND_FUNCTIONISH and entity["isDef"]:
                disp = lib.clang_getCursorDisplayName(cur).decode() or name
                key = (disp, ln)
                if key not in seen_fn:
                    seen_fn.add(key)
                    returns_exp = _returns_expected(lib, tu, cur)
                    # throw 扫描仅 expected 函数需要（折中版 A）；非 expected 恒 False
                    throws = _body_throws(lib, cur) if returns_exp else False
                    functions.append({
                        "name": disp, "line": ln,
                        "code_lines": len(code_lines_in_range(
                            lib, tu, lib.clang_getCursorExtent(cur), full)),
                        "cognitive": cognitive_complexity(lib, tu, cur),
                        "returns_expected": returns_exp,
                        "throws": throws})
                    record_children(cur)
            if ek == EKIND_FIELD:
                fields.append({"name": name, "line": ln})

            # doc 声明表（与旧文本管线逐条对账后的形态）：
            # - class：顶层与一层嵌套 emit（旧 iter_decls 含 nested），更深吞掉；
            # - enum：容器 record ≤1（类内枚举 emit，嵌套类内枚举旧不 emit）；
            # - function：仅非 record 容器（自由函数/模板，旧文本 namespace 直下）；
            # - method：容器是**本文件顶层 record**（类内声明）；出类 inline 定义
            #   （同文件已有类内声明 → isRedecl）折叠；跨文件 partition 定义
            #   （本文件无先序声明）保留——旧 dump 的正是定义这一条；
            # - 转换函数不 emit（旧 _record_shape 不收 CXXConversion）。
            if not name:
                return
            chain = cont_chain_depth(entity["cont_id"])
            if ek in EKIND_RECORD:
                if entity["isDef"] and chain <= 1:
                    _emit_decl("class", name, ln, entity)
            elif ek == EKIND_ENUM:
                if chain <= 1:
                    _emit_decl("enum", name, ln, entity)
            elif ek == EKIND_FUNCTION:
                if chain == 0:
                    _emit_decl("function", name, ln, entity)
            elif ek in (20, 21, 22, 23):  # 不含 24 ConversionFunction
                if entity["cont_id"] in record_depth and \
                        record_depth[entity["cont_id"]] == 1:
                    _emit_decl("method", name, ln, entity)

        def _emit_decl(k, n, l, entity):
            # method decl+def 折叠：类内纯声明出过 (method, n) → 出类定义条目
            # 丢弃（旧文本 dump 里 namespace 直下的 CXXMethodDecl 不 emit，
            # 类内 decl 一条即真相源）。自由函数不折叠——旧 dump 对同文件
            # decl+def 两条都 emit（loadEnvCubeMapFiles 实测），行为保持。
            if k == "method" and (k, n) in pure_decl_names and entity["isRedecl"]:
                return
            if k == "method" and not entity["isDef"] and not entity["isRedecl"]:
                pure_decl_names.add((k, n))
            if (k, n, l) in seen_decl:
                return
            seen_decl.add((k, n, l))
            decls.append({"k": k, "n": n, "l": l})

        index_decls(lib, cxindex, tu, on_decl)
        return {"total_lines": total_lines,
                "file_code_lines": len(file_lines),
                "functions": functions,
                "fields": fields,
                "decls": decls}
    finally:
        lib.clang_disposeTranslationUnit(tu)
