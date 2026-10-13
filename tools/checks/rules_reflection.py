"""反射三方对账（债-12 的最后一半）：`generated/*.cppm` ↔ `*_schema.lua` ↔ `.slang`。

## 这条规则补的是哪一层（分工先说清，否则会被读成冗余）

`tests/test_reflection_layout.cppm` 头注释把布局契约分成四层，前三层早有护栏，**第四层
原文写着「留给后续批次（qa/ 或 tools/），本文件不臆造对账通道」**——本文件就是那个通道：

| 层 | 谁在守 |
|---|---|
| C++ struct ↔ 反射 | generated 文件自带 `static_assert`（编译期） |
| 生成物**内部**语义关系 | `tests/test_reflection_layout.cppm`（doctest） |
| C++ 侧手写偏移常量 ↔ 生成物 | `tests/` + `scene_data.cppm` 的 `static_assert` |
| 🔴 generated ↔ schema.lua ↔ shader 字面量 | **本文件** |

### 与构建开头 `reflection_check`（SHA1 护栏）的分工——本规则**不是**它的复刻

`cmake/ReflectionManifest.cmake` 只哈希**输入**（`.slang` + `_schema.lua` + `.inja`），
它守的是「改了输入忘了 `just reflect`」。它**看不见**的三类，正是本规则的射程：

1. **生成物被手改**：manifest 里根本没有产物的哈希。有人直接编辑 `rgl/generated/*.cppm`
   （文件头写着 DO NOT EDIT，但注释不夺非零码），SHA1 全绿、构建全绿，C++ 侧从此按错的
   偏移写 scene data。→ 面 1/2/4/5（generated ↔ schema ↔ shader）。
2. **`constants` 段反射根本不校验**：`tools/generator.cppm` 原话「反射端无对应数据，直接由
   schema 声明生成」。而 schema 常量真正的消费者常常是**另一枚不在该管线输入清单里的着色器**
   ——例：`model_schema.lua` 的 `kTrianglesPerGroup` 被 `model.mesh.slang` /
   `shadow.mesh.slang` / `izb_mask.mesh.slang` 各抄一份字面量，而这三个文件**都不是** model
   反射管线的输入 ⟹ 改它们既不触发 SHA1、也不进反射，C++ 侧 group 数照旧 = 静默错。
   → 面 3（schema 常量 ↔ 全仓 `.slang` 字面量）。
3. **`descriptorSlots` 的 binding 号在 slang 反射路径里完全不校验**：
   `tools/reflection_tool.cpp` 的 `slang` 子命令只跑 `validateSlangVertexLayout` /
   `validateSlangPushConst`，descriptorSlots 只被用来发 `heapSlotOffset` 常量
   （`validateSchema` 属于已退役的 JSON 路径）。⟹ schema 写 `index = 24`、shader 写
   `[[vk::binding(25, 0)]]`，反射一声不吭，运行期就是往错的堆槽读数据。
   → 面 6（schema descriptorSlots ↔ shader `[[vk::binding]]`）。

### 六条对账面（与**不覆盖**面同等重要）

1. **面 1 vertexLayouts ↔ generated struct**：struct 存在性、字段数、字段名与顺序、schema
   类型映射出的 C++ 类型、`sizeof == stride`、逐字段 `offsetof == offset`（双向：生成物有
   assert 而 schema 没这个字段也报）。
2. **面 2 constants / descriptorSlots 的 heapSlotOffset 常量 ↔ generated constexpr**：
   值相等 + 两侧都不许有孤儿。
3. **面 3 schema constants ↔ 全仓 `.slang` 的 `const uint <名> = <值>u;`**：出现才对账
   （不要求每个常量都有 shader 消费者——那是 `meshlet_spec_checks` 的死常量判据）。
   🔴 `meshlet_cull_schema.lua` 整份**豁免**：它的 constants 已由
   `rules_text.meshlet_spec_checks` 覆盖（且多两条关系对账），同一处漂移报两遍只会让人
   学会忽略警告。
4. **面 4 scalar block layout 独立重算 ↔ generated 常量**：从 `.slang` 的字段声明**独立重算**
   逐字段偏移与总尺寸（scalar layout = 逐字段紧排、零 padding，与
   `tools/schema_loader.cppm::vertexAttrSize` 同口径），再与 generated 的 `sizeof`/`offsetof`
   对账。**这一面不信任反射工具自己**：反射工具是 schema↔API 的唯一校验者、是 C++ 二进制、
   只在 `just reflect` 时跑；面 1 比的是 schema↔generated，两边**可以一起是旧的**，只有本面
   会因「shader 改了字段类型/顺序而两边都没跟上」变红。
5. **面 5 pushConstants ↔ generated struct + shader push 变量**：schema 的 pushConstants
   `name` 是 **slang 变量名**（drawArgs / cullPush），生成物里的 struct 名是**类型名**
   （DrawArgs / MeshletCullPush）——本面经 shader 的 `[[vk::push_constant]]` 把两者接起来，
   再比字段名/顺序/类型与独立重算的 sizeof。
6. **面 6 descriptorSlots ↔ shader `[[vk::binding(N, M)]]`**：binding 号与 shape↔声明类型一致。
   🔴 只查**正向**；反向（shader 声明了但 schema 没登记）刻意不查——`scene.slang` 有 7 条
   binding 而 `scene_forward_schema.lua` 一个 descriptorSlots 都不登记（那些槽走运行期
   allocSlot），反向判据会把现行合法形态全禁了。

### 本规则**管不到**的（别把它当布局保证）

- **C++ 侧手写的基址字面量 ↔ 生成物常量**（如 `scene_data.cppm` 的 `alignUp` 推导链）：
  编译期 `static_assert` 已把它变成编译错误，且 checks 是纯文本、不求值 C++ 常量表达式，
  做进来只会是 static_assert 的劣质复刻。已有归属：`tests/test_reflection_layout.cppm`。
- **消费者着色器里手写的字节偏移**（`model.mesh.slang` 的 `byteOffset + 12`）：把 `+12`
  归到 `ModelVertex.normal` 需要数据流分析，文本级归因会大量误报。存量由 R5 棘轮
  （`tools/check_shader_deps.py`）管「只准减」，语义对账不在本规则射程。
- **schema 未登记的 shader struct**：那是提取清单漏项，`reflection_tool` 查不到 struct 时
  自己会 [FAIL]，本规则不重复。
- **`features_generated` / `param_semantics_generated` 两条 render 管线**：输入是 JSON 不是
  schema+slang，没有第三方可对账（配对清单只取 `rgl_reflect_slang`，见 `_pipelines`）。
- **运行期设备能力**：本规则全是源码级文本对账，不碰设备。

### 失效必须出声（抄 `rules_layers` 的纪律：护栏静默失效比没有更坏）

配对清单读不到、schema 段存在却解析出 0 条、`.slang` struct 解析出 0 字段、声明符解析不动、
类型尺寸表缺名、配对着色器没解析到、契约 struct 在配对着色器里根本不存在 ⟹ **error 级**并
声明「本次未做任何对账」。覆盖面读数每次构建打印在 `[style-check] 反射三方对账: …` 一行
（`main.py` 挂载），让「跑过且扫了多大」一眼可见——一条「什么都没找到」的规则与一条
「扫错了目录」的规则输出必须不同。

🔴 **行号必须落在消息所属的那个文件**：三方对账同时拿着三个文件的行号，schema 的行号挂到
`.slang` 上就是指着无辜的一行人（踩过：`ModelVertex` 在第 9 行、报成 `model.slang:39`）。
每条消息的锚点 = 它自己 `rel` 里的位置，另一侧的位置写进消息正文。

🔴 唯一的收窄：**解析失效只在契约 struct 上出声**（名单由 schema 现算，见 `_watch_names`）。
非契约 struct（各 pass 的 `In` / `*VertexOutput`）本规则一条都不比对，在它们身上报
「解析不动」会把失效信号做成噪声墙——第一次落地时全仓 31 处语义标注字段
（`float4 position : SV_Position;`）加 6 处 `nointerpolation` 限定符就是这么刷出几十条 error
的，噪声墙的实际后果是没人再相信这条规则。代价：非契约 struct 若被契约 struct 嵌套引用，
schema 的 type 串同样登记它 ⟹ 自动进名单；真漏一层的写法（schema 没登记的二级嵌套）解析
失败只降级成面 4 的尺寸不符 warning，不是静默。

### severity 口径（本单拍板：先 WARNING 不阻断）

**对账不一致 = warning**（首次落地，先让它被看见，而不是先夺非零码）；
**规则自身失效 = error**（失效不出声时，「零警告」会被读成「布局自洽」）。
两者都**不阻断构建**——只有 `--strict` 才随其它警告 exit 1。

### 类型尺寸表是第二副本吗？（会被问，先答）

`REFLECTION_SCALAR_TYPE_SIZE` 与 `tools/schema_loader.cppm::vertexAttrSize` 是同一口径的两份
拼写（Python 侧读不了 C++ 常量）。**漂了会被立刻发现**：尺寸表错 ⟹ 面 4 手算的偏移同时与
schema 声明和 generated 常量两侧对不上 ⟹ 现状 0 条立刻变红。这份副本的漂移方向是「吵」，
不是「静默」，可以接受。
"""

