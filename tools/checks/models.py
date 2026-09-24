"""checks 共享类型（Warning 载体 / 阈值 / metrics TypedDict 钉缓存 schema）。

缓存 JSON 结构与 CACHE_VERSION 见 config.py；本模块只做 Python 侧类型化，
不参与缓存 key——schema 不变时不得因类型注解 bump 版本。
"""

from dataclasses import dataclass
from typing import NamedTuple, TypedDict

from config import (DEFAULT_COGNITIVE_THRESHOLD, DEFAULT_FILE_THRESHOLD,
                    DEFAULT_FUNC_THRESHOLD, DEFAULT_IMPL_THRESHOLD)


class Warning(NamedTuple):
    """一条检查警告：仓库相对路径 + 1-based 行号（0 = 文件级）+ 人类可读消息。"""

    rel: str
    line: int
    msg: str


@dataclass(frozen=True, slots=True)
class Thresholds:
    """四条数值阈值（CLI 解析后传入规则的统一载体）。"""

    file: int = DEFAULT_FILE_THRESHOLD
    func: int = DEFAULT_FUNC_THRESHOLD
    cognitive: int = DEFAULT_COGNITIVE_THRESHOLD
    impl: int = DEFAULT_IMPL_THRESHOLD


class FunctionMetrics(TypedDict):
    """单函数指标（metrics.analyze_file 产出；含 lambda 独立条目）。"""

    name: str
    line: int
    code_lines: int
    cognitive: int
    returns_expected: bool
    throws: bool


class FieldMetrics(TypedDict):
    """成员字段指标。"""

    name: str
    line: int


class DeclEntry(TypedDict):
    """Doxygen 声明表条目（k=kind 拼写, n=name, l=line）。"""

    k: str
    n: str
    l: int


class FileMetrics(TypedDict):
    """单文件缓存指标（schema = metrics.py 文件头 / config.CACHE_VERSION）。"""

    total_lines: int
    file_code_lines: int
    functions: list[FunctionMetrics]
    fields: list[FieldMetrics]
    decls: list[DeclEntry]


# compile_commands.json 单条（cmdclean/main 消费）
type CompileEntry = dict[str, object]
