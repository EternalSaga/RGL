module;
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN 是 doctest 多 TU 用法里**唯一**的实现宿主：
// 其余测试 TU 只 include 头（拿 TEST_CASE/CHECK 声明），靠静态注册对象把用例挂进
// 本 TU 的实现里。**每个 TU 都 define 会 multiple definition 炸链接**（doctest 的
// AssertData 构造 + g_cs 都是非 inline 符号）——已踩过一次，见 tests/CMakeLists.txt。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>  // doctest 实现 + main（唯一一处，GMF 收敛）

// 单元测试入口（tests/test_main.cppm）——doctest 实现与 main 的**唯一**宿主。
//
// 形状说明（与 main.cppm / main_impl.cppm 同惯例）：doctest 的实现宏放模块接口
// 单元是合法的（它只是展开成一堆全局定义），故这里既是接口单元也是入口单元。
// 若日后 doctest 与本项目模块的 ODR 冲突，拆分方式 = 本文件改为 `module
// test_main:impl;` + 另立接口单元声明（CMake FILE_SET 收实现单元，故接口单元
// 仍需存在）。
export module test_main;

import test_support;
