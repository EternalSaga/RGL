"""单文件指标：解析一个 .cppm（复用其构建编译命令），产出结构与复杂度指标。

指标结构（缓存 JSON 的 schema，CACHE_VERSION 随结构变化递增）：
    {total_lines, file_code_lines,
     functions: [{name, line, code_lines, cognitive}],
     fields:    [{name, line}]}
代码行 = 去注释与空行，token 级精确（libclang tokenize）。
"""

import ctypes

from clangbind import code_lines_in_range, cognitive_complexity, collect_decls
from cmdclean import clean_cmd


def analyze_file(lib, entry, full, scratch_dir, resdir):
    """解析一个 .cppm 并算全部指标；parse 失败（NULL）抛 RuntimeError。"""
    cmd = clean_cmd(entry, full, scratch_dir, resdir)
    idx = lib.clang_createIndex(0, 0)
    argv = (ctypes.c_char_p * len(cmd))(*[a.encode() for a in cmd])
    tu = lib.clang_parseTranslationUnit(idx, full.encode(), argv, len(cmd), None, 0, 0)
    if not tu:
        raise RuntimeError("parse 返回 NULL（BMI 缺失/陈旧或 libclang 模块缺陷）")
    try:
        src = open(full, encoding="utf-8", errors="replace").read()  # 仅供 total_lines；坏字节不抛
        total_lines = len(src.splitlines())
        tu_cur = lib.clang_getTranslationUnitCursor(tu)
        file_lines = code_lines_in_range(lib, tu, lib.clang_getCursorExtent(tu_cur), full)
        funcs, fields = collect_decls(lib, tu_cur, full)
        functions = []
        for name, ln, cur in funcs:
            flines = code_lines_in_range(lib, tu, lib.clang_getCursorExtent(cur), full)
            functions.append({"name": name, "line": ln,
                              "code_lines": len(flines),
                              "cognitive": cognitive_complexity(lib, tu, cur)})
        return {"total_lines": total_lines,
                "file_code_lines": len(file_lines),
                "functions": functions,
                "fields": [{"name": n, "line": l} for n, l in fields]}
    finally:
        lib.clang_disposeTranslationUnit(tu)
