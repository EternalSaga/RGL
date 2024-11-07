cmake_minimum_required (VERSION 3.29)
project("RGL" LANGUAGES CXX)

include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)

add_subdirectory(api_abstractor)
add_subdirectory(app)
add_subdirectory(render_core)
add_subdirectory(practices)


add_subdirectory(entry)

#add_subdirectory(FindJavaTest)
add_subdirectory(ECSTest)
