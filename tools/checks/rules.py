"""全部规则判定：每条规则一个函数（样式八类 + Doxygen 一类；阈值经
config.Thresholds 传入；libclang 指标规则消费 metrics.analyze_file 的缓存指标，
纯文本/注释规则主进程直接读源码）。

样式规则（--only style|all 生效）：
- rule_file_lines      文件代码行 > file 阈值 → 拆文件
- rule_function_lines  函数代码行 > func 阈值 → 拆函数
- rule_cognitive       函数认知复杂度 > cognitive 阈值 → 拆函数（SonarSource 风格，
                       算法 = clangbind.cognitive_complexity）
- rule_impl_slim       接口瘦身：非 _impl 文件函数体 > impl 阈值 → 迁入 xxx_impl.cppm
- rule_member_naming   成员变量 _/m_ 前缀、_ 后缀 → 自然命名
- text_checks          （文本）分层零 entt + 点名禁止 import 边 + 模块优先
                       （rgl/ 禁 .h/.hpp/.hh、.cpp 白名单、禁内部头 #include "）
                       + GMF STL 白名单（只许 ranges/filesystem/chrono）
                       + 模块结构四条（partition export/主接口 import/GMF 外
                       include/vulkan.hpp 仅 rlvk）—— 见 module_structure_checks
- forbidden_vulkan_checks
                       （文本）禁用旧式 Vulkan API（经典 descriptor pool/pipeline/
                       render pass/push constants/输入装配——项目已全面 descriptor
                       heap / shader object / dynamic rendering / mesh shader）。
                       扫描先剥注释（现有注释
                       提到 vkCmdPushConstants 互斥语义，不剥会误报）；名单与精确
                       匹配护栏见 config.FORBIDDEN_VULKAN_APIS。
- modern_cpp_checks    （文本）现代 C++ 零违规护栏（goto/friend/RTTI/NULL/enum/
                       malloc/printf…）+ new/delete 豁免名单 + using/export namespace
                       豁免—— 见 docs/风格检查规则.md §2
- docs_status_checks   （文本）docs/**.md 头部状态行（`> **状态**`）—— 见 §1.5
- rule_expected_throw  （指标）返回 std::expected 的函数体内禁 throw（折中版 A：
                       双通道防混用；非构造路径请用 expected 传播）—— 见 §2.3

Doxygen 规则（--only doc|all 生效）：
- rule_doxygen         声明首行正上方须紧邻非空 /** */ 块；decls 来自
                       metrics（Indexer 采集，穿透 export——旧文本 AST 管线
                       2026-09-23 废弃）。判定细节（重载容忍/前向声明锚点/
                       impl 接口名豁免）逐条从 doccheck 移植，行为保持。
"""

import os
import re

from config import (ALLOWED_CPP, DOCS_STATUS_MAX_LINES, EXCLUDE_FILES,
                    EXPORT_NAMESPACE_EXEMPT, FORBIDDEN_EDGES,
                    FORBIDDEN_VULKAN_APIS, FORBIDDEN_VULKAN_MSG, MODERN_BANNED,
                    NEW_DELETE_EXEMPT, ROOT, STL_GMF_EXCEPTIONS, STL_HEADERS,
                    USING_NAMESPACE_EXEMPT, VULKAN_HPP_INCLUDE_FILES,
                    ZERO_ENTT_MODULES)

MODULE_LINE_RE = re.compile(r"^\s*(?:export\s+)?module\s*([\w:]+)?\s*;")
IMPORT_RE = re.compile(r"^\s*import\s+([\w:]+)\s*;")
# 两组定界符各自闭合（不能用 \1 反引用：< 的闭合符是 >）
INCLUDE_RE = re.compile(r"^\s*#include\s*(?:<([^>]+)>|\"([^\"]+)\")")