import re
from pathlib import Path

from config import (REFLECTION_CONSTANTS_SHADER_EXEMPT, REFLECTION_PIPELINE_CMAKE,
                    REFLECTION_SCALAR_TYPE_SIZE, REFLECTION_SCHEMA_TO_CPP_TYPE,
                    REFLECTION_SHAPE_TO_SLANG_TYPES, ROOT)
from fs import read_text, rgl_tree_files
from models import Warning
from rules_text import strip_comments

# ─── 扫描面读数（main.py 每次构建打印，让「跑过且覆盖面多大」看得见）───────────

SCAN: dict[str, int] = {}


def scan_readout() -> str:
    """最近一次执行的覆盖面读数（未执行过 = 空串，调用方据此判断本规则有没有跑）。"""
    if not SCAN:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN.items())


def _fail(rel: str, line: int, msg: str) -> Warning:
    """规则自身失效 = error 级（不是「一条不一致」，是「本规则没脸自称是绿的」）。"""
    return Warning(rel, line, f"反射三方对账失效：{msg}", "error")


def _rx_lines(src: str, pattern: str):
    """按正则扫全文并回报**源文件行号**（行号 = 匹配起点所在行，注释已剥、换行未删）。"""
    rx = re.compile(pattern)
    for m in rx.finditer(src):
        yield (*m.groups(), src.count("\n", 0, m.start()) + 1)


# ─── Lua schema 迷你解析（stdlib-only：不引 lua 解释器，也不引 pip 依赖）────────


def _strip_lua_comments(src: str) -> str:
    """剥 `--` 行注释（字符串内的 `--` 不算），字符置空格、换行保留 ⟹ 行号不漂。"""
    out: list[str] = []
    i, n = 0, len(src)
    state = 0  # 0=code 1=line-comment 2=string
    while i < n:
        c = src[i]
        d = src[i + 1] if i + 1 < n else ""
        if state == 0:
            if c == "-" and d == "-":
                state = 1
                out.append("  ")
                i += 2
                continue
            if c in "\"'":
                state = 2
            out.append(c)
        elif state == 1:
            if c == "\n":
                state = 0
                out.append(c)
            else:
                out.append(" ")
        else:
            out.append(c)
            if c in "\"'":
                state = 0
        i += 1
    return "".join(out)


def _match_brace(text: str, open_idx: int) -> int:
    """与 `text[open_idx] == '{'` 配对的 `}` 下标；不配对返回 -1。"""
    depth = 0
    for idx in range(open_idx, len(text)):
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
            if depth == 0:
                return idx
    return -1


def _child_groups(text: str, lo: int, hi: int) -> list[tuple[int, int, str]]:
    """text[lo:hi] 的**直接子** `{…}` 组 → [(组内起, 组内止, 组内文本)]。

    🔴 下标一律**绝对**（相对整份源文件），调用方直接 `src.count("\\n", 0, idx) + 1` 取行号。
    踩过：早先返回相对 body 的下标，报出来的行号漂到文件另一头（警告指错行比不报更坏——
    人会照着行号去改一个无辜的字段）。本函数不碰文本，所以行号一定对得上源文件
    （注释已按字符等长替换过）。
    """
    out: list[tuple[int, int, str]] = []
    i = lo
    while i < hi:
        if text[i] == "{":
            close = _match_brace(text, i)
            if close < 0 or close >= hi:
                break
            out.append((i + 1, close, text[i + 1:close]))
            i = close + 1
            continue
        i += 1
    return out


# 🔴 **不能写 `\A`**：本模式总是配 `_SCALAR_RX.match(entry, i)` 带偏移用，而 `\A` 只匹配
# 字符串**绝对开头** ⟹ i>0 时永不命中，schema 侧静默全 0（踩过：对账全部空转，
# 只剩「解析不出 name」的失效噪声）。`re.match` 本身即锚定传入偏移。
_SCALAR_RX = re.compile(r'(\w+)\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|(-?\d+))')
_BLOCK_KEY_RX = re.compile(r"\b(\w+)\s*=\s*\{")


def _entry_scalars(entry: str) -> dict[str, str | int]:
    """一个 `{ … }` 组内的**本层**标量字段（`attributes = {…}` 之类的嵌套块整块跳过）。"""
    vals: dict[str, str | int] = {}
    i, n = 0, len(entry)
    while i < n:
        ch = entry[i]
        if ch == "{":
            close = _match_brace(entry, i)
            i = n if close < 0 else close + 1
            continue
        m = _SCALAR_RX.match(entry, i)
        if m:
            if m.group(2) is not None:
                vals[m.group(1)] = m.group(2)
            elif m.group(3) is not None:
                vals[m.group(3)] = m.group(3)
            else:
                vals[m.group(1)] = int(m.group(4))
            i = m.end()
            continue
        i += 1
    return vals


def _section(src: str, key: str) -> tuple[int, int] | None:
    """取 `key = { … }` 的**块体绝对区间** (inner_lo, inner_hi)；段不存在返回 None（段可合法缺席）。"""
    for m in _BLOCK_KEY_RX.finditer(src):
        if m.group(1) != key:
            continue
        close = _match_brace(src, m.end() - 1)
        if close < 0:
            return None
        return (m.end(), close)
    return None


