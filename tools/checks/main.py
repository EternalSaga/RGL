"""RGL 源码检查统一入口（CMake source_checks 目标 / just checks|doc-check|style-check）。

单包单入口跑两套规则，**每个编译单元只做一次 libclang parse**，样式指标与
Doxygen 声明表共享同一份 AST 与同一份缓存（build/checks_cache，源字节+命令
SHA 键；schema = metrics.py 文件头，CACHE_VERSION 随结构演进）：

- 样式八类规则（--only style|all）→ rules.evaluate_metrics + text_checks +
  forbidden_vulkan_checks（判定与名单见 rules.py / config.py）。
- Doxygen 注释规则（--only doc|all）→ rules.rule_doxygen；声明表来自
  libclang Indexer（穿透 export，坑 7；2026-09-23 起替代文本 -ast-dump 双管线，
  行为对账记录见 docs/调试历史.md 同日条目）。impl 单元接口已声明函数的
  定义处不重复要求注释（真相源 = 声明处）。

竞态契约（坑 4c）：本工具与编译并行会读到 ninja 半写的 BMI/.modmap →
libclang crash / parse NULL。主防线 = CMake 侧 add_dependencies(source_checks rgl)
（构建产物就绪后才跑检查）；worker 内 bmi_stable 双采样 + 1/2/4s 退避重试兜底
手动并行场景；全部尝试耗尽按"检查失败（跳过）"上报——**非 --strict 恒
exit 0，任何检查失败都不阻断构建**（BrokenProcessPool 同样只警告）。

坑位索引（详见各模块文件头）：@response 展开/resource-dir/argv 混入/BMI 预检
→ cmdclean.py；LLVM 22 结构体布局/Indexer/export 不可见 → clangbind.py；
TOK_COMMENT/CACHE_VERSION 等常量 → config.py。

用法：
    python3 tools/checks/main.py [--build-dir build] [--only style|doc|all]
                                 [--workers N] [--file-threshold N] [--func-threshold N]
                                 [--cognitive-threshold N] [--impl-threshold N]
                                 [--max-gap N] [--methods] [--strict] [--verbose]
（--methods 为兼容旧调用的 no-op：方法检查现已恒开）
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
from rules import (evaluate_metrics, forbidden_vulkan_checks,
                   interface_declared_names, rule_doxygen, text_checks)
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
    ap = argparse.ArgumentParser(description="RGL 源码检查（样式 + Doxygen）")
    ap.add_argument("--build-dir", default=None, help="build 目录（默认 <repo>/build）")
    ap.add_argument("--only", choices=("style", "doc", "all"), default="all",
                    help="只跑一组规则的求值（parse/缓存共享；默认 all）")
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
    ap.add_argument("--max-gap", type=int, default=0,
                    help="Doxygen：注释块与声明之间允许的最大空行数（默认 0 = 紧贴）")
    ap.add_argument("--methods", action="store_true",
                    help="兼容旧调用的 no-op（方法检查已恒开）")
    ap.add_argument("--strict", action="store_true",
                    help="有警告时 exit 1（默认只警告不阻断）")
    ap.add_argument("--verbose", action="store_true",
                    help="附加输出认知复杂度 top 6 函数（调阈值/sanity check 用）")
    return ap.parse_args()


def main():
    args = parse_args()
    only = args.only
    thresholds = Thresholds(file=args.file_threshold, func=args.func_threshold,
                            cognitive=args.cognitive_threshold,
                            impl=args.impl_threshold)

    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(ROOT, "build")
    by_file, driver = load_db(build)
    if driver is None:
        raise SystemExit("error: compile_commands.json 里找不到 clang 驱动路径")
    libclang = find_libclang(driver)
    if libclang is None:
        print(f"[checks] 警告: 找不到 libclang（驱动 {driver} 推导），跳过检查")
        return
    resdir = subprocess.run([driver, "-print-resource-dir"],
                            capture_output=True, text=True).stdout.strip()

    units = select_units(by_file)

    # 预检构建产物（坑 4b）：clean 后首次构建 modmap/BMI 尚未生成，未就绪的跳过
    # libclang 解析——纯文本规则（分层/模块优先/STL 白名单）不受影响照常跑。
    ready = [u for u in units if bmi_ready(u[1])]
    notready = len(units) - len(ready)

    ctx = {"root": ROOT, "build": build, "libclang": libclang,
           "cache_dir": os.path.join(build, "checks_cache"),
           "scratch_dir": os.path.join(build, "checks_scratch"),
           "resdir": resdir}
    os.makedirs(ctx["scratch_dir"], exist_ok=True)
    os.makedirs(ctx["cache_dir"], exist_ok=True)

    metrics_by_rel = {}
    warnings, errors, hits, parsed = [], [], 0, 0
    skipped_notready_err = 0
    cog_top = []
    try:
        pool_iter = _pool_results(ctx, ready, args.workers)
        for rel, metrics, how in pool_iter:
            if metrics is None:
                if how.startswith("skip:"):
                    skipped_notready_err += 1
                    errors.append((rel, "跳过（产物未就绪）: " + how[5:]))
                else:
                    errors.append((rel, how[len("error:"):]))
                continue
            if how == "cache":
                hits += 1
            else:
                parsed += 1
            metrics_by_rel[rel] = metrics
            if only in ("style", "all"):
                warnings.extend(evaluate_metrics(rel, metrics, thresholds))
            for fn in metrics["functions"]:
                cog_top.append((fn["cognitive"], rel, fn["line"], fn["name"]))
    except BrokenProcessPool:
        # libclang worker 进程猝死（段错误/OOM）：本批未消费文件整体跳过（警告语义不
        # 阻断构建——2026-09-11 教训：pool 崩溃 traceback 会让 ninja 误判检查目标
        # 失败，-k1 停调度连带拖死整条构建）。已就绪 BMI 重跑即恢复。
        print("[checks] libclang worker 进程崩溃（BrokenProcessPool），本次剩余文件跳过"
              "——多为 BMI 写坏（构建中途 kill）或 libclang 缺陷，编译完成重跑即恢复")

    if only in ("style", "all"):
        warnings.extend(text_checks())
        warnings.extend(forbidden_vulkan_checks())

    doc_warnings, doc_checked, doc_unverified, doc_skipped = [], 0, 0, 0
    if only in ("doc", "all"):
        # 第一遍：接口单元声明名表（impl 定义处跨查豁免用）
        iface_names = {}
        for rel, m in metrics_by_rel.items():
            if not rel.endswith("_impl.cppm"):
                iface_names[rel] = interface_declared_names(m)
        for rel in sorted(metrics_by_rel):
            src = os.path.join(ROOT, rel)
            try:
                with open(src, encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
            except OSError:
                continue
            skip = None
            if rel.endswith("_impl.cppm"):
                base = rel[: -len("_impl.cppm")] + ".cppm"
                skip = iface_names.get(base)
            w, c, u, s = rule_doxygen(rel, metrics_by_rel[rel], lines,
                                      max_gap=args.max_gap, skip_names=skip)
            doc_warnings.extend(w)
            doc_checked += c
            doc_unverified += u
            doc_skipped += s

    style_warnings = warnings if only in ("style", "all") else []
    if only in ("style", "all"):
        for rel, line, msg in sorted(warnings, key=lambda w: (w[0], w[1])):
            loc = f"{rel}:{line}" if line else rel
            print(f"[style-check] {loc}: {msg}")
    for rel, err in errors:
        print(f"[checks] {rel}: 检查失败（跳过）: {err}")
    if only in ("doc", "all"):
        for rel, line, msg in sorted(doc_warnings, key=lambda w: (w[0], w[1])):
            print(f"[doc-check] {rel}:{line}: {msg}")
    if args.verbose and only in ("style", "all"):
        print("[style-check] 认知复杂度 top 6:")
        for cog, rel, line, name in sorted(cog_top, reverse=True)[:6]:
            print(f"[style-check]   {cog:3d}  {rel}:{line}  {name}")
    if only in ("style", "all"):
        skipped_note = f"、产物未就绪跳过 {notready}" if notready else ""
        print(f"[style-check] 汇总: {len(style_warnings)} 警告 / {len(units)} 个 .cppm"
              f"（缓存命中 {hits}、解析 {parsed}、失败 {len(errors)}{skipped_note}）")
        if notready:
            print("[style-check] 提示: 跳过 = modmap/BMI 构建产物缺失（clean 后首次构建属正常，"
                  "与编译并行时尚未生成）；本次仅跑纯文本规则，编译完成后重跑即全量生效")
    if only in ("doc", "all"):
        print(f"[doc-check] 汇总: {len(doc_warnings)} 警告 / {doc_checked} 声明已检查"
              f"（{doc_unverified} 行号未验证、{doc_skipped} 接口已声明跳过），"
              f"{len(units)} 个 .cppm")
    all_warns = (style_warnings if only in ("style", "all") else []) + \
                (doc_warnings if only in ("doc", "all") else [])
    sys.exit(1 if (args.strict and (all_warns or errors)) else 0)


if __name__ == "__main__":
    main()
