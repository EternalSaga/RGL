-- ssr_rig 靶的渲染图：**必须显式 deferred**。
--
-- 缺本文件 = `variant` 缺省 = **forward**，而 SSR 是 deferred-only（D1 对等已豁免，
-- `docs/phase15_plan.md` §0.1）——靶在 forward 下没有 G-buffer，SSR 根本没有落点，
-- 症状是"SSR 开关画面纹丝不动"，最难归因的一种失效。
-- deferred 是内建图（exclusive），故本文件只需要 variant 一个键。
return {
    variant = "deferred",
}
