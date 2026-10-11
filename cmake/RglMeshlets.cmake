# ─── 簇产物构建期生成（簇化轴 ★0-b2，docs/mdi_plan.md §7.8 拍板 3）──────────────
# 由 rgl/CMakeLists.txt include。meshlet_pack（tools/，EXCLUDE_FROM_ALL）把清单内 glb
# 转成 `<模型路径>.meshlets.bin`（引擎侧路径约定 = 模型路径 + 本后缀，按 CWD 相对解析，
# 即 build/rgl/models/<name>.glb.meshlets.bin）。
#
# ## 为什么构建期生成、不进 git（拍板 3 重述，改动前先读 §7.8）
# 产物是纯派生文件（bistro ≈184MB），缓存键 = 源 glb + 工具版本——ninja 的
# OUTPUT/DEPENDS 天然就是这台缓存机器：源 glb 或 meshlet_pack 二进制任一变更才重跑。
# 进 git = 几百 MB 第二副本且会与源漂；运行期重算 = 为启动路径付秒级（★0-b1 裁决 1）。
#
# ## 增量必须成立（验收判据）
# bistro 转换 ≈4.3s + 读 481MB，**每次构建都付不可接受**。add_custom_command(OUTPUT)
# 的语义保证产物存在且不比依赖新就整条边跳过——第二次 just build 不许出现 Packing 行。
#
# ## 🔴 git-lfs 防御在 configure 期，不在 build 期
# glb 由 git-lfs 管理：新克隆没拉 LFS 时源文件是 ~135B 的指针文本。把它喂给
# meshlet_pack = **构建红**——比运行期失败更糟：产物是可选加速资产，引擎缺它走现行路径
# （§7.8 拍板 3「缺失 → 回落」），构建红却把干净 checkout 的正常构建一起挡掉。所以
# configure 期按**源文件大小**检查，低于阈值（指针文件与真 glb 差六个数量级，1MB 是
# 宽松判据）就 WARNING 点名并**不注册**该条 custom command，而不是留到构建期炸。
#
# ## 产物不进 git
# 输出落在 build/rgl/models/（构建目录整体被 .gitignore 覆盖）——天然不被跟踪，
# 无需为它动 .gitignore。

include_guard()

# 本批清单 = 只有 bistro（其余 5 个 glb **不进清单**：每个都是几百 MB 产物与数秒转换，
# 全量生成不是 ★0-b2 的口径；扩清单 = 后续批次按验收需要加行）。
set(RGL_MESHLET_MODELS
    "${PROJECT_SOURCE_DIR}/assets/bistro_exterior.glb")

# git-lfs 指针文件（~135B）与真 glb（数百 MB）的存在性阈值（判据见文件头）。
set(RGL_MESHLET_MIN_GLB_BYTES 1048576)

# models/ 目录由 RglRuntimeAssets.cmake 的 rgl_copy_files 创建，但两份清单可独立增删
# （本文件只依赖目录存在，不依赖 copy_models 目标），故自建一次（幂等）。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models")

set(RGL_MESHLET_OUTPUTS "")
foreach(glb IN LISTS RGL_MESHLET_MODELS)
    get_filename_component(glbName "${glb}" NAME)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/models/${glbName}.meshlets.bin")
    if(NOT EXISTS "${glb}")
        message(WARNING
            "RglMeshlets: 源模型 ${glbName} 不存在，跳过簇产物生成（引擎缺产物走现行路径，不红）")
        continue()
    endif()
    file(SIZE "${glb}" glbBytes)
    if(glbBytes LESS RGL_MESHLET_MIN_GLB_BYTES)
        message(WARNING
            "RglMeshlets: ${glbName} 只有 ${glbBytes}B（< ${RGL_MESHLET_MIN_GLB_BYTES}B，"
            "疑似 git-lfs 指针文件未拉取），跳过簇产物生成——`git lfs pull` 后重新 just config。"
            "不注册 custom command 的理由见本文件头「git-lfs 防御」")
        continue()
    endif()
    # DEPENDS 的 meshlet_pack 是**目标级依赖**（tools/ 为 EXCLUDE_FROM_ALL：平时不编，
    # 这条边需要它时才编）；$<TARGET_FILE:> 保证 COMMAND 指到构建出的可执行文件。
    add_custom_command(
        OUTPUT "${out}"
        COMMAND $<TARGET_FILE:meshlet_pack> "${glb}" --out "${out}"
        DEPENDS meshlet_pack "${glb}"
        COMMENT "Packing ${glbName} -> ${glbName}.meshlets.bin (meshlet_pack)"
        VERBATIM)
    list(APPEND RGL_MESHLET_OUTPUTS "${out}")
endforeach()

if(RGL_MESHLET_OUTPUTS)
    add_custom_target(rgl_meshlets ALL DEPENDS ${RGL_MESHLET_OUTPUTS})
endif()
