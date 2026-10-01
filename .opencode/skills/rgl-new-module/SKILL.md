---
name: rgl-new-module
description: 新建或拆分 C++20 模块 (.cppm)——rlstd/GMF include 细则与完整可用面、partition implementation unit 接口瘦身的三条坑、CMake FILE_SET 登记、源码检查（source_checks）规则红线。触发词：新建模块、新 cppm、拆 impl、接口瘦身、GMF、rlstd、加 include。
---

# 新建与拆分 .cppm 模块

## 新模块 checklist

1. `.cppm`（禁 .hpp/.cpp 头文件对；rgl/ 禁 .h/.hpp/.hh，.cpp 仅单头库实现单元
   tinygltf_impl/vam_impl；入口 = main 模块三件套：main.cppm 接口 + main_impl/main_scene
   分区实现——分区实现单元与接口同登记 FILE_SET；FILE_SET 不收**无 module 声明的裸实现文件**（旧 main.cpp））；导出包裹在 `namespace RGL { ... }` 内
2. STL 走 `import rlstd;`（不在 GMF `#include` STL）；Vulkan C++ 头一律 `import rlvk;`（新用 vk:: 名报 "not visible" → 补一行 `export using` 保持字典序）；仅用 `VK_*` 宏/C 函数的文件可额外 `#include <vulkan/vulkan.h>`（core 量级 ~0.4s）
3. 函数体 >100 行 → 拆 `xxx_impl.cppm`（`module xxx:impl;`）
4. CMake 与接口同登记 FILE_SET
5. 类/枚举/函数/方法声明正上方非空 `/** */` 块（Doxygen 检查在 source_checks 内；真相源 = 接口声明处，impl 里接口已声明函数的定义不重复要求；重载组共用最上方一个块；注释挂 `template` 行上方亦可）

## rlstd / GMF 细则

- **rlstd 是全部 STL 名的唯一来源，且必须导出"完整可用面"**：除类型名外还包括 namespace 作用域自由运算符/函数（`operator==` 等重载集、`begin/end/size/swap`、`make_shared`、`getenv`、`ifstream/ios` 等）与 `__gnu_cxx::operator==/<=>`（libstdc++ 容器迭代器比较在 `__gnu_cxx`，C++20 重写机制使 `!=` 自动可用）——BMI 只序列化被声明引用的类本体、**不带自由运算符**，缺名时症状是 "invalid operands to binary expression"（2026-09-02 GMF 掏空后的批量补课，勿再回退）
- GMF 中只保留第三方库头文件（SDL、glm、fmt、sol2 等）+ 三例外：`<ranges>`（`std::views::` 是 libstdc++ namespace alias，无法 re-export）、`<filesystem>`、`<chrono>` 需保留 GMF `#include`
- 新加 STL include 时，先检查是否应补入 `rlstd.cppm` 而非直接加到 GMF
- **GMF 要收敛**：GMF 头解析 + BMI 序列化（WriteAST）是模块编译耗时大头（-ftime-trace 实测，codegen 仅 8~11%）——未用的第三方 include 一律删（2026-09-02 编译提速教训）

## 接口瘦身：partition implementation unit

重模块（函数体 >100 行）不把实现写进主接口 `.cppm`，拆 `xxx_impl.cppm`（`module xxx:impl;`，CMake 与接口同登记 FILE_SET）：**接口 = 声明 + 轻量类型 + 文档注释，实现 = 叶子单元**。收益：接口 BMI 不含函数体（importer 全部受益）、实现单元与其他工作并行不上关键路径。
已应用：llm_debug / vkrenderer / model_importer / texture_upload / camera_config / scene_config / render_list / gpu_canary / ecs / vkresources / shader_registry / render_graph / render_graph_config / gltf_loader（函数语义注释留接口声明处）。

三条坑：

1. **实现单元必须显式 `import xxx;`**——clang 不给 partition implementation unit 做隐式主接口 import（标准 [cpp.import.module] 的隐式 import GCC 有、clang 无）
2. 默认实参只写接口声明，实现定义去掉；类方法改类外定义（`static`/`[[nodiscard]]` 只留声明侧）
3. 导出实体在实现单元定义时**不得带 `export`**（声明已在接口）

小函数/模板/constexpr 留接口内联无妨。

## 检查工具规则红线（just build 编译完成后跑 source_checks = tools/checks/main.py 单入口，只警告）

- 文件 >500 代码行 → 拆文件；函数 >120 代码行、认知复杂度 >15（SonarSource 风格，**lambda 独立计分**）→ 拆函数
- 非 _impl 文件函数体 >100 行 → 迁入 xxx_impl.cppm（接口瘦身）
- 成员自然命名：无 `_`/`m_` 前缀、无 `_` 后缀
- 分层：`ZERO_ENTT_MODULES` 名单不 import ecs 且 GMF 不 include `<entt>`；点名禁止 import 边 `FORBIDDEN_EDGES`（名单见 `tools/checks/config.py`）；禁内部头 `#include "`
- GMF `#include <STL>` 白名单：只许 ranges/filesystem/chrono，余走 import rlstd
- 禁用旧式 Vulkan API：经典 descriptor pool / pipeline object / render pass / CPU 端 push constants（`vkCmdPushConstants`；shader 端 `[[vk::push_constant]]` 属性保留、CPU 端一律 `vkCmdPushDataEXT`，见 skill `rgl-shader-contract`）/ 输入装配（`bindVertexBuffers*`/`VkVertexInput*` 等，mesh shader 路线封死经典 IA；`*` = 前缀匹配 token）（项目已全面 descriptor heap / shader object / dynamic rendering / mesh shader；护栏名单 `FORBIDDEN_VULKAN_APIS`，标识符边界匹配；**勿把现代机制写成前缀串**——`vkCmdBindDescriptorBuffersEXT` 是 descriptor buffer 当代用法）
- 模块结构（2026-09-24）：partition 实现单元禁 `export` 声明、须显式 `import` 主接口、`#include` 只许 GMF、`vulkan/vulkan.hpp` 仅 `rlvk.cppm`（见 `docs/风格检查规则.md` §1）
- 现代 C++ 护栏（同日）：`goto`/`friend`/RTTI/`typedef`/裸 `NULL`/裸 `enum`/`malloc`/`printf`/`std::cout` 零违规回归；裸 `new`/`delete` 仅 `NEW_DELETE_EXEMPT`（`VmaBuffer.cppm`）；`using namespace` 仅入口、`export namespace` 仅 rlvk；返回 `std::expected` 的函数体内禁 `throw`（折中版 A，提示非构造路径用 expected）（§2）
- `docs/**.md` 前 10 行须有状态行 `> **状态**：…`（§1.5）
