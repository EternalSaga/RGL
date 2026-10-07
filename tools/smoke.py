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

用法:
    python tools/smoke.py smoke [--variant forward|deferred|offscreen]
                               [--from-build] [--frames N] [--timeout S]
    python tools/smoke.py shot  [--name NAME] [--size WxH]
                               [--variant ...] [--from-build] [--frames N] [--timeout S]

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
CONFIG_FILES = ("camera.lua", "scene.lua", "runtime.lua", "diagnostics.lua", "render_graph.lua")
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
                  velocity_field: bool = False) -> None:
    """把一次性配置铺进 staging 目录并打好运行模式键（每次重建，跑完不残留可复原状态）。"""
    base = (BUILD_RGL / "configs") if from_build else (ROOT / "configs")
    if missing := [f for f in CONFIG_FILES if not (base / f).is_file()]:
        fail(f"配置基准 {base.relative_to(ROOT)} 缺 {missing}")
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


def run_engine(exe: Path, timeout: int) -> tuple[int, str, float]:
    """跑引擎到自行退出（llm_debug 拍完收工）；返回 (退出码, 合并输出, 墙钟秒)。"""
    env = dict(os.environ, RGL_CONFIG_DIR=str(STAGING))
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


def report(log: str) -> tuple[list[str], list[str]]:
    """打印设备/选路现场，返回 (error 行, warning 行)。"""
    errors = [ln for ln in log.splitlines() if "[error]" in ln]
    warnings = [ln for ln in log.splitlines() if "[warning]" in ln]
    gpu = re.search(r"Selected rendering GPU: (.+?) \(score=(\d+)\)", log)
    dgc = re.search(r"DGC path: device='[^']*'.*?-> (.+)", log)
    print(f"GPU    : {gpu.group(1) if gpu else '未取到（引擎没走到选设备？）'}"
          + (f" (score={gpu.group(2)})" if gpu else ""))
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
                         "双向核对：开了没建 velocity RT = FAIL；没开却建了 = FAIL（门关不拢）")
    ap.add_argument("--from-build", action="store_true",
                    help="以 build/rgl/configs 副本为基准（跑调试现场；默认用仓库 configs/）")
    ap.add_argument("--timeout", type=int, default=90, help="引擎最长存活秒数（默认 90）")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    size = None
    if args.size:
        m = re.fullmatch(r"(\d+)\s*[xX]\s*(\d+)", args.size.strip())
        if not m:
            fail(f"--size 要 WxH 形态，收到 {args.size!r}")
        size = (int(m.group(1)), int(m.group(2)))
    if "/" in args.name or ".." in args.name:
        fail(f"--name 只准文件名（不要带路径），收到 {args.name!r}")

    exe = check_prereq()
    single_instance_or_die()

    print(f"主机   : {socket.gethostname()} · {platform.system()} {platform.release()} · {git_state()}")
    for note in stale_shaders():
        print(f"[smoke] 注意（陈旧 SPV）: {note}")
    if newer := stale_binary():
        print(f"[smoke] 注意（二进制早于源，先 just build？）: {', '.join(newer)}")
    if diff := dirty_build_copy():
        print(f"[smoke] 注意（build 副本 configs/ 与仓库不同）: {', '.join(diff)}")

    shot_file = (f"{STAGING.name}/probe.png" if args.mode == "smoke"
                 else f"screenshots/{args.name}.png")
    stage_configs(args.from_build, args.variant, shot_file, args.frames, size,
                  args.render_scale, args.velocity_field)
    print(f"配置   : 基准={'build 副本' if args.from_build else '仓库 configs/'}"
          f"  variant={args.variant or '(随配置)'}  frames={args.frames}"
          f"  size={f'{size[0]}x{size[1]}' if size else '(随配置)'}"
          f"  render_scale={args.render_scale if args.render_scale is not None else '(随配置)'}"
          f"  velocity_field={'on' if args.velocity_field else '(随配置)'}")

    code, log, secs = run_engine(exe, args.timeout)
    LOG_PATH.write_text(log, encoding="utf-8")
    errors, warnings = report(log)

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
    if args.velocity_field and not vel:
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
    if not args.velocity_field and vel:
        fail(f"默认档（velocity_field 关）却建了 velocity RT：{vel.group(0)}"
             " ⟹ 门关不拢，「图里没有这个 pass」的逐位不变红线失效")
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
