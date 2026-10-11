// tinygltf 的 C++ API 实现 TU（rgl/tinygltf_impl.cpp 同法）：vcpkg port 只装头文件，
// 实现体在 TINYGLTF_IMPLEMENTATION 宏守卫内，本工具自组一个实现翻译单元。
// stb 断连：本工具只要几何，像素一个字节都不碰（glb_source.cpp 同定义，两 TU 面一致）。
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
