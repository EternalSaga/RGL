#!/usr/bin/env python3
"""SSR 18 臂画质参数扫描表复跑驱动（Phase 15 批 15.3 子步 A，还 §3.2 未还账④，stdlib-only）。

**为什么存在**：§3.2 的 18 臂扫描表（「每枚旋钮一张扫描表 + 一次消融阳性锚点」的实测）
由一次性脚本驱动、住在 `/tmp` ⟹ 表会腐烂。本脚本把驱动落进仓库。靶 = `ssr_rig` 闭式
反射靶（`just ssr-check` 的通路）：每臂出 off/on 两图喂 `tools/ssr_hull_check.py`
（判据算子的单一真相源，这里只搬运读数不重抄）；共享骨架（smoke 出图 / diffstruct 解析 /
markdown 表）住 `tools/scan_common.py`，与 `wet_scan.py` 同源（④ 原文「别复制脚本」）。

臂表 = 五枚画质系数（configs/runtime.lua `ssr` 块：max_distance_radii / thickness_px /
bias_px / rough_full / rough_cutoff）× 档位，共 18 臂（含出厂默认），与 §3.2 原表逐臂
对应（含反向档与退化档两条不变式违反臂）。

**阳性锚点 = 非默认臂的覆盖率或差分与默认臂不同**：hull check 读数（凸包内变亮数 / 覆盖率 /
变暗数 / Δp50）任一不同，或该臂 ON 图对默认臂 ON 图的 diffstruct 差分 > 0 px。
不能只拿「覆盖率不同」判——原表里 max_distance 0.52、rough_cutoff 0.80 这类臂覆盖率
本就饱和相同，差分在 Δ 亮度或凸包外的量化位上。

前置：`qa/configs/ssr_rig/runtime.lua` 的 `ssr` 块**显式写出五枚键**（默认值 = SsrTuning
出厂值，行为零变化）——patch_lua 的「键命中 ≠ 1 即 FAIL」护栏要求键在场，与 `enabled`
写在靶配置里的理由同一条：靶配置不吃隐式默认。

用法（复跑命令示例）:
    python3 tools/ssr_scan.py                        # 全表（18 臂 × off/on 两图）→ markdown 到 stdout
    python3 tools/ssr_scan.py --only thickness_px=0  # 只跑一臂（消融档抽查）

进度与失败详情走 stderr，stdout 只有 markdown。
退出码: 0 = 全臂出数且**旋钮级**锚点全 PASS（臂级 FAIL 若来自语义等价臂是正确读数，
见输出里的解释；--only 模式仍按该臂的臂级锚点给码）· 1 = 前置不满足 / 出图失败 / 旋钮级 FAIL。
"""

from __future__ import annotations

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

import scan_common as sc

HULL = sc.ROOT / "tools" / "ssr_hull_check.py"
CONFIG_BASE = "qa/configs/ssr_rig"  # 相对 ROOT（sc.shot 按 ROOT 解）
TAG = "ssr_scan"

# 臂表 = §3.2 原 18 臂表逐臂照搬（出厂默认 + 17 档）。sets = 注入 runtime.lua 的裸键。
# (label, sets, note)；label 同时是 --only 的选择子与出图 slug 的来源。
ARMS: list[tuple[str, list[str], str]] = [
    ("max_distance_radii=0.03", ["max_distance_radii=0.03"], ""),
    ("max_distance_radii=0.13", ["max_distance_radii=0.13"], ""),
    ("max_distance_radii=0.52", ["max_distance_radii=0.52"], "（= 默认值，饱和标定臂）"),
    ("thickness_px=0", ["thickness_px=0"], "（消融档）"),
    ("thickness_px=1.0", ["thickness_px=1.0"], ""),
    ("thickness_px=3.0", ["thickness_px=3.0"], ""),
    ("thickness_px=12.0", ["thickness_px=12.0"], ""),
    ("bias_px=0.3", ["bias_px=0.3"], ""),
    ("bias_px=1.2", ["bias_px=1.2"], ""),
    ("bias_px=8.0", ["bias_px=8.0"], "（消融档：bias ≥ thickness）"),
    ("rough_full=0.05", ["rough_full=0.05"], ""),
    ("rough_full=0.30", ["rough_full=0.30"], ""),
    ("rough_cutoff=0.13", ["rough_cutoff=0.13"], "（门关掉死）"),
    ("rough_cutoff=0.25", ["rough_cutoff=0.25"], ""),
    ("rough_cutoff=0.80", ["rough_cutoff=0.80"], ""),
    ("rough_full=0.40,rough_cutoff=0.15", ["rough_full=0.40", "rough_cutoff=0.15"],
        "（反向：rough_full > rough_cutoff）"),
    ("rough_full=0.2,rough_cutoff=0.2", ["rough_full=0.2", "rough_cutoff=0.2"],
        "（退化：rough_full = rough_cutoff）"),
]


