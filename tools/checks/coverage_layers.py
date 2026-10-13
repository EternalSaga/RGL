"""分层测试覆盖量具（债-15）—— 「哪些模块有 tests/ 消费者」的一张表，**只出数不裁决**。

## 这条工具为什么存在（不是"覆盖率焦虑"）

台账债-15 的原句是「`rlframe` 整层 33 模块 0% 覆盖，且没有"要不要测"的裁决」。
它要的是**决策的取证面**，不是一个新阈值：改执行器 / 帧同步 / 剔除编舞时 `just test`
全绿是**必然**的——这句话此前只是断言，本工具把它变成可复现的数。

🔴 **本工具刻意没有任何判据**：没有下限、没有棘轮、没有"该补测试"的结论。补谁、补多少、
要不要补，是用户的裁决（同 `docs/还债总账.md` 债-15 的「测量状态 = 不需测（要的是决策记录）」）。
唯一会报 error 的是**数不出来**：清单解析不到 / 某层空了 / 扫描面为空 / 有文件读不进去。
「没数」不是「数是 0」——这条纪律与 `rules_layers` / `rules_reflection` 同源。

## 归属面（模块住在哪一层）

真相源 = `rgl/CMakeLists.txt` 的**五份清单**，解析直接复用 `rules_layers`
（`parse_layer_of_file` / `module_names`）——**另起一套解析就是两个真相源**，本仓最忌讳。
因此这里的分层与 `libcore_closure` 那条分层规则**必然同口径**，改名一起漂、一起喊。

- **模块 ≠ 文件**：`vkrenderer.cppm` / `_impl` / `_passrec` / `_offgraph` 是**一个**模块
  `vkrenderer`（分区实现单元按主模块归层）。台账那句「33 模块」是**清单条目**口径
  （2026-10-14 实测该层 37 条 / 16 个模块），两个数都印，别拿它们互相印证。
- 刻意不按磁盘 `.cppm` 归层：漏登记的文件由 configure 期闸门点名，这里不重复报、也不替它猜层。

## 消费者面（口径定义 —— 本文件的判据真相源就在这里）

`tests/` 链的是全部四层库（`tests/CMakeLists.txt` 的决策注释：「`import rlvk` 是**编译期**
依赖 ≠ 需要一个 VkDevice 是**运行期**依赖」）。⟹ **链了四层库 ≠ 覆盖了四层**，链接面对
"谁被测"零信息量。真正的信号只有一个：**某个测试 TU 直接 `import` 了哪个模块名**。

| 口径 | 定义 | 它诚实在哪 |
|---|---|---|
| `direct` | 模块被 ≥1 个 `tests/*.cppm` TU **直接 import**（分区按主模块） | 只认 import 边，不认链接面、不认传递闭包 |
| `offline` | 上者中，被「**含非 gpu 套件 `TEST_CASE`** 的测试 TU」直接 import | `just test` 默认子集（`--test-suite-exclude=gpu`）**真的会跑到**它 |

测试 TU 按用例分三类（决定它贡献哪些口径）：

- `offline`：有 ≥1 个不在 `TEST_SUITE("gpu")` 里的 `TEST_CASE` ⟹ 贡献 direct + offline；
- `gpu`：只有 gpu 套件用例 ⟹ 只贡献 direct（要真设备 + VVL 才有答案，毫秒级回归里没有它）；
- `fixture`：零 `TEST_CASE`（如 `test_device_primitives_impl.cppm`）⟹ 它的 import 是**夹具管线**，
  只贡献 direct。

🔴 **两条已知低估**（口径的边界，不是 bug，读表的人必须知道）：

1. **只认直接 import**：测试经 `render_list` 用到 `frame_draw_list` 的类型，不给
   `frame_draw_list` 记功。传递闭包会把「链了」重新读成「测了」——那正是本工具要防的误读。
2. **只认模块名，不认符号**：`import rlvk;` 拿的是 `vk::Format` 这类**类型依赖**（就是
   `rules_layers` §5.1 警告过的那个混淆），它算 direct，但"rlvk 被测了"这句话没有意义。
   逐模块清单把消费者 TU 名字一并印出，这类条目肉眼可辨——**不做白名单过滤**，
   过滤就是替用户裁决。

## 挂点（为什么是「checks 里一行读数 + just coverage 出全表」）

见 `docs/风格检查规则.md` §8 与本工具的 `just coverage` 配方注释。一句话：全表每次构建都印
是噪声（它不是判据、且几乎不漂），但**完全没人跑的工具会静默烂掉**（债-10 的同一种病），
所以 `just checks` 里留一行读数 + 失效 error，全表按需出。
"""

