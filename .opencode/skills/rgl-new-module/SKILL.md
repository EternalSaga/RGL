---
name: rgl-new-module
description: 新建或拆分 C++20 模块 (.cppm)——rlstd/GMF include 细则与完整可用面、partition implementation unit 接口瘦身的三条坑、CMake FILE_SET 登记。触发词：新建模块、新 cppm、拆 impl、接口瘦身、GMF、rlstd、加 include。
---

# 新建与拆分 .cppm 模块

## 新模块 checklist

1. `.cppm`（禁 .hpp/.cpp 头文件对；rgl/ 禁 .h/.hpp/.hh，`.cpp` 仅单头库实现单元白名单；
   入口 = main 模块三件套：`main.cppm` 接口 + `main_impl` / `main_scene` 分区实现；
   FILE_SET **不收无 module 声明的裸实现文件**）；导出包裹在 `namespace RGL { ... }` 内
2. STL 走 `import rlstd;`（不在 GMF `#include` STL）；Vulkan C++ 头一律 `import rlvk;`
   （新用 `vk::` 名报 "not visible" → 补一行 `export using` 保持字典序）；只用 `VK_*` 宏 /
   C 函数的文件可额外 `#include <vulkan/vulkan.h>`
3. 函数体 >100 行 → 拆 `xxx_impl.cppm`（见下）
4. CMake 与接口**同登记 FILE_SET**（漏登记 = configure 期归属完整性闸门 `FATAL_ERROR` 点名）
5. 类/枚举/函数/方法声明正上方非空 `/** */` 块（真相源 = 接口声明处，impl 里接口已声明函数的
   定义不重复要求；重载组共用最上方一个块）

## rlstd / GMF 细则

- **rlstd 是全部 STL 名的唯一来源，且必须导出"完整可用面"**：除类型名外还包括 namespace 作用域
  自由运算符/函数（`operator==` 重载集、`begin/end/size/swap`、`make_shared`…）与
  `__gnu_cxx::operator==/<=>`（libstdc++ 容器迭代器比较在 `__gnu_cxx`）。
  **BMI 只序列化被声明引用的类本体、不带自由运算符** ⟹ 缺名时症状是
  `invalid operands to binary expression`，而不是"找不到符号"。补进 `rlstd.cppm` 而不是往 GMF 加头。
- GMF 只留第三方库头（SDL/glm/fmt/sol2…）+ 三例外 `<ranges>`（`std::views::` 是 libstdc++
  namespace alias，无法 re-export）`<filesystem>` `<chrono>`。
- **GMF 要收敛**：GMF 头解析 + BMI 序列化（WriteAST）是模块编译耗时大头（`-ftime-trace` 实测
  codegen 仅 8~11%）——未用的第三方 include 一律删。

## 接口瘦身：partition implementation unit

重模块（函数体 >100 行）不把实现写进主接口 `.cppm`，拆 `xxx_impl.cppm`（`module xxx:impl;`，
CMake 与接口同登记 FILE_SET）：**接口 = 声明 + 轻量类型 + 文档注释，实现 = 叶子单元**。
收益：接口 BMI 不含函数体（importer 全部受益）、实现单元不上关键路径。小函数/模板/constexpr
留接口内联无妨。

三条坑（每条都是编译期或链接期才炸）：

1. **实现单元必须显式 `import xxx;`**——clang 不给 partition implementation unit 做隐式主接口
   import（标准 [cpp.import.module] 的隐式 import GCC 有、clang 无）
2. 默认实参只写接口声明，实现定义去掉；类方法改类外定义（`static`/`[[nodiscard]]` 只留声明侧）
3. 导出实体在实现单元定义时**不得带 `export`**（声明已在接口）

## 样式 / Doxygen 规则：**别背清单**

`just build` 编译完成后自动跑 `source_checks`（= `tools/checks/main.py`，只警告不阻断），
违规会**点名到文件:行号**。规则参数唯一真相源 = `tools/checks/config.py`，规则全景 =
`docs/风格检查规则.md`。**本 skill 刻意不抄规则清单**——抄一份就漂一份（曾在此处抄过一份，
后来工具侧加了独立阈值集 + 基线棘轮，抄本整节失效）。

改完只跑 `just checks` 看点名结果即可；要改阈值改 `config.py`，不要在文档里另立口径。
工具侧（`tools/imgqa/`）走**独立阈值集 + 基线棘轮**（`tool_baseline.json`，只准降不准升），
rgl 侧「0 警告」这条性质单列一行报告，不被工具侧数字污染。
