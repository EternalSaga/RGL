add_executable(vulkan_practice main.cpp)


target_sources(vulkan_practice PUBLIC 
            FILE_SET CXX_MODULES
            FILES utils.cppm
)

find_package(Vulkan REQUIRED)

target_link_libraries(vulkan_practice PRIVATE Vulkan::Vulkan rllogger)

target_compile_features(vulkan_practice PRIVATE cxx_std_23)


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    
    set(VCPKG_LAYER_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/vulkan/explicit_layer.d/")
    message(STATUS "VCPKG LAYER DIR: ${VCPKG_LAYER_DIR}  ")

    set(RUN_ENV "VK_ADD_LAYER_PATH=${VCPKG_LAYER_DIR}")
    
    set_property(TARGET vulkan_practice PROPERTY
        ENVIRONMENT "VK_ADD_LAYER_PATH=${VCPKG_LAYER_DIR}"
    )

endif()