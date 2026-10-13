"""ECS 五条设计规范的机器护栏（债-14；AGENTS.md §ECS 设计规范）。

## 本模块判什么、不判什么（先读这段再改代码）

五条规范里**静态可判的只有三个片段**，逐条的裁决与依据（全仓实测，2026-10-14）：

- **规范 1（registry 是场景唯一数据源）：不判，连报告级都不做。**
  "场景状态"与"资产数据（合法住 AssetTable）/每帧 context（合法住 registry）"是语义区分，
  任何语法近似都会把 AssetTable、Renderer 成员、main 装配层局部量全部报成违规 = 纯噪声。
  它的两个常见违规形态（全局变量平行维护、组件直接持资源）恰好被 R-EC4 / R-EC1 间接覆盖。
- **规范 2（system 化、执行顺序显式）：只判"不用 entt::scheduler"这一条红线（R-EC3）。**
  AGENTS 规范 2 括号里写死了这条拍板（entt 3.16 的 scheduler 是 process 调度器，
  执行顺序无保证，见 ecs_compliance_review 处置记录 2026-08-24）。"system 是逻辑唯一载体"
  判不了——"业务逻辑"与"装配/控制流"语法上不可区分。
- **规范 3（构成数据驱动）：判"全局可变状态"这条腿（R-EC4）。**
  "不用全局变量决定构成"静态可判：namespace 作用域可变变量 = 全局可变状态。
  "config 字段/构造参数决定构成"判不了——config 读进 context 是**合法现行形态**
  （render_list import shadow_config/runtime_config 读级联数就是合法的），
  拿 import 边近似 = 误报一片。
- **规范 4（分层）：已有护栏**（ZERO_ENTT_MODULES + FORBIDDEN_EDGES + libcore_closure），不重复。
- **规范 5（资产与实例分离）：判"组件是纯数据"（R-EC1）+ 组件定义位（R-EC2）。**
  ⚠ **诚实边界**：`MeshRef.asset`（资产表下标，合法）与旧 `primitiveIndex`（侧数组下标，违规）
  都是 uint32_t——**下标 vs ID 的区分静态不可判**（区别在被索引的容器，是语义）。
  本规则判的是它的必要条件：组件字段必须纯数据（无指针/引用/智能指针/std::function/Vulkan 句柄）
  ——组件直接持资源正是"实例与资产融合"的形态。R-EC2 把"组件集合"钉死在契约模块
  scene_context.cppm，**没有它 R-EC1 的扫描面是漏的**（新组件定义在别处 = 纯度检查静默漏检）。

## 失效必须出声（同 rules_layers / rules_reflection / doc_pointer 的纪律）

- 契约模块读不到 / 一个 export struct 都没解析到 / 组件集合收不到任何名字 ⟹ error。
- R-EC4 存量走棘轮基线（`config.ECS_GLOBAL_BASELINE_FILE`，机制先例 = doc_pointer §1.7）：
  基线不可用 = error 声明"本次未做任何对账"，绝不把"零警告"读成"没有全局可变状态"。

## R-EC4 的判据形态（列0 启发式，绑本仓排版风格）

本仓 namespace 体**不缩进**（成员/函数体缩进），故"剥注释后列 0 的变量定义行"= namespace 作用域
变量定义。已知盲区（写下来，不装没看见）：跨行声明的续行（类型与名字分行）不收；
类内 static 成员与函数体不列（那是类作用域，形态与"全局变量"不同病）；
`T name(args);` 直接初始化不收——它与函数声明 `bool f(int);` 语法同形（most vexing parse），
保守判没；`T name{args};` 花括号初始化**收**。
"""

import json
import re
from pathlib import Path

from config import (ECS_BANNED_TOKENS, ECS_COMPONENT_MODULE,
                    ECS_GLOBAL_BASELINE_COMMENT, ECS_GLOBAL_BASELINE_FILE,
                    ECS_GLOBAL_UPDATE_CMD, ROOT)
from fs import rgl_cppm_files, read_text
from models import Warning
from rules_text import strip_comments

# ─── 共用：契约模块的 export struct 体 ────────────────────────────────────────