import json
import re
import sys
from dataclasses import dataclass, field

from config import COVERAGE_GPU_SUITE, COVERAGE_TEST_DIRS, LIB_LAYER_ORDER, ROOT
from fs import read_text, tests_cppm_files
from models import Warning
from rules_layers import module_names, parse_layer_of_file
from rules_text import parse_module_file, strip_comments

# doctest 的用例宏与套件块：先 strip_comments，再按花括号深度定位 TEST_CASE 落在哪个
# 套件里（注释里出现的 TEST_SUITE("gpu") 不算，见 _suite_case_counts）。
_TEST_TOKEN_RE = re.compile(r'TEST_CASE|TEST_SUITE\s*\(\s*"([^"]*)"\s*\)|[{}]')


# ─── 采集 ─────────────────────────────────────────────────────────────────────

@dataclass(slots=True)
class TestTU:
    """一个测试编译单元：它 import 了什么 + 它的用例属于哪个套件。"""

    rel: str
    imports: dict[str, int] = field(default_factory=dict)
    cases_offline: int = 0     # 不在 gpu 套件里的 TEST_CASE 数
    cases_gpu: int = 0

    @property
    def kind(self) -> str:
        """offline = 默认子集会跑它；gpu = 只有设备套件；fixture = 零用例的夹具。"""
        if self.cases_offline:
            return "offline"
        return "gpu" if self.cases_gpu else "fixture"


@dataclass(slots=True)
class Module:
    """一个引擎模块（分区已并入主模块）及其测试消费者。"""

    name: str
    layer: int
    files: list[str] = field(default_factory=list)
    consumers: dict[str, str] = field(default_factory=dict)   # tests TU → offline|gpu|fixture

    @property
    def direct(self) -> bool:
        return bool(self.consumers)

    @property
    def offline(self) -> bool:
        return any(k == "offline" for k in self.consumers.values())

    @property
    def state(self) -> str:
        """逐模块清单的标记（**只描述，不裁决**；ASCII 是为了表格对齐与 --json 好读）：

        none = 没有任何测试 TU 直接 import · fixture-only = 只被零用例的夹具 TU import ·
        gpu-only = 只被「全部用例都在 gpu 套件」的 TU import · offline = 默认子集跑得到。
        """
        if not self.consumers:
            return "none"
        kinds = set(self.consumers.values())
        if "offline" in kinds:
            return "offline"
        return "gpu-only" if "gpu" in kinds else "fixture-only"


@dataclass(slots=True)
class Report:
    """一次采集的全部事实（渲染与 error 判定都从它出，不重算）。"""

    modules: list[Module] = field(default_factory=list)
    tus: list[TestTU] = field(default_factory=list)
    tu_unreadable: list[str] = field(default_factory=list)
    # 不在五份清单里的 import 名 → 命中它的 TU 数（rlstd / 测试自身模块 / 第三方）
    unmatched: dict[str, int] = field(default_factory=dict)
    files_per_layer: dict[int, int] = field(default_factory=dict)

    def layer_rows(self) -> list[tuple[str, int, int, int, int]]:
        """按层汇总：(层名, 模块数, 清单条目数, direct, offline)。"""
        rows = []
        for i, name in enumerate(LIB_LAYER_ORDER):
            ms = [m for m in self.modules if m.layer == i]
            rows.append((name, len(ms), self.files_per_layer.get(i, 0),
                         sum(1 for m in ms if m.direct), sum(1 for m in ms if m.offline)))
        return rows


