// 透明植被变体 = transparent.frag 本体 + MATERIAL_CLASS=1（half-Lambert wrap + 背光透射 +
// env 镜面归零，与 opaque 侧 deferred_geometry_vegetation 同一类别语义）。
// 存在的理由 = blend 与类别是**两个独立维度**：grass_variations.glb 的材质名是 Foliage_*
// 而 alphaMode 全 BLEND，只给一支 PBR 透明的话，"是透明体"会吃掉"是植被"（反之亦然）。
// 路由：scene.lua transparent_styles.vegetation（默认值即本规格）。
#define MATERIAL_CLASS 1.0
#include "transparent.frag.slang"
