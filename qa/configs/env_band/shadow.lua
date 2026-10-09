-- 变体 1「带边」探针的阴影门：**整条阴影路关掉**（原 diagnostics.lua 的 shadow_pass /
-- shadow_pcss / shadow_evssm / izb_* 六键）。
--
-- 关阴影 + 关 IZB 的理由（原 diagnostics.lua 头注，随键一起搬来）：本探针唯一的激励是
-- env 镜面项（light.intensity=0），阴影因子应当恒 1；留着阴影段只是多两个 pass 和一条
-- "阴影走样"的潜在噪点来源。bistro 那侧的消融已实测"阴影+IZB 关掉白点 49→30"，说明在
-- bistro 上阴影确有贡献——但那是**直射光开着**的场景；本探针直射光已归零，混淆源移除。
--
-- ⚠ 为什么两件事都得写：mode="none" 关图集与采样（旧 shadow_pass），izb=false 关 IZB 与
--   其 frag 变体（旧 izb_shadow）。本目录 render_graph.lua 是 **forward** 图而 IZB 只接
--   deferred light pass ⟹ izb 开着会撞 gate_lint 启动期 error；且 N6a 起 izb 的代码默认
--   是 true，"没写"不等于"关"。
--
-- ⚠ 缺 shadow.lua 不是"没阴影"而是**回落交付形态 = 有影**（整目录 --config 用法下这是
--   静默换臂）。键名写错同理——本项目最怕"配置写了、什么都没发生"。
return {
    mode = "none",
    izb = false,
}
