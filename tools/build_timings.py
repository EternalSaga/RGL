#!/usr/bin/env python3
"""解析 ninja .ninja_log，输出最近一次 build 的耗时报告。

用法: python3 build_timings.py <build_dir>
"""
import os
import sys

# .ninja_log 每行: start_ms <TAB> end_ms <TAB> mtime_ns <TAB> output <TAB> cmdhash
# 注意: start/end 是相对“本次 ninja 运行”的毫秒，跨 build 会重叠，
# 必须用第 3 列 mtime_ns（绝对纳秒）切分 build 段
GAP_NS = 30_000_000_000  # 30s


def main() -> None:
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    log = os.path.join(build_dir, ".ninja_log")
    if not os.path.exists(log):
        raise SystemExit(f"未找到 {log}，请先构建")

    rows: list[tuple[int, int, int, str]] = []
    with open(log) as f:
        next(f, None)
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4:
                try:
                    rows.append((int(p[0]), int(p[1]), int(p[2]), p[3]))
                except ValueError:
                    pass
    if not rows:
        raise SystemExit(f"{log} 为空")

    # 按绝对完成时间排序，>30s 间隔视为新的 build（连续 build 间隔 <30s 会合并，结果取最新记录）
    rows.sort(key=lambda r: r[2])
    segs, cur = [], [rows[0]]
    for r in rows[1:]:
        if r[2] - max(x[2] for x in cur) > GAP_NS:
            segs.append(cur)
            cur = [r]
        else:
            cur.append(r)
    segs.append(cur)

    # 最近一次 build 内，同一输出保留最后一条记录
    last = {}
    for s, e, _m, out in segs[-1]:
        last[out] = (s, e)

    s0 = min(v[0] for v in last.values())
    edges: dict[tuple[int, int], list[str]] = {}
    for out, (s, e) in last.items():
        edges.setdefault((s - s0, e - s0), []).append(out)

    wall = max(e for s, e in edges)
    tot = sum(e - s for s, e in edges)
    n = os.cpu_count() or 1
    print(
        f"最近一次 build: wall {wall/1000:.1f}s | 任务 {len(edges)} | "
        f"CPU 总耗时 {tot/1000:.1f}s | 利用率 {tot/(wall*n)*100:.0f}% ({n} 核)"
    )
    print(f"{'耗时':>8}  {'起始':>8}  目标")

    def nice(outs: list[str]) -> str:
        for pref in (".cppm.o", ".cpp.o", ".o", ".pcm", ".a", ".spv"):
            for o in outs:
                if o.endswith(pref):
                    return o
        return sorted(outs)[0]

    for (s, e), outs in sorted(edges.items(), key=lambda kv: kv[0][1] - kv[0][0], reverse=True)[:15]:
        o = nice(outs).replace(build_dir + "/", "")
        print(f"{(e-s)/1000:7.1f}s  {s/1000:7.1f}s  {o}")


if __name__ == "__main__":
    main()
