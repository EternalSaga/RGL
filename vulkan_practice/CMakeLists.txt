add_executable(vulkan_practice main.cpp)


target_sources(vulkan_practice PUBLIC 
            FILE_SET CXX_MODULES
            FILES utils.cppm sdl3window.cppm QueueFamilyManager.cppm
)

find_package(Vulkan REQUIRED)

target_link_libraries(vulkan_practice PRIVATE Vulkan::Vulkan rllogger)

find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(vulkan_practice
    PRIVATE
    SDL3::SDL3
    )

target_compile_features(vulkan_practice PRIVATE cxx_std_23)
