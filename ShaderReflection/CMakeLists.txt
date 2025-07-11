file(GLOB_RECURSE SHADERREFLECTION_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")


add_executable(ShaderReflection ${SHADERREFLECTION_FILES})

target_compile_features(ShaderReflection PRIVATE cxx_std_23)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

find_package(Boost REQUIRED COMPONENTS program_options)

target_link_libraries(ShaderReflection PRIVATE spirv-cross-core spirv-cross-glsl spirv-cross-cpp nlohmann_json::nlohmann_json Boost::program_options)

