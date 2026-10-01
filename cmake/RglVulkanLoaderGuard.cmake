# ─── Vulkan loader 守卫（2026-10-02 「删 build 重建」事故复盘）─────────────────
# 症状：窗口模式 SDL 报 "Installed Vulkan doesn't implement VK_KHR_wayland_surface"
# （headless 模式因 headless_surface 两边都有而免疫——所以测试/冒烟全绿、只有直接跑炸）。
# 根因：vcpkg 的 vulkan port 在某些解析路径下会把自己构建的 loader 装进
#   <build>/vcpkg_installed/<triplet>/lib/libvulkan.so.1.*，而 rgl 二进制的 RUNPATH
#   就指向该目录 → 运行时整个进程（含 SDL 的同进程 loader 查询）被这个
#   feature-reduced loader 劫持（实测只有 headless_surface，无 wayland/xlib/xcb）。
# 定论：**Vulkan loader 必须是系统（发行版）那份**——winsys 支持与 ICD/layer 搜索
#   配置由发行版编译期决定，且要与系统 VVL 的 loader 接口协商配套；vcpkg 只管头。
# 本守卫在 configure 期把 vcpkg_installed 里的 loader 替换成系统 loader 的符号链接
# （幂等：已是链且指向系统 = 跳过；vcpkg 重装回真文件 = 再次自愈）。
# 前置：find_package(Vulkan) 已执行（本文件由 rgl/CMakeLists.txt 在其后 include）。

if(WIN32)
    return()  # Windows loader 来自 Vulkan SDK 安装，无此劫持形态
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()
set(_vk_guard_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")

if(NOT EXISTS "${_vk_guard_dir}/libvulkan.so.1" AND NOT EXISTS "${_vk_guard_dir}/libvulkan.so")
    message(STATUS "RglVulkanLoaderGuard: vcpkg 内无 loader，跳过（dir=${_vk_guard_dir}）")
    return()  # vcpkg 没装 loader（常态），无需守卫
endif()

# 系统 loader：显式 PATHS + NO_DEFAULT_PATH，防被 CMAKE_PREFIX_PATH（vcpkg 污染）反噬
find_library(RGL_SYSTEM_VULKAN_LOADER NAMES vulkan
    PATHS /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib /lib64
    NO_DEFAULT_PATH)
if(NOT RGL_SYSTEM_VULKAN_LOADER)
    message(WARNING
        "RglVulkanLoaderGuard: 未找到系统 Vulkan loader，vcpkg_installed 内的 loader 未守卫——"
        "窗口模式可能报 wayland_surface 缺失（headless 不受影响）")
    return()
endif()
file(REAL_PATH "${RGL_SYSTEM_VULKAN_LOADER}" _vk_system_real)

# 已指向系统 loader = 幂等跳过（EXISTS 对断链为 false；注：本机 cmake 4.3.4 的
# file(REAL_PATH ... BASE_DIR RESULTS_MISSING_FILE) 形态报 unexpected arguments，
# 用 EXISTS 前置替代）
set(_vk_cur_real "")
if(EXISTS "${_vk_guard_dir}/libvulkan.so.1")
    file(REAL_PATH "${_vk_guard_dir}/libvulkan.so.1" _vk_cur_real)
endif()
if(_vk_cur_real STREQUAL _vk_system_real)
    return()
endif()

file(GLOB _vk_files "${_vk_guard_dir}/libvulkan.so*")
foreach(_f IN LISTS _vk_files)
    file(REMOVE "${_f}")
endforeach()
file(CREATE_LINK "${_vk_system_real}" "${_vk_guard_dir}/libvulkan.so.1" SYMBOLIC)
file(CREATE_LINK "libvulkan.so.1" "${_vk_guard_dir}/libvulkan.so" SYMBOLIC)
message(STATUS
    "RglVulkanLoaderGuard: vcpkg_installed loader 已替换为系统 loader 符号链接 → ${_vk_system_real}")