def _suite_case_counts(src: str) -> tuple[int, int]:
    """数 (非 gpu 套件 TEST_CASE, gpu 套件 TEST_CASE)。

    为什么不能"文件里出现过 TEST_SUITE(\"gpu\") 就算设备用例"：实测 tests/ 有 4 个文件
    **只在头注释里**提到它（`test_mdi_rows` / `test_meshlet_overlay` / `test_shader_mapping`
    / `test_meshlet_shadow_args`），其中前三个是纯 CPU 判据、`just test` 天天在跑。
    所以按花括号深度定位 TEST_CASE 落在哪个套件块里（`strip_comments` 先剥注释、行号不漂）。
    """
    code = strip_comments(src)
    depth = 0
    suite_at: dict[int, str] = {}     # 花括号深度 → 该层套件名
    pending: str | None = None        # 已见 TEST_SUITE("x") 但还没吃到 '{'
    n_gpu = n_other = 0
    for m in _TEST_TOKEN_RE.finditer(code):
        tok = m.group(0)
        if tok.startswith("TEST_SUITE"):
            pending = m.group(1)
            continue
        if tok == "{":
            depth += 1
            if pending is not None:
                suite_at[depth] = pending
                pending = None
            continue
        if tok == "}":
            suite_at.pop(depth, None)
            depth -= 1
            continue
        # TEST_CASE：所在套件 = 当前深度往上最近的已登记套件
        enclosing = [suite_at[d] for d in suite_at if d <= depth]
        if enclosing and all(s == COVERAGE_GPU_SUITE for s in enclosing):
            n_gpu += 1
        else:
            n_other += 1
    return n_other, n_gpu


def collect() -> Report:
    """采集归属面（五份清单）× 消费者面（tests/ import 边）。**不判对错**。"""
    rep = Report()
    layer_of_file = parse_layer_of_file()
    names = module_names(layer_of_file)

    by_name: dict[str, Module] = {}
    for rel, layer in sorted(layer_of_file.items()):
        rep.files_per_layer[layer] = rep.files_per_layer.get(layer, 0) + 1
    for name, (layer, _decl_file) in sorted(names.items()):
        mod = Module(name=name, layer=layer)
        by_name[name] = mod
        rep.modules.append(mod)
    for rel, layer in sorted(layer_of_file.items()):
        # 该层的哪个文件声明了哪个主模块（分区实现单元与主接口都算它的"文件"）
        src = read_text(f"{ROOT}/{rel}")
        if src is None:
            continue
        scan = parse_module_file(src)
        base = (scan.name or "").split(":")[0]
        mod = by_name.get(base)
        if mod is not None:
            mod.files.append(rel)

    # 消费者面：tests/*.cppm 的 import 边 + 用例所在套件
    ok, bad = tests_cppm_files(COVERAGE_TEST_DIRS)
    rep.tu_unreadable = bad
    own = set()          # tests/ 自己声明的模块名（不算引擎消费者）
    for rel in ok:
        src = read_text(f"{ROOT}/{rel}")
        if src is None:
            continue
        scan = parse_module_file(src)
        if scan.name:
            own.add(scan.name.split(":")[0])
        n_off, n_gpu = _suite_case_counts(src)
        tu = TestTU(rel=rel, imports=dict(scan.imps), cases_offline=n_off, cases_gpu=n_gpu)
        rep.tus.append(tu)
        for imp in tu.imports:
            base = imp.split(":")[0]
            mod = by_name.get(base)
            if mod is None:
                rep.unmatched[base] = rep.unmatched.get(base, 0) + 1
                continue
            mod.consumers[rel] = tu.kind
    rep.unmatched = {k: v for k, v in rep.unmatched.items() if k not in own}
    return rep


# ─── checks 侧：失效必须出声 + 每次构建一行读数 ─────────────────────────────────

_LAST: Report | None = None


def _fail(msg: str) -> Warning:
    """本工具失效 = **error 级**（同 rules_layers / rules_reflection：失效不出声时，
    「零警告」会被读成「覆盖面无缺口」；`--strict` 必须为它变红）。"""
    return Warning("tools/checks/coverage_layers.py", 0, f"分层测试覆盖失效：{msg}", "error")


