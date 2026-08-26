#!/usr/bin/env just

# RGL Project — Gentoo build recipes

WORKSPACE := `echo ${ZED_WORKTREE_ROOT:-$PWD}`
VCPKG := `(which vcpkg | xargs dirname)`
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
              -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
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
    @echo "=== CMake Building (parallel $(nproc) ) ==="
    cmake --build {{ BUILD }} --parallel "$(nproc)"

# 全量编译基准: clean-all + config + 计时编译（输出 wall 时间）
# 注意: clean-all 会触发 vcpkg manifest 重装（本机约 8 分钟），大头在依赖安装而非编译
profile:
    just clean-all
    just config
    time just build

# 纯编译全量基准: 只清理编译产物（保留 config + vcpkg 缓存），计时重编全部目标
profile-fast:
    just clean
    time just build

# 编译耗时报告: 解析 .ninja_log，输出最近一次 build 的 wall / CPU 利用率 / top15 目标耗时
timings:
    @python3 {{ WORKSPACE }}/tools/build_timings.py {{ BUILD }}

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

# 检查二进制 + 运行时资源（configs/、generated_shaders/）在 build 目录完整
# 注意：不能设为 private — run/run-igpu/run-dgpu 通过 `just check-run-ready` 子进程调用，
# private recipe 拒绝主调用（just 的已知限制）
check-run-ready:
    @if [ ! -f {{ BUILD }}/vulkan_practice/vulkan_practice ]; then \
        echo "Error: Binary not found. Run 'just build' first."; \
        exit 1; \
    fi
    @if [ ! -f {{ BUILD }}/vulkan_practice/configs/log_level.json ] \
        || [ ! -d {{ BUILD }}/vulkan_practice/generated_shaders ]; then \
        echo "Error: Runtime resources missing (configs/ or generated_shaders/) in {{ BUILD }}/vulkan_practice. Run 'just build' first."; \
        exit 1; \
    fi

# 运行入口统一 cd 到 build/vulkan_practice：程序按相对路径读取
# configs/*.json 和 generated_shaders/*.spv，CWD 必须是该目录
run:
    just check-run-ready
    @(cd {{ BUILD }}/vulkan_practice && if lspci 2>/dev/null | grep -qiE 'vga.*amd|display.*amd'; then RADV_EXPERIMENTAL=heap ./vulkan_practice; else ./vulkan_practice; fi)

# 核显运行（AMD 集显，display GPU 原生渲染，无 offload）
# 必须用 VK_DRIVER_FILES 隐藏 NVIDIA ICD — 设备评分器默认选 dGPU（分数更高）
# RADV_EXPERIMENTAL=heap 用于 RADV 启用 descriptor heap
run-igpu:
    just check-run-ready
    @RADV_ICD=$(ls /usr/share/vulkan/icd.d/radeon_icd.*.json /etc/vulkan/icd.d/radeon_icd.*.json 2>/dev/null | head -1); \
    if [ -z "$RADV_ICD" ]; then \
        echo "Error: radeon ICD json not found"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/vulkan_practice && VK_DRIVER_FILES="$RADV_ICD" RADV_EXPERIMENTAL=heap ./vulkan_practice)

# 独显运行（NVIDIA dGPU，prime offload；PROVIDER=3 为本机 dGPU 的 offload provider 编号）
# 必须用 VK_DRIVER_FILES 隐藏 radeon ICD — prime offload 环境变量对 Vulkan 设备枚举无过滤作用
# （实测：NV_optimus 层激活后设备列表仍是 iGPU+dGPU 两个，选择完全取决于应用内打分器）
run-dgpu:
    just check-run-ready
    @NV_ICD=$(ls /usr/share/vulkan/icd.d/nvidia_icd*.json /etc/vulkan/icd.d/nvidia_icd*.json 2>/dev/null | head -1); \
    if [ -z "$NV_ICD" ]; then \
        echo "Error: nvidia ICD json not found"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/vulkan_practice && VK_DRIVER_FILES="$NV_ICD" \
        __NV_PRIME_RENDER_OFFLOAD=1 \
        __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 \
        __GLX_VENDOR_LIBRARY_NAME=zink \
        __VK_LAYER_NV_optimus=NVIDIA_only \
        ./vulkan_practice)

# 独显运行（Intel Arc A310 dGPU，anv 驱动）
# 必须用 VK_DRIVER_FILES 只留 intel ICD — 设备评分器默认选分数更高的 dGPU（RTX 5090）
# 要求 mesa >= 26.2：anv 自 26.2 起默认启用 VK_EXT_descriptor_heap（本项目核心扩展）；
# 26.1 及以下会在设备创建期抛 VK_ERROR_EXTENSION_NOT_PRESENT 后报错退出
run-intel:
    just check-run-ready
    @INTEL_ICD=$(ls /usr/share/vulkan/icd.d/intel_icd.*.json /etc/vulkan/icd.d/intel_icd.*.json 2>/dev/null | head -1); \
    if [ -z "$INTEL_ICD" ]; then \
        echo "Error: intel ICD json not found"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/vulkan_practice && VK_DRIVER_FILES="$INTEL_ICD" ./vulkan_practice)

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
