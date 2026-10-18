// 透明 unlit 变体 = transparent.frag 本体 + MATERIAL_CLASS=3（baseColor 原样出像素，
// 无光照/无自发光相加/无阴影，与 opaque 侧 deferred_geometry_unlit 同一类别语义）。
// 入口 = prim.unlit（glTF KHR_materials_unlit）。KHR_materials_unlit + alphaMode="BLEND"
// 是 glTF 里很常见的一组（粒子、贴图玻璃、UI 片），缺这格 = 这类资产在透明段被静默打光，
// 正是 2026-10-18 探针重立到 deferred 时 aa-coverage 电平自检抓出的那类 D1 破口。
// 路由：scene.lua transparent_styles.unlit。
#define MATERIAL_CLASS 3.0
#include "transparent.frag.slang"
