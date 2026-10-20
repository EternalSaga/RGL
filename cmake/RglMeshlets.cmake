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
#
# 每条 = `<glb 路径>|<--must-l0-diag 门槛>`，门槛单位 = **源 glb 单位**（不是米！见下）。
# 🔴 门槛**逐资产显式给**，没有全局默认：must_L0 的判据是"分量包围对角线 ≥ 门槛"，而对角线
# 住在源单位里（format.h 坐标系一节：产物未乘 scene.lua 的 scale）⟹ 同一个数字在 scale 不同的
# 资产上是完全不同的物理尺寸。写一个全局默认 = 造一个隐式的资产相关参数（正是 P0 数 1b 首版
# 踩过的坑，探针 `SIZE_BINS_M` 注释记着）。新模型进清单必须自己写一个数，或写 `|0` 关闭。
#
# ## bistro 为什么是 100
# 100 源单位 × scene.lua 的 scale=0.01 = **世界 1m**，即"保住对角线 ≥1 米的独立遮挡体"这条
# 产品口径（`docs/shadow_proxy_plan.md` §7.1 的目标）。
# 🔴 **门槛低于 1m 时代价爆炸**（实测，别当线性外推）：0.3m → **1.99×** 三角、0.1m → **2.41×**
# （机理 = 一个 L0 group 有 60.6% 跨 ≥5 个分量，门槛一低几乎每个 group 都含"必须保"的分量
# ⟹ "整组钉 L0"退化成"全量"）。读数与曲线 = `docs/还债实测数据.md` §4.8 的「数 6」表（跑
# `python3 tools/shadow_proxy_probe.py <产物> --glb-scale 0.01` 可复现），裁决记录 =
# `docs/shadow_proxy_plan.md` §5「门槛低于 1m」那一行（**不做**）。
set(RGL_MESHLET_MODELS
    "${PROJECT_SOURCE_DIR}/assets/bistro_exterior.glb|100")

# git-lfs 指针文件（~135B）与真 glb（数百 MB）的存在性阈值（判据见文件头）。
set(RGL_MESHLET_MIN_GLB_BYTES 1048576)

# models/ 目录由 RglRuntimeAssets.cmake 的 rgl_copy_files 创建，但两份清单可独立增删
# （本文件只依赖目录存在，不依赖 copy_models 目标），故自建一次（幂等）。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models")

set(RGL_MESHLET_OUTPUTS "")
foreach(entry IN LISTS RGL_MESHLET_MODELS)
    # `<glb>|<门槛>` 拆分。🔴 形态不对就 configure 期 FATAL：静默按 0 处理 = 产物悄悄没有
    # MustL0 段，而 P2 会把"没有段"读成"这个资产没有要保的部件"——那是比构建红更坏的形态。
    if(NOT entry MATCHES "^(.+)\\|([0-9]+(\\.[0-9]+)?)$")
        message(FATAL_ERROR
            "RglMeshlets: 清单条目 `${entry}` 不是 `<glb 路径>|<must-l0-diag 门槛（源单位）>` 形态；"
            "关闭判定要显式写 `|0`（为什么门槛逐资产给 → 本文件头）")
    endif()
    set(glb "${CMAKE_MATCH_1}")
    set(mustL0Diag "${CMAKE_MATCH_2}")
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
        COMMAND $<TARGET_FILE:meshlet_pack> "${glb}" --out "${out}" --must-l0-diag "${mustL0Diag}"
        DEPENDS meshlet_pack "${glb}"
        COMMENT "Packing ${glbName} -> ${glbName}.meshlets.bin (meshlet_pack, must-l0-diag=${mustL0Diag})"
        VERBATIM)
    list(APPEND RGL_MESHLET_OUTPUTS "${out}")
endforeach()

if(RGL_MESHLET_OUTPUTS)
    add_custom_target(rgl_meshlets ALL DEPENDS ${RGL_MESHLET_OUTPUTS})
endif()
