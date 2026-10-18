-- ssr_rig 的渲染图：**空表 = 内建 deferred**。
--
-- SSR 是 deferred-only（D1 对等已豁免，`docs/phase15_plan.md` §0.1）——靶没有 G-buffer
-- 就没有 SSR 落点，症状是"SSR 开关画面纹丝不动"，最难归因的一种失效。
-- 2026-10-18 刀② 起「缺文件 = forward」的旧缺省没了（缺文件/空表都 = 内建 deferred），
-- 本文件只剩**消音 + 立字据**两个作用：省掉引擎每跑一次的 "config not found" warn，
-- 并把"本靶必须 deferred"写成文件而不是口头。
return {}
