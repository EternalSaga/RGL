#pragma once
//
// stats.h —— --stats 统计表打印（与 docs/mdi_plan.md §7.2 逐项对拍的读数面）+
// UV 手性实测（★1 前置读数，钉住 format.h 的 tangent.w 契约，不进产物）。
// 打印口径 = §7.2 各行；数字变了先怀疑 meshoptimizer 版本（PATCHES.md 升级流程第 3 步）。
//
#include <cstdint>
#include <cstdio>
#include <vector>

#include "glb_source.h"
#include "packer.h"

namespace RGL::meshpack {

void printStats(const PackedAsset& asset, const std::vector<SourcePrim>& prims, const char* sourcePath, std::FILE* out);

}  // namespace RGL::meshpack
