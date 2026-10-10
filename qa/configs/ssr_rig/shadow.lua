-- ssr_rig 靶的阴影门：整条阴影路关掉。
--
-- 柱是不透明的，开阴影就会在倒影区里投一块影。影只会让该区**更暗**，理论上不污染
-- "SSR 让它变亮"的判据；但靶的原则是"不留任何需要解释的像素"，且 analytic 探针的
-- 同款教训是：缺 shadow.lua = 回落**交付形态** mode=csm = 有影，整目录 --config 用法下
-- 这是**静默换臂**。所以显式写死，不靠默认兜。
-- 两件事都要写：mode="none"（图首 shadow0..3 零 draw + 采样端恒全亮）、izb=false。
return {
    mode = "none",
    izb = false,
}
