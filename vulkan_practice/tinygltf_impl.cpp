// tinygltf 的 C++ API 实现 TU（同 vam_impl.cpp 模式）：
// v3 把实现体从 header-only 移到 TINYGLTF_IMPLEMENTATION 宏守卫内，
// vcpkg port 只装头文件不装 tiny_gltf.cc，此处自组实现翻译单元。
// NO_STB_IMAGE：不解码纹理（Phase 2 flat unlit），且 port 未安装 stb_image.h。
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
