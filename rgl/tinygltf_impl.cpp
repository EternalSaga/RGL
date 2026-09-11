// tinygltf 的 C++ API 实现 TU（同 vam_impl.cpp 模式）：
// v3 把实现体从 header-only 移到 TINYGLTF_IMPLEMENTATION 宏守卫内，
// vcpkg port 只装头文件不装 tiny_gltf.cc，此处自组实现翻译单元。
// stb 全面断连（2026-09-11，图像编解码迁 image_codec 模块）：NO_STB_IMAGE +
// NO_STB_IMAGE_WRITE（gltf_loader_impl.cppm 同定义，两 TU API 面一致）——tinygltf
// 不再解码像素（本项目纹理走 bufferView 原始字节 → texture_pool，见其 SetImageLoader）。
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
