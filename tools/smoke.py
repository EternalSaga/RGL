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
    python tools/smoke.py shot  --name refcam --set camera:mode='"trackball"' \
                               --set "camera:position={ -0.07, 0.06, -0.02 }"
                               # 注入别的配置文件：`--set [FILE:]KEY=VALUE`，FILE ∈
                               # runtime|camera|scene|render_graph|diagnostics（缺省 runtime，
                               # 旧形态逐字不变）。护栏同 patch_lua：键命中 ≠ 1 即 FAIL
    python tools/smoke.py shot  --name gbuf3 --set diagnostics:dump_gbuf=3
                               # dump_gbuf 档下引擎**忽略 llm_shot.file**，出图名写死
                               # screenshots/gbuffer_3.png ⟹ 本工具按那个名字核对（见下）
                               # ⚠ 走 `just shot` 时值里的引号要**再逃一层**（just 把参数原样
                               # 拼进 shell 行、sh 再剥一次）：`just shot --name refcam --set
                               # 'camera:mode=\"trackball\"'`；带空格的值同理（`--set
                               # 'camera:position="{ 1, 2, 3 }"'`）。上面的 python 直调无此问题。

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
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_RGL = ROOT / "build" / "rgl"
STAGING = BUILD_RGL / ".smoke"
LOG_PATH = BUILD_RGL / "smoke_run.log"
EXE_NAME = "rgl.exe" if os.name == "nt" else "rgl"
# configDir() 里必须齐的配置（少一个引擎就启动失败，见 main_impl.cppm loadAllConfigs）
CONFIG_FILES = ("camera.lua", "scene.lua", "runtime.lua", "diagnostics.lua", "render_graph.lua", "shadow.lua")
VALID_VARIANTS = ("forward", "deferred", "offscreen")
# `--set [FILE:]KEY=VALUE` 的目标文件白名单（批 15.2 子步 D1）。为什么要有这张表而不是
# "前缀是什么就打开同名文件"：拼错的目标（`--set camrea:mode=...`）会去 staging 里新建一个
# 没人加载的 camrea.lua ⟹ **静默不生效**，而 patch_lua 的"键命中 ≠ 1 即 FAIL"护栏在错误
# 的文件上照样能命中 0 次以外的形态（比如恰好同名）。白名单 + 配置文件清单同源才拦得住。
# 🔴 「同源」是**从磁盘派生**，不是手抄一份名单：本常量曾经手抄五个，configs/shadow.lua
# 落地时没人回来加它 ⟹ `--set shadow:mode=pcss` 一直撞在白名单上（批 N+1 实测：报
# 「目标文件 'shadow' 不在白名单」）。护栏一点没松——拼错的前缀磁盘上没有那个文件，照样 fail。
def set_targets() -> tuple[str, ...]:
    """`--set [FILE:]KEY=VALUE` 允许的目标文件 = 仓库 configs/ 里实际存在的 `*.lua`。"""
    names = tuple(sorted(p.stem for p in (ROOT / "configs").glob("*.lua")))
    if not names:
        fail(f"{rel(ROOT / 'configs')} 里一个 .lua 都没有——配置目录不对？"
             "（白名单从它派生，扫空 = 这条护栏已经空转）")
    return names


def fail(msg: str, *, say=print) -> None:
    """响亮失败——本工具的价值就在"没跑成要说清为什么"，绝不静默返回 0。

    `say` 只换**输出通道**（`verdicts` 把它接到注入的 sink 上，selftest 才不会拿一堆
    FAIL 刷屏），判定语义一个字不动：**首条失败即 `exit(1)`**。
    """
    say(f"[smoke] FAIL: {msg}")
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


def _lua_block_span(text: str, block: str) -> tuple[int, int]:
    """返回 `block = { … }` 块的 (内容起点, 块尾 `}` 位置)。命中 0 或多处 = 报错。

    只数花括号配对，不解析 lua（配置里 `{` 只出现在表构造里；真出现字符串内花括号会算错，
    那种形态由"子键命中次数"护栏兜住——算错区域后子键多半命中 0 次，直接红，不会静默）。
    """
    rx = re.compile(rf"^[ \t]*{re.escape(block)}[ \t]*=[ \t]*\{{", re.M)
    hits = list(rx.finditer(text))
    if len(hits) != 1:
        fail(f"配置里块 `{block} = {{` 命中 {len(hits)} 次（须恰好 1 次）"
             f"——配置格式变了？同步改 tools/smoke.py，别让它静默不生效")
    start = hits[0].end()
    depth = 1
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return start, i
    fail(f"配置里块 `{block} = {{` 没找到配对的 `}}`")
    raise SystemExit  # 不可达，fail 已 exit


def patch_lua(path: Path, key: str, value: str) -> None:
    """换掉 lua 表里 `key = <值>` 的值（保留尾逗号与同行注释）。

    不走 `dofile` 重序列化：sol2 沙箱禁 dofile/require，且重序列化会丢注释。
    键命中 0 次或多于 1 次 = **报错**而不是静默不改——拼错键的失效形态最坏（看着改了、
    引擎看着读了、判据看着在跑，其实跑在默认值上）。行首必须是键名（`headless_size`
    不会被 `size` 命中）。

    **嵌套键用点号**（`meshlet.enable`）：先定位 `meshlet = {` 块，再在块内匹配 `enable`。
    为什么必须有它：新门一律是嵌套块（`meshlet`/`wet`/`fsr`/`taa`/`ssr`），而裸子键
    `enable` 能不能命中，取决于别的块有没有恰好同名的键——**那是运气不是护栏**
    （2026-10-12 实测：`--set enable=true` 当时唯一命中 meshlet 块，加第二个 `enable`
    就变成"命中 2 次"红，或更坏：静默改到别人的键）。裸键仍可用（旧调用逐字不变）。
    """
    parent, _, child = key.rpartition(".")
    if parent:
        text = path.read_text(encoding="utf-8")
        lo, hi = _lua_block_span(text, parent)
        scope = text[lo:hi]
        offset = lo
    else:
        scope = text = path.read_text(encoding="utf-8")
        offset = 0
    # 🔴 **单行子表**（`izb = { on = false, grid = 512, dump_frame = 0 }`）必须走另一条式子。
    # 行锚定式子在块作用域里靠 `^` 找行首，而 `re.M` 的 `^` **也匹配 scope 的 0 号位**——
    # 单行块的 scope 起点在行中间 ⟹ 第一个键命中，`.*$` 一路吃到行尾，替换时把
    # **后面的兄弟键连同 `}` 一起吃掉**（实测：`--set shadow:izb.on=true` 把
    # `grid`/`dump_frame` 静默删成 lua 默认值，护栏全绿、量的是默认档）。其余子键则
    # 反过来命中 0 次（`--set shadow:izb.dump_frame=1` 直接红）。
    inline = bool(parent) and "\n" not in scope
    if inline:
        # 前缀 `[{},]?` 与后缀 `[,}]?` 都是**可选**的：块作用域从 `{` 之后开始、到 `}` 之前
        # 结束 ⟹ 第一个键前面什么都没有、最后一个键后面也没有分隔符。
        # 值式子只吃标量——单行块里再嵌一层表（`{ a = { 1, 2 } }`）本工具不支持，那种键要改
        # 请先把配置改成多行形态（切在逗号处会写出坏 lua，引擎解析期就炸，不会静默）。
        rx = re.compile(
            rf"([{{,}}]?[ \t]*){re.escape(child)}[ \t]*=[ \t]*([^,}}\n]*)([ \t]*)([,}}]?)")
    else:
        rx = re.compile(rf"^([ \t]*){re.escape(child)}[ \t]*=[ \t]*+(.*)$", re.M)
    hits = list(rx.finditer(scope))
    if len(hits) != 1:
        where = f"块 `{parent}` 内的键 `{child}`" if parent else f"键 `{key}`"
        fail(f"{rel(path)} 里{where}命中 {len(hits)} 次（须恰好 1 次）"
             f"——配置格式变了？同步改 tools/smoke.py，别让它静默不生效")
    m = hits[0]
    # 🔴 少写引号的值 = **静默空操作**，比键名拼错更坏。`--set shadow:mode=pcss` 写进 lua 是
    # `mode = pcss`，pcss 是个未定义全局 = nil，解析器按「缺键回落默认」处理 ⟹ 引擎照跑默认档、
    # patch_lua 的「键命中 ≠ 1」护栏全绿、smoke 报 PASS errors=0，而**量的是默认档**。判据 =
    # 原值是带引号字符串而新值是裸标识符 ⟹ 几乎必然是漏引号（真要写裸值，自己把引号写全）。
    if inline:
        oldRaw, comma, comment = m.group(2).strip(), "", m.group(3) + m.group(4)
        a, b = offset + m.start(), offset + m.end()
        new_line = f"{m.group(1)}{child} = {value}{comment}"
    else:
        oldRaw = _split_value_comment(m.group(2))[0].strip()
        a, b = offset + m.start(), offset + m.end()
        _, comma, comment = _split_value_comment(m.group(2))
        new_line = f"{m.group(1)}{child} = {value}{',' if comma else ''}{comment}"
    newRaw = value.strip()
    if oldRaw[:1] in {'"', "'"} and newRaw[:1] not in {'"', "'"}:
        fail(f"{rel(path)} 的键 `{child}` 原值是**字符串**（{oldRaw}），而 --set 给的是裸值 "
             f"{newRaw!r} ⟹ 写进 lua 是 `{child} = {newRaw}`，那是个未定义全局 = nil = 回落默认，"
             f"配置改了但引擎读不到（静默空操作）。要字符串值请带引号：--set '{child}=\"{newRaw}\"'")
    path.write_text(text[:a] + new_line + text[b:], encoding="utf-8")


def split_set(item: str) -> tuple[str, str, str]:
    """把 `--set` 的一项拆成 (目标文件 stem, 键, 值)。

    形态 `[FILE:]KEY=VALUE`，FILE 缺省 = runtime（批 15.2B 的旧形态 `--set KEY=VALUE` 逐字
    不变）。FILE 只准是 `set_targets()`（= 仓库 configs/ 里实际存在的 .lua）之一——不在白名单
    里的前缀（`camrea:` 拼错）会去新建一个引擎根本不加载的文件，护栏再严也是在错误的文件上严。

    值里带冒号是合法的（`--set runtime:url="http://x"`），所以判"这是不是前缀"的条件是
    **冒号左侧不含 `=`**：含 `=` 说明冒号在值里，整项按无前缀处理。
    """
    head, sep, rest = item.partition(":")
    target = "runtime"
    if sep:
        if head in set_targets():
            target, item = head, rest
        elif "=" not in head:
            fail(f"--set 的目标文件 {head!r} 不在白名单 {set_targets()}"
                 f"（不带前缀 = 打 runtime.lua；键名以 configs/<文件>.lua 本体为准）")
    key, eq, value = item.partition("=")
    if not eq or not key.strip():
        fail(f"--set 要 [FILE:]KEY=VALUE 形态，收到 {item!r}（VALUE 允许带空格/引号，KEY 不允许）")
    return target, key.strip(), value


