
file(GLOB_RECURSE glframeworkSrc *.cpp *.hpp)

add_library(glFramework ${glframeworkSrc})

execute_format(${glframeworkSrc})

find_package(glad CONFIG REQUIRED)
target_link_libraries(glFramework PUBLIC glad::glad)

target_compile_features(glFramework PUBLIC cxx_std_20)

target_include_directories(glFramework PRIVATE ${PROJECT_SOURCE_DIR}/logger ${PROJECT_SOURCE_DIR}/app 
${PROJECT_SOURCE_DIR}/api_abstractor ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/light

)


target_link_libraries(glFramework PRIVATE rllogger)
find_package(Boost CONFIG REQUIRED COMPONENTS headers pfr variant2)
target_link_libraries(glFramework PRIVATE Boost::headers Boost::pfr Boost::variant2)


find_package(Stb REQUIRED)
target_include_directories(glFramework PRIVATE ${Stb_INCLUDE_DIR})

set_property(TARGET glFramework PROPERTY CMAKE_COMPILE_WARNING_AS_ERROR ON)

find_package(SDL3 CONFIG REQUIRED)
    target_link_libraries(glFramework
    PUBLIC
    SDL3::SDL3
    )
find_package(imgui CONFIG REQUIRED)
target_link_libraries(glFramework PUBLIC imgui::imgui)
find_package(EnTT CONFIG REQUIRED)
target_link_libraries(glFramework PUBLIC EnTT::EnTT)


find_package(assimp CONFIG REQUIRED)
target_link_libraries(glFramework PRIVATE assimp::assimp)