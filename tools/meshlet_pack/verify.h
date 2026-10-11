#pragma once
//
// verify.h —— 产物自检（--verify）。检查项与变异红线的说明全文在 verify.cpp 头注释。
//
#include <cstdio>
#include <vector>

#include "packer.h"

namespace RGL::meshpack {

// 跑全部自检并逐项打印 PASS/FAIL；全绿返回 true。只读 asset/prims，不改任何状态。
bool verifyAsset(const PackedAsset& asset, const std::vector<SourcePrim>& prims, std::FILE* out);

}  // namespace RGL::meshpack
