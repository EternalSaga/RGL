"""阈值与名单（checks 规则参数的唯一真相源，零逻辑）。

各规则的语义与本机实测坑见对应模块 docstring：
- libclang 绑定与坑 5/7（LLVM 22 结构体布局、Indexer 与 export 可见性）→ clangbind.py
- 编译命令清理与坑 1/2/3/4b/4c（@response 展开/resource-dir/BMI 预检/竞态稳定）→ cmdclean.py
- libclang 指标规则 → rules_metrics.py；纯文本规则 → rules_text.py；Doxygen → rules_doxygen.py
- Warning/阈值/metrics TypedDict → models.py；路径遍历 → fs.py
"""

from pathlib import Path

# 本包目录与仓库根（main.py 从 tools/checks/ 以脚本方式启动，ROOT 上两级）
HERE = Path(__file__).resolve().parent
ROOT = str((HERE / ".." / "..").resolve())

# 检查排除项：生成物目录与 Vulkan 包装层不参与样式规则
EXCLUDE_SUBDIRS = ("/rgl/generated/",)
EXCLUDE_FILES = ("rgl/rlvk.cppm",)

# 缓存版本：指标结构变化时 +1（自动失效旧缓存）。
# v2→v3：函数指标 nesting → cognitive（认知复杂度替换嵌套深度）
# v3→v4：lambda 独立计分（宿主不再叠加 lambda 体；lambda 收录为独立条目）
# v4→v5：TOK_COMMENT 5→4 修正（注释 token 此前未被过滤，代码行数普遍虚高）
# v5→v6：单 parse+Indexer 管线（2026-09-23）——schema 增 decls（doc 声明表）；
#         export 包裹的类内方法/字段首次进入 functions/fields（此前游标树
#         不可见 = 样式规则盲区）；FUNC_SPELLINGS 修正（下）连带构造/析构/
#         转换函数开始被收录与正确停止嵌套计分。
# v6→v7：functions[] 增 returns_expected/throws（throw 折中版 A 消费，
#         2026-09-24 风格检查两轮扩展，见 docs/风格检查规则.md）。
CACHE_VERSION = "v7"

DEFAULT_FILE_THRESHOLD = 500
DEFAULT_FUNC_THRESHOLD = 120
DEFAULT_COGNITIVE_THRESHOLD = 15
# 认知复杂度 error 级（超此值打红粗体 ERROR，仍不阻断——仅 --strict 随警告 exit 1）
DEFAULT_COGNITIVE_ERROR_THRESHOLD = 30
DEFAULT_IMPL_THRESHOLD = 100

# ─── 工具侧（tools/）样式检查（批次 A：imgqa 纳入样式规则）──────────────────────
# 工具单元 = compile_commands 里落在 TOOL_UNIT_PREFIXES 下、后缀在 TOOL_UNIT_SUFFIXES
# 里的编译命令（imgqa 是 EXCLUDE_FROM_ALL 的 C++ 工具，configure 期仍导出编译命令，
# 故 checks 无需 imgqa 被构建就能解析它——它的命令里没有 -fmodule-file，不依赖 BMI）。
# **Doxygen 规则不纳入工具侧**：imgqa 的注释是散文式不是 Doxygen 式，套 doc 规则 =
# 265 条 d2/d4/d6 噪声（打印 helper 各缺 5 份注释），main.py 的 doc 段跳过工具单元。
TOOL_UNIT_PREFIXES = ("tools/imgqa/",)
TOOL_UNIT_SUFFIXES = (".cpp", ".h")

# 工具侧阈值：起步与 rgl 现值对齐，但**各写一份**（后续单独收紧工具侧时不牵动 rgl，
# 反之亦然）。impl 阈值对工具侧无意义（impl_slim 已关），故不列。
TOOL_FILE_THRESHOLD = 500
TOOL_FUNC_THRESHOLD = 120
TOOL_COGNITIVE_THRESHOLD = 15
TOOL_COGNITIVE_ERROR_THRESHOLD = 30

