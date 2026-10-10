#!/usr/bin/env python3
"""agent 冒烟 / 截图一次性运行器（`just smoke` / `just shot` 的实现体，stdlib-only）。

**解决的痛点**（2026-10-03）：既有流程要手改 `build/rgl/configs/` 副本（headless /
llm_debug / variant），跑完再手 `cp` 复原——配置于是有两个真相源，而"忘复原"是静默的。
引擎早有 `RGL_CONFIG_DIR`（契约见 `rgl/main.cppm` configDir 注释：只覆盖配置目录，
资产仍按 CWD 相对解析），本工具用它把一次性配置落到 `build/rgl/.smoke/`，
**全程不碰任何配置副本**，也就没有需要"复原"的状态。

同时把三机轮换下每次开工都要先问的事打在输出开头（省掉"我这台是哪台 / 跑的是新二进制吗"）：
主机与 GPU、DGC 选路、二进制与 SPV 新鲜度、git 现场，以及 **build 副本是否已被手工
改脏**（脏 = 点名并说明本次跑的是哪一份基准，绝不静默替用户选）。

判定口径：**冒烟 = 真出一帧**。两条子命令共用同一条路径（`llm_debug` 截图），区别只在
PNG 留不留——于是"进程还活着"与"渲染链跑通了"不是两回事，也不靠超时猜"该渲染完了吧"。
errors 计数抓 `[error]` 级日志行：引擎自身 error 与 VVL 的 VUID 都落这一级（severity
映射见 `rgl/utils.cppm` vulkanDebugCallback）。

同步验证档（`--sync`，2026-10-09）：在 `VK_LAYER_VALIDATE_SYNC=1` 下跑，VVL 的
`*_HAZARD detected` 落 `[error]` 级 ⟹ **hazard 天然计入 errors**，无需另设计数。
三条必须知道的坑：
  1. 本机 VVL 层清单自述 `validate_sync` **default = false**，而引擎只
     `setPEnabledLayerNames`（`rgl/vkinstance.cppm`）、项目无 `vk_layer_settings`
     ⟹ **不开这个 env 就等于同步验证从来没开过**（"VVL 常开"给人的一切安全感里，
     同步这一维是空的）。
  2. SDK 文档里那几个写法（`VK_LAYER_ENABLES=VK_ENABLE_SYNCHRONIZATION_VALIDATION`、
     `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`）在本机层清单里
     **不存在 = no-op**，照抄会得到"开了也抓不到"的假结论。
  3. 阳性锚点（**改完 barrier 相关代码后照此重验一次闸门本身**，别只信 errors=0）：
     把 `rgl/render_graph_impl.cppm` 的 `usageInfo(depthWrite).stage` 里的
     `Stage::eLateFragmentTests` 删掉（`cp` 备份，验完还原）→ `just build` →
     `just smoke --sync` **必须 FAIL**（阴影图 DSAO→SRO 报 WRITE_AFTER_WRITE）；
     还原 → 重新 `just build` → 必须回 PASS。闸门不红 = 开关漂了或本档没生效。

用法:
    python tools/smoke.py smoke [--variant forward|deferred|offscreen]
                               [--from-build] [--frames N] [--timeout S] [--sync]
    python tools/smoke.py shot  [--name NAME] [--size WxH]
                               [--variant ...] [--from-build] [--frames N]
                               [--timeout S] [--sync]
    python tools/smoke.py shot  --config-base qa/configs/ssr_rig --name ssr_rig
                               # 用仓库里的靶配置出图（qa/configs/* 的既有目录同此用法）
    python tools/smoke.py shot  --name wetoff --set strength=0
                               # 通用配置注入（可重复，作用于 staging 的 runtime.lua；
                               # 键命中 ≠ 1 即 FAIL——湿面关消融档 / 参数扫描表复现）

退出码: 0 = PASS · 1 = FAIL（前置不满足 / 引擎非零退出 / 有 error 级日志 / 没出图）。
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_RGL = ROOT / "build" / "rgl"
STAGING = BUILD_RGL / ".smoke"
LOG_PATH = BUILD_RGL / "smoke_run.log"
EXE_NAME = "rgl.exe" if os.name == "nt" else "rgl"
# configDir() 里必须齐的配置（少一个引擎就启动失败，见 main_impl.cppm loadAllConfigs）
CONFIG_FILES = ("camera.lua", "scene.lua", "runtime.lua", "diagnostics.lua", "render_graph.lua", "shadow.lua")
VALID_VARIANTS = ("forward", "deferred", "offscreen")


def fail(msg: str) -> None:
    """响亮失败——本工具的价值就在"没跑成要说清为什么"，绝不静默返回 0。"""
    print(f"[smoke] FAIL: {msg}")
    sys.exit(1)


def tail(log: str, n: int = 15) -> str:
    """日志尾部（失败时直接贴出来，省一次"再去打开日志看"）。"""
    return "\n".join("  " + ln for ln in log.splitlines()[-n:])


def rel(path: Path) -> str:
    """仓库相对显示路径；仓库外的路径原样显示（报错信息自己不许先崩）。"""
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def check_prereq() -> Path:
    """前置：构建产物与运行资源齐备（缺了直接指路，不让引擎去报模糊错）。"""
    exe = BUILD_RGL / EXE_NAME
    if not exe.is_file():
        fail(f"找不到 {exe.relative_to(ROOT)}——先 just build")
    shaders = BUILD_RGL / "generated_shaders"
    if not any(shaders.glob("*.spv")):
        fail("build/rgl/generated_shaders/ 下没有 spv——先 just build")
    if not (BUILD_RGL / "configs" / "log_level.json").is_file():
        fail("build/rgl/configs/log_level.json 缺失（日志器按 CWD 读它）——先 just build")
    return exe


def single_instance_or_die() -> None:
    """单实例纪律（并行实例互相遮挡合成，画面与帧率双双失真——见 rgl-verify 测量纪律）。"""
    if os.name == "nt":
        return  # 无 pgrep；tasklist 解析留到有需求再做（宁缺不假实现）
    if subprocess.run(["pgrep", "-x", EXE_NAME], capture_output=True).returncode == 0:
        fail("已有 rgl 进程在跑——先等它退出（pgrep -x rgl），并行跑出来的东西不可信")


def stale_shaders() -> list[str]:
    """SPV 陈旧清单（复用 shader_dep_check 生成的 spv→源闭包清单 = 既有单一真相源）。

    与 imgqa `checkShadersFresh` 同判据：任一源比 spv 新 = 引擎将跑陈旧着色器。
    清单缺失**报问题而不是跳过**——跳过会被读成"着色器是新的"。
    """
    manifest = BUILD_RGL / "shader_deps.txt"
    if not manifest.is_file():
        return ["shader_deps.txt 缺失（随 shader_dep_check 生成）——先 just build"]
    stale: list[str] = []
    for line in manifest.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split()
        spv = BUILD_RGL / "generated_shaders" / cols[0]
        if not spv.is_file():
            continue
        spv_t = spv.stat().st_mtime
        for dep in cols[1:]:
            src = ROOT / dep
            if src.is_file() and src.stat().st_mtime > spv_t:
                stale.append(f"{cols[0]} 旧于 {dep}")
                break
    return stale


def stale_binary() -> list[str]:
    """引擎二进制是否落后于源（只警告——有时就是要跑旧二进制复现问题）。"""
    exe_t = (BUILD_RGL / EXE_NAME).stat().st_mtime
    newer = [
        src.relative_to(ROOT).as_posix()
        for pat in ("rgl/**/*.cppm", "rgl/**/*.slang", "configs/*.lua")
        for src in ROOT.glob(pat)
        if "generated" not in src.parts and src.is_file() and src.stat().st_mtime > exe_t
    ]
    return sorted(newer)[:5]


def git_state() -> str:
    """git 现场（短 hash + 是否脏）——冒烟/截图结论要能对上某个现场。"""
    try:
        commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True, timeout=5).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"],
                               capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "git 不可用"
    return f"{commit or '?'}（工作区脏）" if dirty else (commit or "?")


def dirty_build_copy() -> list[str]:
    """仓库 configs/ 与 build 副本内容不一致的项（副本 = 手工调试现场 = 双源实况）。"""
    src_dir, dst_dir = ROOT / "configs", BUILD_RGL / "configs"
    if not dst_dir.is_dir():
        return []
    diff = [f.name for f in sorted(src_dir.glob("*"))
            if f.is_file() and (not (dst_dir / f.name).is_file()
                                or (dst_dir / f.name).read_bytes() != f.read_bytes())]
    diff += [f.name + "（仅副本有）" for f in sorted(dst_dir.glob("*"))
             if f.is_file() and not (src_dir / f.name).is_file()]
    return diff


def _split_value_comment(rest: str) -> tuple[str, bool, str]:
    """把 `=` 右侧拆成 (值, 有尾逗号, 同行注释)——引号外的 `--` 才算注释起点。

    值里可以带逗号（`{ 1280, 720 }`），所以不能拿逗号当值的终止符，只能先剥注释、
    再剥**最外层**尾逗号。
    """
    quote = ""
    value, comment = rest, ""
    for i, ch in enumerate(rest):
        if quote:
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif rest.startswith("--", i):
            head = rest[:i].rstrip()
            # 注释前的空白回填给注释（否则 `8,  -- 注` 会变成 `3,-- 注`，diff 变脏）
            value, comment = head, rest[:i][len(head):] + rest[i:]
            break
    value = value.rstrip()
    has_comma = value.endswith(",")
    return (value[:-1].rstrip() if has_comma else value), has_comma, comment


def patch_lua(path: Path, key: str, value: str) -> None:
    """换掉 lua 表里 `key = <值>` 的值（保留尾逗号与同行注释）。

    不走 `dofile` 重序列化：sol2 沙箱禁 dofile/require，且重序列化会丢注释。
    键命中 0 次或多于 1 次 = **报错**而不是静默不改——拼错键的失效形态最坏（看着改了、
    引擎看着读了、判据看着在跑，其实跑在默认值上）。行首必须是键名（`headless_size`
    不会被 `size` 命中）。
    """
    text = path.read_text(encoding="utf-8")
    rx = re.compile(rf"^([ \t]*){re.escape(key)}[ \t]*=[ \t]*+(.*)$", re.M)
    hits = list(rx.finditer(text))
    if len(hits) != 1:
        fail(f"{rel(path)} 里键 `{key}` 命中 {len(hits)} 次（须恰好 1 次）"
             f"——配置格式变了？同步改 tools/smoke.py，别让它静默不生效")
    m = hits[0]
    _, comma, comment = _split_value_comment(m.group(2))
    new_line = f"{m.group(1)}{key} = {value}{',' if comma else ''}{comment}"
    path.write_text(text[:m.start()] + new_line + text[m.end():], encoding="utf-8")


def png_size(path: Path) -> tuple[int, int] | None:
    """读 PNG 头拿真实宽高（stdlib，不引图像库）。判据用途见调用处注释。"""
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")


def stage_configs(from_build: bool, variant: str | None, shot_file: str, frames: int,
                  size: tuple[int, int] | None, render_scale: float | None,
                  velocity_field: bool = False, aa: str | None = None,
                  clip: int | None = None,
                  fsr_temporal: bool = False, fsr_jitter: bool = False,
                  ssr: bool = False,
                  config_base: Path | None = None,
                  overrides: list[str] | None = None) -> None:
    """把一次性配置铺进 staging 目录并打好运行模式键（每次重建，跑完不残留可复原状态）。"""
    if config_base is not None and from_build:
        fail("--config-base 与 --from-build 互斥（前者已经指定了基准目录）")
    if config_base is not None and not config_base.is_dir():
        fail(f"--config-base {config_base} 不是目录")
    base = config_base if config_base is not None else \
        ((BUILD_RGL / "configs") if from_build else (ROOT / "configs"))
    if missing := [f for f in CONFIG_FILES if not (base / f).is_file()]:
        # 不用 relative_to(base)：--config-base 允许指到仓库外，那时它会抛 ValueError
        # 而不是报出"缺哪几个文件"这条真正有用的信息。
        fail(f"配置基准 {base} 缺 {missing}")
    shutil.rmtree(STAGING, ignore_errors=True)
    STAGING.mkdir(parents=True)
    for f in base.iterdir():
        if f.is_file():
            shutil.copy2(f, STAGING / f.name)

    runtime = STAGING / "runtime.lua"
    patch_lua(runtime, "headless", "true")
    patch_lua(runtime, "llm_debug", "true")
    patch_lua(runtime, "file", f'"{shot_file}"')
    patch_lua(runtime, "delay_frames", str(frames))
    if size:
        patch_lua(runtime, "size", f"{{ {size[0]}, {size[1]} }}")
    if variant:
        patch_lua(STAGING / "render_graph.lua", "variant", f'"{variant}"')
    if render_scale is not None:
        # 越界值不在这里挡：引擎侧 clampRenderScale 是口径的唯一权威，工具重复实现一遍
        # = 两处规则漂移的机会。工具只负责**事后核对实际生效值**（见 main 里的核对）。
        patch_lua(runtime, "render_scale", repr(float(render_scale)))
    if velocity_field:
        # 批 14.4：逐像素速度场。创建期量（决定图里有没有 velocity pass），无热重载。
        patch_lua(runtime, "velocity_field", "true")
    if ssr:
        # 批 15.1 前半：屏幕空间反射（占位拷贝）。同为创建期量（决定图里有没有 ssr
        # pass）。键 = ssr 块里的 enabled（行首键名全文件唯一，唯一性由 patch_lua 自守）。
        patch_lua(runtime, "enabled", "true")
    # ── 抗锯齿三态（批 14.6）：一个槽位取代旧的 taa.enabled × fsr_upscaler 两个布尔 ──
    if aa:
        patch_lua(runtime, "aa", f'"{aa}"')
    if aa == "fsr":
        # FSR 的两枚调参**总是显式写**：配置文件出厂值是 true/true（= 设计点），而工具的
        # 每个臂必须是**自己写出来的那个配置**——不写键 = 拿出厂默认 = 想量纯空间却静默跑成
        # 时域+抖动（静默换臂）。旗标缺省 = false = 纯空间臂（EASU+RCAS，vendor 每帧 reset、
        # 无历史 ⟹ 按构造无鬼影）；时域臂 = --fsr-temporal（+ --fsr-jitter 才是设计点，
        # 守卫在下面）。velocity_field 不用这里打开：aa=fsr 时**引擎解析期隐含打开**
        # （单一来源在 applyAaKey），旧版工具替引擎补依赖的写法随扁平键一起退役。
        patch_lua(runtime, "temporal", "true" if fsr_temporal else "false")
        patch_lua(runtime, "jitter", "true" if fsr_jitter else "false")
    # ── 历史裁剪档位 ──
    # 为什么要有这个旗标：**红线 B 的定义本身就是「aa=taa + clip_mode = N」**，没有它
    # 每次重立基线都得手改**仓库** configs/scene.lua —— 而那正是「改了副本/源不会自动复原」
    # 这类静默状态的来源。键的唯一性由 `patch_lua` 自己守（命中 ≠ 1 即报错）。
    if clip is not None:
        patch_lua(STAGING / "scene.lua", "clip_mode", str(clip))
    # ── 通用配置注入（--set KEY=VALUE，可重复；批 15.2B，§3.2 未还账④「配置注入通路」）──
    # 具名旗标只覆盖高频臂；低频/一次性扫描（湿面六旋钮、将来的 ssr 18 臂扫描表复现）
    # 不该每加一枚键就长一条旗标。键唯一性护栏 = patch_lua 本体（命中 ≠ 1 即 FAIL，
    # 拼错键不会被读成"改了"）。作用在**全部具名旗标之后** = 显式 --set 覆盖具名档
    # （后写者赢，可预测）；只准打 runtime.lua——要改别的文件说明该长具名旗标了。
    for item in overrides or []:
        key, sep, value = item.partition("=")
        if not sep or not key.strip():
            fail(f"--set 要 KEY=VALUE 形态，收到 {item!r}（VALUE 允许带空格/引号，KEY 不允许）")
        patch_lua(runtime, key.strip(), value)


def run_engine(exe: Path, timeout: int, syncval: bool = False) -> tuple[int, str, float]:
    """跑引擎到自行退出（llm_debug 拍完收工）；返回 (退出码, 合并输出, 墙钟秒)。

    syncval = True 时置 `VK_LAYER_VALIDATE_SYNC=1`（VVL 同步验证在本机的唯一有效开关，
    理由与"哪几个写法是 no-op"见模块 docstring 第 1/2 条）。
    """
    env = dict(os.environ, RGL_CONFIG_DIR=str(STAGING))
    if syncval:
        env["VK_LAYER_VALIDATE_SYNC"] = "1"
    started = time.monotonic()
    try:
        proc = subprocess.run([f"./{EXE_NAME}"], cwd=BUILD_RGL, env=env, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace")
        return proc.returncode, proc.stdout or "", time.monotonic() - started
    except subprocess.TimeoutExpired as exc:
        raw = exc.stdout or ""
        log = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
        LOG_PATH.write_text(log, encoding="utf-8")
        print(f"[smoke] 日志尾部：\n{tail(log)}")
        fail(f"引擎 {timeout}s 未退出（llm_debug 没生效？headless 被别处盖掉？"
             f"）——日志 {LOG_PATH.relative_to(ROOT)}")


def count_severities(log: str) -> tuple[list[str], list[str]]:
    """数 error / warning 级日志行（**不打印**，给只要计数不要现场的工具用）。

    severity 映射见 `rgl/utils.cppm` vulkanDebugCallback：引擎自身 error 与 VVL 的 VUID
    都落 `[error]`。拆出来是为了让 `tools/regress.py` 复用同一口径——两处各写一遍
    `"[error]" in ln` 迟早漂成两个判据。
    """
    return ([ln for ln in log.splitlines() if "[error]" in ln],
            [ln for ln in log.splitlines() if "[warning]" in ln])


def gpu_of(log: str) -> str:
    """启动日志里的实际渲染 GPU（三机轮换，任何读数都要绑机器）。"""
    m = re.search(r"Selected rendering GPU: (.+?) \(score=(\d+)\)", log)
    return f"{m.group(1)} (score={m.group(2)})" if m else "未取到（引擎没走到选设备？）"


def parse_size(text: str | None) -> tuple[int, int] | None:
    """`--size` 解析（WxH）。给 regress 复用，免得两处对同一旗标有两种宽容度。"""
    if not text:
        return None
    m = re.fullmatch(r"(\d+)\s*[xX]\s*(\d+)", text.strip())
    if not m:
        fail(f"--size 要 WxH 形态，收到 {text!r}")
    return int(m.group(1)), int(m.group(2))


def guard_flags(clip: int | None, aa: str | None, fsr_temporal: bool,
                fsr_jitter: bool) -> None:
    """旗标守卫（批 14.6 三态化后从三条收成两条）。

    单独成函数是为了 `tools/regress.py` 复用同一组守卫：守卫只在一处生效的话，
    绕过它的那条路就会静默量到一张与默认逐位相同的图，却报成「档位生效了」。
    """
    if clip is not None and aa != "taa":
        fail("--clip 必须配 --aa taa：TAA 不在图时 taa 段整个不进图，clip_mode 不被任何 "
             "shader 消费 ⟹ 这条旗标会静默量到一张与默认逐位相同的图，却报成「档位生效了」")
    if (fsr_temporal or fsr_jitter) and aa != "fsr":
        fail("--fsr-temporal/--fsr-jitter 是 --aa fsr 的子选项：其余态下 fsr 块调参没有"
             "消费者（引擎只在 aa=fsr 时才接 FSR），只开子选项 = 空开关（配置里写了、"
             "什么都没发生）")
    if fsr_jitter and not fsr_temporal:
        fail("--fsr-jitter 必须配 --fsr-temporal：时域档关着 ⟹ 没有历史重投影去抵消抖动"
             " ⟹ 量到的只是一张整体亚像素平移过的纯空间图（configs/runtime.lua fsr 块注释"
             "早写过这个坑）")


def report(log: str) -> tuple[list[str], list[str]]:
    """打印设备/选路现场，返回 (error 行, warning 行)。"""
    errors, warnings = count_severities(log)
    dgc = re.search(r"DGC path: device='[^']*'.*?-> (.+)", log)
    print(f"GPU    : {gpu_of(log)}")
    print(f"DGC 路 : {dgc.group(1).strip() if dgc else '未取到'}")
    # 阳性锚点：headless 真生效了吗——否则本次跑的是一次交互窗，判据全废
    if "headless 模式" not in log:
        errors.append("[self-check] 日志缺 'headless 模式' 行——本次没跑在 headless 上")
    return errors, warnings


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="RGL 一次性 headless 冒烟 / 截图（走 RGL_CONFIG_DIR，不碰配置副本）")
    ap.add_argument("mode", choices=("smoke", "shot"))
    ap.add_argument("--variant", choices=VALID_VARIANTS,
                    help="render_graph.lua 路线（不给 = 用配置基准里的值）")
    ap.add_argument("--frames", type=int, default=8, help="第 N 帧截图（默认 8 = llm_shot 默认）")
    ap.add_argument("--size", default="", help="截图尺寸 WxH（默认用配置里的 llm_shot.size）")
    ap.add_argument("--name", default="smoke_shot", help="shot 的 PNG 名（screenshots/<name>.png）")
    ap.add_argument("--render-scale", type=float, default=None,
                    help="runtime.lua render_scale（内部渲染分辨率倍率，批 14.3；"
                         "不给 = 用配置基准里的值）。会**核对引擎实际生效值**，"
                         "传了没生效即 FAIL，不会静默跑在默认档上")
    ap.add_argument("--velocity-field", action="store_true",
                    help="runtime.lua velocity_field = true（逐像素速度场，批 14.4）。"
                         "双向核对：开了没建 velocity RT = FAIL；没开却建了 = FAIL（门关不拢）。"
                         "aa=fsr 时无需给：引擎解析期已隐含打开")
    ap.add_argument("--ssr", action="store_true",
                    help="runtime.lua ssr 块 enabled = true（屏幕空间反射，批 15.1 前半 = "
                         "占位拷贝）。双向核对：开了没建 ssr RT 且无「图形态不支持」warn = FAIL；"
                         "没开却建了 = FAIL（门关不拢 = 「门开=门关逐位相同」红线失效）")
    ap.add_argument("--aa", choices=("off", "fsr", "taa"), default=None,
                    help="runtime.lua aa（批 14.6 抗锯齿三态开关）：off = 出厂默认（无 TAA "
                         "无 FSR，红线 A 那张图）；fsr = FSR3-upscaler 做抗锯齿（隐含打开 "
                         "velocity_field，调参走 --fsr-temporal/--fsr-jitter）；taa = 自研 TAA "
                         "做抗锯齿（原 --taa，调参 --clip）。互斥是**结构性的**——旧「--taa 撞 "
                         "--fsr-temporal」那类组合根本表达不出来。不给 = 用配置基准里的值。"
                         "双向核对：请求了没建对应 pass = FAIL；没请求却建了 = FAIL（门关不拢）")
    ap.add_argument("--fsr-temporal", action="store_true",
                    help="runtime.lua fsr 块 temporal = true（让 vendor 累积历史）。不给 = "
                         "显式写 false = 纯空间 EASU+RCAS，每帧 reset、**按构造无鬼影**。必须配 "
                         "--aa fsr（其余态下该调参没有消费者 = 空开关）。生效核对读日志"
                         "「fsr temporal: 开/关」——读**生效**而不是**请求**")
    ap.add_argument("--fsr-jitter", action="store_true",
                    help="runtime.lua fsr 块 jitter = true（FSR 自己的 Halton 抖动，相位 = "
                         "8·(上屏宽/渲染宽)²，0.75 档实测 14）。必须配 --fsr-temporal：单独开它"
                         "只会让输入图整体亚像素平移而没有任何东西去抵消。⚠ 它与 taa.jitter_scale "
                         "是**两个独立旋钮**，不是同一个的别名")
    ap.add_argument("--from-build", action="store_true",
                    help="以 build/rgl/configs 副本为基准（跑调试现场；默认用仓库 configs/）")
    ap.add_argument("--set", dest="overrides", action="append", default=None,
                    metavar="KEY=VALUE",
                    help="通用配置注入（可重复）：对 staging 的 runtime.lua 走 patch_lua "
                         "（键命中 ≠ 1 即 FAIL，护栏与具名旗标同源）。例：`just shot --name "
                         "wetoff --set strength=0` = 湿面关消融档。作用在具名旗标**之后** = "
                         "显式注入覆盖具名档；只准打 runtime.lua（键名以 configs/runtime.lua "
                         "本体为准）")
    ap.add_argument("--config-base", type=Path, default=None,
                    help="以任意配置目录为基准（如 qa/configs/ssr_rig、qa/configs/analytic）。"
                         "与 --from-build 互斥。走的是同一套 staging + 生效核对，"
                         "**不碰该目录也不碰 configs/ 副本**——靶配置因此可以直接放仓库里跑")
    ap.add_argument("--clip", type=int, default=None,
                    help="scene.lua taa.clip_mode = N（历史裁剪档位：0 门关硬盒 / 11 上一版"
                         "交付档 / 13 现行交付档）。🔴 必须配 --aa taa：TAA 不在图时该键根本不被"
                         "消费，单独给它会静默量到一张「什么都没改」的图。档位是否**真的生效**靠"
                         "阳性对照：`clip_mode = 0` 的哈希必须与 11/13 不同（实测 `37b6a5f2…` ≠ "
                         "`360ce0d2…`）。⚠ **别拿「11 与 13 必须不同」当对照**——静止机位下两者"
                         "逐位相同（`360ce0d2…` = 红线 B），臂 13 只在 `motion > 0` 时才分家，"
                         "这正是它转正零像素代价的原因；拿它当对照会把一个正确行为报成回归")
    ap.add_argument("--timeout", type=int, default=90, help="引擎最长存活秒数（默认 90）")
    ap.add_argument("--sync", "--syncval", dest="sync", action="store_true",
                    help="在 VK_LAYER_VALIDATE_SYNC=1 下跑 = 打开 VVL 的同步验证（syncval），"
                         "hazard 以 [error] 落日志 ⟹ 计入 errors。默认不开（syncval 有实打实的"
                         "CPU 成本，且它查的是「依赖漏没漏」而不是「画面对不对」，逐位红线档不需要它）。"
                         "⚠ 本机 VVL 自述 validate_sync 默认 false，而 SDK 文档里 "
                         "VK_LAYER_ENABLES=VK_ENABLE_SYNCHRONIZATION_VALIDATION 那几个写法是 "
                         "no-op——唯一有效开关就是这个 env（详见本文件 docstring）")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    size = parse_size(args.size)
    if "/" in args.name or ".." in args.name:
        fail(f"--name 只准文件名（不要带路径），收到 {args.name!r}")

    guard_flags(args.clip, args.aa, args.fsr_temporal, args.fsr_jitter)

    exe = check_prereq()
    single_instance_or_die()

    print(f"主机   : {socket.gethostname()} · {platform.system()} {platform.release()} · {git_state()}")
    # 同步验证档的现场记录：syncval 是本工具唯一"改 env 才成立"的臂，而 env 不在上面那行
    # 配置摘要里——不写出来，事后拿着 errors=0 的读数无法回答"这次到底开没开"。
    print(f"同步验证: {'开（VK_LAYER_VALIDATE_SYNC=1，hazard 计入 errors）' if args.sync else '关（默认）'}")
    for note in stale_shaders():
        print(f"[smoke] 注意（陈旧 SPV）: {note}")
    if newer := stale_binary():
        print(f"[smoke] 注意（二进制早于源，先 just build？）: {', '.join(newer)}")
    if diff := dirty_build_copy():
        print(f"[smoke] 注意（build 副本 configs/ 与仓库不同）: {', '.join(diff)}")

    shot_file = (f"{STAGING.name}/probe.png" if args.mode == "smoke"
                 else f"screenshots/{args.name}.png")
    stage_configs(args.from_build, args.variant, shot_file, args.frames, size,
                  args.render_scale, args.velocity_field, args.aa,
                  args.clip, args.fsr_temporal, args.fsr_jitter,
                  ssr=args.ssr, config_base=args.config_base,
                  overrides=args.overrides)
    base_desc = (str(args.config_base) if args.config_base is not None
                 else ('build 副本' if args.from_build else '仓库 configs/'))
    print(f"配置   : 基准={base_desc}"
          f"  variant={args.variant or '(随配置)'}  frames={args.frames}"
          f"  size={f'{size[0]}x{size[1]}' if size else '(随配置)'}"
          f"  render_scale={args.render_scale if args.render_scale is not None else '(随配置)'}"
          f"  velocity_field={'on' if args.velocity_field else '(随配置)'}"
          f"  ssr={'on' if args.ssr else '(随配置)'}"
          f"  aa={args.aa or '(随配置)'}"
          + (f"  fsr(temporal={'on' if args.fsr_temporal else 'off'}"
             f",jitter={'on' if args.fsr_jitter else 'off'})" if args.aa == "fsr" else "")
          + (f"  set[{', '.join(args.overrides)}]" if args.overrides else ""))

    code, log, secs = run_engine(exe, args.timeout, args.sync)
    LOG_PATH.write_text(log, encoding="utf-8")
    errors, warnings = report(log)
    # 同步验证档的**生效**核对（同 render_scale/velocity 的纪律：请求 ≠ 生效）。这里能廉价
    # 抓到的静默失效只有一种，但恰好是最贵的一种：diagnostics.no_vvl = true 时引擎根本不
    # 启用验证层 ⟹ syncval env 再对也一条不报，errors=0 会被读成"同步干净"。启动日志的
    # "diagnostics:" 行只列非默认值（见 diagnostics_impl.cppm），故出现 no_vvl 即为 true。
    if args.sync:
        if re.search(r"diagnostics:.*no_vvl", log):
            fail("--sync 档下 no_vvl = true ⟹ 验证层没启用，syncval 等于没跑"
                 "（本档的 errors=0 不作数，改回 no_vvl = false 再测）")
        hazards = [ln for ln in log.splitlines() if "hazard detected" in ln]
        print(f"同步验证: hazard {len(hazards)} 条"
              + (f"（{hazards[0].split('hazard detected')[0].split('] ')[-1].strip()} …）"
                 if hazards else ""))

    shot_path = BUILD_RGL / shot_file
    if code != 0:
        print(f"[smoke] 引擎退出码 {code}，日志尾部：\n{tail(log)}")
        fail("引擎非零退出")
    if "截图已保存" not in log or not shot_path.is_file():
        print(f"[smoke] 日志尾部：\n{tail(log)}")
        fail(f"没拿到截图 {shot_file}——llm_debug 链路没通，见 {LOG_PATH.relative_to(ROOT)}")

    for ln in errors[:5]:
        print(f"[smoke] ERROR 行: {ln.strip()}")
    if len(errors) > 5:
        print(f"[smoke] ...（其余 {len(errors) - 5} 条见日志）")
    grid = re.search(r"Swapchain extent: (\d+)x(\d+)", log)
    dims = png_size(shot_path)
    if grid and dims and dims != (int(grid.group(1)), int(grid.group(2))):
        fail(f"截图尺寸 {dims[0]}x{dims[1]} ≠ 后缓冲 {grid.group(1)}x{grid.group(2)}"
             f"——回读链路与交换链对不上，这张图不能当判据")

    # render_scale 生效核对（批 14.3）。**必须核对而不是只打补丁**：patch_lua 只保证
    # "配置文件里改了"，不保证"引擎按它算了"（键名漂了 / 引擎读错表 / 被钳制）。一个
    # 静默跑在默认档上的 --render-scale 会把"0.5 档回归"做成"1.0 档回归"，而两者都绿。
    eff = re.search(r"render scale = (\S+) → render (\d+)x(\d+) \(swapchain (\d+)x(\d+)\)", log)
    if not eff:
        fail("日志里没有 `render scale = ... → render WxH (swapchain WxH)` 行"
             "——引擎的口径日志被删了或文案改了，批 14.3 的判据失去出处")
    if args.render_scale is not None:
        got = float(eff.group(1))
        if abs(got - args.render_scale) > 1e-6:
            fail(f"请求 --render-scale {args.render_scale} 引擎实际生效 {got}"
                 f"（合法区间 [0.25, 1.0]，越界会被钳制；非有限值回落 1.0）")
    render_grid = (int(eff.group(2)), int(eff.group(3)))
    # velocity_field 生效核对（批 14.4）。同 render_scale 的纪律：patch 只保证"配置里写了"，
    # 不保证"引擎按它建了 RT"。**反向同样要核**：默认档不该出现 velocity RT 行——出现了就说明
    # 门关不拢（图里多了一个 pass，而逐位不变红线单看截图未必抓得住）。
    vel = re.search(r"velocity \(RGBA16F[^)]*\): (\d+)x(\d+) slot (\d+)", log)
    velUnsupported = "velocity: 图形态不支持" in log
    # aa=fsr 隐含 velocity_field（FSR 的 MV 输入就是那张图，隐含发生在引擎解析期）⟹ 核对
    # 判据必须把两条来源并起来，否则 --aa fsr 会被自己的依赖判成「门关不拢」。
    velRequested = args.velocity_field or args.aa == "fsr"
    if velRequested and not vel:
        # 区分两种"没建 RT"：**图形态不支持**（装饰器 warn 后正常跳过，offscreen 变体用
        # blit 上屏，无"写 swapchain 且采色"的 pass = 与 decorateTaaPass 同判据）与
        # **静默没落图**（键名漂了 / 日志文案改了 / 装饰器漏调）。前者是正确行为，
        # 一律 FAIL 会把"正确不支持"报成缺陷，反过来也不许它拿"不支持"当遮羞布：
        # 必须真的看到那条 warn 才算数。
        if not velUnsupported:
            fail("请求 --velocity-field 但日志里既没有 velocity RT 行、也没有"
                 "「velocity: 图形态不支持」告警 ⟹ 装饰器静默没落图"
                 "（配置键名漂了 / 启动日志文案改了 / 装饰调用被删）")
        print("速度场: 本图变体不支持（装饰器已 warn 并跳过）✓ 非错误")
    if not velRequested and vel:
        fail(f"默认档（velocity_field 关）却建了 velocity RT：{vel.group(0)}"
             " ⟹ 门关不拢，「图里没有这个 pass」的逐位不变红线失效")
    # ssr 生效核对（批 15.1 前半）。同 velocity 的双向纪律：正向 = 门开必须见 ssr RT 行
    # （或装饰器显式「图形态不支持」warn）；反向 = 门关一行都不许有——本批的判据本体就是
    # 「门开 = 门关逐位相同」，它依赖「门关 = 图里根本没有这个 pass」，门关不拢时截图
    # 恰好也可能相同（占位拷贝），画面判据抓不住，只有日志能抓。
    ssrRt = re.search(r"ssr \(RGBA16F[^)]*\): (\d+)x(\d+) slot (\d+)", log)
    ssrUnsupported = "ssr: 图形态不支持" in log
    if args.ssr and not ssrRt:
        if not ssrUnsupported:
            fail("请求 --ssr 但日志里既没有 ssr RT 行、也没有「ssr: 图形态不支持」告警"
                 " ⟹ 装饰器静默没落图（配置键名漂了 / 启动日志文案改了 / 装饰调用被删）")
        print("SSR    : 本图变体不支持（装饰器已 warn 并跳过）✓ 非错误")
    if not args.ssr and ssrRt:
        fail(f"默认档（ssr.enabled 关）却建了 ssr RT：{ssrRt.group(0)}"
             " ⟹ 门关不拢，「门开 = 门关逐位相同」红线的前提失效")
    if ssrRt and (int(ssrRt.group(1)), int(ssrRt.group(2))) != render_grid:
        fail(f"ssr RT {ssrRt.group(1)}x{ssrRt.group(2)} ≠ 渲染网格 "
             f"{render_grid[0]}x{render_grid[1]}——占位拷贝要求同格式同网格")
    if ssrRt:
        print(f"SSR    : {ssrRt.group(0)} = 渲染网格 ✓")
    # aa=fsr 生效核对（批 14.5 的门 + 批 14.6 的槽位）。同 velocity 的纪律：双向都要核。
    # 正向 = 门开必须看到 vendor context 构造行；反向 = 门关**一行都不许有**——FSR 是
    # 图外 compute，门关不拢时截图未必变（本批它的输出尚无消费者），只看画面会漏。
    fsrRequested = args.aa == "fsr"
    fsr = re.search(r"FsrUpscaler: context 就绪[^\n]*?render=(\d+)x(\d+) upscale=(\d+)x(\d+)", log)
    fsrOff = re.search(r"fsr: 输入附件不齐", log)
    if fsrRequested and not fsr:
        if fsrOff:
            fail(f"请求 --aa fsr 但引擎报输入附件不齐（{fsrOff.group(0)}）"
                 " ⟹ velocity/depth/色输入三者有一个没进图，FSR 无从接起")
        else:
            fail("请求 --aa fsr 但日志里没有「FsrUpscaler: context 就绪」行"
                 " ⟹ 装饰/门链路静默没落图（配置键名漂了 / 生效判据被改 / 构造日志文案改了）")
    if not fsrRequested and fsr:
        fail(f"默认档（aa 不是 fsr）却构造了 FSR context：{fsr.group(0)}"
             " ⟹ 门关不拢，「零构造零命令」的逐位不变红线失效")
    if fsr:
        # FSR 的两个尺寸必须是**实际**渲染网格与后缓冲网格：vendor 的内部共享图按
        # maxRenderSize 定尺寸，填错 = dispatch 硬失败（FFX_ERROR_OUT_OF_RANGE）。
        if (int(fsr.group(1)), int(fsr.group(2))) != render_grid:
            fail(f"FSR render={fsr.group(1)}x{fsr.group(2)} ≠ 渲染网格 "
                 f"{render_grid[0]}x{render_grid[1]}")
        print(f"FSR: context 就绪 render={fsr.group(1)}x{fsr.group(2)} "
              f"upscale={fsr.group(3)}x{fsr.group(4)} ✓")
    # aa=taa 生效核对。同 velocity/FSR 的纪律：双向都要核。
    # 反向这条尤其值钱：**出厂默认是 aa="off"**（`configs/runtime.lua`），
    # 而红线 A（`fdece9d0…`）就是那张图。若哪天门关不拢、taa pass 悄悄进了图，红线 A 的
    # 含义就变了 —— 而单看截图未必发现（TAA 开也可能只改时域不改这一帧）。
    taaRequested = args.aa == "taa"
    taaRt = re.search(r"taa \(RGBA16F\)[^\n]*out slot (\d+)", log)
    if taaRequested and not taaRt:
        fail("请求 --aa taa 但日志里没有「taa (RGBA16F): … out slot …」行"
             " ⟹ TAA pass 静默没进图（runtime.lua 键名漂了 / decorateTaaPass 的图形态判据"
             "变了 / 启动日志文案改了）")
    if not taaRequested and taaRt:
        fail(f"默认档（aa 不是 taa）却建了 taa RT：{taaRt.group(0)}"
             " ⟹ 门关不拢，红线 A（出厂默认图）的含义已失效")
    if taaRt:
        print(f"TAA  : {taaRt.group(0)} ✓"
              + (f"  clip_mode={args.clip}" if args.clip is not None else ""))
    # fsr 时域调参生效核对。**必须读「生效」而不是「请求」**：日志行报的是引擎实际接上的
    # 档位，而配置里写的可能是另一回事（"静默换臂"的引擎侧版本就藏在这个差里）。
    # 批 14.6 三态之后「TAA 在图 = true 且 temporal 请求开」已不可能同时成立（aa 一个槽位），
    # 所以这里判关只可能是：velocity 没进图 ⟹ fsrUsable 假 / 图形态不支持 / 本来就没请求。
    fsrTemp = re.search(r"fsr temporal: (开|关).*?fsr_temporal = (\w+)，TAA 在图 = (\w+)", log)
    if args.fsr_temporal and not (fsrTemp and fsrTemp.group(1) == "开"):
        detail = (f"引擎判关（配置键 = {fsrTemp.group(2)}，TAA 在图 = {fsrTemp.group(3)}）"
                  if fsrTemp else "日志里没有「fsr temporal:」行（fsrUsable 为假 = FSR 根本没接进图）")
        fail(f"请求 --fsr-temporal 但 vendor 时域没开：{detail}"
             " ⟹ 这条读数会是「纯空间升频」却被记成时域档")
    if not args.fsr_temporal and fsrTemp and fsrTemp.group(1) == "开":
        fail("默认档（fsr_temporal 关）却开着 vendor 时域 ⟹ 门关不拢，"
             "「FSR 关 = 逐位不变」的红线含义已失效")
    # fsr_jitter 生效核对（正向 = 必须看到接入行并读出相位数；反向 = 门关一行不许有）。
    jit = re.search(r"fsr_jitter: 抖动已接入投影[^\n]*?相位 (\d+)", log)
    if args.fsr_jitter and not jit:
        fail("请求 --fsr-jitter 但日志里没有「fsr_jitter: 抖动已接入投影」行"
             " ⟹ 抖动静默没接（键名漂了 / 生效判据被改 / 日志文案改了）")
    if not args.fsr_jitter and jit:
        fail(f"默认档（fsr_jitter 关）却把抖动接进了投影：{jit.group(0)} ⟹ 门关不拢")
    if fsrTemp or jit:
        bits = []
        if fsrTemp:
            bits.append("时域=vendor累积历史" if fsrTemp.group(1) == "开" else "时域=纯空间")
        if jit:
            bits.append(f"抖动相位 {jit.group(1)}")
        print(f"FSR档: {' · '.join(bits)} ✓")
    if vel:
        # 速度场必须与深度**同网格**：它的输出单位是"渲染分辨率像素"，网格不同则单位错。
        if (int(vel.group(1)), int(vel.group(2))) != render_grid:
            fail(f"velocity RT {vel.group(1)}x{vel.group(2)} ≠ 渲染网格 "
                 f"{render_grid[0]}x{render_grid[1]}——速度场必须与深度同网格")
        print(f"速度场: {vel.group(0)} = 渲染网格 ✓")
    if size and dims and dims != size:
        fail(f"请求 --size {size[0]}x{size[1]} 实得 {dims[0]}x{dims[1]}"
             f"（headless 下 llm_shot.size 应当直接生效；启动行：{grid.group(0) if grid else '无 Swapchain extent 日志'}）")
    print(f"网格   : 后缓冲 {dims[0]}x{dims[1]}  渲染 {render_grid[0]}x{render_grid[1]}"
          f"  render_scale={eff.group(1)}"
          if dims else "网格   : 未取到（PNG 头异常？）")
    kb = shot_path.stat().st_size // 1024
    if args.mode == "smoke":
        print(f"截图   : {kb} KB（冒烟探针，随 staging 丢弃）")
        shot_path.unlink(missing_ok=True)
    else:
        print(f"截图   : {shot_path} ({kb} KB)")
    print(f"日志   : {LOG_PATH.relative_to(ROOT)}")
    print(f"结果   : {'FAIL' if errors else 'PASS'}  errors={len(errors)}"
          f"  warnings={len(warnings)}  用时 {secs:.1f}s")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
