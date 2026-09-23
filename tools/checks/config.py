"""阈值与名单（checks 规则参数的唯一真相源，零逻辑）。

各规则的语义与本机实测坑见对应模块 docstring：
- libclang 绑定与坑 5/7（LLVM 22 结构体布局、Indexer 与 export 可见性）→ clangbind.py
- 编译命令清理与坑 1/2/3/4b/4c（@response 展开/resource-dir/BMI 预检/竞态稳定）→ cmdclean.py
- 八类样式规则 + Doxygen 规则判定 → rules.py
"""

import os
from dataclasses import dataclass

# 本包目录与仓库根（main.py 从 tools/checks/ 以脚本方式启动，ROOT 上两级）
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

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
CACHE_VERSION = "v6"

DEFAULT_FILE_THRESHOLD = 500
DEFAULT_FUNC_THRESHOLD = 120
DEFAULT_COGNITIVE_THRESHOLD = 15
DEFAULT_IMPL_THRESHOLD = 100


@dataclass
class Thresholds:
    """四条数值阈值（CLI 解析后传入 rules 的统一载体）。"""
    file: int = DEFAULT_FILE_THRESHOLD
    func: int = DEFAULT_FUNC_THRESHOLD
    cognitive: int = DEFAULT_COGNITIVE_THRESHOLD
    impl: int = DEFAULT_IMPL_THRESHOLD


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
    "image_codec",
}
# AGENTS.md 点名的假边（配置不依赖执行模块；截图经回调）
FORBIDDEN_EDGES = {
    "camera_config": {"llm_debug"},
    "ecs": {"llm_debug"},
}

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

# ─── 禁用旧式 Vulkan API（纯文本回归护栏，判定 = rules.forbidden_vulkan_checks）──
# 项目已全面转向现代 Vulkan：descriptor heap / shader object / dynamic rendering
# （AGENTS.md）。以下经典 API 不得再引入。名单刻意精确（标识符边界匹配见 rules.py）：
# - 绝不匹配裸 DescriptorSet——现代在用的 VkDescriptorSetAndBindingMappingEXT
#   含子串 DescriptorSet，只匹配下列精确串（VkDescriptorSetLayout 与
#   VkDescriptorSetAndBindingMappingEXT 不同串，安全）；
# - 不加 vk::DescriptorSet 单独串（会命中现代 mapping 别名）；
# - 不加宽泛串（如裸 VkPipeline——会命中 VkPhysicalDevice*Pipeline*Features 等
#   现代 feature 结构体）。
# - 以 '*' 结尾的 token = 前缀匹配（无右边界，rules.py 实现）：用于结构体族与
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
