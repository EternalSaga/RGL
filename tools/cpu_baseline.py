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

**会话内噪声带**（债-112 的最小修法，2026-10-14）：`--repeat N` 让每臂**同配置连跑 N 次**，把逐
指标的 min..max 作为**噪声带**随读数打印，并给每个指标一句"能不能拿来 A/B"的提示。为什么要它：
`frame p50` 的会话内波动比 0.1ms 级的改动大一个量级（台账债-112：本机同代码四跑跨 **0.69ms**），
而同一张表里的 pass 级 GPU zone 只有 0.01~0.02ms ⟹ **同一张表里有的指标能 A/B、有的不能**，
工具把这件事**印出来**，不靠人记（踩过：把一次零代价改动记成"退化 +0.14ms"）。
🔴 默认（不带 `--repeat`）**不印带、输出与旧版逐字相同**——`docs/cpu_perf_plan.md` 与
`docs/还债实测数据.md` 的基线表全部引用它，改口径 = 让所有历史读数失效。

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
    just cpu --arms default --repeat 4        # 同配置连跑 4 次 → 逐指标噪声带（债-112）
    just cpu --window 200 --warmup 60         # 加长窗口（帧数，不是墙钟）
    just cpu --json /tmp/base.json            # 落机器可读读数
    just cpu --compare /tmp/base.json         # 与上次读数逐项对差（改渲染后用）
    just cpu --selftest                       # 量具自检（合成读数，不跑引擎，毫秒级）

