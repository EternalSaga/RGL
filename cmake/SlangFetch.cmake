# ─── Slang 编译器：FetchContent 拉取官方预构建（替代 vcpkg shader-slang port）────────────────
# vcpkg shader-slang port 停在 2026.7.1（上游 master 亦同，Slang 月更，落后 9+ release），
# 见 docs/heap_texture_research.md §九.4。此处 pin 版本 + sha256 下载 Slang 官方
# GitHub release 的 Linux 预构建包（解压到 build/_deps/slang-prebuilt-src/，bin/slangc）。
#
# 用法：include(cmake/SlangFetch.cmake) 后调用 rgl_fetch_slang()
#   → 设置 SLANGC_EXECUTABLE（编译器路径）与 SLANG_PREBUILT_DIR（解压根目录）
#
# 升级：改 SLANG_VERSION 后从 release 页取新资产 sha256sum 回填 SLANG_SHA256。
#   资产命名：slang-<ver>-linux-x86_64.tar.gz（glibc 老系统换 -glibc-2.27 变体）。

include(FetchContent)

function(rgl_fetch_slang)
    # $ENV{SLANGC_DIR} 显式覆盖（试别版本/离线；同 spike CMakeLists 模式，刻意不查系统
    # PATH——vcpkg 旧 shader-slang 2026.7.1 残留会被误拾，破坏 pin 意义）
    if(DEFINED ENV{SLANGC_DIR})
        find_program(SLANGC_DIR_EXECUTABLE slangc
            HINTS "$ENV{SLANGC_DIR}"
            NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH)
        if(SLANGC_DIR_EXECUTABLE)
            message(STATUS "slangc (SLANGC_DIR 覆盖): ${SLANGC_DIR_EXECUTABLE}")
            set(SLANGC_EXECUTABLE "${SLANGC_DIR_EXECUTABLE}" PARENT_SCOPE)
            set(SLANG_PREBUILT_DIR "" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Windows: 优先使用 Vulkan SDK 自带的 slangc
    if(WIN32)
        find_program(VULKAN_SDK_SLANGC slangc
            HINTS "$ENV{VULKAN_SDK}/Bin"
            NO_DEFAULT_PATH)
        if(VULKAN_SDK_SLANGC)
            message(STATUS "slangc (Vulkan SDK): ${VULKAN_SDK_SLANGC}")
            set(SLANGC_EXECUTABLE "${VULKAN_SDK_SLANGC}" PARENT_SCOPE)
            set(SLANG_PREBUILT_DIR "" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Linux: FetchContent 拉取官方预构建
    set(SLANG_VERSION "2026.16")
    set(SLANG_URL "https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-linux-x86_64.tar.gz")
    set(SLANG_SHA256 "e572e1a293f21971d8a98124c4576848ad4cd6a19b922ce5bad784fee0950648")

    FetchContent_Declare(slang-prebuilt
        URL "${SLANG_URL}"
        URL_HASH "SHA256=${SLANG_SHA256}"
    )
    FetchContent_Populate(slang-prebuilt)

    set(SLANG_PREBUILT_DIR "${slang-prebuilt_SOURCE_DIR}" PARENT_SCOPE)
    set(SLANGC_EXECUTABLE "${slang-prebuilt_SOURCE_DIR}/bin/slangc" PARENT_SCOPE)
endfunction()
