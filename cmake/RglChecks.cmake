# ─── 构建期源码检查（source_checks，样式 + Doxygen，只警告不阻断）─────────────
# 由 rgl/CMakeLists.txt include。单 target ALL 跑 tools/checks/main.py：
# 每个 .cppm 一次 libclang parse（Indexer 穿透 export，样式指标与 doc 声明表
# 共享同一 AST 与同一缓存 build/checks_cache），纯文本规则主进程直读源码。
#
# 竞态契约（2026-09-23，坑 4c）：libclang 读 ninja 半写的 BMI/.modmap 会 crash/
# parse NULL——检查目标必须 **add_dependencies(source_checks rgl)**（见
# rgl/CMakeLists.txt）等编译产物就绪后再跑；工具内另有产物稳定性双采样 +
# 退避重试兜底手动并行场景。失败一律"检查失败（跳过）"级输出，不阻断构建。
include_guard()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# 样式八类规则（libclang 指标 + 纯文本护栏）：
# 1) 文件代码行 >500（去注释空行，token 级精确）→ 拆文件
# 2) 函数代码行 >120 → 拆函数
# 3) 认知复杂度 >15（SonarSource 风格：if/for/while/do/catch/三元/lambda 结构
#    +1+嵌套；switch 与 else/else-if +1 不递增嵌套；一段连续 &&/|| 整段 +1；
#    lambda 与成员函数独立计分）→ 拆函数
# 4) 接口瘦身：非 _impl 文件函数体 >100 → 迁入 xxx_impl.cppm
# 5) 成员命名：_ / m_ 前缀、_ 后缀 → 自然命名
# 6) 分层/模块优先（文本）：底层零 entt（名单 ZERO_ENTT_MODULES，拦 import ecs
#    与 GMF #include <entt>）、点名禁止 import 边、rgl/ 禁 .h/.hpp/.hh、
#    .cpp 仅单头库实现单元（tinygltf_impl/vam_impl；入口 main.cppm 为模块实现单元）、
#    禁内部头 #include "（外部 SDK 头不报）
# 7) GMF STL 白名单：#include <STL> 只许 ranges/filesystem/chrono，余走 import rlstd
# 8) 禁用旧式 Vulkan API（文本，剥注释）：经典 descriptor pool / pipeline object /
#    render pass / push constants / 输入装配（vertex/index 绑定）名单
#    （config.FORBIDDEN_VULKAN_APIS）命中即警告——项目已全面 descriptor heap /
#    shader object / dynamic rendering / mesh shader（2026-09-24 封死经典 IA 路径）
# Doxygen 规则：类/枚举/函数/方法声明首行正上方须紧邻非空 /** */ 块（impl 里
# 接口已声明函数的定义处豁免——真相源 = 声明处）。
# 单独跑：just checks / just style-check / just doc-check [--strict/--only/...]。
add_custom_target(source_checks ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/checks/main.py
            --build-dir ${CMAKE_BINARY_DIR}
    COMMENT "源码检查：样式 + Doxygen（警告不阻断）")
