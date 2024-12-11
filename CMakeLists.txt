cmake_minimum_required (VERSION 3.29)
project("RGL" LANGUAGES CXX)
option(FMT_UNICODE "Enable Unicode support." ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)

add_subdirectory(api_abstractor)
add_subdirectory("./logger")
add_subdirectory(app)
add_subdirectory(render_core)
add_subdirectory(practices)


add_subdirectory(entry)

#add_subdirectory(FindJavaTest)
#add_subdirectory(ECSTest)

