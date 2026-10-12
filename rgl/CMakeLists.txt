# slangc 由 Slang 官方预构建提供：FetchContent 下载（cmake/SlangFetch.cmake，
# 根 CMakeLists 顶部已调用 rgl_fetch_slang() 设置 SLANGC_EXECUTABLE）。
# 模块分解（2026-09-23 瘦身）：
#   cmake/RglSlang.cmake         SLANG_FLAGS 契约 + compile_slang/rgl_shader
#   cmake/RglReflection.cmake    反射管线 + reflect/reflection_check
#   cmake/RglChecks.cmake        source_checks（样式 + Doxygen）+ shader_dep_check（DEP 表）
#   cmake/RglRuntimeAssets.cmake configs/models/skybox/env 拷贝
#   cmake/RglMeshlets.cmake    簇产物构建期生成（★0-b2，meshlet_pack 清单内模型）
include(${CMAKE_SOURCE_DIR}/cmake/RglSlang.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglReflection.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglChecks.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglRuntimeAssets.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/RglMeshlets.cmake)

# ─── Shader Compilation ───────────────────────────────────────────────────────
# rgl_shader(<name> <OUT_VAR> [<DEP>...])：短名相对 shaders/，DEP = import 契约文件。
# （OUT_VAR 供调试；add_executable 经全局属性 RGL_SPV_OUTPUTS 汇总，勿依赖散落变量。）
rgl_shader(cube.mesh MESH_SPV types material scene)
rgl_shader(cube.frag FRAG_SPV types)
rgl_shader(model.mesh MODEL_MESH_SPV model material scene)
# 簇化轴 ★1-a1ii-α：读簇产物的 mesh shader（影子叠加）。MESHLET_DEBUG_OFFSET 变体 =
# 阳性对照（判据 3）：门 = 选 SPV 变体不是运行时 flag（2026-10-02 slang fp 重排教训，
# 见 RglSlang.cmake 门控变体注释）。
rgl_shader(meshlet.mesh MESHLET_MESH_SPV model material scene)
rgl_shader_defined(meshlet.mesh MESHLET_MESH_DEBUG_SPV MESHLET_DEBUG_OFFSET model material scene)
rgl_shader(model.frag MODEL_FRAG_SPV model material scene)
# Phase 5 批次 3：真简化 PBR frag（mesh 复用 model.mesh.spv，import scene.slang 光照数据）
rgl_shader(model_pbr.frag MODEL_PBR_FRAG_SPV model material scene)
# Phase 5 批次 3：基础 Phong frag（第二条光照路线，同契约同 mesh SPV，scene.lua styles 一行切换）
rgl_shader(model_phong.frag MODEL_PHONG_FRAG_SPV model material scene)
# Phase 5.5：IBL 路线（model_pbr + 环境 cubemap 采样替换常 ambient；env.slang 契约）
# Phase 8 ③a：normal 贴图导数 TBN（normalmap.slang 契约，binding 6/7 消费）
rgl_shader(model_pbr_ibl.frag MODEL_PBR_IBL_FRAG_SPV model material scene env normalmap lights shadow)
# Phase 8 植被/光源专属 frag（forward-only；mesh 复用 model.mesh.spv，import 契约同 model_pbr_ibl）
rgl_shader(model_vegetation.frag MODEL_VEGETATION_FRAG_SPV model material scene env normalmap lights shadow)
rgl_shader(model_light.frag MODEL_LIGHT_FRAG_SPV model material scene env normalmap lights shadow)
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
rgl_shader(light.frag LIGHT_FRAG_SPV scene lights sky shadow)
# 批 11.3-1e：IZB 消费变体（-DIZB_CONSUME；门关选原版 = 逐位红线构造性保证，见 RglSlang.cmake）
rgl_shader_defined(light.frag LIGHT_IZB_FRAG_SPV IZB_CONSUME scene lights sky shadow)
# 批 11.5：PCSS 接触硬化软阴影变体（-DPCSS_SOFT；阴影采样 frag 恰 4 个文件——grep "import shadow"。
# 门关选原版 = 逐位红线构造性保证；IZB×PCSS 组合 = 双 define（registry 拼接顺序 = 此处顺序））
rgl_shader_defined(model_pbr_ibl.frag MODEL_PBR_IBL_PCSS_FRAG_SPV PCSS_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(model_vegetation.frag MODEL_VEGETATION_PCSS_FRAG_SPV PCSS_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(model_light.frag MODEL_LIGHT_PCSS_FRAG_SPV PCSS_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(light.frag LIGHT_PCSS_FRAG_SPV PCSS_SOFT scene lights sky shadow)
rgl_shader_defined2(light.frag LIGHT_IZB_PCSS_FRAG_SPV IZB_CONSUME PCSS_SOFT scene lights sky shadow)
# 批 11.6d-2：EVSSM 远场软阴影变体（-DEVSSM_SOFT；同 4 个阴影采样 frag + IZB 组合，
# 门控模式同 11.5。两门同开 = shadow.slang 内 EVSSM 优先分支）
rgl_shader_defined(model_pbr_ibl.frag MODEL_PBR_IBL_EVSSM_FRAG_SPV EVSSM_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(model_vegetation.frag MODEL_VEGETATION_EVSSM_FRAG_SPV EVSSM_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(model_light.frag MODEL_LIGHT_EVSSM_FRAG_SPV EVSSM_SOFT model material scene env normalmap lights shadow)
rgl_shader_defined(light.frag LIGHT_EVSSM_FRAG_SPV EVSSM_SOFT scene lights sky shadow)
rgl_shader_defined2(light.frag LIGHT_IZB_EVSSM_FRAG_SPV IZB_CONSUME EVSSM_SOFT scene lights sky shadow)
# Phase 11 批 11.2：级联阴影 depth-only 管线。mesh 自带 8B push（objectIndex +
# cascadeIndex）——**不 import material.slang**：共享的 4B DrawArgs 契约不能为阴影多加一个
# 字段（改了 = 1591 条模型条目全重建），故阴影管线自带 push 块。frag 空壳（无输出）。
# shadow.slang（采样契约，被 forward/deferred 两条光照路 import）无新结构体 → 不需单独反射管线。
rgl_shader(shadow.mesh SHADOW_MESH_SPV model scene)
rgl_shader(shadow.frag SHADOW_FRAG_SPV)
# Phase 8 批次 ①：HDR tonemap frag（mesh 复用 light.mesh.spv；import scene 读 @56 曝光）
rgl_shader(tonemap.frag TONEMAP_FRAG_SPV scene)
# Phase 11 批 11.4：SSAO frag（mesh 复用 light.mesh.spv 全屏大三角；import scene 读
# @12 eyePos + @64 invViewProj 做深度↔世界坐标重建；binding 4 深度图 / 5 point sampler）
rgl_shader(ao.frag AO_FRAG_SPV scene)
rgl_shader(velocity.frag VELOCITY_FRAG_SPV scene)   # 批 14.4：逐像素速度场（全屏 pass，消费 scene data 既有槽位）
# 批 12.6 TAA：时域滤波 frag（mesh 复用 light.mesh.spv；import scene 读 @64 invViewProj
# + TAA 尾区 @722080；binding 4 本帧色 / 5 历史 / 6 深度 / 7 线性 sampler / 8 point sampler）
# 批 14.5 第三批：AO 施加专用全屏 pass（把「AO 乘在时域滤波之前」从 taa.frag 里抽出来，
# 与「谁做时域」解耦；mesh 复用 light.mesh.spv，binding 4 色 / 5 共享 sampler / 6 ao 可见度）
rgl_shader(ao_apply.frag AO_APPLY_FRAG_SPV scene)
# 批 15.3 子步 B：SSR 拆两 pass（march 本体半分辨率写 premultiplied ssr_trace + 全分辨率
# apply 做粗糙度感知螺旋模糊后加法；15.1 的单 pass ssr.frag 自此退役，见两文件头注释）
rgl_shader(ssr_trace.frag SSR_TRACE_FRAG_SPV scene)
rgl_shader(ssr_apply.frag SSR_APPLY_FRAG_SPV scene)
rgl_shader(taa.frag TAA_FRAG_SPV scene)
# Phase 7b（批次 5）：GPU culling compute（独立契约 cull.comp.slang 头注释，无 import 契约文件）
rgl_shader(cull.comp CULL_SPV)
# 批 11.3-1d：FTIZB IZB 创建 + priming（独立契约，头注释即布局真相；heap + shader object，
# 消费侧 = vk_renderer 门控 sandwich，docs/hfts_izb_plan.md）
rgl_shader(izb_create.comp IZB_CREATE_SPV)
rgl_shader(izb_prime.comp IZB_PRIME_SPV)
# 批 11.3-1d 后半：光空间保守光栅 mask pass（mesh 形态 = shadow.mesh 镜像 + 平摊三 uv）
rgl_shader(izb_mask.mesh IZB_MASK_MESH_SPV model scene)
rgl_shader(izb_mask.frag IZB_MASK_FRAG_SPV)
# 批 11.6d-1：EVSSM 矩派生链（图外 compute，C 方案；docs/evssm_plan.md §4.1）
rgl_shader(evssm_moments.comp EVSSM_MOMENTS_SPV)
rgl_shader(evssm_downsample.comp EVSSM_DOWN_SPV)
# Phase 10 批次 10.5：GPU id 点选 frag（mesh 复用 model.mesh.spv；写 objectIndex+1 进 r32ui）
rgl_shader(picking.frag PICKING_FRAG_SPV model material)
# Phase 10 批次 10.7：点选高亮 frag（mesh 复用 model.mesh.spv；菲涅尔边缘发光 alpha 混合
# 叠加 scene_hdr；选中范围按 materialId 在 CPU 侧过滤 draw list，本 frag 不读选中标志）
rgl_shader(highlight.frag HIGHLIGHT_FRAG_SPV model material scene)
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
# main:scene; 重函数定义）+ main_wiring.cppm（module main:wiring; 诊断/热重载挂点装配）
# + main_impl.cppm（module main:impl; 轻函数 + main()）
# ——2026-09-14 由 main.cpp 转来：CMake FILE_SET 不收**无 module 声明的裸实现文件**
#（旧 main.cpp），故"接口声明 + 分区实现"形状（同 ecs/ecs_impl 惯例；分区实现单元
# main_impl/main_scene 本身与接口同登记 FILE_SET）；经 FILE_SET 编译为模块 TU 并纳入
# source_checks（tools/checks/）全规则管理。
get_property(RGL_SPV GLOBAL PROPERTY RGL_SPV_OUTPUTS)

# ═══════════════════════════════════════════════════════════════════════════
# 引擎四库分层（2026-09-30 批 12.2c）—— rlbase / rlgfx / rlscene / rlframe
# ═══════════════════════════════════════════════════════════════════════════
#
# ## 为什么抽库
#
# 起因是自动化测试（tests/ + qa/）：测试要 import 引擎模块，而 `rgl` 是**可执行
# 文件**——它的 CXX_MODULES FILE_SET 只有它自己能 import，测试无法复用。模块必须
# 住进库。
#
# ## 为什么按功能分四层，而不是一个内核库
#
# 批 12.1 先做的是「rlcore = import 闭包不含 Vulkan 的模块」，靠一张 73 项排除清单
# + 逐项理由维护。实测那条判据**把两件事混成了一条**：
#   · 它禁掉的是「import Vulkan」这个**动作**
#   · 它想守的是「能不能不建设备就测」这个**能力**
# 两者不等价，于是产生大量连坐：`frame_draw_list` / `render_list` / `render_graph` /
# `scene_data` 各自只因为借了一个资源句柄就把整个 Vulkan 栈拖进来（源码注释自称
# 「架构债务续账」）。逐条算过「切断这条边后闭包还含不含 Vulkan 根」：**13 条里 12 条
# 是徒劳的**（vk_resources 是枢纽、VmaRaii 在它后面），只有 param_pages→asset_table
# 一条切了会变干净 —— 而那也不必切断，把 param_pages 挪到它逻辑所属的层即可。
#
# 所以顺序反过来：**库边界照现有的真实结构（资源枢纽）划，不追求「更纯」**。该不该
# 上设备测试由它实际需不需要设备决定，不由边界好不好看决定。
#
# ## 划分依据（机器验证，非手拍）
#
# 从源码正则直抽完整 import 图（110 模块 / 790 条边），跑「归属完整性 + 库间
# 逆向边 + GPU 需求」三查。⚠️ **不要**用 modern_vulkan_tutorial/tools/explore 的
# model.json 做这件事：实测它漏 326 条 import 边（294 条来自 `module X:impl;`
# 分区实现单元，占 90%），用它验证出来的「零逆向边」是假的——本文件第一次尝试就
# 被它坑了，漏掉 param_pages:impl → ecs，编译期才炸。
#
# 为消掉逆向边做的调整（都是**移动归属，零代码改动**）：
#   · `param_pages` base→scene→**frame**：它读 ECS 组件（SelectedObject ctx）写材质
#     资产，逻辑上就属 ECS/app 层；先试 scene 才发现 impl 还 import ecs，而 ecs 因
#     **接口**就 import vk_renderer 只能留 frame，故最终在 frame。这是完整图上
#     唯一一条需要跟着上移的边。
#   · `mip_gen` scene→gfx：GPU compute 原语，sampler_library 与 texture_upload 都要。
#   · `ecs` scene→frame：同上的接口依赖。
#
# ## 分层与收益
#
#   rlbase(30 文件/22 模块) 值类型·数学·四份配置 loader·glTF·图像编解码·反射产物
#                      ·profiler·RPC 传输·param_registry
#        │  **整层 import 闭包不含 rlvk/VmaRaii/QueueFamilyManager —— 无 GPU 无窗口即可测**
#   rlgfx(22/18)  Vulkan 抽象 + 实例/设备/交换链/资源/采样器/状态表
#   rlscene(22/16) 场景数据·渲染图·环境·纹理·模型·draw list
#   rlframe(33/18) 帧生命周期·捕获·拾取·剔除·帧循环 ECS·调试面·面板页
#        │
#   rgl(3/3)     main / main:impl / main:scene —— **含 main()，必须留可执行文件**
#
# 收益：
# ① 单元测试直接 import 真实引擎模块（同一次编译产物，不重编一份）；
# ② **「rlbase 无设备依赖」由库边界直接保证**，不再需要闭包判据 + 排除清单去维护；
# ③ 库边界即依赖方向的机器可查表达：出现逆向 import 立刻链接失败；
# ④ 静态库按需链接——消费者只 import scene_context 时，其余库的 Vulkan 模块不进
#    测试二进制（实测 nm 干净）。
#
# 命名沿用 rlstd / rllogger 的 rl 前缀族（rl = RGL library）。
#
# ⚠️ include 面 / 编译宏的归属判据 = **哪个库拥有用它的那个 TU**，不是"谁在用它"。
#   批 12.2c 在这两处各踩一次：imgui 头挂在 rlgfx 而唯一 include 它的是 rlframe 的
#   imgui_layer_impl；RGL_SLANGC_PATH 宏留在 rgl 而用它的 shader_hot_reload* 已搬进
#   rlframe。后者症状极偏：`#ifdef` 落进 #else 分支（那里是 return，编译期仍继续
#   解析）→ 后面某行报 "undeclared identifier 'ec'"。
# ═══════════════════════════════════════════════════════════════════════════

set(RGL_BASE_MODULE_SOURCES
    # ── 纯值 / 数学 / 配置解析 / 资产解析 —— **整层无设备依赖** ──
    "${CMAKE_CURRENT_SOURCE_DIR}/camera.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/cube.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/aa_target.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkerror.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gbuffer_contract.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/slot_ledger.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/pass_vocabulary.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/taa_jitter.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/velocity_field.cppm"   # 批 14.4：逐像素速度场纯函数层（rlbase，FSR 硬前置）
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_context.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/camera_config.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/camera_config_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_config.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_config_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime_config.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime_config_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shadow_config.cppm"      # 批 N6a：configs/shadow.lua loader（阴影路线枚举）
    "${CMAKE_CURRENT_SOURCE_DIR}/shadow_config_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gltf_loader.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gltf_loader_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/image_codec.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/image_codec_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_asset.cppm"      # ★0-b1：meshlet_pack 产物读取侧（只读视图+校验，纯 CPU 无设备）
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_asset_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_cull.cppm"       # ★0.5-a：CPU 参考簇剔除器（线性扫描，GPU 版的对账伙伴；纯 CPU 无设备）
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_ranges.cppm"     # ★1-a1i：逐 prim 簇区间扫描（四条产物不变量探测器；纯 CPU 无设备）
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_ranges_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_hand.cppm"       # ★0.5-c：UV 手性 CPU 参考实现 + 簇→三角寻址遍历（★1-a1ii mesh shader 的对账伙伴；纯 CPU 无设备）
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_hand_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/timing_util.cppm"      # 时间戳/时间统计纯函数层（自 profiler / present_timing / gpu_profiler 三处收敛）
    "${CMAKE_CURRENT_SOURCE_DIR}/profiler.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/profiler_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/hot_reload.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_probe.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/param_registry.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug_rpc.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug_rpc_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/types_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/material_types_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/lights_types_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/model_types_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/scene_forward_types_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/param_semantics_generated.cppm"
)

set(RGL_GFX_MODULE_SOURCES
    # ── Vulkan 抽象 + 实例 / 设备 / 交换链 / 资源 / 采样器 / 状态表 ──
    "${CMAKE_CURRENT_SOURCE_DIR}/rlvk.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/VmaBuffer.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/utils.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdl3window.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/QueueFamilyManager.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/features.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/features_generated.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/state_kit.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_profiler.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_profiler_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkinstance.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkdevice.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkswapchain.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkresources.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkresources_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/izb_shadow.cppm"        # 批 11.3-1d：FTIZB IZB 数据层（图外 compute，门控构造）
    "${CMAKE_CURRENT_SOURCE_DIR}/izb_shadow_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/evssm_shadow.cppm"      # 批 11.6d-1：EVSSM 矩图数据层（图外 compute，门控构造）
    "${CMAKE_CURRENT_SOURCE_DIR}/evssm_shadow_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsr_upscaler.cppm"      # 批 14.5：FSR 3.1.4 upscaler 封装（图外 compute，门控构造）
    "${CMAKE_CURRENT_SOURCE_DIR}/fsr_upscaler_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_target_cache.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/sampler_library.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/mip_gen.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/mip_gen_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset_table.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_geometry.cppm"   # ★1-a0：簇产物 4 段的 GPU 侧持有者（要 VkDevice → rlgfx）
    "${CMAKE_CURRENT_SOURCE_DIR}/meshlet_geometry_impl.cppm"
)

set(RGL_SCENE_MODULE_SOURCES
    # ── 场景数据 / 渲染图 / 环境 / 纹理 / 模型 / draw list ──
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_data.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_formats.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_builtin.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_decorate.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_rtslot.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_config.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph_config_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gate_lint.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gate_lint_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/environment.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/env_prefilter.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/env_prefilter_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_upload.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_upload_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_upload_cubemap.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_pool.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/model_importer.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/model_importer_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin_geometry.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin_geometry_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_list.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_list_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_draw_list.cppm"
)

set(RGL_FRAME_MODULE_SOURCES
    # ── 帧生命周期 / 捕获 / 拾取 / 剔除 / 帧循环 ECS / 调试面 / 面板页 ──
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_session.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_session_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_capture.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_capture_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/picking.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/picking_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/cull_driver.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/cull_driver_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_cull.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_cull_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_canary.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu_canary_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/present_timing.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/present_timing_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vk_indirect_commands.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vk_indirect_commands_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkrenderer.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkrenderer_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkrenderer_passrec.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vkrenderer_offgraph.cppm"  # 批 14.3：图外 compute 编舞（IZB/EVSSM；14.5 FSR 的家）
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui_layer.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui_layer_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug_rpc_exec.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug_rpc_exec_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_debug.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_debug_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_registry.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_registry_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_registry_fullscreen.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_hot_reload.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_hot_reload_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/ecs.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/ecs_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/ecs_frame_diag.cppm"      # 批 14.3：帧循环诊断旁路分区（截图时序 / 帧时间统计）
    "${CMAKE_CURRENT_SOURCE_DIR}/param_pages.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/param_pages_impl.cppm"
)

set(RGL_APP_MODULE_SOURCES
    # ── 入口 —— 含 main()，**必须**留在可执行文件 ──
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/main_impl.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/main_scene.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/main_wiring.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/main_meshlets.cppm"   # ★1-a1i：簇区间表装配（★1-a1 挂号 ①：装配接线不再塞 main_scene）
)

# ── 归属完整性闸门（configure 期，违反即 FATAL_ERROR）────────────────────
# 新增 .cppm 后若忘了登记到某个库，这里**点名报错**。刻意不用「GLOB 后减掉已登记」
# 的老做法：那时新文件会静默落进 rgl，而现在 rgl 只该有 main* —— 静默落进去等于
# 把一个 Vulkan 模块编进入口，错误一路漂到链接期才炸，且症状与真因无关。
# 「忘登记」在这里是**响亮失败**，这比任何默认落点都安全。
file(GLOB RGL_ALL_MODULE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cppm")
set(RGL_REGISTERED_MODULE_FILES
    ${RGL_BASE_MODULE_SOURCES} ${RGL_GFX_MODULE_SOURCES}
    ${RGL_SCENE_MODULE_SOURCES} ${RGL_FRAME_MODULE_SOURCES} ${RGL_APP_MODULE_SOURCES})
list(REMOVE_DUPLICATES RGL_REGISTERED_MODULE_FILES)
set(_unregistered "")
foreach(_f IN LISTS RGL_ALL_MODULE_FILES)
    get_filename_component(_abs "${_f}" ABSOLUTE)
    if(NOT _abs IN_LIST RGL_REGISTERED_MODULE_FILES)
        list(APPEND _unregistered "${_f}")
    endif()
endforeach()
if(_unregistered)
    string(REPLACE ";" "\n    " _pretty "${_unregistered}")
    message(FATAL_ERROR
        "以下 .cppm 未登记到任何引擎库（rlbase/rlgfx/rlscene/rlframe/rgl）：\n"
        "    ${_pretty}\n"
        "请把它加进 rgl/CMakeLists.txt 中职责归属正确的那一份清单。新增模块时先问"
        "「它需不需要 Vulkan 设备」——不需要的进 rlbase（该层保证无设备可测）。")
endif()
list(LENGTH RGL_ALL_MODULE_FILES _n_all)
list(LENGTH RGL_REGISTERED_MODULE_FILES _n_reg)
if(NOT _n_all EQUAL _n_reg)
    message(FATAL_ERROR "模块归属数量不符：磁盘上 ${_n_all} 个，登记 ${_n_reg} 个（清单里有文件已不存在？）")
endif()


# rlcore 的第三方依赖（**故意不含 Vulkan / SDL3 / VMA / imgui**——那层是 rgl 的）
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(libpng CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(freeimage CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
# tinygltf 是 header-only 且该 vcpkg port 不导出 CMake target：find_path 取头文件目录。
# SYSTEM = -isystem——第三方头内警告不进本项目 -Werror 面。
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)


# NVIDIA Aftermath GPU crash dump（诊断能力，编译期开关；默认关，排查 NV 崩溃时开）
# 用法：just config 前设 RGL_AFTERMATH=ON，或 cmake -DRGL_AFTERMATH=ON
option(RGL_AFTERMATH "Enable NVIDIA Aftermath GPU crash dumps (Linux/NVIDIA only)" OFF)
if(RGL_AFTERMATH AND UNIX AND NOT APPLE)
    set(AFTERMATH_SDK_DIR "${CMAKE_SOURCE_DIR}/../Aftermath")
    target_include_directories(rlgfx PUBLIC "${AFTERMATH_SDK_DIR}/include")
    target_link_libraries(rlgfx PUBLIC "${AFTERMATH_SDK_DIR}/lib/x64/libGFSDK_Aftermath_Lib.x64.so")
    target_compile_definitions(rlgfx PUBLIC RGL_AFTERMATH=1)
endif()
# ── 第三方依赖（分层 find_package，各库只拿自己用得到的）────────────────────
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(libpng CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(freeimage CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
# tinygltf 是 header-only 且该 vcpkg port 不导出 CMake target：find_path 取头文件目录。
# SYSTEM = -isystem——第三方头内警告不进本项目 -Werror 面。
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)
find_package(Vulkan REQUIRED)
# loader 守卫（2026-10-02 事故：vcpkg 自 build 的 feature-reduced loader 劫持 RUNPATH
# = 窗口模式 wayland_surface 缺失，headless 免疫故测试全绿）——见文件头注释。
include(${PROJECT_SOURCE_DIR}/cmake/RglVulkanLoaderGuard.cmake)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# ── rlbase：整层无设备依赖（闭包不含 rlvk / VmaRaii / QueueFamilyManager）────
add_library(rlbase STATIC tinygltf_impl.cpp)
target_sources(rlbase PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_BASE_MODULE_SOURCES}
)
target_compile_features(rlbase PUBLIC cxx_std_23)
# GLM 深度约定必须与上层一致（zero-to-one + 弧度）：frustumBoundingSphere 的近远
# 反推、projWithDepthRange 全依赖这两个宏。不一致 = 测试在测另一个函数。
# PUBLIC = 沿 rlbase→rlgfx→rlscene→rlframe 逐层传递。
target_compile_definitions(rlbase PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
target_include_directories(rlbase SYSTEM PUBLIC ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(rlbase
    PUBLIC
        rlstd
        rllogger
        glm::glm
        EnTT::EnTT
        sol2::sol2
        lua
    PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
        PNG::PNG
        libjpeg-turbo::jpeg-static
        freeimage::FreeImage
        ZLIB::ZLIB
)
# 反射生成物在 rlbase 的 FILE_SET 里 → 反射产物必须先于它就绪
add_dependencies(rlbase reflection_check)

# ── rlgfx：Vulkan 抽象 + 设备 / 资源。vam_impl 是 VmaRaii 模块的实现 TU ─────
# include 面归属：SDL3 头由 sdl3window.cppm（vulkan_window，本库）的 GMF include；
# imgui 头由 imgui_layer_impl.cppm（**rlframe**）的 GMF include → 必须挂 rlframe。
add_library(rlgfx STATIC vam_impl.cpp)
target_sources(rlgfx PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_GFX_MODULE_SOURCES}
)
target_link_libraries(rlgfx
    PUBLIC rlbase Vulkan::Vulkan
    PRIVATE GPUOpen::VulkanMemoryAllocator SDL3::SDL3
            # 批 14.5：FSR 3.1.4 upscaler VK 后端。PRIVATE 足够——fsr_upscaler 走 pimpl，
            # vendor 头只出现在它的 impl partition 里，不外泄 BMI；PRIVATE 也就不把
            # vendor include 面灌进 rlscene/rlframe。（其 PRIVATE 宏 FFX_FSR3UPSCALER
            # 只影响 .cpp 侧排列表实例化，头文件无条件编译守卫。）
            ffx_fsr3upscaler_vk)
add_dependencies(rlgfx reflection_check)   # features_generated 在本库

# ── rlscene：场景数据 / 渲染图 / 资产 ─────────────────────────────────────
add_library(rlscene STATIC)
target_sources(rlscene PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_SCENE_MODULE_SOURCES}
)
target_link_libraries(rlscene PUBLIC rlbase rlgfx)
add_dependencies(rlscene reflection_check)

# ── rlframe：帧执行 / ECS 帧循环 / 调试面 / 面板页 ─────────────────────────
add_library(rlframe STATIC)
target_sources(rlframe PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_FRAME_MODULE_SOURCES}
)
# imgui 的 include 面在本库（imgui_layer_impl.cppm 的 GMF）——见 rlgfx 段注
target_link_libraries(rlframe
    PUBLIC rlbase rlgfx rlscene
    PRIVATE imgui::imgui httplib::httplib nlohmann_json::nlohmann_json)
# Phase 10 批 10.4 shader 热重载（L1）：fork slangc 重编源树 shaders/ 所需三件套
# （D11：flags = SLANG_FLAGS_STR 同源派生串，与构建期 SPV 编译 flag 恒一致；
#  路径/flags 缺失 = wireShaderHotReload 降级关并日志）。归属 **本库**：
#  grep 核对仅 shader_hot_reload / shader_hot_reload:impl 使用这三个宏。
target_compile_definitions(rlframe PRIVATE
	"RGL_SLANGC_PATH=\"${SLANGC_EXECUTABLE}\""
	"RGL_SLANG_FLAGS=\"${SLANG_FLAGS_STR}\""
	"RGL_SOURCE_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\"")
add_dependencies(rlframe reflection_check)


# rgl 可执行文件：vam_impl（VMA 编译单元）+ SPV 产物 + **剩余**模块（GLOB 减去 rlcore）
# ── rgl 可执行文件：SPV 产物 + 入口模块（main*，含 main()）─────────────────
# 入口**只能**住这里：CMake CXX_MODULES FILE_SET 只有本 target 能 import 自己的
# 模块，且 main() 合法于分区实现单元。
add_executable(rgl ${RGL_SPV})
target_sources(rgl PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_APP_MODULE_SOURCES}
)
target_link_libraries(rgl PRIVATE rlframe)
target_compile_features(rgl PRIVATE cxx_std_23)

add_dependencies(rgl reflection_check)
# shader DEP 表检查（批 12.3d-D1）：脚本只读源树，与编译并行（挂法同 reflection_check，
# 详见 cmake/RglChecks.cmake 内注释）。
add_dependencies(rgl shader_dep_check)
# 源码检查必须等编译产物就绪（libclang 读半写 BMI 会 crash，坑 4c，
# 详见 cmake/RglChecks.cmake 头注释）。
add_dependencies(source_checks rgl)

find_package(Vulkan REQUIRED)

# rlstd/rllogger/glm/EnTT/sol2/lua 经 rlcore 的 PUBLIC 传递；此处只补 Vulkan 侧的直接依赖
target_link_libraries(rgl PRIVATE Vulkan::Vulkan)
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
