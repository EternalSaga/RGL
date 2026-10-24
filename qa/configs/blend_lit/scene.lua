-- 透明段 lit 变体的常驻像素覆盖靶（债-134 先决；边界全文 → docs/透明混合管线_plan.md §4）。
--
-- 与同目录 blend_sort 的**根本差别**：那边三层全 unlit ⟹ 着色链塌成可闭式的一条式子；
-- 这边**故意让透明段吃真光照**（env + 直射光都不归零），因为本靶要证的正是「透明段对
-- 交付图有贡献」——把光照关掉就只证到「混合公式在动」，证不到「这条 lit 前向路在画」。
-- 代价 = 没有像素值预测（透明光照的闭式要复刻 BRDF + IBL + CSM，台账裁「成本 > 收益」），
-- 判据因此是弱判据：透明段有 draw + VVL 干净 + 交付图相对 OPAQUE 臂变了。
--
-- 每个键都标了**承重**还是**双保险**。承重的那条删了判据就失效，双保险的只是替将来兜底。
return {
    entities = {
        -- BLEND 臂。反向臂 models/blend_lit_opaque.glb 由 imgqa 侧改写本键切过去
        -- （两臂同源 = tools/qa_scenes.py 同一个 scene_blend_lit(blend=)，只差 alphaMode）。
        { type = "model", file = "models/blend_lit.glb" },
    },

    -- ── 环境段：既是硬前置又是被测对象本身 ──────────────────────────────────────
    -- 硬前置：场景含 alphaMode="BLEND" 图元而 environment 缺失 ⟹ 装配期
    -- validateBlendSupport 直接抛（透明前向 frag 要采 env cubemap 的 binding 4/5，
    -- 没有 env 资产烘不出 mapping，Vulkan 在建 shader 时报 VUID-VkShaderCreateInfoEXT-flags-11292）。
    -- 与 blend_sort 相反：那边 intensity=0 是「只要它存在」，这边 intensity≠0 是**载荷**
    -- ——env 归零则 IBL 项整条消失，「透明段在吃光照」这件事就退化成只剩直射光。
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
        intensity = 1.0,        -- 🔴 承重：见上
    },

    -- 直射光同样不归零（同上，本靶的「lit」就是这两项）。方向取斜上方而非 blend_sort 的
    -- 正下方：pane 是 XY 平面、法线 +Z，direction=(0,-1,0) 与法线垂直 ⟹ N·L=0，直射项
    -- 恒 0，「光照开着」会变成一句空话。这里给一个真正打到正面的方向。
    light = {
        direction = { -0.3, -0.5, -0.8 },
        color = { 1.0, 1.0, 1.0 },
        intensity = 3.0,        -- 🔴 承重：0 = 直射项消失
        ambient = { 0.0, 0.0, 0.0 },
    },

    spin = false,               -- 🔴 承重：转起来 ⟹ pane 的屏幕 footprint 逐帧漂，
                                -- 「这块像素属于哪个 pane」的区域归因作废
    cube = false,
    exposure = 1.0,             -- 双保险：本靶不预测像素值，中性曝光只是少一个未知量，
                                -- 让「双臂像素差」的读数跨重放稳定
    -- AO 关掉。注意它**不是**恒等旁路问题（blend_sort 那边才是）：ao.strength 在 tonemap
    -- pass 里乘全部像素、两臂同乘，不会让「双臂不同」变成假阳。这里关它是为了把读数
    -- 收窄到「透明段自己写的那部分」，并让反向臂（OPAQUE）的像素与透明臂的差只来自混合。
    ao = { strength = 0.0 },

    -- 材质风格两张表**故意不写**：缺省 = 吃 C++ 默认集（scene_config_impl 的
    -- deferredStyleMap / transparentStyleMap + material_rules 的 Emissive→light、
    -- Foliage→vegetation）。本靶三支 pane 的名字就是靠那两条 pattern 路由出
    -- transparent / transparent_vegetation / transparent_light 三支 SPV 的；在这里重抄
    -- 一遍 = 开第二份副本，将来默认表加风格本靶该跟着走却不会跟。
}
