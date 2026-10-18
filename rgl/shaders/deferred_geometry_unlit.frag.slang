// class 3：unlit deferred 几何变体 = deferred_geometry 本体 + MATERIAL_CLASS=3。
// class 经 emissive.a 通道传给 light pass（unlit 分支：color = albedo 原样出像素，
// 无光照/无自发光相加/无阴影，对齐已删除的 forward model.frag——历史对偶）。入口 = prim.unlit
//（glTF KHR_materials_unlit，gltf_loader 读扩展存在性）——deferred 此前**没有** unlit
// 路，这类材质静默回落 deferred_geometry 被当 PBR 打光 = D1 破口（2026-10-18 探针
// 重立到 deferred 时由 aa-coverage 电平自检抓出，docs/archive/图变体收敛_plan.md §6.1-1）。
// 路由：scene.lua deferred_styles.unlit。
#define MATERIAL_CLASS 3.0
#include "deferred_geometry.frag.slang"
