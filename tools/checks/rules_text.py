"""纯文本样式规则：模块结构解析 + 分层/模块优先 + modern C++ + docs 状态头 +
旧式 Vulkan API 名单（主进程直读源码，不走 libclang）。

- text_checks：A3 分层零 entt + 点名禁止 import 边 + 模块优先（rgl/ 禁 .h/.hpp/.hh、
  .cpp 白名单、禁内部头 #include "）+ GMF STL 白名单 + 模块结构四条
- modern_cpp_checks / docs_status_checks / forbidden_vulkan_checks /
  agents_doc_index_checks / agents_index_status_checks / doc_pointer_checks：见各自
  docstring 与 docs/风格检查规则.md §1–§2
"""

import json
import re
from pathlib import Path
from typing import NamedTuple

from config import (ALLOWED_CPP, AGENTS_DOC_INDEX_SECTION, AGENTS_FILE,
                    AGENTS_INDEX_STATUS_WORDS,
                    DECORATION_ORDER_ANCHOR, DECORATION_ORDER_DECORATOR_RE,
                    DECORATION_ORDER_LATE_NAME_RE, DECORATION_ORDER_PREDICATE,
                    DECORATION_ORDER_SOURCE_FILE,
                    DOC_POINTER_BARE_EXTS, DOC_POINTER_BASELINE_COMMENT,
                    DOC_POINTER_BASELINE_FILE, DOC_POINTER_EXAMPLE_FILES,
                    DOC_POINTER_IMGQA_CLI_TABLE, DOC_POINTER_JUSTFILE,
                    DOC_POINTER_PATH_EXTS, DOC_POINTER_PATH_ROOTS,
                    DOC_POINTER_REF_ROOTS, DOC_POINTER_UPDATE_CMD,
                    DOCS_STATUS_MAX_LINES, EXCLUDE_FILES,
                    EXPORT_NAMESPACE_EXEMPT, FORBIDDEN_EDGES,
                    FORBIDDEN_VULKAN_APIS, FORBIDDEN_VULKAN_MSG, JUDGMENTS_TABLE_GLOB,
                    GENERATED_INIT_BASELINE_COMMENT, GENERATED_INIT_BASELINE_FILE,
                    GENERATED_INIT_SCAN_DIRS, GENERATED_INIT_UPDATE_CMD,
                    GENERATED_STRUCT_RE, GENERATED_TRUTH_DIR,
                    GRAPH_FACT_PREDICATE_RE, GRAPH_FACT_PREDICATE_SOURCE_FILE,
                    GRAPH_FACT_PROVIDER_FILES,
                    IMGQA_SELFTEST_CALL_RE, IMGQA_SELFTEST_CHK_TOKEN,
                    IMGQA_SELFTEST_DECL_COUNT_RE, IMGQA_SELFTEST_EXPECT_RE,
                    IMGQA_SELFTEST_HEADER, IMGQA_SELFTEST_ROUTINE,
                    IMGQA_SELFTEST_TEST_RE, IMGQA_SELFTEST_TU_GLOB,
                    JARGON_BASELINE_COMMENT, JARGON_BASELINE_FILE,
                    JARGON_DISPOSITIONS, JARGON_DISPOSITION_REPLACE,
                    JARGON_GLOSSARY_FILE, JARGON_SCAN_DIRS, JARGON_SCAN_EXCLUDE,
                    JARGON_SCAN_FILES, JARGON_SCAN_SUFFIXES, JARGON_TABLE_HEAD,
                    JARGON_UPDATE_CMD,
                    MODERN_BANNED,
                    NEW_DELETE_EXEMPT, RETIRED_DOC_NAMES, ROOT,
                    RG_ENUM_VOCAB_ENUMS, RG_ENUM_VOCAB_SOURCE_FILE,
                    RG_ENUM_VOCAB_TABLE_FILE,
                    STL_GMF_EXCEPTIONS,
                    STL_HEADERS, USING_NAMESPACE_EXEMPT, VULKAN_HPP_INCLUDE_FILES,
                    ZERO_ENTT_MODULES)
from fs import (docs_markdown_files, doc_pointer_files, rgl_cppm_files,
                rgl_tree_files, read_text, rel_posix)
from models import Warning

MODULE_LINE_RE = re.compile(r"^\s*(?:export\s+)?module\s*([\w:]+)?\s*;")
IMPORT_RE = re.compile(r"^\s*import\s+([\w:]+)\s*;")
# 两组定界符各自闭合（不能用 \1 反引用：< 的闭合符是 >）
INCLUDE_RE = re.compile(r"^\s*#include\s*(?:<([^>]+)>|\"([^\"]+)\")")


class ModuleScan(NamedTuple):
    """parse_module_file 结果（GMF include / 声明后 include / partition 判定）。"""

    name: str | None
    imps: dict[str, int]
    gmf_incs: list[tuple[str, int, str]]
    post_incs: list[tuple[str, int, str]]
    module_line: int | None
    is_partition: bool


def parse_module_file(src: str) -> ModuleScan:
    """按行状态机解析：GMF = 裸 module; 之后、具名模块声明之前。

    post_incs = 具名模块声明之后的 #include（违规盲区，module_structure 消费）；
    is_partition = `module X:part;`（非 export module 主接口）。
    """
    name: str | None = None
    imps: dict[str, int] = {}
    gmf_incs: list[tuple[str, int, str]] = []
    post_incs: list[tuple[str, int, str]] = []
    in_gmf = named = is_partition = False
    module_line: int | None = None
    for i, ln in enumerate(src.splitlines(), 1):
        m = MODULE_LINE_RE.match(ln)
        if m:
            if m.group(1) is None:
                in_gmf = True
            else:
                name = m.group(1)
                in_gmf = False
                named = True
                module_line = i
                # 分区实现 = 裸 `module X:part;`；主接口 = `export module X[...]`
                is_partition = (":" in name) and not ln.lstrip().startswith("export")
            continue
        m = INCLUDE_RE.match(ln)
        if m and (in_gmf or (named and name is not None)):
            path = m.group(1) if m.group(1) is not None else m.group(2)
            delim = "<" if m.group(1) is not None else '"'
            if in_gmf:
                gmf_incs.append((path, i, delim))
            elif named:
                post_incs.append((path, i, delim))
        m = IMPORT_RE.match(ln)
        if m and m.group(1) not in imps:
            imps[m.group(1)] = i
    return ModuleScan(name, imps, gmf_incs, post_incs, module_line, is_partition)


def is_internal_header(rel: str, inc: str) -> bool:
    """引号 include 能否解析到仓库内文件 = 内部头（模块优先违规）。

    外部 SDK 头（如 Aftermath GFSDK_*.h，include 目录在仓库外）解析不到 → 不误报
    （引号 include 本身不违规，违规的是"内部头文件存在"——模块间只走 import）。
    """
    root = Path(ROOT).resolve()
    base = (Path(ROOT) / rel).parent
    for cand in (base / inc, Path(ROOT) / inc):
        p = cand.resolve()
        if p.is_file() and p.is_relative_to(root):
            return True
    return False


def text_checks() -> list[Warning]:
    """A3 分层依赖 + B1 模块优先 + B2 GMF STL 白名单 + 模块结构四条。"""
    warnings: list[Warning] = []
    mods: dict[str, tuple[str, dict[str, int], list[tuple[str, int, str]]]] = {}
    for rel in rgl_cppm_files():
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        scan = parse_module_file(src)
        if scan.name is None:
            continue
        assert scan.name is not None
        name, imps, gmf_incs = scan.name, scan.imps, scan.gmf_incs
        mods[name] = (rel, imps, gmf_incs)
        # B1 内部头 + B2 GMF STL 白名单（一趟扫 gmf_incs）
        for inc, i, delim in gmf_incs:
            if delim == '"' and is_internal_header(rel, inc):
                warnings.append(Warning(rel, i,
                    f'GMF #include "{inc}"——模块优先：模块间依赖只走 import，'
                    f"内部头文件不应存在"))
            elif (delim == "<" and "/" not in inc
                  and inc in STL_HEADERS and inc not in STL_GMF_EXCEPTIONS):
                warnings.append(Warning(rel, i,
                    f"GMF #include <{inc}>——STL 走 import rlstd"
                    f"（例外仅 ranges/filesystem/chrono；rlstd 缺名则补入 rlstd.cppm）"))
        # 模块结构：GMF 外 include + partition 四条（见 docs/风格检查规则.md §1）
        warnings.extend(_module_structure_checks(
            rel, name, imps, scan.post_incs, scan.module_line, scan.is_partition, src))
    # A3：底层零 entt + 点名禁止边
    for name, (rel, imps, gmf_incs) in mods.items():
        if name in ZERO_ENTT_MODULES:
            if "ecs" in imps:
                warnings.append(Warning(rel, imps["ecs"],
                    f"模块 '{name}' import ecs——底层渲染层不得依赖 ECS"
                    f"（entity/component/system 归场景层，见 AGENTS.md 分层）"))
            for inc, i, _ in gmf_incs:
                if inc.startswith("entt/"):
                    warnings.append(Warning(rel, i,
                        f"模块 '{name}' GMF #include <{inc}>"
                        f"——底层渲染层零 entt（见 AGENTS.md 分层）"))
        for target in sorted(FORBIDDEN_EDGES.get(name, ())):
            if target in imps:
                warnings.append(Warning(rel, imps[target],
                    f"模块 '{name}' import '{target}'——禁止依赖边"
                    f"（见 AGENTS.md 构造依赖图原则）"))
    # B1：rgl/ 下 .h/.hpp/.hh/.cpp 文件
    for rel in rgl_tree_files(".h", ".hpp", ".hh", ".cpp"):
        fn = Path(rel).name
        if fn.endswith((".h", ".hpp", ".hh")):
            warnings.append(Warning(rel, 0,
                "头文件——模块优先：所有新代码必须 C++20 modules (.cppm)"))
        elif fn.endswith(".cpp") and fn not in ALLOWED_CPP:
            warnings.append(Warning(rel, 0,
                ".cpp 文件——模块优先：仅单头库实现单元（tinygltf_impl/vam_impl）"
                "允许 .cpp"))
    return warnings


def _module_structure_checks(
    rel: str, name: str, imps: dict[str, int],
    post_incs: list[tuple[str, int, str]],
    module_line: int | None, is_partition: bool, src: str,
) -> list[Warning]:
    """模块结构四条（§1.1–1.4）：partition export / 主接口 import / GMF 外 include /
    vulkan.hpp 仅 rlvk。name 形如 `foo:impl`（partition）或 `foo`（主接口）。"""
    warnings: list[Warning] = []
    # §1.4 vulkan C++ 头仅 rlvk
    if rel not in VULKAN_HPP_INCLUDE_FILES:
        for inc, i, _delim in post_incs:
            if inc in ("vulkan/vulkan.hpp", "vulkan/vulkan_raii.hpp"):
                warnings.append(Warning(rel, i,
                    f"#include <{inc}>——Vulkan C++ 头一律 import rlvk"
                    f"（仅 rlvk.cppm 可 include hpp；C 头 vulkan.h 仍允许）"))
    # §1.3 具名模块声明之后的任何 #include（GMF 外）
    for inc, i, _delim in post_incs:
        warnings.append(Warning(rel, i,
            f"模块声明后 #include <{inc}>——GMF 外禁 #include；"
            f"第三方/STL 头放 GMF（裸 module; 之后、具名 module 之前）"))
    if not is_partition or module_line is None:
        return warnings
    # 主接口名 = partition 前缀（name = base:part）
    base = name.split(":", 1)[0]
    if base not in imps:
        warnings.append(Warning(rel, module_line,
            f"partition '{name}' 缺 import {base}——"
            f"clang 不给 partition impl 隐式 import 主接口，必须显式"))
    # §1.1 partition 实现单元禁 export 声明（export import/using 放行）
    for i, ln in enumerate(src.splitlines(), 1):
        if i <= module_line:
            continue
        if re.match(r"^export\s+", ln) and not re.match(
                r"^export\s+(import|using|module)\b", ln):
            warnings.append(Warning(rel, i,
                f"partition '{name}' 实体内带 export——"
                f"导出实体在实现单元定义不得带 export（接口已声明，三条坑 #3）"))
    return warnings


# ─── 现代 C++ 零违规回归护栏 + new/delete + using/export namespace ────────────

_MODERN_REGEXES = [(msg, re.compile(rx)) for rx, msg in MODERN_BANNED]
_NEW_RX = re.compile(r"\bnew\s+[A-Za-z_(]")
_DELETE_RX = re.compile(r"(?<![=\w])\bdelete\s+(?!default\b)")
_USING_NS_RX = re.compile(r"^using namespace\s+", re.M)
_EXPORT_NS_RX = re.compile(r"^export namespace\s+", re.M)


def modern_cpp_checks() -> list[Warning]:
    """§2.1–2.2：剥注释后 token 级现代 C++ 护栏 + new/delete 豁免 + namespace 豁免。"""
    warnings: list[Warning] = []
    for rel in rgl_cppm_files():
        if rel in EXCLUDE_FILES:
            continue
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        clean = strip_comments(src)
        lines = clean.splitlines()
        for lineno, line in enumerate(lines, 1):
            for msg, rx in _MODERN_REGEXES:
                if rx.search(line):
                    warnings.append(Warning(rel, lineno, msg))
        if rel not in NEW_DELETE_EXEMPT:
            for lineno, line in enumerate(lines, 1):
                if _NEW_RX.search(line):
                    warnings.append(Warning(rel, lineno,
                        "new 表达式——禁裸 new（分配器/C API 包装走豁免名单 "
                        "NEW_DELETE_EXEMPT；其余用 make_unique/make_shared/RAII）"))
                if _DELETE_RX.search(line):
                    warnings.append(Warning(rel, lineno,
                        "delete 表达式——禁裸 delete（= delete 特殊成员删除不命中；"
                        "分配器 deleter 走 NEW_DELETE_EXEMPT）"))
        if rel not in USING_NAMESPACE_EXEMPT:
            for m in _USING_NS_RX.finditer(clean):
                lineno = clean[:m.start()].count("\n") + 1
                warnings.append(Warning(rel, lineno,
                    "using namespace——仅入口 main_impl 豁免；模块内用限定名"))
        if rel not in EXPORT_NAMESPACE_EXEMPT:
            for m in _EXPORT_NS_RX.finditer(clean):
                lineno = clean[:m.start()].count("\n") + 1
                warnings.append(Warning(rel, lineno,
                    "export namespace——仅 rlvk 的 vk re-export 豁免"))
    return warnings


# ─── docs/** 状态头（§1.5）────────────────────────────────────────────────────

_DOCS_STATUS_RX = re.compile(r"^(?:>\s*)?\**状态")


def docs_status_checks() -> list[Warning]:
    """docs/（含 archive/）.md 前 DOCS_STATUS_MAX_LINES 行内须有状态行。
    接受 `> **状态**：` / `> 状态：` / 行首 `状态：`（含 `**状态（日期）**` 变体）。"""
    warnings: list[Warning] = []
    for rel in docs_markdown_files():
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        head = src.splitlines()[:DOCS_STATUS_MAX_LINES]
        if any(_DOCS_STATUS_RX.match(ln.strip()) for ln in head):
            continue
        warnings.append(Warning(rel, 1,
            f"缺状态行——docs 头部前 {DOCS_STATUS_MAX_LINES} 行须有 "
            f"`> **状态**：现行/历史/调研/…`（见 AGENTS.md 其他文档）"))
    return warnings


# ─── AGENTS.md 文档索引双向对账（§1.6）───────────────────────────────────────

# AGENTS 正文里指向 docs 的相对路径串（含中文名与 §引用前的路径）
_AGENTS_DOC_PATH_RX = re.compile(r"docs/[0-9A-Za-z_\u4e00-\u9fff./\-]*\.md")


