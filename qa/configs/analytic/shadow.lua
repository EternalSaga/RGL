-- 分析探针的阴影门：**整条阴影路关掉**（原 diagnostics.lua 的 shadow_pass = false）。
--
-- 为什么必须显式写在这里（而不是"缺文件回落默认"）：缺 shadow.lua = 回落**交付形态**
-- mode=csm（izb 现已关，但 csm 本身就画影）= 有影，而探针要的是 sampleShadow1Tap 恒 1.0
-- （无遮挡）。整目录 --config 用法下这个回落是**静默换臂**——画面看着"合理"，闭式解对不上才发现。
--
-- ⚠ 两件事都要写，只关一件不够：
--   mode = "none" → 图首 shadow0..3 段零 draw + 采样端 enabled=0 恒全亮（旧 shadow_pass）
--   izb  = false  → 不构造 IZB、light 条目不选 IZB_CONSUME 变体（旧 izb_shadow）。
--     本目录走内建 deferred 图（刀② 起），但 IZB 消费变体同样换被测 frag SPV
--     ⟹ izb 开着既可能触发 gate_lint 启动期 error，又会把 frag 换成 izb 变体（换 SPV = 换
--     被测对象）。交付默认 2026-10-10 已翻成 false（docs/shadow_routes_plan.md §7），这一条
--     看着"能靠默认兜住"了——**别删**：探针的语义要的是"我关掉了它"，不是"别人恰好关着"。
--
-- 键名口径提醒（analytic 探针踩过的坑还在）：键名写错 = 解析器不读 = 静默用默认值 =
-- 在默认值上下判据。旧键（shadow_pass / izb_shadow / pcss_*）现在出现在**任何**配置文件
-- 里都会立刻报错或告警，不再静默。
return {
    mode = "none",
    izb = { on = false },
}
