# slangc 由 Slang 官方预构建提供：FetchContent 下载（cmake/SlangFetch.cmake，
# 根 CMakeLists 顶部已调用 rgl_fetch_slang() 设置 SLANGC_EXECUTABLE）。
# 模块分解（2026-09-23 瘦身）：
#   cmake/RglSlang.cmake         SLANG_FLAGS 契约 + compile_slang/rgl_shader
#   cmake/RglReflection.cmake    反射管线 + reflect/reflection_check
#   cmake/RglChecks.cmake        source_checks（样式 + Doxygen）
#   cmake/RglRuntimeAssets.cmake configs/models/skybox/env 拷贝
include(${CMAKE_SOURCE_DIR}/cmake/RglSlang.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglReflection.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglChecks.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglRuntimeAssets.cmake)

# ─── Shader Compilation ───────────────────────────────────────────────────────
# rgl_shader(<name> <OUT_VAR> [<DEP>...])：短名相对 shaders/，DEP = import 契约文件。
# （OUT_VAR 供调试；add_executable 经全局属性 RGL_SPV_OUTPUTS 汇总，勿依赖散落变量。）
rgl_shader(cube.mesh MESH_SPV types material scene)
rgl_shader(cube.frag FRAG_SPV types)
rgl_shader(model.mesh MODEL_MESH_SPV model material scene)
rgl_shader(model.frag MODEL_FRAG_SPV model material scene)
# Phase 5 批次 3：真简化 PBR frag（mesh 复用 model.mesh.spv，import scene.slang 光照数据）
rgl_shader(model_pbr.frag MODEL_PBR_FRAG_SPV model material scene)
# Phase 5 批次 3：基础 Phong frag（第二条光照路线，同契约同 mesh SPV，scene.lua styles 一行切换）
rgl_shader(model_phong.frag MODEL_PHONG_FRAG_SPV model material scene)
# Phase 5.5：IBL 路线（model_pbr + 环境 cubemap 采样替换常 ambient；env.slang 契约）
# Phase 8 ③a：normal 贴图导数 TBN（normalmap.slang 契约，binding 6/7 消费）
rgl_shader(model_pbr_ibl.frag MODEL_PBR_IBL_FRAG_SPV model material scene env normalmap lights)
# Phase 8 植被/光源专属 frag（forward-only；mesh 复用 model.mesh.spv，import 契约同 model_pbr_ibl）
rgl_shader(model_vegetation.frag MODEL_VEGETATION_FRAG_SPV model material scene env normalmap lights)
rgl_shader(model_light.frag MODEL_LIGHT_FRAG_SPV model material scene env normalmap lights)
# Phase 5.5 批次 4：天空盒（全屏大三角 mesh + clip→world 射线采 env cubemap frag）
rgl_shader(skybox.mesh SKYBOX_MESH_SPV sky scene)
rgl_shader(skybox.frag SKYBOX_FRAG_SPV sky scene env)
# Phase 7 D1：deferred G-buffer 几何 frag（mesh 复用 model.mesh.spv，写 4 MRT，import scene 对象矩阵）
# Phase 8 ③a：normal 贴图导数 TBN + ORM（normalmap.slang 契约，与 forward 严格同源）
rgl_shader(deferred_geometry.frag DEFERRED_GEOM_FRAG_SPV model material scene normalmap)
# Phase 8 批次 F：deferred 材质类别变体（#include 本体 + MATERIAL_CLASS 宏，class 进 emissive.a；
# DEPS 含本体——改本体必触发两变体重编译）
rgl_shader(deferred_geometry_vegetation.frag DEFERRED_GEOM_VEG_FRAG_SPV deferred_geometry.frag model material scene normalmap)
rgl_shader(deferred_geometry_light.frag DEFERRED_GEOM_LIGHT_FRAG_SPV deferred_geometry.frag model material scene normalmap)
# Phase 7 D1：light pass（全屏大三角 mesh + 采样 G-buffer 的 deferred 光照 frag）
rgl_shader(light.mesh LIGHT_MESH_SPV)
rgl_shader(light.frag LIGHT_FRAG_SPV scene lights sky)
# Phase 8 批次 ①：HDR tonemap frag（mesh 复用 light.mesh.spv；import scene 读 @56 曝光）
rgl_shader(tonemap.frag TONEMAP_FRAG_SPV scene)
# Phase 7b（批次 5）：GPU culling compute（独立契约 cull.comp.slang 头注释，无 import 契约文件）
rgl_shader(cull.comp CULL_SPV)
# Phase 10 批次 10.5：GPU id 点选 frag（mesh 复用 model.mesh.spv；写 objectIndex+1 进 r32ui）
rgl_shader(picking.frag PICKING_FRAG_SPV model material)
# Phase 8 批次 ③b：2D 纹理 compute mip 链（独立契约 mip_gen.comp.slang 头注释，无 import 契约文件；
# 传统 descriptor set + 经典 compute 管线，与 bindless 渲染路径隔离，见 mip_gen.cppm）
rgl_shader(mip_gen.comp MIP_GEN_SPV)
# Phase 8 批次 ④b：GGX 预滤波 env cubemap compute（独立契约 env_prefilter.comp.slang 头注释，
# 无 import 契约文件；descriptor heap + shader object，mip_gen 变体，见 env_prefilter.cppm）
rgl_shader(env_prefilter.comp ENV_PREFILTER_SPV)
# 静态 shader DGC spike：per-entry 序列流扇出（独立契约 kernel 头注释，docs/archive/radv_static_dgc_spike.md）
rgl_shader(stream_fanout.comp STREAM_FANOUT_SPV)

