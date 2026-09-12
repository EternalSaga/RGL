"""文件/函数结构检查（CMake style_check 目标 / just style-check）——CLI 入口。

八类规则（前五项走 libclang 解析，后三项纯文本主进程；判定全在 rules.py）：
- 文件代码行 > --file-threshold（默认 500）→ 提示拆分文件
- 函数代码行 > --func-threshold（默认 120）→ 提示拆分函数
- 函数认知复杂度 > --cognitive-threshold（默认 15）→ 提示拆分函数
  （SonarSource 风格：if/for/while/do/catch/三元 结构 +1+嵌套；switch 与
  else/else-if +1 不递增嵌套；一段连续 &&/|| 整段 +1；**lambda 独立计分**——
  宿主不叠加 lambda 体，lambda 各自成条目从 nesting 0 算——算法 = clangbind）
- 接口瘦身：非 _impl 文件里函数定义 > --impl-threshold（默认 100）代码行
  → 提示迁入 xxx_impl.cppm（AGENTS.md"接口瘦身"，接口 BMI 不含函数体）
- 成员命名：成员变量名带 _/m_ 前缀或 _ 后缀 → 提示自然命名
- 分层/模块优先（文本）：底层零 entt（不得 import ecs、GMF 不得 include <entt>，
  名单 ZERO_ENTT_MODULES）；点名的禁止 import 边（FORBIDDEN_EDGES）；rgl/ 下
  不得有 .h/.hpp/.hh、.cpp 仅许 main.cpp；任何内部头 #include " 禁止
- GMF STL 白名单：GMF #include <STL 头> 只许 ranges/filesystem/chrono 三例外，
  其余走 import rlstd（AGENTS.md"优先使用 rlstd"）
- 禁用旧式 Vulkan API（文本，剥注释）：经典 descriptor pool / pipeline object /
  render pass / push constants 名单命中即警告（项目已全面 descriptor heap /
  shader object / dynamic rendering，见 AGENTS.md）

代码行 = 去除注释与空行，token 级精确。

包结构（本目录，模块间平铺 import；ProcessPoolExecutor 顶层可 pickle 要求见 worker.py）：
- stylecheck.py — 本文件：argparse + 主流程编排
- config.py     — 阈值常量 + Thresholds + 全部名单（含 FORBIDDEN_VULKAN_APIS）
- clangbind.py  — libclang ctypes 结构体/绑定 + AST helper + 认知复杂度
- cmdclean.py   — 编译命令清理（@response 展开、-fmodule-file 绝对化、BMI 预检）
- metrics.py    — analyze_file 单文件指标
- worker.py     — worker 进程（_worker_init/_worker_task/_pool_results）
- rules.py      — 全部规则判定（每条规则一个函数）

设计要点：
- 与 doccheck 同源：复用 build/compile_commands.json 的编译命令（含 C++20 modules 的
  .modmap BMI 依赖），libclang（ctypes）进程内解析，纯 Python 标准库零 pip 依赖。
- 坑（本机实测，勿回退）——实现位置：
  1. libclang C API 不展开 @response file（模块参数藏在 *.modmap，须内联展开）
     → cmdclean.expand_at；
  2. 输入文件与驱动路径不能混进 argv（混入 → parse 返回 NULL）→ cmdclean.clean_cmd；
  3. Gentoo 布局须显式 -resource-dir（否则 stddef.h not found、AST 残缺）
     → cmdclean.clean_cmd；
  4. `export module` + import + BMI 加载失败（缺失/标准不匹配）→ parse 返回 NULL
     且无诊断（libclang 缺陷）——按"跳过该文件 + 警告"处理（metrics.analyze_file
     抛 RuntimeError，主进程记"检查失败"不阻断）；
  4b. clean 后首次构建：@modmap 与 -fmodule-file 指向的 BMI 都是构建产物、此时不
     存在——主进程预检 bmi_ready 整体跳过未就绪文件（纯文本规则不受影响照常跑），
     不当失败报（cmdclean.bmi_ready）；
  5. LLVM 22 的 C API 结构体布局与网上旧例程全不同（CXCursor=32B、
     CXSourceLocation/Range=24B、CXToken=24B 值类型、CXString=16B 值类型；
     clang_makeRange 更名 clang_getRange）——照旧例程写必段错误 → clangbind。
  6. CXToken_Comment 枚举值 = 4（非 5）——曾误写 TOK_COMMENT=5 致注释 token 漏
     过滤、每条注释首行计入代码行，行数类规则大面积虚高（2026-09-12 修正）。
- 并发：ctypes 持 GIL，线程并行无效——ProcessPoolExecutor 一进程一文件（每进程加载
  一次 libclang），默认 min(cpu, 4) 个 worker，每进程 ~550MB 内存（worker.py）。
- 缓存 {build}/stylecheck_cache（key = sha256(CACHE_VERSION + 源字节 + 命令)）：
  增量只重解析改动文件；全量 69 文件 4 worker ~20s，与编译并行不占关键路径。
- 只警告不阻断（exit 0）；--strict 有警告 exit 1。

用法：
    python3 tools/stylecheck/stylecheck.py [--build-dir build] [--workers N]
                                           [--file-threshold 500] [--func-threshold 120]
                                           [--cognitive-threshold 15] [--impl-threshold 100]
                                           [--strict] [--verbose]
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures.process import BrokenProcessPool

from clangbind import find_libclang
from cmdclean import bmi_ready
from config import (DEFAULT_COGNITIVE_THRESHOLD, DEFAULT_FILE_THRESHOLD,
                    DEFAULT_FUNC_THRESHOLD, DEFAULT_IMPL_THRESHOLD, EXCLUDE_FILES,
                    EXCLUDE_SUBDIRS, ROOT, Thresholds)
from rules import evaluate_metrics, forbidden_vulkan_checks, text_checks
from worker import _pool_results


def load_db(build_dir):
    """读 compile_commands.json → ({绝对源路径: 条目}, clang 驱动路径)。"""
    db_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        raise SystemExit(
            f"error: 找不到 {db_path}\n  检查器复用 RGL 构建的编译命令（modules 依赖在里面）。\n"
            "  请先 just config && just build。")
    db = json.load(open(db_path, encoding="utf-8"))
    by_file = {}
    driver = None
    for e in db:
        full = os.path.abspath(os.path.join(e["directory"], e["file"]))
        by_file[full] = e
        if driver is None:
            first = shlex.split(e["command"], posix=os.name != "nt")[0]
            if "clang" in os.path.basename(first):
                driver = first
    return by_file, driver


def select_units(by_file):
    """db 条目 → 待检 .cppm 单元 [(rel, entry)]（排除 generated/ 与 EXCLUDE_FILES）。"""
    units = []
    for full in sorted(by_file):
        if not full.endswith(".cppm"):
            continue
        norm = full.replace(os.sep, "/")
        if "/rgl/" not in norm:
            continue
        if any(x in norm for x in EXCLUDE_SUBDIRS):
            continue
        rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
        if rel in EXCLUDE_FILES:
            continue
        units.append((rel, by_file[full]))
    return units


def parse_args():
    ap = argparse.ArgumentParser(description="RGL 文件/函数结构检查")
    ap.add_argument("--build-dir", default=None, help="build 目录（默认 <repo>/build）")
    ap.add_argument("--workers", type=int, default=min(os.cpu_count() or 4, 4),
                    help="并行进程数（默认 min(cpu,4)，每进程 ~550MB 内存）")
    ap.add_argument("--file-threshold", type=int, default=DEFAULT_FILE_THRESHOLD,
                    help=f"文件代码行阈值（默认 {DEFAULT_FILE_THRESHOLD}）")
    ap.add_argument("--func-threshold", type=int, default=DEFAULT_FUNC_THRESHOLD,
                    help=f"函数代码行阈值（默认 {DEFAULT_FUNC_THRESHOLD}）")
    ap.add_argument("--cognitive-threshold", type=int,
                    default=DEFAULT_COGNITIVE_THRESHOLD,
                    help=f"函数认知复杂度阈值（默认 {DEFAULT_COGNITIVE_THRESHOLD}）")
    ap.add_argument("--impl-threshold", type=int, default=DEFAULT_IMPL_THRESHOLD,
                    help=f"接口单元函数体代码行阈值（默认 {DEFAULT_IMPL_THRESHOLD}）")
    ap.add_argument("--strict", action="store_true",
                    help="有警告时 exit 1（默认只警告不阻断）")
    ap.add_argument("--verbose", action="store_true",
                    help="附加输出认知复杂度 top 6 函数（调阈值/sanity check 用）")
    return ap.parse_args()


def main():
    args = parse_args()
    thresholds = Thresholds(file=args.file_threshold, func=args.func_threshold,
                            cognitive=args.cognitive_threshold, impl=args.impl_threshold)

    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(ROOT, "build")
    by_file, driver = load_db(build)
    if driver is None:
        raise SystemExit("error: compile_commands.json 里找不到 clang 驱动路径")
    libclang = find_libclang(driver)
    if libclang is None:
        print(f"[style-check] 警告: 找不到 libclang（驱动 {driver} 推导），跳过检查")
        return
    resdir = subprocess.run([driver, "-print-resource-dir"],
                            capture_output=True, text=True).stdout.strip()

    units = select_units(by_file)

    # 预检构建产物（坑 4b）：clean 后首次构建 modmap/BMI 尚未生成且与编译并行竞争，
    # 未就绪的跳过 libclang 解析——纯文本规则（分层/模块优先/STL 白名单）不受影响
    ready = [u for u in units if bmi_ready(u[1])]
    notready = len(units) - len(ready)

    ctx = {"root": ROOT, "build": build, "libclang": libclang,
           "cache_dir": os.path.join(build, "stylecheck_cache"),
           "scratch_dir": os.path.join(build, "stylecheck_scratch"),
           "resdir": resdir}
    os.makedirs(ctx["scratch_dir"], exist_ok=True)
    os.makedirs(ctx["cache_dir"], exist_ok=True)

    warnings, errors, hits, parsed = [], [], 0, 0
    cog_top = []  # --verbose：[(cognitive, rel, line, name)]
    try:
        pool_iter = _pool_results(ctx, ready, args.workers)
        for rel, metrics, how in pool_iter:
            if metrics is None:
                errors.append((rel, how[len("error:"):]))
                continue
            if how == "cache":
                hits += 1
            else:
                parsed += 1
            warnings.extend(evaluate_metrics(rel, metrics, thresholds))
            for fn in metrics["functions"]:
                cog_top.append((fn["cognitive"], rel, fn["line"], fn["name"]))
    except BrokenProcessPool:
        # libclang worker 进程猝死（段错误/OOM）：本批未消费文件整体跳过（警告语义不
        # 阻断构建——2026-09-11 教训：pool 崩溃 traceback 会让 ninja 误判 style_check
        # 边失败，-k1 停调度连带拖死整条构建）。已就绪 BMI 重跑即恢复。
        print("[style-check] libclang worker 进程崩溃（BrokenProcessPool），本次剩余文件跳过"
              "——多为 BMI 写坏（构建中途 kill）或 libclang 缺陷，编译完成重跑即恢复")

    warnings.extend(text_checks())
    warnings.extend(forbidden_vulkan_checks())

    for rel, line, msg in sorted(warnings, key=lambda w: (w[0], w[1])):
        loc = f"{rel}:{line}" if line else rel
        print(f"[style-check] {loc}: {msg}")
    for rel, err in errors:
        print(f"[style-check] {rel}: 检查失败（跳过）: {err}")
    if args.verbose:
        print("[style-check] 认知复杂度 top 6:")
        for cog, rel, line, name in sorted(cog_top, reverse=True)[:6]:
            print(f"[style-check]   {cog:3d}  {rel}:{line}  {name}")
    skipped_note = f"、产物未就绪跳过 {notready}" if notready else ""
    print(f"[style-check] 汇总: {len(warnings)} 警告 / {len(units)} 个 .cppm"
          f"（缓存命中 {hits}、解析 {parsed}、失败 {len(errors)}{skipped_note}）")
    if notready:
        print("[style-check] 提示: 跳过 = modmap/BMI 构建产物缺失（clean 后首次构建属正常，"
              "与编译并行时尚未生成）；本次仅跑纯文本规则，编译完成后重跑即全量生效")
    sys.exit(1 if (args.strict and (warnings or errors)) else 0)


if __name__ == "__main__":
    main()