退出码: 0 = 全部臂量到 · 1 = 任一臂没量到（前置不满足 / 引擎没起 / 窗口没填满）。
**量不到就响亮失败**，绝不输出半张表——半张表会被读成"这一臂没问题"。
"""

from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from contextlib import redirect_stdout
from pathlib import Path
from statistics import median

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
        "关 4 级阴影（izb 必须一起关：gate_lint 对 `izb × mode=\"none\"` 报 error，"
        "债-32 收口时加的）→ 债-20 的配置轴。批 N6b 起两键都在 shadow.lua（izb 是整键替换：子表 "
        "{ on, grid, dump_frame } 换成最小子表 { on = false }；硬切 2026-10-17 起解析器只收子表形态）",
        [("shadow.lua", "mode", '"none"'), ("shadow.lua", "izb", "{ on = false }")],
    ),
    "izb-off": (
        "只关 IZB（与 default 之差 = IZB 的 CPU 录制增量，债-05 的复核位）",
        [("shadow.lua", "izb", "{ on = false }")],
    ),
    "scale-half": (
        "render_scale=0.5：GPU 工作量 ÷4、CPU 录制量几乎不变（draw 数一条不少）"
        "⟹ 债-30 的**因果探针**：若 cull.fenceWait 随 GPU 时间一起掉，stall 就是"
        "「等上一帧 GPU 排空」；若不掉，stall 与 GPU 无关，另找原因",
        [("runtime.lua", "render_scale", "0.5")],
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
    "ssr-on": (
        "ssr.enabled=true（trace+apply 双 pass）× no-vvl：SSR 的**直接读数**看本臂 pass 行的 "
        "gpu= 列（ssr_trace / ssr 两 pass 各自的 GPU 时间戳；债-87 已还，2026-10-11 起可用）。"
        "与 no-vvl 臂的 frame p50 之差只作**代理读数**保留：GPU-bound 时它被帧预算吃掉、"
        "CPU-bound 时是下界，都不等于 pass 自身成本（15.3B/C 当年因量具失效只能用这条）。"
        "键 `enabled` 行首唯一 = smoke --ssr 同款 patch",
        [("diagnostics.lua", "no_vvl", "true"), ("runtime.lua", "enabled", "true")],
    ),
    "ssr-on-t6": (
        "ssr.enabled=true × thickness_px=6（15.3C 改默认 12 之前的出厂值）× no-vvl：与 "
        "ssr-on 臂之差 = **thickness 6→12 这一改动本身**的成本（同会话对照，跨会话 frame "
        "漂移不可比）。预期 ≈0：M3 拒绝与命中都在同一步 break，多出来的只有细化 + 取色",
        [("diagnostics.lua", "no_vvl", "true"), ("runtime.lua", "enabled", "true"),
         ("runtime.lua", "thickness_px", "6.0")],
    ),
}
DEFAULT_ARMS = "default,no-vvl,shadow-off,izb-off,scale-half"

# ── device fault 诊断通路的开销臂（2026-10-20，MDI hang 归因批）───────────────────
# 问的是"每帧一次 vkGetDeviceFaultReportsKHR(timeout=0) 要不要钱"。判据 = 与 default 臂的
# frame_p50 / cpu.record 差值落在噪声带内（本工具 --repeat 5 的带宽口径），**不是**"看着差不多"。
# 与 proxy-arm* 的区别：那组量 GPU pass 时间（阴影段），本臂量 CPU 侧录制路径（轮询是 CPU 调用）。
ARMS["fault-poll"] = (
    "diagnostics.fault_poll=true：每帧一次 masked fault 轮询（timeout=0 不阻塞）。与 default 之差"
    " = 诊断通路自身的 CPU 成本；门关时连 Vulkan 调用都不发，所以这条量的就是'旁路到底多轻'",
    [("diagnostics.lua", "fault_poll", "true")],
)

# ─── N10 P3 shadow proxy 四臂（整条线的生死判据）────────────────────────────────
# 判据 = **arm2 vs arm0 的阴影逐 pass GPU 合计**（`gpu:shadow1..4`，p50）；arm1/arm3 只做
# 归因，不许拿差值凑净账（任务书红线）。口径三条写在这里而不是抄进文档：
#  · 单实例串行（本工具逐臂串行跑，跑期间别起别的 rgl）；主口径 p50；
#  · 🔴 每臂跑 **VVL 开与关两档**。VVL 只吃 CPU，但它改变 CPU/GPU 的相对位置：两档的
#    `gpu:shadow*` 读数若明显不一致，说明测到的不是阴影 pass 自身的工作（GPU 被 CPU 侧卡住），
#    那就要停下来查量具，不下结论 —— 这条对照本身就是判据（skill rgl-verify：CPU 读数必须带
#    VVL 档；本批把同一条纪律用到 GPU 读数上）。
#  · 目标形态 = 门开 D=7（数 6 的代价账以 D=7 为基准）。D=5 + 钉段是**归因臂**不是目标形态。
for _name, _desc, _proxy, _lod in [
    ("arm0", "门关 D=5（出厂态）= 四臂基线", "false", "5"),
    ("arm1", "门开 D=5 = 钉段边际成本归因（+39 draws、+11% 三角）", "true", "5"),
    ("arm2", "门开 D=7 = **目标形态**（几何账 0.76× + ≤39 draws）", "true", "7"),
    ("arm3", "门关 D=7 = D=7 省量的裸参照（arm2−arm3 = 钉段在 D=7 下的真实边际）", "false", "7"),
]:
    _patches = [("runtime.lua", "meshlet.shadow_proxy", _proxy),
                ("runtime.lua", "meshlet.shadow_lod", _lod)]
    ARMS[f"proxy-{_name}"] = (_desc + "；VVL 开（与 default 同档）", list(_patches))
    ARMS[f"proxy-{_name}-novvl"] = (
        _desc + "；VVL 关（交付路径档，与同臂 VVL 开档互为量具自检）",
        [("diagnostics.lua", "no_vvl", "true"), *_patches])

# 报告里固定点名的段（缺位 = 报 "—"，不写 0：0 与"没有这个测点"是两件事）。
CULL_STEPS = ("cull.record", "cull.submit", "cull.fenceWait", "cull.readback")
# 归因表里的非 pass 段（pass 段不用点名：CPU/GPU 两侧同名即 pass，由 pass_names 自动识别）。
FIXED_SEGMENTS = (
    "drawFrame.cull", "drawFrame.staging", "drawFrame.dataPath", "drawFrame.gpuStamp",
    "drawFrame.izbPre", "drawFrame.evssmPre", "drawFrame.fsrPre", "drawFrame.offscreen",
    "drawFrame.capture", "drawFrame.collect", "drawFrame.picking", "drawFrame.cmdEnd",
    "fenceWait", "acquire", "submit", "present",
)

# ── 会话内噪声带（债-112 最小修法：把"同会话可重复性"随读数一起印出来）────────────────
# 判据 = **带 ≤ max(AB_ABS_MS, AB_REL × 中位) ⟹ 该指标可拿来 A/B**。两个阈值不是拍的，锚在台账
# 那组数上（`docs/还债总账.md` 债-112，2026-10-13 本机同代码四跑）：`frame_p50` 跨 0.69ms
# （中位 ≈5.4ms ⟹ 判据线 0.11ms）**必须判不可**；同期 `shadow` 合计跨 0.014ms、`geometry` 跨
# 0.019ms（判据线 = 0.05ms 绝对地板）**必须判可**。判据要复现的正是"同一张表里有的指标能 A/B、
# 有的不能"。绝对地板 0.05ms 的理由：本仓性能问题的最小关心粒度是 0.1ms 级（债-112 原文），带
# 不到它一半 = 这个量级的差异不会被带吃掉；相对项 2% 的理由：大指标（cpu.record 28ms）的绝对带
# 天然大，纯绝对判据会把"在 28ms 上分辨 1ms"误判成不可 A/B。
# 🔴 这是**提示不是闸门**：不出 error、不阻断、不改判定。带只量"同配置同会话的可重复性"，它
#    盖不住跨会话漂移（换会话仍要先复测），也不保证归因正确（一个很稳的指标照样可能被别的东西
#    带着动 —— 例如 cull.fenceWait 稳如磐石，但它跟着 gpu.frame 走，不是你要改的那个东西）。
AB_ABS_MS = 0.05
AB_REL = 0.02
# n=2 的"带"只是两点差（极差估 σ 还要除 d2≈1.128），当不了判据 ⟹ 只印带不印结论。
MIN_BAND_RUNS = 3
# 噪声带表里固定点名的关键读数（顺序 = 打印顺序）；pass / seg 行由代表跑动态补齐。
# 值 = 打印标签（ASCII 为主：带表要按列对齐，CJK 宽度对不齐）。
BAND_KEYS = ("frame_p50", "frame_avg", "cpu_record", "gpu_frame", "cull_stall", "frame_wait",
             "cpu_useful")
BAND_LABELS = {
    "frame_p50": "frame_p50", "frame_avg": "frame_avg", "cpu_record": "cpu.record",
    "gpu_frame": "gpu.frame", "cull_stall": "cull.fenceWait", "frame_wait": "frame.fenceWait",
    "cpu_useful": "cpu_useful", "unattr_p50": "unattr_p50", "unattr_max": "unattr_max",
}


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
    # 渲染网格读 `/state` 的 render 段（能力观测批 2：与 smoke 判据同源，不再捞日志）。
    # 🔴 缺键 = ArmFail 而不是"未取到"：extent 是每臂现场里被当权威读的尺寸，「来源坏了」
    # 与「尺寸没捞到」不能塌成同一个读数（能力路径上 `.get(key, 默认)` 同罪——禁用）。
    if "render" not in state:
        raise ArmFail(f"/state 回包缺 render 段（顶层键 = {sorted(state.keys())}）"
                      "——来源坏了，本臂读数不作数")
    render = state["render"]
    for k in ("renderWidth", "renderHeight"):
        if k not in render:
            raise ArmFail(f"/state render 段缺键 {k!r} ——缺键是错，不回落日志也不回落默认值")
    extent = f"{int(render['renderWidth'])}x{int(render['renderHeight'])}"
    stats, trace = newest_profile(started_wall)
    return stats, trace, {
        "device": smoke.gpu_of(log_text),
        "extent": extent,
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


def flat_metrics(s: dict) -> dict[str, float | None]:
    """一臂读数摊平成 {指标标签: ms}——噪声带逐指标算的就是这张表（值来自 summarize，不另立口径）。"""
    out: dict[str, float | None] = {BAND_LABELS[k]: s.get(k) for k in BAND_KEYS}
    un = s.get("unattributed") or {}
    out[BAND_LABELS["unattr_p50"]] = un.get("p50_ms")
    out[BAND_LABELS["unattr_max"]] = un.get("max_ms")
    for key, v in (s.get("zones") or {}).items():  # 键形如 "cpu|geometry" → 标签 "cpu:geometry"
        kind, _, name = key.partition("|")
        out[f"{kind}:{name}"] = v
    return out


def band_order(zones: dict) -> list[str]:
    """带表的行序：关键读数 → pass（**GPU 在前**，债-112 的"读直接消费者的 GPU zone"）→ cull 四段 → 固定段。"""
    order = [BAND_LABELS[k] for k in BAND_KEYS]
    order += [BAND_LABELS["unattr_p50"], BAND_LABELS["unattr_max"]]
    for name in pass_names(zones):
        order += [f"gpu:{name}", f"cpu:{name}"]
    order += [f"cpu:{n}" for n in CULL_STEPS]
    order += [f"cpu:{n}" for n in FIXED_SEGMENTS]
    return order


def noise_bands(flats: list[dict]) -> dict[str, dict]:
    """逐指标噪声带：同配置 n 次连跑 ⟹ 带 = max−min。

    🔴 n<2 **不出带**（lo/hi/spread 一律 None、判据写"缺测"）——把"没测"印成"噪声带 0.000ms"
    会被读成"这个指标很稳"，比不印更糟。n 在 [2, MIN_BAND_RUNS) 之间只印带不印 A/B 结论。
    """
    out: dict[str, dict] = {}
    for key in dict.fromkeys(k for f in flats for k in f):
        vals = [f[key] for f in flats if isinstance(f.get(key), (int, float))]
        n = len(vals)
        if n < 2:
            out[key] = {"n": n, "lo": None, "hi": None, "med": None, "spread": None, "rel": None,
                        "verdict": f"缺测(n={n})"}
            continue
        lo, hi, med = min(vals), max(vals), median(vals)
        spread = hi - lo
        if n < MIN_BAND_RUNS:
            verdict = f"样本不足(n={n})"
        elif spread <= max(AB_ABS_MS, AB_REL * abs(med)):
            verdict = "可 A/B"
        else:
            verdict = "带内不可 A/B"
        out[key] = {"n": n, "lo": lo, "hi": hi, "med": med, "spread": spread,
                    "rel": (spread / med) if med else None, "verdict": verdict}
    return out


def band_mark(b: dict | None) -> str:
    """带表里的角标：空格 = 可 A/B · ✗ = 带太宽分辨不出 · ? = 样本不足或缺测（**别读成"稳"**）。"""
    if not b:
        return " "
    return {"可 A/B": " "}.get(b["verdict"], "✗" if b["verdict"] == "带内不可 A/B" else "?")


def print_bands(flats: list[dict], bands: dict, order: list[str]) -> None:
    """把逐指标带打出来（advisory：只印数与提示，不参与任何判定、不阻断）。"""
    n = len(flats)
    ser = [f.get(BAND_LABELS["frame_p50"]) for f in flats]
    print(f"    会话内连跑 {n} 次 · frame p50 逐次 = "
          + " / ".join("—" if v is None else f"{v:.3f}" for v in ser))
    print(f"    噪声带（带 = max−min · 中位 = {n} 次中位 · 判据：带 ≤ "
          f"max({AB_ABS_MS}ms, {100 * AB_REL:.0f}%×中位) ⟹ 可 A/B）")
    print(f"      {'指标':<24}{'中位':>9}{'带':>8}{'相对':>8}  判据")
    for label in order:
        b = bands.get(label)
        if b is None:
            continue
        print(f"      {label:<24}{fmt(b['med'], 9)}{fmt(b['spread'], 8)}"
              + (f"{100 * b['rel']:>7.1f}%" if b["rel"] is not None else "—".rjust(8))
              + f"  {b['verdict']}")
    ok = [k for k, b in bands.items() if b["verdict"] == "可 A/B"]
    bad = sorted(((b["spread"], k) for k, b in bands.items() if b["verdict"] == "带内不可 A/B"),
                 reverse=True)
    print(f"    ⟹ 逐指标判据：可 A/B {len(ok)} · 不可 {len(bad)}"
          + ("（不可的按带宽降序：" + " · ".join(f"{k} {s:.3f}" for s, k in bad[:6]) + "）"
             if bad else ""))
    if len(bands) > len(order):
        extra = [k for k in bands if k not in set(order)]
        if extra:
            print(f"      （另有 {len(extra)} 个 zone 未点名，带在 --json 的 bands 里）")
    print("      （带只量同配置同会话的可重复性：盖不住跨会话漂移，也不保证归因正确"
          " → docs/cpu_perf_plan.md §1 条 4）")


def report_arm(arm: str, runs: list[dict]) -> dict:
    """一臂 = 同配置连跑 N 次（N=1 时输出与旧版逐字相同）。返回摘要（--json / --compare 用）。

    主体读数取**代表跑** = `frame_p50` 最接近 n 次中位的那一次，而不是逐指标各取中位：派生量必须
    自洽（`cpu 真干 = cpu.record − 两处等待`、`bound` 由 cpu/gpu 比出来），逐指标中位拼出来的是一台
    不存在的机器。带是**额外印出来**的东西，一毫米都不改主体口径。
    """
    sums = [{**summarize(r["zones"], r["top"]), "unattributed": r["unattr"]} for r in runs]
    p50 = [s["frame_p50"] for s in sums]
    idx = 0
    if len(p50) > 1 and all(isinstance(v, (int, float)) for v in p50):
        idx = min(range(len(p50)), key=lambda i: abs(p50[i] - median(p50)))
    s = report(arm, runs[idx]["zones"], runs[idx]["top"], runs[idx]["unattr"], runs[idx]["info"])
    flats = [flat_metrics(x) for x in sums]
    out = {**s, "repeat": len(runs), "rep_run": idx, "runs": flats, "bands": {}}
    out["stats_file"] = runs[idx]["stats"]
    out["stats_files"] = [r["stats"] for r in runs]
    # 各次跑的测点必须一致，否则"带"量的是两件不同的事（尺寸漂 = 可见集漂 = 根本不是同一台机器）
    if len({r["info"]["extent"] for r in runs}) > 1 or len({r["info"]["device"] for r in runs}) > 1:
        devs = " / ".join(sorted({r["info"]["device"] for r in runs}))
        exts = " / ".join(sorted({r["info"]["extent"] for r in runs}))
        print(f"    ⚠ 各次跑的 device/render 尺寸不一致——测点在漂，带不可用：{devs} / {exts}")
    # 非代表跑的 error 行不能被"代表"掉：任何一次跑出错都要出声（只印首行，全量在日志里）
    for i, r in enumerate(runs):
        if i != idx and r["info"]["error_lines"]:
            print(f"    [error] 第 {i + 1} 次跑：{r['info']['error_lines'][0]}")
    if len(runs) >= 2:
        out["bands"] = noise_bands(flats)
        print_bands(flats, out["bands"], band_order(runs[idx]["zones"]))
    return out


def compare(arm: str, cur: dict, base: dict, bands: dict | None = None) -> None:
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
        line = f"      {k:<12} {b:>{10}{w}} → {a:>{10}{w}}  Δ{d:>+8.3f} ({pct})"
        # 带只在 --repeat≥2 时存在（旧基线 JSON 没有 bands 键也走这条路）：Δ 与"同配置连跑的带"
        # 同量级 = 这次对差分辨不出改动，别记成退化/收益。🔴 仍是提示，不改判定。
        bd = (bands or {}).get(BAND_LABELS.get(k, k))
        if bd and bd.get("spread") is not None:
            line += (f"   ⟂ 本臂带 {bd['spread']:.3f}"
                     + ("（Δ 在带内 = 分辨不出）" if abs(d) <= bd["spread"] else "（Δ 超带）"))
        print(line)


def selftest() -> int:
    """量具自检（`just cpu --selftest`，毫秒级、**不碰 GPU / 不碰配置**）——债-112 的常驻护栏。

    为什么 Python 工具也要自检：`just test` 只覆盖 C++ 侧，而本文件的纯函数那半（带 / 判据 /
    代表跑 / 缺测）没有断言就等于没有——判据被改坏是**静默的**（照样出一张表，只是把不可 A/B 的
    指标印成"可 A/B"，正是本工具要防的那类错）。三条锚：
      ① 台账那组数（债-112）：`frame_p50` 跨 0.69ms 必须判不可、`shadow`/`geometry` 必须判可；
      ② **默认口径逐字不变**：n=1 时 `report_arm` 的输出必须与 `report` 逐字节相同（带表一行都不许漏进默认路径）；
      ③ 样本不足不许伪装成"很稳"：n<2 不出带、n=2 不出判据。
    """
    bad: list[str] = []

    def check(name: str, ok: bool, extra: str = "") -> None:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else '   ' + extra}")
        if not ok:
            bad.append(name)

    def make(seed: int, p50: float, shadow: float, geom: float, drop: str | None = None,
             gshadow: float | None = None, ggeom: float | None = None) -> dict:
        """合成一臂读数（形状 = load_zones/unattributed 的产物，值随便给：自检只验判据不验数值）。

        🔴 **`shadow`/`geom` 是 `cpu:` 录制段，`gshadow`/`ggeom` 是 `gpu:` pass 段，两者不可混**。
        债-112 那组 0.014 / 0.019 是 **GPU zone** 的跨度（`docs/mdi_plan.md` 原文"而 GPU zone
        四次波动 0.014ms"），把它们挂到 `cpu:` 上会造出一条**说反话的锚**：本晚实测
        `cpu:geometry` 同会话带 **0.21ms**（判不可），而 `gpu:geometry` 只有 0.002ms（判可）
        —— 差两个量级，正是 §1 条 4 那句"读直接消费者的 GPU zone"的全部依据。
        """
        zones = {
            ("cpu", "drawFrame"): {"mean_ms": p50 - 0.1}, ("gpu", "gpu.frame"): {"mean_ms": 5.35},
            ("cpu", "geometry"): {"mean_ms": geom},
            ("gpu", "geometry"): {"mean_ms": geom / 1.7 if ggeom is None else ggeom},
            ("cpu", "shadow1"): {"mean_ms": shadow},
            ("gpu", "shadow1"): {"mean_ms": shadow / 4 if gshadow is None else gshadow},
            ("cpu", "fenceWait"): {"mean_ms": 0.022}, ("cpu", "present"): {"mean_ms": 0.011},
        }
        if drop:
            del zones[("cpu", drop)]
        top = {"frames": 120, "drops": 0, "frame_ms": {"p50_ms": p50, "avg_ms": p50 + 0.02},
               "counters": {"draw.count": {"min": 6601, "max": 6601},
                            "vkcmd.count": {"min": 19994, "max": 19994}}}
        return {"zones": zones, "top": top,
                "unattr": {"frames": 120, "mean_ms": 0.003, "p50_ms": 0.003, "max_ms": 0.004},
                "info": {"device": "自检", "extent": "1280x720", "errors": 0, "warnings": 0,
                         "error_lines": []},
                "stats": f"stats_{seed}.json"}

    def arm(runs: list[dict]) -> tuple[str, dict]:
        buf = io.StringIO()
        with redirect_stdout(buf):
            out = report_arm("default", runs)
        return buf.getvalue(), out

    print("=== cpu_baseline 自检（合成读数，不跑引擎）")
    one = make(1, 14.07, 3.08, 3.99)
    old = io.StringIO()
    with redirect_stdout(old):
        report("default", one["zones"], one["top"], one["unattr"], one["info"])
    text1, out1 = arm([one])
    check("n=1 输出与 report() 逐字节相同（默认口径没被带表污染）", old.getvalue() == text1)
    check("n=1 不印带（bands 空 = --json 里也不出带）", out1["bands"] == {} and "噪声带" not in text1)
    check("n=1 stats_file 语义不变（= 唯一那次跑的 stats）", out1["stats_file"] == "stats_1.json")

    _, out2 = arm([make(1, 14.07, 3.08, 3.99), make(2, 13.90, 3.09, 3.98)])
    two = [b for b in out2["bands"].values() if b["n"] == 2]
    check("n=2 只印带不印 A/B 结论", bool(two) and all(b["verdict"].startswith("样本不足")
                                                  for b in two))

    # ① 台账锚（债-112 同代码四跑：`frame_p50` 跨 0.693ms；GPU zone 的 shadow 合计跨 0.014、
    #    geometry 跨 0.019 → `docs/mdi_plan.md` 原文"而 GPU zone 四次波动 0.014ms"）。
    #    🔴 第四条是本晚实测补的**反向锚**：`cpu:geometry` 同会话带 0.215ms 判**不可** ——
    #    同一份 geometry 数据，GPU 侧可 A/B、CPU 侧不可，差两个量级。少了这条，判据被改成
    #    "什么都判可" 时前三条锚可能照样绿（它们全是小数值），有了它才锚住 §1 条 4 的实质。
    _, out4 = arm([make(i, p, 3.08, cg, gshadow=gs, ggeom=gg) for i, (p, cg, gs, gg) in enumerate(
        [(5.729, 3.985, 1.512, 3.980), (5.712, 3.900, 1.507, 3.975),
         (5.338, 4.050, 1.521, 3.994), (5.036, 3.835, 1.512, 3.978)], 1)])
    b4 = out4["bands"]
    check("台账锚：frame_p50 跨 0.693ms → 判不可 A/B", b4["frame_p50"]["verdict"] == "带内不可 A/B",
          str(b4["frame_p50"]))
    check("台账锚：gpu:shadow1 跨 0.014ms → 判可 A/B", b4["gpu:shadow1"]["verdict"] == "可 A/B",
          str(b4["gpu:shadow1"]))
    check("台账锚：gpu:geometry 跨 0.019ms → 判可 A/B", b4["gpu:geometry"]["verdict"] == "可 A/B",
          str(b4["gpu:geometry"]))
    check("反向锚：cpu:geometry 跨 0.215ms → 判不可 A/B（同指标 GPU 侧可、CPU 侧不可）",
          b4["cpu:geometry"]["verdict"] == "带内不可 A/B", str(b4["cpu:geometry"]))
    check("代表跑 = p50 居中那次（派生量自洽的前提）", out4["rep_run"] in (1, 2), str(out4["rep_run"]))
    check("主体读数是真实一次跑（cpu 真干 = cpu.record − 两处等待）",
          abs(out4["cpu_useful"] - (out4["cpu_record"] - out4["frame_wait"])) < 1e-9)

    # ③ 缺测：某次跑没有该 zone → 只有一条样本，绝不能印成"带 0.000 = 很稳"
    _, out3 = arm([make(1, 14.0, 3.08, 3.99), make(2, 14.0, 3.08, 3.99, drop="present"),
                   make(3, 14.0, 3.08, 3.99, drop="present")])
    miss = out3["bands"]["cpu:present"]
    check("缺测(n=1) 不出带值（verdict 点名缺测、spread=None）",
          miss["verdict"] == "缺测(n=1)" and miss["spread"] is None, str(miss))

    drift = [make(1, 14.0, 3.08, 3.99), make(2, 14.0, 3.08, 3.99)]
    drift[1]["info"]["extent"] = "1600x900"
    text5, _ = arm(drift)
    check("各次跑测点尺寸不一致要出声（带不可用）", "测点在漂" in text5)

    old_fmt = io.StringIO()
    with redirect_stdout(old_fmt):
        compare("default", out1, out1, {})
    check("compare 无带时不追加任何标注（旧 --compare 输出不变）",
          "⟂" not in old_fmt.getvalue() and "Δ vs 基线" in old_fmt.getvalue())

    print("=== 自检 " + ("全部通过" if not bad else f"FAIL：{bad}"))
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="CPU 分布账重测（债-02/20/30 读数）")
    ap.add_argument("--arms", default=DEFAULT_ARMS, help=f"逗号分隔，默认 {DEFAULT_ARMS}")
    ap.add_argument("--window", type=int, default=120, help="profiler 帧窗口（默认 120）")
    ap.add_argument("--warmup", type=int, default=40, help="窗口前丢弃的帧数（默认 40）")
    ap.add_argument("--timeout", type=int, default=240, help="单臂等帧数上限秒（默认 240）")
    # 默认 None（不是 1）= 分得清"没要带"与"明说要 1 次"：后者要出声（N=1 没有带，别让人以为
    # 表上没写带就是"很稳"）。repeat=1 时输出与旧版逐字相同。
    ap.add_argument("--repeat", type=int, default=None,
                    help="同配置连跑 N 次 → 逐指标噪声带（债-112）；默认 1 = 不印带、口径逐字不变")
    ap.add_argument("--json", type=Path, help="把读数写到该路径")
    ap.add_argument("--compare", type=Path, help="与该基线 JSON 逐项对差")
    ap.add_argument("--list-arms", action="store_true", help="列出臂表并退出")
    ap.add_argument("--selftest", action="store_true",
                    help="只跑量具自检（合成读数，不碰 GPU / 不碰配置，毫秒级）并退出")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    if a.list_arms:
        for name, (why, _) in ARMS.items():
            print(f"{name:<12} {why}")
        return 0

    repeat = 1 if a.repeat is None else a.repeat
    if repeat < 1:
        smoke.fail("--repeat 要 ≥1（0 次跑不出任何东西，含噪声带）")

    arms = [x.strip() for x in a.arms.split(",") if x.strip()]
    unknown = [x for x in arms if x not in ARMS]
    if unknown:
        smoke.fail(f"未知臂 {unknown}——现读口：just cpu --list-arms")

    smoke.check_prereq()
    smoke.single_instance_or_die()
    port_free_or_die(RPC_PORT)
    print(f"[cpu] {smoke.git_state()} · 窗口 {a.window} 帧 / 热身 {a.warmup} 帧 · 臂 {arms}")
    print("[cpu] 串行跑（并行 = 读数作废），每臂约 15~25s，启动本身约 10s")
    if a.repeat == 1:
        print(f"[cpu] --repeat 1：**单次跑，无噪声带**（N≥2 才有带、N≥{MIN_BAND_RUNS} 才出 A/B 判据，建议 4）")
    elif repeat >= 2:
        print(f"[cpu] 每臂同配置连跑 {repeat} 次 = 墙钟约 ×{repeat}"
              f"（{len(arms)} 臂 × {repeat} 次 = {len(arms) * repeat} 次引擎跑）；带随读数打印")

    wall0 = time.monotonic()
    results: dict[str, dict] = {}
    failed: list[str] = []
    for arm in arms:
        runs: list[dict] = []
        why: str | None = None
        for i in range(repeat):
            if i:  # 连跑之间同样要单实例（上一臂没退干净 = 后面所有读数作废）
                smoke.single_instance_or_die()
            try:
                stats, trace, info = run_engine(arm, a.window, a.warmup, a.timeout)
                zones, top = load_zones(stats)
                if top["frames"] < a.window:
                    raise ArmFail(f"窗口只填了 {top['frames']}/{a.window} 帧——读数不可用")
                runs.append({"zones": zones, "top": top, "unattr": unattributed(trace),
                             "info": info, "stats": stats.name})
            except ArmFail as e:
                # 失败通常是环境问题（引擎没起 / 端口被占 / 窗口没填满），连跑只会连败 ⟹ 不烧时间
                why = f"{e}" if repeat == 1 else f"第 {i + 1}/{repeat} 次跑失败：{e}"
                break
        if runs:
            results[arm] = report_arm(arm, runs)
        if why:
            failed.append(arm)
            print(f"\n=== 臂 {arm} FAIL: {why}"
                  + (f"（已量到的 {len(runs)} 次照常打印，带按 n={len(runs)} 出）"
                     if runs and repeat > 1 else ""))
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
    if repeat >= 2 and results:
        # 关键指标的带单独再来一张表：汇总表的读数来自代表跑，读者第一反应是"这数能信几毛"，
        # 这张表就是回答它的（✗ = 该指标的带盖不住 0.1ms 级差异，别拿它 A/B）。
        cols = ("frame_p50", "cpu_record", "gpu_frame", "cpu_useful")
        print("\n=== 会话内噪声带（同配置连跑，带 = max−min ms）")
        print(f"    {'arm':<12}{'n':>3}" + "".join(f"{BAND_LABELS[c]:>12}" for c in cols)
              + "   （✗ = 带太宽分辨不出 · ? = 样本不足）")
        for arm, s in results.items():
            cells = "".join(f"{fmt((s['bands'].get(BAND_LABELS[c]) or {}).get('spread'), 11)}"
                            f"{band_mark(s['bands'].get(BAND_LABELS[c]))}" for c in cols)
            print(f"    {arm:<12}{s['repeat']:>3}{cells}")
    if a.compare and a.compare.is_file():
        base = json.loads(a.compare.read_text(encoding="utf-8"))["arms"]
        for arm, s in results.items():
            if arm in base:
                compare(arm, s, base[arm], s.get("bands"))
    if a.json:
        payload = json.dumps({"git": smoke.git_state(), "arms": results},
                             ensure_ascii=False, indent=1)
        a.json.write_text(payload, encoding="utf-8")
        print(f"\n[cpu] 读数已写 {a.json}")
    if repeat >= 2:
        # 开销诚实：带是拿墙钟换的，全臂 ×N 要多久印出来，让人自己选 N（口径不因此改变）。
        print(f"\n[cpu] 本次墙钟 {time.monotonic() - wall0:.1f}s（{len(arms)} 臂 × {repeat} 次，串行）")
    if failed:
        smoke.fail(f"这些臂没量到：{failed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
