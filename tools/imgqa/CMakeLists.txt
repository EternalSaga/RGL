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
    # `diffblock` 的 spec 段（★0.5-b1，同上一条的拆法：主表撞护栏，缝按仪器切）。
    cli_table_diffblock.cpp
    # `blend-sort` 的 spec 段（债-132，同上一条的拆法：主表撞 500 行、specsSwimAndProbe 撞 120 行）。
    cli_table_blend.cpp
    # `blend-lit` 的 spec 段（债-134 先决，同 blend 段的拆法：主表撞 500 行，缝按仪器切）。
    cli_table_blend_lit.cpp
    # `shadow-grain` 的 spec 段（同 blend 段的拆法：主表撞 500 行、specsSwimAndProbe 撞 120 行，
    # 缝按仪器切）。
    cli_table_grain.cpp
    # `shadow-cascade-seam` 的 spec 段（同 grain 段的拆法：主表撞 500 行、specsSwimAndProbe
    # 撞 120 行，缝按仪器切）。
    cli_table_seam.cpp
    # `shadow-caster-survival` 的 spec 段（同 seam 段的拆法，缝按仪器切）。
    cli_table_survival.cpp
    # `shadow-edge-ruler` 的 spec 段（同 survival 段的拆法，缝按仪器切）。
    cli_table_edge.cpp
    analytic.cpp
    ao_metrics.cpp
    # `ao-noise`（护栏逼的三刀：ao_noise.cpp 903 代码行 > 500），缝按仪器切、缝说明全文见
    # `ao_noise_metric.h` 头注释。`ao_noise_metric.cpp` = **口径层**（帧差算子 / 逐位相同判定 /
    # 取值分布与扫描线 / 存图可视化，同 shadow_kernel_metric.* 与 diff_struct_metric.* 那条缝）；
    # `ao_noise_groups.cpp` = **组 2/3/4/5/6**（`组` = 本命令天然的仪器边界：一组一次采集、
    # 一组一张读数表）；`ao_noise.cpp` 留入口 + 采集 + 组 1/1b + 判定（「清空 outDir 陈旧产物」
    # 必须只执行一次 ⟹ 留在入口）。跨 TU 的类型与阈值常量唯一出处 = `ao_noise_metric.h`。
    ao_noise.cpp
    ao_noise_metric.cpp
    ao_noise_groups.cpp
    # 引擎驱动的三个面（护栏逼的一刀：engine.cpp 969 代码行 > 500），缝按**仪器层次**切：
    #   · engine_lua_edit.cpp  = **lua 文本编辑原语**——在一份 lua 文本里定位并替换一个键的值，
    #                            不认识 ProfileOpts、不认识任何文件名、不读写盘（跨 TU 面 =
    #                            `engine_lua_edit.h`，其余定位件留在本 TU 匿名 namespace）
    #   · engine_profile.cpp   = **配置语义**——把 ProfileOpts 翻译成 lua（交付侧逐键覆写 /
    #                            合成侧整份写出），`makeProfile` 按配置域切小函数
    #   · engine_profile_gates.cpp = **例外门**（第 4..8 枚：写盘后的 read-modify-write +
    #                            写后读回自检；护栏逼的一刀——加第 8 枚门后 521 代码行 > 500。
    #                            共同契约「门闭 = 逐位不变」与调用顺序语义见其头注）
    #   · engine.cpp           = **引擎会话**——SPV 新鲜度守卫 + rgl 子进程生命周期 + RPC 客户端
    # 依赖只向下：会话层不认识任何一份 lua 文件的名字，配置层不 fork 进程。
    engine_lua_edit.cpp
    engine_profile.cpp
    engine_profile_gates.cpp
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
    # `diffblock` 判据段（★0.5-b1，1 条 `diffblock-thickness`），缝按仪器切（同 ssr 段）。
    judgments_table_block.cpp
    # `blend-sort` 判据段（债-132，4 条），缝按仪器切（同 block / ssr 段）。
    judgments_table_blend.cpp
    # `blend-lit` 判据段（债-134 先决，5 条），缝按仪器切（同 blend / block / ssr 段）。
    judgments_table_blend_lit.cpp
    # `shadow-grain` 判据段（2 条：痤疮上界 + slope_bias 接线阳性锚点），缝按仪器切
    # （`judgments_table_shadow.cpp` 把头注缝声明为「三条全部属于 shadow-kernel-scale」）。
    judgments_table_grain.cpp
    # `shadow-cascade-seam` 判据段（3 条：LOD 归因增量 / 两 force 臂阳性锚点 / D 扫描形状），
    # 缝按仪器切（`judgments_table_grain.cpp` 把头注缝声明为「三条全部属于 shadow-grain」）。
    judgments_table_seam.cpp
    # `shadow-caster-survival` 判据段（2 条：出厂档小块/近场存活率【阈值未定只报数】/ 面积存活率
    # 随 D 单调不增），缝按仪器切（`judgments_table_seam.cpp` 把头注缝声明为「本段三条全部属于
    # shadow-cascade-seam」）。
    judgments_table_survival.cpp
    # `shadow-edge-ruler` 判据段（3 条：出厂档绝对误差【界未定只报数】/ 相邻级缝宽【N 未定
    # 只报数】/ |误差| 随 D 单调不减），缝按仪器切（`judgments_table_survival.cpp` 把头注缝
    # 声明为「本段两条属于 shadow-caster-survival」）。
    judgments_table_edge.cpp
    # `selftest`（护栏逼的五刀：selftest.cpp 1927 代码行 > 500），缝按**被测仪器**切、
    # 缝说明全文见 `selftest.h` 头注释。`selftest.cpp` 留**地基**（image.cpp 的图像算子 +
    # judge.cpp 的判据/反模式注册表 + cli.cpp 的 CLI 注册表）与 `cmdSelftest` 汇总路由
    # （39 个 `testXxx(c)` 的调用顺序 = 失败时的打印顺序，**一字不许换**）；其余按仪器族分八个
    # TU：judgments（判定口径与配置解析）/ shadow（阴影族两台量具）/ ao_residual（AO 度量 +
    # 时域绝对残差）/ bands（不读图不启引擎的纯度量层）/ motion（运动态时域族）/
    # ao_noise（ao-noise 口径层，债-90/91）/ swim（taa-swim 输出面，债-94）/ block（diffblock）。
    # 跨 TU 的 `Checker` 与共享断言辅助唯一出处 = `selftest.h`：每个 TU 各抄一份 Checker 不会
    # 编译失败，只会把 `bad` 变成每 TU 一份、cmdSelftest 只看得见自己那一份 = 其余 TU 的失败
    # 静默吞掉。
    selftest.cpp
    selftest_judgments.cpp
    selftest_shadow.cpp
    selftest_ao_residual.cpp
    selftest_bands.cpp
    selftest_motion.cpp
    # `diffblock` 形态判据的锚点段（★0.5-b1）：五条阳性锚点 + τ 语义 + erode↔distanceTransform
    # 等价性（合成亮度图直测 `diff_block_metric.h` 口径层，不读 PNG 不启引擎）。
    selftest_block.cpp
    # `ao-noise` 口径层（债-90/91）与 `taa-swim` 仪器层输出面（债-94）的自检段（P1-G1）：
    # 合成小图/合成帧序列直测打印形态，不启引擎。
    selftest_ao_noise.cpp
    selftest_swim.cpp
    # `blend-sort` 口径层（债-132）的自检段：投影锚点 / 合成闭式 / 显示链金标准 / 判别力 /
    # 三条拒跑 / 挂号，全部**不启引擎**（合成 BlendPiece 直打 `blend_sort_metric.cpp`）。
    selftest_blend_sort.cpp
    # `blend-lit` 口径层（债-134 先决）的自检段：投影锚点（命中实机 pane 边界）/ 分桶正解 /
    # 分桶反例（confined 判据不是恒绿的摆设）/ pad 边界口径 / 四条拒跑 / 五条挂号，
    # 全部**不启引擎**（合成几何 + 合成两臂小图直打 `blend_lit_metric.cpp`）。
    selftest_blend_lit.cpp
    # `shadow-grain` 口径层（影场颗粒度）的自检段 = **判据 4**：任务钦定四条算子自检
    # （棋盘/竖条纹 = 恰 1.0、34 宽斜坡 = 恰 1/33、常数 = 恰 0）+ 逐带存活率（在场 = 恰 1 /
    # 半高 = 恰 0.5 / 被抹 = 恰 0 / 抖动 = 恰 2 + 三条守卫）+ 线性域锚点 / eps 守卫 /
    # clip / 窗口纪律 + 三条判决线挂号，全部闭式不启引擎（合成 cov 场直打
    # `shadow_grain_metric.cpp`）。
    selftest_grain.cpp
    # `shadow-cascade-seam` 口径层（相邻级联阴影分歧）的自检段 = **判据 4**：任务钦定的闭式
    # （常数场台阶 = 恰 0、单条 0.25 台阶 = 恰 0.25、高度 1 = 恰 1.0、宽 33 斜坡 halfWidth=1 =
    # 恰 1/33、翻转率精确分数）+ 线性域锚点 / eps 守卫 / clip / 严格阈值 / band 是分母 +
    # 四条守卫各一条 throwsStd + 三条判决线挂号，全部闭式不启引擎（合成场直打
    # `shadow_seam_metric.cpp`）。
    selftest_seam.cpp
    # `shadow-caster-survival` 口径层（阴影遮挡体存活率）的自检段 = **判据 4**：同图→四项恰
    # 1.000 / 全 0→恰 0.000 / 🔴 立命断言「只丢小块、总面积不变」合成对（面积存活率恰 1.000 而
    # 小块档恰 0.000——证明本判据不是 seam 差分判据的换皮）/ 消失-变淡各一条闭式 + 边界每半边 +
    # 分档边界 + 8 邻接 + 空档 NaN + 守卫各一条 throwsStd + 闸门 C/D + 两条判决线挂号，全部闭式
    # 不启引擎（合成场直打 `shadow_survival_metric.cpp`）。
    selftest_survival.cpp
    # `shadow-edge-ruler` 口径层（影边世界位置绝对尺）的自检段：🔴 立命断言「合成斜坡 50%
    # 过零点误差 <1e-9」/ 变宽 50% 点不动 / 全 0·全 1·多过零三守卫（多过零必须抛）/
    # worldPerpMm 闭式 / 级联公式链闭式对账 / 闸门 C·D（含零臂·零板空集合守卫）/ 挂号，
    # 全部闭式不启引擎（合成 cov 场直打 `shadow_edge_metric.cpp`）。
    selftest_edge.cpp
    # `temporal` / `taa-swim`（护栏逼的两刀：temporal.cpp 926 代码行 > 500，里面住着**两台
    # 仪器**），缝按仪器切、缝说明全文见 `temporal_swim.h` 头注释。`temporal.cpp` = **锚点线**
    # （静止机位 + jitter 的抑制比：yawCam / runSequence / temporalDiffStat / cmdTemporal）；
    # `temporal_swim_metric.cpp` = 游动线**口径层**（平移搜索 freeShift / lagTranslation /
    # 相位与 λ 标定 / absMeasure / runSwimSequence，同 shadow_kernel_metric.* 那条「口径 vs
    # 仪器」的缝）；`temporal_swim.cpp` = 游动线**仪器层**（cmdTaaSwim 与阶段函数 + 判决线）。
    # 跨 TU 类型（SwimRun / FreeShift / LagStat / …）与 d6/d2 唯一出处 = `temporal_swim.h`。
    temporal.cpp
    temporal_swim_metric.cpp
    temporal_swim.cpp
    # `temporal-residual`（护栏逼的七刀：temporal_residual.cpp 1908 代码行 > 500，且九个函数被
    # 120 行 / 认知 15 两条点名，其中 runBistroCamera 认知 57、runBistroClipAblation 认知 43 双双
    # 过 30 ERROR 线），缝按**仪器**切、缝表全文见 `temporal_residual_metric.h` 头注释：
    #   · temporal_residual_metric.cpp        = **纯度量算子层**——折叠 / 分带 / 拒收分桶 / EMA 闭式，
    #     不碰引擎，selftest 直接打这一层（同 shadow_kernel_metric.* 那条「口径 vs 仪器」的缝）。
    #   · temporal_residual_target.cpp        = 合成靶**采集层**（R0–R4 + 批 13.7 长短窗 B1–B4）。
    #   · temporal_residual_target_verdicts.cpp = 合成靶**判据层**（验参 → 出表 → 7 判据 → 台账）。
    #   · temporal_residual_bistro.cpp        = bistro **采集原语** + 批 13.10 历史拒收交叉表。
    #   · temporal_residual_bands.cpp         = 批 13.7c 静止态「亮度 × 梯度」二维分带出表。
    #   · temporal_residual_bistro_camera.cpp = 一机位**编排**（六组顺序 / 两条守卫 / 热图 = 契约）。
    #   · temporal_residual_bistro_abl.cpp    = 批 13.11 裁剪消融 A/B 臂集与两个 sweep。
    #   · temporal_residual_moving_bands.cpp  = 批 13.9 移动机位分带（两把尺子共用的出表与判决）。
    #   · temporal_residual.cpp               = 命令入口与变体分岔。
    # 跨 TU 类型（Run / BistroRun / BistroReject / RejectReport / BandReport / SwingCore）、阈值常量
    # 整块与 d6/d4/d2/dExp 唯一出处 = `temporal_residual_metric.h`（⚠ 该头只准 temporal_residual*.cpp
    # include：d6/d2 与 temporal_swim.h 同名同签名，同 TU 双 include 会重定义报错）。
    temporal_residual_metric.cpp
    temporal_residual_target.cpp
    temporal_residual_target_verdicts.cpp
    temporal_residual_bistro.cpp
    temporal_residual_bands.cpp
    temporal_residual_bistro_camera.cpp
    temporal_residual_bistro_abl.cpp
    temporal_residual_moving_bands.cpp
    temporal_residual.cpp
    # `temporal-motion` 家族（护栏逼的十三刀：temporal_motion.cpp 2748 代码行 > 500，且里面
    # 住着**六台各自独立的仪器**），缝按**仪器**切、缝说明见 `temporal_motion.h` 头注释：
    #   · temporal_motion_metric.cpp  = **口径层**——内容位移闭式解 / 平移 / 峰值池化 / 拒收分桶 /
    #     逐帧 argmin 对齐 / 偏差与抖动相位拟合 / 尾长阶梯 / 静止收敛差（同 `shadow_kernel_metric.*`
    #     那条「口径 vs 仪器」的缝，这一层不启引擎）。
    #   · temporal_motion_collect.cpp = **采集层**——`collectGroup`「一组 = 一个引擎进程」，六台
    #     仪器共用这一条链；只碰引擎与磁盘，不做判据算术。
    #   · temporal_motion_report.cpp  = 批 13.9 移动机位「亮度 × 梯度」分带出表与三分支判决。
    #   · temporal_motion.cpp         = `temporal-motion` 入口（头注释 = 整族的设计与红线全文）。
    #   · temporal_feedback.cpp       = `temporal-feedback` 运动态 feedback 扫描（采集 + 守卫 + 出表）。
    #   · temporal_feedback_still.cpp = 同台的**静止态对照**（窗棂残余 swing + 借掩码口径两列）。
    #   · temporal_feedback_judge.cpp = 同台的 **fb 曲线聚合与四条判决线**（缝 = 出表 vs 判决）。
    #   · temporal_tail.cpp           = `temporal-tail`（spin 版）鬼影尾长直测。
    #   · temporal_smear.cpp          = `temporal-tail --motion camera` 的**采集与打表**。
    #   · temporal_tail_camera.cpp    = 同台的**闸门与判决编排**（缝 = 采集 vs 判据，同
    #     `temporal_residual_target*.cpp` 那条缝）。
    #   · temporal_step.cpp           = `temporal-tail --motion step` 的 TAA 臂与判决线。
    #   · temporal_step_fsr.cpp       = 同台的 FSR 臂集（缝 = 同一台仪器的 TAA 臂 vs FSR 臂）。
    # 跨 TU 的类型（Spec / Group / TailRow / SmearAngle / StepArm / …）、阈值常量与 d4/d2/pct2
    # 唯一出处 = `temporal_motion.h`（⚠ 该头只准本族 .cpp include：`Group`/`kSize` 与
    # `temporal_flicker_metric.h`、`temporal.cpp` 的同名件在同一个二进制里，靠 `tmotion` 命名空间隔开）。
    temporal_motion_metric.cpp
    temporal_motion_collect.cpp
    temporal_motion_report.cpp
    temporal_motion.cpp
    temporal_feedback.cpp
    temporal_feedback_still.cpp
    temporal_feedback_judge.cpp
    temporal_tail.cpp
    temporal_smear.cpp
    temporal_tail_camera.cpp
    temporal_step.cpp
    temporal_step_fsr.cpp
    # `temporal-flicker`（护栏逼的一刀：temporal_flicker.cpp 571 代码行 > 500），缝按**仪器层次**切、
    # 缝说明全文见 `temporal_flicker_metric.h` 头注释。`temporal_flicker_metric.cpp` = **口径层**
    # （闭式解 / 反标定 / Hann 半谱 / 帧差分位 / 边缘定位 / measureGroup，不碰引擎、selftest 用
    # 闭式直接测，同 shadow_kernel_metric.* 与 diff_struct_metric.* 那条缝）；`temporal_flicker.cpp`
    # 留采集 + 命令 + 判决线。跨 TU 类型（Group）与常量（kPi）唯一出处 = `temporal_flicker_metric.h`。
    temporal_flicker_metric.cpp
    temporal_flicker.cpp
    # `temporal-homography`（护栏逼的两刀：temporal_homography.cpp 824 代码行 > 500，且
    # reportMovingBandsFromHomography / cmdTemporalHomography 双双认知 >30 ERROR），缝按**仪器
    # 层次**切、缝说明全文见 `temporal_homography_metric.h` 头注释。这台判据工具是三层，故三刀：
    #   · temporal_homography_metric.cpp = **重投影算子层**——闭式单应 + 与引擎 TAA 逐式同权重的
    #     重建核（Catmull-Rom / Lanczos3 / 双线性）+ 三条核自检（恒等性 / 整数平移 / 与
    #     cv::warpPerspective 比量级）+ 两条分位口径 + α 扫描。同 shadow_kernel_metric.* 那条
    #     「口径 vs 仪器」的缝；三条 check 是仪器自己的标定锚点，权重系数一个都不许化简。
    #   · temporal_homography_bands.cpp  = **运动带判据层**——分带尺子（含 τ 的实测覆盖）、可测性
    #     守卫的执行与回显、六格表 + 材质归因 + 三分支判决（判决线在 temporal_residual.h）。
    #   · temporal_homography.cpp        = **采集与命令**——runHomography + deliveryGlb +
    #     cmdTemporalHomography（CLI 生命周期、读数表、守卫闸门）。
    # 跨 TU 类型（WarpKernel / HomoStat / SrcMap / MovingBandRuler / MovingBandReport）与常量
    # （kFovDeg）唯一出处 = `temporal_homography_metric.h`。
    temporal_homography_metric.cpp
    temporal_homography_bands.cpp
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
    # `shadow-grain`：出厂档不许吃掉真实影带（程序化 shadow_grain 靶·多缝深组 + 六臂 +
    # 闭式存活率刻度）。拆 TU 的缝 = **口径 vs 仪器内部**：`shadow_grain_metric.cpp` 是
    # 四个纯算子（覆盖度场 / 跳变密度 / 逐带跳变计数 / 逐带存活率，selftest 用闭式直接测，
    # 不启引擎）；仪器内部再按职责切（护栏：单文件 < 500 代码行；先例 ao_noise /
    # temporal_residual）：`shadow_grain.cpp` = 臂表 + 采集 + 回显对账 + 在场闸门 + 入口，
    # `shadow_grain_target.cpp` = manifest 测量接口 + 投影窗/列区间，`shadow_grain_judge.cpp`
    # = 三条判决线。跨 TU 类型与量具常数唯一出处 = `shadow_grain.h`（口径常数在
    # `shadow_grain_metric.h`）。
    shadow_grain_metric.cpp
    shadow_grain_target.cpp
    shadow_grain_judge.cpp
    shadow_grain.cpp
    # `shadow-cascade-seam`：相邻级联对阴影的分歧（远级簇 LOD 不许造出级联交界缝，债-110）。
    # 拆 TU 的缝 = **口径 vs 仪器内部**（四切法照 shadow_grain，但**没有 target TU**：本仪器
    # 的区域是与几何无关的固定带，理由住 `shadow_cascade_seam.cpp` 头注「区域为什么是固定带」）：
    # `shadow_seam_metric.cpp` = 口径层（覆盖度场 / 翻转率 / 最硬横向台阶，被 selftest 闭式直测）；
    # `shadow_cascade_seam.cpp` = 臂表 + 采集 + 引擎回显对账 + 🔴 簇路径生效闸门 + 入口编排；
    # `shadow_cascade_seam_judge.cpp` = 三条判决线。跨 TU 类型与量具常数唯一出处 =
    # `shadow_cascade_seam.h`（口径常数在 `shadow_seam_metric.h`）。
    shadow_seam_metric.cpp
    shadow_cascade_seam_judge.cpp
    shadow_cascade_seam.cpp
    # `shadow-caster-survival`：阴影遮挡体存活率（**绝对量**判据，参照 = D=-1 全量几何；补
    # shadow-cascade-seam 对「四级一起变差」的结构失明，债-139）。拆 TU 的缝 = **口径 vs 仪器内部**
    # （四切法照 shadow-cascade-seam，同样**没有 target TU**——区域 = 全幅 + 固定近场带）：
    # `shadow_survival_metric.cpp` = 口径层（面积存活率 / 连通块分档存活率 / 消失-变淡归因 /
    # 闸门 C·D，被 selftest 闭式直测）；`shadow_caster_survival.cpp` = 臂表 + 双机位采集 +
    # 引擎回显对账 + 四道闸门 + 扫描表；`shadow_caster_survival_judge.cpp` = J1（INFO 不判）+
    # J2（单调性）。跨 TU 类型与量具常数唯一出处 = `shadow_caster_survival.h`（口径常数在
    # `shadow_survival_metric.h`）。`cluster_path_gate.cpp` = 簇路径生效闸门（本仪器与
    # shadow-cascade-seam **共用一份实现**——needle 名单抄两份 = 第二份真相，见其头注释）。
    cluster_path_gate.cpp
    shadow_survival_metric.cpp
    shadow_caster_survival_judge.cpp
    shadow_caster_survival.cpp
    # `shadow-edge-ruler`：影边世界位置**绝对尺**（真值 = 闭式算术，补两台「拿渲染比渲染」
    # 仪器对共模错误的结构失明）。拆 TU 的缝 = **口径 vs 仪器内部**（四切法照
    # shadow-caster-survival）：`shadow_edge_metric.cpp` = 口径层（50% 过零点 / 世界毫米换算 /
    # 级联公式链闭式 / 闸门 C·D，被 selftest 闭式直测）；`shadow_edge_target.cpp` = manifest
    # measure 块读取 + 扫描线构造 + 两个独立实现的公式链对账；`shadow_edge_ruler.cpp` = 臂表 +
    # 采集 + 引擎回显对账 + 四道闸门 + 误差曲线；`shadow_edge_ruler_judge.cpp` = J1/J2（INFO
    # 不判）+ J3（单调性）。跨 TU 类型与量具常数唯一出处 = `shadow_edge_ruler.h`（口径常数在
    # `shadow_edge_metric.h`）。第 8 例外门（换 entities 装靶）住 `engine_profile_gates.cpp`。
    shadow_edge_metric.cpp
    shadow_edge_target.cpp
    shadow_edge_ruler_judge.cpp
    shadow_edge_ruler.cpp
    # `shadow-edge-rebase`（相对坐标批 5 第 2 刀）：四格对拍（2 工作点 × 2 开关态）的臂表 +
    # 采集 + 回显四查 + G1–G6 + J-R1/R2/R3。复用 ruler 的口径层与靶读取（`loadEdgeTarget`
    # 收 stem 参数），🔴 不抄第二份真值——闭式可量性预判 `plateMeasurableInCascade` 已抬成
    # 跨 TU 面（`shadow_edge_ruler.h`），两台仪器共用。第 9 例外门住 `engine_profile_gates.cpp`。
    shadow_edge_rebase.cpp
    shadow_edge_rebase_judge.cpp
    # `blend-sort`（债-132 结案：把刀3 那份透明排序输入接成常驻检查）。拆两 TU 的缝同上一条
    # （口径 vs 仪器）：`blend_sort_metric.cpp` 是纯度量（投影 / 合成 / 判别力 / 三条拒跑条件，
    # selftest 用合成输入闭式直接测，不启引擎、不读文件、不碰 OpenCV），`blend_sort.cpp` 是仪器
    # （复制冻结配置目录 → 跑引擎 → 取四区像素 → 比字节 → 落台账）。常数唯一出处 =
    # `blend_sort_metric.h`；显示链不在任何一份里，吃 `analytic.cpp` 的 `toBytes`。
    blend_sort_metric.cpp
    blend_sort.cpp
    # `blend-lit`（债-134 先决：透明段 lit 变体的常驻像素贡献判据）。拆 TU 的缝同上一条
    # （口径 vs 仪器）：`blend_lit_metric.cpp` 是纯度量（解析投影 → footprint、背景板铺满对账、
    # 双臂逐像素按 footprint 内外分桶；selftest 用合成两臂小图闭式直测，不启引擎、不读文件），
    # `blend_lit.cpp` 是仪器（复制冻结配置目录 → 双臂各跑一次 → 轮询 /state 读条数 → 取图 →
    # 五条判据 → 落台账）。常数唯一出处 = `blend_lit_metric.h`。与 blend-sort 的分工：那边
    # 证「画对了」（有闭式），这边证「**有**贡献」（无闭式，台账裁「透明光照的闭式成本 > 收益」）。
    blend_lit_metric.cpp
    blend_lit.cpp
    # `diffstruct`（批 15.2 子步 D1：把一次性脚本 ssr_subtract_check.py 升成常驻算子）。
    # 拆两 TU 的缝 = **口径 vs 仪器**（同 shadow_kernel_*）：`diff_struct_metric.cpp` 是纯度量
    # （selftest 用合成数组闭式直接测，不读图、不启引擎），`diff_struct.cpp` 是读图 + 报数 + 判决。
    diff_struct_metric.cpp
    diff_struct.cpp
    # `diffblock`（★0.5-b1：差异的形态判据「有没有整块」，docs/mdi_plan.md §11.2）。缝同上一条
    # （口径 vs 仪器）：`diff_block_metric.h` 是口径与常量唯一出处（纯算子声明），`diff_block.cpp`
    # 是读图 + 距离变换 + 三样输出（厚度分位 / U(r) 曲线 / 连通域）+ 判决 + JSON。selftest 直接打
    # 口径层（合成亮度图 → 读数），等价性自检钉住 distanceTransform 与字面 erode 的同语义。
    diff_block.cpp
    # main.cpp 拆 TU（护栏逼的：main.cpp 726 代码行 > 500，cmdSwim 认知 38 > 30 ERROR）。
    # 缝按**仪器**切、不按行数切：一个判据命令族一个 TU（既有先例 aa_coverage.cpp /
    # shadow_softness.cpp）；main.cpp 只留路由（main + 三条分派链 + fmt6）。
    #   · rig_attribution.cpp  = attribution / faces / switch —— 同一条
    #     `loadManifest → makeProfile → captureFrame → attribute()` 采集链的三种读法
    #   · rig_shadow_field.cpp = izb / swim —— izb_rig 影场族：同一套 deferred + shadowPass
    #     采集，两条都问"影子画在它该在的地方吗"（一个问寻址、一个问世界不变性）
    #   · probe.cpp            = probe —— 闭式解探针，消费冻结配置目录 qa/configs/analytic
    #     （与 analytic.cpp 同一条线，不吃 --glb/manifest），独立仪器
    #   · registry_read.cpp    = check / refuse —— 只读注册表、不碰引擎的命令
    # 命令函数的声明集中在 imgqa.h（本仓惯例：实现分散、声明集中，不为拆 TU 另设声明头）。
    rig_attribution.cpp
    rig_shadow_field.cpp
    probe.cpp
    registry_read.cpp)

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