class SchemaLayout:
    """一条 vertexLayouts 条目。"""

    def __init__(self, name: str, stride: int, attrs: list[dict], line: int):
        self.name = name
        self.stride = stride
        self.attrs = attrs  # [{name, type, offset, line}]
        self.line = line


class SchemaPush:
    """一条 pushConstants 条目（name = slang **变量名**，不是 struct 名）。"""

    def __init__(self, name: str, fields: list[dict], line: int):
        self.name = name
        self.fields = fields
        self.line = line


class SchemaSlot:
    """一条 descriptorSlots 条目（heapSlotOffset < 0 = 运行期 allocSlot，不发常量）。"""

    def __init__(self, name: str, index: int, shape: str, heap_offset: int,
                 cpp_name: str, line: int):
        self.name = name
        self.index = index
        self.shape = shape
        self.heap_offset = heap_offset
        self.cpp_name = cpp_name
        self.line = line


def _parse_schema(rel: str) -> tuple[dict, list[Warning]]:
    """解析一份 `*_schema.lua` → {layouts, pushes, slots, consts, expects_output}。

    🔴 段存在却解析出 0 条 = 失效（格式变了没同步本解析器），出 error 而不是返回空。
    """
    ws: list[Warning] = []
    raw = read_text(Path(ROOT) / rel)
    if raw is None:
        return {}, [_fail(rel, 0, f"读不到 schema 文件（{rel}）")]
    src = _strip_lua_comments(raw)

    def line_of(idx: int) -> int:
        """绝对下标 → 源文件行号（注释按字符等长替换、换行未删 ⟹ 一定对得上）。"""
        return src.count("\n", 0, idx) + 1

    layouts: list[SchemaLayout] = []
    pushes: list[SchemaPush] = []
    slots: list[SchemaSlot] = []
    consts: dict[str, tuple[int, int]] = {}

    def sub_groups(inner_lo: int, inner_hi: int, key: str):
        """条目体内 `key = { … }` 的直接子组 → [(组内起, 组内文本)]；没有该键返回 []。"""
        m = re.search(rf"\b{key}\s*=\s*\{{", src[inner_lo:inner_hi])
        if not m:
            return []
        open_idx = inner_lo + m.end() - 1
        close = _match_brace(src, open_idx)
        if close < 0:
            return []
        # _child_groups 的首元素已是「组内起」（`{` 之后第一个字符）⟹ 直接当行号锚点
        return [(a, c) for a, _b, c in _child_groups(src, open_idx + 1, close)]

    vl_sec = _section(src, "vertexLayouts")
    if vl_sec is not None:
        for lo, hi, inner in _child_groups(src, *vl_sec):
            vals = _entry_scalars(inner)
            line = line_of(lo)
            name = str(vals.get("name", ""))
            if not name:
                ws.append(_fail(rel, line,
                    "vertexLayouts 里有条目解析不出 name（格式变了？同步改 "
                    "tools/checks/rules_reflection.py::_parse_schema）"))
                continue
            vl = SchemaLayout(name, int(vals.get("stride", -1)), [], line)
            attrs = sub_groups(lo, hi, "attributes")
            if attrs:
                for a_lo, a_inner in attrs:
                    av = _entry_scalars(a_inner)
                    vl.attrs.append({
                        "name": str(av.get("name", "")),
                        "type": str(av.get("type", "")),
                        "offset": int(av.get("offset", -1)),
                        "line": line_of(a_lo),
                    })
            elif re.search(r"\battributes\s*=\s*\{", inner):
                ws.append(_fail(rel, line,
                    f"vertexLayout '{name}' 有 attributes 段却解析出 0 个 attribute"
                    "（本面对它是空跑）"))
            layouts.append(vl)

    pc_sec = _section(src, "pushConstants")
    if pc_sec is not None:
        for lo, hi, inner in _child_groups(src, *pc_sec):
            vals = _entry_scalars(inner)
            line = line_of(lo)
            name = str(vals.get("name", ""))
            fields: list[dict] = []
            for f_lo, f_inner in sub_groups(lo, hi, "fields"):
                fv = _entry_scalars(f_inner)
                fields.append({"name": str(fv.get("name", "")),
                               "type": str(fv.get("type", "")),
                               "line": line_of(f_lo)})
            if not name or not fields:
                ws.append(_fail(rel, line,
                    "pushConstants 条目解析不出 name/fields（格式变了？同步改 _parse_schema）"))
            pushes.append(SchemaPush(name, fields, line))

    ds_sec = _section(src, "descriptorSlots")
    if ds_sec is not None:
        for lo, hi, inner in _child_groups(src, *ds_sec):
            vals = _entry_scalars(inner)
            line = line_of(lo)
            name = str(vals.get("name", ""))
            if not name:
                ws.append(_fail(rel, line, "descriptorSlots 条目解析不出 name"))
                continue
            slots.append(SchemaSlot(name, int(vals.get("index", -1)),
                                    str(vals.get("shape", "")),
                                    int(vals.get("heapSlotOffset", -1)),
                                    str(vals.get("cppName", "")), line))

    c_sec = _section(src, "constants")
    if c_sec is not None:
        for lo, hi, inner in _child_groups(src, *c_sec):
            vals = _entry_scalars(inner)
            line = line_of(lo)
            name = str(vals.get("name", ""))
            if not name or "value" not in vals:
                ws.append(_fail(rel, line,
                    "constants 段里有条目解析不出 name/value（格式变了？同步改 "
                    "_parse_schema，别让它静默少对一条常量）"))
                continue
            consts[name] = (int(vals["value"]), line)
        if not consts:
            ws.append(_fail(rel, 1,
                "constants 段存在却解析出 0 条（格式变了？同步改 _parse_schema，"
                "别让它静默全绿）"))

    if vl_sec is None and pc_sec is None and ds_sec is None and c_sec is None:
        ws.append(_fail(rel, 1,
            "schema 里四个段（vertexLayouts / pushConstants / descriptorSlots / constants）"
            "一个都没找到——本文件本次未做任何对账"))
    expects = bool(layouts or pushes or consts
                   or any(s.heap_offset >= 0 and s.cpp_name for s in slots))
    return {"layouts": layouts, "pushes": pushes, "slots": slots, "consts": consts,
            "expects_output": expects}, ws


# ─── generated/*.cppm 解析 ──────────────────────────────────────────────────


class GenStruct:
    def __init__(self, name: str, line: int):
        self.name = name
        self.line = line
        self.fields: list[tuple[str, str, int]] = []  # (cpp_type, name, line)
        self.size: int | None = None
        self.size_line: int = line
        self.offsets: dict[str, tuple[int, int]] = {}  # field → (offset, line)