_STRUCT_OPEN = re.compile(r"^export struct (\w+)\b")


def _component_structs(rel: str) -> tuple[dict[str, list[tuple[int, str]]], str | None]:
    """契约模块 → ({struct 名: [(行号, 行文本) 字段行]}, 失效原因)。

    体边界 = `export struct X` 行起、列 0 `};` 止（本仓排版：struct 顶格、闭括号顶格）。
    字段行 = 体内以 `;` 结尾的行；方法/构造器判定**先剥尖括号**（`std::function<void()> cb;`
    是字段不是方法——阳性锚点当场抓出过没剥时的假绿），剥后仍含 `(` 的才算方法行放行
    （"纯数据"约束的是字段类型，不是禁构造器；Bounds(c,r) 是合法形态）。
    """
    src = read_text(Path(ROOT) / rel)
    if src is None:
        return {}, f"契约模块 {rel} 读不到"
    clean = strip_comments(src)
    structs: dict[str, list[tuple[int, str]]] = {}
    cur: str | None = None
    for i, ln in enumerate(clean.splitlines(), 1):
        if cur is None:
            m = _STRUCT_OPEN.match(ln)
            if m:
                structs[m.group(1)] = []
                cur = m.group(1)
            continue
        if ln.startswith("};"):
            cur = None
            continue
        s = ln.strip()
        if not s or not s.endswith(";"):
            continue
        # 方法/构造器判定要**先剥尖括号**：`std::function<void()> cb;` 是字段不是方法
        # （函数指针声明形态 `void (*fp)(int);` 剥后仍含括号，但它是被禁字段，放行到判据）。
        stripped = _strip_angles(s)
        if "(" in stripped and not re.search(r"\(\s*\*", stripped):
            continue
        structs[cur].append((i, s))
    return structs, None


# ─── R-EC1 组件纯数据（规范 5 + 规范 2「component 是纯数据」）───────────────────

# 字段行里的非纯数据形态（名字 → 判据说明）。`*`/`&` 成员 = 组件直接指资源/外部状态；
# 智能指针 = 拥有资源；std::function = 组件携带回调（执行能力进数据）；
# vk::/Vk = GPU 句柄进组件；entt::registry = 组件反向持有容器。
_PURITY_BANS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bstd::function\b"), "std::function 成员——组件携带回调，回调进 system/context 钩子"),
    (re.compile(r"\bstd::(?:unique|shared|weak)_ptr\b"), "智能指针成员——组件拥有/共享资源，资源归资产表"),
    (re.compile(r"\bvk::|\bVk[A-Z]"), "Vulkan 类型成员——GPU 资源经资产 ID 查资产表，不进组件"),
    (re.compile(r"\bentt::registry\b"), "registry 成员——组件不反向持有容器"),
    (re.compile(r"\bvirtual\b"), "virtual——组件是多态对象不是纯数据"),
    (re.compile(r"\(\s*\*"), "函数指针成员——执行能力进数据"),
]
_PTRREF = re.compile(r"[\w\)\]]\s*[*&]+\s*\w+\s*(?:\[[^\]]*\])?\s*(?:=|;)")


def ecs_component_purity_checks() -> list[Warning]:
    """R-EC1：契约模块全部 export struct 的字段必须纯数据（规范 5/2）。

    扫描面 = ECS_COMPONENT_MODULE 的 struct 体字段行；扫描面为空（模块改名/struct 全没）
    = error——"零警告"不许被读成"组件都纯"。
    """
    structs, bad = _component_structs(ECS_COMPONENT_MODULE)
    if bad:
        return [Warning(ECS_COMPONENT_MODULE, 0,
            f"ECS 组件纯度护栏（R-EC1）**本次未做任何对账**：{bad}"
            "——先修规则，别把「零警告」读成「组件都纯」", "error")]
    if not structs:
        return [Warning(ECS_COMPONENT_MODULE, 0,
            f"ECS 组件纯度护栏（R-EC1）**本次未做任何对账**：{ECS_COMPONENT_MODULE} 里"
            "一个 `export struct` 都没解析到（改名了？排版变了？）", "error")]
    SCAN_ECS_RULES["struct"] = len(structs)
    warnings: list[Warning] = []
    for name, fields in sorted(structs.items()):
        for line, s in fields:
            why = next((msg for rx, msg in _PURITY_BANS if rx.search(s)), None)
            if why is None and _PTRREF.search(s):
                why = "指针/引用成员——组件只存值与资产 ID（ECS 规范 5：资源经资产表，不直连）"
            if why:
                warnings.append(Warning(ECS_COMPONENT_MODULE, line,
                    f"组件 {name} 非纯数据（ECS 规范 2/5）：{why}", "error"))
    return warnings


