# meshlet_pack —— 离线簇化打包器（簇化轴 ★0-a，docs/mdi_plan.md §9）。
# 随 tools/ 继承 EXCLUDE_FROM_ALL：不进 just build 主构建，just build-tools 显式构建
# （imgqa 同条教训：不进 build-tools 的 C++ 工具 = 编译红没人发现）。
#
# 依赖红线（§7.8 拍板 3）：只链 meshoptimizer（vcpkg overlay port 钉 1.3，
# third_party/vcpkg-ports/）+ tinygltf（vcpkg，header-only 无导出 target ⟹ find_path，
# rgl/CMakeLists.txt 同款）；**不链任何 rgl 引擎库**——引擎只读产物、不链
# meshoptimizer（判据：nm -C build/rgl/rgl | grep -c meshopt == 0）。
# clusterlod.h = third_party/meshoptimizer/ vendored（v1.3 逐字节未改，PATCHES.md）。
find_package(meshoptimizer CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)

add_executable(meshlet_pack
    # TU 切缝 = 输入 / 转换 / 自检 / 输出 / 读数 / 缓存键 / CLI（理由全文见 main.cpp 头注释）
    main.cpp
    glb_source.cpp
    tinygltf_impl.cpp      # tinygltf 实现 TU（port 只装头文件，rgl/tinygltf_impl.cpp 同法）
    packer.cpp             # 唯一的 CLUSTERLOD_IMPLEMENTATION 单元
    components.cpp         # must_L0 判据（焊接/并查集/逐级覆盖）+ L0 段 group 重排
    verify.cpp
    serialize.cpp
    stats.cpp
    sha1.cpp)

target_compile_features(meshlet_pack PRIVATE cxx_std_23)
target_include_directories(meshlet_pack SYSTEM PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_include_directories(meshlet_pack PRIVATE ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer)
target_link_libraries(meshlet_pack PRIVATE meshoptimizer::meshoptimizer)
