-- 探针配置（qa/configs/analytic）的渲染图：**空表 = 内建 deferred**。
--
-- 2026-10-18 探针重立（docs/archive/图变体收敛_plan.md §6.1-1）：闭式解探针走 deferred 分段双锚
--（gbuffer_albedo/metalrough 编码锚 + 最终像素锚，量化建模见 tools/imgqa/analytic.cpp）。
-- 本目录同时是 imgqa 合成 profile 的模板（engine_profile.cpp）⟹ 内建 deferred 就是
-- 全部合成判据命令的默认路线。
-- 刀② 前这里写的是 variant = "forward" + forward 图的 attachments/passes 逐字副本；
-- variant 键已删除（出现即启动报错），选路规则见 configs/render_graph.lua 头注。
return {}
