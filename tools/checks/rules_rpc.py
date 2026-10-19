"""RPC 能力单源对账（docs/rpc_单源_plan.md 批 1 换血 + 批 2 收请求字段形状 + 批 3 收响应形状与文档生成）。

## 为什么不是给 §10 打补丁

§10 的办法是拿注册侧与分发侧两份手写名单互相对账。它解决的是当年撞见的那一种形态（B2-5 的
`/pass` 404），但"引擎能做什么"这件事的副本不止两份（注册、分发、路由回包、接口注释、文档表、
客户端常量），每加一处就要再写一条正则、再自述一次"看不到什么"。批 1 把事实收进
`rgl/rpc_schema.lua` 并用既有反射管线生成 `rgl/generated/rpc_schema_generated.cppm` 与
`rgl/generated/rpc_schema.json`，服务端注册 / 分发 / 超时 / 路由回包 / 请求形状校验全部吃生成物
⟹ 副本不存在了，本规则也从"名单对账"换成下面四个面。

## 面 1｜schema ↔ 两份生成物 **三方逐条对账**（防"产物被手改 / 只重生成了一份"）

`cmake/ReflectionManifest.cmake` 的构建期守卫**只哈希输入**——产物被编辑它看不见（同
`rules_reflection.py` 补的那块盲区）。而本管线一次出两份产物（`.cppm` 给 C++、`.json` 给
Python 客户端），"只重生成其中一份"是真实失效形态：两份产物会各自携带一份过期事实，而任何
单侧对账都测不出来。⟹ 三方按**顺序 + 逐字段**对账。

🔴 **批 2 起 schema 是嵌套的**（`request = { fields = { {name=…, min=…, …}, … } }`），所以
本模块的 Lua 读取从"认深度 1 的扁平 `key = value`"换成**递归下降的 Lua 表解析器**
（`parse_lua_table`）。为什么必须换：批 2 之前那版解析器只认一层，遇到嵌套表会把
`fields = {…}` 整段读不出来 ⟹ 字段级的对账**根本不存在**，而"零警告"会被读成"三方一致"。
换完之后 schema ↔ `.json` 是**整份记录的深度相等**（含任意层嵌套），不是逐字段挑着比。

## 面 2｜代码里出现路由/字段名字面量 = ERROR（并确认消费文件真的 import 了生成模块）

单源之后，`rgl/` 里再出现 `server.Post("/x", …)`、`request.path == "/x"`、或
`body.at("label")` 这类字面量，就意味着有人绕过 schema 加了一条事实——它今天能对上，
明天改 schema 时不会有人回来改它。逐文件点名行号。
同时要求 `RPC_SCHEMA_CONSUMER_FILES` 里每个文件确实 `import rpc_schema_generated`：找不到
= 这条面已经瞎了（改名/搬文件后规则会扫不到任何东西而"零警告"），必须出声。

## 面 3｜每个声明的字段都必须有 `rpc_field::<端点>::<字段>` 常量消费

批 2 的"单源承重"全靠这条：处理函数读字段名吃的是生成物里的常量，所以改 schema 里的字段名
= 处理函数编译不过。但**常量没被消费就等于没消费**——本面逐个字段查引用点是否存在
（引用面 = 扫描目录全部 `.cppm`，生成物除外），查不到 = ERROR 点名。
没有这条，"字段名常量"会静默退化成"生成物里一堆没人读的东西 + 一份装饰用的 schema"。

## 面 4｜manualShape 端点必须带 `manualReason`（止损线不许只写在注释里）

`manualShape = true` 是批 2 的止损线出口（形状表达不出来的端点退回手写）。它天然是一块
**无机器对账**的残余面，所以至少要把"为什么"钉成必填：缺 `manualReason` 或它是空串 = ERROR。

## 面 5｜response 段自洽（批 3）

`response = { fields = { … } }` 与根级 `shapes` 是**文档面**（C++ 不消费，消费方 = md 参考与
`.json` 产物），所以它的机器判据只能落在 schema 侧的结构规则上：每个端点必须有非空
response.fields；kind 在词表内（`config.RPC_RESPONSE_KINDS`）；object / object[] 必须带 shape
且指向存在的 shapes 条目、非 object 不许带 shape；`optional = true` 必带非空 `absentWhen`
（"能缺哪些"与"有哪些"是同一份契约的两半）；端点内字段名唯一；shapes 条目名唯一、fields 非空、
**每条 shape 至少被一个端点引用**（没人引用的结构 = 装饰）。

## 面 6｜生成的 md 参考 ↔ schema 一致（批 3）

`rgl/generated/rpc_schema.md` 是字段级文档唯一的那份文件（`docs/技术概念.md` 的表已删，指向它），
"文档与 schema 漂了"这件事从"人记得住"变成机器判据：端点**条数与顺序**一致、每端点的响应
**字段名集合与顺序**一致、共享结构的**名集合与字段名集合**一致。md 是正则解析的生成物文本
（同 `.cppm` 侧的口径）：模板改形态 → 解析式认不出 → 报"本次未做任何对账"，失效方向是误报
不是假绿。

## 失效必须出声

schema 读不到 / 解析出 0 条 / **解析报错**、任一生成物读不到或解析出 0 条、消费文件读不到
⟹ ERROR 声明"本次未做任何对账"（同 `graph_fact_predicate_checks` / rules_layers 的纪律：
没数不是相等）。解析器**报错也算出声**——它比"解析出 0 条"更该响：形态变了却静默少解析一块，
正是"看起来对上了"的假绿。

## 盲区（写下来不装没看见）

- 面 2 认三种路由字面量形态（`server.<方法>("…")` / `request.path (==|!=) "…"` / `"GET /…"`)
  与两种字段名字面量形态（`body.at("…")` / `body.contains("…")`）。把路径或字段名拼进常量、
  raw string、或换一种写法会扫不到——但**扫不到 ≠ 扫错**：分发侧一旦不再查生成表，面 1 的
  三方对账与穷尽 switch 的编译期检查仍在；字段侧一旦不再用常量，面 3 会报"没有引用点"。
- 面 1 对的是"生成物与 schema 一致"，不对"schema 与运行期行为一致"（后者 = 端点冒烟，
  `tests/test_rpc_contract.cppm` + `just regress burst` + 两个截图锚）。**批 3 的 response 段
  同口径**：面 5/6 对的是"response 声明自洽 + md 与 schema 一致"，"schema 说的回包字段与
  处理函数实际回包一致"没有机器判据（回包由处理函数拼装，无运行期校验可挂）——改回包
  记得同步 schema，`tools/rpc_endpoints_live.py` 逐端点对拍回包键集（读数记录 →
  `docs/rpc_单源_plan.md` §4 批 3）。
- 面 1 对 `.cppm`、面 6 对 `.md` 都是**正则解析生成物文本**（C++/markdown 没有便宜的语法树
  读法）。它们的失效方向是误报：模板改形态 → 解析式认不出 → 报"解析出 0 条 / 本次未做
  任何对账"，不会假绿。
  残余面：**schema 与产物同时被手改成同一个错值**没人守（`reflection_check` 只哈希输入）。
  这条是"生成物已提交"这一形态的固有代价，与 §7 `rules_reflection.py` 同口径。
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from config import (
    ROOT,
    RPC_FIELD_LITERAL_RES,
    RPC_RESPONSE_KINDS,
    RPC_SCHEMA_CONSUMER_FILES,
    RPC_SCHEMA_CPPM,
    RPC_SCHEMA_FILE,
    RPC_SCHEMA_IMPORT_MODULE,
    RPC_SCHEMA_JSON,
    RPC_SCHEMA_MD,
    RPC_ROUTE_LITERAL_RE,
    RPC_ROUTE_METHOD_TEXT_RE,
    RPC_ROUTE_PATH_COMPARE_RE,
    RPC_SCHEMA_ROUTE_RE,
    RPC_SCHEMA_SCAN_DIRS,
    RPC_SCHEMA_SCAN_EXCLUDE_DIRS,
    RPC_SCHEMA_SCAN_SUFFIXES,
)
from fs import read_text
from models import Warning

# config.py 是"零逻辑"的参数表 ⟹ 正则在那里以**字符串**存，编译在本模块做。
# 面 2 的路由三条（注册 / 分发 / 路由字符串字面量）——命中即说明有人绕过 schema 写路由事实。
RPC_ROUTE_LITERAL_RX = re.compile(RPC_ROUTE_LITERAL_RE)
RPC_ROUTE_PATH_COMPARE_RX = re.compile(RPC_ROUTE_PATH_COMPARE_RE)
RPC_ROUTE_METHOD_TEXT_RX = re.compile(RPC_ROUTE_METHOD_TEXT_RE)
RPC_FIELD_LITERAL_RXS = tuple(re.compile(p) for p in RPC_FIELD_LITERAL_RES)
RPC_SCHEMA_ROUTE_RX = re.compile(RPC_SCHEMA_ROUTE_RE)
RPC_SCHEMA_ROUTE_RX = re.compile(RPC_SCHEMA_ROUTE_RE)
# 消费点的 import 判据（语句形态：行首 import，注释掉的写法不算——见 config 里的说明）
RPC_SCHEMA_IMPORT_RX = re.compile(
    rf"(?m)^[ \t]*import[ \t]+{RPC_SCHEMA_IMPORT_MODULE}[ \t]*;")

# 覆盖面读数（main.py 每次构建印一行）：三方各自的条数 + 面 2 的扫描文件数
SCAN_RPC_SCHEMA: dict[str, str] = {}


def rpc_schema_readout() -> str:
    """四方条数与扫描面读数；没跑过 = 空串（main.py 据此打「未执行」）。"""
    if not SCAN_RPC_SCHEMA:
        return ""
    return (f"schema {SCAN_RPC_SCHEMA['schema']} 条 / .cppm {SCAN_RPC_SCHEMA['cppm']} 条 / "
            f".json {SCAN_RPC_SCHEMA['json']} 条 / .md {SCAN_RPC_SCHEMA['md']} 条"
            f" · 请求字段 schema {SCAN_RPC_SCHEMA['schema_fields']}"
            f" / .cppm {SCAN_RPC_SCHEMA['cppm_fields']}"
            f" · 响应字段 schema {SCAN_RPC_SCHEMA['schema_resp_fields']}"
            f" / .md {SCAN_RPC_SCHEMA['md_resp_fields']}"
            f" · shapes {SCAN_RPC_SCHEMA['shapes']} / .md {SCAN_RPC_SCHEMA['md_shapes']}"
            f" · 字面量扫描面 {SCAN_RPC_SCHEMA['scanned']} 个 .cppm")


# ═══════════════════════════════════════════════════════════════════════════════════════
# Lua schema 读取：**递归下降的 Lua 表解析器**（stdlib-only：不引 Lua 解析器，不 shell 调 lua）
#
# 为什么不是"再给正则加一条嵌套分支"：批 2 的 schema 深度是 3（endpoints → fields → 一条
# 字段记录），且字段记录里的键是**可选**的（`min` 只有数值字段有）。用正则去认"深度 ≤ N 的
# 表"等于每加一层就重写一次解析式，而写错的方向是**静默少解析一块**（= 假绿）。递归下降一次
# 把整份表读成 Python 对象，读不出来就抛 ⟹ 失效方向只有"报错出声"，没有"少读一块还全绿"。
#
# 认的形态 = rgl/rpc_schema.lua 实际用到的那个 Lua 数据子集：
#   return { k = v, { … }, "字符串", 3, -1, 3.5, true, false, { 嵌套 = { 表 } } }
# 分隔符 `,` `;` 与换行都可有可无（Lua 允许）。函数 / 变量引用 / 长字符串 `[[…]]` 一律
# **抛错**——它们要么在沙箱里读不出值（`loadLuaToJson` 那边是抛，两侧必须同口径），要么是
# 有人在 schema 里写代码（能力清单是数据，不是代码）。
# ═══════════════════════════════════════════════════════════════════════════════════════

class LuaParseError(Exception):
    """schema 读不出一个明确的值（形态超出数据子集 / 括号不配对 / 混了键形态）。"""


def _skip_ws(text: str, i: int) -> int:
    """跳过空白、`--` 行注释与 `--[[ ]]` 块注释。"""
    n = len(text)
    while i < n:
        ch = text[i]
        if ch.isspace():
            i += 1
            continue
        if text.startswith("--[[", i):
            end = text.find("]]", i)
            if end < 0:
                raise LuaParseError(f"第 {text[:i].count(chr(10)) + 1} 行 `--[[` 没闭合")
            i = end + 2
            continue
        if text.startswith("--", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue
        return i
    return i


_LUA_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}


def _parse_string(text: str, i: int) -> tuple[str, int]:
    """一个引号串（双/单引号都认，反斜杠转义认 `n t r \\ " '`）。"""
    quote = text[i]
    out: list[str] = []
    i += 1
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "\\":
            if i + 1 >= n:
                raise LuaParseError("字符串以反斜杠结尾")
            nxt = text[i + 1]
            if nxt not in _LUA_ESCAPES:
                raise LuaParseError(f"不认的转义 `\\{nxt}`（本解析器只认 n/t/r/\\\\/\"/'）")
            out.append(_LUA_ESCAPES[nxt])
            i += 2
            continue
        if ch == quote:
            return "".join(out), i + 1
        if ch == "\n":
            raise LuaParseError("字符串里有裸换行（跨行文案请用转义 \\n）")
        out.append(ch)
        i += 1
    raise LuaParseError("字符串没闭合")


def _parse_number(text: str, i: int) -> tuple[int | float, int]:
    m = re.compile(r"[+-]?(?:0[xX][0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)").match(text, i)
    if not m:
        raise LuaParseError(f"第 {_line(text, i)} 行不是数字也不是字符串：{text[i:i + 20]!r}")
    raw = m.group(0)
    i = m.end()
    if raw.lower().startswith("0x"):
        return int(raw, 16), i
    if re.fullmatch(r"[+-]?\d+", raw):
        return int(raw), i
    return float(raw), i


def _line(text: str, pos: int) -> int:
    return text[:pos].count("\n") + 1


def _parse_table(text: str, i: int) -> tuple[list | dict, int]:
    """`{ … }` → list（全位置键）或 dict（全字符串键）。混合 / 裸标识符 = 抛。"""
    n = len(text)
    assert text[i] == "{"
    i += 1
    items: list[tuple[object, object]] = []
    while True:
        i = _skip_ws(text, i)
        if i >= n:
            raise LuaParseError("`{` 没闭合")
        if text[i] == "}":
            i += 1
            break
        if text[i] == "," or text[i] == ";":  # 允许连续分隔符（Lua 也允许）
            i += 1
            continue
        key: object = None
        m = re.compile(r"([A-Za-z_]\w*)\s*=(?!=)").match(text, i)
        if m:
            key = m.group(1)
            i = _skip_ws(text, m.end())
        if text[i] == "{":
            value, i = _parse_table(text, i)
        elif text[i] in "\"'":
            value, i = _parse_string(text, i)
        elif re.compile(r"[A-Za-z_]\w*").match(text, i):
            word = re.compile(r"[A-Za-z_]\w*").match(text, i).group(0)
            if word not in ("true", "false", "nil"):
                raise LuaParseError(
                    f"第 {_line(text, i)} 行出现裸标识符 `{word}`——能力清单是数据，"
                    "不含变量引用/函数（沙箱侧同样不落地）")
            value, i = (word == "true"), i + len(word)
            if word == "nil":
                value = None
        else:
            value, i = _parse_number(text, i)
        items.append((key, value))
        i = _skip_ws(text, i)
        if i < n and text[i] in ",;":
            i += 1

    if all(k is None for k, _ in items):
        return [v for _, v in items], i
    if all(isinstance(k, str) for k, _ in items):
        return {k: v for k, v in items}, i  # type: ignore[misc]
    raise LuaParseError("同一个表里混了位置键与命名键（JSON 表达不了，生成侧同样会抛）")


def parse_lua_table(text: str) -> list | dict:
    """读一份 Lua 文件里 `return { … }` 的那张表（本仓 schema 文件的唯一形态）。"""
    m = re.search(r"\breturn\s*\{", text)
    if not m:
        raise LuaParseError("文件里没有 `return {`（schema 文件必须 `return { … }`）")
    value, end = _parse_table(text, m.end() - 1)
    tail = _skip_ws(text, end)
    if tail < len(text) and text[tail] not in "":
        raise LuaParseError(f"第 {_line(text, tail)} 行 `return` 的表之后还有内容：{text[tail:tail + 20]!r}")
    return value


def _schema_root() -> tuple[dict, str | None]:
    """读 Lua schema 的根表 → (根表, 失效原因)。"""
    src = read_text(Path(ROOT) / RPC_SCHEMA_FILE)
    if src is None:
        return {}, f"能力清单 {RPC_SCHEMA_FILE} 读不到（改名 / 搬走了？）"
    try:
        root = parse_lua_table(src)
    except LuaParseError as exc:
        return {}, f"{RPC_SCHEMA_FILE} 解析失败：{exc}"
    if not isinstance(root, dict):
        return {}, f"{RPC_SCHEMA_FILE} 的根不是命名表（实际是 {type(root).__name__}）"
    return root, None


def _schema_endpoints() -> tuple[list[dict], str | None]:
    """读 Lua schema 的 endpoints → (逐条记录（**含嵌套 request / response**）, 失效原因)。"""
    root, bad = _schema_root()
    if bad:
        return [], bad
    endpoints = root.get("endpoints")
    if not isinstance(endpoints, list) or not endpoints:
        return [], f"{RPC_SCHEMA_FILE} 的 endpoints 缺失或解析出 0 条"
    if any(not isinstance(e, dict) for e in endpoints):
        return [], f"{RPC_SCHEMA_FILE} 的 endpoints 里有不是表的条目（每条端点必须是一条命名表）"
    return endpoints, None  # type: ignore[return-value]


def _schema_shapes(root: dict) -> tuple[list[dict], str | None]:
    """读 Lua schema 根级的 shapes（批 3 共享响应子结构）→ (逐条记录, 失效原因)。

    允许为空列表吗——不允许：response.fields 里 object 类字段必须引用 shape，而批 3 的表
    里本来就有共享结构。解析出 0 条 = 根键被删/改名，与 endpoints 同口径要报错而不是变绿。
    """
    shapes = root.get("shapes")
    if not isinstance(shapes, list) or not shapes:
        return [], (f"{RPC_SCHEMA_FILE} 的 shapes 缺失或解析出 0 条（批 3 的共享响应子结构；"
                    "根键被删/改名了？）")
    if any(not isinstance(s, dict) for s in shapes):
        return [], f"{RPC_SCHEMA_FILE} 的 shapes 里有不是表的条目（每条结构必须是一条命名表）"
    return shapes, None  # type: ignore[return-value]


# ─── 面 1 的 .cppm 侧解析（生成物文本 → 同构的 Python 记录）───────────────────────────

# 一条字段行（模板保证一条一行）：抽 `.设计器 = 值` 对，值形态各自一条正则。
_CPPM_FIELD_ROW_RE = re.compile(r'^\s*RpcFieldDef\{\.name = rpc_field::(?P<fid>\w+)::(?P<name>\w+).*\},\s*$')
_CPPM_FIELD_ATTR_RES = {
    "kind": re.compile(r'\.kind = RpcFieldKind::(\w+)'),
    "required": re.compile(r'\.required = (true|false)'),
    "nonEmpty": re.compile(r'\.nonEmpty = (true|false)'),
    "min": re.compile(r'\.minValue = ([-\d.eE+]+)'),
    "max": re.compile(r'\.maxValue = ([-\d.eE+]+)'),
    "error": re.compile(r'\.error = "((?:\\.|[^"\\])*)"'),
    "emptyError": re.compile(r'\.emptyError = "((?:\\.|[^"\\])*)"'),
    "oneOfError": re.compile(r'\.oneOfError = "((?:\\.|[^"\\])*)"'),
    "exclusiveError": re.compile(r'\.exclusiveError = "((?:\\.|[^"\\])*)"'),
}
_CPPM_EXCLUSIVE_RE = re.compile(r'\.exclusive = rpc_field::\w+::(\w+)')
_CPPM_ONEOF_REF_RE = re.compile(r'\.oneOf = rpc_schema_detail::kOneOf_(\w+)')
_CPPM_ONEOF_ARRAY_RE = re.compile(
    r'kOneOf_(?P<name>\w+)\{(?P<body>.*?)\n\};', re.S)
_CPPM_ONEOF_VALUE_RE = re.compile(r'std::string_view\{"((?:\\.|[^"\\])*)"\}')
_CPPM_CONST_NS_RE = re.compile(
    r'export namespace rpc_field::(?P<fid>\w+)\s*\{(?P<body>.*?)\}\s*//\s*namespace rpc_field',
    re.S)
_CPPM_CONST_RE = re.compile(
    r'inline constexpr std::string_view (?P<ident>\w+)\{"((?:\\.|[^"\\])*)"\}')


def _cpp_unescape(text: str) -> str:
    """把生成物字面量里的 `\\"` `\\\\` 还原成原文（模板侧的 cpp_escape 是反向操作）。"""
    return re.sub(r'\\(.)', lambda m: m.group(1), text)


def _cppm_products() -> tuple[list[dict], dict[str, dict[str, str]], str | None]:
    """读 `.cppm` 生成物 → (与 schema 同构的端点记录, 每端点的字段名常量表, 失效原因)。

    🔴 返回的记录**与 schema 同构**（`request.fields` 挂在端点记录里），好让面 1 用同一条
    深度比较判三方一致——而不是像批 1 那样"挑几个字段比"（挑着比 = 新加的键默认不在比较范围
    里，那正是"schema 变装饰"的机制版）。

    字段名从**常量表**取（`.name = rpc_field::camera::mode` 里的 `mode` 是常量标识符，它的
    值才是 `"mode"`）——两件事分开对账，才分得清"常量名漂了"与"常量值漂了"。
    解析不出任何一条路由 = 模板或生成物形态变了 ⟹ 交回失效原因（不许静默变绿）。
    """
    src = read_text(Path(ROOT) / RPC_SCHEMA_CPPM)
    if src is None:
        return [], {}, f"生成物 {RPC_SCHEMA_CPPM} 读不到（跑过 just reflect 了吗？）"

    oneofs: dict[str, list[str]] = {}
    for m in _CPPM_ONEOF_ARRAY_RE.finditer(src):
        oneofs[m.group("name")] = [_cpp_unescape(x)
                                   for x in _CPPM_ONEOF_VALUE_RE.findall(m.group("body"))]

    fields: dict[str, list[dict]] = {}
    for line in src.splitlines():
        m = _CPPM_FIELD_ROW_RE.match(line)
        if not m:
            continue
        rec: dict = {"name": m.group("name")}
        for key, rx in _CPPM_FIELD_ATTR_RES.items():
            hit = rx.search(line)
            if not hit:
                continue
            raw = hit.group(1)
            if key in ("required", "nonEmpty"):
                rec[key] = raw == "true"
            elif key in ("min", "max"):
                rec[key] = int(raw) if re.fullmatch(r"[-\d]+", raw) else float(raw)
            else:
                rec[key] = _cpp_unescape(raw)
        excl = _CPPM_EXCLUSIVE_RE.search(line)
        if excl:
            rec["exclusive"] = excl.group(1)
        ref = _CPPM_ONEOF_REF_RE.search(line)
        if ref:
            rec["oneOf"] = oneofs.get(ref.group(1), [])
        fields.setdefault(m.group("fid"), []).append(rec)

    consts: dict[str, dict[str, str]] = {}
    for m in _CPPM_CONST_NS_RE.finditer(src):
        consts[m.group("fid")] = {i: _cpp_unescape(v)
                                  for i, v in _CPPM_CONST_RE.findall(m.group("body"))}

    routes: list[dict] = []
    for m in RPC_SCHEMA_ROUTE_RX.finditer(src):
        method = m.group("method").upper()
        if method != m.group("methodText"):
            return [], {}, (f"生成物 {RPC_SCHEMA_CPPM} 内部不一致：RpcRouteMethod::"
                            f"{m.group('method')} 配 methodText {m.group('methodText')!r}"
                            "（模板写坏了？）")
        rec = {
            "id": m.group("id"), "method": method, "path": m.group("path"),
            "summary": _cpp_unescape(m.group("summary")),
            "timeout_ms": int(m.group("timeout")),
        }
        if m.group("manual") == "true":
            rec["manualShape"] = True
        declared = fields.get(m.group("id")) or []
        if declared:
            rec["request"] = {"fields": declared}
        routes.append(rec)
    if not routes:
        return [], {}, (f"生成物 {RPC_SCHEMA_CPPM} 里解析出 0 条 RpcRouteDef（模板或生成物形态"
                        "变了？同步改 tools/checks/rules_rpc.py 与 config.py 的 "
                        "RPC_SCHEMA_ROUTE_RE）")
    return routes, consts, None


def _json_root() -> tuple[dict, str | None]:
    """读 `.json` 生成物的**根对象** → (根, 失效原因)。批 3 起根级还有 shapes，
    端点级对账之外再对一次根级（module_name / version / shapes / 未来新根键默认被比）。"""
    raw = read_text(Path(ROOT) / RPC_SCHEMA_JSON)
    if raw is None:
        return {}, (f"生成物 {RPC_SCHEMA_JSON} 读不到（它是给非 C++ 客户端读的那一份，"
                    "与 .cppm 同一次 just reflect 生成）")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        return {}, f"生成物 {RPC_SCHEMA_JSON} 不是合法 JSON：{exc}"
    if not isinstance(data, dict):
        return {}, f"生成物 {RPC_SCHEMA_JSON} 的根不是对象"
    endpoints = data.get("endpoints")
    if not isinstance(endpoints, list) or not endpoints:
        return {}, f"生成物 {RPC_SCHEMA_JSON} 的 endpoints 缺失或为空"
    return data, None


def _json_endpoints() -> tuple[list[dict], str | None]:
    """读 `.json` 生成物的 endpoints → (逐条记录, 失效原因)。"""
    data, bad = _json_root()
    if bad:
        return [], bad
    return data["endpoints"], None


# ─── 面 6 的 .md 侧解析（生成物文本 → 与 schema 同构的记录）─────────────────────────────
#
# 锚定形态由 tools/templates/rpc_schema_md.inja 决定，两侧必须同步改；模板改形态而这里
# 认不出 → 解析出 0 条 → "本次未做任何对账"（失效方向是误报，不是假绿）：
#   端点标题     ### `GET /`（方法 + 空格 + 路径，整行）
#   响应清单起点  - 成功回包字段：（整行）
#   响应字段行    `  - \`name\`（…`（两空格缩进，一条一行；请求字段同形态但在标记之前）
#   共享结构标题  ### `camera`（无反引号内的空格 ⟹ 与端点标题不冲突）
#   结构字段行    `- \`name\`（…`（无缩进）

_MD_ENDPOINT_HEAD_RX = re.compile(r"^### `(GET|POST) ([^`]+)`$")
_MD_SHAPE_HEAD_RX = re.compile(r"^### `([A-Za-z_][A-Za-z0-9_]*)`$")
_MD_RESPONSE_MARKER = "- 成功回包字段："
_MD_INDENTED_FIELD_RX = re.compile(r"^  - `([A-Za-z_][A-Za-z0-9_]*)`")
_MD_TOP_FIELD_RX = re.compile(r"^- `([A-Za-z_][A-Za-z0-9_]*)`")


def _md_products() -> tuple[list[dict], list[dict], str | None]:
    """读 `.md` 生成物 → (端点记录 [{method,path,fields}], 结构记录 [{name,fields}], 失效原因)。

    fields = 该端点**成功回包**字段名的出现顺序（面 6 既比集合也比顺序：md 按 schema 顺序
    渲染，顺序漂了说明有人只改了一边）。
    """
    src = read_text(Path(ROOT) / RPC_SCHEMA_MD)
    if src is None:
        return [], [], (f"生成物 {RPC_SCHEMA_MD} 读不到（批 3 起它是面 6 的对账对象，"
                        "just reflect 一条 render 出三份产物）")
    endpoints: list[dict] = []
    shapes: list[dict] = []
    current: dict | None = None
    in_response = False
    in_shape = False
    for line in src.splitlines():
        m = _MD_ENDPOINT_HEAD_RX.match(line)
        if m:
            current = {"method": m.group(1), "path": m.group(2), "fields": []}
            endpoints.append(current)
            in_response, in_shape = False, False
            continue
        s = _MD_SHAPE_HEAD_RX.match(line)
        if s:
            current = {"name": s.group(1), "fields": []}
            shapes.append(current)
            in_response, in_shape = False, True
            continue
        if current is None:
            continue
        if in_shape:
            fm = _MD_TOP_FIELD_RX.match(line)
            if fm:
                current["fields"].append(fm.group(1))
            continue
        if line.rstrip() == _MD_RESPONSE_MARKER:
            in_response = True
            continue
        if in_response:
            fm = _MD_INDENTED_FIELD_RX.match(line)
            if fm:
                current["fields"].append(fm.group(1))
            else:
                in_response = False  # 清单结束（空行 / 下一节标题）
    if not endpoints:
        return [], [], (f"生成物 {RPC_SCHEMA_MD} 里解析出 0 条端点标题（模板或产物形态变了？"
                        "同步改 rules_rpc 的解析式与 rpc_schema_md.inja 的锚定形态）")
    if not shapes:
        return [], [], (f"生成物 {RPC_SCHEMA_MD} 里解析出 0 条共享结构标题"
                        "（「共享响应子结构」一节换形态了？同步改解析式与模板）")
    return endpoints, shapes, None


# ─── 面 5 的 response 段自洽校验（schema 侧结构规则）────────────────────────────────────

def _response_fields_problems(fields: list, where: str, shape_names: set) -> list[str]:
    """一条 response.fields（端点的或 shape 的，同构）→ 违规清单。判据逐条的为什么见模块 docstring 面 5。"""
    problems: list[str] = []
    seen: set[str] = set()
    for i, f in enumerate(fields):
        loc = f"{where} 的 fields[{i}]"
        if not isinstance(f, dict):
            problems.append(f"{loc} 不是命名表")
            continue
        name = f.get("name")
        if not isinstance(name, str) or not name:
            problems.append(f"{loc} 缺 name（客户端按名取值，没名字的登记等于没登记）")
            continue
        loc = f"{where} 的响应字段 {name}"
        if name in seen:
            problems.append(f"{loc} 在同一条回包内重名（后一条永远读不到）")
        seen.add(name)
        kind = f.get("kind")
        if kind not in RPC_RESPONSE_KINDS:
            problems.append(f"{loc} 的 kind {kind!r} 不在词表 {list(RPC_RESPONSE_KINDS)} 里"
                            "（词表 = config.RPC_RESPONSE_KINDS，加档要同步 rpc_schema_md.inja）")
        shape = f.get("shape")
        if kind in ("object", "object[]"):
            if not shape:
                problems.append(f"{loc} 是 {kind} 却没写 shape——子结构的字段清单没地方登记")
            elif shape not in shape_names:
                problems.append(f"{loc} 的 shape {shape!r} 在根级 shapes 里不存在（引用悬空）")
        elif shape:
            problems.append(f"{loc} 不是 object/object[] 却带了 shape {shape!r}")
        if f.get("optional") and not str(f.get("absentWhen") or "").strip():
            problems.append(f"{loc} 标了 optional 却没写 absentWhen——对客户端来说「能缺哪些」"
                            "与「有哪些」是同一份契约的两半，只登记前者等于没登记")
    return problems


def _response_shape_violations(schema_rows: list[dict], shapes: list[dict]) -> list[str]:
    """面 5：每个端点必须有 response；response/shapes 结构自洽；shapes 不许没人引用。"""
    out: list[str] = []
    shape_names: set[str] = set()
    names = [s.get("name") for s in shapes]
    for n in names:
        if not isinstance(n, str) or not n:
            out.append("shapes 里有条目缺 name")
        elif n in shape_names:
            out.append(f"shapes 里有条目重名：{n!r}（引用按名解析，重名 = 后一条不可达）")
        else:
            shape_names.add(n)
    used: set[str] = set()
    for e in schema_rows:
        eid = e.get("id", "?")
        resp = e.get("response")
        if not isinstance(resp, dict) or not isinstance(resp.get("fields"), list) or not resp["fields"]:
            out.append(f"端点 {eid} 没有非空的 response.fields——批 3 起每个端点必须登记"
                       "成功回包（字段清单读处理函数的实际回包，不许凭记忆或凭旧文档抄）")
            continue
        out.extend(_response_fields_problems(resp["fields"], f"端点 {eid}", shape_names))
        for f in resp["fields"]:
            if isinstance(f, dict) and f.get("shape"):
                used.add(str(f["shape"]))
    for s in shapes:
        sid = s.get("name") or "?"
        fields = s.get("fields")
        if not isinstance(fields, list) or not fields:
            out.append(f"共享结构 {sid} 的 fields 缺失或为空")
            continue
        out.extend(_response_fields_problems(fields, f"共享结构 {sid}", shape_names))
        for f in fields:
            if isinstance(f, dict) and f.get("shape"):
                used.add(str(f["shape"]))
    for s in shapes:
        sid = s.get("name")
        if isinstance(sid, str) and sid and sid not in used:
            out.append(f"共享结构 {sid} 没有任何端点或结构引用——没人读的结构就是装饰"
                       "（要么被 response 引用，要么从 shapes 删掉）")
    return out


# ─── 面 1 的比较器：整份记录深度比较，差异点名到路径 ──────────────────────────────────

def _diff_paths(a: object, b: object, path: str, ignore: frozenset[str] = frozenset()) -> list[str]:
    """两份同构记录 → 差异清单（每条 = `路径：A 侧 …，B 侧 …`）。

    为什么不是"逐字段挑着比"（批 1 的做法）：批 2 起一条端点记录里嵌着字段表，挑着比 =
    新加的键**默认不在比较范围内**（那正是"schema 变装饰"的机制版）。深度比较反过来：
    新加的键**默认被比**，要放过必须显式传 `ignore` 并说清为什么。
    """
    out: list[str] = []
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key in ignore:
                continue
            if key not in a:
                out.append(f"{path}.{key}：A 侧没有这个键，B 侧是 {_brief(b[key])}")
            elif key not in b:
                out.append(f"{path}.{key}：A 侧是 {_brief(a[key])}，B 侧没有这个键")
            else:
                out.extend(_diff_paths(a[key], b[key], f"{path}.{key}", ignore))
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}：A 侧 {len(a)} 条，B 侧 {len(b)} 条")
            return out
        for i, (x, y) in enumerate(zip(a, b)):
            out.extend(_diff_paths(x, y, f"{path}[{i}]", ignore))
        return out
    if a != b:
        out.append(f"{path}：A 是 {_brief(a)}，B 是 {_brief(b)}")
    return out


# 与 `.cppm` 对账时放过的键。**只准放"生成物按构造不可能带上"的键**，放业务事实 = 关掉判据。
#   manualReason：manualShape 的理由是给人和客户端读的（面 4 直接查 schema），C++ 里
#   没有任何代码读它 ⟹ 生成物不带它。与 `.json` 对账时**不**放过它（那份产物带）。
#   response：批 3 的响应形状是文档面（消费方 = md 参考与 .json 客户端产物），C++ 没有
#   消费者 ⟹ rpc_schema_cppm.inja 按设计不渲染它（模板头部写着这条）。它的一致性由两条
#   **更强的面**兜：与 `.json` 的整份深度对账（不放过任何键）+ 面 6 的 md 对账。放这里
#   只是声明"`.cppm` 按构造不带它"，不是"这条事实没人对账"。
_CPPM_ONLY_IGNORE = frozenset({"manualReason", "response"})


def _brief(value: object) -> str:
    text = repr(value)
    return text if len(text) <= 70 else text[:67] + "…"


def _compare(label_a: str, rows_a: list[dict], label_b: str, rows_b: list[dict],
             ignore: frozenset[str] = frozenset()) -> list[Warning]:
    """两份端点记录按顺序逐条深度对账 → 差异。调用方固定以 schema 为 A 侧（真相源）。"""
    warnings: list[Warning] = []
    if len(rows_a) != len(rows_b):
        warnings.append(Warning(RPC_SCHEMA_FILE, 0,
            f"RPC 单源对账：{label_a} {len(rows_a)} 条 ≠ {label_b} {len(rows_b)} 条 —— "
            "改了 schema 没跑 just reflect，或两份产物只重生成了一份", "error"))
    for i, (a, b) in enumerate(zip(rows_a, rows_b), start=1):
        aid = a.get("id", f"第 {i} 条")
        bid = b.get("id", f"第 {i} 条")
        if aid != bid:
            warnings.append(Warning(RPC_SCHEMA_FILE, 0,
                f"RPC 单源对账：第 {i} 条端点的 id 不一致 —— {label_a} 是 {aid!r}，"
                f"{label_b} 是 {bid!r}（顺序即注册顺序，别只改一边）", "error"))
            continue
        for diff in _diff_paths(a, b, f"{label_a} 侧的 {aid}", ignore):
            warnings.append(Warning(RPC_SCHEMA_FILE, 0,
                f"RPC 单源对账：{diff} —— {label_b} 是被手改了，或只重生成了一份产物"
                "（改了 schema 就 just reflect，并把 rgl/generated/ 两份产物一起提交）", "error"))
    return warnings


def _field_const_inconsistencies(consts: dict[str, dict[str, str]]) -> list[str]:
    """生成物**内部**自洽：字段名常量的标识符必须等于它的值。

    为什么单列一条：面 1 的 `.cppm` 记录里字段名取的是常量**标识符**（`rpc_field::camera::mode`
    的 `mode`），而运行期真正用的是常量**值**（`"mode"`）。手改其中一边，三方对账测不出来
    （schema 侧根本没有"标识符 / 值"这对概念）。这条兜的就是那个缝。
    """
    out: list[str] = []
    for fid, table in sorted(consts.items()):
        for ident, value in sorted(table.items()):
            if ident != value:
                out.append(f"rpc_field::{fid}::{ident} 的值是 {value!r}（标识符与值不一致）")
    return out


def _scan_files() -> tuple[list[str], str | None]:
    """面 2/3 的扫描文件清单 = 扫描目录下全部引擎模块，排除生成物目录。"""
    rels: list[str] = []
    for d in RPC_SCHEMA_SCAN_DIRS:
        base = Path(ROOT) / d
        if not base.is_dir():
            return [], (f"扫描面目录 {d} 不存在（rgl/ 改名或搬走了？同步改 "
                        "tools/checks/config.py 的 RPC_SCHEMA_SCAN_DIRS）")
        for suffix in RPC_SCHEMA_SCAN_SUFFIXES:
            for p in base.rglob("*" + suffix):
                rel = p.relative_to(ROOT).as_posix()
                if any(x in f"/{rel}" for x in RPC_SCHEMA_SCAN_EXCLUDE_DIRS):
                    continue
                rels.append(rel)
    if not rels:
        return [], f"扫描面解析出 0 个文件（{RPC_SCHEMA_SCAN_DIRS} 里没有 {RPC_SCHEMA_SCAN_SUFFIXES}？）"
    return sorted(set(rels)), None


def _field_consumers() -> tuple[str, str | None]:
    """面 3 的引用面 = 扫描目录全部 .cppm 拼成的文本（找 `rpc_field::<id>::<name>`）。"""
    rels, bad = _scan_files()
    if bad:
        return "", bad
    chunks: list[str] = []
    for rel in rels:
        src = read_text(Path(ROOT) / rel)
        if src is not None:
            chunks.append(src)
    if not chunks:
        return "", "引用面里一个文件都读不到（刚被删/搬走？）"
    return "\n".join(chunks), None


def rpc_schema_checks() -> list[Warning]:
    """RPC 能力单源的六个对账面（见模块 docstring）。"""
    warnings: list[Warning] = []

    # ── 失效必须出声：四方任一拿不到数 ⟹ 一条 error 说清"本次未做任何对账" ──
    schema_rows, schema_bad = _schema_endpoints()
    cppm_routes, cppm_consts, cppm_bad = _cppm_products()
    json_root, json_bad = _json_root()
    json_rows = json_root.get("endpoints", []) if not json_bad else []
    md_eps, md_shapes, md_bad = _md_products()
    schema_root, root_bad = _schema_root()
    shapes_rows, shapes_bad = (_schema_shapes(schema_root) if not root_bad
                               else ([], schema_bad))
    SCAN_RPC_SCHEMA.clear()
    SCAN_RPC_SCHEMA.update({
        "schema": str(len(schema_rows)), "cppm": str(len(cppm_routes)),
        "json": str(len(json_rows)), "md": str(len(md_eps)), "scanned": "0",
        "schema_fields": str(sum(len((e.get("request") or {}).get("fields") or [])
                                for e in schema_rows if isinstance(e, dict))),
        "cppm_fields": str(sum(len((e.get("request") or {}).get("fields") or [])
                               for e in cppm_routes)),
        "schema_resp_fields": str(sum(
            len((e.get("response") or {}).get("fields") or [])
            for e in schema_rows if isinstance(e, dict))),
        "md_resp_fields": str(sum(len(m["fields"]) for m in md_eps)),
        "shapes": str(len(shapes_rows)), "md_shapes": str(len(md_shapes)),
    })
    bads = [x for x in (schema_bad, cppm_bad, json_bad, md_bad, shapes_bad) if x]
    if bads:
        return [Warning(RPC_SCHEMA_FILE, 0,
            "RPC 能力单源对账（schema ↔ .cppm ↔ .json ↔ .md）**本次未做任何对账**："
            + "；".join(dict.fromkeys(bads)) + "——「零警告」不是「四方一致」", "error")]

    # ── 面 1：三方逐条深度对账（含批 2 的嵌套字段表与批 3 的 response 段）──
    warnings.extend(_compare("schema", schema_rows, ".cppm 生成物", cppm_routes,
                             _CPPM_ONLY_IGNORE))
    warnings.extend(_compare("schema", schema_rows, ".json 生成物", json_rows))
    # 根级对账（批 3）：endpoints 之外的根键（module_name / version / **shapes** / 未来新键）
    # 与 .json 整份深度比较——endpoints 已在上面按 id 归因比过，这里放掉避免双报。
    for diff in _diff_paths(schema_root, json_root, "schema 根级", frozenset({"endpoints"})):
        warnings.append(Warning(RPC_SCHEMA_FILE, 0,
            f"RPC 单源对账：{diff} —— {RPC_SCHEMA_JSON} 是被手改了，还是改了 schema 没跑 "
            "just reflect（根级新键默认进比较范围，与端点记录同口径）", "error"))
    for bad in _field_const_inconsistencies(cppm_consts):
        warnings.append(Warning(RPC_SCHEMA_CPPM, 0,
            f"RPC 生成物内部不一致：{bad} —— 模板写坏了？（对账取标识符，运行期吃值）", "error"))

    # ── 面 5：response 段自洽（批 3，schema 侧结构规则）──
    for bad in _response_shape_violations(schema_rows, shapes_rows):
        warnings.append(Warning(RPC_SCHEMA_FILE, 0, f"RPC 响应形状（批 3）：{bad}", "error"))

    # ── 面 6：生成的 md 参考 ↔ schema（批 3）──
    if len(md_eps) != len(schema_rows):
        warnings.append(Warning(RPC_SCHEMA_MD, 0,
            f"RPC md 参考对账：schema {len(schema_rows)} 条端点 ≠ {RPC_SCHEMA_MD} "
            f"{len(md_eps)} 条 —— 改了 schema 没跑 just reflect，或 md 产物被手改", "error"))
    for i, (e, m) in enumerate(zip(schema_rows, md_eps), start=1):
        want_head = f"{e.get('method')} {e.get('path')}"
        got_head = f"{m['method']} {m['path']}"
        if want_head != got_head:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：第 {i} 条端点不一致 —— schema 是 {want_head!r}，md 是 "
                f"{got_head!r}（条序 = 注册顺序 = schema 记录顺序，md 按它渲染）", "error"))
            continue
        want = [f.get("name") for f in (e.get("response") or {}).get("fields") or []]
        got = m["fields"]
        missing = [x for x in want if x not in got]
        extra = [x for x in got if x not in want]
        if missing or extra:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：{want_head} 的响应字段名集合不一致 —— schema 独有 "
                f"{missing}，md 独有 {extra} —— md 是被手改了，还是改了 schema 没跑 "
                "just reflect（三份产物一起生成，别只重生成一份）", "error"))
        elif want != got:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：{want_head} 的响应字段**顺序**不一致 —— schema 是 {want}，"
                f"md 是 {got}（md 按 schema 顺序渲染；顺序漂 = 有一侧被手工挪过）", "error"))
    md_shape_by_name = {s["name"]: s["fields"] for s in md_shapes}
    schema_shape_names = [s.get("name") for s in shapes_rows]
    for name in schema_shape_names:
        fields = [f.get("name") for f in (next(
            (s for s in shapes_rows if s.get("name") == name), {}).get("fields") or [])]
        if name not in md_shape_by_name:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：共享结构 {name!r} 在 md 里没有对应小节 —— md 被手改了，"
                "或改了 schema 没跑 just reflect", "error"))
        elif md_shape_by_name[name] != fields:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：共享结构 {name!r} 的字段清单不一致 —— schema 是 {fields}，"
                f"md 是 {md_shape_by_name[name]}", "error"))
    for name in md_shape_by_name:
        if name not in schema_shape_names:
            warnings.append(Warning(RPC_SCHEMA_MD, 0,
                f"RPC md 参考对账：md 里有共享结构 {name!r} 而 schema 的 shapes 里没有 —— "
                "md 是被手改了（schema 侧的 shapes 才是真相源）", "error"))

    # ── 面 4：manualShape 必须带 manualReason（止损线不许只写在注释里）──
    for e in schema_rows:
        if e.get("manualShape") and not str(e.get("manualReason") or "").strip():
            warnings.append(Warning(RPC_SCHEMA_FILE, 0,
                f"RPC 单源：端点 {e.get('id')} 标了 manualShape 却没写 manualReason ——"
                "manualShape 是一块**无机器对账**的残余面，'为什么表达不出来'必须留在表里",
                "error"))

    # ── 面 3：每个声明的字段都要有 rpc_field::<端点>::<字段> 的引用点 ──
    consumers, consume_bad = _field_consumers()
    if consume_bad:
        warnings.append(Warning(RPC_SCHEMA_FILE, 0,
            f"RPC 字段常量引用面**本次未做任何检查**：{consume_bad}", "error"))
    for e in schema_rows:
        for f in (e.get("request") or {}).get("fields") or []:
            name = f.get("name") if isinstance(f, dict) else None
            if not name:
                warnings.append(Warning(RPC_SCHEMA_FILE, 0,
                    f"RPC 单源：端点 {e.get('id')} 的 request.fields 里有条目没有 name", "error"))
                continue
            ref = f"rpc_field::{e.get('id')}::{name}"
            if ref not in consumers:
                warnings.append(Warning(RPC_SCHEMA_FILE, 0,
                    f"RPC 单源：字段 {ref} 在 rgl/ 里**没有任何引用点** —— 生成物里的常量没人读"
                    f"= 这条字段声明在装饰（处理函数改回吃字面量了？还是它已没人用？"
                    f"要么去用常量，要么把 {name} 从 {RPC_SCHEMA_FILE} 删掉）", "error"))

    # ── 面 2：手写注册/分发/字段名字面量 + 消费文件确实 import 生成模块 ──
    rels, scan_bad = _scan_files()
    if scan_bad:
        warnings.append(Warning(RPC_SCHEMA_FILE, 0,
            f"RPC 字面量扫描（面 2）**本次未做任何扫描**：{scan_bad}", "error"))
        rels = []
    SCAN_RPC_SCHEMA["scanned"] = str(len(rels))
    for rel in rels:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            warnings.append(Warning(rel, 0, "RPC 字面量扫描：文件读不到（刚被删/搬走？）", "error"))
            continue
        for rx in (RPC_ROUTE_LITERAL_RX, RPC_ROUTE_PATH_COMPARE_RX, RPC_ROUTE_METHOD_TEXT_RX):
            for m in rx.finditer(src):
                line = src[:m.start()].count("\n") + 1
                warnings.append(Warning(rel, line,
                    f"RPC 路由事实写在了代码里（第 {line} 行的 `{m.group(0)}`）——端点/方法/路径/"
                    f"超时的唯一来源是 {RPC_SCHEMA_FILE}，改它然后 just reflect；本行这条副本"
                    "今天能对上，改 schema 时没人会回来改它", "error"))
        if RPC_SCHEMA_FILE not in rel and _is_rpc_request_file(rel):
            for rx in RPC_FIELD_LITERAL_RXS:
                for m in rx.finditer(src):
                    line = src[:m.start()].count("\n") + 1
                    warnings.append(Warning(rel, line,
                        f"RPC 请求字段名写成了字面量（第 {line} 行的 `{m.group(0)}`）——字段名的"
                        f"唯一来源是 {RPC_SCHEMA_FILE}，本文件该用生成表的 "
                        "`rpc_field::<端点>::<字段>` 常量（改 schema 里的字段名才会编译不过）",
                        "error"))
    for rel in RPC_SCHEMA_CONSUMER_FILES:
        src = read_text(Path(ROOT) / rel)
        if src is None:
            warnings.append(Warning(rel, 0,
                f"RPC 单源的消费文件读不到（改名 / 搬走了？同步改 tools/checks/config.py 的 "
                f"RPC_SCHEMA_CONSUMER_FILES，别让这条面静默变瞎）", "error"))
            continue
        if not RPC_SCHEMA_IMPORT_RX.search(src):
            warnings.append(Warning(rel, 0,
                f"RPC 单源的消费文件里没有 `import {RPC_SCHEMA_IMPORT_MODULE};` 这条**语句** —— "
                "分发/注册不再查生成表 = 这条面已经瞎了（改回查表，或把本文件从名单里删掉"
                "并说明谁在消费；注释掉那行不算，判据认行首 import）", "error"))
    return warnings


def _is_rpc_request_file(rel: str) -> bool:
    """面 2 的字段字面量只扫**读请求体**的文件（`body` 是 RPC 请求体的变量名约定）。

    为什么限定：`body.at("…")` 这个写法在别处（配置解析 / 参数页）也可能出现，全局扫会误伤。
    名单按文件名挑 = 口径显式，新增读请求体的模块时把它加进来（漏加的后果是那条面少扫一个
    文件，而面 3 的"字段常量必须有人读"仍兜着装饰化）。
    """
    return any(part in rel for part in ("debug_rpc_exec_impl", "debug_rpc_burst",
                                        "debug_rpc_shape", "debug_rpc_payload"))
