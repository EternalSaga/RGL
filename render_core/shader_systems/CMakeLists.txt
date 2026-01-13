file(GLOB shader_systems_sources "*.cpp" "*.hpp")
add_library(shader_systems ${shader_systems_sources})

target_include_directories(shader_systems PRIVATE ${PROJECT_SOURCE_DIR}/render_core/gl
${PROJECT_SOURCE_DIR}/app
${PROJECT_SOURCE_DIR}/logger
)

find_package(glm CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

target_link_libraries(shader_systems PRIVATE glad::glad glm::glm)

target_compile_features(shader_systems PRIVATE cxx_std_20)

target_add_shader(shader_systems ${CMAKE_SOURCE_DIR}/shaders/Light/phong_ubo_instanced.vert)
target_add_shader(shader_systems ${CMAKE_SOURCE_DIR}/shaders/Light/grass_fragment.frag)