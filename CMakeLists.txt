cmake_minimum_required (VERSION 3.28)
project("RGL" LANGUAGES CXX)



find_package(GLM REQUIRED)


add_subdirectory(io)
add_subdirectory(front_end)
add_subdirectory(opengl_core)
add_subdirectory(entry)