# ─── R-EC2 组件定义位（规范 5 扫描面闭合）─────────────────────────────────────

# 实体级 entt 调用（组件集合的采集面）。ctx().emplace/set/get/insert_or_assign = registry
# context，**不是**组件，负向前瞻排除（context 合法住各模块：FrameDrawList/Camera/…）。
# get_or_emplace/remove 现状零使用，但它们是组件的自然未来形态——不收就把"新组件定义在
# 别处"的口子留着（R-EC1 扫描面闭合是本条的存在理由）。
_ENTITY_EMPLACE = re.compile(r"(?<!ctx\(\)\.)\b(?:emplace|get_or_emplace)<\s*([A-Za-z_]\w*)\s*>")
_ENTITY_VIEW = re.compile(r"(?<!ctx\(\)\.)\bview<([^<>]*(?:<[^<>]*>)?[^<>]*)>")
_ENTITY_PRED = re.compile(r"\b(?:all_of|any_of|none_of)<\s*([A-Za-z_]\w*)\s*>")
_ENTITY_GET = re.compile(r"\b(?:rg|registry|reg)\.get<\s*([A-Za-z_]\w*)\s*>")
_ENTITY_REMOVE = re.compile(r"(?<!ctx\(\)\.)\bremove<\s*([A-Za-z_]\w*)\s*>\s*\(")


def _collect_component_names() -> tuple[dict[str, set[str]], str | None]:
    """扫 rgl/*.cppm 的实体级 entt 调用 → ({组件名: {引用它的 rel}}, 失效原因)。"""
    files = rgl_cppm_files()
    if not files:
        return {}, "rgl/*.cppm 一个都扫不到（rgl_cppm_files 失效？）"
    users: dict[str, set[str]] = {}
    for rel in files:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        clean = strip_comments(src)
        for rx in (_ENTITY_EMPLACE, _ENTITY_PRED, _ENTITY_GET, _ENTITY_REMOVE):
            for m in rx.finditer(clean):
                users.setdefault(m.group(1).removeprefix("RGL::"), set()).add(rel)
        for m in _ENTITY_VIEW.finditer(clean):
            for part in m.group(1).split(","):
                name = part.strip().removeprefix("RGL::").strip()
                if re.fullmatch(r"\w+", name):
                    users.setdefault(name, set()).add(rel)
    return users, None


def ecs_component_home_checks() -> list[Warning]:
    """R-EC2：被当组件用的类型必须定义在契约模块（规范 5 扫描面闭合）。

    动机：R-EC1 的扫描面 = 契约模块。新组件若定义在别处（ecs.cppm / render_list.cppm），
    纯度检查**静默漏检**——本条把"组件集合"与"契约模块"钉成同一个集合，让 R-EC1 的
    覆盖面有机器凭据。采集面为空 = error（entt 调用形态改了、正则收不到 = 护栏空转）。
    """
    users, bad = _collect_component_names()
    if bad:
        return [Warning(ECS_COMPONENT_MODULE, 0,
            f"ECS 组件定义位护栏（R-EC2）**本次未做任何对账**：{bad}", "error")]
    if not users:
        return [Warning(ECS_COMPONENT_MODULE, 0,
            "ECS 组件定义位护栏（R-EC2）**本次未做任何对账**：全仓一个实体级 "
            "emplace<>/view<>/get<> 都没收到（entt 调用形态改了？正则与代码已脱钩）", "error")]
    structs, bad2 = _component_structs(ECS_COMPONENT_MODULE)
    if bad2:
        return [Warning(ECS_COMPONENT_MODULE, 0,
            f"ECS 组件定义位护栏（R-EC2）**本次未做任何对账**：{bad2}", "error")]
    SCAN_ECS_RULES["组件"] = len(users)
    warnings: list[Warning] = []
    for name, rels in sorted(users.items()):
        if name in structs:
            continue
        where = "、".join(sorted(rels)[:3])
        warnings.append(Warning(ECS_COMPONENT_MODULE, 0,
            f"组件 {name}（{where} 当组件用）不在 {ECS_COMPONENT_MODULE}——"
            "组件定义位漂移会让纯度护栏（R-EC1）静默漏检；组件是纯数据契约，"
            "住进契约模块，或改走 ctx() context / 资产表", "error"))
    return warnings


