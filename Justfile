#!/usr/bin/env just

# RGL Project — Gentoo build recipes

WORKSPACE := `echo ${ZED_WORKTREE_ROOT:-$PWD}`
VCPKG := "/home/robin/program/vcpkg"
BUILD := WORKSPACE / "build"

# 基础 CMake 配置命令
# 用法:
#   just config             -> clang, Release
#   just config gcc         -> gcc, Release
#   just config Debug       -> clang, Debug
#   just config clang Debug -> clang, Debug
#   just config gcc Debug   -> gcc, Debug
config COMPILER="clang" BUILD_TYPE="Release":
    @COMPILER="{{ COMPILER }}"; BUILD_TYPE="{{ BUILD_TYPE }}"; \
    if [ "$COMPILER" = "Debug" ] || [ "$COMPILER" = "Release" ] || [ "$COMPILER" = "RelWithDebInfo" ] || [ "$COMPILER" = "MinSizeRel" ]; then \
        BUILD_TYPE="$COMPILER"; COMPILER="clang"; \
    fi; \
    if [ "$BUILD_TYPE" = "clang" ] || [ "$BUILD_TYPE" = "gcc" ]; then \
        TEMP="$BUILD_TYPE"; BUILD_TYPE="$COMPILER"; COMPILER="$TEMP"; \
    fi; \
    echo "=== CMake Configure ($BUILD_TYPE, $COMPILER) ==="; \
    CC="$COMPILER"; if [ "$COMPILER" = "gcc" ]; then CC="gcc"; else CC="clang"; fi; \
    CXX="$COMPILER"; if [ "$COMPILER" = "gcc" ]; then CXX="g++"; else CXX="clang++"; fi; \
    env \
        "CC=$CC" "CXX=$CXX" \
        cmake -S {{ WORKSPACE }} \
              -B {{ BUILD }} \
              -G Ninja \
              -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
              -DCMAKE_TOOLCHAIN_FILE={{ VCPKG }}/scripts/buildsystems/vcpkg.cmake \
              -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
              -DVCPKG_MANIFEST_MODE=ON \
              -DVCPKG_OVERLAY_TRIPLETS={{ WORKSPACE }}/custom-triplets \
              -DVCPKG_TARGET_TRIPLET=x64-linux-native \
              -DVCPKG_HOST_TRIPLET=x64-linux-native \
              -DVCPKG_APPLOCAL_DEPS=ON \
              -DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON \
              -DVCPKG_VERBOSE=ON

build:
    @if [ ! -f {{ BUILD }}/build.ninja ]; then \
        echo "Error: Build directory not configured. Run 'just config' first."; \
        exit 1; \
    fi
    @echo "=== CMake Building (parallel) ==="
    cmake --build {{ BUILD }} --parallel "$(nproc)"

# 一键配置并编译 Release
build-release COMPILER="clang":
    just config {{ COMPILER }} Release
    just build

# 一键配置并编译 Debug
build-debug COMPILER="clang":
    just config {{ COMPILER }} Debug
    just build

# ------------------------------------------------------------------------------
# 2. 运行
# ------------------------------------------------------------------------------

run:
    @if [ ! -f {{ BUILD }}/vulkan_practice/vulkan_practice ]; then \
        echo "Error: Binary not found. Run 'just build' first."; \
        exit 1; \
    fi
    @(cd {{ BUILD }}/vulkan_practice && RADV_EXPERIMENTAL=heap ./vulkan_practice)

# ------------------------------------------------------------------------------
# 3. 清理与辅助命令
# ------------------------------------------------------------------------------

# CMake 增量清理 (保留配置和 vcpkg 缓存)
clean:
    @if [ -d {{ BUILD }} ]; then \
        cmake --build {{ BUILD }} --target clean; \
    fi

# 彻底删除整个 build 目录 (切编译器时推荐使用)
clean-all:
    rm -rf {{ BUILD }}

all:
    just clean-all
    just build-release
