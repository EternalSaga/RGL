#!/usr/bin/env just

# RGL Project — Cross-platform build recipes

# ─── 变量定义 ──────────────────────────────────────────────────────────────────
# just 启动时急切求值所有 backtick——必须用两个平台都能执行的命令
# 使用 cmd.exe /c 在 Windows 上执行，sh -c 在 Linux 上执行
WORKSPACE := `python -c "import os; print(os.getcwd())"`
BUILD := WORKSPACE / "build"
VCPKG_DIR := `python -c "import os,shutil; print('E:/program/vcpkg' if os.name=='nt' else (os.environ.get('VCPKG_ROOT') or (os.path.dirname(shutil.which('vcpkg')) if shutil.which('vcpkg') else '')))"`
LLVM_DIR := `python -c "import os; print('C:/Program Files/LLVM' if os.name=='nt' else '')"`
NPROC := `python -c "import os; print(os.cpu_count() or 4)"`
NINJA_EXE := `python -c "import os; p='E:/program/vcpkg/downloads/tools/ninja/1.13.1-windows/ninja.exe'; print(p if os.path.isfile(p) else 'ninja')"`

# ─── CMake 配置 ───────────────────────────────────────────────────────────────
# 用法:
#   just config              → Release (Linux: clang, Windows: clang++)
#   just config Debug        → Debug
#   just config gcc          → gcc (Linux only)
#   just config msvc         → MSVC / Visual Studio generator (Windows only)
config COMPILER="default" BUILD_TYPE="Release":
    #!/usr/bin/env python
    import os, sys, subprocess
    compiler = "{{ COMPILER }}"
    build_type = "{{ BUILD_TYPE }}"
    workspace = "{{ WORKSPACE }}"
    build = "{{ BUILD }}"
    vcpkg = "{{ VCPKG_DIR }}"
    llvm = "{{ LLVM_DIR }}"
    is_win = os.name == 'nt'

    # 参数位置互换检测
    if build_type in ("clang", "gcc", "msvc"):
        compiler, build_type = build_type, compiler
    if compiler in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        build_type, compiler = compiler, "default"

    print("=== CMake Configure (%s, %s) ===" % (build_type, compiler))

    def fwd(path):
        """统一正斜杠，避免反斜杠被 shell 转义"""
        return path.replace("\\", "/")

    toolchain = fwd(os.path.join(vcpkg, "scripts", "buildsystems", "vcpkg.cmake"))
    if not os.path.isfile(toolchain):
        print("Error: vcpkg toolchain not found: %s (set VCPKG_ROOT or put vcpkg on PATH)" % toolchain)
        sys.exit(1)
    triplets = fwd(os.path.join(workspace, "custom-triplets"))

    cmake_cmd = [
        "cmake", "-S", workspace, "-B", build,
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DVCPKG_MANIFEST_MODE=ON",
        "-DVCPKG_OVERLAY_TRIPLETS=" + triplets,
        "-DVCPKG_APPLOCAL_DEPS=ON",
        "-DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON",
        "-DVCPKG_VERBOSE=ON",
    ]

    if is_win:
        if compiler == "msvc":
            cmake_cmd += ["-G", "Visual Studio 18 2026", "-A", "x64"]
        else:
            cc = fwd(os.path.join(llvm, "bin", "clang.exe"))
            cxx = fwd(os.path.join(llvm, "bin", "clang++.exe"))
            rc = fwd(os.path.join(llvm, "bin", "llvm-rc.exe"))
            cmake_cmd += [
                "-G", "Ninja",
                "-DCMAKE_MAKE_PROGRAM=" + fwd("{{ NINJA_EXE }}"),
                "-DCMAKE_C_COMPILER=" + cc,
                "-DCMAKE_CXX_COMPILER=" + cxx,
                "-DCMAKE_RC_COMPILER=" + rc,
                "-DCMAKE_BUILD_TYPE=" + build_type,
                "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
                "-DVCPKG_TARGET_TRIPLET=x64-windows-static",
                "-DVCPKG_HOST_TRIPLET=x64-windows-static",
            ]
    else:
        compiler_map = {
            "default": ("clang", "clang++"),
            "gcc": ("gcc", "g++"),
            "clang": ("clang", "clang++"),
        }
        cc, cxx = compiler_map.get(compiler, (compiler, compiler + "++"))
        cmake_cmd += [
            "-G", "Ninja",
            "-DCMAKE_C_COMPILER=" + cc,
            "-DCMAKE_CXX_COMPILER=" + cxx,
            "-DCMAKE_BUILD_TYPE=" + build_type,
            "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
            "-DVCPKG_TARGET_TRIPLET=x64-linux-native",
            "-DVCPKG_HOST_TRIPLET=x64-linux-native",
        ]

    print("  cmd: %s" % " ".join(cmake_cmd))
    result = subprocess.run(cmake_cmd)
    sys.exit(result.returncode)

build:
    @python -c "import os,sys,subprocess; b='{{ BUILD }}'; \
        (subprocess.call(['cmake','--build',b,'--parallel','{{ NPROC }}']) if os.path.exists(os.path.join(b,'build.ninja')) else (print('Error: Run just config first.'), sys.exit(1)))"

# 重跑 4 条反射管线（改动 shaders/*.slang / *_schema.lua / tools/templates /
# features_config.json 后必须执行；产物 rgl/generated/ 已入版本控制需随
# 改动一并提交，否则 reflection_check 守卫会拦构建）
reflect:
    @cmake --build {{ BUILD }} --target reflect

