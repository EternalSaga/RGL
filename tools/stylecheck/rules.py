"""全部规则判定：每条规则一个函数，返回 [(rel, line, msg)]（无命中返回 []）。

八类规则（阈值经 config.Thresholds 传入；libclang 指标规则消费 metrics.analyze_file
的缓存指标，纯文本规则主进程直接读源码）：
- rule_file_lines      文件代码行 > file 阈值 → 拆文件
- rule_function_lines  函数代码行 > func 阈值 → 拆函数
- rule_cognitive       函数认知复杂度 > cognitive 阈值 → 拆函数（SonarSource 风格，
                       算法 = clangbind.cognitive_complexity）
- rule_impl_slim       接口瘦身：非 _impl 文件函数体 > impl 阈值 → 迁入 xxx_impl.cppm
- rule_member_naming   成员变量 _/m_ 前缀、_ 后缀 → 自然命名
- text_checks          （文本）分层零 entt + 点名禁止 import 边 + 模块优先
                       （rgl/ 禁 .h/.hpp/.hh、.cpp 白名单、禁内部头 #include "）
                       + GMF STL 白名单（只许 ranges/filesystem/chrono）
- forbidden_vulkan_checks
                       （文本）禁用旧式 Vulkan API（经典 descriptor pool/pipeline/
                       render pass/push constants——项目已全面 descriptor heap /
                       shader object / dynamic rendering）。扫描先剥注释（现有注释
                       提到 vkCmdPushConstants 互斥语义，不剥会误报）；名单与精确
                       匹配护栏见 config.FORBIDDEN_VULKAN_APIS。
"""

import os
import re

from config import (ALLOWED_CPP, EXCLUDE_FILES, FORBIDDEN_EDGES,
                    FORBIDDEN_VULKAN_APIS, FORBIDDEN_VULKAN_MSG, ROOT,
                    STL_GMF_EXCEPTIONS, STL_HEADERS, ZERO_ENTT_MODULES)

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


def evaluate_metrics(rel, metrics, thresholds):
    """单文件全部 libclang 指标规则（顺序：文件行→逐函数[行/认知/瘦身]→成员命名；
    同 (rel,line) 并列时保持此相对顺序——排序在 main 里按 (rel,line) 稳定排）。"""
    warnings = rule_file_lines(rel, metrics, thresholds.file)
    for fn in metrics["functions"]:
        warnings += rule_function_lines(rel, fn, thresholds.func)
        warnings += rule_cognitive(rel, fn, thresholds.cognitive)
        warnings += rule_impl_slim(rel, fn, thresholds.impl)
    warnings += rule_member_naming(rel, metrics["fields"])
    return warnings


# ─── 纯文本：模块声明/import/include 状态机解析 ──────────────────────────────

def parse_module_file(src):
    """按行状态机解析：GMF = 裸 module; 之后、具名模块声明之前。

    返回 (模块名, {import: 行号}, [(include 路径, 行号, 定界符)])。
    """
    name, imps, gmf_incs = None, {}, []
    in_gmf = False
    for i, ln in enumerate(src.splitlines(), 1):
        m = MODULE_LINE_RE.match(ln)
        if m:
            if m.group(1) is None:
                in_gmf = True
            else:
                name = m.group(1)
                in_gmf = False
            continue
        if in_gmf:
            m = INCLUDE_RE.match(ln)
            if m:
                path = m.group(1) if m.group(1) is not None else m.group(2)
                delim = "<" if m.group(1) is not None else '"'
                gmf_incs.append((path, i, delim))
            continue
        m = IMPORT_RE.match(ln)
        if m and m.group(1) not in imps:
            imps[m.group(1)] = i
    return name, imps, gmf_incs


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
    """A3 分层依赖 + B1 模块优先 + B2 GMF STL 白名单。返回 [(rel, line, msg)]。"""
    warnings = []
    mods = {}
    for rel in rgl_cppm_files():
        src = read_text(os.path.join(ROOT, rel))
        if src is None:
            continue
        name, imps, gmf_incs = parse_module_file(src)
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
    - 以 '(' 结尾的 token（pushConstants(）本身即调用签名，只加左边界。
    精确串互不包含现代名（VkDescriptorSetAndBindingMappingEXT 等均不命中）。"""
    out = []
    for tok in FORBIDDEN_VULKAN_APIS:
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
