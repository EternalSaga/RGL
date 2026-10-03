# imgqa —— 区域归因的图像判据工具箱（C++ + vcpkg，用户拍板：依赖全走 vcpkg，
# 三台机器一条命令对齐，不维护任何 Python 侧环境。本 target 随 tools/ 继承
# EXCLUDE_FROM_ALL：不进 just build 主构建，just imgqa 显式构建。）
# OpenCV 的 CMake 包名 = `OpenCV`（官方 OpenCVConfig.cmake），**不是 vcpkg port 名
# `opencv4`**——find_package(opencv4) 会报"没有 opencv4Config.cmake"（实测踩过）。
find_package(OpenCV CONFIG REQUIRED core imgproc imgcodecs)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(imgqa
    main.cpp
    analytic.cpp
    engine.cpp
    image.cpp
    judge.cpp
    selftest.cpp
    temporal.cpp)

target_compile_features(imgqa PRIVATE cxx_std_23)
target_include_directories(imgqa PRIVATE ${OpenCV_INCLUDE_DIRS})
# 工程根 = 找 build/rgl / qa/configs / rgl 源码（键名校验扫解析器源码）的位置；
# 运行期可用 RGL_ROOT 环境变量覆盖（产物可搬运）。
target_compile_definitions(imgqa PRIVATE RGL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(imgqa PRIVATE
    ${OpenCV_LIBS}
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads)