# 工具侧关闭的规则名（evaluate_metrics 的 disabled 参数消费；名字 = 规则函数名去掉
# rule_ 前缀）。impl_slim 的消息是「迁入 xxx_impl.cppm（接口 BMI 不含函数体）」——
# C++20 modules 专属语义，对纯 .cpp 工具无意义（实测套上去 = 24 条纯噪声）。
TOOL_DISABLED_RULES = frozenset({"impl_slim"})

# 工具侧样式基线（「只准降不准升」，非阻断）：{文件: 允许条数} + 便于人读的总数。
# 处置上升 = 拆函数/拆文件；下调基线 = main.py --update-tool-baseline。
TOOL_BASELINE_FILE = "tools/checks/tool_baseline.json"

# ─── libclang 游标 kind 拼写集合（clang_getCursorKindSpelling 返回串，LLVM 22 实查）──

# **v6 修正**（2026-09-23 实测 kind_spelling 值）：构造/析构/转换函数的 LLVM 22
# 拼写是 CXXConstructor/CXXDestructor/CXXConversion——旧表里 Constructor/
# Destructor/ConversionFunction 三个名从匹配不上（v5 前 ctor/dtor 函数指标
# 隐形、认知复杂度 stop 集亦漏这三类）。metrics 主判定已走 Indexer ekind，
# 本表供 cognitive_complexity 游标遍历的"独立嵌套单元"边界。
FUNC_SPELLINGS = {"FunctionDecl", "CXXMethod", "CXXConstructor", "CXXDestructor",
                  "CXXConversion"}
CTRL_SPELLINGS = {"IfStmt", "WhileStmt", "DoStmt", "ForStmt", "CXXForRangeStmt",
                  "SwitchStmt", "CXXCatchStmt", "LambdaExpr"}
# CXToken_Comment 枚举值 = 4（CXTokenKind: Punctuation=0/Keyword=1/Identifier=2/
# Literal=3/Comment=4，LLVM 22 实测 tokenize 对 //、/*、/** 均产出 kind 4——包括
# parse options=0）。**曾误写 5**：注释 token 全部漏过滤、每条注释首行被算进
# 代码行，本高注释库的行数类规则大面积虚高（2026-09-12 修正，缓存 v5 随之）。
TOK_COMMENT = 4

# 认知复杂度（SonarSource 风格，算法 = clangbind.cognitive_complexity）：
# 结构 +1+nesting 且子树递增 nesting 的一族（含三元；**不含 lambda**——
# lambda 是独立计分单元，宿主不叠加，见下 LAMBDA_SPELLING）
COGNITIVE_STRUCT_SPELLINGS = {"IfStmt", "ForStmt", "CXXForRangeStmt", "WhileStmt",
                              "DoStmt", "CXXCatchStmt", "ConditionalOperator"}
# lambda = "函数中的函数"：宿主认知复杂度不叠加其体（walk 遇 LambdaExpr 即停），
# metrics 收录为独立条目、从 lambda 自身起单独计分（nesting 0 起算）
LAMBDA_SPELLING = "LambdaExpr"
# +1 但子树 nesting 不变
SWITCH_SPELLINGS = {"SwitchStmt"}
BINARY_OPERATOR_SPELLING = "BinaryOperator"
IF_SPELLING = "IfStmt"
ELSE_TOKEN = "else"
LOGICAL_OPERATOR_TOKENS = frozenset({"&&", "||"})

# ─── 分层依赖名单（AGENTS.md"构造依赖图"：只允许下层 import 上层）───────────────

# 底层渲染层/纯配置零 entt：不得 import ecs，GMF 不得 #include <entt>
# （entt 经 GMF #include 暴露，import ecs 与 GMF include 都要拦）
ZERO_ENTT_MODULES = {
    "rlvk", "vkerror", "vulkan_utils", "features", "queue_family_manager",
    "VmaRaii", "vk_instance", "vk_device", "vk_swapchain", "vulkan_window",
    "vk_resources", "shader", "shader_registry", "state_kit", "gpu_cull",
    "vk_indirect_commands", "frame_session", "frame_capture", "cull_driver",
    "frame_draw_list", "render_graph", "render_graph_config", "diagnostics",
    "gpu_canary", "runtime_config", "camera_config", "scene_config",
    "image_codec", "profiler", "gpu_profiler", "present_timing",
}
# AGENTS.md 点名的假边（配置不依赖执行模块；截图经回调）
FORBIDDEN_EDGES = {
    "camera_config": {"llm_debug"},
    "ecs": {"llm_debug"},
}