def _parse_generated(rel: str) -> tuple[dict, list[Warning]]:
    """解析生成物 → {structs: {名: GenStruct}, consts: {名: (值, 行)}}。

    生成物**可以合法为空**（`types_generated.cppm`：types_schema 只登记不带 heapSlotOffset
    的 descriptorSlots，模板因此不发任何 struct/constexpr）。空是不是失效由调用方按
    `expects_output` 判，本函数不越权。
    """
    ws: list[Warning] = []
    raw = read_text(Path(ROOT) / rel)
    if raw is None:
        return {}, [_fail(rel, 0, f"读不到生成物（{rel}）")]
    src = strip_comments(raw)
    lines = src.split("\n")

    structs: dict[str, GenStruct] = {}
    decl_rx = re.compile(r"^\s*export\s+struct\s+(\w+)\s*\{")
    field_rx = re.compile(r"^\s*([\w:]+(?:<[^>]*>)?)\s+(\w+)\s*;\s*$")
    cur: str | None = None
    for no, line in enumerate(lines, 1):
        m = decl_rx.match(line)
        if m:
            cur = m.group(1)
            structs[cur] = GenStruct(cur, no)
            continue
        if cur is not None:
            if re.match(r"^\s*\}\s*;", line):
                cur = None
                continue
            fm = field_rx.match(line)
            if fm:
                structs[cur].fields.append((fm.group(1), fm.group(2), no))

    for name, size, line in _rx_lines(
            src, r"static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*==\s*(\d+)"):
        if name in structs:
            structs[name].size = int(size)
            structs[name].size_line = line
    for name, field, off, line in _rx_lines(
            src, r"static_assert\s*\(\s*offsetof\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)"
                 r"\s*==\s*(\d+)"):
        if name in structs:
            structs[name].offsets[field] = (int(off), line)

    consts: dict[str, tuple[int, int]] = {}
    for name, value, line in _rx_lines(
            src, r"export\s+inline\s+constexpr\s+uint32_t\s+(\w+)\s*=\s*(\d+)\s*;"):
        consts[name] = (int(value), line)

    return {"structs": structs, "consts": consts}, ws


# ─── .slang 解析：struct 字段声明 / push 变量 / binding / 常量字面量 ───────────


class SlangField:
    def __init__(self, type_: str, name: str, count: str, line: int):
        self.type = type_
        self.name = name
        self.count = count  # 数组长度原文（"6" / "kMaxLights"），标量 = ""
        self.line = line


_DECL_RX = re.compile(r"\A([A-Za-z_]\w*)\s+(.+)\Z")
_DECLAR_RX = re.compile(r"\A(\w+)\s*(?:\[\s*([^\]]+?)\s*\])?\Z")
# 字段声明可带**插值/存储限定符**（`nointerpolation float4 uvA;`）与**语义标注**
# （`float4 position : SV_Position;`，可带分量下标 `: COLOR[0]`）。两者都是本面必须认识的
# 写法——踩过：语义标注没处理时，全仓 31 条字段刷 error，把「解析不动」这个失效信号
# 变成了噪声墙（噪声墙的实际后果是没人再相信这条规则）。
_FIELD_MODIFIER_RX = re.compile(
    r"\A(?:nointerpolation|precise|flat|centroid|sample|patch|groupshared|const|static"
    r"|uniform|volatile|readonly|writeonly|in|out|inout|export|public|private|internal"
    r"|extern|coherent|restrict)\s+")
_SEMANTIC_RX = re.compile(r"\s*:\s*[A-Za-z_]\w*(?:\s*\[\s*\w+\s*\])?\s*\Z")


def _strip_modifiers(decl: str) -> str:
    while True:
        m = _FIELD_MODIFIER_RX.match(decl)
        if not m:
            return decl
        decl = decl[m.end():]


def _split_declarators(rest: str) -> list[str]:
    """`c0, c1, c2, c3` / `uvA : TEXCOORD0, uvB : TEXCOORD1` → 各声明符（`[]` 内逗号不切）。"""
    out: list[str] = []
    buf: list[str] = []
    depth = 0
    for ch in rest:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(buf))
            buf = []
            continue
        buf.append(ch)
    out.append("".join(buf))
    return out


def _struct_bodies(src: str) -> dict[str, tuple[str, int]]:
    """`struct X { … }` → {名: (花括号内文本, 声明行)}（花括号配平）。"""
    out: dict[str, tuple[str, int]] = {}
    for m in re.finditer(r"\bstruct\s+(\w+)\s*\{", src):
        close = _match_brace(src, m.end() - 1)
        if close < 0:
            continue
        out[m.group(1)] = (src[m.end():close], src.count("\n", 0, m.start()) + 1)
    return out


def _field_decls(body: str) -> list[tuple[str, int]]:
    """struct 体内**以 `;` 收尾的本层片段** = 字段声明；成员函数（先遇 `{`）整段丢弃。

    行号取该声明**首个非空白字符**所在行——声明可跨行，报起点比报 `;` 更好定位。
    """
    out: list[tuple[str, int]] = []
    buf: list[str] = []
    start = 0
    i, n = 0, len(body)
    while i < n:
        ch = body[i]
        if ch == "{":
            close = _match_brace(body, i)
            i = n if close < 0 else close + 1
            buf = []
            continue
        if ch == ";":
            decl = re.sub(r"\s+", " ", "".join(buf)).strip()
            if decl:
                out.append((decl, 1 + body.count("\n", 0, start)))
            buf = []
            i += 1
            continue
        if not buf and not ch.isspace():
            start = i
        buf.append(ch)
        i += 1
    return out


def _parse_decls(decls: list[tuple[str, int]], ws: list[Warning], rel: str,
                 owner: str) -> list[SlangField]:
    """`float4 c0, c1, c2, c3;` / `float4 planes[6];` / `nointerpolation float4 uvA;`
    / `float3 n : NORMAL;` → SlangField 列表。"""
    fields: list[SlangField] = []
    for decl, line in decls:
        m = _DECL_RX.match(_strip_modifiers(decl))
        if not m:
            ws.append(_fail(rel, line,
                f"struct '{owner}' 的字段声明 '{decl}' 解析不动（写法变了？同步改 "
                "tools/checks/rules_reflection.py::_parse_decls，别静默少扫字段）"))
            continue
        for part in _split_declarators(m.group(2)):
            # 语义标注挂在**每个**声明符后面（`uvA : TEXCOORD0, uvB : TEXCOORD1`），
            # 所以先按逗号切、再逐个剥——先剥整串会只吃掉最后一个。
            part = _SEMANTIC_RX.sub("", part.strip())
            if not part:
                continue
            dm = _DECLAR_RX.match(part)
            if not dm:
                ws.append(_fail(rel, line,
                    f"struct '{owner}' 的声明符 '{part}'（原声明 '{decl}'）解析不动"
                    "（本面对它是空跑）"))
                continue
            fields.append(SlangField(m.group(1), dm.group(1),
                                     (dm.group(2) or "").strip(), line))
    return fields


