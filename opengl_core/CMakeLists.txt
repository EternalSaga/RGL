add_library(GLFramework)

target_sources(GLFramework PUBLIC FILE_SET CXX_MODULES FILES "GLFramework.ixx" "GLCheckError.ixx" "GLObj.ixx" "ExecsiceCodes.ixx"  "LoadShader.ixx")

find_package(glad CONFIG REQUIRED)
target_link_libraries(GLFramework PUBLIC glad::glad)

target_compile_features(GLFramework PUBLIC cxx_std_23)

find_package(glfw3 CONFIG REQUIRED)

target_link_libraries(GLFramework PRIVATE glfw)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(GLFramework PUBLIC spdlog::spdlog)