def set_value(overrides: list[str] | None, target: str, key: str) -> str | None:
    """`--set` 注入里 `[target:]key` 的**最后一个** VALUE（没注入 = None）。

    「最后一个」= 与 patch_lua 的作用顺序同口径：`--set` 逐条后写者赢（见 stage_configs
    末尾那个循环），护栏读到的必须是真正落到配置里的那一次。lua 字符串值的引号剥掉
    （`'"taa"'` → `taa`），比较方不必再记两种写法。
    """
    found = None
    for item in overrides or []:
        t, k, v = split_set(item)
        if t == target and k == key:
            found = v.strip().strip('"').strip("'")
    return found


def set_switch_on(overrides: list[str] | None, target: str, key: str) -> bool:
    """`--set` 有没有把某个布尔开关打开（VALUE ∈ {true, 1}，大小写不敏感）。

    🔴 **为什么"门关不拢"类反向判据必须问这一句**：`--ssr` / `--velocity-field` 这类**具名
    旗标**只是配置的一条来路，`--set ssr.enabled=true` 是另一条**等价**来路（低频/一次性臂
    走它，见 `--set` 的 help）。反向判据若只看具名旗标，就会在「用 --set 开的门」上把
    **门开**读成**门关不拢**——批 N+1 实测四条全部中招：`--set ssr.enabled=true` /
    `--set velocity_field=true` / `--set aa="taa"` / `--set aa="fsr"`（最后这条还是被
    隐含依赖 velocity_field 撞的）。护栏反过来咬自己人时，唯一的绕行办法就是手改配置副本，
    而那正是本工具的立项目标里点名要消灭的双源。
    """
    return (set_value(overrides, target, key) or "").lower() in {"true", "1"}


def gbuf_channel(overrides: list[str] | None) -> int | None:
    """从 `--set` 里取 dump_gbuf 通道号（N>0 才算开），没注入/值为负 → None。

    为什么工具必须认识这个键：引擎在该档下**忽略 `llm_shot.file`**，出图名写死
    `screenshots/gbuffer_{N}.png`（`rgl/llm_debug_impl.cppm` 的 writeCapturePng）。于是
    "请求的名字"与"磁盘上的名字"是两个，而本工具判 PASS 的根据恰恰是"请求的那个文件在不在"
    ——不核对名字，注入 dump_gbuf 的每一次跑都会误报"没拿到截图"（或更坏：命中上一次留下的
    同名旧图）。
    """
    for item in overrides or []:
        target, key, value = split_set(item)
        if target != "diagnostics" or key != "dump_gbuf":
            continue
        try:
            n = int(value.strip())
        except ValueError:
            fail(f"--set diagnostics:dump_gbuf 要整数（-1 = 关，0..4 = 通道号），收到 {value!r}")
        # 🔴 `>= 0` 不是笔误：引擎侧 0..4 都是合法通道（0 = albedo），-1 才是"关"。
        # 旧写法 `n > 0` 让 dump_gbuf=0 返回 None ⟹ 工具去找 llm_shot.file 里那个引擎
        # 根本不写的名字 ⟹ ch0 采集每次误报"没拿到截图"（★1-a1ii-β-1 子 agent 发现）。
        return n if n >= 0 else None
    return None


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
        # 批 15.1 前半：屏幕空间反射。同为创建期量（决定图里有没有 ssr pass；15.3B 起 =
        # trace + apply 两个）。键 = ssr 块里的 enabled（行首键名全文件唯一，唯一性由
        # patch_lua 自守）。
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
    # ── 通用配置注入（`--set [FILE:]KEY=VALUE`，可重复；批 15.2B 起键，批 15.2 D1 加文件前缀）──
    # 具名旗标只覆盖高频臂；低频/一次性扫描（湿面六旋钮、将来的 ssr 18 臂扫描表复现）
    # 不该每加一枚键就长一条旗标。键唯一性护栏 = patch_lua 本体（命中 ≠ 1 即 FAIL，
    # 拼错键不会被读成"改了"）。作用在**全部具名旗标之后** = 显式 --set 覆盖具名档
    # （后写者赢，可预测）。前缀缺省 runtime；带前缀是为了参考机位（camera.lua 三键联动）
    # 与 dump_gbuf（diagnostics.lua）这类**必须跨文件**的一次性臂——以前只能手改 configs 副本，
    # 而"改了副本不会自动复原"正是本工具立项目标里点名要消灭的双源。
    for item in overrides or []:
        target, key, value = split_set(item)
        patch_lua(STAGING / f"{target}.lua", key, value)


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


def caps_of(log: str) -> str:
    """设备 mesh/task 能力 + DGC `graphicsIndirect` —— 四机矩阵那类能力表的**唯一现读口**。

    🔴 文档只准写"现读口 = `just smoke` 的本行 / 启动日志 `Mesh/task device caps` 行"，
    **不准把数值抄进文档**：换机器就漂（三机轮换 + `run-igpu/dgpu/intel` 强制换设备），
    而漂了的能力列会被下一个会话当权威读。

    数值本体 = `rgl/utils.cppm::logMeshTaskDeviceCaps`（选完设备**无条件**打，与配置门无关，
    描述的是引擎**实际选中**的那台）；`graphicsIndirect` 取自 `DGC path:` 行（三条分支都带）。
    """
    m = re.search(r"Mesh/task device caps: (.+)", log)
    if not m:
        return "未取到（旧构建没有 caps 行？引擎没走到选设备？）"
    caps = m.group(1).strip()
    g = re.search(r"DGC path: device='[^']*' graphicsIndirect=(\S+)", log)
    return f"{caps} graphicsIndirect={g.group(1) if g else '未取到'}"


def parse_size(text: str | None) -> tuple[int, int] | None:
    """`--size` 解析（WxH）。给 regress 复用，免得两处对同一旗标有两种宽容度。"""
    if not text:
        return None
    m = re.fullmatch(r"(\d+)\s*[xX]\s*(\d+)", text.strip())
    if not m:
        fail(f"--size 要 WxH 形态，收到 {text!r}")
    return int(m.group(1)), int(m.group(2))


def guard_flags(clip: int | None, aa: str | None, fsr_temporal: bool,
                fsr_jitter: bool, aa_eff: str | None = None, *, say=print) -> None:
    """旗标守卫（批 14.6 三态化后从三条收成两条）。

    单独成函数是为了 `tools/regress.py` 复用同一组守卫：守卫只在一处生效的话，
    绕过它的那条路就会静默量到一张与默认逐位相同的图，却报成「档位生效了」。

    `aa_eff` = 含 `--set aa=…` 之后的**实际** aa（后写者赢）。缺省回落到具名旗标
    （regress.py 的旧调用逐字不变）。不并这一条的话，`--set aa=fsr --fsr-temporal`
    这条**合法**组合会被守卫判成"空开关"拒掉——护栏反过来挡住了合法入口（同批修掉的
    四条反向判据是同一个病：只看具名旗标，认不出 `--set` 这条等价来路）。

    `say` 与 `verdicts` 同口径（只换输出通道、不改判定），selftest 才敢直接驱动它。
    """
    aa = aa_eff if aa_eff is not None else aa
    if clip is not None and aa != "taa":
        fail("--clip 必须配 --aa taa：TAA 不在图时 taa 段整个不进图，clip_mode 不被任何 "
             "shader 消费 ⟹ 这条旗标会静默量到一张与默认逐位相同的图，却报成「档位生效了」",
             say=say)
    if (fsr_temporal or fsr_jitter) and aa != "fsr":
        fail("--fsr-temporal/--fsr-jitter 是 aa=fsr 的子选项：其余态下 fsr 块调参没有"
             "消费者（引擎只在 aa=fsr 时才接 FSR），只开子选项 = 空开关（配置里写了、"
             "什么都没发生）", say=say)
    if fsr_jitter and not fsr_temporal:
        fail("--fsr-jitter 必须配 --fsr-temporal：时域档关着 ⟹ 没有历史重投影去抵消抖动"
             " ⟹ 量到的只是一张整体亚像素平移过的纯空间图（configs/runtime.lua fsr 块注释"
             "早写过这个坑）", say=say)


def report(log: str, *, say=print) -> tuple[list[str], list[str]]:
    """打印设备/选路现场，返回 (error 行, warning 行)。"""
    errors, warnings = count_severities(log)
    dgc = re.search(r"DGC path: device='[^']*'.*?-> (.+)", log)
    say(f"GPU    : {gpu_of(log)}")
    say(f"设备规格: {caps_of(log)}")
    say(f"DGC 路 : {dgc.group(1).strip() if dgc else '未取到'}")
    # 阳性锚点：headless 真生效了吗——否则本次跑的是一次交互窗，判据全废
    if "headless 模式" not in log:
        errors.append("[self-check] 日志缺 'headless 模式' 行——本次没跑在 headless 上")
    return errors, warnings


@dataclass
class VerdictCfg:
    """`verdicts` 的输入面：main 从命令行派生后传入，selftest 直接构造合成档。

    🔴 刻意**不含** Path / 磁盘 / 时钟：截图在不在磁盘上、PNG 真实宽高由 main 读好后以
    `shot_exists` / `dims` 传进来——判据函数一碰磁盘，selftest 就得去造真图（更糟是造真引擎）。
    默认值 = 「默认档 + 全好」那份合成现场，用例只写它要改的那一两个字段；不这样写，
    26 条判据的用例会长成 26 份参数表，而参数表本身没人校验。
    """

    expect_file: str = ".smoke/probe.png"     # 该出现在磁盘上的出图名（相对 build/rgl）
    shot_exists: bool = True                  # 那张图真在磁盘上吗（main 读）
    dims: tuple[int, int] | None = None       # PNG 头里的真实宽高（读不到 = None）
    size: tuple[int, int] | None = None       # --size 请求值
    render_scale: float | None = None         # --render-scale 请求值
    velocity_field: bool = False              # --velocity-field
    ssr: bool = False                         # --ssr
    fsr_temporal: bool = False                # --fsr-temporal
    fsr_jitter: bool = False                  # --fsr-jitter
    clip: int | None = None                   # --clip（只进 TAA 读数行）
    sync: bool = False                        # --sync
    aa_eff: str | None = None                 # 含 `--set aa=…` 之后的**实际** aa
    overrides: list[str] | None = None        # --set 原样：反向判据要认这条等价来路
    gbuf: int | None = None                   # --set diagnostics:dump_gbuf=N（None = 普通档）
    log_rel: str = str(LOG_PATH.relative_to(ROOT))  # 报错里指的日志路径（常量，非输入）


