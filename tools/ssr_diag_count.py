#!/usr/bin/env python3
"""SSR 未命中成因诊断档的计数脚本（Phase 15 批 15.3 子步 A，stdlib-only）。

输入 = `rgl/shaders/ssr.frag.slang` 诊断档（kDiagnose=true）在三档构建下出的三张阈值色图
（参考机位 + `--ssr`，出图命令见 docs/phase15_plan.md §3.4），输出 = 六类各占过门控像素的
百分比 + H1 敏感性表（0.02/0.05/0.10）+ 一致性核对。**诊断档是临时形态，本脚本是常驻
读数器**——重跑诊断需再开 shader 常量，但图在手上就能复算（④ 的教训：读数器别留 /tmp）。

编码口径（与 shader 诊断档段头逐条对账，改一边必须同步另一边）：
  · 每通道一个二值掩码，shader 只写端点 0 / 4.0；截图经 exposure × AO × ACES × sRGB 后
    「on」落 ≈ 137–255、「off」恰为 0 ⟹ 阈值 >127。
  · **ACES 矩阵有通道串扰**（闭式核算，AO=1）：R 掩码 → (255,110,65)、G 掩码 → (211,245,173)、
    B 掩码 → (93,14,250)。纯「通道>127 即属类」会把绿掩码像素读成红类（R=211）。
    ⟹ **mode1/2 按 G→B→R 优先序判属类**（六类互斥，每像素恰一个通道置位）：
      G 类 ⟺ G>127；B 类 ⟺ B>127 ∧ G≤127；R 类 ⟺ R>127 ∧ G≤127 ∧ B≤127。
    AO 压暗整组缩放后优先序仍成立（AO=0.25：R=(218,0,13) G=(89,209,73) B=(0,0,206)；
    AO=0.1：R=(137,0,~0) G=(147,…) B=(…,140)）。
  · **mode3 是嵌套集合不是互斥类**（分母 ⊇ H1@0.10 ⊇ H1@0.02，同像素可多通道同置：
    分母+H1@0.02 → (249,245,187)，分母+仅 H1@0.10 → (255,126,250)）⟹ 优先序会把分母
    漏数成「非敏感性那部分」。mode3 改**逐通道独立计数**：R>127 = 分母、G>127 = H1@0.02、
    B>127 = H1@0.10。串扰方向核对：分母-only 的 G=110、仅 H1@0.10 的 G=126，都 ≤127
    （AO ≤ 1 只会更低），不互串；嵌套不变量 h1_002 ≤ h1_010 ≤ denom 由脚本硬核对。
  · 全黑 = 未过门控（分母之外）。非黑且（按各自口径）判不进任何集合 = 「过暗掉阈」
    （AO 把端点压进 ≤127 区），单独计数报出——它不为 0 就说明端点法有系统性低估。

三张图的通道语义（= shader kDiagMode 的编码）：
  mode1：R=M1 出屏 · G=M2 预算耗尽 · B=M3 厚度拒绝
  mode2：R=M4 命中但 weight<0.01 · G=H1 命中但反射源暗(×0.05) · B=H2 命中且亮
  mode3：R=分母（过门控掩码）· G=H1@0.02 · B=H1@0.10（敏感性带）

用法:
    python3 tools/ssr_diag_count.py <mode1.png> <mode2.png> <mode3.png>
退出码: 0 = 出表且一致性核对通过（六类并集 = 分母 ± 掉阈；mode3 嵌套不变量成立）
      · 1 = 输入缺失 / 对账失败（编码或优先序漂了）。
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ssr_hull_check import png_decode_rgb  # 共享最小 PNG 解码（stdlib-only，同 tools/）

TH = 127  # 阈值色判据线：端点 0 与 4.0 之间唯一的合法切位（见模块 docstring）


def _pixels(path: Path):
    w, h, rows, ch = png_decode_rgb(path.read_bytes())
    for y in range(h):
        row = rows[y]
        for x in range(w):
            yield row[ch * x:ch * x + 3]


def _dims(path: Path) -> tuple[int, int]:
    w, h, _rows, _ch = png_decode_rgb(path.read_bytes())
    return w, h


def tally_exclusive(path: Path) -> dict[str, int]:
    """mode1/2（六类互斥，每像素 ≤1 通道置位）：G→B→R 优先序判属类（理由见 docstring）。"""
    n = {"R": 0, "G": 0, "B": 0, "black": 0, "dropped": 0}
    for px in _pixels(path):
        r, g, b = px[0], px[1], px[2]
        if r <= TH and g <= TH and b <= TH:
            n["black" if (r, g, b) == (0, 0, 0) else "dropped"] += 1
        elif g > TH:
            n["G"] += 1
        elif b > TH:
            n["B"] += 1
        else:
            n["R"] += r > TH
    return n


def tally_nested(path: Path) -> dict[str, int]:
    """mode3（分母 ⊇ 敏感性带，可多通道同置）：逐通道独立计数（理由见 docstring）。"""
    n = {"denom": 0, "h1_002": 0, "h1_010": 0, "black": 0, "dropped": 0}
    for px in _pixels(path):
        r, g, b = px[0], px[1], px[2]
        if (r, g, b) == (0, 0, 0):
            n["black"] += 1
            continue
        if r > TH:
            n["denom"] += 1
        elif g > TH or b > TH:
            n["dropped"] += 1  # 敏感性像素掉阈但分母也掉——嵌套被破坏，报出来
        n["h1_002"] += g > TH
        n["h1_010"] += b > TH
    return n


def main() -> int:
    if len(sys.argv) != 4:
        print("用法: ssr_diag_count.py <mode1.png> <mode2.png> <mode3.png>", file=sys.stderr)
        print(__doc__)
        return 1
    m1, m2, m3 = (Path(a) for a in sys.argv[1:4])
    for p in (m1, m2, m3):
        if not p.is_file():
            print(f"[ssr_diag_count] FAIL: 找不到 {p}", file=sys.stderr)
            return 1
    dims = [_dims(p) for p in (m1, m2, m3)]
    if len(set(dims)) != 1:
        print(f"[ssr_diag_count] FAIL: 三张图尺寸不一致 {dims}（不是同机位同尺寸的三档构建？）",
              file=sys.stderr)
        return 1
    w, h = dims[0]

    t1, t2 = tally_exclusive(m1), tally_exclusive(m2)
    t3 = tally_nested(m3)
    denom = t3["denom"]  # 分母 = 过门控掩码（mode3 R 通道，独立计数）
    if denom == 0:
        print("[ssr_diag_count] FAIL: 分母为 0（SSR 没开？机位没有过门控像素？）", file=sys.stderr)
        return 1
    nested_ok = t3["h1_002"] <= t3["h1_010"] <= denom

    classes = [
        ("M1 出屏/相机平面后", t1["R"]),
        ("M2 预算耗尽", t1["G"]),
        ("M3 厚度拒绝", t1["B"]),
        ("M4 命中但 weight<0.01", t2["R"]),
        ("H1 命中但反射源暗(×0.05)", t2["G"]),
        ("H2 命中且亮", t2["B"]),
    ]
    total = sum(c for _, c in classes)
    hit_w = t2["G"] + t2["B"]  # 命中 ∧ weight≥0.01（H1+H2）
    dropped = t1["dropped"] + t2["dropped"] + t3["dropped"]

    print("# SSR 未命中成因分家 · 诊断读数（批 15.3 子步 A）\n")
    print(f"- 图 {w}x{h} · 分母（过门控像素，mode3 R 独立计数）= {denom} "
          f"= 画面 {100.0 * denom / (w * h):.2f}%")
    print(f"- 掉阈像素（非黑但判不进集合，AO 把端点压进 ≤{TH} 区）：mode1/2/3 = "
          f"{t1['dropped']} / {t2['dropped']} / {t3['dropped']}\n")
    print("| 类 | 像素 | 占过门控 |")
    print("|---|---|---|")
    for name, cnt in classes:
        print(f"| {name} | {cnt} | {100.0 * cnt / denom:.2f}% |")
    print(f"\n一致性：六类并集 {total} vs 分母 {denom}，差 {denom - total}"
          "（应 ≈ 0；≈ 掉阈数 = 端点被 AO 压掉的系统性低估，读数带误差条读）")
    print("\n## H1 敏感性（「暗」的定义不给裸数，阈值给三档）\n")
    print("| H1 阈值（F·reflected 亮度 < hdr 亮度 ×） | 像素 | 占过门控 | 占命中(weight≥0.01) |")
    print("|---|---|---|---|")
    for label, cnt in (("0.02", t3["h1_002"]), ("0.05（主档）", t2["G"]), ("0.10", t3["h1_010"])):
        share_hit = f"{100.0 * cnt / hit_w:.2f}%" if hit_w else "无定义（命中集为空）"
        print(f"| {label} | {cnt} | {100.0 * cnt / denom:.2f}% | {share_hit} |")
    print(f"\n命中 ∧ weight≥0.01 = H1+H2 = {hit_w}（占过门控 {100.0 * hit_w / denom:.2f}%）")
    ok = abs(denom - total) <= max(dropped, 1) and nested_ok
    print(f"\n对账: 嵌套不变量 h1@0.02 ≤ h1@0.10 ≤ 分母: {'PASS' if nested_ok else 'FAIL'}"
          f" · 并集对分母: {'PASS' if abs(denom - total) <= max(dropped, 1) else 'FAIL'}"
          f"\n总结: {'PASS' if ok else 'FAIL（编码或优先序漂了）'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
