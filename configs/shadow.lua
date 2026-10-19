-- configs/shadow.lua — 阴影路线选择与逐档调参（批 N6a，定案 = docs/shadow_routes_plan.md §2）
-- 全字段可选：缺文件/缺段/缺键 = 回落代码默认（= 本文件写着的这套值 = 交付形态，
-- 解析器 = rgl/shadow_config.cppm，回显见启动日志 "shadow: mode=... izb.on=..."）。
--
-- 为什么 izb 是**正交布尔**而不是 mode 枚举的第五值（shadow_routes_plan.md §2 红线 1）：
-- 「IZB 近场细影 + 远场软影」混接（evssm_plan.md 11.6e）要求两者共存 ⟹ 枚举管**软影滤波轴**
-- （none/csm/pcss/evssm 互斥），izb 管**硬影 mask**，两条轴正交。izb.on=true × mode="none" 的组合
-- 无意义，由 gate_lint 在启动期报 error（N6b 已接管），不在这里造第五个枚举值。
-- ⚠ 正交是**形状**上的判断，不代表交付该同时开：izb 现在默认 **false**，理由见下方 izb 块。
--
-- ⚠ 旧键（shadow_pass / shadow_pcss / shadow_evssm / pcss_* / izb_*）已从 diagnostics.lua
-- 删除且**出现即启动失败**（红线 2：不静默映射；fail-fast 永久保留）。N6b 起本文件是阴影
-- 门的**唯一**真相源：DiagnosticsConfig 的九个旧字段已删除，六处消费点一律经
-- shadowConfig() + shadowFormPlan()（形态决策表的唯一实现体在 rgl/shadow_config_impl.cppm，
-- 本文件不抄映射——红线 4）。
return {
    -- 软影滤波轴（四值互斥；未知值 = 启动失败并列出合法值，不静默回落——"你以为在测
    -- EVSSM 其实跑的是 CSM"是本项目最怕的失效形态）。
    --   "none"  = 无阴影（旧 shadow_pass=false）
    --   "csm"   = 级联阴影 + 4 抽头 PCF（交付形态，旧 shadow_pass=true）
    --   "pcss"  = 接触硬化软阴影（旧 shadow_pcss=true；mode 枚举形状与逐位锚 → shadow_routes_plan.md §2。
    --             旧写法「docs/phase11_plan.md 批 11.5」是断头指针：该文档没有 11.5 章节，
    --             「批 11.5」只是当年的批次号，不是文档锚点）
    --   "evssm" = 远场软影矩图（旧 shadow_evssm=true，docs/evssm_plan.md；生效性边界 → shadow_routes_plan.md §4）
    mode = "csm",

    -- 正交修饰：光空间硬影 mask（旧 izb_shadow；要求 mode ~= "none"，仅 deferred light pass 消费）。
    -- **只收子表形态**（硬切 2026-10-17：扁平布尔 = 启动报错——布尔/表双读 = 「扁平键双读
    -- 兼容 = 两个真相源」这条配置纪律登记的反模式，被嵌套未知键护栏照出后裁决删除）；
    -- 子表收编旧 izb_grid / izb_dump_frame 两键（语义原样）：
    --   on         = 开关（缺省 **false**）
    --   grid       = 光空间网格边长 G（旧 izb_grid；clamp [64, 2048]，防手滑建巨型缓冲）
    --   dump_frame = 第 N 帧（1 起）起每 30 帧回读 head 直方图打日志 + 覆盖写 izb_mask.ppm
    --                （旧 izb_dump_frame；0 = 关）——IZB 冒烟阳性锚点，N+1 帧起输出
    --
    -- ⚠ **on 从 true 改成 false（2026-10-10，用户放行换红线）**。IZB 有两个结构性缺陷，
    --    读数与复现配方 → docs/shadow_routes_plan.md §7：
    --      ① 它只覆盖 **cascade-0 光空间盒**，盒外回落 CSM ⟹ 两套算法在盒边界两侧给出不同
    --         答案，边界成了一条**随相机在世界表面爬行的线**（默认机位实测 1.76% 屏幕判反、
    --         亮度差中位 16 LSB、最高 160 LSB）。
    --      ② 在自己管辖范围内就系统性欠遮挡：`just imgqa izb` 打印的「CSM 遮而 IZB 放」
    --         = 405048 px（rig，占屏 44%）。
    --    关掉是测出来的：CSM 单独一档 `imgqa swim` 影场世界不变（零位移失配 0/63835）。
    --    开回来 = 把上面两条一起开回来，gate_lint 会 warn。
    izb = { on = false, grid = 512, dump_frame = 0 },

    -- CSM 档调参（作用在**阴影深度 pass 的光栅阶段**，与软影滤波档无关：mode 取 csm/pcss/evssm
    -- 三档都要画图集，所以三档都吃这里的键；mode = "none" 时段必空 = gate_lint 的 inertNote）。
    -- 批 N6a 的定案形状里这里是预留空块（"暂无调参键"），本批填入第一枚键；顺带旧注释里
    -- 「写了不认识的子键不报错也不生效」那句不再成立——csm 现在有 `csm.` 点分回显成员，
    -- 未知键护栏会对它下钻一层并 warn（判据 → rgl/shadow_config_impl.cppm declaresNestedKeys）。
    --   slope_bias = 硬件**斜率深度偏置**（slope-scaled depth bias）。单位 = Vulkan 的
    --     depthBiasSlopeFactor（**无量纲斜率系数**，不是深度值也不是纹素数）：光栅阶段给每个
    --     片元的深度加 offset = m·dZ/dpixel，深度测试与写入都用加完之后的值。clamp [0, 64]
    --     防手滑（口径同 pcss 两半径）；**0 = 关闭**（连 depthBiasEnable 一起关）。
    -- 治的是**陡峭面的自阴影**：dZ/dpixel 大的面上深度图纹素量化 + 4 抽头 PCF 会让这种面自己
    -- 遮自己。⚠ 但它**不是**「影子随相机漂」的解——那块门面经分辨率 ×4 探针（图集 2048→8192）
    -- 验证不随精度消失（覆盖度 0.071→0.087，不降反升）⟹ 它是真实遮挡，它随机位变是算错 → 债-139。
    -- ⚠ 与**常数** depth bias 的分工（别拿它治同一个病）：常数项给整面统一加一段容差，写大就把
    --    陡峭面与缓面一起吃掉——真影子的近处边缘也一起消失；斜率项只按表面陡峭程度加容差。
    --    实测常数项推到能压住门板（0.02）时全图阴影面积掉到 23.3%（−21%），远差于本键同收益档。
    --    常数项保持 0（RenderState::depthBiasScale 全仓无人写）。
    -- 0 = 实测定档（**关闭**；bistro 出厂场景 + 用户报障机位 + shadow_rig 合成靶，2026-10-19）。
    -- 定档必须**同时**看两列，只看左边会一路调到 16 去（本仓差点这么定，主 agent 也错过一次）：
    --   本值   两机位覆盖度差   全图阴影面积   靶上真实槽底影带存活率（d=0.30/0.24/0.20/0.17/0.14 m）
    --     0        0.069          29.6%        1.00/1.00/1.00/1.00/1.00   ← 出厂
    --     4        0.060          28.6%        1.00/1.00/1.00/1.00/0.00
    --     8        0.057          28.0%        1.00/1.40/0.00/0.00/0.00
    --    16        0.017          26.8%        0.00/0.00/0.00/0.00/0.00
    -- ⟹ **本键的收益区间与它的真影子损失区间重合**：任何非零值都会把某个量级的真实遮挡带整条
    -- 抹光（0.00 = 抹光，不是变淡），而稳定性收益只有 17%（0→8）——拿 17% 换掉一档真实遮挡不
    -- 划算。🔴 护栏 = `imgqa shadow-grain` J1 就是守这张表的，把本值调大它**立刻红**并点名
    -- 被抹光的那条缝；改本值前先跑它。逐字段语义与接线史 → rgl/shadow_config.cppm 的
    -- ShadowCsmTuning；⚠ 接线坑（vkCmdSetDepthBias 的形参顺序把 slope 送进过 clamp 位）
    -- → rgl/shader.cppm applyDynamicState 的 setDepthBias 注释。
    --   force_cascade = **强制选级诊断键**（不是画质旋钮！）。-1 = 关（出厂，逐位不变）；
    --     0..3 = 把**全场景**钉到该级采样。clamp [-1, 7] 防手滑（上限 7 与 slope_bias 的 64
    --     同性质：只是"手滑多写一个 0"的闸门，不代表 7 处有语义；真射程 = 生效级数，判据在
    --     消费函数 forceShadowCascadeSplits 里，越界原样返回 = 等同关）。
    --   为什么只改四个数就能改选级：采样端选级是**三段无条件比较**
    --     （shadow.slang 的 `if (depth > splits.x) cascade = 1u;` 那三行），把 splits 覆写成
    --     `splits[i] = (i < k) ? 0 : 1e9` 就把每个片元都推到第 k 级——shader 一个字不用动。
    --   存在的理由（债-139）：量"相邻两级对同一块表面的分歧有多大"。屏幕空间统计量会被无关的
    --     真影边淹掉（实测分不开），唯一无偏仪器 = 钉级 c 渲一张、钉级 c+1 渲一张做差分。
    --   🔴 只覆写**写进 scene data 的那份** splits：CPU 侧算级联球/切片的 cascadeSplitDistance
    --     照常 ⟹ 几何是真实几何，两次运行只有"从哪一级取遮挡"这一个变量。
    --   ⚠ 钉级后的画面是**错的**（整屏用一级覆盖，出块旁路整片没影子），只准拿来出差分图。
    csm = { slope_bias = 0.0, force_cascade = -1 },

    -- PCSS 档调参（clamp 口径 = 旧 pcss_* 三键原样：sun [0,5]、两半径 [1,64] 成本护栏）
    -- ⚠ 本档**没有** min_footprint_texels：那是 EVSSM 矩滤波专用件（PCSS 的半径由 Δz 现算，
    --    没有"核窄到 0.5 纹素"这个失效），写在这里任何 mode 下都不生效——gate_lint 直接
    --    warn，不靠注释自觉。
    pcss  = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0 },

    -- EVSSM 档调参（**按档分立**——消掉旧门「shadow_pcss 与 shadow_evssm 共用一套 pcss_*」
    -- 的债-16⑤；两档同值只是出厂巧合，改一档不许串到另一档）
    --   min_footprint_texels = 滤波核**宽度下限**（图集纹素；0 = 不设下限）。出厂 0.53° 下
    --    物理半影 < 1 纹素 ⟹ 抽点半径 0.5 纹素 = 5 抽点全落同一个双线性邻域 ⟹ EVSSM 退化成
    --    「单纹素深度比较」= CSM 去掉 PCF（实测太阳角敏感度恰好 0.000%）。
    -- ⚠ 16 = 实测**唯一贴上 CSM 形态**的点（带核比 1.000、每影像素深度 50 vs CSM 52 LSB）。
    --    核宽是「覆盖 ↔ 不透明度」的单调权衡，不是越宽越好：foot=32 覆盖率比涨到 .845 但黑度比
    --    掉到 .608 = 把影冲淡。**改这个数前先看 rgl/shaders/shadow.slang STEP 2.5 的权衡表**。
    -- ⚠ 曾有第二键 coherence_texels（跨断崖抽点一致性闸门），实测**净负作用**（foot=32 时
    --    覆盖率比 0.845→0.526：被剔的抽点恰是能提供遮挡的那些）已删。归因 →
    --    docs/shadow_routes_plan.md §4.2。
    evssm = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0,
              min_footprint_texels = 16.0 },
}