def log(msg: str) -> None:
    sc.log(msg)


def slug(label: str) -> str:
    """臂 label → 文件名安全 slug（smoke.py 的 --name 只准文件名）。= → 空、, → x、. → p。"""
    return "s3_" + label.replace("=", "_").replace(",", "x").replace(".", "p")


def hull_check(off: Path, on: Path) -> dict[str, object]:
    """喂现成两图给闭式靶判据算子并解析读数。

    rc=1 = 判据 FAIL（消融/反向臂的**预期结果**，不是运行失败）；其余非 0 rc 才是失败。
    解析口径 = ssr_hull_check 的打印文案；文案改了同步改这里（解析失败即 FAIL，不静默）。
    """
    proc = subprocess.run(
        [sys.executable, str(HULL), "--off", str(off), "--on", str(on)],
        capture_output=True, text=True, cwd=sc.ROOT)
    if proc.returncode not in (0, 1):
        sc.log(f"[{TAG}] hull check 退出码 {proc.returncode}：\n{proc.stdout}\n{proc.stderr}")
        sc.fail(TAG, f"ssr_hull_check 运行失败：{off.name} vs {on.name}")
    out = proc.stdout
    m = re.search(r"变亮 (\d+) \(([\d.]+)%\) · 变暗 (\d+) · Δ线性亮度 p50=(\S+)", out)
    if not m:
        sc.fail(TAG, f"hull check 输出解析失败（文案改了？同步改本脚本）：\n{out}")
    got: dict[str, object] = {
        "brighter": int(m.group(1)), "cov": m.group(2), "darker": int(m.group(3)),
        "p50": m.group(4), "leak": -1, "verdict_a": "?", "verdict_b": "?",
    }
    if (m := re.search(r"豁免后泄漏 (\d+) px", out)):
        got["leak"] = int(m.group(1))
    if (m := re.search(r"判据 A [^\n]*: (PASS|FAIL)", out)):
        got["verdict_a"] = m.group(1)
    if (m := re.search(r"判据 B [^\n]*: (PASS|FAIL)", out)):
        got["verdict_b"] = m.group(1)
    if got["leak"] == -1:
        sc.fail(TAG, f"hull check 泄漏行解析失败：\n{out}")
    return got


def run_arm(name: str, sets: list[str], default_on: Path) -> dict[str, object]:
    """一臂 = off/on 两图 → hull 读数 + 对默认臂 ON 图的 diffstruct 差分（锚点用）。"""
    sc.shot(TAG, name + "_off", False, sets, config_base=CONFIG_BASE)
    sc.shot(TAG, name + "_on", True, sets, config_base=CONFIG_BASE)
    off, on = sc.SHOTS / f"{name}_off.png", sc.SHOTS / f"{name}_on.png"
    for p in (off, on):
        if not p.is_file():
            sc.fail(TAG, f"smoke.py 报成功但 {p} 不在")
    got = hull_check(off, on)
    got["img_diff"] = int(sc.diffstruct(TAG, on, default_on)["diff"])
    return got