def verdicts(code: int, log: str, cfg: VerdictCfg, *,
             say=print) -> tuple[list[str], list[str]]:
    """读引擎退出码 + 日志做 PASS/FAIL 判定；任一判据不过 → `fail()` 当场退出。

    **为什么拆出来**（债-130①）：判据唯一的资产是「它还能不能看见东西」，而长在 main() 里的
    判据没有任何东西能驱动它——selftest 一度对它零条断言，「这 26 条 fail() 到底会不会红」
    只能靠读代码推理。拆成纯函数后 selftest 用合成日志逐条钉住（见 selftest 的判据段用例）。

    三条不变量，改判据前先读：
      1. **首条失败即退出**（fail = 说 + exit(1)）。🔴 不许改成「收集全部失败再一次性报」：
         后面的判据依赖前面的判据已经退出（`eff` / `saved` 都是正则命中后才 `.group()`）。
      2. 全部输入走 `cfg` 与参数——函数体不读 args / argv / 磁盘 / 时钟（可测性的全部来源）。
      3. 读数行经 `say` 输出（默认 print）；selftest 传 sink，免得污染回归输出。

    返回 (error 行, warning 行)：`errors` 非空本身就是**最后一条判据**（main 据此打
    `结果 : FAIL` 并返回 1），它不走 fail()，所以不能在这里丢掉。

    改判据要**两个方向**都留下证据：判据失效（改成不匹配 / 恒假）要有用例红，判据被放宽
    （削掉字面量 ⟹ 错误日志也打 ✓）也要有用例红——后者是假绿，只有近邻行能抓，见 selftest ⑭。
    """
    gbuf = cfg.gbuf  # dump_gbuf 通道号（None = 普通档，出图名走 llm_shot.file）
    size = cfg.size  # --size 请求值（None = 随配置）

    errors, warnings = report(log, say=say)
    # 同步验证档的**生效**核对（同 render_scale/velocity 的纪律：请求 ≠ 生效）。这里能廉价
    # 抓到的静默失效只有一种，但恰好是最贵的一种：diagnostics.no_vvl = true 时引擎根本不
    # 启用验证层 ⟹ syncval env 再对也一条不报，errors=0 会被读成"同步干净"。启动日志的
    # "diagnostics:" 行只列非默认值（见 diagnostics_impl.cppm），故出现 no_vvl 即为 true。
    if cfg.sync:
        if re.search(r"diagnostics:.*no_vvl", log):
            fail("--sync 档下 no_vvl = true ⟹ 验证层没启用，syncval 等于没跑"
                 "（本档的 errors=0 不作数，改回 no_vvl = false 再测）", say=say)
        hazards = [ln for ln in log.splitlines() if "hazard detected" in ln]
        say(f"同步验证: hazard {len(hazards)} 条"
            + (f"（{hazards[0].split('hazard detected')[0].split('] ')[-1].strip()} …）"
               if hazards else ""))

    if code != 0:
        say(f"[smoke] 引擎退出码 {code}，日志尾部：\n{tail(log)}")
        fail("引擎非零退出", say=say)
    if "截图已保存" not in log or not cfg.shot_exists:
        say(f"[smoke] 日志尾部：\n{tail(log)}")
        fail(f"没拿到截图 {cfg.expect_file}——llm_debug 链路没通，见 {cfg.log_rel}", say=say)
    # dump_gbuf 档的**出图名核对**：光"磁盘上有张 gbuffer_N.png"不够——它可能是上一次别的
    # 通道/别的尺寸留下的。核对引擎自己报的路径（ecs_frame_diag 的「截图已保存 (<路径>)」行）
    # 与按通道号推出的名字一致，才算这次注入真的落到了这张图上。
    if gbuf is not None:
        saved = re.search(r"截图已保存 \(([^)]*)\)", log)
        want = f"gbuffer_{gbuf}.png"
        if not saved or not saved.group(1).endswith(want):
            fail(f"dump_gbuf={gbuf} 档下引擎报的出图路径不是 {want}"
                 f"（日志：{saved.group(1) if saved else '没有「截图已保存」行'}）"
                 " ⟹ 出图名口径变了，本工具的核对与 diffstruct 的 --gbuf 都会认错图", say=say)
        say(f"出图名 : {saved.group(1)} ✓（与 dump_gbuf={gbuf} 一致）")

    for ln in errors[:5]:
        say(f"[smoke] ERROR 行: {ln.strip()}")
    if len(errors) > 5:
        say(f"[smoke] ...（其余 {len(errors) - 5} 条见日志）")
    grid = re.search(r"Swapchain extent: (\d+)x(\d+)", log)
    dims = cfg.dims  # PNG 真实尺寸：main 读好后传入（判据函数不碰磁盘）
    # 后缓冲核对（普通档）：回读链路与交换链对不上 ⟹ 这张图不能当判据。
    # dump_gbuf 档**跳过这一条**：它直读的是渲染网格上的 G-buffer RT，与后缓冲网格本来就
    # 可以不同（render_scale < 1 时必然不同），拿后缓冲判它 = 在合法档上误杀。改与渲染网格比，
    # 见下面 render_grid 处的核对。
    if gbuf is None and grid and dims and dims != (int(grid.group(1)), int(grid.group(2))):
        fail(f"截图尺寸 {dims[0]}x{dims[1]} ≠ 后缓冲 {grid.group(1)}x{grid.group(2)}"
             f"——回读链路与交换链对不上，这张图不能当判据", say=say)

    # render_scale 生效核对（批 14.3）。**必须核对而不是只打补丁**：patch_lua 只保证
    # "配置文件里改了"，不保证"引擎按它算了"（键名漂了 / 引擎读错表 / 被钳制）。一个
    # 静默跑在默认档上的 --render-scale 会把"0.5 档回归"做成"1.0 档回归"，而两者都绿。
    eff = re.search(r"render scale = (\S+) → render (\d+)x(\d+) \(swapchain (\d+)x(\d+)\)", log)
    if not eff:
        fail("日志里没有 `render scale = ... → render WxH (swapchain WxH)` 行"
             "——引擎的口径日志被删了或文案改了，批 14.3 的判据失去出处", say=say)
    if cfg.render_scale is not None:
        got = float(eff.group(1))
        if abs(got - cfg.render_scale) > 1e-6:
            fail(f"请求 --render-scale {cfg.render_scale} 引擎实际生效 {got}"
                 f"（合法区间 [0.25, 1.0]，越界会被钳制；非有限值回落 1.0）", say=say)
    render_grid = (int(eff.group(2)), int(eff.group(3)))
    # dump_gbuf 档的尺寸判据换基准（上面跳过后缓冲核对的对应另一半）：G-buffer RT 与深度
    # **同网格**（渲染网格），diffstruct 的 --gbuf 又要求它与 A/B 两图同尺寸，所以这里必须
    # 真的等于渲染网格——不等就是采集错档（比如 render_scale 不是 1 的那一档）。
    if gbuf is not None and dims and dims != render_grid:
        fail(f"gbuf 出图 {dims[0]}x{dims[1]} ≠ 渲染网格 {render_grid[0]}x{render_grid[1]}"
             "——dump_gbuf 直读的是渲染分辨率上的 G-buffer RT；要与差分对同尺寸请配 render_scale=1", say=say)
    # --size 在 dump_gbuf 档**不是不核**，而是换个对象核：那张图量不到"后缓冲是不是这个尺寸"
    # （它不经过后缓冲），但渲染网格必须等于请求尺寸，否则 --size 被静默忽略也会一路绿。
    if gbuf is not None and size and render_grid != size:
        fail(f"请求 --size {size[0]}x{size[1]}，dump_gbuf 档的渲染网格却是 "
             f"{render_grid[0]}x{render_grid[1]}——--size 没生效（llm_shot.size 被别处盖掉？）", say=say)
    # velocity_field 生效核对（批 14.4）。同 render_scale 的纪律：patch 只保证"配置里写了"，
    # 不保证"引擎按它建了 RT"。**反向同样要核**：默认档不该出现 velocity RT 行——出现了就说明
    # 门关不拢（图里多了一个 pass，而逐位不变红线单看截图未必抓得住）。
    # ── 「这扇门到底开没开」的统一口径 ──────────────────────────────────────────
    # 🔴 三条来路必须并成一条，否则下面的**反向**判据（门关不拢）会拿「门关」的假设去判
    # 一门其实开着的图，把正确行为报成缺陷：
    #   ① 具名旗标：--ssr / --velocity-field / --aa taa|fsr
    #   ② `--set [runtime:]KEY=VALUE` 这条**等价**来路（低频/一次性臂的唯一入口）
    #   ③ 引擎解析期的**隐含依赖**：aa=fsr 隐含 velocity_field（单一来源在 applyAaKey）
    # aa_eff 在 main 开头算（guard_flags 也要它），这里只派生其余三态。
    taaRequested = cfg.aa_eff == "taa"
    fsrRequested = cfg.aa_eff == "fsr"
    ssrRequested = cfg.ssr or set_switch_on(cfg.overrides, "runtime", "ssr.enabled")
    velRequested = (cfg.velocity_field or fsrRequested
                    or set_switch_on(cfg.overrides, "runtime", "velocity_field"))
    vel = re.search(r"velocity \(RGBA16F[^)]*\): (\d+)x(\d+) slot (\d+)", log)
    velUnsupported = "velocity: 图形态不支持" in log
    if velRequested and not vel:
        # 区分两种"没建 RT"：**图形态不支持**（装饰器 warn 后正常跳过，offscreen 变体用
        # blit 上屏，无"写 swapchain 且采色"的 pass = 与 decorateTaaPass 同判据）与
        # **静默没落图**（键名漂了 / 日志文案改了 / 装饰器漏调）。前者是正确行为，
        # 一律 FAIL 会把"正确不支持"报成缺陷，反过来也不许它拿"不支持"当遮羞布：
        # 必须真的看到那条 warn 才算数。
        if not velUnsupported:
            fail("请求 velocity_field（--velocity-field 或 --set velocity_field=true 或 aa=fsr "
                 "隐含）但日志里既没有 velocity RT 行、也没有"
                 "「velocity: 图形态不支持」告警 ⟹ 装饰器静默没落图"
                 "（配置键名漂了 / 启动日志文案改了 / 装饰调用被删）", say=say)
        say("速度场: 本图变体不支持（装饰器已 warn 并跳过）✓ 非错误")
    if not velRequested and vel:
        fail(f"默认档（velocity_field 关）却建了 velocity RT：{vel.group(0)}"
             " ⟹ 门关不拢，「图里没有这个 pass」的逐位不变红线失效", say=say)
    # ssr 生效核对（批 15.1 前半；批 15.3 子步 B 起 = **两 RT 两 pass**）。同 velocity 的
    # 双向纪律：正向 = 门开必须 **trace 与 apply 两行都在**（只缺一行 = 半条链，图能跑但
    # 结果错，单行判据抓不住）；反向 = 门关一行都不许有——本判据的本体仍是「门开 = 门关
    # 逐位相同」，它依赖「门关 = 图里根本没有这两个 pass」，门关不拢时截图恰好也可能相同，
    # 画面判据抓不住，只有日志能抓。尺寸判据分档：apply RT = 渲染网格（消费者全在渲染
    # 网格上工作）、trace RT = 渲染网格 × 0.5 向下取整（口径 = C++ scaleRtExtent）。
    ssrRt = re.search(r"ssr \(RGBA16F[^)]*\): (\d+)x(\d+) slot (\d+)", log)
    ssrTraceRt = re.search(r"ssr_trace \(RGBA16F[^)]*\): (\d+)x(\d+) slot (\d+)", log)
    ssrUnsupported = "ssr: 图形态不支持" in log
    if ssrRequested and not (ssrRt and ssrTraceRt):
        if not ssrUnsupported:
            fail("请求 ssr（--ssr 或 --set ssr.enabled=true）但日志里 trace/apply 两行 RT 不齐"
                 f"（trace={'有' if ssrTraceRt else '无'}, apply={'有' if ssrRt else '无'}），"
                 "也没有「ssr: 图形态不支持」告警 ⟹ 装饰器静默没落图或只落了一半"
                 "（配置键名漂了 / 启动日志文案改了 / 装饰调用被删）", say=say)
        say("SSR    : 本图变体不支持（装饰器已 warn 并跳过）✓ 非错误")
    if not ssrRequested and (ssrRt or ssrTraceRt):
        fail(f"默认档（ssr.enabled 关）却建了 ssr RT：{(ssrRt or ssrTraceRt).group(0)}"
             " ⟹ 门关不拢，「门开 = 门关逐位相同」红线的前提失效", say=say)
    if ssrRt and (int(ssrRt.group(1)), int(ssrRt.group(2))) != render_grid:
        fail(f"ssr(apply) RT {ssrRt.group(1)}x{ssrRt.group(2)} ≠ 渲染网格 "
             f"{render_grid[0]}x{render_grid[1]}——apply 的消费者（tonemap/taa/ao_apply）"
             "都在渲染网格上工作", say=say)
    traceGrid = (max(1, render_grid[0] // 2), max(1, render_grid[1] // 2))
    if ssrTraceRt and (int(ssrTraceRt.group(1)), int(ssrTraceRt.group(2))) != traceGrid:
        fail(f"ssr_trace RT {ssrTraceRt.group(1)}x{ssrTraceRt.group(2)} ≠ 半渲染网格 "
             f"{traceGrid[0]}x{traceGrid[1]}（floor，口径 = scaleRtExtent）"
             "——march 成本随像素数走，缩放口径错 = 成本账整个不对", say=say)
    if ssrRt and ssrTraceRt:
        say(f"SSR    : apply {ssrRt.group(1)}x{ssrRt.group(2)} = 渲染网格 ✓ / "
            f"trace {ssrTraceRt.group(1)}x{ssrTraceRt.group(2)} = 半网格 ✓")
    # aa=fsr 生效核对（批 14.5 的门 + 批 14.6 的槽位）。同 velocity 的纪律：双向都要核。
    # 正向 = 门开必须看到 vendor context 构造行；反向 = 门关**一行都不许有**——FSR 是
    # 图外 compute，门关不拢时截图未必变（本批它的输出尚无消费者），只看画面会漏。
    fsr = re.search(r"FsrUpscaler: context 就绪[^\n]*?render=(\d+)x(\d+) upscale=(\d+)x(\d+)", log)
    # 引擎自己的「开门但接不起来」降级（computeFsrUsable，main_scene）：图形态不支持
    # （offscreen 图无 scene_hdr/velocity/tonemap，或缺 shader16）时 warn **点名原因位**
    # 并正常不接 = 正确行为。与 velocity/SSR 的「图形态不支持」放行同判据：必须真看到
    # 那条 warn 才算数，不许拿"不支持"当遮羞布。（原 `fsr: 输入附件不齐` 分支是死码：
    # 引擎无此文案，且"输入不齐"现行设计就是这条降级 warn，不是失败。）
    fsrDegraded = re.search(r'aa="fsr" 开门但接不起来[^\n]*', log)
    if fsrRequested and not fsr:
        if fsrDegraded:
            say(f"FSR    : 本图变体接不起来，引擎已 warn 点名原因位 ✓ 非错误"
                f"（{fsrDegraded.group(0).split('] ')[-1].strip()}）")
        else:
            fail("请求 --aa fsr 但日志里没有「FsrUpscaler: context 就绪」行，"
                 "也没有「aa=\"fsr\" 开门但接不起来」降级告警"
                 " ⟹ 装饰/门链路静默没落图（配置键名漂了 / 生效判据被改 / 构造日志文案改了）", say=say)
    if not fsrRequested and fsr:
        fail(f"默认档（aa 不是 fsr）却构造了 FSR context：{fsr.group(0)}"
             " ⟹ 门关不拢，「零构造零命令」的逐位不变红线失效", say=say)
    if fsr:
        # FSR 的两个尺寸必须是**实际**渲染网格与后缓冲网格：vendor 的内部共享图按
        # maxRenderSize 定尺寸，填错 = dispatch 硬失败（FFX_ERROR_OUT_OF_RANGE）。
        if (int(fsr.group(1)), int(fsr.group(2))) != render_grid:
            fail(f"FSR render={fsr.group(1)}x{fsr.group(2)} ≠ 渲染网格 "
                 f"{render_grid[0]}x{render_grid[1]}", say=say)
        say(f"FSR: context 就绪 render={fsr.group(1)}x{fsr.group(2)} "
            f"upscale={fsr.group(3)}x{fsr.group(4)} ✓")
    # aa=taa 生效核对。同 velocity/FSR 的纪律：双向都要核。
    # 反向这条尤其值钱：**出厂默认是 aa="off"**（`configs/runtime.lua`），
    # 而红线 A（`fdece9d0…`）就是那张图。若哪天门关不拢、taa pass 悄悄进了图，红线 A 的
    # 含义就变了 —— 而单看截图未必发现（TAA 开也可能只改时域不改这一帧）。
    # taaRequested 的口径见上面「这扇门到底开没开」的统一口径（`--set aa="taa"` 也算开）。
    taaRt = re.search(r"taa \(RGBA16F\)[^\n]*out slot (\d+)", log)
    if taaRequested and not taaRt:
        fail("请求 --aa taa 但日志里没有「taa (RGBA16F): … out slot …」行"
             " ⟹ TAA pass 静默没进图（runtime.lua 键名漂了 / decorateTaaPass 的图形态判据"
             "变了 / 启动日志文案改了）", say=say)
    if not taaRequested and taaRt:
        fail(f"默认档（aa 不是 taa）却建了 taa RT：{taaRt.group(0)}"
             " ⟹ 门关不拢，红线 A（出厂默认图）的含义已失效", say=say)
    if taaRt:
        say(f"TAA  : {taaRt.group(0)} ✓"
            + (f"  clip_mode={cfg.clip}" if cfg.clip is not None else ""))
    # fsr 时域调参生效核对。**必须读「生效」而不是「请求」**：日志行报的是引擎实际接上的
    # 档位，而配置里写的可能是另一回事（"静默换臂"的引擎侧版本就藏在这个差里）。
    # 批 14.6 三态之后「TAA 在图 = true 且 temporal 请求开」已不可能同时成立（aa 一个槽位），
    # 所以这里判关只可能是：velocity 没进图 ⟹ fsrUsable 假 / 图形态不支持 / 本来就没请求。
    fsrTemp = re.search(r"fsr temporal: (开|关).*?fsr_temporal = (\w+)，TAA 在图 = (\w+)", log)
    # 🔴 两条判据的口径**故意不对称**，别"顺手统一"：
    #   正向只认具名旗标 `--fsr-temporal`（guard_flags 已保证它必须配 aa=fsr，请求了就必须看到）；
    #   反向认两条来路（`--set fsr.temporal=true` 也是合法开法）。反向若只看具名旗标，
    #   `--set aa=fsr --set fsr.temporal=true` 会被判成"门关不拢"——而门是开着的。
    fsrTempRequested = (cfg.fsr_temporal
                        or set_switch_on(cfg.overrides, "runtime", "fsr.temporal"))
    if cfg.fsr_temporal and not (fsrTemp and fsrTemp.group(1) == "开"):
        detail = (f"引擎判关（配置键 = {fsrTemp.group(2)}，TAA 在图 = {fsrTemp.group(3)}）"
                  if fsrTemp else "日志里没有「fsr temporal:」行（fsrUsable 为假 = FSR 根本没接进图）")
        fail(f"请求 --fsr-temporal 但 vendor 时域没开：{detail}"
             " ⟹ 这条读数会是「纯空间升频」却被记成时域档", say=say)
    if not fsrTempRequested and fsrTemp and fsrTemp.group(1) == "开":
        fail("默认档（fsr_temporal 关）却开着 vendor 时域 ⟹ 门关不拢，"
             "「FSR 关 = 逐位不变」的红线含义已失效。⚠ 若这一档是用 `--set aa=fsr` 进来的：\n"
             "     fsr 块出厂值是 temporal=true / jitter=true（= vendor 设计点），而工具的每个臂\n"
             "     必须是**自己写出来的那个配置**（不写键 = 静默换臂）——显式补\n"
             "     `--set fsr.temporal=false --set fsr.jitter=false` 才是 --aa fsr 的纯空间臂", say=say)
    # fsr_jitter 生效核对（正向 = 必须看到接入行并读出相位数；反向 = 门关一行不许有）。
    # 反向的口径与 fsr_temporal 同：`--set fsr.jitter=true` 也是一条合法开法。
    jit = re.search(r"fsr_jitter: 抖动已接入投影[^\n]*?相位 (\d+)", log)
    if cfg.fsr_jitter and not jit:
        fail("请求 --fsr-jitter 但日志里没有「fsr_jitter: 抖动已接入投影」行"
             " ⟹ 抖动静默没接（键名漂了 / 生效判据被改 / 日志文案改了）", say=say)
    if not (cfg.fsr_jitter or set_switch_on(cfg.overrides, "runtime", "fsr.jitter")) and jit:
        fail(f"默认档（fsr_jitter 关）却把抖动接进了投影：{jit.group(0)} ⟹ 门关不拢"
             "（`--set aa=fsr` 进来的话要显式 `--set fsr.jitter=false`，理由见上面 temporal 那条）", say=say)
    if fsrTemp or jit:
        bits = []
        if fsrTemp:
            bits.append("时域=vendor累积历史" if fsrTemp.group(1) == "开" else "时域=纯空间")
        if jit:
            bits.append(f"抖动相位 {jit.group(1)}")
        say(f"FSR档: {' · '.join(bits)} ✓")
    if vel:
        # 速度场必须与深度**同网格**：它的输出单位是"渲染分辨率像素"，网格不同则单位错。
        if (int(vel.group(1)), int(vel.group(2))) != render_grid:
            fail(f"velocity RT {vel.group(1)}x{vel.group(2)} ≠ 渲染网格 "
                 f"{render_grid[0]}x{render_grid[1]}——速度场必须与深度同网格", say=say)
        say(f"速度场: {vel.group(0)} = 渲染网格 ✓")
    if size and dims and gbuf is None and dims != size:
        fail(f"请求 --size {size[0]}x{size[1]} 实得 {dims[0]}x{dims[1]}"
             f"（headless 下 llm_shot.size 应当直接生效；启动行：{grid.group(0) if grid else '无 Swapchain extent 日志'}）", say=say)
    say(f"网格   : 后缓冲 {dims[0]}x{dims[1]}  渲染 {render_grid[0]}x{render_grid[1]}"
        f"  render_scale={eff.group(1)}"
        if dims else "网格   : 未取到（PNG 头异常？）")
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
                    help="runtime.lua ssr 块 enabled = true（屏幕空间反射，批 15.3B 起 = "
                         "trace + apply 两 pass）。双向核对：开了没建齐 ssr_trace/ssr 两行 RT "
                         "且无「图形态不支持」warn = FAIL；没开却建了 = FAIL（门关不拢 = "
                         "「门开=门关逐位相同」红线失效）")
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
                    metavar="[FILE:]KEY=VALUE",
                    help="通用配置注入（可重复）：对 staging 的配置文件走 patch_lua "
                         "（键命中 ≠ 1 即 FAIL，护栏与具名旗标同源）。例：`just shot --name "
                         "wetoff --set strength=0` = 湿面关消融档。作用在具名旗标**之后** = "
                         "显式注入覆盖具名档。带 `FILE:` 前缀换目标文件，FILE = 仓库 configs/ "
                         "里实际存在的 .lua（从磁盘派生，本 help 不抄名单；缺省 runtime；键名以 "
                         "configs/<文件>.lua 本体为准）。⚠ **字符串值必须自己带引号**："
                         "`--set shadow:mode=pcss` 写进 lua 是 `mode = pcss` = nil = 回落默认，"
                         "patch_lua 现在直接拒收这种裸值。例：`--set camera:mode='\"trackball\"'` "
                         "换机位模式、`--set diagnostics:dump_gbuf=3` 出 G-buffer 直读图"
                         "（后者引擎会忽略 --name，出图名固定 screenshots/gbuffer_3.png，"
                         "本工具按该名字核对）")
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

    # aa 的**实际**值：`--set aa=…` 作用在具名旗标之后 = 后写者赢（与 patch_lua 顺序同口径）。
    # 守卫与下面所有 aa 相关判据都用它，否则 `--set aa=fsr --fsr-temporal` 会被拒、
    # `--set aa=taa` 会被自己的反向判据判成"门关不拢"（详见 set_switch_on 的注释）。
    aaEff = set_value(args.overrides, "runtime", "aa") or args.aa
    guard_flags(args.clip, args.aa, args.fsr_temporal, args.fsr_jitter, aa_eff=aaEff)

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
    # dump_gbuf 注入档：引擎忽略 llm_shot.file、出图名写死 screenshots/gbuffer_N.png
    # ⟹ "请求的名字"与"该出现在磁盘上的名字"从这里分叉（判 PASS 与末尾的丢弃都按后者）。
    gbuf = gbuf_channel(args.overrides)
    expect_file = f"screenshots/gbuffer_{gbuf}.png" if gbuf is not None else shot_file
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
    if gbuf is not None:
        print(f"出图   : dump_gbuf={gbuf} 注入 ⟹ 引擎忽略 llm_shot.file，按 {expect_file} 核对")

    code, log, secs = run_engine(exe, args.timeout, args.sync)
    LOG_PATH.write_text(log, encoding="utf-8")
    shot_path = BUILD_RGL / expect_file
    # 判据段整体住进 verdicts()（债-130①），它自己不碰磁盘 ⟹ 图在不在、PNG 多大由这里读好传进
    # 去。🔴 **顺序是隐式契约，别把这两行挪进 verdicts 之后再算**：
    #   · 为什么必须先读：verdicts 里「没拿到截图」那条判据要在缺图时干净红掉，而 `png_size`
    #     对不存在的文件会抛 FileNotFoundError。`is_file()` 先判 = 缺图走判据、不走异常。
    #   · 残余差异窗口（重构前 png_size 在 code!=0 判据**之后**才跑）：只剩「引擎非零退出 且
    #     截图文件存在但 open() 抛 OSError（权限/IO 错）」这一种组合——原来是干净 FAIL，现在会
    #     抛 traceback。它不出现在任何正常路径上（文件在且可读才可能进 open），不值得为它把
    #     判据函数重新绑上磁盘；真要收，就把这两行做成 verdicts 的惰性入参（Callable）。
    exists = shot_path.is_file()
    errors, warnings = verdicts(code, log, VerdictCfg(
        expect_file=expect_file, shot_exists=exists,
        dims=png_size(shot_path) if exists else None, size=size,
        render_scale=args.render_scale, velocity_field=args.velocity_field,
        ssr=args.ssr, fsr_temporal=args.fsr_temporal, fsr_jitter=args.fsr_jitter,
        clip=args.clip, sync=args.sync, aa_eff=aaEff, overrides=args.overrides,
        gbuf=gbuf))
    kb = shot_path.stat().st_size // 1024
    if args.mode == "smoke" and gbuf is None:
        print(f"截图   : {kb} KB（冒烟探针，随 staging 丢弃）")
        shot_path.unlink(missing_ok=True)
    elif args.mode == "smoke":
        # dump_gbuf 档的产物**不在 staging 里**（引擎写死 screenshots/gbuffer_N.png），
        # 照探针那句"随 staging 丢弃"去删，删掉的正是用户这次跑要的那张图。
        print(f"截图   : {shot_path} ({kb} KB) — dump_gbuf 产物，不随 staging 丢弃")
    else:
        print(f"截图   : {shot_path} ({kb} KB)")
    print(f"日志   : {LOG_PATH.relative_to(ROOT)}")
    print(f"结果   : {'FAIL' if errors else 'PASS'}  errors={len(errors)}"
          f"  warnings={len(warnings)}  用时 {secs:.1f}s")
    return 1 if errors else 0


