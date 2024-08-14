cmake_minimum_required (VERSION 3.28)
project("RGL" LANGUAGES CXX)





add_subdirectory(api_abstractor)

add_subdirectory(opengl_core)
#add_subdirectory(software_renderer)

add_subdirectory(front_end)
add_subdirectory(entry)

add_subdirectory(Test)