def agents_doc_index_checks() -> list[Warning]:
    """AGENTS.md 写出的文档指针须**指向存在的路径**（单向反查）。

    🔴 **正查（「docs/ 下每篇都须出现在 AGENTS」）已于 2026-10-15 退役**，用户拍板：
    「AGENTS.md 不需要索引，索引在我脑子里。」正查的前提是 *AGENTS 是唯一开机路由表*，
    而这个前提被它的 owner 当面否证 ⟹ 继续跑它只会产出每次构建 24 条与真实工作无关的警告，
    而**脆护栏的实际效果是把人训练成忽略警告**（债-118 的同一取向：先有阳性锚点再谈入册，
    前提没了就直接撤）。撤的是判据，不是记忆——想查有哪些文档，`ls docs/` 就是真相源。

    **保留的半边**（反查，error）：`AGENTS.md` 里写出的 `docs/**.md` 磁盘上须存在——
    文档撒谎比缺文档更贵，这一条与"谁持有索引"无关，故不受正查退役影响。
    通配写法（`docs/archive/*_已删除.md`）不作单文件判定。

    AGENTS.md 读不到 → error 级**出声**而不是静默通过（"零警告"会被读成"指针干净"，
    同 rules_layers.py 失效也报 error 的取向）。
    """
    agents = read_text(Path(ROOT) / AGENTS_FILE)
    if agents is None:
        return [Warning(AGENTS_FILE, 1,
            f"读不到 {AGENTS_FILE}——文档指针对账**无法执行**（这不是「指针干净」）", "error")]
    warnings: list[Warning] = []
    for path in sorted(set(_AGENTS_DOC_PATH_RX.findall(agents))):
        if "*" in path or "~" in path:
            continue
        if not (Path(ROOT) / path).exists():
            warnings.append(Warning(AGENTS_FILE, 1,
                f"AGENTS 本节写出的 {path} 磁盘上不存在——悬空指针（先补目的地再修指针，"
                "见 skill rgl-doc-placement）", "error"))
    return warnings


# ─── 全仓文档指针有效性（§1.7）───────────────────────────────────────────────

# 仓库根起的项目目录 + 中文文件名 + §引用前的路径（同 §1.6 的形态，但扫全仓不止 AGENTS）
_DOC_PATH_RX = re.compile(
    r"(?<![\w/.-])((?:" + "|".join(re.escape(r) for r in DOC_POINTER_REF_ROOTS)
    + r")/[0-9A-Za-z_\u4e00-\u9fff./\-]*\.md)")


def doc_pointer_checks() -> list[Warning]:
    """全仓（含代码注释）写出的文档指针须**指向存在的路径**，且**不得指向退役文档**。

    §1.6 只管 AGENTS.md 一个文件；本条把同一判据铺到全仓，动机是 2026-10-08 删除全局
    调试时间线文档那类 MOVE：一份文档被 70+ 处引用（docs 与代码注释各占一半），删完文件
    留下满仓悬空指针 = 后来者按指针找不到东西，比从没有过指针更贵（会怀疑是自己没找到）。

    四条判定（均 error 级）：
    1. **悬空**：`<项目目录>/**.md` 磁盘上不存在。含通配（`*`）或 `~` 的写法跳过——
       那是模式不是路径（`docs/archive/*_已删除.md`）。
    2. **退役**：出现 `config.RETIRED_DOC_NAMES` 里的文件名（裸名或带目录都算）。
       删文档的复发形态是"从 git 捞回来重建一份"，只查悬空查不到（文件一回来指针就"有效"了），
       故退役名单单独点名，并在消息里给出**现在的归宿**。
    3. **反引号内的项目路径**（债-111）：`` `tools/x/` `` / `` `rgl/x.cppm` `` / `` `docs/x.md` ``
       这类形态磁盘上不存在。判据形态与**排除形态**见 `_bad_backtick_path`。
    4. **反引号内的命令名**（债-111）：`` `just <配方>` `` / `` `imgqa <子命令>` `` 的第二 token
       不在真相源（Justfile / imgqa 注册表）里。

    ③④ 的存量走**棘轮基线**（`config.DOC_POINTER_BASELINE_FILE`，机制先例 =
    `tool_baseline.py` / `byte_offset_baseline.json`）：只准降不准升，新增一条即红；
    🔴 基线不可用 / 扫描面为空 / 真相源解析出 0 条 ⟹ 一律 error 声明「本次未做任何对账」，
    绝不把"零警告"读成"指针都有效"（同 `rules_layers` / `rules_reflection` 的纪律）。

    扫描面/排除面见 `config.DOC_POINTER_*`（tools/checks/ 自身不扫：规则必然写出被禁名）。
    """
    warnings: list[Warning] = []
    files = doc_pointer_files()
    for rel in files:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        for name, where in RETIRED_DOC_NAMES.items():
            idx = src.find(name)
            if idx >= 0:
                warnings.append(Warning(rel, src[:idx].count("\n") + 1,
                    f"引用已退役文档 {name}——{where}", "error"))
        for m in _DOC_PATH_RX.finditer(src):
            path = m.group(1)
            if "*" in path or "~" in path:
                continue
            if not (Path(ROOT) / path).exists():
                warnings.append(Warning(rel, src[:m.start()].count("\n") + 1,
                    f"悬空文档指针 {path} 磁盘上不存在——先补目的地再修指针"
                    "（见 skill rgl-doc-placement）", "error"))
    warnings.extend(_backtick_pointer_checks(files))
    return warnings


# ─── 判定 ③④：反引号内的项目路径 / 命令名（债-111）────────────────────────────

# 反引号片段。单行、上限 120 字符：跨行反引号在本仓是排版事故，不是指针。
_BACKTICK_RX = re.compile(r"`([^`\n]{1,120})`")
# 出现即**不是路径**的字符：通配与占位 ⟹ 那是模式（`docs/*.md`、`rgl/scene_config*.cppm`、
# `third_party/{meshoptimizer,vcpkg-ports}/`、`build/rgl/*`），按存在性判必误报。
_MODE_CHARS = frozenset("*?[]{}~<>$%…")
# `x.cppm:12-30` / `x.cppm::Sym` / `docs/x.md §段` —— 行号 / 符号 / 段号后缀不是路径的一部分。
# 🔴 只截到路径为止：**符号存在性是台账明写的第二步**（要 grep/ctags 兜底，误报面大）。
_SUFFIX_RX = re.compile(r"::|[:§]")
# 判定 ④ 的真相源解析：Justfile 配方行（`name:` / `name ARGS="x":`，**不含** `VAR := …` 变量行）
# 与 imgqa 注册表条目名（`SubcommandSpec{ "名字"`，权威清单 = cli_table.cpp 本体）。
# ⚠ 变量行的排除**不能**写成 `[^:=]*`（那是"既不含 : 也不含 ="，`test *ARGS=""` 里的 = 会把
# 整条配方判没，实测 44 条只解析出 18 条 ⟹ 真配方被报成"没有这一条"= 规则说谎）。
_JUST_RECIPE_RX = re.compile(
    r"(?m)^(?!@?[A-Za-z_][A-Za-z0-9_-]*\s*[:?+]?=)(@?[A-Za-z_][A-Za-z0-9_-]*)(?:\s.*)?:\s*$")
_IMGQA_SUB_RX = re.compile(r'SubcommandSpec\{\s*"([a-z][a-z0-9_-]*)"')
# 反引号里写成命令时**不算子命令名**的第二 token：flag、可选参数、交替简写、占位
_CMD_PLACEHOLDER = ("-", "[", "<", "(", '"', "“", "…", "|", "*", "$")

# 最近一次反引号指针对账的覆盖面读数（取向同 rules_reflection.SCAN：一条"什么都没抓到"的
# 规则与一条"扫错了目录"的规则，输出必须不同）。
SCAN_BACKTICK: dict[str, int] = {}
# 最近一次执行逐文件命中的条数（`--update-doc-pointer-baseline` 收账时的输入）
LAST_BACKTICK_COUNTS: dict[str, int] = {}


def doc_pointer_readout() -> str:
    """最近一次反引号指针对账的覆盖面读数；没跑过 = 空串（调用方据此判规则有没有跑）。"""
    if not SCAN_BACKTICK:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN_BACKTICK.items())


def doc_pointer_counts() -> dict[str, int]:
    """最近一次执行逐文件命中的条数（收账命令的输入；未跑过 = 空表）。"""
    return dict(LAST_BACKTICK_COUNTS)


def _basename_index() -> set[str]:
    """裸名判据的存在性索引 = 各仓库根目录下所有文件的基名（不含 build/ 与 .git/）。

    裸名指针（`` `render_graph.cppm` ``）写的时候不带目录，存在性只能问"全仓有没有这个文件"；
    索引面 = `config.DOC_POINTER_PATH_ROOTS` **再加仓库根散文件**（实测踩过两次：漏了 `logger/`
    把真存在的 `rllogger.cppm` 判成悬空、漏了仓库根把 `AGENTS.md` 判成悬空 ×27）。
    """
    out: set[str] = {p.name for p in Path(ROOT).iterdir() if p.is_file()}
    for d in DOC_POINTER_PATH_ROOTS:
        base = Path(ROOT) / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.is_file():
                out.add(p.name)
    return out


def _bad_backtick_path(span: str, names: set[str]) -> tuple[bool, str | None]:
    """反引号片段 → (是不是**路径候选**, 磁盘上不存在的判定串或 None)。

    反引号里绝大多数内容不是路径（实测 24933 个片段里只有约 1500 个像路径），判据必须收窄。
    **排除形态**（逐条都有实测误报作依据，别当保守删掉）：

    - 含空白 / `=`：命令行与配置键（`just reflect`、`aa = "off"`、`shadow_lod = -1`）；
    - 含通配与占位（`_MODE_CHARS`）：`docs/*.md`、`aa_coverage.{h,cpp}`、`<项目目录>`；
    - 含 `xxx` / `yyy`：示例占位（`xxx_impl.cppm`）；
    - 斜杠路径但首段不是项目根：外部与系统路径（`vulkan/vulkan.hpp`、`glm/glm.hpp`、
      `github.com/…`、`~/…`、`C:/…`），以及 `build/…`（构建产物随机器漂，不是源码指针）；
    - 斜杠路径的中间段带扩展名：`rgl/diagnostics.cppm/_impl.cppm` 是「主接口 + 实现单元」
      **两个文件**的简写，不是路径（本仓 7 处，全在 archive）；
    - 裸名但扩展名不是项目自有（`.h` / `.hpp` / `.cpp` / `.glsl` / `.json`）：本仓大量按裸名
      引用**上游与 SDK 文件**（`ShadowMap.cpp` = Filament、`vulkan_core.h` = SDK、
      `callbacks_glsl.h` = FSR vendor），磁盘上本来就没有 ⟹ 判悬空是纯误报，且没有
      "上游文件白名单"这种不漂的真相源可写；
    - 裸名以 `_` / `.` 开头：那是**命名模式**不是文件（`_impl.cppm`、`_schema.lua`、
      `.manifest.json`、`.mesh.slang`）。
    """
    s = span.strip()
    if not s or any(c in s for c in _MODE_CHARS) or "=" in s:
        return False, None
    if any(c.isspace() for c in s):
        return False, None
    s = _SUFFIX_RX.split(s, maxsplit=1)[0]
    if not s or "xxx" in s.lower() or "yyy" in s.lower():
        return False, None
    if "/" in s:
        segs = s.split("/")
        if any("." in seg for seg in segs[:-1]):
            return False, None
        if segs[0] not in DOC_POINTER_PATH_ROOTS:
            return False, None
        if (Path(ROOT) / s).exists():
            return True, None
        # 无扩展名的文件指针：`tests/test_scene_config` 指的就是 test_scene_config.cppm
        return (True, None) if any(Path(ROOT, s + e).exists()
                                   for e in DOC_POINTER_PATH_EXTS) else (True, s)
    if s.startswith(("_", ".")) or Path(s).suffix.lower() not in DOC_POINTER_BARE_EXTS:
        return False, None
    return (True, None) if s in names else (True, s)


def _bad_backtick_command(span: str, recipes: set[str], subs: set[str]) -> tuple[bool, str | None]:
    """反引号片段 → (是不是**命令候选**, 不在真相源里的判定串或 None)。

    只做 `just X` 与 `imgqa X` 两条：它们有**机器可读的真相源**（Justfile 配方行 /
    imgqa 注册表本体），零歧义。不做"任意命令名存在性"——实测含空格的反引号片段 2872 处、
    首 token 755 种，绝大多数是散文/公式/代码片段（`if (flag)`、`gate = saturate(2 − motion)`、
    `base = ti*3`），没有不漂的外部命令白名单可写，硬做就把护栏变成没人敢写反引号的墙。
    """
    s = span.strip()
    for tool, truth, label in (("just", recipes, "Justfile 配方"),
                               ("imgqa", subs, "imgqa 子命令注册表")):
        if s != tool and not s.startswith(tool + " "):
            continue
        rest = s[len(tool):].strip().split()
        if not rest:
            return False, None
        tok = rest[0]
        # `just cpu [--arms …]` / `just regress ab|floor|burst` / `just run-*` / `imgqa <子命令>`
        # —— 参数位上的占位与交替简写不是配方名
        if tok.startswith(_CMD_PLACEHOLDER) or any(c in tok for c in "*|…()<>"):
            return False, None
        return (True, None) if tok in truth else (
            True, f"{tool} {tok}（{label}里没有这一条）")
    return False, None


def load_doc_pointer_baseline() -> tuple[dict[str, int], str | None]:
    """读反引号指针棘轮基线 → ({rel: 允许条数}, 不可用原因)。

    🔴 与 `tool_baseline.load_baseline`（缺失 → 空表按未登记）**故意不同**：这里的空表会被
    下游读成「零悬空指针」，正是"失效不出声 = 假绿"。缺失 / 坏 JSON / 格式不符一律返回原因串
    （先例 = `check_shader_deps.py::load_byte_offset_baseline`）。
    """
    path = Path(ROOT) / DOC_POINTER_BASELINE_FILE
    if not path.is_file():
        return {}, f"基线文件不存在：{DOC_POINTER_BASELINE_FILE}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {}, f"基线文件读不出来：{DOC_POINTER_BASELINE_FILE}（{type(e).__name__}: {e}）"
    if not isinstance(data, dict):
        return {}, f"基线顶层不是 JSON 对象：{DOC_POINTER_BASELINE_FILE}"
    files = data.get("files")
    if not isinstance(files, dict):
        return {}, f"基线缺 `files` 对象（格式不符）：{DOC_POINTER_BASELINE_FILE}"
    bad = sorted(str(k) for k, v in files.items()
                 if isinstance(v, bool) or not isinstance(v, int))
    if bad:
        return {}, f"基线 `files` 有非整数条数（格式不符）：{DOC_POINTER_BASELINE_FILE} 的 {bad[:3]}"
    return {str(k): int(v) for k, v in files.items()}, None