# 全量编译基准: clean-all + config + 计时编译
profile:
    just clean-all
    just config
    time just build

# 纯编译全量基准
profile-fast:
    just clean
    time just build

# 一键配置并编译 Release
build-release COMPILER="default":
    just config {{ COMPILER }} Release
    just build

# 一键配置并编译 Debug
build-debug COMPILER="default":
    just config {{ COMPILER }} Debug
    just build

# ------------------------------------------------------------------------------
# 2. 运行
# ------------------------------------------------------------------------------

check-run-ready:
    @python -c "import os,sys; \
        b='{{ BUILD }}/rgl'; \
        exe='rgl.exe' if os.name=='nt' else 'rgl'; \
        path=os.path.join(b, exe); \
        cfg=os.path.join(b,'configs','log_level.json'); \
        shaders=os.path.join(b,'generated_shaders'); \
        sys.exit(0) if os.path.isfile(path) else (print('Error: %s not found. Run just build first.' % exe), sys.exit(1)); \
        sys.exit(0) if os.path.isfile(cfg) and os.path.isdir(shaders) else (print('Error: Runtime resources missing. Run just build first.'), sys.exit(1))"

# 运行入口统一 cd 到 build/rgl：程序按相对路径读 configs/*.lua 和
# generated_shaders/*.spv，CWD 必须是该目录。
# Linux 自动检测：lspci 见 AMD VGA/display → 设 RADV_EXPERIMENTAL=heap（RADV 启用 descriptor heap）
run:
    just check-run-ready
    @python -c "import os,subprocess,re,shutil; \
        b='{{ BUILD }}/rgl'; \
        exe='rgl.exe' if os.name=='nt' else 'rgl'; \
        os.chdir(b); \
        env=dict(os.environ); \
        out=subprocess.run(['lspci'],capture_output=True,text=True).stdout if (os.name!='nt' and shutil.which('lspci')) else ''; \
        (env.update(RADV_EXPERIMENTAL='heap') if re.search(r'(?i)(vga|display).*amd',out) else None); \
        subprocess.call([os.path.join('.', exe)],env=env)"  # './' 前缀：Python 3.12+ 起无路径分隔符的相对名改走 PATH 搜索（裸名 = FileNotFoundError）

# 核显运行（Linux AMD 集显，原生渲染，无 offload）
# 必须用 VK_DRIVER_FILES 隐藏 NVIDIA ICD —— 设备评分器默认选 dGPU（分数更高）
# RADV_EXPERIMENTAL=heap 用于 RADV 启用 descriptor heap
run-igpu:
    just check-run-ready
    @RADV_ICD=$(ls /usr/share/vulkan/icd.d/radeon_icd.*.json /etc/vulkan/icd.d/radeon_icd.*.json 2>/dev/null | head -1); \
    if [ -z "$RADV_ICD" ]; then \
        echo "Error: radeon ICD json not found (Linux-only recipe)"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/rgl && VK_DRIVER_FILES="$RADV_ICD" RADV_EXPERIMENTAL=heap ./rgl)

# 独显运行（Linux NVIDIA prime offload；PROVIDER=3 为本机 dGPU 的 offload provider 编号）
# 必须用 VK_DRIVER_FILES 隐藏 radeon ICD —— prime offload 环境变量对 Vulkan 设备枚举无过滤作用
# （实测：NV_optimus 层激活后设备列表仍是 iGPU+dGPU 两个，选择完全取决于应用内打分器）
run-dgpu:
    just check-run-ready
    @NV_ICD=$(ls /usr/share/vulkan/icd.d/nvidia_icd*.json /etc/vulkan/icd.d/nvidia_icd*.json 2>/dev/null | head -1); \
    if [ -z "$NV_ICD" ]; then \
        echo "Error: nvidia ICD json not found"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/rgl && VK_DRIVER_FILES="$NV_ICD" \
        __NV_PRIME_RENDER_OFFLOAD=1 \
        __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 \
        __GLX_VENDOR_LIBRARY_NAME=zink \
        __VK_LAYER_NV_optimus=NVIDIA_only \
        ./rgl)

# 独显运行（Linux Intel dGPU，anv 驱动）
# 必须用 VK_DRIVER_FILES 只留 intel ICD —— 设备评分器默认选分数更高的 dGPU（RTX 5090）
# 要求 mesa >= 26.2：anv 自 26.2 起默认启用 VK_EXT_descriptor_heap（本项目核心扩展）；
# 26.1 及以下会在设备创建期抛 VK_ERROR_EXTENSION_NOT_PRESENT 后报错退出
run-intel:
    just check-run-ready
    @INTEL_ICD=$(ls /usr/share/vulkan/icd.d/intel_icd.*.json /etc/vulkan/icd.d/intel_icd.*.json 2>/dev/null | head -1); \
    if [ -z "$INTEL_ICD" ]; then \
        echo "Error: intel ICD json not found (Linux-only recipe)"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/rgl && VK_DRIVER_FILES="$INTEL_ICD" ./rgl)

# ------------------------------------------------------------------------------
# 3. 清理
# ------------------------------------------------------------------------------

clean:
    @python -c "import os,subprocess; \
        b='{{ BUILD }}'; \
        subprocess.call(['cmake','--build',b,'--target','clean']) if os.path.isdir(b) else None"

clean-all:
    @python -c "import os,shutil; shutil.rmtree('{{ BUILD }}', ignore_errors=True)"

all:
    just clean-all
    just build-release
