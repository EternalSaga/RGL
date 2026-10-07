"""RGL 源码检查统一入口（CMake source_checks 目标 / just checks|doc-check|style-check）。

要求 **Python ≥ 3.14**（注解惰性求值 + 项目工具链对齐）。

单包单入口跑两套规则，**每个编译单元只做一次 libclang parse**，样式指标与
Doxygen 声明表共享同一份 AST 与同一份缓存（build/checks_cache，源字节+命令
SHA 键；schema = metrics.py 文件头，CACHE_VERSION 随结构演进）：

- 样式规则（--only style|all）→ rules_metrics.evaluate_metrics + rules_text.*
  （判定与名单见 rules_*.py / config.py；设计见 docs/风格检查规则.md）。
- Doxygen 注释规则（--only doc|all）→ rules_doxygen.rule_doxygen；声明表来自
  libclang Indexer（穿透 export，坑 7；2026-09-23 起替代文本 -ast-dump 双管线，
  行为对账记录见 docs/调试历史.md 同日条目）。impl 单元接口已声明函数的
  定义处不重复要求注释（真相源 = 声明处）。

工具侧（批次 A：tools/imgqa 纳入样式规则）：工具单元走**独立阈值集**、关掉 impl_slim
（C++20 modules 接口瘦身语义，对纯 .cpp 无意义），且 **Doxygen 规则跳过工具单元**
（imgqa 注释是散文式，套 doc 规则 = 265 条噪声）。存量钉死在 tool_baseline.json
「只准降不准升」（机制 → tool_baseline.py）；汇总单列一行报告工具侧，让 rgl 侧
「0 警告」这个既有性质仍然一眼可读。`--strict` 走**棘轮**：工具侧存量不夺非零码，
只有基线上升才红——否则 `just checks --strict` 0 警告这条验收仪式从此永久作废。

竞态契约（坑 4c）：本工具与编译并行会读到 ninja 半写的 BMI/.modmap →
libclang crash / parse NULL。主防线 = CMake 侧 add_dependencies(source_checks rgl)
（构建产物就绪后才跑检查）；worker 内 bmi_stable 双采样 + 1/2/4s 退避重试兜底
手动并行场景；全部尝试耗尽按"检查失败（跳过）"上报——**非 --strict 恒
exit 0，任何检查失败都不阻断构建**（BrokenProcessPool 同样只警告）。

坑位索引（详见各模块文件头）：@response 展开/resource-dir/argv 混入/BMI 预检
→ cmdclean.py；LLVM 22 结构体布局/Indexer/export 不可见 → clangbind.py；
TOK_COMMENT/CACHE_VERSION 等常量 → config.py；Warning/阈值 → models.py。

    用法：
    python3 tools/checks/main.py [--build-dir build] [--only style|doc|all]
                                 [--workers N] [--file-threshold N] [--func-threshold N]
                                 [--cognitive-threshold N] [--cognitive-error-threshold N]
                                 [--impl-threshold N] [--max-gap N] [--methods]
                                 [--strict] [--verbose] [--update-tool-baseline]
（--methods 为兼容旧调用的 no-op：方法检查现已恒开）
（--file-threshold/--func-threshold/--cognitive-* 显式传时 rgl 与工具侧**同步生效**，
  不传则各用各的默认；--update-tool-baseline 用当前条数重写工具侧基线）
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path

if sys.version_info < (3, 14):
    raise SystemExit(
        f"error: tools/checks 需要 Python ≥3.14，当前 {sys.version.split()[0]}")

from clangbind import find_libclang
from cmdclean import bmi_ready
from config import (DEFAULT_COGNITIVE_ERROR_THRESHOLD, DEFAULT_COGNITIVE_THRESHOLD,
                    DEFAULT_FILE_THRESHOLD, DEFAULT_FUNC_THRESHOLD,
                    DEFAULT_IMPL_THRESHOLD, EXCLUDE_FILES, EXCLUDE_SUBDIRS, ROOT,
                    TOOL_COGNITIVE_ERROR_THRESHOLD, TOOL_COGNITIVE_THRESHOLD,
                    TOOL_DISABLED_RULES, TOOL_FILE_THRESHOLD, TOOL_FUNC_THRESHOLD,
                    TOOL_UNIT_PREFIXES, TOOL_UNIT_SUFFIXES)
from fs import read_text
from rules_doxygen import interface_declared_names, rule_doxygen
from rules_metrics import evaluate_metrics
from rules_layers import lib_layering_checks
from rules_text import (agents_doc_index_checks, docs_status_checks,
                        forbidden_vulkan_checks, slang_load_bitcast_checks,
                        modern_cpp_checks, text_checks)
from models import Thresholds, Warning
from tool_baseline import compare as compare_baseline
from tool_baseline import load_baseline, write_baseline
from worker import _pool_results


def load_db(build_dir: str) -> tuple[dict[str, dict], str | None]:
    """读 compile_commands.json → ({绝对源路径: 条目}, clang 驱动路径)。"""
    db_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        raise SystemExit(
            f"error: 找不到 {db_path}\n  检查器复用 RGL 构建的编译命令（modules 依赖在里面）。\n"
            "  请先 just config && just build。")
    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)
    by_file: dict[str, dict] = {}
    driver = None
    for e in db:
        full = os.path.abspath(os.path.join(e["directory"], e["file"]))
        by_file[full] = e
        if driver is None:
            first = shlex.split(e["command"], posix=os.name != "nt")[0]
            if "clang" in os.path.basename(first):
                driver = first
    return by_file, driver


def select_units(by_file: dict[str, dict]) -> list[tuple[str, dict, bool]]:
    """db 条目 → 待检单元 [(rel, entry, is_tool)]（排除 generated/ 与 EXCLUDE_FILES）。

    两类单元（is_tool 区分，下游三处行为不同：阈值集 / 规则开关 / doc 规则跳过）：
    - rgl 单元 = `.cppm` 且路径含 `/rgl/`（generated/ 与 EXCLUDE_FILES 除外）；
    - 工具单元 = 落在 config.TOOL_UNIT_PREFIXES 下、后缀在 TOOL_UNIT_SUFFIXES 里
      （批次 A：tools/imgqa 的 15 个 .cpp TU；`.h` 现在没有独立编译命令，列上是为了
      将来把头挂进 target 时不用改代码）。
    两侧共用排除项，语义一致。
    """
    units: list[tuple[str, dict, bool]] = []
    root = Path(ROOT).resolve()
    for full in sorted(by_file):
        norm = full.replace(os.sep, "/")
        try:
            rel = Path(full).resolve().relative_to(root).as_posix()
        except ValueError:
            continue  # 仓库外的编译命令（vcpkg 生成物等）不检
        is_tool = (rel.startswith(TOOL_UNIT_PREFIXES)
                   and rel.endswith(TOOL_UNIT_SUFFIXES))
        if not is_tool and not (full.endswith(".cppm") and "/rgl/" in norm):
            continue
        if is_tool:
            # imgqa 的 @response(.modmap) 实测 0 字节且它没有 -fmodule-file，故允许缺失
            # （否则 imgqa 从未构建的树上工具侧护栏整体空转）——机制见 cmdclean.expand_at
            by_file[full]["at_optional"] = True
        if any(x in norm for x in EXCLUDE_SUBDIRS):
            continue
        if rel in EXCLUDE_FILES:
            continue
        units.append((rel, by_file[full], is_tool))
    return units


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="RGL 源码检查（样式 + Doxygen；需 Python ≥3.14）")
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
    ap.add_argument("--cognitive-error-threshold", type=int,
                    default=DEFAULT_COGNITIVE_ERROR_THRESHOLD,
                    help=f"认知复杂度 error 级阈值（超此值红粗体高亮，"
                         f"默认 {DEFAULT_COGNITIVE_ERROR_THRESHOLD}；仍不阻断）")
    ap.add_argument("--impl-threshold", type=int, default=DEFAULT_IMPL_THRESHOLD,
                    help=f"接口单元函数体代码行阈值（默认 {DEFAULT_IMPL_THRESHOLD}）")
    ap.add_argument("--max-gap", type=int, default=0,
                    help="Doxygen：注释块与声明之间允许的最大空行数（默认 0 = 紧贴）")
    ap.add_argument("--methods", action="store_true",
                    help="兼容旧调用的 no-op（方法检查已恒开）")
    ap.add_argument("--strict", action="store_true",
                    help="有警告时 exit 1（默认只警告不阻断）。工具侧（tools/）存量由"
                         "基线豁免、不夺非零码，只有**基线上升/未登记**才变红（棘轮）")
    ap.add_argument("--verbose", action="store_true",
                    help="附加输出认知复杂度 top 6 函数（调阈值/sanity check 用）")
    ap.add_argument("--update-tool-baseline", action="store_true",
                    help="用当前工具侧条数重写 tool_baseline.json（样式警告降了之后收账；"
                         "上升时**不要**用它，那是拆函数的活儿）")
    return ap.parse_args()


def tool_thresholds(args: argparse.Namespace) -> Thresholds:
    """工具侧阈值集。

    不传 CLI 时用 config.TOOL_*（起步与 rgl 现值一致）；**显式传了就两侧同步生效**
    ——阳性锚点靠这个性质：`--cognitive-threshold 200` 必须能看见工具侧条数掉下来，
    否则"阈值接到了工具单元上"这件事无法验证。impl 阈值无意义（impl_slim 已关）。
    """
    def pick(cli: int, rgl_default: int, tool_default: int) -> int:
        return tool_default if cli == rgl_default else cli

    return Thresholds(
        file=pick(args.file_threshold, DEFAULT_FILE_THRESHOLD, TOOL_FILE_THRESHOLD),
        func=pick(args.func_threshold, DEFAULT_FUNC_THRESHOLD, TOOL_FUNC_THRESHOLD),
        cognitive=pick(args.cognitive_threshold, DEFAULT_COGNITIVE_THRESHOLD,
                       TOOL_COGNITIVE_THRESHOLD),
        cognitive_error=pick(args.cognitive_error_threshold,
                             DEFAULT_COGNITIVE_ERROR_THRESHOLD,
                             TOOL_COGNITIVE_ERROR_THRESHOLD),
        impl=DEFAULT_IMPL_THRESHOLD)


def main() -> None:
    args = parse_args()
    only = args.only
    run_style = only in ("style", "all")
    run_doc = only in ("doc", "all")
    thresholds = Thresholds(file=args.file_threshold, func=args.func_threshold,
                            cognitive=args.cognitive_threshold,
                            cognitive_error=args.cognitive_error_threshold,
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
    tool_rels = {rel for rel, _e, is_tool in units if is_tool}
    tool_total = len(tool_rels)
    rgl_total = len(units) - tool_total

    # 预检构建产物（坑 4b）：clean 后首次构建 modmap/BMI 尚未生成，未就绪的跳过
    # libclang 解析——纯文本规则（分层/模块优先/STL 白名单）不受影响照常跑。
    # 工具单元（.cpp）的命令里没有 -fmodule-file，只 @ 自己的（空）modmap。
    ready = [(rel, entry) for rel, entry, _t in units if bmi_ready(entry)]
    ready_rels = {rel for rel, _e in ready}
    rgl_notready = sum(1 for rel, _e, is_tool in units
                       if not is_tool and rel not in ready_rels)

    ctx = {"root": ROOT, "build": build, "libclang": libclang,
           "cache_dir": os.path.join(build, "checks_cache"),
           "scratch_dir": os.path.join(build, "checks_scratch"),
           "resdir": resdir}
    os.makedirs(ctx["scratch_dir"], exist_ok=True)
    os.makedirs(ctx["cache_dir"], exist_ok=True)

    tool_th = tool_thresholds(args)
    metrics_by_rel = {}
    warnings: list[Warning] = []
    tool_warnings: list[Warning] = []
    errors: list[tuple[str, str]] = []
    tool_errors: list[tuple[str, str]] = []
    hits = parsed = skipped_notready_err = 0
    tool_counts: dict[str, int] = {}   # rel → 当前样式条数（基线比对的输入）
    cog_top: list[tuple[int, str, int, str]] = []
    try:
        pool_iter = _pool_results(ctx, ready, args.workers)
        for rel, metrics, how in pool_iter:
            is_tool = rel in tool_rels
            if metrics is None:
                bucket = tool_errors if is_tool else errors
                if how.startswith("skip:"):
                    if not is_tool:
                        skipped_notready_err += 1
                    bucket.append((rel, "跳过（产物未就绪）: " + how[5:]))
                else:
                    bucket.append((rel, how[len("error:"):]))
                continue
            if not is_tool:  # 工具侧不单独计缓存命中（汇总行只报条数/基线）
                if how == "cache":
                    hits += 1
                else:
                    parsed += 1
            metrics_by_rel[rel] = metrics
            if not run_style:
                continue
            if is_tool:
                # 工具侧：独立阈值集 + 关掉 impl_slim（modules 专属语义）
                found = evaluate_metrics(rel, metrics, tool_th, TOOL_DISABLED_RULES)
                tool_warnings.extend(found)
                tool_counts[rel] = len(found)
            else:
                warnings.extend(evaluate_metrics(rel, metrics, thresholds))
                for fn in metrics["functions"]:
                    cog_top.append((fn["cognitive"], rel, fn["line"], fn["name"]))
    except BrokenProcessPool:
        # libclang worker 进程猝死（段错误/OOM）：本批未消费文件整体跳过（警告语义不
        # 阻断构建——2026-09-11 教训：pool 崩溃 traceback 会让 ninja 误判检查目标
        # 失败，-k1 停调度连带拖死整条构建）。已就绪 BMI 重跑即恢复。
        print("[checks] libclang worker 进程崩溃（BrokenProcessPool），本次剩余文件跳过"
              "——多为 BMI 写坏（构建中途 kill）或 libclang 缺陷，编译完成重跑即恢复")

    if run_style:
        warnings.extend(text_checks())
        warnings.extend(forbidden_vulkan_checks())
        warnings.extend(slang_load_bitcast_checks())
        warnings.extend(modern_cpp_checks())
        warnings.extend(docs_status_checks())
        warnings.extend(agents_doc_index_checks())
        warnings.extend(lib_layering_checks())

    # 工具侧基线（只准降不准升）：--update-tool-baseline 先重写再比对（必然持平）。
    # 未解析的 TU 不参与比对，其基线条数原样保留（否则 clean 后一跑就把基线清零）。
    baseline_warnings: list[Warning] = []
    tool_report = None
    if run_style and tool_total:
        if args.update_tool_baseline:
            merged = {k: v for k, v in load_baseline().items()
                      if os.path.exists(os.path.join(ROOT, k))}
            merged.update(tool_counts)
            path = write_baseline(merged)
            print(f"[style-check] 工具侧基线已重写: {sum(merged.values())} 条 / "
                  f"{len(merged)} 个文件 → {os.path.relpath(path, ROOT)}")
        tool_report = compare_baseline(tool_counts, load_baseline(),
                                       unparsed=tool_total - len(tool_counts))
        baseline_warnings = tool_report.warnings

    doc_warnings: list[Warning] = []
    doc_checked = doc_unverified = doc_skipped = 0
    if run_doc:
        # 工具单元不吃 doc 规则（imgqa 注释是散文式，265 条 d2/d4/d6 噪声）——
        # 声明名表与求值两处都要排除，否则工具单元会混进 iface_names 与计数。
        doc_rels = [r for r in metrics_by_rel if r not in tool_rels]
        # 第一遍：接口单元声明名表（impl 定义处跨查豁免用）
        iface_names = {}
        for rel in doc_rels:
            if not rel.endswith("_impl.cppm"):
                iface_names[rel] = interface_declared_names(metrics_by_rel[rel])
        for rel in sorted(doc_rels):
            src = read_text(Path(ROOT) / rel)
            if src is None:
                continue
            lines = src.splitlines()
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

    def emit(ws: list[Warning], tag: str) -> None:
        """按 (rel,line) 稳定序打印警告；error 级红粗体辣眼高亮（强制 ANSI，ninja
        捕获输出也可见；仍不阻断构建）。"""
        for w in sorted(ws, key=lambda x: (x.rel, x.line)):
            loc = f"{w.rel}:{w.line}" if w.line else w.rel
            if w.severity == "error":
                print(f"\033[1;31m[{tag}-ERROR] {loc}: {w.msg}\033[0m")
            else:
                print(f"[{tag}] {loc}: {w.msg}")

    style_warnings = warnings if run_style else []
    if run_style:
        emit(style_warnings, "style-check")
        emit(tool_warnings + baseline_warnings, "style-check")
    for rel, err in errors + tool_errors:
        print(f"[checks] {rel}: 检查失败（跳过）: {err}")
    if run_doc:
        for w in sorted(doc_warnings, key=lambda x: (x.rel, x.line)):
            print(f"[doc-check] {w.rel}:{w.line}: {w.msg}")
    if args.verbose and run_style:
        print("[style-check] 认知复杂度 top 6:")
        for cog, rel, line, name in sorted(cog_top, reverse=True)[:6]:
            print(f"[style-check]   {cog:3d}  {rel}:{line}  {name}")
    if run_style:
        n_err = sum(1 for w in style_warnings if w.severity == "error")
        skipped_note = f"、产物未就绪跳过 {rgl_notready}" if rgl_notready else ""
        err_note = f"（其中 ERROR {n_err}）" if n_err else ""
        print(f"[style-check] 汇总: {len(style_warnings)} 警告{err_note} / {rgl_total} 个 .cppm"
              f"（缓存命中 {hits}、解析 {parsed}、失败 {len(errors)}{skipped_note}）")
        if rgl_notready:
            print("[style-check] 提示: 跳过 = modmap/BMI 构建产物缺失（clean 后首次构建属正常，"
                  "与编译并行时尚未生成）；本次仅跑纯文本规则，编译完成后重跑即全量生效")
        if tool_total and tool_report:
            # 工具侧单列一行：rgl 侧「0 警告」是既有性质，不被工具侧存量污染
            cur, allowed = tool_report.current, tool_report.allowed
            compared, unparsed = tool_report.compared, tool_report.unparsed
            n_tool_err = sum(1 for w in tool_warnings + baseline_warnings
                             if w.severity == "error")
            err_note_tool = f"（其中 ERROR {n_tool_err}）" if n_tool_err else ""
            if compared == 0:
                # 一个都没解析成（imgqa 从未构建 → modmap 缺失）：绝不许读成
                # 「基线 0，持平」——没数不是零，那是假绿的镜像
                state = f"未比对：{unparsed} 个 TU 未解析"
            else:
                delta = cur - allowed
                trend = ("持平" if delta == 0 else
                         f"上升 {delta}" if delta > 0 else f"可下调 {-delta}")
                state = (f"基线 {allowed}，{trend}"
                         + (f"，未解析 {unparsed}（未比对）" if unparsed else ""))
            label = "、".join(p.rstrip("/") for p in TOOL_UNIT_PREFIXES)
            print(f"[style-check] 工具侧（{label}）: "
                  f"{len(tool_warnings)} 警告{err_note_tool} / {tool_total} 个 TU"
                  f"（{state}）")
            if unparsed:
                print("[style-check] 工具侧提示: 未解析 = imgqa 的 .modmap 尚未生成"
                      "（编译命令里没有 -fmodule-file，但 bmi_ready 要 @response 文件在）"
                      "——跑 just build-tools 后即生效")
    if run_doc:
        print(f"[doc-check] 汇总: {len(doc_warnings)} 警告 / {doc_checked} 声明已检查"
              f"（{doc_unverified} 行号未验证、{doc_skipped} 接口已声明跳过），"
              f"{rgl_total} 个 .cppm")
    # --strict 的计数面（**棘轮**，不是"全部警告"）：`just checks --strict` 0 警告是本项目
    # 历次批次的验收仪式（phase10b/11 多处以此收账）。工具侧一次性纳入时存量 74 条，
    # 若把它计入，这条仪式从此永久红，往后每条验收都得解释例外——仪式作废比没有仪式更贵。
    # 故：工具侧**存量**由基线豁免（照常打印，只是不夺非零码），**基线上升/未登记**
    # （error 级）照样让 --strict 变红——棘轮只挡倒退，不挡历史。
    # 「可下调」（warning 级）不计数：降了是好事，非零码会惩罚正确的行为。
    strict_warns = (style_warnings if run_style else []) + (doc_warnings if run_doc else [])
    if run_style:
        strict_warns += [w for w in baseline_warnings if w.severity == "error"]
    sys.exit(1 if (args.strict and (strict_warns or errors or tool_errors)) else 0)


if __name__ == "__main__":
    main()