def main() -> int:
    ap = argparse.ArgumentParser(description="SSR 18 臂画质参数扫描表复跑（15.3 子步 A，还 §3.2④）")
    ap.add_argument("--only", metavar="LABEL", default=None,
                    help="只跑一臂（label 见 ARMS，如 thickness_px=0；默认档写 default）")
    args = ap.parse_args()
    sc.check_prereq(TAG)
    if not HULL.is_file():
        sc.fail(TAG, f"找不到 {HULL.relative_to(sc.ROOT)}")

    today = datetime.date.today().isoformat()

    def arm_row(label: str, sets: list[str], note: str, base_on: Path,
        base_stat: dict[str, object]) -> tuple[list[str], bool]:
        name = slug(label)
        log(f"[{TAG}] 臂 {label} …")
        r = run_arm(name, sets, base_on)
        # 阳性锚点：hull 读数任一维与默认臂不同，或 ON 图差分 > 0 px（覆盖率饱和臂靠后者）。
        changed = (int(r["brighter"]) != int(base_stat["brighter"])
            or str(r["cov"]) != str(base_stat["cov"])
            or int(r["darker"]) != int(base_stat["darker"])
            or str(r["p50"]) != str(base_stat["p50"])
            or int(r["img_diff"]) > 0)
        row = [label + note, name, f"{r['brighter']}（{r['cov']}%）", str(r["darker"]),
            str(r["p50"]), str(r["leak"]), f"{r['verdict_a']}/{r['verdict_b']}",
            str(r["img_diff"]), "PASS" if changed else "**FAIL**"]
        return row, changed

    # 默认臂（出厂 ssr 块 = SsrTuning 默认值）先跑，既是基线也是锚点参照。
    log(f"[{TAG}] 默认臂（出厂 ssr 块，靶 {CONFIG_BASE}）…")
    default_name = slug("default")
    sc.shot(TAG, default_name + "_off", False, [], config_base=CONFIG_BASE)
    gpu_out = sc.shot(TAG, default_name + "_on", True, [], config_base=CONFIG_BASE)
    gpu = "未取到"
    if m := re.search(r"GPU    : (.+)", gpu_out):
        gpu = m.group(1).strip()
    default_on = sc.SHOTS / f"{default_name}_on.png"
    base_hull = hull_check(sc.SHOTS / f"{default_name}_off.png", default_on)

    if args.only:
        if args.only == "default":
            sc.fail(TAG, "--only default 无意义（默认档 = 基线本身）；要复现默认读数就跑全表")
        hit = [a for a in ARMS if a[0] == args.only]
        if not hit:
            sc.fail(TAG, f"--only {args.only!r} 不在臂表里（臂表见 ARMS）")
        label, sets, note = hit[0]
        row, changed = arm_row(label, sets, note, default_on, base_hull)
        print(f"# SSR 18 臂扫描 · 单臂复跑（{label}，{today}）\n")
        print(sc.md_table(["臂", "出图", "凸包内变亮（覆盖率）", "变暗", "Δp50", "泄漏",
            "判据A/B", "ON图差分", "锚点"], [row]))
        return 0 if changed else 1

    rows: list[list[str]] = [[
        "**出厂默认**", default_name,
        f"{base_hull['brighter']}（{base_hull['cov']}%）", str(base_hull["darker"]),
        str(base_hull["p50"]), str(base_hull["leak"]),
        f"{base_hull['verdict_a']}/{base_hull['verdict_b']}", "基线", "—"]]
    anchors: list[tuple[str, bool]] = []
    for label, sets, note in ARMS:
        row, changed = arm_row(label, sets, note, default_on, base_hull)
        anchors.append((label, changed))
        rows.append(row)

    n_fail = sum(1 for _, ok in anchors if not ok)
    # 旋钮级锚点 = §4 原表的判据本体「每枚旋钮一张扫描表 + 一次消融阳性锚点」：一枚旋钮
    # 只要有任一臂与默认不同即接线成立。**臂级 FAIL 不必然 = 旋钮坏了**——语义等价臂
    # （= 默认值本身 / 靶粗糙度单值下门控同值的档）ON 图逐位相同，锚点按定义红，
    # 旋钮级绿才是「配置改了没反应」这条护栏的正面证据。
    knobs: dict[str, bool] = {}
    for (label, ok), (lab0, _, _) in zip(anchors, ARMS):
        knob = lab0.split("=")[0].split(",")[0]
        knobs[knob] = knobs.get(knob, False) or ok
    knob_fail = [k for k, v in knobs.items() if not v]
    print("# SSR 画质参数 18 臂扫描表 · 复跑（Phase 15 批 15.3 子步 A，还 §3.2 未还账④）\n")
    print(f"- 靶 = `ssr_rig`（配置基准 `{CONFIG_BASE}`，湿面钉 strength=0、aa 锁 off）· "
          f"尺寸 {sc.SIZE} · 日期 {today}")
    print(f"- 设备：{gpu}")
    print("- 每臂 = off/on 两图喂 `tools/ssr_hull_check.py`（判据 A 覆盖率 / 判据 B 泄漏的"
          "口径真相源在该算子）；**阳性锚点** = 非默认臂的 hull 读数任一维与默认臂不同，"
          "或 ON 图对默认臂 ON 图的 diffstruct 差分 > 0 px（覆盖率饱和臂靠后者）\n")
    print(sc.md_table(["臂", "出图", "凸包内变亮（覆盖率）", "变暗", "Δp50", "泄漏",
        "判据A/B", "ON图差分", "锚点"], rows))
    print(f"\n臂级阳性锚点：{len(anchors) - n_fail}/{len(anchors)} PASS"
          + ("；FAIL 臂：" + ", ".join(k for k, ok in anchors if not ok) if n_fail else ""))
    print("（臂级 FAIL ≠ 旋钮坏：上列 FAIL 臂的 ON 图与默认臂**逐位相同** = 语义等价臂"
          "（= 默认值本身，或靶镜面 roughness 恒 0.122 下门控取值不变的档）。"
          "「每枚旋钮动得了画面」的判据本体在下面的旋钮级汇总。)")
    print(f"\n旋钮级阳性锚点（§4 原表判据本体）：{len(knobs) - len(knob_fail)}/{len(knobs)} PASS"
          + ("" if not knob_fail else f"；FAIL 旋钮：{', '.join(knob_fail)}"))
    print("\n复跑：`python3 tools/ssr_scan.py`（全表）· "
          "`python3 tools/ssr_scan.py --only thickness_px=0`（单臂）")
    # 退出码按**旋钮级**：语义等价臂的臂级 FAIL 是正确读数，不该让复跑永远红。
    return 1 if knob_fail else 0


if __name__ == "__main__":
    sys.exit(main())
