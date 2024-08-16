add_executable(TestGIL "TestGIL.cpp" )




find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(TestGIL
    PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

target_compile_features(TestGIL PRIVATE cxx_std_23)

