"""仓库路径遍历与容错读文件（pathlib；相对路径一律仓库根起 POSIX 串）。

read_text 的 OSError→None 约定（style 非阻断、绝不 uncaught 崩构建）见函数
docstring——纯文本规则与 docs 扫描共用。
"""

from pathlib import Path
from typing import Iterator

from config import (DOC_POINTER_DIRS, DOC_POINTER_EXCLUDE_DIRS, DOC_POINTER_FILES,
                    DOC_POINTER_SUFFIXES, ROOT)


def read_text(path: str | Path) -> str | None:
    """容错读源文件文本：非 UTF-8 字节按 replace 解码（坏字节→U+FFFD，token 匹配无害）。

    纯文本规则在主进程跑，单个畸形文件（二进制/非法编码/读失败）**绝不允许**抛异常崩掉
    整个检查——style_check 非阻断，但 uncaught 异常 exit 1 会让 ninja 误判目标失败、-k1
    stall 整条构建（同 worker BrokenProcessPool 教训）。失败返回 None，调用方跳过该文件。
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def rel_posix(path: Path) -> str:
    """绝对/相对 Path → 仓库根起 POSIX 相对串。"""
    return path.resolve().relative_to(Path(ROOT).resolve()).as_posix()


def rgl_cppm_files() -> list[str]:
    """rgl/ 下全部 .cppm（含 rlvk，排除 generated/），仓库相对 POSIX 路径。"""
    base = Path(ROOT) / "rgl"
    out = [
        rel_posix(p)
        for p in base.rglob("*.cppm")
        if "generated" not in p.relative_to(base).parts
    ]
    return sorted(out)


def rgl_tree_files(*suffixes: str) -> list[str]:
    """rgl/ 下给定后缀的文件（排除 generated/），仓库相对 POSIX 路径（排序）。"""
    base = Path(ROOT) / "rgl"
    out = [
        rel_posix(p)
        for p in base.rglob("*")
        if p.is_file()
        and p.suffix in suffixes
        and "generated" not in p.relative_to(base).parts
    ]
    return sorted(out)


def docs_markdown_files() -> list[str]:
    """docs/（含子目录）全部 .md，仓库相对 POSIX 路径（排序）。"""
    base = Path(ROOT) / "docs"
    return sorted(rel_posix(p) for p in base.rglob("*.md") if p.is_file())


def doc_pointer_files() -> list[str]:
    """文档指针扫描面：白名单目录 × 白名单后缀 + 散文件（排除生成物/依赖/日志/文献）。

    与 docs_markdown_files 的区别：那条服务 docs 状态头（只看 .md），本条要覆盖**代码注释里
    的文档指针**（rgl/*.cppm、tests/、tools/、configs/*.lua 都写着 `docs/….md`）。
    """
    out = []
    for d in DOC_POINTER_DIRS:
        base = Path(ROOT) / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if not p.is_file() or p.suffix not in DOC_POINTER_SUFFIXES:
                continue
            rel = rel_posix(p)
            if any(rel.startswith(e) for e in DOC_POINTER_EXCLUDE_DIRS):
                continue
            out.append(rel)
    for f in DOC_POINTER_FILES:
        if (Path(ROOT) / f).is_file():
            out.append(f)
    return sorted(set(out))


def iter_read(paths: Iterator[str] | list[str]) -> Iterator[tuple[str, str]]:
    """逐文件 read_text；读失败跳过。"""
    for rel in paths:
        src = read_text(Path(ROOT) / rel)
        if src is not None:
            yield rel, src
