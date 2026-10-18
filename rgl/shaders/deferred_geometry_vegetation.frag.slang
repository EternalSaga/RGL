// Phase 8 批次 F：植被 deferred 几何变体 = deferred_geometry 本体 + MATERIAL_CLASS=1。
// class 经 emissive.a 通道传给 light pass（植被分支：half-Lambert wrap + 背光透射 +
// envSpec×0.2，对齐已删除的 forward model_vegetation——历史对偶）。路由：scene.lua material_styles/deferred_styles。
#define MATERIAL_CLASS 1.0
#include "deferred_geometry.frag.slang"
