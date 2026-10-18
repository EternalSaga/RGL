-- nm_rig 的渲染图：**空表 = 内建 deferred**。
--
-- 2026-10-18 图变体收敛 §6.1-3 切 deferred（原锁 forward 的三条理由逐条作废：
-- ① 天穹——camera.lua 俯角 25° 地平线出画，且 deferred 天穹现由 env.intensity 总闸门控；
-- ② 法线量化——gbuffer_normal 是 rg16_snorm，噪声远低于本 rig 测量底；
-- ③ 探针路线——probe 已重立到 deferred 双锚）。
-- 刀② 后 variant 键删除、forward 图的 attachments/passes 副本删除（出现即被当自定义图
-- 解析），选路规则见 configs/render_graph.lua 头注。
return {}