def _parse_slang(rel: str, watch: set[str] | None = None) -> tuple[dict, list[Warning]]:
    """一份 `.slang` → {structs, consts, bindings, pushes}。

    `watch` = **本规则的契约 struct 名**（由 schema 侧给出，见 `_watch_names`）。解析失效只在
    契约 struct 上出声：非契约 struct（如各 pass 的 `In` / `*VertexOutput`）本规则一条都不比对，
    在它们身上报「解析不动」是把失效信号做成噪声墙。代价写进 docstring：非契约 struct 若被
    契约 struct 以字段类型嵌套引用，schema 的 type 串同样登记它 ⟹ 自动进 watch，不丢信号。
    """
    ws: list[Warning] = []
    raw = read_text(Path(ROOT) / rel)
    if raw is None:
        return {}, [_fail(rel, 0, f"读不到着色器（{rel}）")]
    src = strip_comments(raw)

    consts: dict[str, tuple[int, int]] = {}
    for name, value, line in _rx_lines(
            src, r"\b(?:static\s+)?const\s+uint\s+(\w+)\s*=\s*(\d+)u?\s*;"):
        consts[name] = (int(value), line)

    structs: dict[str, list[SlangField]] = {}
    struct_lines: dict[str, int] = {}
    for name, (body, sline) in _struct_bodies(src).items():
        loud = watch is None or name in watch
        decls = _field_decls(body)
        if not decls and loud:
            # struct 存在却没有字段声明 = 本面空跑。空 struct 在本仓不存在，出现即出声。
            ws.append(_fail(rel, sline, f"struct '{name}' 解析出 0 个字段声明（空跑）"))
        structs[name] = _parse_decls(decls, ws if loud else [], rel, name)
        struct_lines[name] = sline

    bindings: dict[str, tuple[int, str, int]] = {}  # 变量名 → (binding, 声明类型, 行)
    for b, type_, var, line in _rx_lines(
            src, r"\[\[\s*vk::binding\s*\(\s*(\d+)\s*,\s*\d+\s*\)\s*\]\]\s*"
                 r"([A-Za-z_]\w*)\s*(?:<[^>]*>)?\s*(\w+)\s*;"):
        bindings[var] = (int(b), type_, line)

    pushes: dict[str, tuple[str, int]] = {}  # push 变量名 → (struct 名, 行)
    for type_, var, line in _rx_lines(
            src, r"\[\[\s*vk::push_constant\s*\]\]\s*([A-Za-z_]\w*)\s+(\w+)\s*;"):
        pushes[var] = (type_, line)

    return {"structs": structs, "struct_lines": struct_lines, "consts": consts,
            "bindings": bindings, "pushes": pushes}, ws


# ─── scalar block layout 独立重算（与 schema_loader.cppm::vertexAttrSize 同口径）──


def _field_size(type_: str, count: str, structs: dict[str, list[SlangField]],
                consts: dict[str, tuple[int, int]],
                seen: frozenset[str]) -> tuple[int, str]:
    """字段字节数。→ (尺寸, 失败原因)；**原因非空 = 调用方必须出声**，此时尺寸是占位 0，
    绝不允许被拿去比对（`_recompute` 先查原因再取值，所以占位 0 到不了对账现场）。
    """
    if type_ in REFLECTION_SCALAR_TYPE_SIZE:
        base = REFLECTION_SCALAR_TYPE_SIZE[type_]
    elif type_ in structs:
        if type_ in seen:
            return 0, f"struct '{type_}' 递归引用自身，尺寸算不出"
        total = 0
        for f in structs[type_]:
            sub, why = _field_size(f.type, f.count, structs, consts,
                                   seen | {type_})
            if why:
                return 0, why
            total += sub
        base = total
    else:
        return 0, (f"类型 '{type_}' 既不在 REFLECTION_SCALAR_TYPE_SIZE 里、也不是本仓"
                   "已解析的 struct（新类型？补表并同步 "
                   "tools/schema_loader.cppm::vertexAttrSize）")
    if not count:
        return base, ""
    if count.isdigit():
        return base * int(count), ""
    hit = consts.get(count)
    if hit is None:
        return 0, (f"数组长度 '{count}' 既不是字面量、也不在本文件的 "
                   "`const uint <名> = <值>u;` 里 ⟹ 尺寸算不出")
    return base * hit[0], ""


def _recompute(fields: list[SlangField], structs: dict[str, list[SlangField]],
               consts: dict[str, tuple[int, int]]
               ) -> tuple[dict[str, int], int, str]:
    """scalar layout 紧排 ⟹ 偏移 = 前面字段尺寸之和。→ ({字段: 偏移}, 总尺寸, 失败原因)。"""
    offs: dict[str, int] = {}
    cursor = 0
    for f in fields:
        size, why = _field_size(f.type, f.count, structs, consts, frozenset())
        if why:
            return {}, 0, why
        offs[f.name] = cursor
        cursor += size
    return offs, cursor, ""


# ─── 管线配对清单（真相源 = cmake/RglReflection.cmake，不在 config 里抄名单）────


def _pipelines(ws: list[Warning]) -> list[tuple[str, str, str]]:
    """`rgl_reflect_slang(<slang> <schema> <module>)` → [(slang, schema, generated)]。"""
    rel_cmake = REFLECTION_PIPELINE_CMAKE
    cmake = read_text(Path(ROOT) / rel_cmake)
    if cmake is None:
        ws.append(_fail(rel_cmake, 0, f"读不到 {rel_cmake}，配对清单无从解析"))
        return []
    out: list[tuple[str, str, str]] = []
    rx = re.compile(r"rgl_reflect_slang\(\s*([\w./-]+)\s+([\w./-]+)\s+(\w+)\s*\)")
    for slang, schema, module in rx.findall(cmake):
        out.append((f"rgl/{slang}", f"rgl/{schema}", f"rgl/generated/{module}.cppm"))
    if not out:
        ws.append(_fail(rel_cmake, 1,
            f"{rel_cmake} 里一条 rgl_reflect_slang(...) 都没解析到（改名了？同步改 "
            "tools/checks/rules_reflection.py::_pipelines）——本次未做任何对账"))
    return out


# ─── 契约 struct 名单（决定 .slang 侧「谁解析失败必须出声」，不在 config 里抄名单）──


_PUSH_DECL_RX = re.compile(
    r"\[\[\s*vk::push_constant\s*\]\]\s*([A-Za-z_]\w*)\s+(\w+)\s*;")


def _watch_names(schemas: dict[str, dict], pipes: list[tuple[str, str, str]]
                 ) -> dict[str, set[str]]:
    """{配对着色器: 该文件里被 schema 契约引用的 struct 名（含 schema 登记过的嵌套 struct）}。

    名单**从 schema 现算**，不抄清单：vertexLayouts 名 + attributes/pushConstants 里非内置
    类型的类型名（`GpuLight[8]` / `ObjectMatrix` 这类嵌套 struct 因此自动进名单）
    + 该文件 `[[vk::push_constant]] <Struct> <var>;` 的结构名。
    只走 schema 声明的这一层（不解析 .slang 再往下钻）：schema 是提取清单，它没登记的嵌套
    struct 本来也不进反射，代价见模块 docstring「失效必须出声」一节。
    """
    out: dict[str, set[str]] = {}
    for slang_rel, schema_rel, _gen in pipes:
        schema = schemas.get(schema_rel)
        if not schema:
            continue
        names = out.setdefault(slang_rel, set())
        pending: list[str] = []
        for vl in schema["layouts"]:
            names.add(vl.name)
            pending += [a["type"] for a in vl.attrs]
        for pc in schema["pushes"]:
            pending += [f["type"] for f in pc.fields]
        while pending:
            base = re.sub(r"\[.*\]\Z", "", pending.pop().replace(" ", ""))
            if not base or base in names:
                continue
            if base in REFLECTION_SCALAR_TYPE_SIZE or base in REFLECTION_SCHEMA_TO_CPP_TYPE:
                continue  # 内置标量（slang 侧名 / schema 侧名两张表都认），不是 struct
            # 非内置类型 ⟹ 它是嵌套 struct，进名单（它的字段类型 schema 里也已登记，
            # 所以不必再往下钻；真漏一层的代价 = 该 struct 解析失败只降级成面 4 的
            # 尺寸不符 warning，不是静默）
            names.add(base)
        raw = read_text(Path(ROOT) / slang_rel)
        if raw is not None:
            for type_, _var in _PUSH_DECL_RX.findall(strip_comments(raw)):
                names.add(type_)
    return out


