
file(GLOB_RECURSE RGLFramework_src *.cpp *.hpp)

add_library(RGLFramework ${RGLFramework_src})

find_package(glad CONFIG REQUIRED)
target_link_libraries(RGLFramework PUBLIC glad::glad)

target_compile_features(RGLFramework PUBLIC cxx_std_23)


find_package(spdlog CONFIG REQUIRED)
target_link_libraries(RGLFramework PUBLIC spdlog::spdlog)
target_include_directories(RGLFramework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gl ${CMAKE_CURRENT_SOURCE_DIR}/logger
${CMAKE_CURRENT_SOURCE_DIR}/cpu
) 
target_include_directories(RGLFramework PUBLIC  ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/api_abstractor)

find_package(Boost REQUIRED)
target_link_libraries(RGLFramework PRIVATE Boost::headers)
find_package(Stb REQUIRED)
target_include_directories(RGLFramework PRIVATE ${Stb_INCLUDE_DIR})
target_compile_features(RGLFramework PRIVATE cxx_std_23)
set_property(TARGET RGLFramework PROPERTY CMAKE_COMPILE_WARNING_AS_ERROR ON)



find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(RGLFramework
    PUBLIC
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )