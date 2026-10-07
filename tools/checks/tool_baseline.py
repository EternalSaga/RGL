"""工具侧样式基线（「只准降不准升」，非阻断）。

为什么要有基线：tools/imgqa 一次性纳入样式检查时存量 74 条，不可能一批拆完。
没有基线，"74" 会慢慢漂成 "90" 而没人知道是哪天多的。基线把存量钉死：

- 当前 > 基线 → severity "error"（main 侧红粗体；点名"基线上升 N > M"，处置是
  **拆函数/拆文件，不是改基线**）；
- 当前 < 基线 → severity "warning"（提示可下调，一条 `--update-tool-baseline` 收账
  ——不许悄悄漂移，也不许把"已经降了"这件事只留在脑子里）；
- 文件不在基线里且当前 > 0 → error（新文件必须显式登记，防止新增文件白拿额度）。

文件形态见 config.TOOL_BASELINE_FILE 注释；**按文件记**而非全局一个数——否则
「A 拆掉 3 条、B 加 3 条」会互相抵消，上升被掩盖。

未解析的文件**不参与比对**（clean 后 imgqa 尚未构建、或 parse 失败）：把"没数"
当成 0 条会误报"可下调基线"，那是假绿的镜像——假降。
"""

import json
from pathlib import Path
from typing import NamedTuple

from config import ROOT, TOOL_BASELINE_FILE
from models import Warning

BASELINE_TOTAL_KEY = "total"
BASELINE_FILES_KEY = "files"
BASELINE_COMMENT_KEY = "_comment"
BASELINE_COMMENT = (
    "工具侧（tools/imgqa）样式基线：每个文件允许的样式警告条数。"
    "只准降不准升——上升的处置是拆函数/拆文件，不是改这里的数字。"
    "下调后用 `python3 tools/checks/main.py --update-tool-baseline` 重写本文件。")


class BaselineReport(NamedTuple):
    """一次基线比对的结果（main 用它出警告 + 汇总行）。"""

    warnings: list[Warning]
    current: int     # 参与比对的文件当前条数之和
    allowed: int     # 同一批文件在基线里的允许条数之和（未登记按 0）
    compared: int    # 参与比对的文件数（= 已解析的工具 TU 数）
    unparsed: int    # 未解析因而未参与比对的文件数（不进 current/allowed）


def baseline_path() -> str:
    """基线文件的绝对路径（config.TOOL_BASELINE_FILE 相对仓库根）。"""
    return str(Path(ROOT) / TOOL_BASELINE_FILE)


def load_baseline(path: str | None = None) -> dict[str, int]:
    """读基线 {rel: 允许条数}；文件缺失/坏 JSON/格式不符 → 空表（调用方按
    「未登记」处理，宁可吵，不可默默放行）。"""
    p = path or baseline_path()
    try:
        with open(p, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    files = data.get(BASELINE_FILES_KEY)
    if not isinstance(files, dict):
        return {}
    return {str(k): int(v) for k, v in files.items() if isinstance(v, int)}


def write_baseline(counts: dict[str, int], path: str | None = None) -> str:
    """按当前条数重写基线（下调收账用）；返回写入路径。"""
    p = path or baseline_path()
    files = {k: int(v) for k, v in sorted(counts.items())}
    data = {
        BASELINE_COMMENT_KEY: BASELINE_COMMENT,
        BASELINE_TOTAL_KEY: sum(files.values()),
        BASELINE_FILES_KEY: files,
    }
    Path(p).parent.mkdir(parents=True, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return p


def compare(counts: dict[str, int], baseline: dict[str, int],
            unparsed: int = 0) -> BaselineReport:
    """逐文件比对当前条数与基线（counts 只含**已解析**的工具单元）。

    @param counts rel → 当前样式警告条数
    @param baseline rel → 允许条数（load_baseline 结果）
    @param unparsed 未解析的工具单元数（只上报，不比对）
    """
    warnings: list[Warning] = []
    current = allowed = 0
    for rel in sorted(set(counts) | set(baseline)):
        cur, base = counts.get(rel, 0), baseline.get(rel, 0)
        if rel not in counts:
            continue  # 未解析（或已从磁盘消失）：不参与比对，见模块 docstring
        current += cur
        allowed += base
        if rel not in baseline:
            if cur > 0:
                warnings.append(Warning(rel, 0,
                    f"工具侧样式基线未登记：当前 {cur} 条（新文件必须显式登记进 "
                    f"{TOOL_BASELINE_FILE}——跑 --update-tool-baseline 或手加）",
                    "error"))
            continue
        if cur > base:
            warnings.append(Warning(rel, 0,
                f"工具侧样式基线上升 {cur} > {base}——处置是拆函数/拆文件，"
                f"**不是**改基线（基线只准降；确实降了才用 --update-tool-baseline）",
                "error"))
        elif cur < base:
            warnings.append(Warning(rel, 0,
                f"工具侧样式基线可下调 {cur} < {base}——用 --update-tool-baseline "
                f"收账（悄悄漂移等于没有基线）"))
    return BaselineReport(warnings, current, allowed, len(counts), unparsed)
