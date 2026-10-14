"""compute 可写 binding 对账（B2 的护栏半边）：反射产物 ↔ 人工基线。

## 这条规则回答的问题，以及为什么必须有两个来源

图外 compute 的同步对账要回答「这个 dispatch 会写哪些资源」。它**不能**只靠人工申报：
申报者和被判者是同一个人，漏一条边的人同样会漏一条申报（docs/compute_调研.md §7.10）。
所以有两个来源，本规则是它们的**对账**：

| 来源 | 答什么 | 答不了什么 |
|---|---|---|
| 产物 `rgl/generated/compute_access.json`（Slang 反射，`just reflect` 生成） | 「这里**会写**」（声明级） | 「写完谁读」——没有读者就没有写后读 hazard |
| 基线 `tools/checks/compute_write_baseline.json`（人工，带 reason） | 「这条写我们**评审过**，要不要边、为什么」 | 「代码里到底还有没有这条写」——它会陈旧 |

两个来源各自都会静默腐烂，对账才成立：产物腐烂（改了 shader 没 `just reflect`）由
`reflection_check` 的 SHA1 守卫管；**基线腐烂由本规则管**，它同时反向盯产物的完整性。

## 两条判据（都是 error）

1. **产物可写、基线没登记** → error。新增一条写 = 新增一次「要不要同步边」的裁决，
   裁决没落处就是没做过。
2. **基线登记了、产物里已不是可写** → error（陈旧基线）。基线是**裁决记录**不是历史存档：
   留着一条已经不存在的写，下一次读它的人会按一个不存在的资源去推理同步关系。

`access == "none"`（ConstantBuffer / push constant）与 `"read"` **跳过**——前者实测从不与
可写重合（§7.11 的八种声明映射表），后者不需要边。`"unknown"`（反射没分类 / slang 新增
取值）单独出 **warning**：当成可写会误报、当成只读会静默漏报，两个猜测都比「吵一声不知道」
更坏。

## 判据的天花板（别把它当同步正确性）

- 🔴 **声明级，不是使用级**：`RW*` 声明即使体内只读不写也算可写。自家实证 =
  `evssm_downsample` 的 binding 0 `mipSrc`（声明 RWTexture2D 只为从 GENERAL 布局 load，
  体内只读）。⟹ 判据**保守**（宁可多要求一次裁决），代价是基线里会出现「其实不写」的条目——
  那条信息只有人给得出，正是基线 reason 字段存在的理由，不是判据的缺陷。
- **不判「该不该有边」**：基线只记 reason，不记边表；本规则只保证「每一条写都被**看过**」。
- **不判读侧**：「写 R 且之后有人读 R」才要求边，而读侧判定要跨 dispatch 的数据流，
  静态判不了（§7.11 读数 2 的 `izb_prime.primed` 就是无读者的合法写，不需要边）。
- **只覆盖进了这条反射管线的 compute**：名单在 `cmake/RglReflection.cmake` 的
  `rgl_reflect_compute_access(...)` 调用处，不在本文件抄（抄名单反模式）。

## 失效必须出声（抄 `rules_layers` 的纪律：护栏静默失效比没有更坏）

产物读不到 / 不是合法 JSON / 没有 `shaders` 段 / param 缺 `access` 字段（产物比规则旧）、
基线读不到 / 不是合法 JSON / 没有 `writes` 段 / 条目缺 shader·binding·param / 同一条登记两次
⟹ **error** 并声明「本次未做任何对账」。理由：本规则的存在意义是「没有一条 compute 写能
不被看见」，而「文件读不到」与「对账干净」的输出一模一样时，前者会被读成后者。

行号锚点 = 消息所属文件里那一行（产物侧的「新增」挂产物、基线侧的「陈旧」挂基线）；
JSON 被重排成多行形态时锚点退化成第 1 行——退化只让定位变粗，不让消息消失。
"""

import json
import re
from pathlib import Path

from config import (COMPUTE_ACCESS_PRODUCT, COMPUTE_WRITABLE_ACCESS,
                    COMPUTE_WRITE_BASELINE, ROOT)
from fs import read_text
from models import Warning

# 行号锚点：产物与基线都是「一行一条」（生成器钉死 2 空格缩进 + 每行一条 param），
# 所以能按行匹配。JSON 被重排时匹配不到 ⟹ 退化到第 1 行，不丢消息。
_STR = r'((?:[^"\\]|\\.)*)'
_PARAM_RX = re.compile(rf'"binding":\s*(\d+)\s*,\s*"param":\s*"{_STR}"')
_SHADER_RX = re.compile(rf'"shader":\s*"{_STR}"')
_WRITE_RX = re.compile(rf'"shader":\s*"{_STR}"\s*,\s*"binding":\s*(\d+)\s*,\s*'
                       rf'"param":\s*"{_STR}"')

# 一条记录 = (shader, binding, param)。三元组而不是二元组：同 binding 换名（重命名）与
# 换 binding（挪号）是两种不同的改动，都必须在对账里显形，不能被二元组糊成「同一条」。
Key = tuple[str, int, str]

# ─── 扫描面读数（main.py 每次构建打印，让「跑过且覆盖面多大」看得见）───────────
# 立论同 rules_reflection：一条「什么都没找到」的规则与一条「读不到文件」的规则，
# 输出必须不同——否则「零警告」在两种情况下长得一模一样。
SCAN: dict[str, int] = {}


def scan_readout() -> str:
    """最近一次执行的覆盖面读数（未执行过 = 空串，调用方据此判断本规则有没有跑）。"""
    if not SCAN:
        return ""
    return " / ".join(f"{k} {v}" for k, v in SCAN.items())


def _scan_empty() -> None:
    """失效退出时的读数：全 0 + 一条 error。0 不是「干净」，是「没数」，靠同批 error 区分。"""
    SCAN.clear()
    SCAN.update({"可写": 0, "基线": 0, "对账": 0})


def _fail(rel: str, line: int, msg: str) -> Warning:
    """规则自身失效 = error 级（不是「一条不一致」，是「本规则没脸自称是绿的」）。"""
    return Warning(rel, line, f"compute 可写 binding 对账失效：{msg}", "error")


def _load(rel: str, key: str) -> tuple[str | None, list | None, list[Warning]]:
    """读一份 JSON（产物或基线）并取出顶层数组段。失败一律 error，**绝不返回空表**。"""
    src = read_text(Path(ROOT) / rel)
    if src is None:
        return None, None, [_fail(rel, 0,
            f"读不到文件——本次未做任何对账。本规则守的是「每一条 compute 写都被评审过」，"
            "而文件不在时也会打出「零警告」，不区分这两者就是亲手造出它要防的那种假绿")]
    try:
        doc = json.loads(src)
    except json.JSONDecodeError as e:
        return None, None, [_fail(rel, 1, f"不是合法 JSON（{e}）——本次未做任何对账")]
    if not isinstance(doc, dict):
        return None, None, [_fail(rel, 1, "顶层不是对象——本次未做任何对账")]
    rows = doc.get(key)
    if not isinstance(rows, list):
        return None, None, [_fail(rel, 1,
            f"顶层没有 `{key}` 数组段（改名了？输出形态变了？同步改 "
            "tools/checks/rules_compute_access.py）——本次未做任何对账")]
    return src, rows, []


def _line_of(src: str, pred) -> int:
    """按行找第一条满足 pred 的行 → 1-based 行号；找不到返回 1（退化锚点，不返回 0）。"""
    for no, line in enumerate(src.splitlines(), 1):
        if pred(line):
            return no
    return 1


def _product_line(src: str, key: Key) -> int:
    """产物里那条 param 的行号。必须带 shader 上下文——同 binding 号在不同 shader 遍地都是。"""
    shader, binding, param = key
    cur = ""
    for no, line in enumerate(src.splitlines(), 1):
        sm = _SHADER_RX.search(line)
        if sm:
            cur = sm.group(1)
        if cur != shader:
            continue
        pm = _PARAM_RX.search(line)
        if pm and int(pm.group(1)) == binding and pm.group(2) == param:
            return no
    return 1


def _baseline_line(src: str, key: Key) -> int:
    """基线里那条 write 的行号（一行一条：shader/binding/param 同处一行才认）。"""
    def hit(line: str) -> bool:
        m = _WRITE_RX.search(line)
        return bool(m) and (m.group(1), int(m.group(2)), m.group(3)) == key
    return _line_of(src, hit)


def _key_of(shader: object, binding: object, param: object) -> Key | None:
    """三个原始 JSON 值 → Key；形态不对返回 None（调用方出声）。

    🔴 本规则跑在主进程且**不许抛异常**：checks 非阻断，但 uncaught 异常 exit 1 会让 ninja
    误判 source_checks 失败、-k1 停调度拖死整条构建（同 worker BrokenProcessPool 教训）。
    所以来自文件的每个值都先过类型检查，而不是 `int(row["binding"])` 直接转。
    """
    if not isinstance(shader, str) or not isinstance(param, str):
        return None
    if isinstance(binding, bool) or not isinstance(binding, int):
        return None
    return (shader, binding, param)


