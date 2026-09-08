// tinygltf 的 C++ API 实现 TU（同 vam_impl.cpp 模式）：
// v3 把实现体从 header-only 移到 TINYGLTF_IMPLEMENTATION 宏守卫内，
// vcpkg port 只装头文件不装 tiny_gltf.cc，此处自组实现翻译单元。
// stb_image 解码已启用（Phase 3 纹理：vcpkg stb port 装有 stb_image.h，
// 实现体在 stb_impl.cpp 的 STB_IMAGE_IMPLEMENTATION）；
// NO_STB_IMAGE_WRITE 保留（stb 写图实现体同在 stb_impl.cpp，llm_debug PNG 输出用）。
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
