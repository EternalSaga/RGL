#!/usr/bin/env just

# RGL Project — Gentoo build recipes

WORKSPACE := `echo ${ZED_WORKTREE_ROOT:-$PWD}`
VCPKG := "/home/robin/program/vcpkg"
BUILD := WORKSPACE / "build"

# ------------------------------------------------------------------------------
# 1. 配置阶段 (Configure)
# ------------------------------------------------------------------------------

# 基础 CMake 配置命令
# 用法:
#   just config             -> clang, Release
#   just config gcc         -> gcc, Release
#   just config clang Debug -> clang, Debug
#   just config gcc Debug   -> gcc, Debug
config COMPILER="clang" BUILD_TYPE="Release":
    @echo "=== CMake Configure ({{ BUILD_TYPE }}, {{ COMPILER }}) ==="
    env "CC={{ if COMPILER == "gcc" { "gcc" } else { "clang" } }}" \
        "CXX={{ if COMPILER == "gcc" { "g++" } else { "clang++" } }}" \
        cmake -S {{ WORKSPACE }} \
              -B {{ BUILD }} \
              -G Ninja \
              -DCMAKE_BUILD_TYPE={{ BUILD_TYPE }} \
              -DCMAKE_TOOLCHAIN_FILE={{ VCPKG }}/scripts/buildsystems/vcpkg.cmake \
              -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
              -DVCPKG_MANIFEST_MODE=ON \
              -DVCPKG_OVERLAY_TRIPLETS={{ WORKSPACE }}/custom-triplets \
              -DVCPKG_TARGET_TRIPLET=x64-linux-native \
              -DVCPKG_HOST_TRIPLET=x64-linux-native \
              -DVCPKG_APPLOCAL_DEPS=ON \
              -DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON \
              -DVCPKG_VERBOSE=ON

# ------------------------------------------------------------------------------
# 2. 构建阶段 (Build)
# ------------------------------------------------------------------------------

# 仅执行编译 (依赖已有的 build 目录配置)
build:
    @if [ ! -f {{ BUILD }}/build.ninja ]; then \
        echo "Error: Build directory not configured. Run 'just config' first."; \
        exit 1; \
    fi
    @echo "=== Ninja Building ==="
    ninja -C {{ BUILD }}

# 一键配置并编译 Release
build-release COMPILER="clang":
    just config {{ COMPILER }} Release
    just build

# 一键配置并编译 Debug
build-debug COMPILER="clang":
    just config {{ COMPILER }} Debug
    just build

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
