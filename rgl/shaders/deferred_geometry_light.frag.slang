// Phase 8 批次 F：光源 deferred 几何变体 = deferred_geometry 本体 + MATERIAL_CLASS=2。
// class 经 emissive.a 通道传给 light pass（光源分支：emission×1/4 还原 + 金属 0，
// 对齐 forward model_light 暗绿玻璃）。路由：scene.lua material_styles/deferred_styles。
#define MATERIAL_CLASS 2.0
#include "deferred_geometry.frag.slang"
