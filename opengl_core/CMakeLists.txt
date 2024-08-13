add_library(GLFramework)

target_sources(GLFramework PUBLIC FILE_SET CXX_MODULES FILES "GLFramework.ixx" "GLCheckError.ixx" "GLObj.ixx" "Shader.ixx" "GLTextures.ixx")





find_package(glad CONFIG REQUIRED)
target_link_libraries(GLFramework PUBLIC glad::glad)

target_compile_features(GLFramework PUBLIC cxx_std_23)




target_link_libraries(GLFramework PRIVATE Abstractor)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(GLFramework PUBLIC spdlog::spdlog)

target_include_directories(GLFramework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost REQUIRED)

target_link_libraries(GLFramework PUBLIC Boost::headers)

find_package(Stb REQUIRED)
target_include_directories(GLFramework PRIVATE ${Stb_INCLUDE_DIR})
find_package(JPEG REQUIRED)
target_link_libraries(GLFramework PUBLIC JPEG::JPEG)
find_package(PNG REQUIRED)
target_link_libraries(GLFramework PUBLIC PNG::PNG)

target_compile_features(GLFramework PRIVATE cxx_std_23)




set_property(TARGET GLFramework PROPERTY CMAKE_COMPILE_WARNING_AS_ERROR ON)
