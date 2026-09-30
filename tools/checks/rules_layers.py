"""库分层闭合规则（批 12.2d）—— 把「rlbase 整层无设备依赖」从注释变成机器可验的断言。

## 问法（这一版的关键）

旧问法是「这个模块 import 了 Vulkan 吗」。这个问法**错**在两处：

- `import rlvk` 是**编译期类型依赖**（要 `vk::Format` 这类枚举），
  `需要一个 VkDevice 才能跑` 才是**运行期依赖**。两者不等价。
  `render_graph` / `state_kit` 都 import rlvk，却一个设备都不碰，
  它们的纯逻辑在 `just test` 里就能测。
- 它只盯 Vulkan 一个名字，抓不到「rlbase import 了一个同样不碰 Vulkan 但归属
  rlgfx 的模块」这种**层级越界**——而那才是真正会让库划分散架的病。

新问法只有一条，且完全静态可判：

    **一条 import 边，只能从高层指向同层或更低层。**

层级序（与 rgl/CMakeLists.txt 的五份清单同源，见 config.LIB_LAYER_ORDER）：

    rlbase(0) ← rlgfx(1) ← rlscene(2) ← rlframe(3) ← rgl(4)

`libcore_closure` 是这条规则在「rlbase 不得 import rlgfx 的任何成员」上的特例，
而 rlgfx 恰好拥有 rlvk / VmaRaii / vulkan_utils，所以**无需再单独点名 Vulkan**：
设备依赖是被「它住在哪一层」间接封住的。

## 为什么值得单独一条规则

四库划分落地时（批 12.2c），唯一的逆向边是 `param_pages:impl → ecs`，
它是在**编译期链接阶段**才炸的，症状是「某个符号找不到」，离真因三跳。
这条规则把它挪到 checks：一行 `文件:行号 + 逆向边 A(layer) → B(layer)`。

分区实现单元（`module X:impl;`）按主模块 `X` 的层归属；纯实现单元同样要扫，
因为上面那条逆向边正是从分区实现单元发出来的。
"""

import re

from config import LIB_LAYER_LIST_VARS, LIB_LAYER_ORDER, ROOT
from fs import rgl_cppm_files, read_text
from models import Warning
from rules_text import parse_module_file

# CMake 清单条目形如：    "${CMAKE_CURRENT_SOURCE_DIR}/camera.cppm"
_CMAKE_LIST_ENTRY = re.compile(r'^\s*"?\$\{CMAKE_CURRENT_SOURCE_DIR\}/([\w./-]+\.cppm)"?')
# 模块声明：export module X; / module X; / module X:part;
_MODULE_DECL = re.compile(r"^\s*(?:export\s+)?module\s+([\w]+)\s*(?::[\w]+)?\s*;")


def _parse_layer_of_file() -> dict[str, int]:
    """从 rgl/CMakeLists.txt 抽「源文件 → 层序号」。

    清单是**唯一真相源**（configure 期另有一道「每个 .cppm 恰好登记一次」的闸门）。
    这里刻意不反向用「GLOB 全部 cppm 减掉已登记」——那会让漏登记的文件静默落进
    最上层，正好掩盖它想守的性质。

    @return 仓库相对路径（rgl/xxx.cppm）→ 层序号（越小越底层）
    """
    cmake = read_text(f"{ROOT}/rgl/CMakeLists.txt")
    if cmake is None:
        return {}
    layer_of_file: dict[str, int] = {}
    for layer, var in enumerate(LIB_LAYER_LIST_VARS):
        # 抓 set(VAR ... ) 整块（允许内部有注释与空行）
        m = re.search(rf"set\(\s*{var}\b(.*?)\n\s*\)", cmake, re.S)
        if not m:
            continue
        for line in m.group(1).splitlines():
            entry = _CMAKE_LIST_ENTRY.match(line)
            if entry:
                layer_of_file[f"rgl/{entry.group(1)}"] = layer
    return layer_of_file


def _module_names(layer_of_file: dict[str, int]) -> dict[str, tuple[int, str]]:
    """建「模块名 → (层序号, 声明它的文件)」。分区按其主模块归层。"""
    names: dict[str, tuple[int, int]] = {}
    file_of: dict[str, str] = {}
    for rel, layer in sorted(layer_of_file.items()):
        src = read_text(f"{ROOT}/{rel}")
        if src is None:
            continue
        for line in src.splitlines():
            m = _MODULE_DECL.match(line)
            if m:
                # module X:part; → 主模块名 X（层随 X）
                names[m.group(1)] = (layer, 0)
                file_of[m.group(1)] = rel
                break
    return {k: (v[0], file_of[k]) for k, v in names.items()}


def lib_layering_checks() -> list[Warning]:
    """扫全部 rgl/*.cppm 的 import 边，报**逆向依赖**（低层 import 高层）。

    @return 每条逆向边一个 error 级 Warning（行号 = import 那一行）
    """
    layer_of_file = _parse_layer_of_file()
    if not layer_of_file:
        # 清单读不到 = 规则失效。失效必须出声，否则「零警告」会被读成「分层干净」。
        return [Warning("rgl/CMakeLists.txt", 0,
            "libcore_closure: 读不到库分层清单（LIB_LAYER_LIST_VARS 与 "
            "CMakeLists 的 set() 名不匹配？），本规则未执行", "error")]

    module_layer = _module_names(layer_of_file)
    warnings: list[Warning] = []
    for rel in rgl_cppm_files():
        src = read_text(f"{ROOT}/{rel}")
        if src is None:
            continue
        scan = parse_module_file(src)
        here = layer_of_file.get(rel)
        if here is None:
            continue  # 未登记的文件由 configure 期闸门点名，这里不重复报
        for imp, line in scan.imps.items():
            base = imp.split(":")[0]
            target = module_layer.get(base)
            if target is None:
                continue  # 外部模块（rlstd / rllogger / doctest / 第三方）不参与分层
            tlayer, tfile = target
            if tlayer > here:
                warnings.append(Warning(rel, line,
                    f"逆向依赖：{LIB_LAYER_ORDER[here]}(第 {here} 层) import "
                    f"{LIB_LAYER_ORDER[tlayer]}(第 {tlayer} 层) 的 '{base}'"
                    f"（{tfile}）。依赖只能同层或向下——把逻辑挪到职责归属正确的那一层，"
                    f"或把被依赖的部分下沉；不要为了让分层好看而扭曲逻辑归属", "error"))
    return warnings
