

add_library(RLApp)

file(GLOB_RECURSE frontend_src *.ixx)

target_sources(RLApp PUBLIC FILE_SET CXX_MODULES FILES ${frontend_src})

 find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(RLApp
        PRIVATE
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )


target_include_directories(RLApp PUBLIC  ${PROJECT_SOURCE_DIR}/opengl_core)

find_package(glad CONFIG REQUIRED)
target_link_libraries(RLApp PUBLIC glad::glad)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(RLApp PUBLIC spdlog::spdlog)


target_link_libraries(RLApp PRIVATE Abstractor GLFramework )

target_compile_features(RLApp PRIVATE cxx_std_23)