def coverage_checks() -> list[Warning]:
    """只报「数不出来」，不报覆盖率本身（本工具没有阈值，见模块 docstring）。"""
    global _LAST
    rep = collect()
    _LAST = rep

    errs: list[Warning] = []
    if not rep.modules:
        errs.append(_fail("五份库清单里一个模块都没解析出来（LIB_LAYER_LIST_VARS 与 "
                          "rgl/CMakeLists.txt 的 set() 名不匹配？），本工具未执行"
                          "——**没数不是 0%**"))
    if not rep.tus:
        errs.append(_fail(f"消费者扫描面为空（COVERAGE_TEST_DIRS={COVERAGE_TEST_DIRS} 下没有"
                          "可读的 .cppm），本工具未执行——**没数不是 0%**"))
    # 某层空了 = 那份清单改名/被删；表里它会显示成「0 模块」，读起来像"这层没东西"而不是"漏了它"
    for i, name in enumerate(LIB_LAYER_ORDER):
        if rep.modules and not any(m.layer == i for m in rep.modules):
            errs.append(_fail(f"第 {i} 层（{name}）在清单里解析到 0 个模块——清单被改名/删除？"
                              "宁可喊错，不许静默少一层"))
    if rep.tu_unreadable:
        errs.append(_fail("这些测试文件读不进去，覆盖率会**偏低且看起来像真数**："
                          + "、".join(rep.tu_unreadable[:5])))
    return errs


def coverage_readout() -> str:
    """`just checks` 每次构建印的那一行（同 rules_reflection 的读数纪律）。

    🔴 名字刻意不叫 `scan_readout`：main.py 同时 import 两个模块的同名读数函数时，
    后 import 的那个会**静默顶掉**前一个（实测踩过：覆盖读数打印成了反射对账读数，
    两行一模一样却没人看得出）。
    """
    if _LAST is None:
        return ""
    rep = _LAST
    if not rep.modules or not rep.tus:
        # 扫描面塌了的时候不许印 0/0——0/0 会被读成"这层没东西"，而真因是"没数"
        return "**扫描面为空**（模块或测试 TU 一个都没解析出来，见上面的 ERROR）"
    parts = []
    for name, mods, _files, direct, offline in rep.layer_rows():
        parts.append(f"{name} {direct}/{mods}·{offline}")
    return (f"{' / '.join(parts)}（direct/模块数·offline）｜测试 TU {len(rep.tus)}"
            f"｜清单条目 {sum(rep.files_per_layer.values())}"
            f"｜未归属 import 名 {len(rep.unmatched)}")


# ─── 全表渲染（just coverage）──────────────────────────────────────────────────

# 表头用 ASCII：本仓注释与输出大量中文，中文是双宽字符，混进等宽表格必然错位
_COLS = (("layer", 10), ("modules", 8), ("entries", 8), ("direct", 8), ("offline", 9),
         ("direct%", 9), ("offline%", 10))


def _row(vals: list[object]) -> str:
    return "".join(f"{str(v):<{w}}" if i == 0 else f"{str(v):>{w}}"
                   for i, (v, (_name, w)) in enumerate(zip(vals, _COLS)))


def _pct(n: int, d: int) -> str:
    return "—" if not d else f"{100.0 * n / d:.1f}%"