# ─── R-EC3 entt::scheduler 禁令（规范 2）──────────────────────────────────────


def ecs_scheduler_ban_checks() -> list[Warning]:
    """R-EC3：禁 entt::scheduler 族（规范 2 括号里的拍板：显式 system 列表，不用调度器）。

    名单为空 = 规则被掏空 ⟹ error（同「失效必须出声」；名单在 config.ECS_BANNED_TOKENS）。
    """
    if not ECS_BANNED_TOKENS:
        return [Warning("tools/checks/config.py", 0,
            "ECS 调度器禁令（R-EC3）**本次未做任何对账**：ECS_BANNED_TOKENS 为空"
            "（名单被清空 ≠ 全仓干净）", "error")]
    regexes = [(tok, re.compile(r"(?<![A-Za-z0-9_])" + re.escape(tok)))
               for tok in ECS_BANNED_TOKENS]
    warnings: list[Warning] = []
    scanned = 0
    for rel in rgl_cppm_files():
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        scanned += 1
        for lineno, line in enumerate(strip_comments(src).splitlines(), 1):
            for tok, rx in regexes:
                if rx.search(line):
                    warnings.append(Warning(rel, lineno,
                        f"{tok}——ECS 规范 2 拍板：显式 system 列表（ecs_impl 的 "
                        "FrameSystem 链），不用 entt::scheduler（process 调度器"
                        "执行顺序无保证，见 ecs_compliance_review 处置记录）", "error"))
    if not scanned:
        return [Warning("tools/checks/config.py", 0,
            "ECS 调度器禁令（R-EC3）**本次未做任何对账**：扫描面为空（rgl/*.cppm 扫不到）",
            "error")]
    SCAN_ECS_RULES["禁令token"] = len(ECS_BANNED_TOKENS)
    return warnings


# ─── R-EC4 namespace 作用域可变全局（规范 3「不用全局变量」）───────────────────