# ─── 库分层闭合（批 12.2d；规则 = rules_layers.lib_layering_checks）──────────────
# 与 rgl/CMakeLists.txt 的五份清单**同源**：清单顺序即层级序（0 = 最底层）。
# 判据只有一条「import 边只能同层或向下」，它把「rlbase 整层无设备依赖」间接包住
# ——设备依赖（rlvk / VmaRaii / vulkan_utils）都住在 rlgfx，低层 import 不到。
# 改名/加库时这里与 CMakeLists 必须同步；不同步的后果是规则**静默失效**，
# 故 rules_layers 在读不到清单时直接报 error 而不是返回空。
LIB_LAYER_LIST_VARS = (
    "RGL_BASE_MODULE_SOURCES",
    "RGL_GFX_MODULE_SOURCES",
    "RGL_SCENE_MODULE_SOURCES",
    "RGL_FRAME_MODULE_SOURCES",
    "RGL_APP_MODULE_SOURCES",
)
LIB_LAYER_ORDER = ("rlbase", "rlgfx", "rlscene", "rlframe", "rgl")

# ─── 模块优先（rgl/ 下 .h/.hpp/.hh 全禁，.cpp 仅单头库实现单元）─────────────────
# （tinygltf/VMA 是单头 C 库，实现宏只能放普通 .cpp——既有例外，勿删；
#  入口 = main.cppm 模块实现单元（module main;，main() 合法于实现单元），
#  2026-09-14 由 .cpp 转 .cppm 起纳入 stylecheck/doccheck 全规则管理）
ALLOWED_CPP = {"tinygltf_impl.cpp", "vam_impl.cpp"}

# ─── GMF 的 STL 白名单（AGENTS.md"优先使用 rlstd"）─────────────────────────────
# STL 头只许三个例外，其余一律走 import rlstd（缺名补入 rlstd.cppm）；第三方头不受此限
STL_HEADERS = {
    "algorithm", "any", "array", "atomic", "bit", "bitset", "charconv",
    "chrono", "compare", "complex", "concepts", "condition_variable",
    "coroutine", "deque", "exception", "execution", "expected",
    "filesystem", "forward_list", "fstream", "functional", "future",
    "generator", "iomanip", "ios", "iosfwd", "iostream", "istream", "latch",
    "limits", "list", "locale", "map", "memory", "mutex", "new", "numbers",
    "numeric", "optional", "ostream", "queue", "random", "ranges", "regex",
    "set", "semaphore", "source_location", "span", "sstream", "stack",
    "stdfloat", "stdexcept", "streambuf", "string", "string_view",
    "stride", "syncstream", "thread", "tuple", "type_traits", "typeindex",
    "typeinfo", "unordered_map", "unordered_set", "variant", "vector",
    "version",
    "cassert", "ccomplex", "cctype", "cerrno", "cfloat", "cfenv",
    "cinttypes", "climits", "clocale", "cmath", "csetjmp", "csignal",
    "cstdarg", "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib",
    "cstring", "ctgmath", "ctime", "cuchar", "cwchar", "cwctype",
}
STL_GMF_EXCEPTIONS = {"ranges", "filesystem", "chrono"}