def read_text(path):
    """容错读源文件文本：非 UTF-8 字节按 replace 解码（坏字节→U+FFFD，token 匹配无害）。

    纯文本规则在主进程跑，单个畸形文件（二进制/非法编码/读失败）**绝不允许**抛异常崩掉
    整个检查——style_check 非阻断，但 uncaught 异常 exit 1 会让 ninja 误判目标失败、-k1
    stall 整条构建（同 worker BrokenProcessPool 教训）。失败返回 None，调用方跳过该文件。
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


# ─── libclang 指标规则（消费 metrics.analyze_file 的指标字典）─────────────────

def rule_file_lines(rel, metrics, threshold):
    """文件代码行超阈 → 提示拆分文件/模块。"""
    if metrics["file_code_lines"] > threshold:
        return [(rel, 0,
            f"文件代码行 {metrics['file_code_lines']} > {threshold}"
            f"（去注释空行，总行数 {metrics['total_lines']}）——应拆分文件/模块")]
    return []


def rule_function_lines(rel, fn, threshold):
    """函数代码行超阈 → 提示拆分函数。"""
    if fn["code_lines"] > threshold:
        return [(rel, fn["line"],
            f"函数 '{fn['name']}' 代码行 {fn['code_lines']} > {threshold}"
            f"（去注释空行）——应拆分为更小函数")]
    return []


def rule_cognitive(rel, fn, threshold):
    """函数认知复杂度超阈 → 提示拆函数（降低嵌套/早返回/抽函数）。"""
    if fn["cognitive"] > threshold:
        return [(rel, fn["line"],
            f"函数 '{fn['name']}' 认知复杂度 {fn['cognitive']} > {threshold}"
            f"——应拆分为更小函数（降低嵌套/早返回/抽函数）")]
    return []


def rule_impl_slim(rel, fn, threshold):
    """接口瘦身（AGENTS.md"接口瘦身"）：非 _impl 接口单元里 >阈值 代码行的函数
    定义应迁入 xxx_impl.cppm（接口 BMI 不含函数体，所有 importer 受益）。"""
    if not rel.endswith("_impl.cppm") and fn["code_lines"] > threshold:
        target = os.path.basename(rel)[:-len(".cppm")] + "_impl.cppm"
        return [(rel, fn["line"],
            f"函数 '{fn['name']}' 函数体代码行 {fn['code_lines']} > {threshold}"
            f"——接口瘦身：实现应迁入 {target}（接口 BMI 不含函数体）")]
    return []


def rule_member_naming(rel, fields):
    """成员变量命名：_/m_ 前缀或 _ 后缀 → 自然命名。"""
    warnings = []
    for fd in fields:
        n = fd["name"]
        if n.startswith("_") or n.startswith("m_") or n.endswith("_"):
            warnings.append((rel, fd["line"],
                f"成员变量 '{n}'——成员命名不加 _/m_ 前缀、不加 _ 后缀（自然命名）"))
    return warnings


def rule_expected_throw(rel, fn):
    """折中版 A：返回 std::expected 的函数体内禁 throw（双通道防混用）。

    非构造/启动 fail-fast 路径请改用 std::expected 传播错误；构造函数无返回
    类型不触发本规则。见 docs/风格检查规则.md §2.3。
    """
    if fn.get("returns_expected") and fn.get("throws"):
        return [(rel, fn["line"],
            f"函数 '{fn['name']}' 返回 std::expected 却在体内 throw——"
            f"双通道混用；非构造/启动 fail-fast 路径请改用 expected 传播错误")]
    return []


def evaluate_metrics(rel, metrics, thresholds):
    """单文件全部 libclang 指标规则（顺序：文件行→逐函数[行/认知/瘦身/expected
    throw]→成员命名；同 (rel,line) 并列时保持此相对顺序——排序在 main 里按
    (rel,line) 稳定排）。"""
    warnings = rule_file_lines(rel, metrics, thresholds.file)
    for fn in metrics["functions"]:
        warnings += rule_function_lines(rel, fn, thresholds.func)
        warnings += rule_cognitive(rel, fn, thresholds.cognitive)
        warnings += rule_impl_slim(rel, fn, thresholds.impl)
        warnings += rule_expected_throw(rel, fn)
    warnings += rule_member_naming(rel, metrics["fields"])
    return warnings


# ─── 纯文本：模块声明/import/include 状态机解析 ──────────────────────────────

def parse_module_file(src):
    """按行状态机解析：GMF = 裸 module; 之后、具名模块声明之前。

    返回 (模块名, {import: 行号}, gmf_incs=[(include, 行号, 定界符)],
    post_incs=[(include, 行号, 定界符)], module_line=int|None, is_partition=bool)。
    post_incs = 具名模块声明之后的 #include（违规盲区，module_structure 消费）；
    is_partition = `module X:part;`（非 export module 主接口）。
    """
    name, imps, gmf_incs, post_incs = None, {}, [], []
    in_gmf, named, is_partition = False, False, False
    module_line = None
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
    return name, imps, gmf_incs, post_incs, module_line, is_partition


def is_internal_header(rel, inc):
    """引号 include 能否解析到仓库内文件 = 内部头（模块优先违规）。

    外部 SDK 头（如 Aftermath GFSDK_*.h，include 目录在仓库外）解析不到 → 不误报
    （引号 include 本身不违规，违规的是"内部头文件存在"——模块间只走 import）。
    """
    root_abs = os.path.realpath(ROOT)
    base = os.path.dirname(os.path.join(ROOT, rel))
    for cand in (os.path.join(base, inc), os.path.join(ROOT, inc)):
        p = os.path.realpath(os.path.normpath(cand))
        if os.path.isfile(p) and p.startswith(root_abs + os.sep):
            return True
    return False


def rgl_cppm_files():
    """rgl/ 下全部 .cppm（含 rlvk，排除 generated/），仓库相对 POSIX 路径。"""
    out = []
    base = os.path.join(ROOT, "rgl")
    for dirpath, dirnames, filenames in os.walk(base):
        norm = dirpath.replace(os.sep, "/")
        if "/generated" in norm:
            dirnames[:] = []
            continue
        for fn in filenames:
            if fn.endswith(".cppm"):
                out.append(os.path.relpath(os.path.join(dirpath, fn), ROOT)
                           .replace(os.sep, "/"))
    return sorted(out)


# ─── 纯文本：分层零 entt + 禁止 import 边 + 模块优先 + GMF STL 白名单 ────────

def text_checks():
    """A3 分层依赖 + B1 模块优先 + B2 GMF STL 白名单 + 模块结构四条。
    返回 [(rel, line, msg)]。"""
    warnings = []
    mods = {}
    for rel in rgl_cppm_files():
        src = read_text(os.path.join(ROOT, rel))
        if src is None:
            continue
        name, imps, gmf_incs, post_incs, module_line, is_partition = parse_module_file(src)
        if name is None:
            continue
        mods[name] = (rel, imps, gmf_incs)
        # B1：内部头 include（模块优先：模块间只走 import）。外部 SDK 头（仓库外，
        # 如 Aftermath GFSDK_*.h）解析不到仓库内文件 → 不报（is_internal_header）。
        for inc, i, delim in gmf_incs:
            if delim == '"' and is_internal_header(rel, inc):
                warnings.append((rel, i,
                    f'GMF #include "{inc}"——模块优先：模块间依赖只走 import，'
                    f"内部头文件不应存在"))
        # B2：GMF 的 STL 头白名单
        for inc, i, delim in gmf_incs:
            if delim != "<" or "/" in inc:
                continue
            if inc in STL_HEADERS and inc not in STL_GMF_EXCEPTIONS:
                warnings.append((rel, i,
                    f"GMF #include <{inc}>——STL 走 import rlstd"
                    f"（例外仅 ranges/filesystem/chrono；rlstd 缺名则补入 rlstd.cppm）"))
        # 模块结构：GMF 外 include + partition 四条（见 docs/风格检查规则.md §1）
        warnings.extend(_module_structure_checks(
            rel, name, imps, post_incs, module_line, is_partition, src))
    # A3：底层零 entt + 点名禁止边
    for name, (rel, imps, gmf_incs) in mods.items():
        if name in ZERO_ENTT_MODULES:
            if "ecs" in imps:
                warnings.append((rel, imps["ecs"],
                    f"模块 '{name}' import ecs——底层渲染层不得依赖 ECS"
                    f"（entity/component/system 归场景层，见 AGENTS.md 分层）"))
            for inc, i, _ in gmf_incs:
                if inc.startswith("entt/"):
                    warnings.append((rel, i,
                        f"模块 '{name}' GMF #include <{inc}>"
                        f"——底层渲染层零 entt（见 AGENTS.md 分层）"))
        for target in sorted(FORBIDDEN_EDGES.get(name, ())):
            if target in imps:
                warnings.append((rel, imps[target],
                    f"模块 '{name}' import '{target}'——禁止依赖边"
                    f"（见 AGENTS.md 构造依赖图原则）"))
    # B1：rgl/ 下 .h/.hpp/.hh/.cpp 文件
    base = os.path.join(ROOT, "rgl")
    for dirpath, dirnames, filenames in os.walk(base):
        norm = dirpath.replace(os.sep, "/")
        if "/generated" in norm:
            dirnames[:] = []
            continue
        for fn in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, "/")
            if fn.endswith((".h", ".hpp", ".hh")):
                warnings.append((rel, 0,
                    "头文件——模块优先：所有新代码必须 C++20 modules (.cppm)"))
            elif fn.endswith(".cpp") and fn not in ALLOWED_CPP:
                warnings.append((rel, 0,
                    ".cpp 文件——模块优先：仅单头库实现单元（tinygltf_impl/vam_impl）"
                    "允许 .cpp"))
    return warnings


def _module_structure_checks(rel, name, imps, post_incs, module_line, is_partition, src):
    """模块结构四条（§1.1–1.4）：partition export / 主接口 import / GMF 外 include /
    vulkan.hpp 仅 rlvk。name 形如 `foo:impl`（partition）或 `foo`（主接口）。"""
    warnings = []
    # §1.4 vulkan C++ 头仅 rlvk
    if rel not in VULKAN_HPP_INCLUDE_FILES:
        for inc, i, delim in post_incs:
            if inc in ("vulkan/vulkan.hpp", "vulkan/vulkan_raii.hpp"):
                warnings.append((rel, i,
                    f"#include <{inc}>——Vulkan C++ 头一律 import rlvk"
                    f"（仅 rlvk.cppm 可 include hpp；C 头 vulkan.h 仍允许）"))
    # §1.3 具名模块声明之后的任何 #include（GMF 外）
    for inc, i, delim in post_incs:
        warnings.append((rel, i,
            f"模块声明后 #include <{inc}>——GMF 外禁 #include；"
            f"第三方/STL 头放 GMF（裸 module; 之后、具名 module 之前）"))
    if not is_partition or module_line is None:
        return warnings
    # 主接口名 = partition 前缀（name = base:part）
    base = name.split(":", 1)[0]
    if base not in imps:
        warnings.append((rel, module_line,
            f"partition '{name}' 缺 import {base}——"
            f"clang 不给 partition impl 隐式 import 主接口，必须显式"))
    # §1.1 partition 实现单元禁 export 声明（export import/using 放行）
    lines = src.splitlines()
    for i, ln in enumerate(lines, 1):
        if i <= module_line:
            continue
        if re.match(r"^export\s+", ln) and not re.match(
                r"^export\s+(import|using|module)\b", ln):
            warnings.append((rel, i,
                f"partition '{name}' 实体内带 export——"
                f"导出实体在实现单元定义不得带 export（接口已声明，三条坑 #3）"))
    return warnings


# ─── 现代 C++ 零违规回归护栏 + new/delete + using/export namespace ────────────

_MODERN_REGEXES = [(msg, re.compile(rx)) for rx, msg in MODERN_BANNED]
_NEW_RX = re.compile(r"\bnew\s+[A-Za-z_(]")
_DELETE_RX = re.compile(r"(?<![=\w])\bdelete\s+(?!default\b)")
_USING_NS_RX = re.compile(r"^using namespace\s+", re.M)
_EXPORT_NS_RX = re.compile(r"^export namespace\s+", re.M)


def modern_cpp_checks():
    """§2.1–2.2：剥注释后 token 级现代 C++ 护栏 + new/delete 豁免 + namespace 豁免。
    返回 [(rel, line, msg)]。"""
    warnings = []
    for rel in rgl_cppm_files():
        if rel in EXCLUDE_FILES:
            continue
        src = read_text(os.path.join(ROOT, rel))
        if src is None:
            continue
        clean = strip_comments(src)
        lines = clean.splitlines()
        for lineno, line in enumerate(lines, 1):
            for msg, rx in _MODERN_REGEXES:
                if rx.search(line):
                    warnings.append((rel, lineno, f"{msg}"))
        if rel not in NEW_DELETE_EXEMPT:
            for lineno, line in enumerate(lines, 1):
                if _NEW_RX.search(line):
                    warnings.append((rel, lineno,
                        "new 表达式——禁裸 new（分配器/C API 包装走豁免名单 "
                        "NEW_DELETE_EXEMPT；其余用 make_unique/make_shared/RAII）"))
                if _DELETE_RX.search(line):
                    warnings.append((rel, lineno,
                        "delete 表达式——禁裸 delete（= delete 特殊成员删除不命中；"
                        "分配器 deleter 走 NEW_DELETE_EXEMPT）"))
        if rel not in USING_NAMESPACE_EXEMPT:
            for m in _USING_NS_RX.finditer(clean):
                lineno = clean[:m.start()].count("\n") + 1
                warnings.append((rel, lineno,
                    "using namespace——仅入口 main_impl 豁免；模块内用限定名"))
        if rel not in EXPORT_NAMESPACE_EXEMPT:
            for m in _EXPORT_NS_RX.finditer(clean):
                lineno = clean[:m.start()].count("\n") + 1
                warnings.append((rel, lineno,
                    "export namespace——仅 rlvk 的 vk re-export 豁免"))
    return warnings


# ─── docs/** 状态头（§1.5）────────────────────────────────────────────────────

_DOCS_STATUS_RX = re.compile(r"^(?:>\s*)?\**状态")


def docs_status_checks():
    """docs/（含 archive/）.md 前 DOCS_STATUS_MAX_LINES 行内须有状态行。
    接受 `> **状态**：` / `> 状态：` / 行首 `状态：`（含 `**状态（日期）**` 变体）。"""
    warnings = []
    base = os.path.join(ROOT, "docs")
    for dirpath, dirnames, filenames in os.walk(base):
        for fn in sorted(filenames):
            if not fn.endswith(".md"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, "/")
            src = read_text(os.path.join(ROOT, rel))
            if src is None:
                continue
            head = src.splitlines()[:DOCS_STATUS_MAX_LINES]
            if any(_DOCS_STATUS_RX.match(ln.strip()) for ln in head):
                continue
            warnings.append((rel, 1,
                f"缺状态行——docs 头部前 {DOCS_STATUS_MAX_LINES} 行须有 "
                f"`> **状态**：现行/历史/调研/…`（见 AGENTS.md 其他文档）"))
    return warnings


# ─── 纯文本：禁用旧式 Vulkan API（回归护栏）──────────────────────────────────

def strip_comments(src):
    """剥离 // 行注释与 /* */ 块注释（字符置空格、换行保留——行号不漂移）。

    字符串/字符字面量内的 // 与 /* 不进注释态（单遍状态机，简单实现；
    raw string R"(...)" 不特殊处理——当前 rgl/*.cppm 无出现，若未来引入
    含 vk 旧 API 字样的 shader 内嵌源再补）。"""
    out = []
    i, n = 0, len(src)
    state = 0  # 0=code 1=line-comment 2=block-comment 3=string 4=char
    while i < n:
        c = src[i]
        d = src[i + 1] if i + 1 < n else ""
        if state == 0:
            if c == "/" and d == "/":
                state = 1; out.append("  "); i += 2; continue
            if c == "/" and d == "*":
                state = 2; out.append("  "); i += 2; continue
            if c == '"':
                state = 3
            elif c == "'":
                state = 4
            out.append(c); i += 1
        elif state == 1:
            if c == "\n":
                state = 0; out.append(c)
            else:
                out.append(" ")
            i += 1
        elif state == 2:
            if c == "*" and d == "/":
                state = 0; out.append("  "); i += 2
            else:
                out.append("\n" if c == "\n" else " "); i += 1
        elif state == 3:
            if c == "\\":
                out.append(c + d); i += 2; continue
            if c == '"':
                state = 0
            out.append(c); i += 1
        else:  # state == 4（char literal）
            if c == "\\":
                out.append(c + d); i += 2; continue
            if c == "'":
                state = 0
            out.append(c); i += 1
    return "".join(out)


def _build_forbidden_regexes():
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
    out = []
    for tok in FORBIDDEN_VULKAN_APIS:
        if tok.endswith("*"):
            out.append((tok, re.compile(r"(?<![A-Za-z0-9_])" + re.escape(tok[:-1]))))
            continue
        esc = re.escape(tok)
        right = "" if not (tok[-1].isalnum() or tok[-1] == "_") else r"(?![A-Za-z0-9_])"
        out.append((tok, re.compile(r"(?<![A-Za-z0-9_])" + esc + right)))
    return out


FORBIDDEN_REGEXES = _build_forbidden_regexes()


def forbidden_vulkan_checks():
    """禁用旧式 Vulkan API 扫描：全部 rgl/*.cppm（排除 rlvk 包装层与 generated/），
    剥注释后按行匹配名单。返回 [(rel, line, msg)]。"""
    warnings = []
    for rel in rgl_cppm_files():
        if rel in EXCLUDE_FILES:
            continue
        src = read_text(os.path.join(ROOT, rel))
        if src is None:
            continue
        for lineno, line in enumerate(strip_comments(src).splitlines(), 1):
            for tok, rx in FORBIDDEN_REGEXES:
                if rx.search(line):
                    warnings.append((rel, lineno, f"{tok}——{FORBIDDEN_VULKAN_MSG}"))
    return warnings


# ─── Doxygen 注释规则（原 doccheck 移植；decls = metrics Indexer 采集）─────────

def _unstar(t):
    """去掉 Doxygen 续行前导 * 后取内容。"""
    if t.startswith("*"):
        t = t[1:]
    return t.strip()


def _block_has_content(lines, start, end):
    """lines[start]=/** 行、lines[end]=块末行：是否有非空注释内容。

    末行两种形态：单独 */ 或 `* 文字 */`（*/ 尾随）；首行 `/** 文字` 同理。
    """
    ts = lines[start].strip()
    if ts.startswith("/**") and len(ts) > 3:
        return True
    te = lines[end].strip()
    if te.endswith("*/") and te != "*/" and _unstar(te[:-2]):
        return True
    for i in range(start + 1, end):
        t = lines[i].strip()
        if t != "*" and _unstar(t):
            return True
    return False


def _is_fwd_line(t, name):
    """同名实体的前向声明行（`export class X;` / 函数原型）——前向声明与
    定义之间可夹空行，注释块挂在前向声明上方（Renderer 先例）。"""
    if not t.endswith(";"):
        return False
    m = re.match(r"^(?:export\s+)?(?:class|struct|enum|union)\s+(\w+)\b", t)
    if m:
        return m.group(1) == name
    return bool(re.search(r"\b%s\s*\(" % re.escape(name), t))


def has_doxygen_above(lines, start_line, max_gap=0, name=None):
    """声明首行（1-based）正上方是否有非空 /** ... */ 块。

    规则：向上可跳过空行、template 引入行（Indexer 的函数 range start 从返回
    类型起算，`template<...>` 行在首行之上而项目惯例把注释挂在 template 前）
    与同名前向声明行（合计至多 max_gap+2 行，覆盖"注释→前向声明→空行→定义"
    形态）；块首行须以 /** 起始（多行块末行为单独 */ 或 `* 文字 */`，
    或单行 /** ... */）；块内容非空。
    """
    i = start_line - 2
    skipped = 0
    budget = max_gap + 2
    while i >= 0:
        t = lines[i].strip()
        if (t == "" or t.startswith("template")
                or (name and _is_fwd_line(t, name))):
            skipped += 1
            if skipped > budget:
                return False
            i -= 1
            continue
        break
    if i < 0:
        return False
    s = lines[i].strip()
    if s == "*/" or (s.endswith("*/") and s.startswith("*")):
        j = i - 1
        while j >= 0:
            t = lines[j].strip()
            if t == "/**" or t.startswith("/**"):
                return _block_has_content(lines, j, i)
            j -= 1
        return False
    if s.startswith("/**") and s.endswith("*/"):
        return len(s) > len("/** */")
    return False


def interface_declared_names(metrics):
    """接口单元声明名集合（自由函数 + 类方法名）——impl 定义处跨查依据。

    注释单一真相源 = 接口声明处；impl 单元里接口已声明函数的定义
    不重复要求 /** 块（名字级匹配，同名 impl 本地函数会被误豁免——罕见，
    属可接受启发式）。
    """
    return {d["n"] for d in metrics.get("decls", []) if d["k"] in ("function", "method")}


def rule_doxygen(rel, metrics, lines, max_gap=0, skip_names=None):
    """单文件 Doxygen 检查：返回 (warnings, checked, unverified, skipped_iface)。

    warnings 条目 = (rel, line, msg)；decls 缺失（老缓存/解析失败）→ 全 0。
    重载容忍（Doxygen 惯例：一组重载共用最上方一个注释块）：同名相邻
    （≤4 行）上方声明有注释 → 本条视为已覆盖。
    """
    decls = [(d["k"], d["n"], d["l"]) for d in metrics.get("decls", [])
             if d["n"] and 1 <= d["l"] <= len(lines)]
    name_lines = {}
    for k, n, l in decls:
        if k in ("function", "method"):
            name_lines.setdefault(n, []).append(l)
    warnings, checked, unverified, skipped = [], 0, 0, 0
    for kind, name, line in decls:
        if (skip_names is not None and kind in ("function", "method")
                and name in skip_names):
            skipped += 1
            continue
        # 行号健全性：名字须出现在声明首行起的 3 行窗口内（多行声明容忍）
        window = " ".join(lines[line - 1:line + 2])
        if name not in window:
            unverified += 1
            continue
        checked += 1
        if has_doxygen_above(lines, line, max_gap=max_gap, name=name):
            continue
        covered = any(
            0 < line - other <= 4
            and has_doxygen_above(lines, other, max_gap=max_gap, name=name)
            for other in name_lines.get(name, []))
        if not covered:
            warnings.append((rel, line,
                             f"{kind} '{name}' 缺 Doxygen 注释"
                             f"（声明正上方需 /** ... */ 块）"))
    return warnings, checked, unverified, skipped
