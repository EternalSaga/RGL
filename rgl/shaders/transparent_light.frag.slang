// 透明光源变体 = transparent.frag 本体 + MATERIAL_CLASS=2（强制电介质 + 自发光 ×1/4 还原
// strength 预乘，与 opaque 侧 deferred_geometry_light / light.frag 的 isLamp 分支同值）。
// 路由：scene.lua transparent_styles.light。
#define MATERIAL_CLASS 2.0
#include "transparent.frag.slang"
