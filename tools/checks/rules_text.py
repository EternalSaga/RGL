"""纯文本样式规则：模块结构解析 + 分层/模块优先 + modern C++ + docs 状态头 +
旧式 Vulkan API 名单（主进程直读源码，不走 libclang）。

- text_checks：A3 分层零 entt + 点名禁止 import 边 + 模块优先（rgl/ 禁 .h/.hpp/.hh、
  .cpp 白名单、禁内部头 #include "）+ GMF STL 白名单 + 模块结构四条
- modern_cpp_checks / docs_status_checks / forbidden_vulkan_checks /
  agents_doc_index_checks / doc_pointer_checks：见各自 docstring 与
  docs/风格检查规则.md §1–§2
"""

import re
from pathlib import Path
from typing import NamedTuple

from config import (ALLOWED_CPP, AGENTS_FILE, AGENTS_INDEX_EXEMPT_DIRS,
                    DOC_POINTER_REF_ROOTS, DOCS_STATUS_MAX_LINES, EXCLUDE_FILES,
                    EXPORT_NAMESPACE_EXEMPT, FORBIDDEN_EDGES,
                    FORBIDDEN_VULKAN_APIS, FORBIDDEN_VULKAN_MSG, JUDGMENTS_TABLE_GLOB,
                     MODERN_BANNED,
                    NEW_DELETE_EXEMPT, RETIRED_DOC_NAMES, ROOT, STL_GMF_EXCEPTIONS,
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
    """AGENTS.md「其他文档」与 docs/ **双向**对账——同一份清单在两处维护 = 双源漂移。

    **正查**（漏索引）：docs/ 下每个 .md（除 AGENTS_INDEX_EXEMPT_DIRS）的文件名须出现在
    AGENTS.md。漏索引的代价不是"少一行"而是**新会话永远找不到它**：AGENTS 是 agent 唯一
    的开机路由表，没被索引的文档等于不存在（实测：执行中的 docs/taa_plan.md 全仓 0 处
    AGENTS 引用，而 AGENTS 把"下一步"指向了 phase12_plan）。
    按**文件名**匹配而非整路径：AGENTS 既写 `docs/x.md` 也写裸 `x.md`（§引用形如
    `技术概念.md §RPC` 不该被要求带目录前缀）；docs/ 非豁免文件基名唯一（实测），不致误配。

    **反查**（悬空指针）：AGENTS 里写出的 `docs/**.md` 磁盘上须存在——文档撒谎比缺文档更贵，
    error 级。通配写法（`docs/archive/*_已删除.md`）不作单文件判定。

    AGENTS.md 读不到 → error 级**出声**而不是静默通过（"零警告"会被读成"索引干净"，
    同 rules_layers.py 失效也报 error 的取向）。
    """
    agents = read_text(Path(ROOT) / AGENTS_FILE)
    if agents is None:
        return [Warning(AGENTS_FILE, 1,
            f"读不到 {AGENTS_FILE}——文档索引对账**无法执行**（这不是「索引干净」）", "error")]
    warnings: list[Warning] = []
    for rel in docs_markdown_files():
        if any(rel.startswith(d) for d in AGENTS_INDEX_EXEMPT_DIRS):
            continue
        if rel.rsplit("/", 1)[-1] not in agents:
            warnings.append(Warning(rel, 1,
                "未入 AGENTS.md「其他文档」索引——新会话按 AGENTS 路由找不到本文档；"
                "已结题就搬进 docs/archive/ 并同步其 README"))
    for path in sorted(set(_AGENTS_DOC_PATH_RX.findall(agents))):
        if "*" in path or "~" in path:
            continue
        if not (Path(ROOT) / path).exists():
            warnings.append(Warning(AGENTS_FILE, 1,
                f"索引里的 {path} 磁盘上不存在——悬空指针（先补目的地再修指针，"
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

    两条判定（均 error 级）：
    1. **悬空**：`<项目目录>/**.md` 磁盘上不存在。含通配（`*`）或 `~` 的写法跳过——
       那是模式不是路径（`docs/archive/*_已删除.md`）。
    2. **退役**：出现 `config.RETIRED_DOC_NAMES` 里的文件名（裸名或带目录都算）。
       删文档的复发形态是"从 git 捞回来重建一份"，只查悬空查不到（文件一回来指针就"有效"了），
       故退役名单单独点名，并在消息里给出**现在的归宿**。

    扫描面/排除面见 `config.DOC_POINTER_*`（tools/checks/ 自身不扫：规则必然写出被禁名）。
    """
    warnings: list[Warning] = []
    for rel in doc_pointer_files():
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


def meshlet_spec_checks() -> list[Warning]:
    """meshlet 路径规格：schema 常量 ↔ .slang 字面量 ↔ 关系式，三方对账。

    ## 为什么反射不够、必须有这条
    `tools/generator.cppm` 对 constants 的原话是「反射端无对应数据，直接由 schema 声明生成」
    ⟹ schema 里的 `constants` **反射不校验**，它只是 C++ 侧的单一声明点。而 meshlet 这批数
    （每任务三角数 85、簇上限 128、段步长 44/48）真正的消费者是**着色器字面量**与
    `render_list_impl.cppm` 的 task 数——schema 漂了反射一声不吭。本规则把"schema 是真相源"
    这句话变成构建期可执行的。

    ## 三段判据（缺一段就有一类漂移隐身）
    1. **数值对账**：schema 常量名若在任一 `.slang` 里以 `const uint <名> = <值>u;` 出现，
       值必须相等（改了 shader 没改 schema、或反之，都红）。
    2. 🔴 **关系对账**（比数值更要命）：`3 × kTaskTris ≤ kMeshletMaxOutputVertices`。
       去索引发射下每任务顶点数 = 3×三角数；这条破了 = mesh shader **越界写顶点输出**，
       不报错、只画垃圾或崩。数值各自"看着都对"而关系破了是真实形态（85→86 就是）。
    3. **死常量**：schema 常量既不出现在任何 `.slang`、也不被任何非生成 `.cppm` 使用
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