# ─── 主规则 ─────────────────────────────────────────────────────────────────


def reflection_contract_checks() -> list[Warning]:
    """反射三方对账。判据、覆盖面与不覆盖面见模块 docstring。"""
    ws: list[Warning] = []
    SCAN.clear()
    n_pipe = n_layout = n_push = n_slot = n_const = 0
    n_gen_struct = n_gen_const = n_shader_struct = n_scalar = 0
    n_watch = 0

    pipes = _pipelines(ws)
    n_pipe = len(pipes)

    # schema 先解析：契约 struct 名单决定 .slang 侧「谁的解析失败必须出声」
    schemas: dict[str, dict] = {}
    for _slang, schema_rel, _gen in pipes:
        doc, w = _parse_schema(schema_rel)
        ws.extend(w)
        if doc:
            schemas[schema_rel] = doc
    watch = _watch_names(schemas, pipes)
    n_watch = sum(len(v) for v in watch.values())

    # 全仓 .slang 一次解析：面 3 要扫全部着色器，面 4/5/6 按配对文件取用
    slang_all: dict[str, dict] = {}
    for rel in rgl_tree_files(".slang"):
        doc, w = _parse_slang(rel, watch.get(rel, set()))
        ws.extend(w)
        if doc:
            slang_all[rel] = doc
    if not slang_all:
        SCAN.update({"管线": n_pipe, "shader": 0})  # 读数全 0 = 空跑，与「没跑」区分开
        ws.append(_fail("rgl/shaders", 0,
            "rgl/shaders 下一个 .slang 都没解析到（目录改名？后缀变了？）"
            "——本次未做任何对账"))
        return ws

    schema_consts: dict[str, tuple[str, int, int]] = {}  # 名 → (schema, 值, 行)
    for slang_rel, schema_rel, gen_rel in pipes:
        schema = schemas.get(schema_rel)
        if not schema:
            continue  # 解析失效已按 schema_rel 出过错，不重复
        gen, w2 = _parse_generated(gen_rel)
        ws.extend(w2)
        if not gen:
            continue
        if schema["expects_output"] and not gen["structs"] and not gen["consts"]:
            ws.append(_fail(gen_rel, 1,
                f"schema {schema_rel} 声明了要发射的内容，生成物却一个 struct / constexpr "
                "都没有——本面对这条管线未做任何对账"))
            continue

        sdoc = slang_all.get(slang_rel, {})
        if not sdoc:
            ws.append(_fail(schema_rel, 1,
                f"配对着色器 {slang_rel} 没解析到（面 4/5/6 对这条管线是空跑）"))

        structs = sdoc.get("structs", {})
        n_layout += len(schema["layouts"])
        n_push += len(schema["pushes"])
        n_slot += len(schema["slots"])
        n_const += len(schema["consts"])
        n_gen_struct += len(gen["structs"])
        n_gen_const += len(gen["consts"])
        n_shader_struct += len(structs)
        n_scalar += len(schema["layouts"]) + len(schema["pushes"])

        _face_layouts(ws, schema_rel, gen_rel, schema["layouts"], gen)
        _face_constants(ws, schema_rel, gen_rel, schema["consts"], schema["slots"], gen)
        _face_push(ws, schema_rel, slang_rel, gen_rel, schema["pushes"],
                   schema["layouts"], gen, sdoc)
        _face_slots(ws, schema_rel, slang_rel, schema["slots"], sdoc)
        _face_scalar_layout(ws, schema_rel, slang_rel, gen_rel, schema["layouts"],
                            gen, structs, sdoc.get("struct_lines", {}),
                            sdoc.get("consts", {}))

        if schema_rel not in REFLECTION_CONSTANTS_SHADER_EXEMPT:
            for name, (value, line) in schema["consts"].items():
                schema_consts[name] = (schema_rel, value, line)

    n_literal = _face_shader_literals(ws, slang_all, schema_consts)

    SCAN.update({"管线": n_pipe, "schema struct": n_layout, "schema push": n_push,
                 "schema slot": n_slot, "schema 常量": n_const,
                 "generated struct": n_gen_struct, "generated 常量": n_gen_const,
                 "shader struct": n_shader_struct, "契约 struct": n_watch,
                 "手算布局": n_scalar, "shader 字面量对账": n_literal})
    if n_layout + n_push + n_const + n_slot == 0:
        ws.append(_fail(REFLECTION_PIPELINE_CMAKE, 1,
            "全部 schema 合计解析出 0 个 struct / 0 条常量 / 0 个 slot"
            "——本规则本次未做任何对账"))
    return ws


# ─── 面 1：vertexLayouts ↔ generated struct ─────────────────────────────────


def _face_layouts(ws: list[Warning], schema_rel: str, gen_rel: str,
                  layouts: list[SchemaLayout], gen: dict) -> None:
    contract = {l.name for l in layouts}  # 嵌套契约 struct 的类型名原样透传（面 1 类型比对用）
    for vl in layouts:
        gs = gen["structs"].get(vl.name)
        if gs is None:
            ws.append(Warning(gen_rel, 1,
                f"反射对账：schema {schema_rel}:{vl.line} 声明 vertexLayout '{vl.name}'，"
                "生成物里没有同名 struct（改了 schema 没 just reflect？或生成物被手改）",
                "warning"))
            continue
        if vl.stride < 0:
            ws.append(_fail(schema_rel, vl.line, f"vertexLayout '{vl.name}' 没有 stride"))
        elif gs.size is not None and vl.stride != gs.size:
            ws.append(Warning(gen_rel, gs.size_line,
                f"反射对账：'{vl.name}' 的 sizeof 不一致——schema {schema_rel}:{vl.line} "
                f"stride = {vl.stride}，生成物 static_assert = {gs.size}"
                "（生成物是产物：改 schema 后 just reflect，别手改生成物）", "warning"))
        if len(gs.fields) != len(vl.attrs):
            ws.append(Warning(gen_rel, gs.line,
                f"反射对账：'{vl.name}' 字段数不一致——schema {schema_rel}:{vl.line} 声明 "
                f"{len(vl.attrs)} 个，生成物有 {len(gs.fields)} 个", "warning"))
        for i, attr in enumerate(vl.attrs):
            if i >= len(gs.fields):
                break
            gtype, gname, gline = gs.fields[i]
            if gname != attr["name"]:
                ws.append(Warning(gen_rel, gline,
                    f"反射对账：'{vl.name}' 第 {i} 个字段名不一致——schema "
                    f"{schema_rel}:{attr['line']} 是 '{attr['name']}'，生成物是 '{gname}'",
                    "warning"))
            expected = _cpp_type(attr["type"], contract)
            if expected and gtype != expected:
                ws.append(Warning(gen_rel, gline,
                    f"反射对账：'{vl.name}.{attr['name']}' 的 C++ 类型不一致——schema 类型 "
                    f"'{attr['type']}' 应映射成 '{expected}'，生成物是 '{gtype}'", "warning"))
            if attr["offset"] < 0:
                ws.append(_fail(schema_rel, attr["line"],
                    f"vertexLayout '{vl.name}' 的 attribute '{attr['name']}' 没有 offset"))
                continue
            got = gs.offsets.get(attr["name"])
            if got is None:
                # 模板只对 offset != 0 的字段发 offsetof assert（inja `{% if f.offset %}`）
                if attr["offset"] != 0:
                    ws.append(Warning(gen_rel, gs.line,
                        f"反射对账：'{vl.name}.{attr['name']}' 在 schema 里 offset = "
                        f"{attr['offset']}，生成物却没有它的 offsetof static_assert"
                        "（生成物被手改？）", "warning"))
            elif got[0] != attr["offset"]:
                ws.append(Warning(gen_rel, got[1],
                    f"反射对账：'{vl.name}.{attr['name']}' 偏移不一致——schema "
                    f"{schema_rel}:{attr['line']} offset = {attr['offset']}，"
                    f"生成物 static_assert = {got[0]}", "warning"))
        known = {a["name"] for a in vl.attrs}
        for gname, (off, gline) in gs.offsets.items():
            if gname not in known:
                ws.append(Warning(gen_rel, gline,
                    f"反射对账：生成物 '{vl.name}.{gname}' 有 offsetof static_assert"
                    f"（= {off}），但 schema {schema_rel}:{vl.line} 的 vertexLayout 里没有"
                    "这个字段（生成物被手改，或 schema 漏字段）", "warning"))