def write_doc_pointer_baseline(counts: dict[str, int]) -> str:
    """按当前条数重写棘轮基线（收账）；返回写入路径。"""
    files = {k: int(v) for k, v in sorted(counts.items())}
    path = Path(ROOT) / DOC_POINTER_BASELINE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(
        {"_comment": DOC_POINTER_BASELINE_COMMENT,
         "total": sum(files.values()), "files": files},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return DOC_POINTER_BASELINE_FILE


def _truth_source(pattern: str, rx: re.Pattern[str], label: str) -> tuple[set[str], str | None]:
    """解析一个真相源（Justfile 配方 / imgqa 子命令表，路径可 glob）→ (名字集合, 失效原因)。

    解析出 **0 条**（或 glob 打不到文件）= 真相源格式变了而本规则没跟上 ⟹ 判定 ④ 空跑。
    空跑必须出声（同 `meshlet_spec_checks` 的「constants 块解析不到」纪律），绝不把
    "零命中"读成"全对"。
    """
    files = sorted(Path(ROOT).glob(pattern))
    if not files:
        return set(), f"真相源 {pattern} 一个文件都没找到（改名了？）"
    names: set[str] = set()
    for f in files:
        src = read_text(f)
        if src is None:
            return set(), f"真相源 {rel_posix(f)} 读不到"
        names |= {n.lstrip("@") for n in rx.findall(src)}
    if not names:
        return set(), f"真相源 {pattern} 解析出 0 条{label}（格式变了？同步改正则，别让判定 ④ 空跑）"
    return names, None


def _backtick_pointer_checks(files: list[str]) -> list[Warning]:
    """判定 ③④ 的驱动：扫反引号片段 → 逐文件计数 → 与棘轮基线比对。"""
    warnings: list[Warning] = []
    SCAN_BACKTICK.clear()
    if not files:
        SCAN_BACKTICK["文件"] = 0
        return [Warning("tools/checks/config.py", 1,
            "反引号指针对账（§1.7 判定 ③④）**本次未做任何对账**：扫描面为空"
            "（doc_pointer_files() 返回 0 个文件）——「零警告」不是「指针都有效」", "error")]

    recipes, bad_just = _truth_source(DOC_POINTER_JUSTFILE, _JUST_RECIPE_RX, "配方")
    subs, bad_sub = _truth_source(DOC_POINTER_IMGQA_CLI_TABLE, _IMGQA_SUB_RX, "子命令")
    # 真相源坏了只停判定 ④（路径判定照跑），并在下面出声——不许静默把 ④ 读成"已通过"
    cmd_off = bool(bad_just or bad_sub)
    names = _basename_index()

    counts: dict[str, int] = {}
    hits: dict[str, list[tuple[int, str]]] = {}
    spans = paths = cmds = 0
    for rel in files:
        if rel in DOC_POINTER_EXAMPLE_FILES:
            continue  # 规则全景文档必须原样写出被禁形态（见 config 注释），①② 仍管它
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        for m in _BACKTICK_RX.finditer(src):
            spans += 1
            span = m.group(1)
            line = src[:m.start()].count("\n") + 1
            cand, bad = _bad_backtick_path(span, names)
            kind = "项目路径"
            if not cand and not cmd_off:
                cand, bad = _bad_backtick_command(span, recipes, subs)
                kind = "命令"
            if not cand:
                continue
            if kind == "项目路径":
                paths += 1
            else:
                cmds += 1
            if bad is None:
                continue
            why = (f"反引号里的{kind} {bad} 磁盘上不存在——凭记忆写的指针（先 ls / grep 再写）"
                   if kind == "项目路径" else
                   f"反引号里的命令 {bad}——凭记忆写的子命令/配方名")
            counts[rel] = counts.get(rel, 0) + 1
            hits.setdefault(rel, []).append((line, why))

    base_total = sum(counts.values())
    LAST_BACKTICK_COUNTS.clear()
    LAST_BACKTICK_COUNTS.update(counts)
    baseline, bad_base = load_doc_pointer_baseline()
    SCAN_BACKTICK.update({"文件": len(files) - len(DOC_POINTER_EXAMPLE_FILES),
                          "反例豁免": len(DOC_POINTER_EXAMPLE_FILES),
                          "反引号": spans, "路径候选": paths,
                          "命令候选": cmds, "命中": base_total,
                          "基线": sum(baseline.values()) if not bad_base else 0})
    # 🔴 三条"规则空转"失效：扫描面非空却一个反引号都没抓到（正则坏了）、真相源解析出 0 条、
    # 基线不可用。任何一种都不许把"零警告"读成"已对账"。
    if not spans:
        warnings.append(Warning("tools/checks/rules_text.py", 1,
            f"反引号指针对账**本次未做任何对账**：扫了 {len(files)} 个文件却一个反引号片段"
            "都没抓到（_BACKTICK_RX 坏了？）", "error"))
    for bad in (bad_just, bad_sub):
        if bad:
            warnings.append(Warning("tools/checks/rules_text.py", 1,
                f"判定 ④（命令名）**本次未做任何对账**：{bad}", "error"))
    if not names:
        warnings.append(Warning("tools/checks/config.py", 1,
            "判定 ③（裸名指针）**本次未做任何对账**：基名索引为空"
            "（DOC_POINTER_PATH_ROOTS 一个目录都打不到？），别把「零命中」读成「裸名都对」",
            "error"))
    if bad_base:
        warnings.append(Warning(DOC_POINTER_BASELINE_FILE, 1,
            f"反引号指针棘轮**本次未做任何对账**：{bad_base}（当前命中 {base_total} 条未比对）"
            "——先修基线文件，别把「没数」读成「零悬空指针」", "error"))
        return warnings

    for rel in sorted(set(counts) | set(baseline)):
        cur, base = counts.get(rel, 0), baseline.get(rel, 0)
        if rel not in counts:
            if base > 0:
                warnings.append(Warning(rel, 0,
                    f"反引号指针基线仍记 {base} 条但该文件已不在扫描面上（或已清零）——"
                    f"用 `{DOC_POINTER_UPDATE_CMD}` 收账"))
            continue
        if cur > base:  # 未登记按 base=0 处理，同样进这一支
            for line, why in hits[rel]:
                warnings.append(Warning(rel, line, why, "error"))
            if rel not in baseline:
                warnings.append(Warning(rel, 0,
                    f"反引号指针基线未登记：当前 {cur} 条（新文件要显式登记进 "
                    f"{DOC_POINTER_BASELINE_FILE}——但**先确认**这些指针本该存在，"
                    "新增一条的处置是改指针，不是来这里登记额度）", "error"))
            else:
                warnings.append(Warning(rel, 0,
                    f"反引号指针基线上升 {cur} > {base}——处置是把凭记忆写的指针改成真实存在的"
                    f"名字，**不是**改基线（基线只准降；确实清了才用 `{DOC_POINTER_UPDATE_CMD}`）",
                    "error"))
        elif cur < base:
            warnings.append(Warning(rel, 0,
                f"反引号指针可下调 {cur} < {base}——用 `{DOC_POINTER_UPDATE_CMD}` 收账"
                "（悄悄漂移等于没有基线）"))
    return warnings



# ─── 判据契约表的「出处指针」有效性（§1.8）────────────────────────────────────

# 出处指针的形态：`tools/imgqa/<文件>` 紧跟一个反引号标识符或「段名」——判据表把它当字符串
# 抄在「钉住：…」里，两种锚点写法都要查（`cmdTemporalTailCamera` 与「判决线」是同一类指针）。
_JUDGMENTS_POINTER_RX = re.compile(
    r"tools/imgqa/([a-z_0-9]+\.(?:cpp|h))\s*(?:`([^`]{1,60})`|「([^」]{2,40})」)")


def judgments_pointer_checks() -> list[Warning]:
    """判据契约表里的 **文件 + 锚点** 出处指针，必须指向该文件里**真实存在**的符号或段名。

    动机（2026-10-11，imgqa 拆 TU 那批）：判据表把"这条判据的口径写在哪个文件的哪段注释"
    抄成字符串，`imgqa check` 原样打印。拆文件把实现搬走后，**文件还在、锚点没了**——
    现有 §1.7 只查 `.md` 路径存在性，管不到这种"指针不悬空但翻空"的失效；实测一次拆文件
    就留下 18 处失效（全在 judgments_table_tail.cpp），闸门全程绿。

    两条判定（均 error 级）：
    1. 指向的 `tools/imgqa/*.{cpp,h}` 磁盘上不存在；
    2. 锚点（剥掉 `()` 与 `::` 右半后）不在该文件文本里 —— 消息里直接给出**当前真正含该
       锚点的文件**，改起来是一次复制不是一次搜索。

    范围只到 `tools/imgqa/`：判据表的职责就是钉住 imgqa 自己的仪器口径。
    """
    warnings: list[Warning] = []
    root = Path(ROOT)
    imgqa = root / "tools/imgqa"
    corpus = {p.name: (read_text(p) or "")
              for p in sorted(imgqa.glob("*.cpp")) + sorted(imgqa.glob("*.h"))}
    for tbl in sorted(imgqa.glob(JUDGMENTS_TABLE_GLOB)):
        src = read_text(tbl) or ""
        rel = rel_posix(tbl)
        for m in _JUDGMENTS_POINTER_RX.finditer(src):
            target, ref = m.group(1), (m.group(2) or m.group(3))
            line = src[:m.start()].count("\n") + 1
            if target not in corpus:
                warnings.append(Warning(rel, line,
                    f"判据出处指针 {target} 磁盘上不存在——文件已改名或删除，改判据表", "error"))
                continue
            key = ref.replace("()", "").split("::")[0]
            if key in corpus[target]:
                continue
            here = sorted(n for n, t in corpus.items() if key in t)
            warnings.append(Warning(rel, line,
                f"判据出处指针 {target} 里没有 `{ref}`——锚点已搬走，现在在 "
                + ("、".join(here[:3]) if here else "全仓找不到")
                + "（拆文件要连着改判据表，否则 imgqa check 打印的出处翻空）", "error"))
    return warnings


# ─── 纯文本：禁用旧式 Vulkan API（回归护栏）──────────────────────────────────

def strip_comments(src: str) -> str:
    """剥离 // 行注释与 /* */ 块注释（字符置空格、换行保留——行号不漂移）。

    字符串/字符字面量内的 // 与 /* 不进注释态（match 单遍状态机）；
    raw string R"(...)" 不特殊处理——当前 rgl/*.cppm 无出现，若未来引入
    含 vk 旧 API 字样的 shader 内嵌源再补。
    """
    out: list[str] = []
    i, n = 0, len(src)
    state = 0  # 0=code 1=line-comment 2=block-comment 3=string 4=char
    while i < n:
        c = src[i]
        d = src[i + 1] if i + 1 < n else ""
        match state:
            case 0:
                if c == "/" and d == "/":
                    state = 1
                    out.append("  ")
                    i += 2
                    continue
                if c == "/" and d == "*":
                    state = 2
                    out.append("  ")
                    i += 2
                    continue
                if c == '"':
                    state = 3
                elif c == "'":
                    state = 4
                out.append(c)
                i += 1
            case 1:
                if c == "\n":
                    state = 0
                    out.append(c)
                else:
                    out.append(" ")
                i += 1
            case 2:
                if c == "*" and d == "/":
                    state = 0
                    out.append("  ")
                    i += 2
                else:
                    out.append("\n" if c == "\n" else " ")
                    i += 1
            case 3:
                if c == "\\":
                    out.append(c + d)
                    i += 2
                    continue
                if c == '"':
                    state = 0
                out.append(c)
                i += 1
            case 4:
                if c == "\\":
                    out.append(c + d)
                    i += 2
                    continue
                if c == "'":
                    state = 0
                out.append(c)
                i += 1
    return "".join(out)


def _build_forbidden_regexes() -> list[tuple[str, re.Pattern[str]]]:
    """token → 带标识符边界的正则（预编译一次）。

    边界是护栏必需（实测）：裸子串匹配会让 bindPipeline 命中项目自己的现代接口
    bindPipelineToAsset（ShaderRegistry，AGENTS.md 现行 API）。边界规则：
    - 左右邻字符不得是 [A-Za-z0-9_]（'.'/'(' ':' 空格 均放行——vk::X 与 cmd->X()
      调用式照抓）；
    - 以 '(' 结尾的 token（pushConstants(）本身即调用签名，只加左边界；
    - 以 '*' 结尾的 token = 显式前缀匹配（去 '*'、**不**加右边界）：结构体族
      （VkVertexInput*）与 sync2 变体（vkCmdBindVertexBuffers2）用，名单里
      只给确定封死的全族（见 config.FORBIDDEN_VULKAN_APIS 注释，防误伤现代名）。
    精确串互不包含现代名（VkDescriptorSetAndBindingMappingEXT 等均不命中）。"""
    out: list[tuple[str, re.Pattern[str]]] = []
    for tok in FORBIDDEN_VULKAN_APIS:
        if tok.endswith("*"):
            out.append((tok, re.compile(r"(?<![A-Za-z0-9_])" + re.escape(tok[:-1]))))
            continue
        esc = re.escape(tok)
        right = "" if not (tok[-1].isalnum() or tok[-1] == "_") else r"(?![A-Za-z0-9_])"
        out.append((tok, re.compile(r"(?<![A-Za-z0-9_])" + esc + right)))
    return out


FORBIDDEN_REGEXES = _build_forbidden_regexes()


def forbidden_vulkan_checks() -> list[Warning]:
    """禁用旧式 Vulkan API 扫描：全部 rgl/*.cppm（排除 rlvk 包装层与 generated/），
    剥注释后按行匹配名单。"""
    warnings: list[Warning] = []
    for rel in rgl_cppm_files():
        if rel in EXCLUDE_FILES:
            continue
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        for lineno, line in enumerate(strip_comments(src).splitlines(), 1):
            for tok, rx in FORBIDDEN_REGEXES:
                if rx.search(line):
                    warnings.append(Warning(rel, lineno, f"{tok}——{FORBIDDEN_VULKAN_MSG}"))
    return warnings


# ─── 纯文本：.slang ByteAddressBuffer 位转纪律（回归护栏）────────────────────

# 文件内声明的字节寻址缓冲变量名（Load/Store 返回 uint 位模式）
_BAB_DECL_RX = re.compile(r"\b(?:RW)?ByteAddressBuffer\s+(\w+)\s*;")
# floatN 名 = ... buf.Load...( 且整行无 asfloat —— 裸赋值 = slang 生成
# ConvertUToF（数值转换非位转），静默出错（2026-10-02 IZB「门开无影」根因①）
_BAB_BARE_LOAD_RX_TMPL = r"\bfloat[234]?\s+\w+\s*=\s*[^;]*\b{name}\.Load[234]?\s*\("


# ─── 纯文本：meshlet 路径规格三方对账（schema ↔ shader ↔ 关系式）─────────────

# mesh stage 的线程数声明：`[numthreads(N, 1, 1)]`，N 允许是字面量或本文件/schema 的常量名
# （★M-3T-b 起两枚 meshlet mesh shader 都写常量名，见各自文件内注释）
_MESH_NUMTHREADS_RX = re.compile(r"\[\s*numthreads\s*\(\s*([A-Za-z_]\w*|\d+)\s*,")
# 文件内的 `const uint 名 = N u?;` 声明（用于把 numthreads 里的标识符还原成数值）
_MESH_CONST_DECL_RX = re.compile(r"\bconst\s+uint\s+(\w+)\s*=\s*(\d+)u?\s*;")


def meshlet_spec_checks() -> list[Warning]:
    """meshlet 路径规格：schema 常量 ↔ .slang 字面量 ↔ 关系式，三方对账。

    ## 为什么反射不够、必须有这条
    `tools/generator.cppm` 对 constants 的原话是「反射端无对应数据，直接由 schema 声明生成」
    ⟹ schema 里的 `constants` **反射不校验**，它只是 C++ 侧的单一声明点。而 meshlet 这批数
    （每任务三角数 85、簇上限 128、段步长 44/48）真正的消费者是**着色器字面量**与
    `render_list_impl.cppm` 的 task 数——schema 漂了反射一声不吭。本规则把"schema 是真相源"
    这句话变成构建期可执行的。

    ## 四段判据（缺一段就有一类漂移隐身）
    1. **数值对账**：schema 常量名若在任一 `.slang` 里以 `const uint <名> = <值>u;` 出现，
       值必须相等（改了 shader 没改 schema、或反之，都红）。
    2. 🔴 **关系对账 ①**（比数值更要命）：`3 × kTaskTris ≤ kMeshletMaxOutputVertices`。
       去索引发射下每任务顶点数 = 3×三角数；这条破了 = mesh shader **越界写顶点输出**，
       不报错、只画垃圾或崩。数值各自"看着都对"而关系破了是真实形态（85→86 就是）。
    3. 🔴 **关系对账 ②**（★M-3T-b 的再武装路径，与关系对账 ① 同族）：任一 `rgl/shaders/*.mesh.slang`
       的 `[numthreads(N, …)]` 之 **N ≤ `kMeshletMaxWorkGroupInvocations`**（= 我们支持的
       设备下限，三机最小值 = NVIDIA 的 128）。
       **为什么规则 1 管不到这一半**：规则 1 是「出现才对账」——它只在 `.slang` 里真出现
       `const uint <schema 常量名> = <值>u;` 时才生效。哪天有人把那个常量删掉、`numthreads`
       直接写回 `256`，规则 1 一声不响；规则 4（死常量）也救不了，因为
       `main_meshlet_tables.cppm::checkMeshletDeviceFit` 永远是该常量的 C++ 侧消费者、它恒
       "被使用" ⟹ 构建全绿，而 NV 上 `vkCreateShadersEXT` 启动就撞
       VUID-RuntimeSpirv-MeshEXT-07295/07298（本批修的就是这个 bug）。本条把**声明的线程数
       本身**纳入射程。取 ≤ 而非 =：其余 mesh shader 本来就远低于上限（`cube` / `model` /
       `shadow` / `izb_mask` = 32，`light` = 3；`skybox` 已随刀②.2 删除），它们不需要恰好等于。
       🔴 **失效必须出声**（同 `rules_layers` 的纪律：失效不出声，"零警告"会被读成"分层干净"）：
       schema 常量缺失、某枚 `.mesh.slang` 解析不到 `[numthreads(…)]`、线程数是个还原不出数值
       的标识符 ⟹ 三种情形都出警告，不许静默跳过本条。
    4. **死常量**：schema 常量既不出现在任何 `.slang`、也不被任何非生成 `.cppm` 使用
       = 本表已漂成小说（生成物 `rgl/generated/` 排除，否则它永远"被使用"）。

    ## 本规则**管不到**的那一半（别把它当设备安全保证）
    `kMeshletMaxOutputVertices = 256` 是**本机实测下限的钉值**，不是设备保证。引擎至今
    **从不运行期查询** `maxMeshOutputVertices`（全仓只出现在注释里）⟹ 换台更低的机器，
    本规则照样全绿而渲染已经坏了。运行期核对 + 不够就回落旧路径 = 挂号项
    （`docs/mdi_plan.md` §17.4）。
    """
    warnings: list[Warning] = []
    schema_rel = "rgl/shaders/meshlet_cull_schema.lua"
    schema_src = read_text(Path(ROOT) / schema_rel)
    if schema_src is None:
        return warnings  # 文件不在 = 别的规则会点名，这里不重复出声

    # ── 解析 schema constants（stdlib-only，不引 lua 解析依赖）──
    const_rx = re.compile(r'\{\s*name\s*=\s*"([A-Za-z_]\w*)"\s*,\s*value\s*=\s*(\d+)\s*\}')
    schema_consts: dict[str, int] = {}
    block = re.search(r"constants\s*=\s*\{(?P<body>.*?)\n    \},", schema_src, re.S)
    if not block:
        # 🔴 失效出声：constants 块整块解析不到 = 本函数四条判据全部空跑（格式变了没同步本规则）。
        # 静默 return 会让"零警告"被读成"规格自洽"（同 rules_layers 的失效必出声纪律）。
        warnings.append(Warning(schema_rel, 1,
            "meshlet schema 的 constants 块整块解析不到（格式变了？同步改 "
            "tools/checks/rules_text.py::meshlet_spec_checks，别让它静默全绿）"))
        return warnings
    for name, value in const_rx.findall(block.group("body")):
        schema_consts[name] = int(value)
    if not schema_consts:
        warnings.append(Warning(schema_rel, 1,
            "meshlet schema 的 constants 块解析出 0 条（格式变了？同步改 "
            "tools/checks/rules_text.py::meshlet_spec_checks，别让它静默全绿）"))
        return warnings

    # ── 收集 .slang 里的同名字面量声明 ──
    decl_rx = {name: re.compile(rf"\bconst\s+uint\s+{name}\s*=\s*(\d+)u?\s*;")
               for name in schema_consts}
    cppm_text: list[tuple[str, str]] = []
    for rel in rgl_tree_files(".cppm"):
        if rel.startswith("rgl/generated/"):
            continue
        text = read_text(Path(ROOT) / rel)
        if text is not None:
            cppm_text.append((rel, text))

    used: set[str] = set()
    for rel in rgl_tree_files(".slang"):
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        stripped = strip_comments(src)
        for name, rx in decl_rx.items():
            for lineno, line in enumerate(stripped.splitlines(), 1):
                hit = rx.search(line)
                if not hit:
                    continue
                used.add(name)
                if int(hit.group(1)) != schema_consts[name]:
                    warnings.append(Warning(rel, lineno,
                        f"meshlet 规格漂移：着色器字面量 {name} = {hit.group(1)}，而 "
                        f"{schema_rel} 的 constants 声明 {name} = {schema_consts[name]}"
                        "（两处必须同改；本表是 meshlet 路径规格的唯一声明点）"))

    for rel, text in cppm_text:
        for name in schema_consts:
            if re.search(rf"\b{name}\b", text):
                used.add(name)

    for name, value in schema_consts.items():
        if name not in used:
            warnings.append(Warning(schema_rel, 1,
                f"meshlet 规格常量 {name} = {value} 无任何消费者（.slang 与手写 .cppm 都不用它）"
                "—— 要么接上消费者，要么删掉；留着会让本表慢慢变成不被校验的散文"))

    # ── 关系对账：去索引发射的硬约束 ──
    tris = schema_consts.get("kTaskTris")
    max_out = schema_consts.get("kMeshletMaxOutputVertices")
    if tris is not None and max_out is not None and 3 * tris > max_out:
        warnings.append(Warning(schema_rel, 1,
            f"meshlet 发射规格破了硬约束：去索引下每任务顶点数 = 3 × kTaskTris = {3 * tris}"
            f" > kMeshletMaxOutputVertices = {max_out} ⟹ mesh shader 越界写顶点输出"
            "（不报错，只画垃圾/崩）。改 kTaskTris 必须同时确认设备上限"))

    # ── 关系对账 ②：mesh stage 声明的线程数 ≤ 设备下限（判据与"为什么规则 1 不够"见 docstring）──
    max_wgi = schema_consts.get("kMeshletMaxWorkGroupInvocations")
    if max_wgi is None:
        warnings.append(Warning(schema_rel, 1,
            "meshlet schema 缺常量 kMeshletMaxWorkGroupInvocations ⟹ 「mesh 线程数 ≤ 设备下限」"
            f"这条对账无上限可比（本批刚为它加判据，别让它空跑）：{schema_rel} 的 constants 补回来"))
        return warnings
    for rel in rgl_tree_files(".slang"):
        if not rel.endswith(".mesh.slang"):
            continue
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        stripped = strip_comments(src)  # 注释里的 numthreads 字样不算声明
        hit = _MESH_NUMTHREADS_RX.search(stripped)
        if not hit:
            warnings.append(Warning(rel, 1,
                "mesh shader 解析不到 `[numthreads(N, …)]`（写法变了？同步改 "
                "tools/checks/rules_text.py::meshlet_spec_checks 的 _MESH_NUMTHREADS_RX）"
                "—— 本条线程数上限判据对它是空跑，别把这种空跑读成通过"))
            continue
        token, lineno = hit.group(1), stripped[:hit.start()].count("\n") + 1
        n: int | None = None
        if token.isdigit():
            n = int(token)
        elif token in schema_consts:
            n = schema_consts[token]
        else:
            # 常量名 → 本文件的 `const uint 名 = Nu;`（两枚 meshlet mesh shader 的形态：
            # 文件级 static const uint 拼写 schema 常量，值由规则 1 另行钉成相等）
            n = {name: int(v) for name, v in _MESH_CONST_DECL_RX.findall(stripped)}.get(token)
        if n is None:
            warnings.append(Warning(rel, lineno,
                f"[numthreads({token}, …)] 的线程数还原不出数值：既不是字面量，也不是 schema 常量，"
                "本文件内也没有 `const uint " + token + " = <值>u;` 声明 ⟹ 本条判据空跑"))
            continue
        if n > max_wgi:
            warnings.append(Warning(rel, lineno,
                f"mesh shader 声明的线程数 {n} > kMeshletMaxWorkGroupInvocations = {max_wgi}"
                f"（{schema_rel}，= 我们支持的设备下限，三机最小值 = NVIDIA 的 maxMeshWorkGroupInvocations）"
                " ⟹ NV 上 vkCreateShadersEXT 撞 VUID-RuntimeSpirv-MeshEXT-07295/07298，"
                "shader 根本建不出来。要么降线程数并循环化覆盖输出（见 meshlet.mesh.slang 的 strided 循环），"
                "要么先重测三机能力矩阵再动 schema"))
    return warnings


def slang_load_bitcast_checks() -> list[Warning]:
    """rgl/shaders/*.slang：floatN 直接 = ByteAddressBuffer.Load…（无 asfloat）→ 警告。

    缓冲名 = **全仓 .slang 声明的并集**（sceneData 声明在 scene.slang、经 import
    跨文件使用——只收本文件声明会漏掉主犯，首版实测踩过）。同名异型（如某文件把
    同名声明成 Texture2D）理论上会误报，全仓现状无此撞名；撞了再收紧。
    StructuredBuffer 全仓未用（用了会带模板参数 <T>，正则不匹配）。
    跨行赋值不追（本仓 shader 无此写法，出现即漏报——lint 取窄不误报是自觉取舍）。
    """
    warnings: list[Warning] = []
    srcs = {}
    for rel in rgl_tree_files(".slang"):
        src = read_text(Path(ROOT) / rel)
        if src is not None:
            srcs[rel] = src
    names = {n for src in srcs.values() for n in _BAB_DECL_RX.findall(src)}
    rxs = [re.compile(tmpl.format(name=n)) for n, tmpl in
        ((n, _BAB_BARE_LOAD_RX_TMPL) for n in names)]
    for rel, src in srcs.items():
        for lineno, line in enumerate(strip_comments(src).splitlines(), 1):
            if "asfloat" in line:
                continue
            if any(rx.search(line) for rx in rxs):
                warnings.append(Warning(rel, lineno,
                    "ByteAddressBuffer.Load 返回 uint 位模式——floatN 裸赋值会被 slang "
                    "降低成 ConvertUToF（数值转换非位转，静默出错）；须 asfloat(buf.Load…)"))
    return warnings


# ─── AGENTS「其他文档」条目禁状态摘要（§1.9）──────────────────────────────────

_AGENTS_SECTION_HEAD_RX = re.compile(
    rf"^## {re.escape(AGENTS_DOC_INDEX_SECTION)}[^\n]*\n", re.M)
_AGENTS_DATE_RX = re.compile(r"20\d{2}-\d{2}-\d{2}")
_AGENTS_BATCH_RX = re.compile(r"批\s*\d+(?:\.\d+)?")


def agents_index_status_checks() -> list[Warning]:
    """AGENTS「其他文档」的**索引条目**里不得出现日期 / 批次号 / 进度状态词。

    **立论**：本节第一句就写「每篇文档头部带状态」，条目里再写一遍 = 给同一个状态开第二份副本，
    而副本必然漂——推进文档的人不会记得同步 AGENTS，**漏一次就永久错**。实测抓到两条已漂且方向
    是误导：一条把文档明写的「非反转」写成「已反转」，一条把已恢复推进的轴写成「挂起」。
    §1.6 只对账**文件名** ⟹ 加挂号有构建奖励、摘要过期零惩罚 = 单向棘轮。本条把「摘要会漂」
    从节规变成机器判据（判据不判"该不该写这条"，只判"是不是状态副本"）。

    **判定**（warning 级）：本节 bullet 行命中 ① `20YY-MM-DD` ② `批 N[.M]` ③
    `config.AGENTS_INDEX_STATUS_WORDS` 任一项。**`>` 引言块豁免**——那里是规则说明区，写日期
    属于论证不属于状态（本节自己的「教训写哪（2026-10-08 换轨…）」就是这条豁免的活体反向用例：
    它必须恒为静默）。

    **失效出声**（error 级）：AGENTS 读不到 / 找不到本节 ⟹ 本节被改名或删掉会让护栏**静默变瞎**，
    而"零警告"会被读成"摘要干净"（同 §1.6 / §5 / §8 的取向）。
    """
    agents = read_text(Path(ROOT) / AGENTS_FILE)
    if agents is None:
        return [Warning(AGENTS_FILE, 1,
            f"读不到 {AGENTS_FILE}——索引状态摘要检查**无法执行**（这不是「摘要干净」）", "error")]
    head = _AGENTS_SECTION_HEAD_RX.search(agents)
    if head is None:
        return [Warning(AGENTS_FILE, 1,
            f"找不到「## {AGENTS_DOC_INDEX_SECTION}」节——本节被改名/删除会让本规则静默变瞎，"
            "要么把节改回来、要么同步 config.AGENTS_DOC_INDEX_SECTION", "error")]
    body = agents[head.end():]
    nxt = re.search(r"^## ", body, re.M)
    if nxt:
        body = body[:nxt.start()]
    head_line = agents[:head.end()].count("\n")
    warnings: list[Warning] = []
    for offset, line in enumerate(body.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(">"):
            continue  # 空行 / 引言块 = 规则说明区，不判
        hits: list[str] = []
        if date := _AGENTS_DATE_RX.search(line):
            hits.append(f"日期 `{date.group(0)}`")
        if batch := _AGENTS_BATCH_RX.search(line):
            hits.append(f"批次号 `{batch.group(0)}`")
        hits += [f"状态词 `{w}`" for w in AGENTS_INDEX_STATUS_WORDS if w in line]
        if hits:
            warnings.append(Warning(AGENTS_FILE, head_line + offset,
                "「其他文档」条目写状态摘要（" + "、".join(hits) + "）——状态只住文档头部，"
                "本节只写「什么时候该读它」与「读哪一节省钱」；文档一推进这条就静默变错"
                "（写法 → skill rgl-doc-placement）"))
    return warnings


# ─── 渲染图 Lua 词表对账（债-122 另一半，§11）─────────────────────────────────

SCAN_RG_ENUM_VOCAB: dict[str, str] = {}


def rg_enum_vocab_readout() -> str:
    """五张表的覆盖面读数（`RgAttachmentKind 5/5 · …`）；没跑过 = 空串。"""
    if not SCAN_RG_ENUM_VOCAB:
        return ""
    return " · ".join(f"{k} {v}" for k, v in SCAN_RG_ENUM_VOCAB.items())


def rg_enum_vocab_checks() -> list[Warning]:
    """枚举本体 → Lua 词表右值 **逐个对账**（债-122 另一半）。

    ## 病根（债-122 前一半的真实形态）
    `depthOffscreen` 在 `RgAttachmentKind` 里住很久，词表却没挂号 ⟹「配置里写
    depthOffscreen 报词表错」而内建图用同一个 kind 跑得好好的 = **两条路只通了一条**。
    以后再加 `RgAttachmentKind` / `RgUsage` 档，漏词表只能靠 `parseEnum` 运行期响亮
    报错，没有构建期判据——本条把它变成构建期 ERROR。

    ## 判据（口径 = 右值引用，不是字符串出现）
    - 枚举本体（`RG_ENUM_VOCAB_SOURCE_FILE`）逐个解析 `enum class RgXxx { … };` 的
      枚举子名单（先 `strip_comments` 再切分：吃掉行内注释与 `= 值` 形态；
      `RgLoadOp`/`RgStoreOp` 是单行体，同一式子覆盖）。
    - 词表侧（`RG_ENUM_VOCAB_TABLE_FILE`）收集 `RgXxx::<enumerator>` 形态的**右值**集合
      （也先剥注释——注释里提一嘴不算挂号）。🔴 不能拿「枚举名当串在文件里找」：
      `RgPassKind::drawMesh` 的 Lua 拼写是 `"draw"`，照串找 = 规则第一天就说谎。
    - 枚举声明了、右值集合没有 = ERROR，点名枚举 + 缺的枚举子 + 两文件 + 词表已覆盖
      名单，症状句写进消息（「两条路只通了一条」）。

    ## 失效必须出声（"没数不是相等"）
    - 两文件任一读不到 ⟹ ERROR。
    - `RG_ENUM_VOCAB_ENUMS` 清单里的枚举在源文件**找不到定义** ⟹ ERROR
      （改名/搬走要红，不许静默少对一张表——清单写死在 config.py 就是为了让它红）。
    - 某枚举体解析出 0 个枚举子 / 某表词表右值扫出 0 条 ⟹ ERROR（格式变了没同步）。

    ## 盲区（写下来不装没看见）
    - **只查「枚举子有没有被右值引用」，不查 Lua 侧拼写本身**——`{"draw", …}` 的键名
      漂了（比如和别的表重名/改拼写）本规则看不见；拼写是 Lua 配置面的 API，改动会
      红在配置解析期（parseEnum 报词表错），不归本条。
    - 反向不查：词表右值引用了枚举里没有的档 = 编译不过（C++ 类型检查），不需要本条。
    - 枚举体里的 `#if` 条件编译档会扫不到（本仓 rgl/*.cppm 无此形态）。
    """
    src = read_text(Path(ROOT) / RG_ENUM_VOCAB_SOURCE_FILE)
    tbl = read_text(Path(ROOT) / RG_ENUM_VOCAB_TABLE_FILE)
    SCAN_RG_ENUM_VOCAB.clear()
    if src is None or tbl is None:
        missing = "、".join(f
                            for f, ok in ((RG_ENUM_VOCAB_SOURCE_FILE, src),
                                          (RG_ENUM_VOCAB_TABLE_FILE, tbl)) if not ok)
        return [Warning("tools/checks/config.py", 0,
            f"渲染图词表对账（债-122）**本次未做任何对账**：扫描面 {missing} 读不到"
            "（改名 / 搬走了？同步改 tools/checks/config.py 的 RG_ENUM_VOCAB_*_FILE）"
            "——「零警告」不是「五张表都齐」", "error")]
    src, tbl = strip_comments(src), strip_comments(tbl)
    warnings: list[Warning] = []
    for enum_name in RG_ENUM_VOCAB_ENUMS:
        m = re.search(rf"enum class {enum_name}\b[^{{;]*\{{([^}}]*)\}}", src)
        if m is None:
            warnings.append(Warning(RG_ENUM_VOCAB_SOURCE_FILE, 1,
                f"清单里的枚举 {enum_name} 在 {RG_ENUM_VOCAB_SOURCE_FILE} 找不到定义"
                f"（改名 / 搬走了？同步改 tools/checks/config.py 的 RG_ENUM_VOCAB_ENUMS，"
                "不许静默少对一张表）", "error"))
            continue
        members: list[str] = []
        for part in m.group(1).split(","):
            ident = part.split("=")[0].strip()
            if ident:
                if not re.fullmatch(r"[A-Za-z_]\w*", ident):
                    warnings.append(Warning(RG_ENUM_VOCAB_SOURCE_FILE, 1,
                        f"{enum_name} 的枚举体切分出非标识符片段 {ident!r}"
                        "（枚举体里出现了本规则没料到的形态，同步改规则）", "error"))
                    break
                members.append(ident)
        if not members:
            warnings.append(Warning(RG_ENUM_VOCAB_SOURCE_FILE, 1,
                f"{enum_name} 的枚举体解析出 0 个枚举子（格式变了？同步改 "
                "tools/checks/rules_text.py::rg_enum_vocab_checks，别让它静默空跑）", "error"))
            continue
        rvals = set(re.findall(rf"\b{enum_name}::(\w+)", tbl))
        if not rvals:
            warnings.append(Warning(RG_ENUM_VOCAB_TABLE_FILE, 1,
                f"{enum_name} 在 {RG_ENUM_VOCAB_TABLE_FILE} 扫出 0 条词表右值"
                f"（词表被搬走 / 右值形态变了？本表 {len(members)} 个枚举子全部对账未发生）",
                "error"))
            continue
        covered = sorted(set(members) & rvals)
        SCAN_RG_ENUM_VOCAB[enum_name] = f"{len(covered)}/{len(members)}"
        missing = [x for x in members if x not in rvals]
        if missing:
            warnings.append(Warning(RG_ENUM_VOCAB_SOURCE_FILE,
                src[:m.start()].count("\n") + 1,
                f"{enum_name} 的枚举子 {'、'.join(missing)} 没有被 {RG_ENUM_VOCAB_TABLE_FILE} "
                f"的任何词表右值 {enum_name}::<档> 引用 = 配置里写不出来这个档，"
                f"而内建图用同一个值跑得好好的 = 两条路只通了一条（债-122 的 "
                f"depthOffscreen 就住在这里）。词表已覆盖："
                f"{'、'.join(covered) if covered else '（空）'}", "error"))
    return warnings


# ─── 生成物聚合初始化必须写指定初始化器（债-118，§12）─────────────────────────

# 覆盖面读数（`struct 34 / 别名 1 / 命中 56 / 位置初始化 24 / 基线 24`）与逐文件命中条数。
# 🔴 SCAN_GENERATED_INIT **只由本规则写**：main.py 的 --update-generated-init-baseline 拿它的
# 空/非空判「本条跑没跑过」，别的规则混进来会让收账命令把「没数」写进基线（护栏变瞎）。
SCAN_GENERATED_INIT: dict[str, str] = {}
LAST_GENERATED_INIT_COUNTS: dict[str, int] = {}

# 别名形态：`using 别名 = <生成物类型>;`（真实形态 = `export using CullFrameInput = CullPush;`）
GENERATED_ALIAS_RE = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*;")
# 生成物 struct 体里的字段声明（`std::array<glm::vec4, 6> planes` / `bool x = false` 都吃）
GENERATED_FIELD_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[\s\S]*)?$")


def generated_init_readout() -> str:
    """生成物聚合初始化护栏的覆盖面读数；没跑过 = 空串（收账命令据此拒绝落笔）。"""
    if not SCAN_GENERATED_INIT:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN_GENERATED_INIT.items())


