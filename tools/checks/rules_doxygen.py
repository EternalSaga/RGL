"""Doxygen 注释规则（原 doccheck 移植；decls = metrics Indexer 采集）。

声明首行正上方须紧邻非空 /** */ 块；impl 里接口已声明函数的定义按名豁免
（真相源 = 接口声明处）。判定细节（重载容忍/前向声明锚点）见 has_doxygen_above /
rule_doxygen。
"""

import re

from models import DeclEntry, FileMetrics, Warning


def _unstar(t: str) -> str:
    """去掉 Doxygen 续行前导 * 后取内容。"""
    if t.startswith("*"):
        t = t[1:]
    return t.strip()


def _block_has_content(lines: list[str], start: int, end: int) -> bool:
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


def _is_fwd_line(t: str, name: str) -> bool:
    """同名实体的前向声明行（`export class X;` / 函数原型）——前向声明与
    定义之间可夹空行，注释块挂在前向声明上方（Renderer 先例）。"""
    if not t.endswith(";"):
        return False
    m = re.match(r"^(?:export\s+)?(?:class|struct|enum|union)\s+(\w+)\b", t)
    if m:
        return m.group(1) == name
    return bool(re.search(rf"\b{re.escape(name)}\s*\(", t))


def has_doxygen_above(lines: list[str], start_line: int,
                      max_gap: int = 0, name: str | None = None) -> bool:
    """声明首行（1-based）正上方是否有非空 /** ... */ 块。

    规则：向上可跳过空行、template 引入行（Indexer 的函数 range start 从返回
    类型起算，`template<...>` 行在首行之上而项目惯例把注释挂在 template 前）
    与同名前向声明行（合计至多 max_gap+2 行，覆盖"注释→前向声明→空行→定义"
    形态）；块首行须以 /** 起始（多行块末行为单独 */ 或 `* 文字 */`，
    或单行 /** ... */）；块内容非空。
    """
    i = start_line - 2
    skipped = 0
    budget = max_gap + 2
    while i >= 0:
        t = lines[i].strip()
        if (t == "" or t.startswith("template")
                or (name and _is_fwd_line(t, name))):
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


def interface_declared_names(metrics: FileMetrics) -> set[str]:
    """接口单元声明名集合（自由函数 + 类方法名）——impl 定义处跨查依据。

    注释单一真相源 = 接口声明处；impl 单元里接口已声明函数的定义
    不重复要求 /** 块（名字级匹配，同名 impl 本地函数会被误豁免——罕见，
    属可接受启发式）。
    """
    return {d["n"] for d in metrics.get("decls", []) if d["k"] in ("function", "method")}


def rule_doxygen(
    rel: str, metrics: FileMetrics, lines: list[str],
    max_gap: int = 0, skip_names: set[str] | None = None,
) -> tuple[list[Warning], int, int, int]:
    """单文件 Doxygen 检查：返回 (warnings, checked, unverified, skipped_iface)。

    decls 缺失（老缓存/解析失败）→ 全 0。
    重载容忍（Doxygen 惯例：一组重载共用最上方一个注释块）：同名相邻
    （≤4 行）上方声明有注释 → 本条视为已覆盖。
    """
    decls = [(d["k"], d["n"], d["l"]) for d in metrics.get("decls", [])
             if d["n"] and 1 <= d["l"] <= len(lines)]
    name_lines: dict[str, list[int]] = {}
    for k, n, l in decls:
        if k in ("function", "method"):
            name_lines.setdefault(n, []).append(l)
    warnings: list[Warning] = []
    checked = unverified = skipped = 0
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
        if has_doxygen_above(lines, line, max_gap=max_gap, name=name):
            continue
        covered = any(
            0 < line - other <= 4
            and has_doxygen_above(lines, other, max_gap=max_gap, name=name)
            for other in name_lines.get(name, []))
        if not covered:
            warnings.append(Warning(rel, line,
                f"{kind} '{name}' 缺 Doxygen 注释"
                f"（声明正上方需 /** ... */ 块）"))
    return warnings, checked, unverified, skipped
