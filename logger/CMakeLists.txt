file(GLOB_RECURSE loggerSrc *.cpp *.hpp)
add_library(rllogger ${loggerSrc})

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(rllogger PRIVATE spdlog::spdlog)

target_compile_features(rllogger PRIVATE cxx_std_23)
