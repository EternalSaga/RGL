-- 透明排序靶场景（透明混合管线刀3，docs/透明混合管线_plan.md §3）。
--
-- 三层全 unlit ⟹ 着色链塌成「baseColor → 非预乘 over 混合 → 曝光 → ACES → sRGB」。
-- 下面每个键都标了它是**承重**还是**双保险**——承重的那条删了判据就错，双保险的只是
-- 让"哪天有人把 unlit 改成采样光照"时不会静默破口。
--
-- 环境段是**硬前置**（不是可选项）：场景含 alphaMode="BLEND" 图元而 environment 缺失
-- ⟹ 装配期 validateBlendSupport 直接抛（透明前向 frag 要采 env cubemap 的 binding 4/5，
-- 没有 env 资产烘不出 mapping，Vulkan 在建 shader 时报 VUID-VkShaderCreateInfoEXT-flags-11292）。
-- 本靶的 env 只贡献"存在"这一件事：intensity=0 + 三层全 unlit（早退不采 env）⟹ 对像素零影响。
return {
    entities = {
        { type = "model", file = "models/blend_sort.glb" },
    },
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
        intensity = 0.0,        -- 双保险（unlit 早退本就不采 env）
    },
    light = {
        direction = { 0.0, -1.0, 0.0 },
        color = { 1.0, 1.0, 1.0 },
        intensity = 0.0,        -- 双保险
        ambient = { 0.0, 0.0, 0.0 },
    },
    spin = false,               -- 🔴 承重：转起来 ⟹ 四个判据区的像素坐标逐帧漂，闭式作废
    cube = false,
    exposure = 1.0,             -- 🔴 承重：ACES 前的线性倍率取中性，闭式里少一个未知量
    ao = { strength = 0.0 },    -- 🔴 承重：AO 在 tonemap pass 里乘**全部**像素（含透明段
                                -- 写的那部分，透明排在 ao 之前），strength=0 才是位级恒等
    -- 材质风格两张表**故意不写**：缺省 = 吃 C++ 默认集（scene_config_impl 的
    -- deferredStyleMap / transparentStyleMap 都带 unlit 格）⟹ unlit 不透明走
    -- deferred_geometry_unlit、unlit 半透走 transparent_unlit。在这里重抄一遍 = 开第二份
    -- 副本，将来默认表加风格本靶该跟着走却不会跟。
}
