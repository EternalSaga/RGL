# imgqa —— 区域归因的图像判据工具箱（C++ + vcpkg，用户拍板：依赖全走 vcpkg，
# 三台机器一条命令对齐，不维护任何 Python 侧环境。本 target 随 tools/ 继承
# EXCLUDE_FROM_ALL：不进 just build 主构建，just imgqa 显式构建。）
# OpenCV 的 CMake 包名 = `OpenCV`（官方 OpenCVConfig.cmake），**不是 vcpkg port 名
# `opencv4`**——find_package(opencv4) 会报"没有 opencv4Config.cmake"（实测踩过）。
find_package(OpenCV CONFIG REQUIRED core imgproc imgcodecs)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(imgqa
    main.cpp
    # CLI 注册表（批次 B）：cli.cpp = 解析/帮助机制，cli_table.cpp = 子命令/flag/默认值表。
    # 拆两个文件是护栏逼的（tools/ 侧样式规则：单文件 < 500 代码行），缝在"表 vs 机制"上。
    cli.cpp
    cli_table.cpp
    # `diffstruct` 的 spec 段（护栏逼的另立 TU：主表 481 代码行、上限 500），缝按仪器切。
    cli_table_diffstruct.cpp
    analytic.cpp
    ao_metrics.cpp
    ao_noise.cpp
    engine.cpp
    image.cpp
    judge.cpp
    # 判据契约表（批次 C3）：按族拆多个 TU + judgments_table.h 声明。拆文件是护栏逼的
    # （表体 558 代码行 > 单文件 500；TemporalStatic 198 / TemporalMotion 269 > 单函数 120），
    # 族 = 成立条件分组，拼接顺序只在 judgments_table.cpp 的 registeredJudgments()。
    # 批 13.15b 再拆一次：`temporal-tail-*` 三段（护栏 / 机位运动 / 平移阶跃，13 条）另立
    # `judgments_table_tail.cpp`——同样是文件代码行护栏逼的（motion.cpp 加判据 ⑤ 后 505 > 500），
    # 缝按**仪器**选（同一台 `temporal-tail` 的三段同处），**判据现序一字未动**。
    judgments_table.cpp
    judgments_table_motion.cpp
    judgments_table_shadow.cpp
    judgments_table_ssr.cpp
    judgments_table_tail.cpp
    selftest.cpp
    temporal.cpp
    temporal_residual.cpp
    temporal_motion.cpp
    temporal_flicker.cpp
    temporal_homography.cpp
    temporal_yaw_remap.cpp
    material_attribution.cpp
    aa_coverage.cpp
    # 批 N6d 步 0：软影滤波轴量具（交付配置五臂 + 太阳角敏感度）。另立 TU 的理由同 aa_coverage：
    # 判据本体（度量口径 + 三条判决线的阈值理由）与 CLI/引擎生命周期不是同一条改动缝。
    shadow_softness.cpp
    # `shadow-kernel-scale`：CSM 核尺度漂移量具（程序化 penumbra_ladder 靶 + 三臂 + 世界米制
    # 半影宽度）。拆两 TU 的缝 = **口径 vs 仪器**：`shadow_kernel_metric.cpp` 是三个纯算子
    # （selftest 用闭式直接测，不启引擎），`shadow_kernel_scale.cpp` 是三臂采集 + 读数表 +
    # 三条判决线。常数唯一出处 = `shadow_kernel_metric.h`（两处各抄一份 = 第二份真相）。
    shadow_kernel_metric.cpp
    shadow_kernel_scale.cpp
    # `diffstruct`（批 15.2 子步 D1：把一次性脚本 ssr_subtract_check.py 升成常驻算子）。
    # 拆两 TU 的缝 = **口径 vs 仪器**（同 shadow_kernel_*）：`diff_struct_metric.cpp` 是纯度量
    # （selftest 用合成数组闭式直接测，不读图、不启引擎），`diff_struct.cpp` 是读图 + 报数 + 判决。
    diff_struct_metric.cpp
    diff_struct.cpp)

target_compile_features(imgqa PRIVATE cxx_std_23)
target_include_directories(imgqa PRIVATE ${OpenCV_INCLUDE_DIRS})
# 工程根 = 找 build/rgl / qa/configs / rgl 源码（键名校验扫解析器源码）的位置；
# 运行期可用 RGL_ROOT 环境变量覆盖（产物可搬运）。
target_compile_definitions(imgqa PRIVATE RGL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(imgqa PRIVATE
    # rlbase：只为一件事——时域判据要**预测**引擎的抖动再去抖，预测必须与引擎同式。
    # 此前 imgqa 手抄 Halton（`temporal.cpp` 一个 lambda、`temporal_motion.cpp` 一个函数），
    # 漂移不会报错，只会让仪器静默量到另一个东西。
    # 「imgqa 不链 rgl」从来不是决策（建项提交 ca18627 拍的是 C++/vcpkg 而非 Python），
    # 是从 `analytic.cpp`「闭式解必须是独立第二实现」那一条**泛化**出来的惯性。实测代价：
    # 二进制体积 0（21M→21M，静态库只拉被引用目标文件）、ldd 0（6 条不变）、
    # 改不相干 rlbase 模块时 imgqa 重编 0 个 TU。唯一成本是一条构建图边。
    # ⚠ 闭式解 `analytic.cpp` 仍**不得** import 引擎——那条边界有真理由，别照本行推广。
    rlbase
    ${OpenCV_LIBS}
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads)