# 列 0（本仓 namespace 体不缩进）的变量定义行。存储期限定词前缀可叠加，逐个**词法**剥掉
# ——不用正则前缀组：`(?:export|inline|static)*` 会回溯少剥一个词，让 constexpr 躲过禁列检查
# （实测 `export inline constexpr` 全漏）。首词在禁列集中的不是可变全局。
_PREFIX_WORDS = frozenset({"export", "inline", "static", "thread_local", "mutable"})
_BANNED_FIRST = frozenset({
    "constexpr", "consteval", "constinit", "const", "static_assert", "template",
    "struct", "class", "enum", "union", "namespace", "using", "module", "import",
    "friend", "operator", "extern", "if", "for", "while", "switch", "return",
    "case", "do", "else", "throw", "co_return", "co_await", "co_yield",
})
# 剩余部分 = `类型 名字 [下标] =|{`；类型首词 + 可选模板实参 + 修饰符，名字后必须紧跟
# 初始化或声明尾（函数声明在更早的括号检查里已被排除）。
_VAR_TAIL = re.compile(
    r"^([\w:]+(?:\s*<[^;{}]*(?:<[^<>]*>)?[^;{}]*>)?(?:[\w:*&<> ]*?))\s+"
    r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|\{|;)")
_ANGLE = re.compile(r"<[^<>]*(?:<[^<>]*>)?[^<>]*>")


def _strip_angles(s: str) -> str:
    prev = None
    while prev != s:
        prev = s
        s = _ANGLE.sub("", s)
    return s


def _is_global_mutable_decl(s: str) -> bool:
    """列 0 行（剥注释、rstrip 后）是不是 namespace 作用域可变变量定义。"""
    if not (s.endswith(";") or s.endswith("{")):
        return False
    if s[:-1].rstrip().endswith(")") or s.endswith(") {"):
        return False  # 多行函数签名的尾参行（`bool x = false);` / `uint32_t p = D) {`）
    toks = s.split()
    i = 0
    while i < len(toks) and toks[i] in _PREFIX_WORDS:
        i += 1
    if i >= len(toks):
        return False
    if toks[i].split("<")[0].rstrip(";=,") in _BANNED_FIRST:
        return False
    rest = " ".join(toks[i:])
    head = _strip_angles(rest.split("=", 1)[0])
    if "(" in head and ")" in head:
        return False  # 函数声明/定义
    return bool(_VAR_TAIL.match(rest))


def _global_mutable_hits() -> tuple[dict[str, list[tuple[int, str]]], str | None]:
    """全仓列 0 可变全局普查 → {rel: [(行号, 行)]}；扫描面塌了给原因。"""
    files = rgl_cppm_files()
    if not files:
        return {}, "rgl/*.cppm 一个都扫不到（rgl_cppm_files 失效？）"
    hits: dict[str, list[tuple[int, str]]] = {}
    scanned = 0
    for rel in files:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            continue
        scanned += 1
        for i, ln in enumerate(strip_comments(src).splitlines(), 1):
            if not ln or ln[0] in " \t#}/":
                continue  # 列 0 判据（见模块 docstring「R-EC4 的判据形态」）
            s = ln.rstrip()
            if not _is_global_mutable_decl(s):
                continue
            hits.setdefault(rel, []).append((i, s[:100]))
    if not scanned:
        return {}, "rgl/*.cppm 一个都读不进去"
    return hits, None


def load_ecs_global_baseline() -> tuple[dict[str, int], str | None]:
    """读 R-EC4 棘轮基线 → ({rel: 允许条数}, 不可用原因)。空表会被读成「零全局可变状态」
    ⟹ 缺失/坏 JSON/格式不符一律返回原因串（同 doc_pointer §1.7 的纪律，不走
    tool_baseline 的「缺失→空表按未登记」）。"""
    path = Path(ROOT) / ECS_GLOBAL_BASELINE_FILE
    if not path.is_file():
        return {}, f"基线文件不存在：{ECS_GLOBAL_BASELINE_FILE}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {}, f"基线文件读不出来：{ECS_GLOBAL_BASELINE_FILE}（{type(e).__name__}: {e}）"
    if not isinstance(data, dict):
        return {}, f"基线顶层不是 JSON 对象：{ECS_GLOBAL_BASELINE_FILE}"
    files = data.get("files")
    if not isinstance(files, dict):
        return {}, f"基线缺 `files` 对象（格式不符）：{ECS_GLOBAL_BASELINE_FILE}"
    bad = sorted(str(k) for k, v in files.items()
                 if isinstance(v, bool) or not isinstance(v, int))
    if bad:
        return {}, f"基线 `files` 有非整数条数：{ECS_GLOBAL_BASELINE_FILE} 的 {bad[:3]}"
    return {str(k): int(v) for k, v in files.items()}, None


def write_ecs_global_baseline(counts: dict[str, int]) -> str:
    """按当前条数重写 R-EC4 棘轮基线（收账）；返回写入路径。"""
    files = {k: int(v) for k, v in sorted(counts.items())}
    path = Path(ROOT) / ECS_GLOBAL_BASELINE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(
        {"_comment": ECS_GLOBAL_BASELINE_COMMENT,
         "total": sum(files.values()), "files": files},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return ECS_GLOBAL_BASELINE_FILE


# 最近一次 R-EC4 命中计数（收账命令的输入）与覆盖面读数（取向同 doc_pointer）。
# 🔴 SCAN_ECS_GLOBAL **只由 R-EC4 写**：main.py 的 --update-ecs-global-baseline 拿它的
# 空/非空判「本条跑没跑过」，别的规则混进来会让收账命令把「没数」写进基线（护栏变瞎）。
LAST_ECS_GLOBAL_COUNTS: dict[str, int] = {}
SCAN_ECS_GLOBAL: dict[str, int] = {}
# R-EC1/EC2/EC3 的覆盖面读数（同一条打印行，但不参与收账守卫）
SCAN_ECS_RULES: dict[str, int] = {}


def ecs_global_readout() -> str:
    """最近一次 R-EC4 的覆盖面读数；没跑过 = 空串（收账命令据此拒绝落笔）。"""
    if not SCAN_ECS_GLOBAL:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN_ECS_GLOBAL.items())


def ecs_rules_readout() -> str:
    """R-EC1/EC2/EC3 的覆盖面读数；没跑过 = 空串。"""
    if not SCAN_ECS_RULES:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN_ECS_RULES.items())


