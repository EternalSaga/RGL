#!/usr/bin/env python3
"""扫描表驱动的共享骨架（Phase 15 批 15.3 子步 A，stdlib-only）。

**为什么存在**（§3.2 未还账④的教训形态）：湿面六旋钮扫描（`wet_scan.py`，子步 D2）与
SSR 18 臂画质参数扫描（`ssr_scan.py`，本批）需要同三样东西——经 `smoke.py` 出图、解析
`imgqa diffstruct` 的报告值、拼 markdown 表。④ 原文「别复制脚本，给算子加参数」禁的是
一次性脚本住 `/tmp`；同一条纪律对仓库内的两个驱动同样成立——**两处不许各抄一份脚本体**：
diffstruct 的解析正则绑算子口径文案，抄两份就有一处会在文案改动后静默读错（解析失败还会
被读成「这一臂没差异」）。

只住这里的东西：路径常量 / `log` 与 `fail` / 前置检查 / `shot()`（smoke.py 出图）/
`diffstruct()`（算子报告值解析）/ `md_table()`。臂表、阳性锚点判据、专属算子的解析
（hull check 归 `ssr_scan.py` 自己）留在各驱动——**共享的是仪器，不是判据**。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SMOKE = ROOT / "tools" / "smoke.py"
IMGQA = ROOT / "build" / "tools" / "imgqa" / "imgqa"
SHOTS = ROOT / "build" / "rgl" / "screenshots"
SIZE = "1280x720"  # 与红线/子步 C/D1 读数同尺寸，跨批可比


def log(msg: str) -> None:
    """进度走 stderr——stdout 只准是 markdown 产物。"""
    print(msg, file=sys.stderr, flush=True)


def fail(tag: str, msg: str) -> None:
    """响亮失败——扫描表最怕「某一臂静默没跑」，失败必须点名臂与原因。"""
    log(f"[{tag}] FAIL: {msg}")
    sys.exit(1)


def check_prereq(tag: str) -> None:
    if not SMOKE.is_file():
        fail(tag, f"找不到 {SMOKE.relative_to(ROOT)}")
    if not IMGQA.is_file():
        fail(tag, f"找不到 {IMGQA.relative_to(ROOT)}——先 just build-tools")


def shot(tag: str, name: str, ssr: bool, sets: list[str],
    config_base: str | None = None, cam: list[str] | None = None) -> str:
    """经 smoke.py 出一张图（staging 注入，不碰配置副本），返回其 stdout。

    - `cam` = 参考机位三键联动的成对参数（`["--set", 'camera:mode=…', …]`，bistro 驱动传；
      靶配置自带机位就不传）。python 直调 subprocess 用 list 参数，无 just/shell 双引号坑。
    - `config_base` = 靶配置目录（qa/configs/*），None = 仓库 configs/。相对路径按 ROOT 解，
      所以调用方的 cwd 不影响。
    - `sets` = 裸 `KEY=VALUE` 注入（缺省目标文件 = runtime.lua）。
    """
    cmd = [sys.executable, str(SMOKE), "shot", "--name", name, "--size", SIZE]
    if config_base:
        cmd += ["--config-base", config_base]
    if ssr:
        cmd.append("--ssr")
    cmd += cam or []
    for s in sets:
        cmd += ["--set", s]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if proc.returncode != 0:
        log(f"[{tag}] smoke.py 退出码 {proc.returncode}，臂 {name} 输出：\n{proc.stdout}\n{proc.stderr}")
        fail(tag, f"出图失败：{name}（sets={sets} ssr={ssr} config_base={config_base}）")
    return proc.stdout


def diffstruct(tag: str, a: Path, b: Path) -> dict[str, object]:
    """调 imgqa diffstruct 算子并解析报告值（口径在算子本体，这里只搬运不重抄）。

    键：diff(int) / diff_pct / darker / brighter / min_dark / ratio / extent。
    """
    proc = subprocess.run([str(IMGQA), "diffstruct", "--a", str(a), "--b", str(b)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        log(f"[{tag}] diffstruct 退出码 {proc.returncode}：\n{proc.stdout}\n{proc.stderr}")
        fail(tag, f"diffstruct 失败：{a} vs {b}")
    out = proc.stdout
    m = re.search(r"差分像素（线性亮度 \|Δ\|>[\d.]+）: (\d+) = ([\d.]+)%", out)
    if not m:
        fail(tag, f"diffstruct 输出解析失败（口径文案改了？同步改本骨架）：\n{out}")
    got: dict[str, object] = {
        "diff": int(m.group(1)),
        "diff_pct": m.group(2),
        "darker": "0", "brighter": "0", "min_dark": "—", "ratio": "—", "extent": "",
    }
    if (m := re.search(r"变暗（Δ<0，线性亮度域）\s*: (\d+)（占差分 ([\d.]+)%，占画面 ([\d.]+)%）", out)):
        got["darker"] = m.group(1)
    if (m := re.search(r"变亮（Δ>0）\s*: (\d+)；变暗像素 Δ 最小值 = (\S+)", out)):
        got["brighter"], got["min_dark"] = m.group(1), m.group(2)
    if (m := re.search(r"p10/p50/p90 = (\S+) / (\S+) / (\S+)；p90/p10 = (\S+)", out)):
        got["ratio"] = m.group(4)
    if (m := re.search(r"^画面 (\d+) x (\d+)", out, re.M)):
        got["extent"] = f"{m.group(1)}x{m.group(2)}"
    return got


def md_table(headers: list[str], rows: list[list[str]]) -> str:
    """markdown 表（表头 + 分隔行 + 数据行）。列宽不补齐——GitHub 渲染不需要。"""
    lines = ["| " + " | ".join(headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|"]
    lines += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(lines)