def _cpp_type(schema_type: str, contract_structs: set[str]) -> str:
    """schema 类型 → 生成物里的 C++ 类型（与 schema_loader.cppm::vertexAttrCppType 同口径）。

    映射不出来返回空串 = 调用方跳过类型比对（**不猜**）；面 4 的尺寸表会独立兜住
    「新类型没进表」的情况（`_field_size` 对未知类型出声）。
    """
    if schema_type in REFLECTION_SCHEMA_TO_CPP_TYPE:
        return REFLECTION_SCHEMA_TO_CPP_TYPE[schema_type]
    if schema_type in contract_structs:
        return schema_type
    m = re.match(r"\A(\w+)\[(\d+)\]\Z", schema_type)
    if m:
        elem = _cpp_type(m.group(1), contract_structs)
        return f"std::array<{elem}, {m.group(2)}>" if elem else ""
    return ""


# ─── 面 2：constants / heapSlotOffset 常量 ↔ generated constexpr ──────────────


def _face_constants(ws: list[Warning], schema_rel: str, gen_rel: str,
                    consts: dict[str, tuple[int, int]], slots: list[SchemaSlot],
                    gen: dict) -> None:
    expected: dict[str, tuple[int, int]] = dict(consts)
    for slot in slots:
        if slot.heap_offset >= 0 and slot.cpp_name:
            expected[slot.cpp_name] = (slot.heap_offset, slot.line)

    for name, (value, sline) in expected.items():
        got = gen["consts"].get(name)
        if got is None:
            ws.append(Warning(gen_rel, 1,
                f"反射对账：schema {schema_rel}:{sline} 声明常量 {name} = {value}，生成物里"
                "没有这条 constexpr（改了 schema 没 just reflect？或生成物被手改）",
                "warning"))
        elif got[0] != value:
            ws.append(Warning(gen_rel, got[1],
                f"反射对账：契约常量 {name} 值不一致——schema {schema_rel}:{sline} 是 "
                f"{value}，生成物 constexpr 是 {got[0]}", "warning"))
    for name, (value, gline) in gen["consts"].items():
        if name not in expected:
            ws.append(Warning(gen_rel, gline,
                f"反射对账：生成物有 constexpr {name} = {value}，schema {schema_rel} 却没"
                "声明它（生成物被手改，或 schema 漏登记）", "warning"))


# ─── 面 3：schema constants ↔ 全仓 .slang 字面量 ─────────────────────────────


def _face_shader_literals(ws: list[Warning], slang_all: dict[str, dict],
                          schema_consts: dict[str, tuple[str, int, int]]) -> int:
    """`.slang` 里的 `const uint <名> = <值>u;` 若与某 schema 常量同名，值必须相等。

    返回**实际参与对账的字面量条数**（读数用：0 也要能区分「没有同名」与「本面空跑」）。
    🔴 豁免表在 config（meshlet_cull_schema.lua 归 meshlet_spec_checks），同一处漂移不报两遍。
    """
    hits = 0
    if not schema_consts:
        return 0
    for rel, doc in slang_all.items():
        for name, (value, line) in doc.get("consts", {}).items():
            hit = schema_consts.get(name)
            if hit is None:
                continue
            hits += 1
            schema_rel, want, sline = hit
            if value != want:
                ws.append(Warning(rel, line,
                    f"反射对账：着色器字面量 {name} = {value}，而 {schema_rel}:{sline} 的 "
                    f"constants 声明 {name} = {want}（两处必须同改；schema 是契约常量的唯一"
                    "声明点，C++ 侧只吃 schema 那一份，而本文件不在该管线的反射输入清单里 "
                    "⟹ 改这里既不触发 SHA1 也不进反射，只有本规则会红）", "warning"))
    return hits


# ─── 面 4：scalar block layout 独立重算 ↔ generated 常量 ──────────────────────


def _face_scalar_layout(ws: list[Warning], schema_rel: str, slang_rel: str, gen_rel: str,
                        layouts: list[SchemaLayout], gen: dict,
                        structs: dict[str, list[SlangField]],
                        struct_lines: dict[str, int],
                        consts: dict[str, tuple[int, int]]) -> None:
    for vl in layouts:
        fields = structs.get(vl.name)
        if fields is None:
            # 🔴 出声，不静默跳过：schema↔generated 那几面只比这两边（两边可以一起是旧的、
            # 一起是没有这个 struct 的），本面是**唯一**会因「着色器里没有这个契约 struct」
            # 变红的地方。行号挂生成物侧的对应文件 = 本文件第 1 行（着色器里没有它，没有
            # 更准的锚点），schema 的行号在消息里给全。
            ws.append(_fail(slang_rel, 1,
                f"schema {schema_rel}:{vl.line} 声明 vertexLayout '{vl.name}'，"
                f"{slang_rel} 里解析不到同名 struct（改名了？搬进别的文件了？"
                "——反射工具的提取清单就是它，本面对这条布局是空跑）"))
            continue
        offs, total, why = _recompute(fields, structs, consts)
        if why:
            # 行号必须落在**本消息所属文件**：vl.line 是 schema 的行号，挂到 slang 上就是
            # 指错行（踩过：报成 model.slang:39，而 struct 在第 9 行）
            ws.append(_fail(slang_rel, struct_lines.get(vl.name, 1),
                f"'{vl.name}' 的 scalar layout 尺寸算不出：{why}"))
            continue
        gs = gen["structs"].get(vl.name)
        if gs is None:
            continue
        if gs.size is not None and gs.size != total:
            ws.append(Warning(gen_rel, gs.size_line,
                "反射对账（独立重算）："
                f"'{vl.name}' 的 sizeof 与着色器声明对不上——{slang_rel} 逐字段紧排 = "
                f"{total}B，生成物 static_assert = {gs.size}B（要么着色器改了字段类型/顺序/"
                "数量而生成物没跟上，要么生成物被手改——生成物是产物，只准 just reflect 生成；"
                "scalar layout 零 padding，尺寸就是字段尺寸之和）",
                "warning"))
        for gname, (off, gline) in gs.offsets.items():
            if gname in offs and offs[gname] != off:
                ws.append(Warning(gen_rel, gline,
                    "反射对账（独立重算）："
                    f"'{vl.name}.{gname}' 偏移不一致——按 {slang_rel} 的字段声明序紧排应在 "
                    f"{offs[gname]}，生成物 static_assert = {off}"
                    "（要么着色器字段序/类型改了而生成物没跟上，要么生成物被手改）",
                    "warning"))
        if vl.stride >= 0 and vl.stride != total:
            ws.append(Warning(schema_rel, vl.line,
                f"反射对账（独立重算）：'{vl.name}' 的 stride 与着色器声明不符——schema 写 "
                f"{vl.stride}，按 {slang_rel} 逐字段紧排 = {total}"
                "（schema 的 stride 必须等于 shader struct 的字节尺寸）", "warning"))


