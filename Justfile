#!/usr/bin/env just

# RGL Project — Cross-platform build recipes

# ─── 变量定义 ──────────────────────────────────────────────────────────────────
# just 启动时急切求值所有 backtick——必须用两个平台都能执行的命令
# 使用 cmd.exe /c 在 Windows 上执行，sh -c 在 Linux 上执行
# 机器相关参数走环境变量（债-29，三机轮换）：VCPKG_ROOT 未设时 Windows 落回
# E:/program/vcpkg（原硬编码值 = 默认，不设也不走错路）；Linux 落回 PATH 上的 vcpkg。
WORKSPACE := `python -c "import os; print(os.getcwd())"`
BUILD := WORKSPACE / "build"
VCPKG_DIR := `python -c "import os,shutil; w=shutil.which('vcpkg'); print(os.environ.get('VCPKG_ROOT') or ('E:/program/vcpkg' if os.name=='nt' else (os.path.dirname(w) if w else '')))"`
LLVM_DIR := `python -c "import os; print('C:/Program Files/LLVM' if os.name=='nt' else '')"`
NPROC := `python -c "import os; print(os.cpu_count() or 4)"`
# vcpkg 自带 ninja 的路径随 VCPKG_ROOT 走（同根派生，不另设第二个硬编码）；找不到落回 PATH 上的 ninja（原语义）
NINJA_EXE := `python -c "import os; root=os.environ.get('VCPKG_ROOT') or 'E:/program/vcpkg'; p=os.path.join(root,'downloads','tools','ninja','1.13.1-windows','ninja.exe'); print(p if os.path.isfile(p) else 'ninja')"`

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
    # 单一 port 的版本钉死走 overlay（不 pull 整个 vcpkg 仓库 = 不触发其它 port 升版与全量重编）：
    # third_party/vcpkg-ports/ 里只放我们需要超前 baseline 的那几个 port（当前 = meshoptimizer 1.3，
    # 簇化打包工具依赖；baseline 里只有 1.2，缺 clusterlod.h 需要的 simplify flag）。
    ports = fwd(os.path.join(workspace, "third_party", "vcpkg-ports"))

    cmake_cmd = [
        "cmake", "-S", workspace, "-B", build,
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DVCPKG_MANIFEST_MODE=ON",
        "-DVCPKG_OVERLAY_TRIPLETS=" + triplets,
        "-DVCPKG_OVERLAY_PORTS=" + ports,
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
    # ⚠ 退出码必须透传（`config` 一直是 `sys.exit(result.returncode)`，本配方此前漏了）：
    # 原先 `subprocess.call(...)` 的返回值没人接，**编译失败也返回 0**，于是任何
    # 「让构建失败」的护栏（如 vendor 的 -Werror=absolute-value，见 third_party/fsr/PATCHES.md P14）
    # 都退化成"打印一行没人必须读的输出"。判据不拦住 = 判据不存在。
    @python -c "import os,sys,subprocess; b='{{ BUILD }}'; \
        (os.path.exists(os.path.join(b,'build.ninja')) or (print('Error: Run just config first.'), sys.exit(1))); \
        sys.exit(subprocess.call(['cmake','--build',b,'--parallel','{{ NPROC }}']))"

# 单元测试（tests/）：**默认只跑无设备子集**（无 GPU、无窗口、毫秒级）。
#
# 批 12.2d 起链接面是全部四层（rlbase/rlscene/rlframe + rlgfx），因为
# **「import rlvk」是编译期依赖、「需要一个 VkDevice」才是运行期依赖**——
# render_graph / state_kit 都 import rlvk 却一个设备都不碰，没理由不让它们进
# 无 GPU 子集。分流靠 **doctest 测试套件**（不是链接面、也不是 tag：本项目
# doctest 2.5.3 的 TEST_CASE 宏只接受一个参数，tag 形态不存在）：
#   设备级用例写在 TEST_SUITE("gpu") { … } 里 → 本命令自动排除
# 代价（明确记录）：测试二进制现在**链接**了 Vulkan/SDL3/VMA，nm 不再为 0。
# tests 是 EXCLUDE_FROM_ALL 的子目录，故这里要显式构建其 target。
# 透传参数给 doctest：just test -- --test-case="*级联*" / -s（打印成功断言）
test *ARGS="":
    @cmake --build {{ BUILD }} --target rgl_tests
    {{ BUILD }}/tests/rgl_tests --test-suite-exclude=gpu {{ ARGS }}

# 全部单元测试（含 gpu 套件：真设备 + VK_LAYER_KHRONOS_validation）。
# **无设备时这些用例是 FAIL 不是 SKIP**——静默跳过 = 假绿，正是本项目反复吃亏的
# 「上次跑过」≠「这次验的是同一组输入」。要验的是回读链路本身，没设备就没有答案。
test-all *ARGS="":
    @cmake --build {{ BUILD }} --target rgl_tests
    {{ BUILD }}/tests/rgl_tests {{ ARGS }}

# 只跑设备级用例（回读原语层；改帧捕获 / 附件回读 / 格式编码后用）
test-gpu *ARGS="":
    @cmake --build {{ BUILD }} --target rgl_tests
    {{ BUILD }}/tests/rgl_tests --test-suite=gpu {{ ARGS }}

# 闭式解探针：算「某个像素**该**是多少」，与实机渲染逐字节对拍。不需要基线图像，
# 也不比 SSIM——它抓的是整条链的数值系统性偏移。
#
# 引擎侧**零改动**：链上每一项都已有配置门（env.intensity / skybox / light.ambient /
# ao.strength / exposure / shadow.lua 的 mode），全套见 qa/configs/analytic/scene.lua
# 的逐项注释。原计划加的 `analytic = true` 开关经实测判定为多余。
# 12.3b 起由 imgqa probe 接管（原 tools/analytic_ref.py 退役删除；参考实现移入 imgqa
# judge.cpp，闭式解与字节判据同一份代码、同一次编译）。
analytic:
    @cmake --build {{ BUILD }} --target imgqa rgl
    @python tools/qa_scenes.py --out {{ BUILD }}/rgl/models --only cornell_box
    {{ BUILD }}/tools/imgqa/imgqa probe

# ssr_rig 闭式反射靶（Phase 15 批 15.0）：生成靶 + 用 qa/configs/ssr_rig 出一张图。
# 判据本体 = 该场景 manifest 的 probe.reflectionHullPx / pillarHullPx（闭式倒影凸包，
# 推导见 tools/qa_scenes.py 的「场景 8：ssr_rig」段头）。SSR 关时凸包内（剥掉柱身）
# 必须全黑，SSR 开起来后同一批像素必须变亮 = 阳性锚点。
# 两个变体都要生成：ssr_rig_empty 是**负对照**（无柱 ⟹ 闭式凸包为空集）。
ssr-target:
    @python tools/qa_scenes.py --out {{ BUILD }}/rgl/models --only ssr_rig
    @python tools/qa_scenes.py --out {{ BUILD }}/rgl/models --only ssr_rig_empty
    @python tools/smoke.py shot --config-base qa/configs/ssr_rig --name ssr_rig

# SSR 闭式靶判据算子（批 15.1）——区域来自 manifest 几何、比较 SSR 开/关**差分**。
# A 阳性锚点 = 闭式倒影凸包内必须变亮（覆盖率下限抓"部分像素整条射线没命中"）；
# B 因果泄漏恒等式 = 凸包外逐位不变（±4px 边界豁免 = 15.3B 半分辨率 footprint + 模糊半径，理由见脚本 docstring）。
# 与 ssr-target 的分工：那个只生成场景 + 门关图，这个才是判据本体。
ssr-check:
    @python tools/ssr_hull_check.py

# imgqa —— 区域归因的图像判据工具箱（C++ + vcpkg；判据的区域来自几何：objectID 反解
# 可见 piece / 闭式解逐面预测 vs 观测亮度簇 / 开关影响域；每条判据的容差理由与 guard 出处见 check）。
# 例：just imgqa selftest | just imgqa attribution --camera near_anchor | just imgqa faces --piece anchor_cube_near
imgqa *ARGS="":
    @cmake --build {{ BUILD }} --target imgqa
    {{ BUILD }}/tools/imgqa/imgqa {{ ARGS }}


# 重跑 8 条反射管线（改动 shaders/*.slang / *_schema.lua / tools/templates /
# features_config.json / param_semantics.json 后必须执行；产物 rgl/generated/ 已入版本控制需随
# 改动一并提交，否则 reflection_check 守卫会拦构建）
reflect:
    @cmake --build {{ BUILD }} --target reflect

# 源码检查统一入口（样式 + Doxygen；每次 just build 已随 source_checks 目标跑、
# 只警告。单 parse 共享缓存，--only 只过滤规则求值不影响解析）。透传参数见
# tools/checks/main.py --help（--only style|doc / --strict / --max-gap N /
# --file-threshold N / --verbose 等）
checks *ARGS="":
    @python "{{ WORKSPACE }}/tools/checks/main.py" --build-dir "{{ BUILD }}" {{ ARGS }}

# Doxygen 注释检查（= checks --only doc，兼容旧调用位）
doc-check *ARGS="":
    @python "{{ WORKSPACE }}/tools/checks/main.py" --build-dir "{{ BUILD }}" --only doc {{ ARGS }}

# 文件/函数结构检查（= checks --only style，兼容旧调用位）
style-check *ARGS="":
    @python "{{ WORKSPACE }}/tools/checks/main.py" --build-dir "{{ BUILD }}" --only style {{ ARGS }}

# 构建 imgqa 再跑 checks。为什么需要这条：imgqa 是 EXCLUDE_FROM_ALL，`just build`
# 根本不编它，编译失败会一直躺着，直到某个人显式跑 `just imgqa` / `just analytic`
# 才发现（真实踩过：`clipArmName` 与 `case 11` 撞 duplicate case，潜伏到下次 analytic
# 才炸）。改完 tools/imgqa 就 `just build-tools`，把「编译不过」压回当轮。
#
# 注意方向：**checks 本身不去编译 imgqa**——source_checks 是 just build 的内建步骤，
# 检查步骤里再触发编译会形成递归并拖慢每次构建。checks 只要 compile_commands.json
# 里的编译命令（configure 期即导出），imgqa 未构建也能解析它。
#
# selftest 也在这里跑（不要设备、毫秒级）：**只在某个工具里自检的护栏，必须有人自动跑它**。
# 2026-10-09 实测踩过：给判据注册表加两条却没抬 selftest 里写死的总数 ⟹ 护栏本身是红的，
# 而本 recipe 原先不跑它 ⟹ 提交者听不见，红留给下一个人才被发现。
build-tools:
    @cmake --build {{ BUILD }} --target imgqa
    @{{ BUILD }}/tools/imgqa/imgqa selftest
    @cmake --build {{ BUILD }} --target meshlet_pack
    just checks

# 分层测试覆盖量具（债-15）：rlbase/rlgfx/rlscene/rlframe 哪些模块有 tests/ 消费者。
# **只出数不裁决**：没有阈值、永远不会"红"，补谁不补谁是人的裁决（口径 direct/offline 的
# 唯一真相源 = tools/checks/coverage_layers.py 的模块 docstring，本文不抄）。
#   just coverage           按层汇总表 + 逐模块清单
#   just coverage --json    机器可读形态（自己排序/切片）
# 为什么不并进 just checks：checks 每次 just build 都跑，一张没人裁决的表每次构建都印 =
# 训练人跳过它（噪声判据比没有判据更贵）。但"完全没人跑的工具会静默烂掉"是债-10 同一种病，
# 所以 checks 里留**一行读数** + 扫描面为空报 error，全表按需出。
coverage *ARGS="":
    @python "{{ WORKSPACE }}/tools/checks/coverage_layers.py" {{ ARGS }}

# ─── 护栏接执行者（债-10）───────────────────────────────────────────────────────
# pre-push 钩子 = push 前跑 `just test`（毫秒级无设备回归）。**钩子本体住仓里**
# （tools/hooks/pre-push，可评审、可 diff、跟着分支走），而 .git/hooks/ 不进版本库
# ⟹ 换机器 / 重新 clone / 新建 worktree 之后要再跑一次 install-hooks（这就是"依赖人记得"
#  的本体，能做的只是让"记得"变成一条命令）。
#   just install-hooks             装；已存在的钩子**绝不静默覆盖**（本仓那一位原本是
#                                  Git LFS 的，覆盖 = push 不再上传 LFS 对象）
#   just install-hooks FORCE=1     接管：原钩子另存成 pre-push.local，测试通过后原样接力
#   just pre-push                  当命令跑一遍（不 push）——验收与日常自检用
# 🔴 钩子里只有 just test（构建也只到 --target rgl_tests）：慢钩子 = 人人心跳
#    --no-verify = 护栏死亡。要加东西先进 just test，别直接塞进钩子。
# 与 Git LFS 的相处（实测）：git lfs install 撞上本钩子会**拒绝覆盖**并打印
#   "Hook already exists: pre-push"（它也不礼貌地抢位置）；反过来 git lfs update --force
#   会把本钩子换掉——届时重跑 just install-hooks 即可（幂等，且会重新接上 pre-push.local）。
install-hooks *ARGS="":
    @python "{{ WORKSPACE }}/tools/hooks/install_hooks.py" {{ ARGS }}

pre-push:
    @python "{{ WORKSPACE }}/tools/hooks/pre-push" --no-stdin origin local-dryrun

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
# Mesa ≥26.2.3 起 RADV 无条件 advertise VK_EXT_descriptor_heap，无需 RADV_EXPERIMENTAL。
run:
    just check-run-ready
    @python -c "import os,subprocess; \
        b='{{ BUILD }}/rgl'; \
        exe='rgl.exe' if os.name=='nt' else 'rgl'; \
        os.chdir(b); \
        subprocess.call([os.path.join('.', exe)])"  # './' 前缀：Python 3.12+ 起无路径分隔符的相对名改走 PATH 搜索（裸名 = FileNotFoundError）

# agent 冒烟 / 截图（一次性配置，**不碰 build/rgl/configs 副本**）：配置铺到
# build/rgl/.smoke/ 经 RGL_CONFIG_DIR 注入，跑完没有"需要复原"的状态；开头回显
# 主机 / GPU / DGC 选路 / 二进制与 SPV 新鲜度 / 副本是否脏（三机轮换先认设备）。
# 判据 = 真出一帧（llm_debug 截图），errors 抓 [error] 级日志行（含 VVL VUID）。
#   just smoke                                  冒烟（PASS/FAIL + errors/warnings 计数）
#   just smoke --variant deferred               换渲染路线冒烟
#   just shot --name foo --size 1600x900        截图 → build/rgl/screenshots/foo.png
#   just smoke --from-build                     以 build 副本现场为配置基准（默认基准 = 仓库 configs/）
#   just smoke --sync                           **VVL 同步验证档**（syncval，改 barrier 后跑）
#     开关名 = `VK_LAYER_VALIDATE_SYNC=1`（工具内置，`--syncval` 是同义写法）。hazard 以
#     [error] 落日志 ⟹ 直接计入 errors，判据仍是 errors=0。默认不开：syncval 有实打实 CPU
#     成本，且逐位红线档不需要它（它查"依赖漏没漏"，不查"画面对不对"）。
#     🔴 两个坑（照抄 SDK 文档会踩，2026-10-09 实测）：
#       · 本机 VVL 层清单自述 `validate_sync` **default = false**，而引擎只
#         setPEnabledLayerNames、项目无 vk_layer_settings ⟹ 不开这个 env，同步验证
#         **从来没开过**（"VVL 常开"里同步那一维一直是空的）。
#       · SDK 文档的 `VK_LAYER_ENABLES=VK_ENABLE_SYNCHRONIZATION_VALIDATION` 与
#         `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` 在本机层清单里
#         **不存在 = no-op**，照抄只会得到"开了也抓不到"的假结论。
#     闸门自身的阳性锚点（怀疑它静默失效时照此重验）→ `tools/smoke.py` docstring 第 3 条。
smoke *ARGS="":
    @python tools/smoke.py smoke {{ ARGS }}

shot *ARGS="":
    @python tools/smoke.py shot {{ ARGS }}

# BIT-IDENTICAL 逐位回归（债-09 + 债-46）。判据口径与归因三态 → `tools/regress.py` docstring：
#   just regress selftest                量具自检（任何判定前也会自动先跑）
#   just regress floor                   同一构建连跑 N 次 = 本机 run-to-run 噪声地板
#   just regress ab                      同会话双构建（工作树 vs --ref，默认 HEAD）+ 归因
#   just regress burst --lag 16          连拍量最小同相位周期（时域位级出口）
# 三条不可谈：零容差（差异大小只归因不放行）· 不存 golden · 不存基线哈希（--expect 只
# 接受你从文档抄来的前缀）。配置旗标与 just smoke 同源（--variant/--aa/--size/--clip/…）。
regress *ARGS="":
    @python tools/regress.py {{ ARGS }}

# CPU 分布账重测（债-02 / 债-20 / 债-30 的读数来源）。口径写死在 tools/cpu_baseline.py：
#   just cpu                                       全部臂（default/no-vvl/shadow-off/izb-off/scale-half）
#   just cpu --arms default,no-vvl                 只跑两臂（改渲染后的最小验收）
#   just cpu --json f.json                         落读数     --compare f.json  逐项对差
#   just cpu --list-arms                           臂表现读口（**别在文档里抄臂表**）
# 三条不可谈：单实例串行（争用 = 读数作废）· 退出走 RPC /quit 不走截图（截图帧的回读落在
# drawFrame 区间内）· 主口径 p50。读数必须绑机器 + 绑 VVL 档（no-vvl 臂就是为这条设的）。
cpu *ARGS="":
    @python tools/cpu_baseline.py {{ ARGS }}

# 核显运行（Linux AMD 集显，原生渲染，无 offload）
# 必须用 VK_DRIVER_FILES 隐藏 NVIDIA ICD —— 设备评分器默认选 dGPU（分数更高）
run-igpu:
    just check-run-ready
    @RADV_ICD=$(ls /usr/share/vulkan/icd.d/radeon_icd.*.json /etc/vulkan/icd.d/radeon_icd.*.json 2>/dev/null | head -1); \
    if [ -z "$RADV_ICD" ]; then \
        echo "Error: radeon ICD json not found (Linux-only recipe)"; \
        exit 1; \
    fi; \
    (cd {{ BUILD }}/rgl && VK_DRIVER_FILES="$RADV_ICD" ./rgl)

# 独显运行（Linux NVIDIA prime offload；provider 编号绑机器——默认 3 = 本机 dGPU 的
# offload provider 编号，换机用 RGL_DGPU_PROVIDER 覆盖，债-29 三机轮换）
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
        __NV_PRIME_RENDER_OFFLOAD_PROVIDER="${RGL_DGPU_PROVIDER:-3}" \
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

# ─── FSR 3.1.4 vendor（third_party/fsr，见 docs/phase14_plan.md）─────────────────
# 排列产物 = 生成一次、签进仓库、构建开头比对（与 just reflect + reflection_check 同构）。
# 判据分工：fsr-check 只比哈希（秒级）；真编译 2560 次 glslang 要 ~27 s，人触发。
#   just fsr-blobs              重生成 blobs/*.h（改 vendor 的 shader 契约后）
#   just fsr-check              SHA1 护栏 + 反射名对账（CI/提交前）
#   just fsr-smoke              构建并执行「最小设备 + 单 pass」冒烟（要真设备 + VVL）
fsr-blobs *ARGS="":
    @python "{{ WORKSPACE }}/third_party/fsr/tools/gen_ffx_shader_blobs.py" {{ ARGS }}

fsr-check:
    @python "{{ WORKSPACE }}/third_party/fsr/tools/gen_ffx_shader_blobs.py" --check

# EXCLUDE_FROM_ALL 的 target，故不进 just build 的运行路径；复用 build/ 的现成配置
# （没配置过就先 just config）。判据 = 两帧 ContextDispatch 全部 dispatched + executed
# 且 VVL 无白名单外报错（白名单逐条根因 → third_party/fsr/PATCHES.md）。
fsr-smoke:
    @cmake --build {{ BUILD }} --target fsr_smoke -j {{ NPROC }}
    {{ BUILD }}/third_party/fsr/fsr_smoke

# 合成靶实验台（批 14.5 时域档发糊根因量化）。判据与诊断补丁说明 → smoke/fsr_lab.cpp 头注释。
# 例：just fsr-lab "--size" "1280x720" "--target" "delta"   ← **参数要各自加引号**；
#     `just fsr-lab -- --size ...` 的常见写法在这里**不生效**（`--` 会当成参数传进去）。
fsr-lab *ARGS="":
    @cmake --build {{ BUILD }} --target fsr_lab -j {{ NPROC }}
    {{ BUILD }}/third_party/fsr/fsr_lab {{ ARGS }}

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
