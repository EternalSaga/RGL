

add_library(RLApp)

target_sources(RLApp PUBLIC FILE_SET CXX_MODULES FILES "RLApp.ixx" "Camera.ixx")

 find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(RLApp
        PUBLIC
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )


target_include_directories(RLApp PUBLIC ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/opengl_core)

find_package(glad CONFIG REQUIRED)
target_link_libraries(RLApp PUBLIC glad::glad)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(RLApp PUBLIC spdlog::spdlog)


target_link_libraries(RLApp PRIVATE Abstractor GLFramework GLPractices)

target_compile_features(RLApp PRIVATE cxx_std_23)

