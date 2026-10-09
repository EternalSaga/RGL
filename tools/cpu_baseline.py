#!/usr/bin/env python3
"""CPU 分布账重测器（`just cpu` 的实现体，stdlib-only）——债-02 / 债-20 / 债-30 的读数来源。

**为什么要有这个文件**（2026-10-09）：债-02「现行配置没有 CPU 分布账」当年是靠一个一次性脚本
`measure.py` 量出来的，那个脚本**没有进仓库、现在已不存在**——于是"重测基线"这件事的成本 =
重新写一遍采集层，而成本高的判据等于没有判据（三机轮换 + 每次改渲染都要重测 → 一定会被跳过）。
本文件把采集层钉成工具，读数才可复现。

**口径（三条不可谈，写死在这里）**：

1. **单实例串行**：帧率 / present timing 是被测对象本身，争用直接扭曲它（→ skill `rgl-verify`
   §测量纪律）。起跑前 `pgrep -x rgl` 双检，跑完再双检（日志终值行）。
2. **退出走 RPC `/quit`，不走 `llm_debug` 截图**：截图帧自带一次 fence 等待 + 全幅回读，而
   `FrameCapturer::collect` 就在 `drawFrame` 区间**之内** ⟹ 用截图退出 = 窗口里必然混进一帧
   被回读污染的 `drawFrame` 样本（实测该帧 290ms，把 `frame_ms` 的 p95 整个抬走）。RPC 门
   独立于 imgui 门，headless 可用（`diagnostics.rpc`）。
3. **主口径 = p50，不是 avg**：帧边界钩子（本工具轮询 `/state`、配置热重载）只污染墙钟的少数
   样本，p50 对这种单点 outlier 天然免疫；avg 一并报出，两者差得远 = 有 outlier 要查。

**观察者效应**：profiler 开着的开销实测 +0.20%（债-01 收账），且 profiler 关时交付逐位不变。
本工具**只读**：不改任何配置副本（`RGL_CONFIG_DIR` 注入 staging 目录，契约见 `rgl/main.cppm`
configDir 注释），不写 golden，不存基线哈希。

**未归因怎么算**（这是本工具唯一不显然的一步）：不是"把 stats JSON 里所有 zone 加起来"——
`cull.fenceWait` 嵌在 `drawFrame.cull` 里，扁平求和会重复计数。正确口径 = **区间并集**：
从 trace JSON 取每帧 `drawFrame` 区间，把落在其内的所有 cpu 区间求并集，未归因 =
`drawFrame − 并集`。嵌套由"子区间必被父区间包含"天然处理，不需要深度字段。

用法:
    just cpu                                  # 全部臂（串行，每臂约 15~25s）
    just cpu --arms default,no-vvl            # 只跑两臂
    just cpu --window 200 --warmup 60         # 加长窗口（帧数，不是墙钟）
    just cpu --json /tmp/base.json            # 落机器可读读数
    just cpu --compare /tmp/base.json         # 与上次读数逐项对差（改渲染后用）

退出码: 0 = 全部臂量到 · 1 = 任一臂没量到（前置不满足 / 引擎没起 / 窗口没填满）。
**量不到就响亮失败**，绝不输出半张表——半张表会被读成"这一臂没问题"。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smoke  # 采集层复用：staging / patch_lua / 单实例检查 / error 计数 / 设备行 / git 现场

ROOT = smoke.ROOT
BUILD_RGL = smoke.BUILD_RGL
STAGING = BUILD_RGL / ".cpu"
LOG_PATH = BUILD_RGL / "cpu_baseline_run.log"
PROFILES = BUILD_RGL / "profiles"
# 与 just smoke（不用 RPC）和交互跑（默认 8788）错开的专用端口；起跑前核对没人占。
RPC_PORT = 8790
# 启动到 RPC 可用的上限：shader object 376 条目纯创建就 1.13s（债-04），整段启动实测 ~10s。
STARTUP_TIMEOUT = 120
# 单次 /state 轮询间隔。为什么轮询这么稀：/state 会 buildDebugParamPages（遍历全参数页），
# 它落在**帧边界钩子**里 ⟹ 每次轮询给墙钟加一点。它不进 drawFrame 区间，p50 不受影响，
# 但没必要拿更密的轮询去换更差的墙钟。
POLL_INTERVAL = 1.0

# ── 臂表：一臂 = 一组配置补丁（file, key, lua 值字面量）+ 一句"这臂在量什么" ──────────────
# 加一臂 = 加一行。**别在文档里抄这张表**（抄一次漂一次），`--list-arms` 是唯一现读口。
ARMS: dict[str, tuple[str, list[tuple[str, str, str]]]] = {
    "default": (
        "出厂配置（VVL 开 / deferred / shadow+izb 全开 / aa=off）= 与总账 §1 债-02 同口径",
        [],
    ),
    "no-vvl": (
        "关 VVL 对照：交付路径**没有**验证层，而 default 臂的每一条 vkCmd 都过 VVL。"
        "两臂之差 = CPU 读数里属于验证层的份额（不测就会把开发环境的成本读成交付成本）",
        [("diagnostics.lua", "no_vvl", "true")],
    ),
    "shadow-off": (
        "关 4 级阴影（izb 必须一起关：gate_lint 对 `izb_shadow × shadow_pass=off` 报 error，"
        "债-32 收口时加的）→ 债-20 的配置轴",
        [("diagnostics.lua", "shadow_pass", "false"), ("diagnostics.lua", "izb_shadow", "false")],
    ),
    "izb-off": (
        "只关 IZB（与 default 之差 = IZB 的 CPU 录制增量，债-05 的复核位）",
        [("diagnostics.lua", "izb_shadow", "false")],
    ),
    "scale-half": (
        "render_scale=0.5：GPU 工作量 ÷4、CPU 录制量几乎不变（draw 数一条不少）"
        "⟹ 债-30 的**因果探针**：若 cull.fenceWait 随 GPU 时间一起掉，stall 就是"
        "「等上一帧 GPU 排空」；若不掉，stall 与 GPU 无关，另找原因",
        [("runtime.lua", "render_scale", "0.5")],
    ),
    "forward": (
        "forward 变体（无 G-buffer / light / ao pass）→ pass 结构差异的对照",
        [("render_graph.lua", "variant", '"forward"')],
    ),
    "cull-gpu-sync": (
        "cull_replay=gpu_sync：GPU 读回结果驱动 draw（= 债-30 还账前的原行为）。与 default "
        "之差 = 方案 C 的净收益；也是将来 DGC 的对照组。⚠ 本臂同时是 fence 生命周期的"
        "唯一阳性探针——默认 cpu 档根本不提交 cull CB，提交类错误只在两 GPU 臂才现形",
        [("diagnostics.lua", "cull_replay", '"gpu_sync"')],
    ),
    "cull-gpu-lagged": (
        "cull_replay=gpu_lagged：GPU 集照跑、滞后一帧对账，draw 仍由 CPU 驱动 ⟹ 与 default "
        "之差 = GPU 集自身的成本（dispatch + 读回拷贝 + 一次提交），不含 stall；"
        "与 cull-gpu-sync 之差 = 帧首那次同步等待",
        [("diagnostics.lua", "cull_replay", '"gpu_lagged"')],
    ),
}
DEFAULT_ARMS = "default,no-vvl,shadow-off,izb-off,scale-half"

# 报告里固定点名的段（缺位 = 报 "—"，不写 0：0 与"没有这个测点"是两件事）。
CULL_STEPS = ("cull.record", "cull.submit", "cull.fenceWait", "cull.readback")
# 归因表里的非 pass 段（pass 段不用点名：CPU/GPU 两侧同名即 pass，由 pass_names 自动识别）。
FIXED_SEGMENTS = (
    "drawFrame.cull", "drawFrame.staging", "drawFrame.dataPath", "drawFrame.gpuStamp",
    "drawFrame.izbPre", "drawFrame.evssmPre", "drawFrame.fsrPre", "drawFrame.offscreen",
    "drawFrame.capture", "drawFrame.collect", "drawFrame.picking", "drawFrame.cmdEnd",
    "fenceWait", "acquire", "submit", "present",
)


class ArmFail(RuntimeError):
    """某一臂量不到——响亮失败，但要把已量到的臂先打出来（半张表比没表更难用，所以不静默）。"""


def rpc(port: int, path: str, method: str = "GET", timeout: float = 3.0) -> dict | None:
    """打一次 RPC；连不上/超时返回 None（轮询启动期与"引擎卡住"都走这条路，由调用方判读）。"""
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError):
        return None


def port_free_or_die(port: int) -> None:
    """端口没人占才开跑：被占 = 有别的 rgl 在跑（或上次没退干净），此时读数不可信。"""
    if rpc(port, "/", timeout=1.0) is not None:
        smoke.fail(f"端口 {port} 上已有 RPC 应答——先退掉在跑的 rgl（pgrep -x rgl），并行跑不可信")


def stage(arm: str, window: int) -> Path:
    """铺一次性配置：出厂 configs/ + profiler/rpc 门 + 本臂补丁（每次重建，跑完不残留）。"""
    base = ROOT / "configs"
    STAGING.mkdir(parents=True, exist_ok=True)
    for old in STAGING.glob("*"):
        old.unlink()
    for f in base.iterdir():
        if f.is_file():
            (STAGING / f.name).write_bytes(f.read_bytes())
    # profiler 窗口与 RPC 是本工具的**测点本身**，不是臂内容器。
    smoke.patch_lua(STAGING / "diagnostics.lua", "profiler", str(window))
    smoke.patch_lua(STAGING / "diagnostics.lua", "rpc", "true")
    smoke.patch_lua(STAGING / "diagnostics.lua", "rpc_port", str(RPC_PORT))
    # headless 是**测点**不是臂内容器（本工具不截图，所以不像 `just smoke` 那样开 llm_debug）：
    # 带窗跑会把合成器的 present 节奏与 acquire 背压混进被测对象（实测 present 0.03→0.67ms、
    # acquire 0.17→0.50ms，且窗口尺寸变化会让可见集逐帧漂 ⟹ draw.count 不再恒定）。
    smoke.patch_lua(STAGING / "runtime.lua", "headless", "true")
    smoke.patch_lua(STAGING / "runtime.lua", "llm_debug", "false")
    for fname, key, value in ARMS[arm][1]:
        smoke.patch_lua(STAGING / fname, key, value)
    return STAGING


def newest_profile(after: float) -> tuple[Path, Path]:
    """取本次运行落的两件产物（mtime 晚于起跑时刻的 stats/trace；同批 epoch 相同）。"""
    stats = [p for p in PROFILES.glob("stats_*.json") if p.stat().st_mtime >= after]
    if not stats:
        raise ArmFail(f"profiles/ 下没有新 stats_*.json（引擎没走到 profilerDump？退出方式不对）")
    newest = max(stats, key=lambda p: p.stat().st_mtime)
    epoch = newest.stem.split("_", 1)[1]
    trace = PROFILES / f"trace_{epoch}.json"
    if not trace.is_file():
        raise ArmFail(f"缺 {trace.name}（stats 与 trace 同批落盘，缺一半 = 写盘失败）")
    return newest, trace


def run_engine(arm: str, window: int, warmup: int, timeout: int) -> tuple[Path, Path, dict]:
    """跑一臂到 `/quit` 退出，返回 (stats 路径, trace 路径, 现场信息)。

    节奏按**帧数**不按墙钟（→ skill `rgl-verify`：按墙钟睡会在争用下量到未收敛的瞬态）。
    """
    stage(arm, window)
    started_wall = time.time() - 1
    log = LOG_PATH.open("w", encoding="utf-8")
    env = dict(os.environ, RGL_CONFIG_DIR=str(STAGING))
    proc = subprocess.Popen([f"./{smoke.EXE_NAME}"], cwd=BUILD_RGL, env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        # ① 等 RPC 起来（启动 ~10s：376 条 shader object 纯创建就 1.13s）
        deadline = time.monotonic() + STARTUP_TIMEOUT
        state = None
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise ArmFail(f"引擎启动期就退了（exit={proc.returncode}），日志 {LOG_PATH}")
            if (state := rpc(RPC_PORT, "/state")) is not None:
                break
            time.sleep(0.25)
        if state is None:
            raise ArmFail(f"{STARTUP_TIMEOUT}s 内 RPC 没起来（{LOG_PATH}）")

        # ② 等帧数过 warmup + window（窗口 = 最近 window 帧，所以要多跑 warmup 帧把它冲干净）
        target = warmup + window
        deadline = time.monotonic() + timeout
        while True:
            time.sleep(POLL_INTERVAL)
            if proc.poll() is not None:
                raise ArmFail(f"引擎在第 {state.get('frame')} 帧自己退了，日志 {LOG_PATH}")
            if (s := rpc(RPC_PORT, "/state")) is not None:
                state = s
            if state.get("frame", 0) >= target:
                break
            if time.monotonic() > deadline:
                raise ArmFail(f"等 {target} 帧超时（现 {state.get('frame')} 帧）——帧率比预期低？")

        # ③ 收口：/quit → 等退出（profilerDump 在 renderSystem 返回后跑）
        if rpc(RPC_PORT, "/quit", method="POST") is None:
            raise ArmFail("/quit 没应答（引擎卡帧循环了？日志尾部见 smoke 同款判读）")
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise ArmFail("/quit 后 60s 没退出")
    finally:
        log.close()
        if proc.poll() is None:
            proc.kill()
    log_text = LOG_PATH.read_text(encoding="utf-8", errors="replace")
    errors, warnings = smoke.count_severities(log_text)
    if "headless 模式" not in log_text:
        raise ArmFail("引擎不是 headless（合成器 present 节奏与窗口尺寸会混进被测对象）"
                      "——staging 里的 headless 键没生效？")
    extent = re.search(r"render scale = .*?→ render (\d+x\d+)", log_text)
    stats, trace = newest_profile(started_wall)
    return stats, trace, {
        "device": smoke.gpu_of(log_text),
        "extent": extent.group(1) if extent else "未取到",
        "errors": len(errors),
        "warnings": len(warnings),
        "error_lines": errors[:3],
        "quit_frame": state.get("frame"),
    }


def load_zones(stats_path: Path) -> tuple[dict, dict]:
    """stats JSON → ({(kind,name): agg}, 顶层读数)。顶层含 frame_ms 分布与 counters。"""
    data = json.loads(stats_path.read_text(encoding="utf-8"))
    zones = {(z["kind"], z["name"]): z for z in data["zones"]}
    counters = {c["name"]: c for c in data.get("counters", [])}
    top = {
        "frames": data["frames"],
        "drops": data["drops"],
        "frame_ms": data["frame_ms"],
        "counters": counters,
    }
    return zones, top


def unattributed(trace_path: Path) -> dict:
    """未归因 = drawFrame − 其内部 cpu 区间的并集（口径见模块 docstring 最后一条）。"""
    events = json.loads(trace_path.read_text(encoding="utf-8"))["traceEvents"]
    frames: dict[int, list[tuple[float, float]]] = {}
    roots: dict[int, tuple[float, float]] = {}
    for e in events:
        if e.get("ph") != "X" or e.get("tid") != 0:
            continue
        frame = e["args"]["frame"]
        span = (e["ts"], e["ts"] + e["dur"])
        if e["name"] == "drawFrame":
            roots[frame] = span
        else:
            frames.setdefault(frame, []).append(span)
    per_frame: list[float] = []
    for frame, (start, end) in roots.items():
        inside = [(a, b) for a, b in frames.get(frame, [])
                  if a >= start - 1e-9 and b <= end + 1e-9 and (b - a) < (end - start)]
        inside.sort()
        covered = 0.0
        cur_start = cur_end = None
        for a, b in inside:
            if cur_end is not None and a <= cur_end:
                cur_end = max(cur_end, b)
                continue
            if cur_end is not None:
                covered += cur_end - cur_start
            cur_start, cur_end = a, b
        if cur_end is not None:
            covered += cur_end - cur_start
        per_frame.append((end - start) - covered)
    if not per_frame:
        return {"frames": 0, "mean_ms": None}
    per_frame.sort()
    return {
        "frames": len(per_frame),
        "mean_ms": sum(per_frame) / len(per_frame) / 1000.0,
        "p50_ms": per_frame[len(per_frame) // 2] / 1000.0,
        "max_ms": per_frame[-1] / 1000.0,
    }


def pass_names(zones: dict) -> list[str]:
    """pass 段名单：与 GPU 侧同名的 cpu 段就是 pass（CPU/GPU 两侧同名由 9.2/9.5 的约定保证）。"""
    gpu = {name for kind, name in zones if kind == "gpu"}
    got = [name for kind, name in zones if kind == "cpu" and name in gpu]
    return sorted(got, key=lambda n: -(zones[("cpu", n)]["mean_ms"]))


def ms(zones: dict, kind: str, name: str) -> float | None:
    z = zones.get((kind, name))
    return None if z is None else z["mean_ms"]


def fmt(v: float | None, width: int = 7, digits: int = 3) -> str:
    return "—".rjust(width) if v is None else f"{v:{width}.{digits}f}"


def summarize(zones: dict, top: dict) -> dict:
    """一臂的关键读数（p50 主口径 + 派生量），报告与 --json 共用同一份，避免两处口径漂。"""
    cpu = ms(zones, "cpu", "drawFrame")
    gpu = ms(zones, "gpu", "gpu.frame")
    stall = ms(zones, "cpu", "cull.fenceWait")
    # 帧内的**主机阻塞等待**有两处：cull 读回等（债-30 那笔）与帧开场的 inFlight/present
    # fence 等。"CPU 真干" 必须两个都减——债-30 还掉之后背压不会消失，只会**搬家**（实测
    # 从 cull.fenceWait 10.78 搬到 fenceWait 7.98），只减前者会把"还在等"读成"在干活"，
    # 并把 bound 误判成 CPU-bound（而 cpu 真干 2.66 vs gpu 10.31 明明是 GPU-bound）。
    frame_wait = ms(zones, "cpu", "fenceWait")
    waits = [v for v in (stall, frame_wait) if v is not None]
    useful = None if (cpu is None or not waits) else cpu - sum(waits)
    busy = cpu if useful is None else useful
    return {
        "frames": top["frames"],
        "drops": top["drops"],
        "frame_p50": top["frame_ms"]["p50_ms"],
        "frame_avg": top["frame_ms"]["avg_ms"],
        "cpu_record": cpu,
        "gpu_frame": gpu,
        "cull_stall": stall,
        "frame_wait": frame_wait,
        "cpu_useful": useful,
        "stall_share_of_cpu": None if not cpu or stall is None else stall / cpu,
        "bound": ("CPU-bound" if (busy or 0) >= (gpu or 0) else "GPU-bound") if busy and gpu else "—",
        "draws": top["counters"].get("draw.count", {}).get("min"),
        "draws_max": top["counters"].get("draw.count", {}).get("max"),
        "cmds": top["counters"].get("vkcmd.count", {}).get("min"),
        "cmds_max": top["counters"].get("vkcmd.count", {}).get("max"),
        "zones": {f"{k}|{n}": z["mean_ms"] for (k, n), z in sorted(zones.items())},
    }


def report(arm: str, zones: dict, top: dict, unattr: dict, info: dict) -> dict:
    """打一臂的读数，同时返回结构化摘要（--json / --compare 用）。"""
    s = summarize(zones, top)
    print(f"\n=== 臂 {arm} — {ARMS[arm][0]}")
    print(f"    device={info['device']}  render={info['extent']}  frames={s['frames']}"
          f"  drops={s['drops']}  errors={info['errors']}  warnings={info['warnings']}")
    if info["error_lines"]:
        for ln in info["error_lines"]:
            print(f"    [error] {ln}")
    print(f"    frame p50={fmt(s['frame_p50'])} avg={fmt(s['frame_avg'])}"
          f" | cpu.record={fmt(s['cpu_record'])} gpu.frame={fmt(s['gpu_frame'])}"
          f" → {s['bound']}")
    share = s["stall_share_of_cpu"]
    print(f"    cpu 真干（cpu.record − cull.fenceWait − 帧首 fenceWait）={fmt(s['cpu_useful'])}"
          f" | 其中帧首等待={fmt(s['frame_wait'])}"
          f" | cull 等待占 cpu.record = {'—' if share is None else f'{100 * share:.1f}%'}")
    # min≠max = 可见集在漂（场景在动 / 取景在变）——那"draws 完全确定"这条读数前提就没了，
    # 必须出声，否则后来者会拿一个区间的中位数去对当年的恒定值。
    draws = s["draws"] if s["draws"] == s["draws_max"] else f"{s['draws']}..{s['draws_max']}"
    cmds = s["cmds"] if s["cmds"] == s["cmds_max"] else f"{s['cmds']}..{s['cmds_max']}"
    print(f"    draws={draws}  vkcmd={cmds}"
          f" | 未归因 p50={fmt(unattr['p50_ms'])} max={fmt(unattr['max_ms'])}（trace 区间并集）")
    print("    cull 四段: " + "  ".join(f"{n.split('.')[-1]}={fmt(ms(zones, 'cpu', n), 6)}"
                                        for n in CULL_STEPS))
    print("    段（cpu 均值 ms，缩进 = 与父段同层）")
    for name in pass_names(zones):
        print(f"      pass {name:<18} cpu={fmt(ms(zones, 'cpu', name), 6)}"
              f"  gpu={fmt(ms(zones, 'gpu', name), 6)}")
    for name in FIXED_SEGMENTS:
        if ("cpu", name) in zones:
            print(f"      seg  {name:<18} cpu={fmt(ms(zones, 'cpu', name), 6)}")
    return {**s, "unattributed": unattr, "device": info["device"], "extent": info["extent"],
            "errors": info["errors"], "warnings": info["warnings"]}


def compare(arm: str, cur: dict, base: dict) -> None:
    """与上次读数逐项对差（改渲染/改代码后的验收位；只报数不判好坏）。"""
    keys = ("frame_p50", "cpu_record", "gpu_frame", "cull_stall", "frame_wait", "cpu_useful",
            "draws", "cmds")
    print(f"    Δ vs 基线（{arm}）")
    for k in keys:
        a, b = cur.get(k), base.get(k)
        if a is None or b is None:
            continue
        # draws/cmds 是计数（整数），其余是 ms —— 两种宽度不能共用一个格式串
        w = "d" if isinstance(a, int) and isinstance(b, int) else ".3f"
        d = a - b
        pct = f"{100 * d / b:+6.1f}%" if b else "     —"
        print(f"      {k:<12} {b:>{10}{w}} → {a:>{10}{w}}  Δ{d:>+8.3f} ({pct})")


def main() -> int:
    ap = argparse.ArgumentParser(description="CPU 分布账重测（债-02/20/30 读数）")
    ap.add_argument("--arms", default=DEFAULT_ARMS, help=f"逗号分隔，默认 {DEFAULT_ARMS}")
    ap.add_argument("--window", type=int, default=120, help="profiler 帧窗口（默认 120）")
    ap.add_argument("--warmup", type=int, default=40, help="窗口前丢弃的帧数（默认 40）")
    ap.add_argument("--timeout", type=int, default=240, help="单臂等帧数上限秒（默认 240）")
    ap.add_argument("--json", type=Path, help="把读数写到该路径")
    ap.add_argument("--compare", type=Path, help="与该基线 JSON 逐项对差")
    ap.add_argument("--list-arms", action="store_true", help="列出臂表并退出")
    a = ap.parse_args()

    if a.list_arms:
        for name, (why, _) in ARMS.items():
            print(f"{name:<12} {why}")
        return 0

    arms = [x.strip() for x in a.arms.split(",") if x.strip()]
    unknown = [x for x in arms if x not in ARMS]
    if unknown:
        smoke.fail(f"未知臂 {unknown}——现读口：just cpu --list-arms")

    smoke.check_prereq()
    smoke.single_instance_or_die()
    port_free_or_die(RPC_PORT)
    print(f"[cpu] {smoke.git_state()} · 窗口 {a.window} 帧 / 热身 {a.warmup} 帧 · 臂 {arms}")
    print("[cpu] 串行跑（并行 = 读数作废），每臂约 15~25s，启动本身约 10s")

    results: dict[str, dict] = {}
    failed: list[str] = []
    for arm in arms:
        try:
            stats, trace, info = run_engine(arm, a.window, a.warmup, a.timeout)
            zones, top = load_zones(stats)
            if top["frames"] < a.window:
                raise ArmFail(f"窗口只填了 {top['frames']}/{a.window} 帧——读数不可用")
            results[arm] = report(arm, zones, top, unattributed(trace), info)
            results[arm]["stats_file"] = stats.name
        except ArmFail as e:
            failed.append(arm)
            print(f"\n=== 臂 {arm} FAIL: {e}")
        # 下一臂起跑前再确认没有残留进程（残留 = 两实例争用 = 后面所有臂的读数作废）
        smoke.single_instance_or_die()
    if results:
        print("\n=== 汇总（ms，p50 帧墙钟 / cpu.record / gpu.frame / 帧内主机等待 / cpu 真干）")
        print(f"    {'arm':<12}{'frame':>8}{'cpu.rec':>9}{'gpu':>8}{'等待':>8}"
              f"{'cpu真干':>9}{'draws':>8}{'cmds':>8}  bound")
        for arm, s in results.items():
            waits = [v for v in (s["cull_stall"], s.get("frame_wait")) if v is not None]
            print(f"    {arm:<12}{fmt(s['frame_p50'], 8)}{fmt(s['cpu_record'], 9)}"
                  f"{fmt(s['gpu_frame'], 8)}{fmt(sum(waits) if waits else None, 8)}"
                  f"{fmt(s['cpu_useful'], 9)}{str(s['draws']):>8}{str(s['cmds']):>8}  {s['bound']}")
    if a.compare and a.compare.is_file():
        base = json.loads(a.compare.read_text(encoding="utf-8"))["arms"]
        for arm, s in results.items():
            if arm in base:
                compare(arm, s, base[arm])
    if a.json:
        payload = json.dumps({"git": smoke.git_state(), "arms": results},
                             ensure_ascii=False, indent=1)
        a.json.write_text(payload, encoding="utf-8")
        print(f"\n[cpu] 读数已写 {a.json}")
    if failed:
        smoke.fail(f"这些臂没量到：{failed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