def render_table(rep: Report) -> str:
    out: list[str] = []
    w = out.append
    w("分层测试覆盖量具（债-15）｜归属真相源 = rgl/CMakeLists.txt 五份清单｜"
      "消费者 = tests/*.cppm 直接 import")
    w("口径 direct  = 被 ≥1 个测试 TU 直接 import（分区按主模块；不认链接面、不认传递闭包）")
    w("     offline = 其中被「含非 gpu 套件 TEST_CASE 的测试 TU」直接 import ⟹ just test 真跑到")
    w("     entries = 清单条目数（= 台账说的「33 模块」那种数；分区实现单元各算一条）")
    w("     两条已知低估/高估见 tools/checks/coverage_layers.py 模块 docstring（本表只出数不裁决）")
    w("")
    w(_row([c[0] for c in _COLS]))
    tot = [0, 0, 0, 0]
    for name, mods, files, direct, offline in rep.layer_rows():
        w(_row([name, mods, files, direct, offline, _pct(direct, mods), _pct(offline, mods)]))
        tot = [tot[0] + mods, tot[1] + files, tot[2] + direct, tot[3] + offline]
    w(_row(["total", tot[0], tot[1], tot[2], tot[3],
            _pct(tot[2], tot[0]), _pct(tot[3], tot[0])]))
    w("")
    w(f"测试 TU {len(rep.tus)} 个（读失败 {len(rep.tu_unreadable)}）："
      f"offline {sum(1 for t in rep.tus if t.kind == 'offline')} / "
      f"gpu {sum(1 for t in rep.tus if t.kind == 'gpu')} / "
      f"fixture {sum(1 for t in rep.tus if t.kind == 'fixture')}")
    if rep.unmatched:
        w("未归属 import 名（不在五份清单里：rlstd / rllogger / 测试自身模块 / 第三方）："
          + "、".join(f"{k}×{v}" for k, v in sorted(rep.unmatched.items(),
                                                    key=lambda kv: (-kv[1], kv[0]))))
    w("")
    w("── 逐模块清单（state：none 无消费者 / fixture-only 只被夹具 import / "
      "gpu-only 只被设备套件 TU import / offline 默认子集跑得到）" + "─" * 12)
    for i, layer in enumerate(LIB_LAYER_ORDER):
        ms = [m for m in rep.modules if m.layer == i]
        if not ms:
            continue
        w(f"\n[{layer}]  direct {sum(1 for m in ms if m.direct)}/{len(ms)}，"
          f"offline {sum(1 for m in ms if m.offline)}/{len(ms)}")
        for m in sorted(ms, key=lambda x: x.name):
            w(f"  {m.state:<13}{m.name:<36}{len(m.files)} 文件  "
              + (f"消费者 {len(m.consumers)}：" + ", ".join(sorted(m.consumers)[:3])
                 + ("…" if len(m.consumers) > 3 else "") if m.consumers else "—"))
    return "\n".join(out)


def as_dict(rep: Report) -> dict:
    """机器可读形态（裁决时自己排序/切片用；字段含义 = 模块 docstring 的口径表）。"""
    return {
        "口径": {"direct": "被 ≥1 个 tests TU 直接 import",
                 "offline": "被含非 gpu 套件 TEST_CASE 的 tests TU 直接 import"},
        "layers": [{"layer": n, "modules": m, "list_entries": f, "direct": d, "offline": o,
                    "direct_pct": round(100.0 * d / m, 1) if m else None,
                    "offline_pct": round(100.0 * o / m, 1) if m else None}
                   for n, m, f, d, o in rep.layer_rows()],
        "modules": [{"name": m.name, "layer": LIB_LAYER_ORDER[m.layer], "files": m.files,
                     "state": m.state, "consumers": dict(sorted(m.consumers.items()))}
                    for m in sorted(rep.modules, key=lambda x: (x.layer, x.name))],
        "test_tus": [{"file": t.rel, "kind": t.kind, "cases_offline": t.cases_offline,
                      "cases_gpu": t.cases_gpu, "imports": sorted(t.imports)}
                     for t in rep.tus],
        "unmatched_imports": dict(sorted(rep.unmatched.items())),
    }


def main(argv: list[str]) -> int:
    """`just coverage`：默认出全表；`--json` 出机器可读形态（只出数，不裁决）。"""
    rep = collect()
    if not rep.modules or not rep.tus:
        print("分层测试覆盖：**扫描面为空**（五份清单或 tests/ 没解析出东西）——"
              "没数不是 0%，先修扫描面", file=sys.stderr)
        return 2
    if "--json" in argv:
        print(json.dumps(as_dict(rep), ensure_ascii=False, indent=1))
        return 0
    print(render_table(rep))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
