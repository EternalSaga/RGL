
file(GLOB_RECURSE cpuframeworkSrc *.cpp *.hpp)

add_library(cpuframework ${cpuframeworkSrc})

target_compile_features(cpuframework PRIVATE cxx_std_23)

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(cpuframework
    PUBLIC
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

target_link_libraries(cpuframework PRIVATE rllogger)

target_include_directories(cpuframework PRIVATE ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/logger)