def generated_init_counts() -> dict[str, int]:
    """最近一次逐文件「位置初始化」条数（收账命令的输入；未跑过 = 空表）。"""
    return dict(LAST_GENERATED_INIT_COUNTS)


def load_generated_init_baseline() -> tuple[dict[str, int], str | None]:
    """读棘轮基线 → ({rel: 允许条数}, 不可用原因)。

    🔴 空表会被下游读成「零位置初始化」⟹ 缺失 / 坏 JSON / 格式不符一律返回原因串
    （同 §9 R-EC4 / §1.7 的纪律，不走 tool_baseline 的「缺失→空表按未登记」）。
    """
    path = Path(ROOT) / GENERATED_INIT_BASELINE_FILE
    if not path.is_file():
        return {}, f"基线文件不存在：{GENERATED_INIT_BASELINE_FILE}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {}, f"基线文件读不出来：{GENERATED_INIT_BASELINE_FILE}（{type(e).__name__}: {e}）"
    if not isinstance(data, dict):
        return {}, f"基线顶层不是 JSON 对象：{GENERATED_INIT_BASELINE_FILE}"
    files = data.get("files")
    if not isinstance(files, dict):
        return {}, f"基线缺 `files` 对象（格式不符）：{GENERATED_INIT_BASELINE_FILE}"
    bad = sorted(str(k) for k, v in files.items()
                 if isinstance(v, bool) or not isinstance(v, int))
    if bad:
        return {}, f"基线 `files` 有非整数条数：{GENERATED_INIT_BASELINE_FILE} 的 {bad[:3]}"
    return {str(k): int(v) for k, v in files.items()}, None


