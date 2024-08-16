add_library(Abstractor)

target_sources(Abstractor PUBLIC FILE_SET CXX_MODULES FILES "api_types.ixx")

target_compile_features(Abstractor PRIVATE cxx_std_23)


find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(Abstractor
    PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)
