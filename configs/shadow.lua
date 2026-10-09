-- configs/shadow.lua — 阴影路线选择与逐档调参（批 N6a，定案 = docs/shadow_routes_plan.md §2）
-- 全字段可选：缺文件/缺段/缺键 = 回落代码默认（= 本文件写着的这套值 = 交付形态，
-- 解析器 = rgl/shadow_config.cppm，回显见启动日志 "shadow: mode=... izb=..."）。
--
-- 为什么 izb 是**正交布尔**而不是 mode 枚举的第五值（shadow_routes_plan.md §2 红线 1）：
-- 交付路径就是 csm + izb 同时开（红线 A 那张图），且「IZB 近场细影 + 远场软影」混接
-- （evssm_plan.md 11.6e）要求两者共存 ⟹ 枚举管**软影滤波轴**（none/csm/pcss/evssm 互斥），
-- izb 管**硬影 mask**，两条轴正交。izb=true × mode="none" 的组合无意义，由 gate_lint 在
-- 启动期报 error（N6b 已接管），不在这里造第五个枚举值。
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
    --   "pcss"  = 接触硬化软阴影（旧 shadow_pcss=true，docs/phase11_plan.md 批 11.5）
    --   "evssm" = 远场软影矩图（旧 shadow_evssm=true，docs/evssm_plan.md；生效性边界 → shadow_routes_plan.md §4）
    mode = "csm",

    -- 正交修饰：光空间硬影 mask（旧 izb_shadow；要求 mode ~= "none"，仅 deferred light pass 消费）。
    -- 布尔简写 = 只开关；子表形态收编旧 izb_grid / izb_dump_frame 两键（语义原样）：
    --   on         = 开关（缺省 true）
    --   grid       = 光空间网格边长 G（旧 izb_grid；clamp [64, 2048]，防手滑建巨型缓冲）
    --   dump_frame = 第 N 帧（1 起）起每 30 帧回读 head 直方图打日志 + 覆盖写 izb_mask.ppm
    --                （旧 izb_dump_frame；0 = 关）——IZB 冒烟阳性锚点，N+1 帧起输出
    izb = { on = true, grid = 512, dump_frame = 0 },

    -- CSM 档暂无调参键（定案形状里的预留块，留着是为了加键时不改形状；写了不认识的
    -- 子键不报错也不生效——未知键 lint 只到顶层，同 diagnostics 口径）
    csm = { },

    -- PCSS 档调参（clamp 口径 = 旧 pcss_* 三键原样：sun [0,5]、两半径 [1,64] 成本护栏）
    pcss  = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0 },

    -- EVSSM 档调参（**按档分立**——消掉旧门「shadow_pcss 与 shadow_evssm 共用一套 pcss_*」
    -- 的债-16⑤；两档同值只是出厂巧合，改一档不许串到另一档）
    evssm = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0 },
}