# tinygltf_impl.cpp：tinygltf C++ API 实现 TU（v3 非 header-only，见该文件头注释）。
# 曾用 -O0（-O2 下编译吃内存，24 并发时打爆 swap）；2026-09-02 用户调整内存/显存
# 分配至 32GB 后回归目标级 -O2（启动路径代码质量优先，编译峰值内存已非瓶颈）。
#
# main.cppm（主接口：启动流 helper 声明 + 轻量 struct）+ main_scene.cppm（module
# main:scene; 重函数定义）+ main_impl.cppm（module main:impl; 轻函数 + main()）
# ——2026-09-14 由 main.cpp 转来：CMake FILE_SET 不收纯实现单元，故"接口声明 + 分区
# 实现"形状（同 ecs/ecs_impl 惯例）；经 FILE_SET 编译为模块 TU 并纳入
# source_checks（tools/checks/）全规则管理。
get_property(RGL_SPV GLOBAL PROPERTY RGL_SPV_OUTPUTS)
add_executable(rgl vam_impl.cpp tinygltf_impl.cpp ${RGL_SPV})

# 模块 TU：rgl/ 根下全部 .cppm（CONFIGURE_DEPENDS：新增文件重跑 configure 自动纳入，
# 2026-09-23 瘦身定案 1B）+ 反射生成物（RglReflection 提供的显式清单）。
file(GLOB RGL_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm")
target_sources(rgl PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_MODULE_SOURCES} ${RGL_GENERATED_CPPM}
)

add_dependencies(rgl reflection_check)
# 源码检查必须等编译产物就绪（libclang 读半写 BMI 会 crash，坑 4c，
# 详见 cmake/RglChecks.cmake 头注释）。
add_dependencies(source_checks rgl)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
# tinygltf 是 header-only 且该 vcpkg port 不导出 CMake target：find_path 取头文件目录。
# SYSTEM = -isystem——第三方头内警告不进本项目 -Werror 面。
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)
target_include_directories(rgl SYSTEM PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(rgl PRIVATE Vulkan::Vulkan rllogger glm::glm EnTT::EnTT sol2::sol2 lua rlstd)
target_compile_definitions(rgl PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
# Phase 10 批 10.3 配置热重载：监视**源树** configs/（D7——build 副本会被 just build
# 内容比对静默覆盖复原，监听它无意义）；运行期路径不存在 = warn + 关（优雅降级）
target_compile_definitions(rgl PRIVATE "RGL_SOURCE_CONFIG_DIR=\"${PROJECT_SOURCE_DIR}/configs\"")
# Phase 10 批 10.4 shader 热重载（L1）：fork slangc 重编源树 shaders/ 所需的三件套
# （D11：flags = SLANG_FLAGS_STR 同源派生串，与构建期 SPV 编译 flag 恒一致；
#  路径/flags 缺失 = wireShaderHotReload 降级关并日志）
target_compile_definitions(rgl PRIVATE
	"RGL_SLANGC_PATH=\"${SLANGC_EXECUTABLE}\""
	"RGL_SLANG_FLAGS=\"${SLANG_FLAGS_STR}\""
	"RGL_SOURCE_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\"")
find_package(SDL3 CONFIG REQUIRED)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
target_link_libraries(rgl
    PRIVATE
    SDL3::SDL3
    GPUOpen::VulkanMemoryAllocator
    )

# ─── Phase 10 批 10.1：IMGUI 调试面板（imgui[core,sdl3-binding,vulkan-binding,
# docking-experimental]，features 见 vcpkg.json；头仅被 imgui_layer_impl GMF include，
# 引擎其余模块零 imgui 依赖）───
find_package(imgui CONFIG REQUIRED)
target_link_libraries(rgl PRIVATE imgui::imgui)

# ─── Phase 10b 批 11b.2：HTTP POST RPC 调试通道（cpp-httplib header-only 同步版 +
# nlohmann_json；头仅被 debug_rpc_impl 与 main_scene 的 GMF include，引擎其余模块零
# HTTP/JSON 依赖；features 见 vcpkg.json —— cpp-httplib 关 default-features 免拉 brotli）───
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(rgl PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

# ─── 图像编解码（image_codec 模块：libpng 简单 API + libjpeg(-turbo) + FreeImage .hdr）───
# vcpkg 静态库的依赖闭包经各 target 的 INTERFACE_LINK_LIBRARIES 传递（freeimage::FreeImage
# 自带完整依赖清单）；ZLIB::ZLIB 显式收尾（libpng/libjpeg 的 INTERFACE 未带 zlib，静态
# 链接顺序要求 zlib 在消费者之后）。
find_package(libpng CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(freeimage CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(rgl PRIVATE PNG::PNG libjpeg-turbo::jpeg-static freeimage::FreeImage ZLIB::ZLIB)

# NVIDIA Aftermath GPU crash dump（诊断能力，编译期开关；默认关，排查 NV 崩溃时开）
# 用法：just config 前设 RGL_AFTERMATH=ON，或 cmake -DRGL_AFTERMATH=ON
option(RGL_AFTERMATH "Enable NVIDIA Aftermath GPU crash dumps (Linux/NVIDIA only)" OFF)
if(RGL_AFTERMATH AND UNIX AND NOT APPLE)
    set(AFTERMATH_SDK_DIR "${CMAKE_SOURCE_DIR}/../Aftermath")
    target_include_directories(rgl PRIVATE "${AFTERMATH_SDK_DIR}/include")
    target_link_libraries(rgl PRIVATE "${AFTERMATH_SDK_DIR}/lib/x64/libGFSDK_Aftermath_Lib.x64.so")
    target_compile_definitions(rgl PRIVATE RGL_AFTERMATH=1)
endif()

target_compile_features(rgl PRIVATE cxx_std_23)