# ─── 面 5：pushConstants ↔ generated struct + shader push 变量 ────────────────


def _face_push(ws: list[Warning], schema_rel: str, slang_rel: str, gen_rel: str,
               pushes: list[SchemaPush], layouts: list[SchemaLayout], gen: dict,
               sdoc: dict) -> None:
    """schema 的 pushConstants `name` 是 **slang 变量名**，生成物里的名字是 **struct 名**。

    接缝靠 shader 的 `[[vk::push_constant]] <Struct> <var>;` 接起来。push 侧 schema 不声明
    偏移（取 API 真值），所以偏移只对账「独立重算 ↔ generated」。
    """
    if not pushes:
        return
    pdoc = sdoc.get("pushes", {})
    structs = sdoc.get("structs", {})
    consts = sdoc.get("consts", {})
    contract = {l.name for l in layouts} | set(structs)
    for pc in pushes:
        entry = pdoc.get(pc.name)
        if entry is None:
            ws.append(Warning(slang_rel, 1,
                f"反射对账：schema {schema_rel}:{pc.line} 声明 push 变量 '{pc.name}'，"
                f"{slang_rel} 里找不到 `[[vk::push_constant]] … {pc.name};`（改名了？"
                "schema 的 name 必须是 slang 变量名，不是 struct 名）", "warning"))
            continue
        struct_name, bline = entry
        gs = gen["structs"].get(struct_name)
        if gs is None:
            ws.append(Warning(gen_rel, 1,
                f"反射对账：push 变量 '{pc.name}' 的类型是 '{struct_name}'"
                f"（{slang_rel}:{bline}），生成物里没有同名 struct", "warning"))
            continue
        if len(gs.fields) != len(pc.fields):
            ws.append(Warning(gen_rel, gs.line,
                f"反射对账：push struct '{struct_name}' 字段数不一致——schema "
                f"{schema_rel}:{pc.line} 声明 {len(pc.fields)} 个，生成物有 "
                f"{len(gs.fields)} 个", "warning"))
        for i, pf in enumerate(pc.fields):
            if i >= len(gs.fields):
                break
            gtype, gname, gline = gs.fields[i]
            if gname != pf["name"]:
                ws.append(Warning(gen_rel, gline,
                    f"反射对账：push struct '{struct_name}' 第 {i} 个字段名不一致——schema "
                    f"{schema_rel}:{pf['line']} 是 '{pf['name']}'，生成物是 '{gname}'",
                    "warning"))
            expected = _cpp_type(pf["type"], contract)
            if expected and gtype != expected:
                ws.append(Warning(gen_rel, gline,
                    f"反射对账：'{struct_name}.{pf['name']}' 的 C++ 类型不一致——schema 类型 "
                    f"'{pf['type']}' 应映射成 '{expected}'，生成物是 '{gtype}'", "warning"))
        fields = structs.get(struct_name)
        if not fields:
            ws.append(_fail(slang_rel, bline,
                f"push 变量 '{pc.name}' 的类型 '{struct_name}' 在本文件解析不到字段声明"
                "（面 5 的 sizeof 独立重算对它是空跑）"))
            continue
        _offs, total, why = _recompute(fields, structs, consts)
        if why:
            # 行号落在本消息所属文件（slang）：pc.line 是 schema 的行号，不能挂这儿
            ws.append(_fail(slang_rel,
                            sdoc.get("struct_lines", {}).get(struct_name, bline),
                            f"push struct '{struct_name}' 的尺寸算不出：{why}"))
        elif gs.size is not None and gs.size != total:
            ws.append(Warning(gen_rel, gs.size_line,
                "反射对账（独立重算）："
                f"push struct '{struct_name}' 的 sizeof 不一致——按 {slang_rel} 逐字段"
                f"紧排 = {total}B，生成物 static_assert = {gs.size}B", "warning"))


# ─── 面 6：descriptorSlots ↔ shader [[vk::binding]] ──────────────────────────


def _face_slots(ws: list[Warning], schema_rel: str, slang_rel: str,
                slots: list[SchemaSlot], sdoc: dict) -> None:
    if not slots:
        return
    bindings = sdoc.get("bindings", {})
    for slot in slots:
        got = bindings.get(slot.name)
        if got is None:
            ws.append(Warning(slang_rel, 1,
                f"反射对账：schema {schema_rel}:{slot.line} 登记 descriptorSlot "
                f"'{slot.name}'，{slang_rel} 里没有它的 `[[vk::binding]]` 声明"
                "（改名了？还是删了没同步 schema）", "warning"))
            continue
        index, type_, bline = got
        if slot.index < 0:
            ws.append(_fail(schema_rel, slot.line,
                f"descriptorSlot '{slot.name}' 没有 index"))
            continue
        if index != slot.index:
            ws.append(Warning(slang_rel, bline,
                f"反射对账：'{slot.name}' 的 binding 号不一致——shader 写 "
                f"[[vk::binding({index}, 0)]]，schema {schema_rel}:{slot.line} 声明 "
                f"index = {slot.index}（slang 反射路径**不校验** binding 号 ⟹ 运行期往错的"
                "堆槽读数据，静默画错）", "warning"))
        want = REFLECTION_SHAPE_TO_SLANG_TYPES.get(slot.shape)
        if want is None:
            ws.append(_fail(schema_rel, slot.line,
                f"descriptorSlot '{slot.name}' 的 shape '{slot.shape}' 不在 "
                "REFLECTION_SHAPE_TO_SLANG_TYPES 里（新 shape？补表）"))
            continue
        if type_ not in want:
            ws.append(Warning(slang_rel, bline,
                f"反射对账：'{slot.name}' 的声明类型与 schema shape 不符——shader 声明成 "
                f"'{type_}'，schema {schema_rel}:{slot.line} 的 shape = '{slot.shape}'"
                f"（应匹配 {' 或 '.join(want)}）", "warning"))