# ─── 禁用旧式 Vulkan API（纯文本回归护栏，判定 = rules_text.forbidden_vulkan_checks）──
# 项目已全面转向现代 Vulkan：descriptor heap / shader object / dynamic rendering
# （AGENTS.md）。以下经典 API 不得再引入。名单刻意精确（标识符边界匹配见 rules_text.py）：
# - 绝不匹配裸 DescriptorSet——现代在用的 VkDescriptorSetAndBindingMappingEXT
#   含子串 DescriptorSet，只匹配下列精确串（VkDescriptorSetLayout 与
#   VkDescriptorSetAndBindingMappingEXT 不同串，安全）；
# - 不加 vk::DescriptorSet 单独串（会命中现代 mapping 别名）；
# - 不加宽泛串（如裸 VkPipeline——会命中 VkPhysicalDevice*Pipeline*Features 等
#   现代 feature 结构体）。
# - 以 '*' 结尾的 token = 前缀匹配（无右边界，rules_text.py 实现）：用于结构体族与
#   sync2 变体（vkCmdBindVertexBuffers2 等），**勿给现代机制加前缀串**——
#   vkCmdBindDescriptorBuffersEXT（descriptor buffer）是 descriptor 的当代用法，
#   绝不能被 "bindDescriptorBuffers*" 之类误伤。
FORBIDDEN_VULKAN_APIS = [
    # 经典描述符模型 → VK_EXT_descriptor_heap
    "VkDescriptorPool", "vk::DescriptorPool",
    "vkCreateDescriptorPool", "vkDestroyDescriptorPool", "vkResetDescriptorPool",
    "vkAllocateDescriptorSets", "vkFreeDescriptorSets", "allocateDescriptorSet",
    "vkUpdateDescriptorSets", "updateDescriptorSet",
    "vkCmdBindDescriptorSets", "bindDescriptorSets",
    "VkDescriptorSetLayout", "vk::DescriptorSetLayout", "vkCreateDescriptorSetLayout",
    "VkWriteDescriptorSet", "VkCopyDescriptorSet",
    # 管线对象 → VK_EXT_shader_object
    "vkCreateGraphicsPipelines", "vkCreateComputePipelines",
    "vkCmdBindPipeline", "bindPipeline",
    "vkCreatePipelineLayout", "vkDestroyPipelineLayout",
    "VkPipelineLayout", "vk::PipelineLayout",
    # 经典 push constants → vkCmdPushDataEXT
    "vkCmdPushConstants", "pushConstants(",
    # render pass → VK_KHR_dynamic_rendering
    "VkRenderPass", "vk::RenderPass", "vkCreateRenderPass",
    "vkCmdBeginRenderPass", "vkCmdEndRenderPass",
    # 经典输入装配（vertex/index 绑定与输入状态）→ mesh shader 路线
    # （2026-09-24 用户确认 mesh shader 兼容性兜底不再需要，此路径封死）
    "vkCmdBindVertexBuffers*", "bindVertexBuffers*",
    "vkCmdBindIndexBuffer*", "bindIndexBuffer*",
    "VkVertexInput*", "vk::VertexInput*",
    "VkPipelineVertexInputStateCreateInfo", "vk::PipelineVertexInputStateCreateInfo",
]
FORBIDDEN_VULKAN_MSG = ("旧式 Vulkan API，已被现代替代（descriptor heap / shader "
                        "object / dynamic rendering / mesh shader；见 AGENTS.md）")

# ─── 风格检查两轮扩展（docs/风格检查规则.md，2026-09-24）──────────────────────

# C++ 头仅 Vulkan 包装层可 include（其余走 import rlvk；vulkan.h/vulkan_core.h 仍允许）
VULKAN_HPP_INCLUDE_FILES = {"rgl/rlvk.cppm"}

# docs/** 状态头：文件前 N 行内须出现状态行（`> **状态**` / 行首 `状态：` 等）
DOCS_STATUS_MAX_LINES = 10

# AGENTS.md 文档索引双向对账（rules_text.agents_doc_index_checks，§1.6）：
# 索引豁免目录——archive/ 的索引由 docs/archive/README.md 承载（AGENTS 只指到目录），
# 参考资料/ 是文献目录（其 README 自带清单，且不是"开工要路由的文档"）。
AGENTS_FILE = "AGENTS.md"
AGENTS_INDEX_EXEMPT_DIRS = ("docs/archive/", "docs/参考资料/")

# 判据契约表出处指针（rules_text.judgments_pointer_checks，§1.8）：判据表把「这条判据钉在
# 哪个文件的哪段注释」抄成字符串打印在 imgqa check 里 ⟹ 拆文件后锚点会搬空（文件还在、符号没了，
# §1.7 查不到）。glob 只圈判据表本体，不圈全部 imgqa 源码（那里的注释由 §1.7 与人工评审管）。
JUDGMENTS_TABLE_GLOB = "judgments_table*.cpp"

