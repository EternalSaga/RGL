# ─── 构建期源码检查（doc_check / style_check，只警告不阻断）──────────────────
# 由 rgl/CMakeLists.txt include；两者每次构建随 ALL 并行跑（与编译不互锁）。
include_guard()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ─── Doxygen 注释检查 ─────────────────────────────────────────────────────────
# 复用 compile_commands.json 对 rgl/ 下 .cppm 做 clang -ast-dump（同一 LLVM，纯标准库
# Python，见 tools/doccheck/doccheck.py），检查类/枚举/函数/方法声明正上方有 /** */ 块。
# AST 缓存 build/doccheck_cache（源+命令哈希键）：增量只重 dump 改动文件（秒级），
# 全量 ~1min。接口已声明函数在 impl 的定义处不重复要求（注释真相源 = 声明处）。
# 单独跑：just doc-check [--methods/--strict/...]（默认构建已含 --methods）。
add_custom_target(doc_check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/doccheck/doccheck.py
            --build-dir ${CMAKE_BINARY_DIR} --methods
    COMMENT "Doxygen 注释检查（警告不阻断）")

# ─── 文件/函数结构检查 ────────────────────────────────────────────────────────
# libclang（ctypes）进程内解析 rgl/ 下 .cppm（复用 compile_commands.json 的 .modmap BMI
# 依赖）+ 纯文本规则，见 tools/stylecheck/stylecheck.py（多模块小包，stylecheck.py 为
# CLI 入口），八类规则：
# 1) 文件代码行 >500（去注释空行，token 级精确）→ 拆文件
# 2) 函数代码行 >120 → 拆函数
# 3) 认知复杂度 >15（SonarSource 风格：if/for/while/do/catch/三元/lambda 结构
#    +1+嵌套；switch 与 else/else-if +1 不递增嵌套；一段连续 &&/|| 整段 +1）→ 拆函数
# 4) 接口瘦身：非 _impl 文件函数体 >100 → 迁入 xxx_impl.cppm
# 5) 成员命名：_ / m_ 前缀、_ 后缀 → 自然命名
# 6) 分层/模块优先（文本）：底层零 entt（名单 ZERO_ENTT_MODULES，拦 import ecs
#    与 GMF #include <entt>）、点名禁止 import 边、rgl/ 禁 .h/.hpp/.hh、
#    .cpp 仅单头库实现单元（tinygltf_impl/vam_impl；入口 main.cppm 为模块实现单元）、
#    禁内部头 #include "（外部 SDK 头不报）
# 7) GMF STL 白名单：#include <STL> 只许 ranges/filesystem/chrono，余走 import rlstd
# 8) 禁用旧式 Vulkan API（文本，剥注释）：经典 descriptor pool / pipeline object /
#    render pass / push constants 名单（config.FORBIDDEN_VULKAN_APIS）命中即警告
# ctypes 持 GIL → ProcessPoolExecutor 一进程一文件（默认 4 worker），缓存
# build/stylecheck_cache（源+命令哈希键）：增量秒级，全量 61 文件 ~12s。
# 单独跑：just style-check [--strict/--file-threshold N/...]。
add_custom_target(style_check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/stylecheck/stylecheck.py
            --build-dir ${CMAKE_BINARY_DIR}
    COMMENT "文件/函数结构检查（警告不阻断）")
