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
# （幂等：已是链且指向系统 = 跳过；vcpkg 重装回真文件 = 再次自愈；**断链也重建**——
#  见下方 GLOB 那条注释，这条路径 2026-10-16 之前是漏的）。
# 前置：find_package(Vulkan) 已执行（本文件由 rgl/CMakeLists.txt 在其后 include）。

if(WIN32)
    return()  # Windows loader 来自 Vulkan SDK 安装，无此劫持形态
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()
set(_vk_guard_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")

# 🔴 判「vcpkg 有没有装 loader」必须用 GLOB，**不能用 EXISTS**：EXISTS 会跟随符号链接，
# 一条**指空**的链接（loader 升级后的形态）与「根本没有 loader」返回同样的 false。2026-10-16
# 实测：正因为写成 EXISTS，本守卫在唯一需要它出手的场景下走了"无 loader，跳过"分支，
# 于是钉死版本号的断链一直断到有人手动重配为止。GLOB 匹配的是**目录项名**，与目标存在与否无关。
file(GLOB _vk_files "${_vk_guard_dir}/libvulkan.so*")
if(NOT _vk_files)
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

# 🔴 链接目标用**稳定 soname**（`libvulkan.so.1`），不用 REAL_PATH 解出来的带版本号实体。
# 2026-10-16 实测的复发形态：本机发行版把 loader 从 1.4.357 升到 1.4.363，本守卫 10-05 建的
# `libvulkan.so.1 -> /usr/lib64/libvulkan.so.1.4.357` 当场指空，而 `just build` **不重跑
# configure** ⟹ 本守卫没有机会自愈，ninja 只留下一句天书
# `error: 'vcpkg_installed/x64-linux-native/lib/libvulkan.so', missing and no known rule to make it`。
# 指向 soname 后 loader 升级由发行版自己挪那条链，本仓永不悬空。
get_filename_component(_vk_system_dir "${RGL_SYSTEM_VULKAN_LOADER}" DIRECTORY)
if(EXISTS "${_vk_system_dir}/libvulkan.so.1")
    set(_vk_link_target "${_vk_system_dir}/libvulkan.so.1")
else()
    set(_vk_link_target "${_vk_system_real}")  # 发行版没提供 soname 软链，退回实体
endif()

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

foreach(_f IN LISTS _vk_files)
    file(REMOVE "${_f}")
endforeach()
file(CREATE_LINK "${_vk_link_target}" "${_vk_guard_dir}/libvulkan.so.1" SYMBOLIC)
file(CREATE_LINK "libvulkan.so.1" "${_vk_guard_dir}/libvulkan.so" SYMBOLIC)
message(STATUS
    "RglVulkanLoaderGuard: vcpkg_installed loader 已替换为系统 loader 符号链接 → ${_vk_link_target}"
    "（实体 ${_vk_system_real}）")