# 文档指针有效性（rules_text.doc_pointer_checks，§1.7）：
# 扫描面 = 下列目录（按后缀白名单）+ 下列散文件；排除目录 = 生成物 / 依赖 / 日志 / 离线文献。
DOC_POINTER_DIRS = ("docs", "rgl", "tests", "tools", "cmake", "configs", "qa",
                    "third_party", "script_spike", ".opencode")
DOC_POINTER_FILES = ("AGENTS.md", "Justfile")
DOC_POINTER_SUFFIXES = frozenset({
    ".md", ".py", ".cpp", ".cppm", ".h", ".hpp", ".slang", ".lua", ".cmake",
    ".txt", ".json", ".jsonl", ".toml", ".yaml", ".yml",
})
# tools/checks/ 不参与：规则自身与本文档必然写出被禁名（写了才拦得住别人）。
# qa/log/ 是历史调用台账（追加式日志，指针当时为真、不回改）。
DOC_POINTER_EXCLUDE_DIRS = ("tools/checks/", ".opencode/node_modules/", "qa/log/",
                            "docs/参考资料/")
# 路径形如 `<root>/….md` 的引用（rules_text 用它拼正则）——只认仓库根起的项目目录
DOC_POINTER_REF_ROOTS = ("docs", "tools", "tests", "rgl", "qa", "cmake", "configs",
                         "third_party", "script_spike", ".opencode")
# 退役文档：文件名（含后缀）→ 退役理由 + 现在的归宿。名字**再次出现**即 error——
# 删一份文档只有在全仓指针清零后才成立，而"从 git 里捞回来重建一份"是最容易复发的形态。
RETIRED_DOC_NAMES = {
    "调试历史.md": "全局调试时间线已于 2026-10-08 删除（原文 "
                   "`git log --all --first-parent -- <该文件>`）；教训就近写**当前任务文档**"
                   "（可改写：被实测推翻的旧教训直接删，只留正确做法），能落成测试/护栏的必须落成；"
                   "引用历史条目写 `git:调试历史 <日期>`",
}

# 现代 C++ 零违规回归护栏：(正则, 提示)——剥注释后按行匹配（rules_text.modern_cpp_checks）
MODERN_BANNED = [
    (r"\bgoto\b", "禁 goto——用结构化控制流"),
    (r"\bfriend\b", "禁 friend——用成员/访问器表达关系"),
    (r"\bdynamic_cast\b", "禁 dynamic_cast——项目无 RTTI/继承分层风格"),
    (r"\bconst_cast\b", "禁 const_cast——修正 const 正确性"),
    (r"\btypeid\s*\(", "禁 typeid——项目无 RTTI"),
    (r"\btypedef\b", "用 using 别名，不用 typedef"),
    (r"(?<!VK_)\bNULL\b", "用 nullptr，不用 NULL（VK_NULL_HANDLE 不受限）"),
    (r"\benum\s+(?!class)\w+", "用 enum class，不用裸 enum"),
    (r"\bmalloc\s*\(", "禁 malloc——走 RAII / VmaBuffer"),
    (r"(?<![\w.>])\bfree\s*\(", "禁 free——走 RAII / VmaBuffer"),
    (r"\bprintf\s*\(|\bputs\s*\(", "走 rllogger，不用 printf/puts"),
    (r"\bstd::cout\b", "走 rllogger，不用 std::cout"),
]

# new/delete 表达式豁免（= delete 特殊成员删除不命中；名单 = 分配器/C API 包装）
NEW_DELETE_EXEMPT = {"rgl/VmaBuffer.cppm"}

# using namespace 豁免（仅入口模块合法；见 AGENTS.md 入口约定）
USING_NAMESPACE_EXEMPT = {"rgl/main_impl.cppm"}

# export namespace 豁免（rlvk 的 vk re-export 字典；且 rlvk 在 EXCLUDE_FILES）
EXPORT_NAMESPACE_EXEMPT = {"rgl/rlvk.cppm"}
