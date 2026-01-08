file(GLOB_RECURSE loggerSrc *.cpp *.hpp)
add_library(rllogger ${loggerSrc})

# 设置 logger 目录本身为公开的 include 路径
target_include_directories(rllogger PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(rllogger PUBLIC cxx_std_20)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(rllogger PUBLIC spdlog::spdlog_header_only)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(rllogger PUBLIC nlohmann_json::nlohmann_json)
