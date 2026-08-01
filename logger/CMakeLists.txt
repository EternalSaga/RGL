add_library(rllogger)

target_sources(rllogger PUBLIC
    FILE_SET CXX_MODULES FILES rllogger.cppm
)

target_compile_features(rllogger PUBLIC cxx_std_23)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(rllogger PUBLIC spdlog::spdlog)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(rllogger PUBLIC nlohmann_json::nlohmann_json)
