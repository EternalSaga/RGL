file(GLOB_RECURSE loggerSrc *.cpp *.hpp)
add_library(rllogger ${loggerSrc})

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(rllogger PRIVATE spdlog::spdlog_header_only)

target_compile_features(rllogger PRIVATE cxx_std_23)

find_package(nlohmann_json CONFIG REQUIRED)
target_include_directories(rllogger PRIVATE nlohmann_json::nlohmann_json)
