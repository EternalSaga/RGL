file(GLOB_RECURSE app_src *.cpp *.hpp)
add_library(app ${app_src})
target_compile_features(app PRIVATE cxx_std_23)
execute_format(${app_src})
find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(app
    PUBLIC
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )

find_package(Boost REQUIRED)
target_link_libraries(app PRIVATE Boost::headers)

find_package(glm CONFIG REQUIRED)

target_link_libraries(app PRIVATE glm::glm RGLFramework practices)

target_include_directories(app PRIVATE ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/practices)

find_package(unofficial-duktape CONFIG REQUIRED)
target_link_libraries(app PUBLIC unofficial::duktape::duktape)

find_path(DUKGLUE_INCLUDE_DIRS "dukglue/detail_class_proto.h")
target_include_directories(app PUBLIC ${DUKGLUE_INCLUDE_DIRS})