def write_generated_init_baseline(counts: dict[str, int]) -> str:
    """按当前条数重写棘轮基线（收账）；返回写入路径。"""
    files = {k: int(v) for k, v in sorted(counts.items())}
    path = Path(ROOT) / GENERATED_INIT_BASELINE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(
        {"_comment": GENERATED_INIT_BASELINE_COMMENT,
         "total": sum(files.values()), "files": files},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return GENERATED_INIT_BASELINE_FILE


def _brace_body(src: str, open_idx: int) -> str:
    """src[open_idx] == '{' → 返回**不含**外层花括号的体（找不到闭合 = 到文件尾）。"""
    depth = 0
    for i in range(open_idx, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_idx + 1:i]
    return src[open_idx + 1:]


def _generated_struct_fields() -> tuple[dict[str, list[str]], str | None]:
    """类型名单真相源 = **生成物本体** → ({struct 名: 字段声明序}, 失效原因)。

    名单不抄进 config 也不抄进规则（抄名单反模式：34 个名字抄一份 = 反射管线加一个 struct
    就静默漏一个）。`types_generated.cppm` 是债-26 的空模块（0 个 struct）——**按目录聚合**，
    只要整个目录解析出 ≥1 个就不出声，单文件 0 个不是失效形态。
    """
    base = Path(ROOT) / GENERATED_TRUTH_DIR
    if not base.is_dir():
        return {}, (f"生成物目录 {GENERATED_TRUTH_DIR}/ 读不到（改名 / 搬走了？"
                    "同步改 tools/checks/config.py 的 GENERATED_TRUTH_DIR）")
    fields: dict[str, list[str]] = {}
    for path in sorted(base.glob("*.cppm")):
        src = read_text(path)
        if src is None:
            continue
        src = strip_comments(src)
        for m in re.finditer(GENERATED_STRUCT_RE, src):
            body = _brace_body(src, m.end() - 1)
            names: list[str] = []
            for stmt in body.split(";"):
                fm = GENERATED_FIELD_RE.search(stmt.strip())
                if fm:
                    names.append(fm.group(1))
            fields[m.group(1)] = names
    if not fields:
        return {}, (f"{GENERATED_TRUTH_DIR}/*.cppm 里解析出 **0 个** `export struct`"
                    f"（生成物形态变了？同步改 tools/checks/config.py 的 GENERATED_STRUCT_RE，"
                    "别让它静默空跑）")
    return fields, None


def _generated_init_scan_files() -> tuple[list[str], str | None]:
    """扫描面 = `GENERATED_INIT_SCAN_DIRS` 下全部 .cppm，排除生成物目录本身。

    `tools/` **不在扫描面**（盲区，见 generated_init_checks 的 docstring）。
    """
    out: list[str] = []
    for d in GENERATED_INIT_SCAN_DIRS:
        base = Path(ROOT) / d
        if not base.is_dir():
            continue
        for p in base.rglob("*.cppm"):
            if not p.is_file():
                continue
            rel = rel_posix(p)
            if rel.startswith(GENERATED_TRUTH_DIR + "/"):
                continue
            out.append(rel)
    if not out:
        return [], ("扫描面为空：" + "、".join(GENERATED_INIT_SCAN_DIRS)
                    + " 下一个 .cppm 都没读到（目录搬走 / 后缀改了？）")
    return sorted(out), None


def generated_init_checks() -> list[Warning]:
    """对反射生成物类型的聚合初始化**必须写指定初始化器**（债-118；存量走棘轮基线）。

    ## 病根（§7.14 实测，这条不是"风格洁癖"）
    把布局换成反射生成物**本身不带来编译期保护**。`docs/compute_调研.md` §7.14 的对照表：
    调用点写 `EnvPrefilterPush{level, mipLevels, levelFs}`（位置形态）时，shader 改字段名
    `just build` **退出码 0**、同类型字段换序**也不红**；改成 `.level = …` 后两条都编译失败
    并点名调用点（C++20 要求指定初始化按声明序 ⟹ **一个约定同时堵「改名」与「换序」两个洞**）。
    于是「八枚 push 已全生成物」被读成「布局错配有机器拦」——拦它的是**约定**不是生成物，
    而这条约定此前**没有任何护栏**：新代码写回位置初始化不会有人响，布局仍然单源、生成物
    自带的 static_assert 仍然绿、`just test` 只在字段恰好被测试引用时才红。
    🔴 编译器给不了这条（clang 唯一近邻旗 `-Wmissing-designated-field-initializers` 报的是
    部分 designated init，位置初始化它不报），只能走文本判据。

    ## 判据形态
    - **类型名单真相源 = 生成物本体**（`GENERATED_TRUTH_DIR` 里的 `export struct`），
      外加扫描面里的 `using 别名 = <生成物类型>;`（真实形态 = `CullFrameInput = CullPush`）。
      🔴 别名不收 = 规则漏掉本仓唯一的**重命名**形态，而它恰恰是最需要护栏的那枚
      （BIT-IDENTICAL 的 CPU 读回重放路）。
    - 命中 = `<Type>` + 可选变量名 + `{`（先 `strip_comments`，行号不漂）。
    - **违规** = 花括号后首个非空白字符既不是 `.` 也不是 `}`。`T{}` 空初始化 + 随后逐字段
      赋值是**合法**风格（`izb_shadow_impl.cppm:290` / `meshlet_cull_service_impl.cppm:238`
      就是这个形态），不许报。
    - 存量走棘轮基线（只准降不准升，照 §9 R-EC4）：新增一条 = ERROR，处置是改成 `.字段 =`，
      **不是**来基线登记额度；ERROR 点名文件:行号 + 类型名 + **该类型的字段声明序**（从生成物
      现读，让人不用翻文件）+ 症状句。

    ## 失效必须出声（"没数不是零警告"）
    - 生成物目录读不到 / 整个目录解析出 **0 个 struct** ⟹ ERROR「本次未做任何对账」。
      （单文件 0 个不算失效：`types_generated.cppm` 是债-26 的空模块。）
    - 扫描面为空 ⟹ 同上。
    - 基线文件缺失 / 顶层或 `files` 不是对象 / 值非整数 ⟹ ERROR 点名基线文件。
    - 覆盖面读数每次构建印一行（`struct N / 别名 M / 命中 K / 位置初始化 P / 基线 B`），
      规则没跑到打「未执行」。

    ## 盲区（写下来不装没看见）
    - **只认字面类型名**：经模板实参（`f<T>()`）、两跳以上的别名链（本规则只收
      `using X = <生成物 struct>;` 一跳）间接出现的类型看不见；`std::array<T, N>{{{…}}}`
      的**内层**花括号（内层不写类型名）看不见；`T v = { … }` 这种**等号形态**也看不见
      （`{` 前面是 `=` 不是类型名）——本条判的是「写了类型名的聚合初始化」这一种最主流形态。
    - **字符串字面量不剥**（`strip_comments` 只剥注释）⟹ `tools/reflection_tool.cpp:163` 的生成器
      模板字符串里就写着 `export struct RgComputeBinding {`，故 `tools/` **不在扫描面**。
      扫描面内的字符串里写出生成物类型名 + `{` 会误报——当前实测 0 条。
    - 注释里提一嘴 `FanoutPush{` 不误报（先剥注释），这也是债挂号时担心的那条脆点，实测不成立。
    - 外层 push data 包装（`MipGenPushData` / `EnvPrefilterPushData`）**不归本条**：它们不是
      生成物类型，不在名单里（§7.14 的裁决）。
    - 基线按**文件**记条数（照 R-EC4）⟹ 同一文件内「删一条位置初始化 + 新写一条」互相抵消、
      不红。换成 `文件::变量名` 那种键（§6.4 R7 形态）能堵，代价是挪代码就得收账。
    """
    fields, bad_truth = _generated_struct_fields()
    if bad_truth:
        SCAN_GENERATED_INIT.clear()
        return [Warning("tools/checks/config.py", 0,
            f"生成物聚合初始化护栏（债-118）**本次未做任何对账**：{bad_truth}"
            "——「零警告」不是「调用点都写了指定初始化器」", "error")]
    rels, bad_scan = _generated_init_scan_files()
    if bad_scan:
        SCAN_GENERATED_INIT.clear()
        return [Warning("tools/checks/config.py", 0,
            f"生成物聚合初始化护栏（债-118）**本次未做任何对账**：{bad_scan}"
            "——「零警告」不是「调用点都写了指定初始化器」", "error")]

    # 别名要收进名单：先扫一遍建 别名 → 生成物类型 的映射（名单 = 生成物 struct ∪ 别名）
    alias_of: dict[str, str] = {}
    bodies: dict[str, str] = {}
    for rel in rels:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        stripped = strip_comments(src)
        bodies[rel] = stripped
        for am in GENERATED_ALIAS_RE.finditer(stripped):
            alias, target = am.group(1), am.group(2)
            if target in fields and alias not in fields:
                alias_of[alias] = target

    names = sorted(set(fields) | set(alias_of), key=lambda x: (-len(x), x))
    hit_re = re.compile(r"(?<![A-Za-z0-9_])(?P<type>" + "|".join(re.escape(n) for n in names)
                        + r")(?![A-Za-z0-9_])\s*(?:[A-Za-z_]\w*\s*)?\{")
    hits: dict[str, list[tuple[int, str]]] = {}
    total_hits = 0
    for rel, stripped in bodies.items():
        for m in hit_re.finditer(stripped):
            total_hits += 1
            if stripped[m.end():].lstrip()[:1] in (".", "}"):
                continue  # 指定初始化 / 空初始化 = 合法
            hits.setdefault(rel, []).append((stripped[:m.start()].count("\n") + 1,
                                             m.group("type")))

    counts = {rel: len(v) for rel, v in hits.items()}
    LAST_GENERATED_INIT_COUNTS.clear()
    LAST_GENERATED_INIT_COUNTS.update(counts)
    baseline, bad_base = load_generated_init_baseline()
    SCAN_GENERATED_INIT.clear()
    SCAN_GENERATED_INIT.update({
        "struct": str(len(fields)), "别名": str(len(alias_of)), "命中": str(total_hits),
        "位置初始化": str(sum(counts.values())),
        "基线": str(sum(baseline.values())) if not bad_base else "0"})
    if bad_base:
        return [Warning(GENERATED_INIT_BASELINE_FILE, 0,
            f"生成物聚合初始化棘轮**本次未做任何对账**：{bad_base}"
            f"（当前位置初始化 {sum(counts.values())} 条未比对）——先修基线文件", "error")]

    def msg(tname: str) -> str:
        target = alias_of.get(tname, tname)
        order = "、".join(fields.get(target, [])) or "（生成物里没解析出字段）"
        who = f"{tname}（= 生成物 {target} 的别名）" if tname in alias_of else tname
        return (f"对反射生成物 {who} 写的是**位置**聚合初始化——字段声明序：{order}。"
                "位置聚合初始化 ⟹ shader 改字段名/同类型换序都不红，生成物单源白单"
                "（§7.14 实测）。处置：改成 `.字段 =` 指定初始化器（C++20 还要求按声明序书写 "
                f"⟹ 换序一起红），**不是**来 {GENERATED_INIT_BASELINE_FILE} 登记额度")

    warnings: list[Warning] = []
    for rel in sorted(set(counts) | set(baseline)):
        cur, base = counts.get(rel, 0), baseline.get(rel, 0)
        if rel not in counts:
            if base > 0:
                warnings.append(Warning(rel, 0,
                    f"生成物聚合初始化基线仍记 {base} 条但该文件已清零/不在扫描面——"
                    f"用 `{GENERATED_INIT_UPDATE_CMD}` 收账"))
            continue
        if cur > base:
            for line, tname in hits[rel]:
                warnings.append(Warning(rel, line, msg(tname), "error"))
            if rel not in baseline:
                warnings.append(Warning(rel, 0,
                    f"生成物聚合初始化基线未登记：当前 {cur} 条（新文件必须显式登记进 "
                    f"{GENERATED_INIT_BASELINE_FILE}——但**先确认**它是生成物类型而不是同名"
                    "的手写结构体，且真的没法改成指定初始化器）", "error"))
            else:
                warnings.append(Warning(rel, 0,
                    f"生成物聚合初始化基线上升 {cur} > {base}——处置是把调用点改成 `.字段 =`，"
                    f"**不是**改基线（基线只准降；确实清了才用 `{GENERATED_INIT_UPDATE_CMD}`）",
                    "error"))
        elif cur < base:
            warnings.append(Warning(rel, 0,
                f"生成物聚合初始化可下调 {cur} < {base}——用 `{GENERATED_INIT_UPDATE_CMD}` "
                "收账（悄悄漂移等于没有基线）"))
    return warnings


# ─── imgqa selftest 断言条数三口径 + testXxx 三方对账（债-123，§13）──────────────

# 覆盖面读数（断言三口径 + testXxx 三方条数）。🔴 SCAN_IMGQA_SELFTEST **只由本规则写**
# （同 SCAN_GENERATED_INIT 纪律：main.py 读数段拿空/非空判「本条跑没跑过」，没数打「未执行」）。
SCAN_IMGQA_SELFTEST: dict[str, str] = {}


def imgqa_selftest_readout() -> str:
    """imgqa selftest 对账的覆盖面读数；没跑过 = 空串（main.py 据此打「未执行」）。"""
    if not SCAN_IMGQA_SELFTEST:
        return ""
    s = SCAN_IMGQA_SELFTEST
    return (f"断言 {s['实跑']}（grep {s['grep']} / 剥注释 {s['剥注释']} / 注释 {s['注释']}）"
            f" · 测试 声明 {s['声明']} · 定义 {s['定义']} · 调用 {s['调用']}")


def imgqa_selftest_checks() -> list[Warning]:
    """`imgqa selftest` 的断言条数三口径相等 + testXxx 声明/定义/调用三方集合相等（债-123）。

    ## 病根（挂号原文 → docs/mdi_plan.md §11.5；总账 债-123）
    断言条数此前**只是 `selftest.h` 头注释里的文档约定，没有机器执行**：`chk` 只在失败时
    打印 ⟹ 全绿时 stdout 只有一句「0 条失败」，**丢一条断言不会让任何东西变红**，它只会让
    那句汇总少守一件事。2026-10-12 当天就被加断言改漂过一次（口径还没记录时两个人数出三个数）。

    🔴 testXxx 三方对账的真实病根（这条最有价值的一半）：**定义了却没被 `cmdSelftest`
    路由 = 这条测试静默不跑**，而 `imgqa selftest` 仍然「0 条失败」——与「丢一条断言什么都
    不红」是同一类缺口，只是发生在**整条测试的粒度**上。声明缺 = 编译/链接期就红（本条把它
    提前到构建期）；**调用缺只有本条能抓**（编译链接都看不见，运行期也没有任何信号）。

    ## 设计红线（为什么是静态调用点口径，不是运行期计数——挂号原文的理由，别翻案）
    「运行期计数 vs 写死常量」看着最省，但运行期计数把**循环内断言按执行次数计**，而循环
    上界一旦随数据变（表长、机位数），护栏就在**合法改动**下变红；静态调用点口径虽然土，
    但它**与激励无关**。本条因此全部走文本口径：期望值从 `selftest.h` 头注释的口径命令行
    现读（`# = N`），**数字不抄进规则也不抄进 config**（抄数字 = 造第二条会漂的副本）。

    ## 判据形态
    - **A 三口径相等**：① 逐文件数**含 `c.chk(` 的行数**（= 头注释口径命令 `grep -cE` 的
      语义，**不剥注释**）；② 先 `strip_comments` 再数 `c.chk(` **出现次数**；③ 头注释
      写死的 `# = N`。① ≠ ② ⟹ 「有 `c.chk(` 被注释掉（死断言，grep 口径会算它但它不会跑）
      或一行写了两条」，处置 = 删死断言/拆行，**不是**改注释数字；① == ② ≠ ③ ⟹
      「加/删了断言没更新头注释」，处置 = 更新那一行的 `# = N`。两条文案分开，ERROR 都打全三个数。
    - **B 三方集合相等**（声明 / 定义 / 调用，**双向**，不是单向 ⊆）：声明 = `selftest.h`
      里的 `void testXxx(Checker`；定义 = 九个 `selftest*.cpp` 里的同式子；调用 =
      `cmdSelftest` 函数体内的 `testXxx(c);`（先剥注释——注释掉的声明/定义/调用都不算数，
      注释掉的调用正是「静默不跑」的形态之一）。缺哪侧点名哪侧 + 函数名 + 三处位置。
    - **C 头注释的「N 条…声明」计数标记**（现两处 46）与实数声明条数逐处对账，行号点名。

    ## 失效必须出声（「没数不是零警告」）
    头注释读不到 / glob 出 0 个 TU / 任一 TU 读不到 / 口径命令行解析不出 `# = N`（口径标记
    没了）/ 三口径任一为 0 / `cmdSelftest` 定义找不到 / 声明或定义或调用集合为 0 ⟹ ERROR
    「本次未做任何对账」，并点名要同步改的 config 常量。覆盖面读数每次构建印一行，
    规则没跑到打「未执行」。

    ## 盲区（写下来不装没看见）
    - 字符串字面量不剥（`strip_comments` 只剥注释）⟹ 字符串里写 `c.chk(` 会被 ② 多算——
      当前实测 0 处；① 口径本来就数含 token 的行，与 grep 语义一致，不算漂移。
    - 声明/定义只认 `void testXxx(Checker` 形态、调用只认 `testXxx(c);`：签名大改或经函数
      指针表/循环注册调用会扫不到——但扫不到的表现是**集合不等而红**（点名缺侧），不是静默。
    - 同名函数在两个 TU 各定义一份（ODR 违规）本条看不见（defs 字典后写覆盖）——链接器管。
    - 本条不编译 C++，判不了「声明了但定义在 `#if 0` 里」——selftest TU 无条件编译形态。
    """
    def fail(why: str) -> list[Warning]:
        return [Warning("tools/checks/config.py", 0,
            f"imgqa selftest 对账（债-123）**本次未做任何对账**：{why}"
            "——「零警告」不是「断言条数与测试路由都对上了」", "error")]

    SCAN_IMGQA_SELFTEST.clear()
    hdr = read_text(Path(ROOT) / IMGQA_SELFTEST_HEADER)
    if hdr is None:
        return fail(f"扫描面 {IMGQA_SELFTEST_HEADER} 读不到（改名 / 搬走了？"
                    "同步改 tools/checks/config.py 的 IMGQA_SELFTEST_HEADER）")
    tus = sorted(Path(ROOT).glob(IMGQA_SELFTEST_TU_GLOB))
    if not tus:
        return fail(f"glob `{IMGQA_SELFTEST_TU_GLOB}` 匹配到 0 个 TU（selftest*.cpp 改名 / "
                    "搬走了？同步改 tools/checks/config.py 的 IMGQA_SELFTEST_TU_GLOB，"
                    "别让这条规则静默空跑）")
    bodies: dict[str, str] = {}
    for p in tus:
        rel = rel_posix(p)
        src = read_text(p)
        if src is None:
            return fail(f"扫描面 {rel} 读不到（读不出来 = 本条少数一个文件，不是少一条警告）")
        bodies[rel] = src

    # 期望值 ③：口径命令行的唯一真相源 = 头注释（数字不许抄进规则/config，见 docstring 设计红线）
    em = re.search(IMGQA_SELFTEST_EXPECT_RE, hdr)
    if em is None:
        return [Warning(IMGQA_SELFTEST_HEADER, 0,
            f"imgqa selftest 对账（债-123）**本次未做任何对账**：{IMGQA_SELFTEST_HEADER} "
            "头注释里解析不出口径命令行的 `# = N` 期望值（**口径标记没了**——恢复 "
            "`grep -cE 'c\\.chk\\(' … # = N` 那一行。期望值的唯一真相源就是它，"
            "别把数字抄进 tools/checks/ 的任何文件）"
            "——「零警告」不是「断言条数与测试路由都对上了」", "error")]
    expect_n = int(em.group(1))
    expect_line = hdr[:em.start()].count("\n") + 1

    # 口径 ①：grep -c 语义（逐文件数含 token 的**行数**，不剥注释）；②：剥注释后数**出现次数**
    grep_n = sum(sum(1 for ln in src.splitlines() if IMGQA_SELFTEST_CHK_TOKEN in ln)
                 for src in bodies.values())
    stripped = {rel: strip_comments(src) for rel, src in bodies.items()}
    live_n = sum(src.count(IMGQA_SELFTEST_CHK_TOKEN) for src in stripped.values())
    if grep_n == 0 or live_n == 0 or expect_n == 0:
        return fail(f"三口径出现 0（grep {grep_n} / 剥注释 {live_n} / 注释 {expect_n}）——"
                    f"断言 token `{IMGQA_SELFTEST_CHK_TOKEN}` 或口径标记的形态变了，"
                    "本条什么都没数到")

    hdr_stripped = strip_comments(hdr)
    decls: dict[str, int] = {m.group(1): hdr_stripped[:m.start()].count("\n") + 1
                             for m in re.finditer(IMGQA_SELFTEST_TEST_RE, hdr_stripped)}
    defs: dict[str, tuple[str, int]] = {}
    calls: dict[str, tuple[str, int]] = {}
    for rel, src in stripped.items():
        for m in re.finditer(IMGQA_SELFTEST_TEST_RE, src):
            defs[m.group(1)] = (rel, src[:m.start()].count("\n") + 1)
        rm = re.search(rf"\b{IMGQA_SELFTEST_ROUTINE}\s*\([^)]*\)\s*\{{", src)
        if rm is None:
            continue
        body = _brace_body(src, rm.end() - 1)
        brace_line = src[:rm.end() - 1].count("\n") + 1
        for cm in re.finditer(IMGQA_SELFTEST_CALL_RE, body):
            calls[cm.group(1)] = (rel, brace_line + body[:cm.start()].count("\n"))
    if not calls:
        return fail(f"在 `{IMGQA_SELFTEST_TU_GLOB}` 里找不到 {IMGQA_SELFTEST_ROUTINE} 的"
                    f"函数体或体内解析出 0 条 `testXxx(c);` 调用（汇总路由改名 / 调用形态变了？"
                    "同步改 tools/checks/config.py 的 IMGQA_SELFTEST_ROUTINE / _CALL_RE）")
    if not decls:
        return fail(f"{IMGQA_SELFTEST_HEADER} 里解析出 0 条 `void testXxx(Checker` 声明"
                    "（声明形态变了？同步改 tools/checks/config.py 的 IMGQA_SELFTEST_TEST_RE）")
    if not defs:
        return fail(f"九个 TU 里解析出 0 条 testXxx 定义（定义形态变了？同步改 "
                    "tools/checks/config.py 的 IMGQA_SELFTEST_TEST_RE）")

    SCAN_IMGQA_SELFTEST.update({
        "实跑": str(live_n), "grep": str(grep_n), "剥注释": str(live_n),
        "注释": str(expect_n), "声明": str(len(decls)), "定义": str(len(defs)),
        "调用": str(len(calls))})
    warnings: list[Warning] = []

    # ── A：三口径相等（① ≠ ② 与 ③ 漂是两种病因，文案必须分开——混写成一条会教人改错地方）──
    if grep_n != live_n:
        warnings.append(Warning(IMGQA_SELFTEST_HEADER, expect_line,
            f"断言三口径分叉：grep 行数 {grep_n} / 剥注释出现次数 {live_n} / 头注释写死 "
            f"{expect_n}。① ≠ ② ⟹ 有 `c.chk(` 被**注释掉了**（死断言：grep 口径会算它，"
            "但它不会跑）或**一行写了两条**（grep 一行只数 1）。处置 = 删掉死断言 / 把两条"
            f"拆成两行，**不是**把 {IMGQA_SELFTEST_HEADER} 的 `# = N` 改成实跑数"
            "——那等于把死断言合法化", "error"))
    elif expect_n != grep_n:
        warnings.append(Warning(IMGQA_SELFTEST_HEADER, expect_line,
            f"断言三口径分叉：grep 行数 {grep_n} = 剥注释出现次数 {live_n} ≠ 头注释写死 "
            f"{expect_n}。③ 与实跑不等 ⟹ 加/删了断言没更新头注释。处置：把 "
            f"{IMGQA_SELFTEST_HEADER}:{expect_line} 的 `# = {expect_n}` 改成 `# = {grep_n}`"
            "（先确认新增断言逐条可红，别拿计数给摆设断言背书）", "error"))

    # ── B：三方集合相等（双向；缺哪侧点名哪侧）──
    for name in sorted(set(decls) | set(defs) | set(calls)):
        missing = [side for side, table in (("声明", decls), ("定义", defs), ("调用", calls))
                   if name not in table]
        if not missing:
            continue
        found = []
        if name in decls:
            found.append(f"声明 {IMGQA_SELFTEST_HEADER}:{decls[name]}")
        if name in defs:
            found.append(f"定义 {defs[name][0]}:{defs[name][1]}")
        if name in calls:
            found.append(f"调用 {calls[name][0]}:{calls[name][1]}")
        why: list[str] = []
        if "调用" in missing:
            why.append(f"但 {IMGQA_SELFTEST_ROUTINE} 没调用它 = **这条测试静默不跑**，而 "
                       "`imgqa selftest` 仍然「0 条失败」（债-123「丢一条断言什么都不红」"
                       f"的同类缺口，只是发生在整条测试的粒度上；编译链接都正常，**调用缺"
                       "只有本条能抓**）。处置：在 selftest.cpp 的 "
                       f"{IMGQA_SELFTEST_ROUTINE} 里补 `{name}(c);`（调用顺序 = 打印顺序）")
        if "定义" in missing:
            why.append(f"但 `{IMGQA_SELFTEST_TU_GLOB}` 里没有它的定义 = 链接期才红"
                       "（本条提前到构建期）。处置：补定义，或确认定义被搬出了扫描面并同步 "
                       "tools/checks/config.py 的 IMGQA_SELFTEST_TU_GLOB")
        if "声明" in missing:
            why.append(f"但 {IMGQA_SELFTEST_HEADER} 里没有它的声明 = cmdSelftest 调不到它"
                       "（编译期就红，本条提前到构建期）。处置：把声明补进头文件的声明块")
        if name in defs:
            loc, line = defs[name]
        elif name in decls:
            loc, line = IMGQA_SELFTEST_HEADER, decls[name]
        else:
            loc, line = calls[name]
        warnings.append(Warning(loc, line,
            f"testXxx 三方集合不等：'{name}' 缺{'与'.join(missing)}侧。已在：{'；'.join(found)}。"
            + "；".join(why), "error"))

    # ── C：头注释的「N 条…声明」计数标记与实数声明条数逐处对账 ──
    occ = [(hdr[:m.start()].count("\n") + 1, int(m.group(1)))
           for m in re.finditer(IMGQA_SELFTEST_DECL_COUNT_RE, hdr)]
    if not occ:
        warnings.append(Warning(IMGQA_SELFTEST_HEADER, 0,
            "imgqa selftest 对账判据 C **本次未执行**：头注释里解析出 0 处「N 条…声明」"
            f"计数标记（{IMGQA_SELFTEST_HEADER} 第 3 行与声明块分隔行的措辞变了？同步改 "
            "tools/checks/config.py 的 IMGQA_SELFTEST_DECL_COUNT_RE，别让这条判据静默空跑）",
            "error"))
    for line, n in occ:
        if n != len(decls):
            warnings.append(Warning(IMGQA_SELFTEST_HEADER, line,
                f"头注释写「{n} 条…声明」，实际 {IMGQA_SELFTEST_HEADER} 里 "
                f"`void testXxx(Checker` 声明有 {len(decls)} 条 ⟹ 加/删了测试没更新这处计数。"
                "处置：改这一行的数字（**先确认** B 的三方集合本来就相等——别拿改数字掩盖缺项）",
                "error"))
    return warnings


# ─── 图事实谓词「无问者即红」（债-129 S1a，§14）─────────────────────────────────

SCAN_GRAPH_FACT_PREDICATES: dict[str, int] = {}
SCAN_GRAPH_FACT_DEAD: list[str] = []


def _balanced_bodies(src: str, names) -> dict[str, str]:
    """取每个名字的**函数体**（花括号配对），供「谓词调谓词」的存活闭包用。

    只认 `name(<不含分号的实参表>) [const] {` 形态 = 定义（声明以 `;` 收尾，`[^;]*?` 吃不过去）。
    取不到体 = 该名字不进闭包（保守：宁可漏判存活、多报一条死谓词让人回答，不可反过来假绿）。
    """
    out: dict[str, str] = {}
    for n in names:
        m = re.search(rf"\b{n}\s*\([^;]*?\)\s*(?:const\s*)?\{{", src)
        if not m:
            continue
        start = src.index("{", m.start())
        depth = 0
        for j in range(start, len(src)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    out[n] = src[start:j + 1]
                    break
    return out


def graph_fact_predicate_readout() -> str:
    """谓词条数与存活分布读数；没跑过 = 空串（main.py 读数段据此打「未执行」）。"""
    if not SCAN_GRAPH_FACT_PREDICATES:
        return ""
    return (f"谓词 {len(SCAN_GRAPH_FACT_PREDICATES)} 条 / 生产问者最少 "
            f"{min(SCAN_GRAPH_FACT_PREDICATES.values())} 条 / 不在存活链 "
            f"{len(SCAN_GRAPH_FACT_DEAD)} 条")


def graph_fact_predicate_checks() -> list[Warning]:
    """`render_graph` 图事实查询面的每条谓词必须有**生产侧**调用者（债-129 S1a）。

    ## 病根
    查询面建了没人用 = 「机制常驻、收益悬空」。现成反面教材 = `OffGraphSchedule::isSchedulable`：
    接口 + 实现 + 十几条单测都在，生产侧零调用者 ⟹「有查询面」被读成「已收口」，而「图能不能
    调度这个 pass」这件事仍然没人问。本规则要防的正是 S1a 自己变成第二个它——所以判据落成
    护栏而不是文档里一句「必须有调用者」（文档只记录不阻止）。

    ## 判据
    - 谓词名单的真相源 = `render_graph.cppm` 的**命名约定**（`export <返回类型> graph<大写>(`）。
      🔴 名单不抄进任何地方（含 config.py）：抄一份「哪些是谓词」正是本规则要防的副本。
    - 问者 = `rgl/` 下除提供方两文件（接口 + impl 分区）外，**剥注释后**出现 `name(` 的文件。
      剥注释是必须的：接口注释里满是在解释这些谓词的名字，不剥 = 每条都自带一个假问者。
    - 🔴 **tests/ 不在问者扫描面**：测试调用者不算消费者（isSchedulable 的调用者全在 tests，
      一条生产路径也没守住）。
    - **传递存活**：被「已存活的谓词」在**函数体内**调用的谓词算活（`graphHasPassState` 的唯一
      问者是同模块的 `graphHasDeferredLight`，而后者有生产问者）。体边界用花括号配对取，
      不按文件粗算——按文件算会让两条**互相引用而都无人问**的死谓词彼此点亮 = 假绿。
    - 零问者 = ERROR，处置二选一：接上问者，或删掉它（裁决句 → docs/archive/配置正交性_plan.md §5.2
      「无消费者的机制删除或降级，不挂号」）。**不是**来登记额度——本规则没有基线文件。
      （本规则落地当天就抓到两条自建的零问者谓词 `graphHasAttachment` / `graphPassIndex`
      ——「先建后找」正是它要拦的动作，S1b/S3 有问者了再长回来。）

    ## 失效必须出声
    源文件读不到 / 解析出 0 条谓词 / 问者面 0 个文件 ⟹ ERROR（"没数"不是"都干净"，
    同 §10 RPC 路由对账的纪律）。命名约定改了（比如全改成 `hasXxx`）会让解析掉到 0 而红——
    那是**期望行为**，不是误报。

    ## 盲区（写下来不装没看见）
    - **只判「有没有」不判「够不够」**：只被一处调用的谓词也过。改成「每条 ≥N 问者」会把
      「先切一处、下一刀再切其余」的正常分阶段施工判红。
    - 经函数指针 / 宏 / 模板间接调用的形态扫不到（本仓当前无此形态）。
    - 名字出现在**字符串字面量**里会被当成问者（`strip_comments` 不剥字面量）——当前无此形态，
      出现即误报，届时收紧而不是放宽本规则。
    """
    src = read_text(Path(ROOT) / GRAPH_FACT_PREDICATE_SOURCE_FILE)
    if src is None:
        return [Warning("tools/checks/config.py", 0,
            f"图事实谓词对账**本次未做任何对账**：{GRAPH_FACT_PREDICATE_SOURCE_FILE} 读不到"
            "（改名 / 搬走了？同步改 tools/checks/config.py 的 GRAPH_FACT_PREDICATE_SOURCE_FILE）"
            "——「零警告」不是「每条谓词都有人问」", "error")]
    names = sorted(set(re.findall(GRAPH_FACT_PREDICATE_RE, strip_comments(src), re.M)))
    if not names:
        return [Warning(GRAPH_FACT_PREDICATE_SOURCE_FILE, 0,
            "图事实谓词对账**本次未做任何对账**：解析出 0 条 "
            f"`export … graph<大写>(` 声明（命名约定改了？同步改 tools/checks/config.py 的 "
            "GRAPH_FACT_PREDICATE_RE，别让这条规则静默空跑）", "error")]
    callers: dict[str, list[str]] = {n: [] for n in names}
    provider_src = ""
    scanned = 0
    for rel in rgl_cppm_files():
        text = strip_comments(read_text(Path(ROOT) / rel) or "")
        if Path(rel).name in GRAPH_FACT_PROVIDER_FILES:
            provider_src += text + "\n"  # 提供方内部引用走下面的「函数体」判据，不算问者
            continue
        scanned += 1
        for n in names:
            if re.search(rf"\b{n}\s*\(", text):
                callers[n].append(rel)
    if scanned == 0:
        return [Warning("tools/checks/config.py", 0,
            "图事实谓词对账**本次未做任何对账**：问者扫描面（rgl/ 的 .cppm）解析出 0 个文件"
            "——「零警告」不是「每条谓词都有人问」", "error")]
    # 传递存活：从「有生产问者」的谓词出发，沿**调用边的反方向**做闭包——
    # 「活的 m 调用了 n」才让 n 活（方向写反 = 被调用者点亮调用者，一条死谓词只要调用了
    # 任何活谓词就永久假绿）。边只在**提供方函数体**内取，不按文件粗算：按文件算会让两条
    # 互相引用、而都无人问的死谓词彼此点亮。
    bodies = _balanced_bodies(provider_src, names)
    live = {n for n in names if callers[n]}
    grew = True
    while grew:
        grew = False
        for n in names:
            if n in live:
                continue
            if any(m in live and m != n and re.search(rf"\b{n}\s*\(", bodies.get(m, ""))
                   for m in names):
                live.add(n)
                grew = True
    SCAN_GRAPH_FACT_PREDICATES.clear()
    SCAN_GRAPH_FACT_PREDICATES.update({n: len(callers[n]) for n in names})
    SCAN_GRAPH_FACT_DEAD[:] = [n for n in names if n not in live]
    return [Warning(GRAPH_FACT_PREDICATE_SOURCE_FILE, 0,
        f"图事实谓词 `{n}` 生产侧零问者、也不在任何存活谓词的调用链上 = 机制常驻、收益悬空"
        "（反面教材 = `OffGraphSchedule::isSchedulable`：接口 + 实现 + 单测都在、生产侧没人问）。"
        "处置二选一：接上问者，或删掉它（→ docs/archive/配置正交性_plan.md §5.2「无消费者的机制"
        "删除或降级，不挂号」）。🔴 本规则没有基线可登记——「先建后找」就是本条要防的形态。",
        "error") for n in SCAN_GRAPH_FACT_DEAD]


# ─── 链装饰器「次序判据不许裸奔」（债-129 S3，§15）──────────────────────────────────

SCAN_DECORATION_ORDER: dict[str, int] = {}


def decoration_order_readout() -> str:
    """装饰器 / 锚点消费者 / 豁免 / 裸奔 四档读数；没跑过 = 空串（main.py 据此打「未执行」）。"""
    if not SCAN_DECORATION_ORDER:
        return ""
    return (f"装饰器 {SCAN_DECORATION_ORDER['decorators']} 条 / 锚点消费者 "
            f"{SCAN_DECORATION_ORDER['anchored']} 条 / 次序豁免 {SCAN_DECORATION_ORDER['exempt']} 条"
            f" / 裸奔 {SCAN_DECORATION_ORDER['missing']} 条")


def decoration_order_predicate_checks() -> list[Warning]:
    """锚 `swapchainColorConsumerIndex` 的装饰器必须问共用次序判据（债-129 S3）。

    ## 病根
    链上装饰器都锚同一个「写 swapchain 且 sampleRead 色附件」的 pass（= tonemap），且都用
    `desc.passes.insert(begin() + consumerIdx, …)` 插入 ⟹ **锚点是对"当前 desc"求值的**：先装饰
    的那个把 tonemap 顶后一格，后装饰的就落在更靠后的一格。于是 main_impl 的调用序
    （`ssr → velocity → taa → ao_apply`）**直接决定图形态**，而挪错的图 compile 照样过、无
    hazard、无 warning、**画面静默错**。实测**可达**的那一形态 = `aa="fsr"`（它同时开 velocity
    与 ao_apply）下把 velocity 挪到 ao_apply 之后 ⟹ velocity 夹在 ao_apply 与 tonemap 之间、
    吃不到时域滤波；变异红线 A 实测它被拒启动。⚠ 反过来「velocity 排到 taa 之后」今天**配不出
    来**：`aa` 三态互斥、`velocityField` 只在 `aa="fsr"` 时置真 ⟹ 两者不可能同时在图里（那一半
    是防御性的，`velocity_field` 键撤并前可达）。本债 S3 给 velocity 补上次序判据只守住
    **今天**：判据的覆盖面本身没有护栏，下一个链装饰器照样裸奔。本规则钉的正是覆盖面——
    与 D 刀「加键不申报宿主绕不过去」同一手法（文档只记录不阻止）。

    ## 判据
    - 扫描面 = `render_graph_decorate.cppm` 里**剥注释后**按命名约定（行首
      `返回类型 decorate<大写>…Pass(`）认出的函数定义，取其**配对花括号函数体**
      （复用 §14 的 `_balanced_bodies`，同一条"体边界不按文件粗算"的理由）。
    - 受约束者 = 函数体里调用了锚点判据 `swapchainColorConsumerIndex` 的那些装饰器。
    - 要求 = 函数体里同时调用共用次序判据 `graphDecoratedTooLate`。
    - 🔴 **豁免名单不抄**：由**次序谓词自己的函数体**里出现的 `kXxxPassName` 反推
      （`kTaaPassName` → `decorateTaaPass`、`kAoApplyPassName` → `decorateAoApplyPass`）。
      理由是硬逻辑：谓词拒绝的那些 pass **本身就是"必须最后装饰"的那两个**，问它们
      "我是不是太晚了"没有意义（"taa 必须最后"是**涌现**性质——只要链上每个成员在"taa /
      ao_apply 已在图里"时都拒绝，它就成立）。豁免面与判据面因此是**同一个真相源**；
      抄一份"谁是晚装饰器"正是本规则要防的那类副本。命名约定漂了会让反推落空 → 下面
      「失效必须出声」把它变红，而不是静默放行。
    - 违反 = ERROR，处置二选一：调用共用判据（新装饰器排在链上，**默认就该是这条**），或
      把它拒绝的 pass 名加进 `graphDecoratedTooLate` 的函数体（新装饰器确实"必须最后"）。
      🔴 **本规则没有基线文件**——给它开额度 = 把护栏改成登记处（同 §14 的裁决）。

    ## 失效必须出声
    四种"没数"各一条 ERROR，都不许被读成"零警告 = 覆盖面干净"：源文件读不到 / 装饰器名单
    解析出 0 条 / **锚点消费者 0 条**（锚点改名或装饰器搬走 ⟹ 判据空跑）/ 次序谓词的函数体
    取不到或反推出 0 个 pass 名（谓词搬走、改名、或体内不再引用 `kXxxPassName`）。

    ## 盲区（写下来不装没看见）
    - **只认这一个锚点**：新装饰器若自己手写一份"找消费者"的逻辑（不调 `swapchainColorConsumerIndex`）
      就扫不到。刻意如此——那种形态本应先被 §14 的「别手写扫描、吃谓词」收掉，给它加第二条
      判据只会把两条判据的漂移空间也造出来。
    - **豁免面 = 谓词体内写了哪些 `kXxxPassName`**：有人往谓词里加一个 pass 名就同时扩大豁免面。
      但那是**改判据语义**的显式动作（且会让该装饰器拒绝"自己已在图里"，正常装配下无意义），
      不是静默漂移；review 看得见，本规则看不见。
    - 装饰器搬出 `render_graph_decorate.cppm` 会让它脱离扫描面（同时锚点消费者掉到 0 而红）——
      搬走时同步改 `DECORATION_ORDER_SOURCE_FILE`，与 §14 对提供方文件改名的处置同口径。
    """
    src = read_text(Path(ROOT) / DECORATION_ORDER_SOURCE_FILE)
    if src is None:
        return [Warning("tools/checks/config.py", 0,
            f"链装饰器次序判据对账**本次未做任何对账**：{DECORATION_ORDER_SOURCE_FILE} 读不到"
            "（改名 / 搬走了？同步改 tools/checks/config.py 的 DECORATION_ORDER_SOURCE_FILE）"
            "——「零警告」不是「每个链装饰器都申报了次序」", "error")]
    code = strip_comments(src)
    names = sorted(set(re.findall(DECORATION_ORDER_DECORATOR_RE, code, re.M)))
    if not names:
        return [Warning(DECORATION_ORDER_SOURCE_FILE, 0,
            "链装饰器次序判据对账**本次未做任何对账**：按命名约定（行首 `返回类型 decorate<大写>"
            "…Pass(`）解析出 0 个装饰器定义（约定改了？同步改 tools/checks/config.py 的 "
            "DECORATION_ORDER_DECORATOR_RE，别让这条规则静默空跑）", "error")]
    bodies = _balanced_bodies(code, names)
    anchored = [n for n in names
                if re.search(rf"\b{DECORATION_ORDER_ANCHOR}\s*\(", bodies.get(n, ""))]
    if not anchored:
        return [Warning(DECORATION_ORDER_SOURCE_FILE, 0,
            f"链装饰器次序判据对账**本次未做任何对账**：{len(names)} 个装饰器里 0 个调用锚点判据 "
            f"`{DECORATION_ORDER_ANCHOR}`（锚点改名 / 装饰器搬走了？同步改 tools/checks/config.py）"
            "——「零警告」是判据空跑的结果，不是覆盖面干净", "error")]
    predBody = _balanced_bodies(code, [DECORATION_ORDER_PREDICATE]).get(DECORATION_ORDER_PREDICATE)
    if predBody is None:
        return [Warning(DECORATION_ORDER_SOURCE_FILE, 0,
            f"链装饰器次序判据对账**本次未做任何对账**：取不到共用次序判据 `{DECORATION_ORDER_PREDICATE}`"
            f" 的函数体（搬出 {DECORATION_ORDER_SOURCE_FILE} 了？改名了？）——豁免名单由它的函数体反推，"
            "取不到就没有豁免，也没有判据", "error")]
    late = sorted(set(re.findall(DECORATION_ORDER_LATE_NAME_RE, predBody)))
    if not late:
        return [Warning(DECORATION_ORDER_SOURCE_FILE, 0,
            f"链装饰器次序判据对账**本次未做任何对账**：`{DECORATION_ORDER_PREDICATE}` 的函数体里反推"
            "出 0 个 `kXxxPassName`（判据不再按 pass 名拒绝？同步改 tools/checks/config.py 的 "
            "DECORATION_ORDER_LATE_NAME_RE）——豁免名单空 = 晚装饰器会被判裸奔，而这不是本规则想说的话",
            "error")]
    exempt = {f"decorate{n}Pass" for n in late}
    missing = [n for n in anchored if n not in exempt
               and not re.search(rf"\b{DECORATION_ORDER_PREDICATE}\s*\(", bodies[n])]
    SCAN_DECORATION_ORDER.clear()
    SCAN_DECORATION_ORDER.update({
        "decorators": len(names), "anchored": len(anchored),
        "exempt": len([n for n in anchored if n in exempt]), "missing": len(missing)})
    return [Warning(DECORATION_ORDER_SOURCE_FILE, 0,
        f"链装饰器 `{n}` 的函数体调用了锚点判据 `{DECORATION_ORDER_ANCHOR}`（= 它排在链上、插在"
        f"同一个消费者之前），却**没有**申报次序：调用序决定它的最终落点，挪错以后图 compile "
        f"照样过、无 hazard、无 warning、画面静默错（velocity 排到 ao_apply 之后 = 夹在 ao_apply "
        f"与 tonemap 之间、吃不到时域滤波，正是本条要拦的形态，实测可达）。处置二选一：① 在函数体开头调用共用判据 "
        f"`{DECORATION_ORDER_PREDICATE}` 并在命中时 return false（新装饰器**默认走这条**）；"
        f"② 本装饰器就是「必须最后装饰」的那一个 ⟹ 把它插入的 pass 的 `kXxxPassName` 加进 "
        f"`{DECORATION_ORDER_PREDICATE}` 的函数体（豁免名单由那里唯一决定）。🔴 本规则没有基线"
        "可登记。", "error") for n in missing]


# ─── 对话黑话冻结（§16；对照表 = skill rgl-glossary）─────────────────────────────

JARGON_SCAN_NOTES: dict[str, str] = {}

_JARGON_ROW_RE = re.compile(r"^\|(.+)\|\s*$")


def jargon_terms() -> tuple[dict[str, str], str | None]:
    """解析 skill `rgl-glossary` 的对照表 → ({术语: 处置}, 失效原因)。

    🔴 **表就是名单的唯一真相源**，config 里不抄词表——抄一份就得再写一条规则去对账两份副本，
    而那正是本项目反复红过的「抄名单」形态（`rgl-doc-placement` §反模式）。表第一列 = 术语
    （` / ` 分隔同义写法，各自独立成条），第三列 = 处置（`config.JARGON_DISPOSITIONS`）。

    失效一律带原因返回：表读不到 / 找不到 `## 对照表` / 解析出 0 条 / 处置不在词表里。本规则的
    判定是**数数**，名单空了不会少一条警告，而是整条规则静默变瞎——而"零警告"会被读成
    "方言没再长大"（同 §1.6 / §10 / §14 的取向）。
    """
    src = read_text(Path(ROOT) / JARGON_GLOSSARY_FILE)
    if src is None:
        return {}, f"读不到 {JARGON_GLOSSARY_FILE}"
    lines = src.splitlines()
    try:
        start = lines.index(JARGON_TABLE_HEAD)
    except ValueError:
        return {}, f"{JARGON_GLOSSARY_FILE} 里找不到「{JARGON_TABLE_HEAD}」小节（改名会让本规则静默变瞎）"
    terms: dict[str, str] = {}
    for line in lines[start + 1:]:
        if line.startswith("##"):
            break
        m = _JARGON_ROW_RE.match(line)
        if not m:
            continue
        cells = [c.strip().strip("`") for c in m.group(1).split("|")]
        if len(cells) < 3 or cells[0] in ("术语", "") or set(cells[0]) <= {"-", ":", " "}:
            continue  # 表头 / 分隔行
        if cells[2] not in JARGON_DISPOSITIONS:
            return {}, (f"「{JARGON_TABLE_HEAD}」表里「{cells[0]}」的处置是 {cells[2]!r}，"
                        f"不在 {JARGON_DISPOSITIONS} 之内（处置写错 = 棘轮面静默变化）")
        for term in (t.strip().strip("`") for t in cells[0].split("/")):
            if term:
                terms[term] = cells[2]
    if not terms:
        return {}, f"「{JARGON_TABLE_HEAD}」解析出 0 条术语（表格式变了而本规则没跟上 = 空跑）"
    return terms, None


def jargon_scan_files() -> tuple[list[str], str | None]:
    """棘轮扫描面 = **驱动对话文体的那一层**（AGENTS + docs + skills 的 .md）。

    不含 `rgl/` 与 `tools/` 的源码注释：那边这套词有局部上下文、单条读得懂，一次性清洗是纯
    churn；而对话文体只由每次开工都进上下文的那一层决定，钉那里才命中成因。
    对照表本体排除——它必须把这些词写出来才能翻译它们（否则它自己就是最大的"违规"文件）。
    """
    if missing := [f for f in JARGON_SCAN_FILES if not (Path(ROOT) / f).is_file()]:
        return [], f"扫描面白名单文件不存在：{missing}"
    out = list(JARGON_SCAN_FILES)
    for d in JARGON_SCAN_DIRS:
        base = Path(ROOT) / d
        if not base.is_dir():
            return [], f"扫描面目录不存在：{d}/"
        out += [rel_posix(p) for p in base.rglob("*")
                if p.is_file() and p.suffix in JARGON_SCAN_SUFFIXES]
    if not (out := sorted(set(out) - set(JARGON_SCAN_EXCLUDE))):
        return [], "扫描面为空"
    return out, None


def jargon_scan() -> tuple[dict[str, str], dict[str, str], str | None]:
    """跑一遍扫描面 → ({术语: 处置}, {文件: 文本}, 失效原因)；顺带记读数供收账命令自检。"""
    JARGON_SCAN_NOTES.clear()
    terms, err = jargon_terms()
    if err:
        return {}, {}, err
    files, err = jargon_scan_files()
    if err:
        return {}, {}, err
    texts: dict[str, str] = {}
    for rel in files:
        if (src := read_text(Path(ROOT) / rel)) is None:
            return {}, {}, f"扫描面文件读不到：{rel}"
        texts[rel] = src
    ratchet = sorted(t for t, d in terms.items() if d == JARGON_DISPOSITION_REPLACE)
    JARGON_SCAN_NOTES["scan"] = (f"{len(texts)} 个文件 / 棘轮词 {len(ratchet)} 条")
    return terms, texts, None


def jargon_readout() -> str:
    """最近一次扫描的覆盖面读数（空 = 扫描面没跑起来，收账命令据此**拒绝落笔**）。"""
    return JARGON_SCAN_NOTES.get("scan", "")


def jargon_counts() -> dict[str, int]:
    """逐文件计数 → `{"文件::词": 次数}`（只数处置为「换掉」的词，零次不登记）。"""
    terms, texts, _ = jargon_scan()
    ratchet = {t for t, d in terms.items() if d == JARGON_DISPOSITION_REPLACE}
    return {f"{rel}::{t}": n for rel, src in texts.items() for t in ratchet if (n := src.count(t))}


def load_jargon_baseline() -> tuple[dict[str, int], str | None]:
    """读黑话棘轮基线 → ({`文件::词`: 允许条数}, 不可用原因)。

    空表 / 坏格式一律返回原因串而不是当"零额度"用——同 `load_doc_pointer_baseline`：
    把"没数"读成"干净"就是假绿。
    """
    path = Path(ROOT) / JARGON_BASELINE_FILE
    if not path.is_file():
        return {}, f"基线文件不存在：{JARGON_BASELINE_FILE}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {}, f"基线文件读不出来：{JARGON_BASELINE_FILE}（{type(e).__name__}: {e}）"
    if not isinstance(data, dict) or not isinstance(data.get("files"), dict):
        return {}, f"基线顶层或缺 `files` 对象（格式不符）：{JARGON_BASELINE_FILE}"
    files = data["files"]
    if bad := sorted(str(k) for k, v in files.items()
                     if isinstance(v, bool) or not isinstance(v, int)):
        return {}, f"基线 `files` 有非整数条数：{JARGON_BASELINE_FILE} 的 {bad[:3]}"
    return {str(k): int(v) for k, v in files.items()}, None


def write_jargon_baseline(counts: dict[str, int]) -> str:
    """按当前条数重写棘轮基线（收账）；返回写入路径。"""
    files = {k: int(v) for k, v in sorted(counts.items())}
    path = Path(ROOT) / JARGON_BASELINE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(
        {"_comment": JARGON_BASELINE_COMMENT,
         "total": sum(files.values()), "files": files},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return JARGON_BASELINE_FILE


def jargon_ratchet_checks() -> list[Warning]:
    """冻结本项目自造隐喻的增长（warning 级）+ 护栏自己失效（error 级）。

    **立论**：这套方言不是"模型爱装"。AGENTS.md + skills 每次开工都进上下文，模型照那个文体
    回话 ⟹ **文档是方言的生成器**；而 AGENTS 的预算制（"写结论 + 指针不写论证"）让一个已付过
    钱的隐喻比一句白话便宜 ⟹ 激励正好在造词：写文档省下的力气，全部转成用户**每一次对话**的解码
    成本，且没有任何规则给这笔账定价。本条把这笔账的**增量**挂回写文档的人身上。

    **判定**（warning 级，两条）：
    ① **棘轮**：处置为「换掉」的词（纯隐喻、无代码 / 命令 / 配置锚点）在某文件的出现次数
       **> 基线** → 点名。存量钉死只准降不准升，所以今天不必清洗 866 处「红线」，但**谁也别再加**。
    ② **死词**：对照表登记了、扫描面里却 0 次 → 点名。表里留一条没人用的词 = 下一个会话照旧
       拿它当权威解释，而它解释的东西可能已经不存在（同 §1.9「副本必然漂」）。

    **失效出声**（error 级）：对照表读不到 / 小节改名 / 解析出 0 条 / 处置写错 / 扫描面起不来 /
    基线不可用——本规则是数数的，"没数"必须区别于"零新增"。

    **不判什么**：对话里的用词（回复不是仓库文件，写不出断言）。那条住全局
    `~/.config/opencode/AGENTS.md`，是注释不是防线，本条不假装它能管到对话。
    """
    terms, texts, err = jargon_scan()
    if err:
        return [Warning(JARGON_GLOSSARY_FILE, 1,
            f"黑话冻结**无法执行**（{err}）——这不是「方言没再长大」", "error")]
    warnings: list[Warning] = []
    for term, disp in sorted(terms.items()):
        if not any(term in src for src in texts.values()):
            warnings.append(Warning(JARGON_GLOSSARY_FILE, 1,
                f"对照表登记「{term}」（处置 {disp}），但扫描面 0 次——这个词已没人用（删这条）"
                "或写法变了（改表）；表里留一条没人用的词，下一个会话仍会拿它当权威解释"))
    baseline, err = load_jargon_baseline()
    if err:
        warnings.append(Warning(JARGON_GLOSSARY_FILE, 1,
            f"黑话棘轮基线不可用（{err}）——此时「零警告」不代表没新增黑话；"
            f"用 `{JARGON_UPDATE_CMD}` 重建", "error"))
        return warnings
    for rel, src in sorted(texts.items()):
        for term in sorted(t for t, d in terms.items() if d == JARGON_DISPOSITION_REPLACE):
            if (n := src.count(term)) > (allowed := baseline.get(f"{rel}::{term}", 0)):
                warnings.append(Warning(rel, 1,
                    f"新增对话黑话「{term}」：{allowed} → {n} 次。这个词在对照表里的处置是「换掉」"
                    "（纯隐喻、无代码锚点，删掉不丢信息）——**改写成本句的白话**，不是来基线里"
                    f"登记额度；白话写法 → skill rgl-glossary（真清掉一批后 `{JARGON_UPDATE_CMD}` 收账）"))
    return warnings
