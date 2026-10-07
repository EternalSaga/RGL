"""libclang 指标规则（消费 metrics.analyze_file 缓存指标）。

判定顺序（evaluate_metrics）：文件行 → 逐函数[行/认知/瘦身/expected throw] →
成员命名；同 (rel,line) 并列时保持此相对顺序——排序在 main 里按 (rel,line)
稳定排。阈值经 types.Thresholds 传入；规则开关经 disabled 参数（工具侧关
impl_slim，默认空集不影响 rgl 侧）。
"""

from pathlib import Path

from models import (FieldMetrics, FileMetrics, FunctionMetrics, Thresholds,
                    Warning)


def rule_file_lines(rel: str, metrics: FileMetrics, threshold: int) -> list[Warning]:
    """文件代码行超阈 → 提示拆分文件/模块。"""
    if metrics["file_code_lines"] > threshold:
        return [Warning(rel, 0,
            f"文件代码行 {metrics['file_code_lines']} > {threshold}"
            f"（去注释空行，总行数 {metrics['total_lines']}）——应拆分文件/模块")]
    return []


def rule_function_lines(rel: str, fn: FunctionMetrics, threshold: int) -> list[Warning]:
    """函数代码行超阈 → 提示拆分函数。"""
    if fn["code_lines"] > threshold:
        return [Warning(rel, fn["line"],
            f"函数 '{fn['name']}' 代码行 {fn['code_lines']} > {threshold}"
            f"（去注释空行）——应拆分为更小函数")]
    return []


def rule_cognitive(rel: str, fn: FunctionMetrics, threshold: int,
                   error_threshold: int) -> list[Warning]:
    """函数认知复杂度超阈 → 提示拆函数（降低嵌套/早返回/抽函数）。

    >error_threshold = severity "error"（main 侧红粗体高亮；仍不阻断构建），
    >threshold = 普通 warning。
    """
    if fn["cognitive"] > error_threshold:
        return [Warning(rel, fn["line"],
            f"函数 '{fn['name']}' 认知复杂度 {fn['cognitive']} > {error_threshold}"
            f"——应拆分为更小函数（降低嵌套/早返回/抽函数）", "error")]
    if fn["cognitive"] > threshold:
        return [Warning(rel, fn["line"],
            f"函数 '{fn['name']}' 认知复杂度 {fn['cognitive']} > {threshold}"
            f"——应拆分为更小函数（降低嵌套/早返回/抽函数）")]
    return []


def rule_impl_slim(rel: str, fn: FunctionMetrics, threshold: int) -> list[Warning]:
    """接口瘦身（AGENTS.md「接口瘦身」）：非 _impl 接口单元里 >阈值 代码行的函数
    定义应迁入 xxx_impl.cppm（接口 BMI 不含函数体，所有 importer 受益）。"""
    if not rel.endswith("_impl.cppm") and fn["code_lines"] > threshold:
        target = Path(rel).stem + "_impl.cppm"
        return [Warning(rel, fn["line"],
            f"函数 '{fn['name']}' 函数体代码行 {fn['code_lines']} > {threshold}"
            f"——接口瘦身：实现应迁入 {target}（接口 BMI 不含函数体）")]
    return []


def rule_member_naming(rel: str, fields: list[FieldMetrics]) -> list[Warning]:
    """成员变量命名：_/m_ 前缀或 _ 后缀 → 自然命名。"""
    warnings = []
    for fd in fields:
        n = fd["name"]
        if n.startswith("_") or n.startswith("m_") or n.endswith("_"):
            warnings.append(Warning(rel, fd["line"],
                f"成员变量 '{n}'——成员命名不加 _/m_ 前缀、不加 _ 后缀（自然命名）"))
    return warnings


def rule_expected_throw(rel: str, fn: FunctionMetrics) -> list[Warning]:
    """折中版 A：返回 std::expected 的函数体内禁 throw（双通道防混用）。

    非构造/启动 fail-fast 路径请改用 std::expected 传播错误；构造函数无返回
    类型不触发本规则。见 docs/风格检查规则.md §2.3。
    """
    if fn.get("returns_expected") and fn.get("throws"):
        return [Warning(rel, fn["line"],
            f"函数 '{fn['name']}' 返回 std::expected 却在体内 throw——"
            f"双通道混用；非构造/启动 fail-fast 路径请改用 expected 传播错误")]
    return []


def evaluate_metrics(rel: str, metrics: FileMetrics, thresholds: Thresholds,
                     disabled: frozenset[str] = frozenset()) -> list[Warning]:
    """单文件全部 libclang 指标规则（顺序见模块 docstring）。

    @param disabled 关闭的规则名集合（去掉 rule_ 前缀的名字）。**默认空集 = rgl 侧
        行为一字不变**。工具侧传 config.TOOL_DISABLED_RULES（当前只有 impl_slim：
        接口瘦身是 C++20 modules 语义，对纯 .cpp 工具无意义）。可关的名字目前
        只有 impl_slim——其余规则（行/认知/命名/expected）对工具代码同样成立。
    """
    warnings = rule_file_lines(rel, metrics, thresholds.file)
    for fn in metrics["functions"]:
        warnings += rule_function_lines(rel, fn, thresholds.func)
        warnings += rule_cognitive(rel, fn, thresholds.cognitive,
                                   thresholds.cognitive_error)
        if "impl_slim" not in disabled:
            warnings += rule_impl_slim(rel, fn, thresholds.impl)
        warnings += rule_expected_throw(rel, fn)
    warnings += rule_member_naming(rel, metrics["fields"])
    return warnings