def ecs_global_baseline_counts() -> dict[str, int]:
    """最近一次 R-EC4 逐文件命中条数（收账命令的输入；未跑过 = 空表）。"""
    return dict(LAST_ECS_GLOBAL_COUNTS)


def ecs_global_state_checks() -> list[Warning]:
    """R-EC4：namespace 作用域可变全局 = 规范 3「不用全局变量决定构成」的静态代理。

    存量走棘轮基线（只准降不准升）：新增一条 = error，处置是把它搬进 registry context /
    函数参数 / 类成员，**不是**来基线登记额度。基线不可用 / 扫描面为空 ⟹ error 出声。
    """
    hits, bad = _global_mutable_hits()
    if bad:
        return [Warning("tools/checks/config.py", 0,
            f"ECS 全局可变状态护栏（R-EC4）**本次未做任何对账**：{bad}"
            "——「零警告」不是「没有全局可变状态」", "error")]
    counts = {rel: len(v) for rel, v in hits.items()}
    LAST_ECS_GLOBAL_COUNTS.clear()
    LAST_ECS_GLOBAL_COUNTS.update(counts)
    baseline, bad_base = load_ecs_global_baseline()
    total = sum(counts.values())
    SCAN_ECS_GLOBAL.update({"全局可变状态": total,
                            "基线": sum(baseline.values()) if not bad_base else 0})
    if bad_base:
        return [Warning(ECS_GLOBAL_BASELINE_FILE, 0,
            f"ECS 全局可变状态棘轮**本次未做任何对账**：{bad_base}"
            f"（当前命中 {total} 条未比对）——先修基线文件", "error")]
    warnings: list[Warning] = []
    for rel in sorted(set(counts) | set(baseline)):
        cur, base = counts.get(rel, 0), baseline.get(rel, 0)
        if rel not in counts:
            if base > 0:
                warnings.append(Warning(rel, 0,
                    f"ECS 全局可变状态基线仍记 {base} 条但该文件已清零/不在扫描面——"
                    f"用 `{ECS_GLOBAL_UPDATE_CMD}` 收账"))
            continue
        if cur > base:
            for line, text in hits[rel]:
                warnings.append(Warning(rel, line,
                    f"namespace 作用域可变全局 `{text}`——ECS 规范 3：构成/场景状态"
                    "不走全局变量（进 registry context、函数参数或类成员）"
                    + (f"；非场景状态的累加器确需全局，才用 `{ECS_GLOBAL_UPDATE_CMD}` 登记"
                       if rel not in baseline else ""), "error"))
            if rel not in baseline:
                warnings.append(Warning(rel, 0,
                    f"ECS 全局可变状态基线未登记：当前 {cur} 条（新文件必须显式登记进 "
                    f"{ECS_GLOBAL_BASELINE_FILE}——但**先确认**它真该是全局的）", "error"))
            else:
                warnings.append(Warning(rel, 0,
                    f"ECS 全局可变状态基线上升 {cur} > {base}——处置是把全局搬进 "
                    f"registry context/参数/成员，**不是**改基线（基线只准降；"
                    f"确实清了才用 `{ECS_GLOBAL_UPDATE_CMD}`）", "error"))
        elif cur < base:
            warnings.append(Warning(rel, 0,
                f"ECS 全局可变状态可下调 {cur} < {base}——用 `{ECS_GLOBAL_UPDATE_CMD}` "
                "收账（悄悄漂移等于没有基线）"))
    return warnings
