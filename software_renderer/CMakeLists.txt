add_library(CPURender)

target_sources(CPURender PUBLIC FILE_SET CXX_MODULES FILES "../front_end/SRFramework.ixx")


target_compile_features(CPURender PUBLIC cxx_std_23)


target_link_libraries(CPURender PRIVATE Abstractor RLApp)
 find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(CPURender
        PRIVATE
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )
