"""clang 文本 AST dump 的行级解析（接口 .cppm 结构抽取）。

为什么是文本 AST 而不是 json AST / 纯正则：
- json AST 对模块单元会把 BMI 导入整体展开（rlvk 2.6GB），且 -ast-dump-filter
  在 json 模式输出拍平树；
- 文本 dump 全单元 ≤10s，行级过滤即可剪掉污染，树结构由缩进恢复；
- 纯正则拿不到 member 类型 / 基类 / 完整签名（跨模块依赖边的原料）。

行格式（clang ASTPrinter）：
    <prefix><connector>Kind 0xADDR <loc...> [flags...] name 'type' extra
  prefix 由 `| `/`  ` 单元组成，depth = len(prefix)//2 - 1（prefix 非空时）。
loc 两态：显式路径 `</abs/path:L:C>` 或省略式（继承父节点上下文）。
模块所有权后缀 `in <mod>`：本单元声明 `in <本模块>`；GMF-include 的 glm/fmt
头文件声明即使标 `in <本模块>`，其首次出现必带外部显式路径。

保留规则：路径上下文 == 本单元文件 ∧ 所有权 == 本模块。
失败节点标毒，其缩进子树整体吞掉。
"""

import re

_LINE_RE = re.compile(r"^(?P<pre>[\s|`\-]*?)(?P<conn>\|-|`-)(?P<rest>.*)$")
_KIND_RE = re.compile(
    r"^(?P<kind>[A-Za-z][A-Za-z0-9_:]*)\s+(?:0x[0-9a-f]+\s+)?(?P<tail>.*)$"
)
_PATH_RE = re.compile(r"</(?P<path>[^:>]+):")
_MODULE_RE = re.compile(r"\bin (?P<mod>[A-Za-z_][\w.:]*)")
_QUOTED_RE = re.compile(r"'([^']*)'")
# 行号：loc 首段 <</path:L:C> 或省略路径式 line:L:C → 声明首行（含返回类型行）
_LOC_LINE_RE = re.compile(r"(?:</[^:>]+>|line):(\d+):\d+")


def _decl_start_line(rest, cur_line):
    """声明首行（多行声明 = 返回类型行）：
    - 范围起点带行号（</path:L:C 或 line:L:C）→ 该值；
    - 范围起点 col-only（打印器省略行号 = 继承上下文行）→ cur_line；
    - 无 <...> 段 → cur_line。
    注意：范围终点行号不是本节点起点（如 <col:8, line:67:1> 起点继承、终点 67），
    取第一个显式行号会把声明错认到块尾。"""
    m = re.search(r"<", rest)
    if not m:
        return cur_line
    seg = rest[m.start() + 1:]
    close = seg.find(">")
    if close == -1:
        return cur_line
    start_el = seg[:close].split(",")[0].strip()
    lm = re.match(r"(?:</[^:>]+>|line):(\d+):\d+", start_el)
    return int(lm.group(1)) if lm else cur_line

_FLAG_WORDS = {
    "implicit", "hidden", "used", "exported", "constexpr", "consteval", "inline",
    "static", "virtual", "pure-virtual", "defaulted", "delete", "scoped",
    "definition", "referenced", "friend", "mutable", "register", "class",
    "struct", "union", "public", "protected", "private", "final", "abstract",
    "override", "extern", "thread_local", "signed", "unsigned", "long",
    "short", "prev", "builtin", "c++17", "maybe_unused", "not_tail_called",
    "warn_unused_result", "deprecated", "noescape", "nonnull", "returns_nonnull",
    "pure", "const", "alloc_size", "format", "hot", "cold", "noreturn",
    "nonnull", "optnone", "optnone", "weak", "weakref", "alias", "ifunc",
    "visibility", "dllimport", "dllexport", "ms_struct", "gcc_struct",
    "unavailable", "swift_name", "objc_runtime_name", "sentinel",
    "vec_type_hint", "ext_vector_type", "block", "global", "host",
}


class ANode:
    __slots__ = ("kind", "name", "type", "flags", "children", "depth", "poison",
                 "line")

    def __init__(self, kind, name, type_, flags, depth, poison=False):
        self.kind = kind
        self.name = name
        self.type = type_
        self.flags = flags
        self.children = []
        self.depth = depth
        self.poison = poison
        self.line = 0

    def dump(self, ind=0):
        print("  " * ind + f"{self.kind} {self.name} : {self.type}"
              + (" [POISON]" if self.poison else ""))
        for c in self.children:
            c.dump(ind + 1)


def _strip_loc_segments(s):
    """去掉 <...> 位置段与 0x 指针（防止把 file:line 当 token）。"""
    s = re.sub(r"<<[^<>]*>>|<[^<>]*>", " ", s)
    s = re.sub(r"0x[0-9a-f]+", " ", s)
    s = re.sub(r"(col|line|offset|tokLen):\d+", " ", s)
    return s


def _parse_tail(kind, tail):
    """从 kind 之后的文本提取 name / type / flags。"""
    quotes = _QUOTED_RE.findall(tail)
    type_ = quotes[0] if quotes else None
    bare = _strip_loc_segments(_QUOTED_RE.sub(" ", tail))
    tokens = [t for t in bare.split() if t not in _FLAG_WORDS]
    flags = {t for t in bare.split() if t in _FLAG_WORDS}
    name = None
    if quotes:
        before = _strip_loc_segments(_QUOTED_RE.split(tail)[0])
        bt = [t for t in before.split() if t not in _FLAG_WORDS]
        # `in <mod>` 尾巴要剔掉（before 段一般不含，防御）
        if len(bt) >= 2 and bt[-2] == "in":
            bt = bt[:-2]
        if bt:
            name = bt[-1]
    elif tokens:
        if len(tokens) >= 2 and tokens[-2] == "in":
            tokens = tokens[:-2]
        if tokens:
            name = tokens[-1]
    return name, type_, flags