def compute_write_coverage_checks() -> list[Warning]:
    """反射产物 ↔ 人工基线双向对账。判据、天花板与失效面见模块 docstring。"""
    prod_rel, base_rel = COMPUTE_ACCESS_PRODUCT, COMPUTE_WRITE_BASELINE
    SCAN.clear()

    prod_src, prod_rows, ws = _load(prod_rel, "shaders")
    if prod_rows is None:
        _scan_empty()
        return ws
    base_src, base_rows, wb = _load(base_rel, "writes")
    ws.extend(wb)
    if base_rows is None:
        _scan_empty()
        return ws

    # ── 产物侧：收集可写集合（access ∈ COMPUTE_WRITABLE_ACCESS）───────────────
    product: dict[Key, int] = {}
    n_param = n_skip = n_unknown = 0
    for sh in prod_rows:
        shader = sh.get("shader") if isinstance(sh, dict) else None
        if not isinstance(shader, str):
            ws.append(_fail(prod_rel, 1,
                "shaders 里有条目解析不出 shader 名（生成器换了输出形态？同步改本文件）"))
            continue
        params = sh.get("params")
        if not isinstance(params, list):
            ws.append(_fail(prod_rel, 1, f"'{shader}' 没有 params 数组段（本条对它是空跑）"))
            continue
        for p in params:
            if not isinstance(p, dict) or "access" not in p:
                # 🔴 缺 access = 产物由**旧版生成器**产出（access 是本次新增字段）。必须出声
                # 而不是当成「没有可写」：旧产物 + 新基线 = 双向都判不出不一致 = 纯假绿。
                ws.append(_fail(prod_rel, 1,
                    f"'{shader}' 有条目没有 access 字段——产物比本规则旧，"
                    "运行: just reflect（并把 rgl/generated/compute_access.json 一并提交）"))
                continue
            key = _key_of(shader, p.get("binding"), p.get("param"))
            if key is None:
                ws.append(_fail(prod_rel, 1,
                    f"'{shader}' 有条目解析不出 binding/param（产物形态变了？本条被跳过）"))
                continue
            n_param += 1
            access = p["access"]
            if access == "unknown":
                n_unknown += 1
                ws.append(Warning(prod_rel, _product_line(prod_src, key),
                    f"compute binding：{shader} binding {key[1]} '{key[2]}' 的 access 是 "
                    "unknown（反射没分类它，或 slang 新增了 access 取值）——本规则既不当它可写、"
                    "也不当它只读，所以**不判**，但必须让你看见：方向表在这里是瞎的", "warning"))
                continue
            if access in COMPUTE_WRITABLE_ACCESS:
                product[key] = _product_line(prod_src, key)
            else:
                n_skip += 1  # read / none（CB、push constant）——实测从不与可写重合

    # ── 基线侧：收集已登记集合（条目缺字段 = 失效；登记两次 = 失效）────────────
    baseline: dict[Key, int] = {}
    for w in base_rows:
        key = _key_of(w.get("shader") if isinstance(w, dict) else None,
                      w.get("binding") if isinstance(w, dict) else None,
                      w.get("param") if isinstance(w, dict) else None)
        if key is None:
            ws.append(_fail(base_rel, 1,
                "writes 里有条目缺 shader/binding/param（基线格式变了？同步改本文件，"
                "别静默少对一条）"))
            continue
        if key in baseline:
            # 重复登记会让「删掉一条」看起来仍然绿（另一条副本还在守着）= 护栏看着在、其实瞎。
            ws.append(_fail(base_rel, _baseline_line(base_src, key),
                f"{key[0]} binding {key[1]} '{key[2]}' 登记了两次——删掉重复的那条"))
            continue
        baseline[key] = _baseline_line(base_src, key)

    # ── 双向对账 ────────────────────────────────────────────────────────────
    SCAN.update({"可写": len(product), "基线": len(baseline), "对账": n_param,
                 "跳过(读/none)": n_skip, "unknown": n_unknown})
    for key in sorted(product):
        if key in baseline:
            continue
        ws.append(Warning(prod_rel, product[key],
            f"新增可写 binding：{key[0]} binding {key[1]} '{key[2]}'（Slang 反射判它可写）"
            "——新增可写 binding：先判要不要加同步边（判据 = 这条写**之后有没有别的 dispatch "
            "读它**；只看写侧会把无读者的预留输出报成假阳性），再登记基线并写 reason 说明它是"
            "干什么的（口径 → docs/compute_调研.md §7.11）", "error"))
    for key in sorted(baseline):
        if key in product:
            continue
        ws.append(Warning(base_rel, baseline[key],
            f"陈旧基线：登记了 {key[0]} binding {key[1]} '{key[2]}'，但产物里它已不是可写"
            "（shader 改成 readonly / 挪了 binding / 删了 / 改了参数名？）——基线是裁决记录"
            "不是历史存档，删掉这条；它的同步边账若还需要，挪进对应 dispatch 条目的 reason",
            "error"))
    return ws