def selftest(quiet: bool = False) -> list[str]:
    """量具自检（无引擎、毫秒级；由 `regress selftest` 调用）。两半，各钉一类静默失效：

    **① `patch_lua` 文本补丁器**——单行子表那个 bug 的失效形态是**静默改坏配置**：护栏全绿、
    smoke 报 PASS errors=0，而量的是默认档（`--set shadow:izb.on=true` 曾把同块的 `grid`/
    `dump_frame` 一起吃掉）。断言全部打在**真配置的临时副本**上 ⟹ 配置格式变了（比如 izb 块
    改成多行）也会在这里红，而不是在某次判据里静默失效。

    **② 判据段 `verdicts`（债-130①）**——"看起来在工作其实没有"这类错靠下一次人肉跑发现不了，
    而判据唯一的资产就是「它还能不能看见东西」。每条判据要**两个方向**都有用例：判据被改坏
    （失效）要红，判据被**放宽**（正向核对的字面量被削掉 ⟹ 在错误日志上也打 ✓）也要红。
    后者只有"近似但不等于"的行能抓，而它恰恰是假绿——2026-10-16 实测：42 条"改成不可能匹配"
    的变异全被抓，21 条"放宽"的变异首轮只抓到 5 条。

    🔴 两半都不许加"要引擎"的用例：selftest 被 regress 在**每次判定前**调用，慢一秒 = 每次
    回归慢一秒。
    """
    import tempfile

    done: list[str] = []

    def ok(cond: bool, what: str) -> None:
        done.append(what)
        if not cond:
            raise RuntimeError(f"smoke 自检失败：{what}")

    def patched(src: Path, *pairs: tuple[str, str]) -> str:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / src.name
            cp.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
            for key, value in pairs:
                patch_lua(cp, key, value)
            return cp.read_text(encoding="utf-8")

    def line(text: str, needle: str) -> str:
        return next((l.strip() for l in text.splitlines() if needle in l), "")

    def exits(src: Path, key: str, value: str) -> bool:
        try:
            patched(src, (key, value))
            return False
        except SystemExit:
            return True

    shadow = ROOT / "configs/shadow.lua"
    runtime = ROOT / "configs/runtime.lua"
    single = line(shadow.read_text(encoding="utf-8"), "izb = {")
    ok("izb = {" in single and "grid" in single,
       f"configs/shadow.lua 的 izb 仍是单行子表（形态变了请同步改自检）：{single}")

    # ① 阳性锚点（那个 bug 本体）：改单行块的**首键**不许吃掉兄弟键
    one = patched(shadow, ("izb.on", "true"))
    ok("on = true" in one and "grid = 512" in one and "dump_frame = 0" in one,
       f"单行子表改首键后兄弟键必须还在：{line(one, 'izb = {')}")
    ok(one.count("}") >= single.count("}"), "单行子表的右花括号不许被吃掉")

    # ② 单行块的**非首键**要能命中（旧实现直接红在"命中 0 次"）
    two = patched(shadow, ("izb.dump_frame", "7"))
    ok("dump_frame = 7" in two, f"单行子表末键可改：{line(two, 'izb = {')}")
    both = patched(shadow, ("izb.on", "true"), ("izb.grid", "1024"))
    ok("on = true" in both and "grid = 1024" in both,
       f"同块两键连改互不破坏：{line(both, 'izb = {')}")

    # ③ 多行块与顶层键的旧路径逐字不变
    ok("enable = true" in patched(runtime, ("meshlet.enable", "true")), "多行块子键仍可改")
    ok("{ 800, 600 }" in patched(runtime, ("headless_size", "{ 800, 600 }")), "顶层键仍可改")

    # ④ 两条护栏不能被上面的改动顺手关掉
    ok(exits(shadow, "mode", "pcss"), "漏引号（原值是字符串而给的是裸值）必须红")
    ok(exits(shadow, "izb.nope", "1"), "拼错键必须红")

    # ── 判据段（verdicts）的合成现场 —— 债-130① ──────────────────────────────
    # 为什么这一组必须存在：上面那些用例只保证"配置里写了"，判据段才回答"引擎到底有没有
    # 按配置跑"。它长在 main() 里时 selftest 对它**零条断言**，26 条 fail() 会不会红只能
    # 靠读代码推理。判据唯一的资产是「它还能不能看见东西」，所以这里的写法是：每条判据造
    # 一份**只坏在它身上**的合成日志，断言"红"且"红在它那一条"（needle）。
    # 骨架那 7 行的形态取自引擎的日志格式串（main_impl.cppm / utils.cppm / vkswapchain.cppm
    # / main_scene.cppm / render_graph_rtslot.cppm / fsr_upscaler_impl.cppm /
    # ecs_frame_diag.cppm），不是凭记忆编的——编出来的形态会验出一条根本不存在的判据。
    vcase = 0

    def vok(cond: bool, detail: str) -> None:
        """判据段用例的断言。红/不红要精确到判据，但**不逐条挂号**进 done：regress 会把
        done 逐行打印，四十行合成日志用例会把真正的读数淹掉。"""
        nonlocal vcase
        vcase += 1
        if not cond:
            raise RuntimeError(f"smoke 自检失败：{detail}")

    def vlog(*extra: str, drop: str = "") -> str:
        """「全好」启动日志骨架；extra 追加行，drop 按子串删行（造"引擎没打这行"的现场）。"""
        base = [
            "[0] [info] [main_impl.cppm:169] headless 模式：脱离 SDL/compositor（1280x720）",
            "[0] [info] [utils.cppm:333] Selected rendering GPU: Test GPU (score=1)",
            "[0] [info] [utils.cppm:284] Mesh/task device caps: maxMeshWorkGroupInvocations=256",
            "[0] [info] [vkswapchain.cppm:464] Swapchain extent: 1280x720 (headless, size config)",
            "[0] [info] [main_impl.cppm:277] render scale = 1 → render 1280x720 (swapchain 1280x720)",
            "[0] [info] [main_scene.cppm:828] DGC path: device='Test GPU' graphicsIndirect=false "
            "computeIndirect=true static=true -> 读回重放 (CPU 回退) [cull_replay=cpu]",
            "[0] [info] [ecs_frame_diag.cppm:89] llm_debug: 截图已保存 (/abs/.smoke/probe.png) "
            "— render 1280x720 / swapchain 1280x720 (scale 1)",
        ]
        if drop:
            base = [ln for ln in base if drop not in ln]
        return "\n".join([*base, *extra])

    def vcfg(**kw) -> VerdictCfg:
        """合成 cfg：默认 = 「默认档 + 全好」（图在盘上、1280x720、aa/ssr/velocity 全关），
        用例只写它要改的那一两个字段。"""
        base = dict(expect_file=".smoke/probe.png", shot_exists=True, dims=(1280, 720))
        base.update(kw)
        return VerdictCfg(**base)

    def shot_line(name: str) -> str:
        """引擎的出图行（dump_gbuf 档引擎忽略 llm_shot.file、名字写死 gbuffer_N.png）。"""
        return ("[0] [info] [ecs_frame_diag.cppm:89] llm_debug: 截图已保存 (/abs/screenshots/"
                + name + ") — render 1280x720 / swapchain 1280x720 (scale 1)")

    # 各档的"生效"行（判据正则吃的就是这些形态）
    VEL = "[0] [info] [render_graph_rtslot.cppm:148] velocity (RGBA16F, 像素单位 prev−cur): 1280x720 slot 5"
    VEL_SMALL = VEL.replace("1280x720", "640x360")
    VEL_NO = "[0] [warning] [main_impl.cppm:404] velocity: 图形态不支持速度场装饰（锚点略）"
    SSR = "[0] [info] [render_graph_rtslot.cppm:195] ssr (RGBA16F, 成片+屏幕空间反射): 1280x720 slot 6"
    SSR_SMALL = SSR.replace("1280x720", "640x360")
    SSR_TRACE = ("[0] [info] [render_graph_rtslot.cppm:221] ssr_trace (RGBA16F, premultiplied "
                 "反射贡献, ×0.5 渲染网格): 640x360 slot 7")
    SSR_TRACE_FULL = SSR_TRACE.replace("640x360", "1280x720")
    SSR_NO = "[0] [warning] [main_impl.cppm:463] ssr: 图形态不支持 SSR 装饰（锚点略）"
    FSR = "[0] [info] [fsr_upscaler_impl.cppm:309] FsrUpscaler: context 就绪（输出槽 8）render=1280x720 upscale=1280x720 vram=12 MiB"
    FSR_SMALL = FSR.replace("render=1280x720", "render=640x360")
    FSR_DEGRADED = ('[0] [warning] [main_scene.cppm:646] aa="fsr" 开门但接不起来'
                    '（velocityDecorated=0 shader16=0 输入齐备=1) ⟹ 不接 FSR，'
                    'tonemap 色输入保持 scene_hdr')
    TAA = "[0] [info] [render_graph_rtslot.cppm:123] taa (RGBA16F): 1280x720 out slot 3 / hist slot 4"
    TEMP_ON = "[0] [info] [main_scene.cppm:687] fsr temporal: 开（配置键 fsr_temporal = true，TAA 在图 = false）"
    TEMP_OFF = TEMP_ON.replace("temporal: 开", "temporal: 关")
    JIT = "[0] [info] [main_scene.cppm:139] fsr_jitter: 抖动已接入投影并申报给 FSR（相位 14 = 渲染 1280 / 上屏 720）"
    NO_VVL = "[0] [warning] [diagnostics_impl.cppm:120] diagnostics: no_vvl = true"
    HAZARD = "[0] [error] [utils.cppm:210] Sync validation: WRITE_AFTER_WRITE hazard detected"
    VVL_ERR = "[0] [error] [utils.cppm:210] VUID-VkCmdPipelineBarrier 01133 违规"
    # 「近似但不等于」的近邻行（⑭ 组用；全部取自引擎真实格式串，行号 = 出处）
    SHOT_FAIL = "[0] [error] [ecs_frame_diag.cppm:93] llm_debug: 截图失败 (回读超时)"
    DIAG_OTHER = "[0] [warning] [diagnostics_impl.cppm:62] diagnostics: rpc_port=0 越界（1..65535），回落 8788"
    HAZARD_EDGE = "[0] [info] [shader_impl.cppm:405] compute hazard: a ↔ b（heap 槽 3）由边 'x' 覆盖"
    SCALE_PARTIAL = "[0] [info] [main_impl.cppm:277] render scale = 1 → render 1280x720"
    VEL_OTHER_FMT = VEL.replace("RGBA16F", "R32F")
    SSR_TRACE_OTHER_FMT = SSR_TRACE.replace("RGBA16F", "R32F")
    FSR_DISPATCH_FAIL = "[0] [error] [fsr_upscaler_impl.cppm:402] FsrUpscaler: dispatch 失败 (7) —— render=1280x720 upscale=1280x720"
    FSR_AA_ECHO = '[0] [info] [main_scene.cppm:640] aa="fsr" 已按请求接入（文案改了的那一行）'
    TAA_HIST_ONLY = "[0] [info] [render_graph_rtslot.cppm:123] taa (RGBA16F): 1280x720 hist slot 4"
    JIT_NOT_WIRED = "[0] [info] [main_scene.cppm:139] fsr_jitter: 抖动未接入投影（相位 0 = 关）"
    VEL_NO_SLOT = VEL.replace(" slot 5", "")
    TAA_OTHER_FMT = TAA.replace("RGBA16F", "RGBA32F")
    #     taa 判据的**名字锚**：`\(RGBA16F\)[^\n]*out slot` 在删掉 `taa ` 之后会去认任何一张
    #     RGBA16F 且带 out slot 的行。velocity / ssr / ssr_trace 三张 RT 行今天都不写 "out slot"，
    #     所以这条在纯引擎输出上是**等价**的——它钉的是「比较对象必须锚在 taa 那一行」，不是某条
    #     现存文案（同 `surface extent:` 那条占位的口径）。
    TAA_OTHER_NAME = TAA.replace("] taa (", "] scene_hdr (")
    SSR_OTHER_FMT = SSR.replace("RGBA16F", "R32F")
    SSR_TRACE_NO_SLOT = SSR_TRACE.replace(" slot 7", "")
    SHOT_GBUF_TAILORED = ("[0] [info] [ecs_frame_diag.cppm:89] llm_debug: 截图已保存 "
                          "(/abs/screenshots/gbuffer_3.png — 1280x720) — render 1280x720")
    GATE_NO_VVL = "[0] [warning] [gate_lint_impl.cppm:354] gate_lint[no_vvl]: 本门 inert（验证层被关）"
    DEGRADE_OTHER = "[0] [warning] [main_impl.cppm:463] ssr: 图形态接不起来（forward 图，文案改了）"

    def vrun(code: int, log: str, cfg: VerdictCfg) -> tuple[str, list[str], list[str]]:
        """跑一遍判据（读数全接进内存），返回 (FAIL 消息, error 行, warning 行)。

        没红 = FAIL 消息为空串。判据段自己崩了（正则没命中还去 `.group()`）也算红，但报成
        "抛异常"而不是甩裸 traceback——自检的红要说清红在哪。
        """
        out: list[str] = []
        errors: list[str] = []
        warnings: list[str] = []
        try:
            errors, warnings = verdicts(code, log, cfg, say=out.append)
        except SystemExit:
            pass
        except Exception as err:  # noqa: BLE001（判据崩了要报成用例红，不是 traceback）
            out.append(f"[smoke] FAIL: 判据段抛 {type(err).__name__}: {err}")
        msg = next((ln[len("[smoke] FAIL: "):] for ln in out
                    if ln.startswith("[smoke] FAIL: ")), "")
        return msg, errors, warnings

    def vsay(code: int, log: str, cfg: VerdictCfg) -> list[str]:
        """跑判据并返回它**打出来的读数行**——读数也会被当权威读，计数口径同样要钉。"""
        out: list[str] = []
        try:
            verdicts(code, log, cfg, say=out.append)
        except SystemExit:
            pass
        return out

    def must_fail(what: str, needle: str, code: int, log: str, cfg: VerdictCfg) -> None:
        """断言这份合成数据**会红**，且红在 needle 点名的那条判据上。

        🔴 needle 不可省：首条失败即退出，某条判据被改坏后完全可能是**别的**判据先红，
        只断言"红了"就是假绿。
        """
        msg, _, _ = vrun(code, log, cfg)
        vok(needle in msg, f"{what}：应红在 {needle!r}，实得 {msg[:80]!r}")

    def must_pass(what: str, code: int, log: str, cfg: VerdictCfg) -> None:
        """断言这份合成数据**不该红**（阳性锚点 / 各条"正确放行"的档）。"""
        msg, _, _ = vrun(code, log, cfg)
        vok(not msg, f"{what}：不该红，却红在 {msg[:80]!r}")

    # ① 阳性锚点：先证明"全好"不红。没有它，下面全部"会红"的用例都是廉价满足的
    #    （判据被改成恒真时，只有这条会红）。
    msg, errs, warns = vrun(0, vlog(), vcfg())
    vok(not msg and not errs and not warns,
        f"阳性锚点不成立：全好的默认档日志被判红（FAIL={msg[:60]!r} errors={errs} warnings={warns}）")

    # ② 引擎非零退出。拆函数之前这条只是**推理**（fail 的位置早于 fsr 那段 ⟹ 引擎拒启动时
    #    工具必红），推理不是防线，测过才是。
    must_fail("引擎非零退出必须红", "引擎非零退出", 1, vlog(), vcfg())
    #    顺序契约：非零退出要**先于**"没拿到截图"报出（引擎崩了的时候缺图是果不是因）
    must_fail("非零退出优先于缺图", "引擎非零退出", 1, vlog(drop="截图已保存"), vcfg())

    # ③ errors 通道（不走 fail() 的最后一条判据：main 据 errors 非空打 FAIL 并返回 1）
    msg, errs, _ = vrun(0, vlog(drop="headless 模式"), vcfg())
    vok(not msg and any("headless" in e for e in errs),
        "缺 headless 行必须计入 errors——那是「本次没跑在 headless 上」的唯一判据")
    msg, errs, _ = vrun(0, vlog(VVL_ERR), vcfg())
    vok(not msg and len(errs) == 1, f"[error] 级日志行必须计入 errors（实得 {errs}）")
    must_pass("--sync 档 hazard 不 fail，只计入 errors", 0, vlog(HAZARD), vcfg(sync=True))

    # ④ 同步验证档的**生效**核对：no_vvl = true 时 errors=0 是假的干净
    must_fail("--sync 撞 no_vvl 必须红", "验证层没启用", 0, vlog(NO_VVL), vcfg(sync=True))
    must_pass("no_vvl 只在 --sync 档算红（默认档不核）", 0, vlog(NO_VVL), vcfg())

    # ⑤ 出图：日志说存了 + 磁盘上真有，两者缺一都算没出图
    must_fail("日志缺「截图已保存」行必须红", "没拿到截图", 0, vlog(drop="截图已保存"), vcfg())
    must_fail("磁盘上没有那张图必须红", "没拿到截图", 0, vlog(), vcfg(shot_exists=False))
    must_fail("截图尺寸 ≠ 后缓冲必须红", "回读链路与交换链对不上", 0, vlog(), vcfg(dims=(800, 600)))
    must_fail("--size 请求了实得却是别的", "实得 1280x720", 0, vlog(), vcfg(size=(800, 600)))

    # ⑥ dump_gbuf 注入档：出图名与尺寸基准都换（引擎忽略 llm_shot.file）
    must_pass("gbuf 档出图名对上就不红", 0, vlog(shot_line("gbuffer_3.png"), drop="截图已保存"),
              vcfg(gbuf=3, expect_file="screenshots/gbuffer_3.png"))
    must_fail("gbuf 档引擎报的出图名不是推出的那个", "出图路径不是 gbuffer_3.png",
              0, vlog(), vcfg(gbuf=3, expect_file="screenshots/gbuffer_3.png"))
    #    gbuf 档的尺寸判据基准是**渲染网格**，拿后缓冲判它 = 在合法档上误杀（needle 换掉
    #    就证明它红在 gbuf 那条而不是后缓冲那条 = 两条判据的分档确实生效）
    must_fail("gbuf 出图 ≠ 渲染网格必须红", "dump_gbuf 直读的是渲染分辨率",
              0, vlog(shot_line("gbuffer_3.png"), drop="截图已保存"),
              vcfg(gbuf=3, expect_file="screenshots/gbuffer_3.png", dims=(640, 360)))
    must_fail("gbuf 档 --size 没落到渲染网格必须红", "--size 没生效",
              0, vlog(shot_line("gbuffer_1.png"), drop="截图已保存"),
              vcfg(gbuf=1, expect_file="screenshots/gbuffer_1.png", size=(800, 600)))

    # ⑦ render_scale：请求 ≠ 生效（patch 只保证"配置里写了"）
    must_fail("缺 render scale 口径行必须红", "批 14.3 的判据失去出处",
              0, vlog(drop="render scale ="), vcfg())
    must_fail("请求 0.5 生效 1.0 必须红", "引擎实际生效 1.0", 0, vlog(), vcfg(render_scale=0.5))
    must_pass("请求 1.0 生效 1.0 不红（上一条的红不是恒真）", 0, vlog(), vcfg(render_scale=1.0))

    # ⑧ velocity_field：双向核对（门开必须有行、门关一行不许有）
    must_fail("请求 velocity 但没建 RT 也没 warn", "请求 velocity_field",
              0, vlog(), vcfg(velocity_field=True))
    must_pass("图形态不支持 = 正确行为，放行", 0, vlog(VEL_NO), vcfg(velocity_field=True))
    must_fail("门关不拢：默认档却建了 velocity RT", "却建了 velocity RT", 0, vlog(VEL), vcfg())
    must_fail("velocity RT ≠ 渲染网格必须红", "速度场必须与深度同网格",
              0, vlog(VEL_SMALL), vcfg(velocity_field=True))
    #    `--set velocity_field=true` 是与具名旗标**等价**的来路：不认它，反向判据就会拿
    #    「门关」的假设去判一门开着的图（批 N+1 实测四条全中招）
    must_pass("--set 开 velocity 也算门开", 0, vlog(VEL), vcfg(overrides=["velocity_field=true"]))

    # ⑨ ssr：门开必须 trace + apply **两行都在**（只缺一行 = 半条链，单行判据抓不住）
    must_fail("请求 ssr 只建了一半必须红", "请求 ssr（", 0, vlog(SSR), vcfg(ssr=True))
    must_pass("ssr 图形态不支持 = 正确行为，放行", 0, vlog(SSR_NO), vcfg(ssr=True))
    must_fail("门关不拢：默认档却建了 ssr RT", "却建了 ssr RT", 0, vlog(SSR), vcfg())
    must_fail("ssr(apply) ≠ 渲染网格必须红", "apply 的消费者",
              0, vlog(SSR_SMALL, SSR_TRACE), vcfg(ssr=True))
    must_fail("ssr_trace ≠ 半渲染网格必须红", "半渲染网格",
              0, vlog(SSR, SSR_TRACE_FULL), vcfg(ssr=True))
    must_pass("ssr 两行齐且尺寸对就不红", 0, vlog(SSR, SSR_TRACE), vcfg(ssr=True))
    must_pass("--set 开 ssr.enabled 也算门开", 0, vlog(SSR, SSR_TRACE),
              vcfg(overrides=["ssr.enabled=true"]))

    # ⑩ aa=fsr：正向看 context 行，降级要真看到 warn 才算数
    must_fail("请求 --aa fsr 既无 context 行也无降级 warn", "请求 --aa fsr 但日志里没有",
              0, vlog(VEL), vcfg(aa_eff="fsr"))
    must_pass("引擎「开门但接不起来」降级 = 正确行为，放行", 0, vlog(VEL, FSR_DEGRADED),
              vcfg(aa_eff="fsr"))
    must_fail("门关不拢：默认档却构造了 FSR context", "却构造了 FSR context", 0, vlog(FSR), vcfg())
    must_fail("FSR render ≠ 渲染网格必须红", "FSR render=640x360",
              0, vlog(VEL, FSR_SMALL), vcfg(aa_eff="fsr"))
    must_pass("aa=fsr 纯空间臂全好就不红", 0, vlog(VEL, FSR), vcfg(aa_eff="fsr"))
    must_pass("aa=fsr 时域+抖动臂全好就不红", 0, vlog(VEL, FSR, TEMP_ON, JIT),
              vcfg(aa_eff="fsr", fsr_temporal=True, fsr_jitter=True))

    # ⑪ aa=taa：出厂默认是 aa="off"，门关不拢时红线 A 的含义就变了
    must_fail("请求 --aa taa 但没建 taa RT", "TAA pass 静默没进图", 0, vlog(), vcfg(aa_eff="taa"))
    must_fail("门关不拢：默认档却建了 taa RT", "却建了 taa RT", 0, vlog(TAA), vcfg())
    must_pass("aa=taa + clip 建了就不红", 0, vlog(TAA), vcfg(aa_eff="taa", clip=13))
    must_pass('--set aa="taa" 也算门开', 0, vlog(TAA), vcfg(aa_eff="taa", overrides=['aa="taa"']))

    # ⑫ fsr 时域/抖动调参：读**生效**而不是读请求；两条判据的口径故意不对称
    must_fail("请求 --fsr-temporal 但引擎判关", "vendor 时域没开",
              0, vlog(VEL, FSR, TEMP_OFF), vcfg(aa_eff="fsr", fsr_temporal=True))
    must_fail("门关不拢：默认档却开着 vendor 时域", "却开着 vendor 时域",
              0, vlog(TEMP_ON), vcfg())
    must_pass("--set fsr.temporal=true 是合法开法（反向认两条来路）", 0,
              vlog(VEL, FSR, TEMP_ON),
              vcfg(aa_eff="fsr", overrides=['aa="fsr"', "fsr.temporal=true"]))
    must_fail("请求 --fsr-jitter 但抖动没接进投影", "抖动静默没接",
              0, vlog(VEL, FSR, TEMP_ON), vcfg(aa_eff="fsr", fsr_temporal=True, fsr_jitter=True))
    must_fail("门关不拢：默认档却把抖动接进投影", "却把抖动接进了投影", 0, vlog(JIT), vcfg())
    must_pass("--set fsr.jitter=true 是合法开法（反向认两条来路）", 0, vlog(VEL, FSR, JIT),
              vcfg(aa_eff="fsr", overrides=['aa="fsr"', "fsr.jitter=true"]))

    # ⑬ 旗标守卫（不在判据段里，但同为 fail() 判据；拆出来之前同样零条断言）
    def guard_msg(**kw) -> str:
        out: list[str] = []
        try:
            guard_flags(kw.get("clip"), kw.get("aa"), kw.get("temporal", False),
                        kw.get("jitter", False), aa_eff=kw.get("aa_eff"), say=out.append)
        except SystemExit:
            pass
        return next((ln[len("[smoke] FAIL: "):] for ln in out
                     if ln.startswith("[smoke] FAIL: ")), "")

    def guard_fail(what: str, needle: str, **kw) -> None:
        msg = guard_msg(**kw)
        vok(needle in msg, f"{what}：应红在 {needle!r}，实得 {msg[:80]!r}")

    def guard_ok(what: str, **kw) -> None:
        msg = guard_msg(**kw)
        vok(not msg, f"{what}：不该红，却红在 {msg[:80]!r}")

    guard_fail("--clip 不配 --aa taa 必须红", "--clip 必须配 --aa taa", clip=13)
    guard_fail("--fsr-temporal 不配 --aa fsr 必须红", "是 aa=fsr 的子选项", temporal=True)
    guard_fail("--fsr-jitter 不配 --fsr-temporal 必须红", "--fsr-jitter 必须配 --fsr-temporal",
               aa="fsr", jitter=True)
    guard_ok("--clip 配 aa=taa 放行", clip=13, aa="taa")
    guard_ok('aa_eff 来自 --set 时 --fsr-temporal 合法（护栏不挡等价来路）',
             temporal=True, aa=None, aa_eff="fsr")

    # ⑭ 放宽类：正则**变松** = 正向判据在错误的日志上也放行（假绿）。
    #     ⑤–⑫ 的变异方向全是"改成不可能匹配"，那验的是判据**会不会失效**；这一组验反方向：
    #     判据的字面量区分度本身还在不在。近邻行全部取自引擎真实格式串（文件:行号见各行），
    #     不是编出来凑数的形态——编出来的形态只会验出一条根本不存在的判据。
    #     每条 must_fail 的"真形态放行档"就在 ⑤–⑫ 对应组里（证明新用例不是恒真）。
    must_fail("「截图失败」行不能当成出图成功（磁盘上还留着上一次的图）", "没拿到截图",
              0, vlog(SHOT_FAIL, drop="截图已保存"), vcfg())
    must_pass("--sync 档下非 no_vvl 的 diagnostics 行不该红", 0, vlog(DIAG_OTHER), vcfg(sync=True))
    must_fail("口径行缺 (swapchain WxH) 尾段 = 文案改了，判据失去出处", "批 14.3 的判据失去出处",
              0, vlog(SCALE_PARTIAL, drop="render scale ="), vcfg())
    must_pass("--size 请求且实得一致就不红（丢掉不等号必须红在这里）", 0, vlog(), vcfg(size=(1280, 720)))
    must_pass("gbuf 档 --size 落到渲染网格就不红（丢掉不等号必须红在这里）", 0,
              vlog(shot_line("gbuffer_1.png"), drop="截图已保存"),
              vcfg(gbuf=1, expect_file="screenshots/gbuffer_1.png", size=(1280, 720)))
    must_fail("velocity 行不是 RGBA16F 格式 = 不是那张 RT", "请求 velocity_field",
              0, vlog(VEL_OTHER_FMT), vcfg(velocity_field=True))
    #     三条"图形态不支持"逃生门各自只认自己那一条 warn：不认前缀的话，ssr 的 warn
    #     会替 velocity 开门（反之同理）——逃生门变成通用遮羞布
    must_fail("velocity 请求了、日志里只有 ssr 的不支持 warn", "请求 velocity_field",
              0, vlog(SSR_NO), vcfg(velocity_field=True))
    must_fail("ssr 请求了、日志里只有 velocity 的不支持 warn", "请求 ssr（",
              0, vlog(VEL_NO), vcfg(ssr=True))
    must_fail("ssr 只建了 trace 一行（apply 缺）必须红", "请求 ssr（", 0, vlog(SSR_TRACE), vcfg(ssr=True))
    must_fail("ssr_trace 行不是 RGBA16F 格式 = 不是那张 RT", "请求 ssr（",
              0, vlog(SSR, SSR_TRACE_OTHER_FMT), vcfg(ssr=True))
    #     ★ FSR：引擎真有一行「FsrUpscaler: dispatch 失败 (…) —— render=WxH upscale=WxH」，
    #     它同时带 render= 与 upscale= 两个数字 ⟹ 正则只要松掉「context 就绪」四个字，
    #     dispatch 硬失败的跑会打出「FSR: context 就绪 … ✓」= 本条债立项的那类假绿
    must_fail("FsrUpscaler dispatch 失败行不能当成 context 就绪", "请求 --aa fsr 但日志里没有",
              0, vlog(VEL, FSR_DISPATCH_FAIL), vcfg(aa_eff="fsr"))
    must_fail('提到 aa="fsr" 但没点名降级的行不许开逃生门', "请求 --aa fsr 但日志里没有",
              0, vlog(VEL, FSR_AA_ECHO), vcfg(aa_eff="fsr"))
    must_fail("taa 行只有 hist slot（没有 out slot）必须红", "TAA pass 静默没进图",
              0, vlog(TAA_HIST_ONLY), vcfg(aa_eff="taa"))
    must_fail("fsr_jitter 行说未接入（仍带相位数字）必须红", "抖动静默没接",
              0, vlog(VEL, FSR, TEMP_ON, JIT_NOT_WIRED),
              vcfg(aa_eff="fsr", fsr_temporal=True, fsr_jitter=True))
    #     后缓冲核对必须**锚在交换链那一行**：run_engine 把 stdout+stderr 合并进 log，驱动与
    #     VVL 的行也在里面，`extent:` 放宽掉 Swapchain 前缀就会让比较对象换人（下面这行是
    #     外部输出形态的占位——引擎自己不发它，钉的是锚定，不是某条引擎文案）
    must_pass("后缓冲核对只认 Swapchain 那一行（别的 extent: 行不得顶替）", 0,
              "[0] [info] [driver] surface extent: 640x360 (当前模式)\n" + vlog(), vcfg())
    must_fail("velocity 行没申报 slot = 不是那张 RT 行", "请求 velocity_field",
              0, vlog(VEL_NO_SLOT), vcfg(velocity_field=True))
    must_fail("taa 行不是 RGBA16F 格式 = 不是那张 RT", "TAA pass 静默没进图",
              0, vlog(TAA_OTHER_FMT), vcfg(aa_eff="taa"))
    must_fail("别的 RGBA16F 行带 out slot，不许顶掉 taa 的名字锚", "TAA pass 静默没进图",
              0, vlog(TAA_OTHER_NAME), vcfg(aa_eff="taa"))
    must_fail("ssr(apply) 行不是 RGBA16F 格式 = 不是那张 RT", "请求 ssr（",
              0, vlog(SSR_OTHER_FMT, SSR_TRACE), vcfg(ssr=True))
    must_fail("ssr_trace 行没申报 slot = 不是那张 RT 行", "请求 ssr（",
              0, vlog(SSR, SSR_TRACE_NO_SLOT), vcfg(ssr=True))
    must_fail("出图名后面多挂了字（口径形态变了）必须红", "出图路径不是 gbuffer_3.png",
              0, vlog(SHOT_GBUF_TAILORED, drop="截图已保存"),
              vcfg(gbuf=3, expect_file="screenshots/gbuffer_3.png"))
    must_pass("gate_lint 提到 no_vvl 不算「验证层没启用」（判据锚在 diagnostics: 行）", 0,
              vlog(GATE_NO_VVL), vcfg(sync=True))
    #     逃生门的锚是「aa="fsr" 开门但接不起来」整串：只留「接不起来」的话，别的门改了文案
    #     也会替 FSR 开门（引擎今天只有一行含「接不起来」，这条钉的是锚不许被削薄）
    must_fail("别条 warn 说「接不起来」不许开 fsr 的逃生门", "请求 --aa fsr 但日志里没有",
              0, vlog(VEL, DEGRADE_OTHER), vcfg(aa_eff="fsr"))
    must_pass("默认档看到「fsr temporal: 关」不该红（门关着就是关着，反向只抓真开着）", 0,
              vlog(TEMP_OFF), vcfg())
    #     ×0.5 的口径是 **floor**（= C++ scaleRtExtent）：偶数网格上 floor 与 ceil 相同，
    #     只有奇数网格能区分二者 ⟹ 骨架那份 1280x720 验不出 ceil 被换成 floor 的变异
    must_pass("ssr_trace 的 ×0.5 是 floor 不是 ceil（奇数网格上才分得出）", 0,
              vlog(SSR, SSR_TRACE).replace("1280x720", "1279x719").replace("640x360", "639x359"),
              vcfg(ssr=True, dims=(1279, 719)))
    #     hazard 计数是 --sync 档的核心读数，放宽成数 "hazard" 子串就会被"已覆盖的边"那行喂饱
    readout = vsay(0, vlog(HAZARD, HAZARD_EDGE), vcfg(sync=True))
    vok(any(ln.startswith("同步验证: hazard 1 条") for ln in readout),
        f"hazard 计数只准数真 hazard（引擎另有一行 'compute hazard: … 由边 … 覆盖' 不是 hazard）：{readout}")

    done.append(f"判据段 verdicts：{vcase} 个合成用例逐条钉住（阳性锚点 + 每条 fail() 会红、"
                "每条放行档不红、每条正则变松也会被近邻行抓到）")

    if not quiet:
        print(f"[smoke] 自检 {len(done)} 项通过（patch_lua 单行子表/多行块/两条护栏 + "
              "判据段 verdicts 逐条会红）")
    return done


if __name__ == "__main__":
    sys.exit(main())
