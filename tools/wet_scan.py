#!/usr/bin/env python3
"""湿面六旋钮扫描表驱动（Phase 15 批 15.2 子步 D2，stdlib-only）。

**为什么存在**（§3.2 未还账④的教训形态）：18 臂扫描表曾由一次性脚本驱动、住在 `/tmp`，
表会腐烂——将来动参数后无法重验「每枚旋钮仍动得了画面」这条反「配置改了没反应」的护栏。
本脚本把湿面六旋钮的扫描驱动落进仓库：出图走 `tools/smoke.py shot`（staging 注入，不碰
配置副本），指标走 `build/tools/imgqa/imgqa diffstruct`（判据口径单一真相源在算子，这里
不重抄）。**只加工具与读数，零引擎/零 shader 改动**。

每臂 = 参考机位（configs/camera.lua 注释段的 SSR 参考机位，湿材质占 22.4%）+ SSR 开
（交付形态）+ 一枚旋钮换档，其余保持出厂值；指标 = 与默认档（出厂 wet 块原值）的
diffstruct 读数。**阳性锚点：非默认臂差分必须 > 0 px**（旋钮不动像素 = 参数没接上）；
`strength=0` 臂 = 消融档（它 vs 默认 = 湿面全效的规模读数）。

另出**结构对比度前后对照**（判据拍板本体 → docs/phase15_plan.md §2 批次表 15.2 行）：
wet 开/关 × SSR 开/关 四图，两对 diffstruct（各 SSR开−SSR关），对照 p90/p10 与 Δ<0。

共享骨架（smoke 出图 / diffstruct 解析 / markdown 表 / log·fail·前置检查）住在
`tools/scan_common.py`——与 `ssr_scan.py`（18 臂表驱动）同源，两处不各抄一份（④ 纪律）。

用法（复跑命令示例）:
    python3 tools/wet_scan.py                          # 全表（13 臂 + 2 张对照补图）→ markdown 到 stdout
    python3 tools/wet_scan.py --only dark=0.4 --twice  # 复现性自证：同一臂连跑两次对同一基线读数
    python3 tools/wet_scan.py --only strength=0        # 只跑一臂（出图与读数照常打印）

进度与失败详情走 stderr，stdout 只有 markdown（可直接 `> scan.md` 重定向）。
退出码: 0 = 全表出数且阳性锚点全 PASS · 1 = 前置不满足 / 出图失败 / 有臂锚点 FAIL。
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
from pathlib import Path

import scan_common as sc

ROOT = sc.ROOT
SHOTS = sc.SHOTS
SIZE = sc.SIZE
TAG = "wet_scan"

# 参考机位三键联动（configs/camera.lua 注释段，2026-10-10 用户提供，街道平视）。
# 必须 mode/position/target 三键一起给：target 仅 trackball 消费（fps 模式写 target
# = 静默到不了这个视角）。python 直调 subprocess 用 list 参数，无 just/shell 双引号坑。
REF_CAM = [
    "--set", 'camera:mode="trackball"',
    "--set", "camera:position={ -0.075051, 0.060435, -0.021944 }",
    "--set", "camera:target={ -0.078430, -0.013424, 0.192736 }",
]

# 出厂 wet 块（configs/runtime.lua 唯一真相源，缺键即这份）——只进表头说明，不注入。
WET_DEFAULT = ("strength=1.0 / dark=0.6 / rough_lo=0.05 / rough_hi=0.35 "
               "/ ripple_amp=0.3 / puddle_freq_radii=12.0")

# 臂表：每旋钮 3 档 = 默认值本身（= 默认臂，不重复出图）+ 2 新档。全部 SSR 开。
# 顺序即出表顺序；strength=0 是消融档（表里单独标注）。
KNOBS: list[tuple[str, list[str]]] = [
    ("strength", ["0", "0.5"]),
    ("dark", ["0.4", "0.8"]),
    ("rough_lo", ["0.02", "0.1"]),
    ("rough_hi", ["0.25", "0.5"]),
    ("ripple_amp", ["0", "0.6"]),
    ("puddle_freq_radii", ["6", "24"]),
]


def log(msg: str) -> None:
    """进度走 stderr——stdout 只准是 markdown 产物。"""
    sc.log(msg)


def fail(msg: str) -> None:
    sc.fail(TAG, msg)


def shot(name: str, ssr: bool, sets: list[str]) -> str:
    """经 smoke.py 出一张图（staging 注入，不碰配置副本），返回其 stdout。

    机位三键在**每个臂**都带：默认档也拍参考机位，臂对默认档的差分才同机位可比。
    """
    return sc.shot(TAG, name, ssr, sets, cam=REF_CAM)


def diffstruct(a: Path, b: Path) -> dict[str, object]:
    return sc.diffstruct(TAG, a, b)


def run_arm(name: str, ssr: bool, sets: list[str], base_png: Path) -> dict[str, object]:
    shot(name, ssr, sets)
    png = SHOTS / f"{name}.png"
    if not png.is_file():
        fail(f"smoke.py 报成功但 {png} 不在")
    return diffstruct(png, base_png)


def main() -> int:
    ap = argparse.ArgumentParser(description="湿面六旋钮扫描表（15.2 子步 D2）")
    ap.add_argument("--only", metavar="KEY=VAL", default=None,
                    help="只跑一臂（如 dark=0.4 / strength=0；默认档写 default）")
    ap.add_argument("--twice", action="store_true",
                    help="配合 --only：同一臂连跑两次、各对默认档读数并打印差值（复现性自证）")
    args = ap.parse_args()
    sc.check_prereq(TAG)

    today = datetime.date.today().isoformat()
    rows: list[list[str]] = []
    anchors: list[tuple[str, bool]] = []
    gpu = "未取到"

    def arm_sets(key: str, value: str) -> list[str]:
        return [f"{key}={value}"]

    # ── 单臂模式（复现性自证 / 调试）──
    if args.only:
        if args.only == "default":
            fail("--only default 无意义（默认档 = 基线本身）；要复现默认读数就跑全表")
        key, _, value = args.only.partition("=")
        if not value or (key, value) not in [(k, v) for k, vs in KNOBS for v in vs]:
            fail(f"--only {args.only!r} 不在臂表里（臂表见 KNOBS，默认档写 default）")
        name = slug(key, value)
        log(f"[{TAG}] 基线臂 default（SSR 开，参考机位）…")
        shot("d2_default", True, [])
        base = SHOTS / "d2_default.png"
        runs = [name, name + "_r2"] if args.twice else [name]
        readings = [run_arm(n, True, arm_sets(key, value), base) for n in runs]
        print(f"# 湿面扫描 · 单臂复跑（{key}={value}，{today}）\n")
        for n, r in zip(runs, readings):
            print(f"| {n} | 差分 {r['diff']} px = {r['diff_pct']}% | Δ<0 {r['darker']} | "
                  f"Δ>0 {r['brighter']} | p90/p10 {r['ratio']} |")
        if args.twice and len(readings) == 2:
            d0, d1 = int(readings[0]["diff"]), int(readings[1]["diff"])
            print(f"\n复现性：两次差分 {d0} vs {d1}，差 |Δ| = {abs(d0 - d1)} px"
                  f"（对照 SSR-on run-to-run 漂移量级 ~600 px → docs/phase15_plan.md §3.3 子步 C）")
        return 0

    # ── 全表 ──
    log(f"[{TAG}] 基线臂 default（出厂 wet 块，SSR 开，参考机位）…")
    out = shot("d2_default", True, [])
    if m := re.search(r"GPU    : (.+)", out):
        gpu = m.group(1).strip()
    base = SHOTS / "d2_default.png"
    rows.append(["（默认档）", "d2_default", "基线", "—", "—", "—", "—", "—"])
    for key, values in KNOBS:
        for value in values:
            name = slug(key, value)
            log(f"[{TAG}] 臂 {key}={value} …")
            r = run_arm(name, True, arm_sets(key, value), base)
            ok = int(r["diff"]) > 0
            anchors.append((f"{key}={value}", ok))
            note = "（消融档）" if (key, value) == ("strength", "0") else ""
            rows.append([f"{key}={value}{note}", name, f"{r['diff']} = {r['diff_pct']}%",
                str(r["darker"]), str(r["brighter"]), str(r["min_dark"]), str(r["ratio"]),
                "PASS" if ok else "**FAIL**"])

    # ── 结构对比度前后对照（判据拍板本体）──
    # 四图两对：wet开+SSR开 = d2_default、wet关+SSR开 = d2_strength_0（复用扫描臂），
    # 另补 SSR 关两张。每对 = 同 wet 档的 SSR开−SSR关，对照两对的 p90/p10 与 Δ<0。
    log(f"[{TAG}] 结构对照补图：wet开+SSR关 …")
    shot("d2_struct_wet_ssr_off", False, [])
    log(f"[{TAG}] 结构对照补图：wet关+SSR关 …")
    shot("d2_struct_wetoff_ssr_off", False, ["strength=0"])
    pairs = {
        "湿面关（strength=0）": (SHOTS / "d2_strength_0.png", SHOTS / "d2_struct_wetoff_ssr_off.png"),
        "湿面开（出厂默认）": (SHOTS / "d2_default.png", SHOTS / "d2_struct_wet_ssr_off.png"),
    }
    struct_rows: list[list[str]] = []
    for label, (a, b) in pairs.items():
        r = diffstruct(a, b)
        struct_rows.append([label, f"{a.name} − {b.name}", f"{r['diff']} = {r['diff_pct']}%",
            str(r["darker"]), str(r["brighter"]), str(r["min_dark"]), str(r["ratio"])])

    n_fail = sum(1 for _, ok in anchors if not ok)
    print("# 湿面六旋钮扫描表（Phase 15 批 15.2 子步 D2）\n")
    print(f"- 机位：SSR 参考机位（camera:mode=\"trackball\" + position/target 三键联动，"
          f"湿材质占 22.4%）· 尺寸 {SIZE} · SSR 档 = 全臂开（交付形态）· 日期 {today}")
    print(f"- 设备：{gpu}")
    print(f"- 基线档 = 出厂 wet 块（{WET_DEFAULT}），出图 d2_default")
    print("- 指标 = `imgqa diffstruct`（A=臂、B=默认档，线性亮度域）；"
          "阳性锚点 = 非默认臂差分 > 0 px（旋钮不动像素 = 参数没接上）\n")
    print("## 扫描表（13 臂）\n")
    print(sc.md_table(["臂", "出图", "差分 px", "Δ<0", "Δ>0", "变暗Δ最小", "p90/p10", "锚点"], rows))
    print(f"\n阳性锚点：{len(anchors) - n_fail}/{len(anchors)} PASS"
          + ("；FAIL 臂：" + ", ".join(k for k, ok in anchors if not ok) if n_fail else ""))
    print("\n## 结构对比度前后对照（每对 = 同 wet 档的 SSR开 − SSR关）\n")
    print(sc.md_table(["wet 档", "对", "差分 px", "Δ<0", "Δ>0", "变暗Δ最小", "p90/p10"], struct_rows))
    print("\n判据拍板 = 「反射结构的对比度」：对照两对的 p90/p10 与 Δ<0"
          "（→ docs/phase15_plan.md §2 批次表 15.2 行）。")
    print("\n复跑：`python3 tools/wet_scan.py`（全表）· "
          "`python3 tools/wet_scan.py --only dark=0.4 --twice`（复现性自证）")
    return 1 if n_fail else 0


def slug(key: str, value: str) -> str:
    """臂名 → 文件名安全 slug（smoke.py 的 --name 只准文件名）。点→p、负号→m。"""
    return "d2_" + key.replace("_", "") + "_" + value.replace(".", "p").replace("-", "m")


if __name__ == "__main__":
    sys.exit(main())
