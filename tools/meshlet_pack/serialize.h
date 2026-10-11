#pragma once
//
// serialize.h —— 内存态产物 → 二进制文件（布局契约 = format.h，唯一真相源）。
//
#include <string>

#include "packer.h"

namespace RGL::meshpack {

// 写文件；失败抛 std::runtime_error。
void writeAsset(const std::string& path, const PackedAsset& asset);

}  // namespace RGL::meshpack
