---
name: rgl-new-module
description: 新建或拆分 C++20 模块 (.cppm)——rlstd/GMF include 细则与完整可用面、partition implementation unit 接口瘦身的分层原则。触发词：新建模块、新 cppm、拆 impl、接口瘦身、GMF、rlstd、加 include。
---

# 新建与拆分 .cppm 模块

## 新模块 checklist

**机器会点名的，此处不写**（`just build` 编译完成后自动跑 `source_checks`，点名到文件:行号）：
文件扩展名（`.cppm` 强制、rgl/ 禁 `.h/.hpp/.hh`、`.cpp` 白名单）· Doxygen 块 · 函数体 >100 行
该拆 impl · 分层 import 方向。FILE_SET 漏登记更硬——configure 期归属闸门直接 `FATAL_ERROR` 点名。
规则参数唯一真相源 = `tools/checks/config.py`，规则全景 = `docs/风格检查规则.md`；**本 skill
刻意不抄规则清单**（曾抄过一份，工具侧后来加了独立阈值集 + 基线棘轮，抄本整节失效）。
工具侧（`tools/imgqa/`）走独立阈值集 + 基线棘轮（`tool_baseline.json`，只准降不准升）。

只有两条机器**不知道**：

1. **入口 = main 模块三件套**：`main.cppm`（接口）+ `main_impl` / `main_scene`（分区实现）。
   FILE_SET 不收**无 module 声明的裸实现文件**，所以入口不能留裸 `.cpp`。
2. **新用 `vk::` 名报 "not visible" → 去 `rlvk.cppm` 补一行 `export using`（保持字典序）**，
   而不是在本文件 `#include` Vulkan C++ 头。只用 `VK_*` 宏 / C 函数的文件可额外
   `#include <vulkan/vulkan.h>`。

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

两条坑（实现单元漏 `import 主接口`、定义上带 `export`）**由 source_checks §1.1 / §1.2 点名**，
报错信息自带原因（"clang 不给 partition impl 隐式 import 主接口，必须显式"），此处不写。
注意这两条 **clang 自己都不拦**（后者实测 rc=0 静默通过）——兜底的是项目护栏，不是编译器。