def parse_ast_text(text, unit_base, module_name, prefilter=True):
    """解析文本 dump，返回 (root, stats)。

    unit_base='render_graph.cppm'，module_name='render_graph'。
    root.children = 保留的顶层声明树（Skip 节点=被剪枝的外来子树占位）。

    prefilter=True（默认）：先按行丢弃既无 `in <module>` 也不指向本单元文件的行。
    依据：本单元声明行必带 `in <module>`（隐式成员/模板/枚举常量同样带）；
    表达式子行与外来子树行被滤掉后，深度由保留行的原始缩进决定，
    树结构与全量解析逐字节等价（tests/test_prefilter.py 全单元对拍）。
    """
    root = ANode("File", module_name, "", set(), -1)
    stack = [root]
    stats = {"lines": 0, "kept": 0, "poisoned": 0}
    # 打印器的 loc/模块状态是全局流式的：显式出现即更新，省略式沿用
    cur_path_ok = False
    cur_mod_ok = False
    cur_line = 0
    in_tag = f"in {module_name}"
    path_tag = f"{unit_base}:"
    unit_base_path = f"/{unit_base}:"

    for line in text.splitlines():
        if prefilter:
            if in_tag not in line and unit_base_path not in line:
                stats["lines"] += 1
                continue
        if not line.strip():
            continue
        m = _LINE_RE.match(line)
        if not m:
            continue
        stats["lines"] += 1
        pre = m.group("pre")
        depth = (len(pre) // 2 - 1) if pre else 0
        rest = m.group("rest")

        pm = _PATH_RE.search(rest)
        if pm:
            cur_path_ok = pm.group("path").endswith(unit_base)
        mm = _MODULE_RE.search(rest)
        if mm:
            cur_mod_ok = mm.group("mod") == module_name
        node_line = _decl_start_line(rest, cur_line)
        all_l = _LOC_LINE_RE.findall(rest)
        if all_l:
            cur_line = int(all_l[-1])

        while len(stack) > 1 and stack[-1].depth >= depth:
            stack.pop()
        parent = stack[-1]

        if parent.kind == "Skip":
            stats["poisoned"] += 1
            continue
        keep = cur_path_ok and cur_mod_ok
        if not keep:
            poison = ANode("Skip", None, "", set(), depth, poison=True)
            parent.children.append(poison)
            stack.append(poison)
            stats["poisoned"] += 1
            continue

        km = _KIND_RE.match(rest)
        if not km:
            poison = ANode("Skip", None, "", set(), depth, poison=True)
            parent.children.append(poison)
            stack.append(poison)
            stats["poisoned"] += 1
            continue
        kind = km.group("kind")
        name, type_, flags = _parse_tail(kind, km.group("tail"))
        if kind == "ImportDecl":
            # `... in <owning_mod> <imported_module>`：导入名在 in 段之后
            bare = _strip_loc_segments(_QUOTED_RE.sub(" ", km.group("tail")))
            toks = [t for t in bare.split() if t not in _FLAG_WORDS]
            name = toks[-1] if toks else None
        node = ANode(kind, name, type_, flags, depth)
        node.line = node_line
        parent.children.append(node)
        stack.append(node)
        stats["kept"] += 1

    return root, stats


# ---------- 结构化提取 ----------

DECL_KINDS = {
    "CXXRecordDecl", "FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl",
    "CXXDestructorDecl", "EnumDecl", "TypedefDecl", "TypeAliasDecl",
    "VarDecl", "FieldDecl", "EnumConstantDecl", "CXXBaseSpecifier",
    "NamespaceAliasDecl", "FunctionTemplateDecl", "ClassTemplateDecl",
    "TypeAliasTemplateDecl", "ImportDecl",
}


def _record_shape(node):
    """CXXRecordDecl → dict（bases/fields/methods/enums/typedefs）。"""
    rec = {
        "name": node.name,
        "kind": "class" if "class" in node.flags else
                "union" if "union" in node.flags else "struct",
        "bases": [], "fields": [], "methods": [], "enums": [], "typedefs": [],
        "nested": [], "flags": sorted(node.flags),
        "line": node.line,
    }
    for c in node.children:
        if c.poison:
            continue
        if c.kind == "CXXBaseSpecifier":
            if c.type:
                rec["bases"].append(c.type)
        elif c.kind == "FieldDecl":
            rec["fields"].append({"name": c.name, "type": c.type,
                                  "flags": sorted(c.flags)})
        elif c.kind in ("CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl"):
            if "implicit" in c.flags:
                continue
            rec["methods"].append({"name": c.name, "sig": c.type,
                                   "flags": sorted(c.flags), "line": c.line})
        elif c.kind == "EnumDecl":
            rec["enums"].append(_enum_shape(c))
        elif c.kind in ("TypedefDecl", "TypeAliasDecl"):
            rec["typedefs"].append({"name": c.name, "type": c.type})
        elif c.kind in ("CXXRecordDecl", "ClassTemplateDecl", "FunctionTemplateDecl"):
            if "implicit" in c.flags:
                continue
            inner = c.children[0] if (c.kind.endswith("TemplateDecl") and c.children) else c
            if inner.kind == "CXXRecordDecl":
                shape = _record_shape(inner)
                shape["line"] = inner.line
                rec["nested"].append(shape)
            else:
                rec["nested"].append({"name": inner.name, "kind": "function",
                                      "sig": inner.type, "line": inner.line})
        elif c.kind == "VarDecl":
            rec.setdefault("statics", []).append(
                {"name": c.name, "type": c.type, "flags": sorted(c.flags)})
    return rec


def _enum_shape(node):
    return {
        "name": node.name,
        "underlying": node.type,
        "constants": [c.name for c in node.children
                      if c.kind == "EnumConstantDecl" and not c.poison],
        "line": node.line,
    }


def extract_unit(root):
    """从解析树提取模块结构：{imports, records, enums, functions, typedefs, vars}。"""
    out = {"imports": [], "records": [], "enums": [], "functions": [],
           "typedefs": [], "vars": []}
    seen_record = {}

    def visit_children(n):
        for c in n.children:
            if c.poison:
                continue
            handle(c)

    def handle(c):
        if c.kind == "ImportDecl":
            # 行尾最后一个词 = 被 import 的模块名（name 提取已拿到）
            if c.name:
                out["imports"].append(c.name)
        elif c.kind == "NamespaceDecl":
            visit_children(c)
        elif c.kind == "ExportDecl":
            visit_children(c)
        elif c.kind == "CXXRecordDecl":
            if "implicit" in c.flags:
                return
            if "definition" not in c.flags:
                return
            shape = _record_shape(c)
            shape["line"] = c.line
            if shape["name"] not in seen_record:
                seen_record[shape["name"]] = shape
                out["records"].append(shape)
        elif c.kind in ("ClassTemplateDecl", "FunctionTemplateDecl"):
            inner = c.children[0] if c.children else None
            if inner is not None and not inner.poison:
                handle(inner)
        elif c.kind == "EnumDecl":
            out["enums"].append(_enum_shape(c))
        elif c.kind == "FunctionDecl":
            if "implicit" in c.flags:
                return
            out["functions"].append({"name": c.name, "sig": c.type,
                                     "flags": sorted(c.flags), "line": c.line})
        elif c.kind in ("TypedefDecl", "TypeAliasDecl"):
            out["typedefs"].append({"name": c.name, "type": c.type,
                                    "line": c.line})
        elif c.kind == "VarDecl":
            out["vars"].append({"name": c.name, "type": c.type,
                                "flags": sorted(c.flags), "line": c.line})

    visit_children(root)
    return out
